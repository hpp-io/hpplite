#!/usr/bin/env python3
"""The store's side of hpplite-fleet/1 (#297, doc/fleet-protocol.md "The store") against the v11 contracts on anvil:
a fleet created by the v11 factory, a database joined in one call (Admit + Claim), its batches written with their
signed heads (the chain hash checked against the bytes), the fence (after the owner's forceExpire the old epoch is
409; the new holder's first write is taken although the store had not polled yet), the heads an anchor builds from,
a manifest and its root posted, the root indexed from RootPosted, and the heads listed again only when they move."""
import hashlib
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
sys.path.insert(0, os.path.join(here, ".."))
sys.path.insert(0, os.path.join(here, "..", "fleet"))
from x402seller import eth  # noqa: E402
import fleet as F  # noqa: E402
import fleet_store  # noqa: E402

RPC = os.environ["HPPLITE_ANVIL_RPC"]
USDC = os.environ.get("HPPLITE_ANVIL_USDC")
FACILITATOR = os.environ.get("HPPLITE_ANVIL_FACILITATOR")
KEY0, KEY1, KEY2, KEY3 = (os.environ["HPPLITE_ANVIL_KEY%d" % i] for i in range(4))
CONTRACTS = os.path.join(here, "..", "..", "contracts")
fails = 0


def check(cond, what):
    global fails
    print(("  ok      " if cond else "  FAIL    ") + what)
    if not cond:
        fails += 1


def cast(*args):
    return subprocess.run(["cast"] + list(args) + ["--rpc-url", RPC], capture_output=True, text=True, check=True).stdout.strip()


def kb(k):
    return bytes.fromhex(k[2:])


def hx(b):
    return "0x" + b.hex()


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def http(method, url, body=None, headers=None):
    req = urllib.request.Request(url, data=body, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            data = r.read()
            return r.status, (json.loads(data) if r.headers.get("Content-Type", "").startswith("application/json") else data)
    except urllib.error.HTTPError as e:
        data = e.read()
        try:
            return e.code, json.loads(data)
        except ValueError:
            return e.code, data


def main():
    chain_id = int(cast("chain-id"))
    owner, anchor, dbowner, seq = (eth.address(kb(k)).lower() for k in (KEY0, KEY2, KEY1, KEY3))
    out = subprocess.run(["forge", "create", "--root", CONTRACTS, "src/HPPLiteFleetFactory.sol:HPPLiteFleetFactory", "--rpc-url", RPC,
                          "--private-key", KEY0, "--broadcast"], capture_output=True, text=True)
    factory = next((l.split()[-1] for l in out.stdout.splitlines() if l.startswith("Deployed to:")), None)
    check(factory is not None, "the v11 factory deployed (%s)" % (factory or out.stderr[-200:]))
    if not factory:
        return 1
    cast("send", "--private-key", KEY0, factory, "createFleet(address,uint64)", anchor, "0")
    fl = cast("call", factory, "fleets(uint256)(address)", "0").lower()
    check(cast("call", fl, "owner()(address)").lower() == owner, "a fleet created, its owner the creator (%s)" % fl)

    work = tempfile.mkdtemp(prefix="hpplite-fleet-store.")
    keyfile = os.path.join(work, "signer.key")
    with open(keyfile, "w") as f:
        f.write(KEY0)
    port = free_port()
    url = "http://127.0.0.1:%d" % port
    log = open(os.path.join(work, "store.log"), "w")
    store = subprocess.Popen([sys.executable, "-I", os.path.join(here, "hpplite_store.py"), "--port", str(port), "--dir", os.path.join(work, "s"),
                              "--signer-key", keyfile, "--pay-to", "0x" + "11" * 20, "--network", "eip155:%d" % chain_id, "--rpc", RPC,
                              "--price-per-write", "0", "--price-per-mib-month", "0", "--tick-interval", "3600"], stdout=log, stderr=log)
    try:
        for _ in range(100):
            try:
                if http("GET", url + "/status")[0] == 200:
                    break
            except OSError:
                time.sleep(0.1)
        rc = run(chain_id, fl, url, owner, dbowner, seq)
    finally:
        store.terminate()
        store.wait()
    if USDC and FACILITATOR:
        rc = paid(chain_id, fl, work) or rc
    else:
        print("  (no MockUSDC or facilitator: the charging store's checks skipped)")
    return rc


def paid(chain_id, fl, work):
    """a store that charges (#311): a fleet database's bucket bound by BindDb (before its record, by the owner alone),
    its writes charged, capped by CapDb and released by ReleaseDb; an unbound database's write refused as a rollup's is"""
    import secrets
    pay_to = eth.address(secrets.token_bytes(32)).lower()
    with open(os.path.join(work, "signer2.key"), "w") as f:
        f.write(KEY2)
    port = free_port()
    url = "http://127.0.0.1:%d" % port
    log = open(os.path.join(work, "store2.log"), "w")
    store = subprocess.Popen([sys.executable, "-I", os.path.join(here, "hpplite_store.py"), "--port", str(port), "--dir", os.path.join(work, "s2"),
                              "--signer-key", os.path.join(work, "signer2.key"), "--pay-to", pay_to, "--rpc", RPC, "--facilitator", FACILITATOR,
                              "--network", "eip155:%d" % chain_id, "--asset", USDC, "--price-per-write", "10", "--price-per-mib-month", "0",
                              "--pull-threshold", "2000", "--tick-interval", "3600"], stdout=log, stderr=log)
    try:
        for _ in range(100):
            try:
                if http("GET", url + "/status")[0] == 200:
                    break
            except OSError:
                time.sleep(0.1)
        sdom = eth.domain_separator("HPPLiteStore", "2", chain_id, pay_to)
        dom = F.domain(chain_id, fl)
        fk = secrets.token_bytes(32)                       # the funding wallet: USDC.e, no ETH
        funder = eth.address(fk).lower()
        cast("send", "--private-key", KEY0, USDC, "transfer(address,uint256)", funder, "5000000")
        dbowner, seq = eth.address(kb(KEY1)).lower(), eth.address(kb(KEY3)).lower()
        name = "orders"
        db = F.db_id(dbowner, name)
        dbx, nh = hx(db), hx(F.name_hash(name))
        issued = int(time.time())

        def binddb(db_key, owner=dbowner, name_hash=nh, issued_at=issued):
            d = eth.typed_digest(sdom, "BindDb(address fleet,bytes32 dbId,address funder,uint256 issuedAt)", fl, db, funder, issued_at)
            return http("POST", url + "/fleet/%s/db/%s/bucket" % (fl, dbx), json.dumps({
                "funder": funder, "issuedAt": issued_at, "owner": owner, "nameHash": name_hash,
                "funderSignature": hx(eth.sign(fk, d)), "dbSignature": hx(eth.sign(kb(db_key), d))}).encode(), {"Content-Type": "application/json"})
        st, body = binddb(KEY2)
        check(st == 401, "BindDb before the record, signed by someone other than the owner: 401 (%s)" % st)
        st, body = binddb(KEY1, name_hash=hx(F.name_hash("another name")))
        check(st == 400, "BindDb before the record with a nameHash that does not make the dbId: 400 (%s)" % st)
        st, body = binddb(KEY1)
        check(st == 200 and body.get("state") in ("covered", "parked"), "BindDb before the record, by the owner alone with owner and nameHash: %s" % body)

        # join it; an unbound database (a second one) gets the rollup's refusal
        deadline = int(time.time()) + 600
        inst = eth.keccak256(b"host-orders")
        admit = eth.sign(kb(KEY0), eth.typed_digest(dom, F.ADMIT_TYPE, db, dbowner, seq, issued))
        claim = eth.sign(kb(KEY3), eth.typed_digest(dom, F.CLAIM_TYPE, db, inst, 0, deadline))
        cast("send", "--private-key", KEY2, fl, "joinFor(bytes32,address,address,bytes32,uint256,bytes,bytes32,uint256,uint256,bytes)",
             dbx, dbowner, seq, nh, str(issued), hx(admit), hx(inst), "0", str(deadline), hx(claim))

        def write(height, data, chain):
            after = fleet_store.chain_hash_next(chain, height, eth.keccak256(data))
            head = {"dbId": db, "instanceId": inst, "fromHeight": height, "toHeight": height, "chainHashAfter": after,
                    "stateRoot": bytes(32), "manifestDigest": hashlib.sha256(data).digest(), "epoch": 1}
            sig = eth.sign(kb(KEY3), F.head_digest(dom, head))
            hj = json.dumps({k: (hx(v) if isinstance(v, bytes) else v) for k, v in head.items()})
            st, body = http("POST", url + "/fleet/%s/name/%s-%d" % (fl, dbx, height), data + inst + sig, {"X-HPPLite-Head": hj})
            return st, body, after

        # the funder's permit: the account can pay
        nonce = int(cast("call", USDC, "nonces(address)(uint256)", funder).split()[0])
        spender = http("GET", url + "/status")[1]["buckets"]["spender"]
        pd = eth.typed_digest(eth.domain_separator("Bridged USDC", "2", chain_id, USDC),
                              "Permit(address owner,address spender,uint256 value,uint256 nonce,uint256 deadline)", funder, spender, 1000000, nonce, deadline + 86400)
        st, body = http("POST", url + "/account/%s/permit" % funder, json.dumps({"value": 1000000, "deadline": deadline + 86400,
                                                                                "signature": hx(eth.sign(fk, pd))}).encode(), {"Content-Type": "application/json"})
        check(st == 200, "the funder's permit taken (%s)" % st)
        st, body, chain = write(1, b"order one", bytes(32))
        check(st == 200, "a write into the bound database: taken, charged to its bucket (%s %s)" % (st, body))
        st, bview = http("GET", url + "/fleet/%s/db/%s/bucket" % (fl, dbx))
        check(st == 200 and bview.get("funder") == funder and bview.get("today", {}).get("writes") == 1 and bview.get("height") == 1,
              "GET the database's bucket: its funder, one write today, height 1 (%s)" % bview)
        st, acct = http("GET", url + "/account/" + funder)
        check({"fleet": fl, "dbId": dbx} in acct.get("databases", []) and acct.get("buckets") == [],
              "the account lists the database apart, buckets staying addresses (%s, %s)" % (acct.get("databases"), acct.get("buckets")))
        cap = eth.typed_digest(sdom, "CapDb(address fleet,bytes32 dbId,uint256 writesPerDay,uint256 bytesPerDay,uint256 issuedAt)", fl, db, 1, 0, issued + 1)
        st, body = http("POST", url + "/fleet/%s/db/%s/bucket/cap" % (fl, dbx), json.dumps({"funder": funder, "writesPerDay": 1, "bytesPerDay": 0,
                        "issuedAt": issued + 1, "signature": hx(eth.sign(fk, cap))}).encode(), {"Content-Type": "application/json"})
        check(st == 200, "CapDb by the funder: one write a day (%s %s)" % (st, body))
        st, body, _ = write(2, b"order two", chain)
        check(st == 429 and body.get("error") == "bucket_capped", "the second write past the cap: 429 bucket_capped (%s %s)" % (st, body.get("error")))
        rel = eth.typed_digest(sdom, "ReleaseDb(address fleet,bytes32 dbId,address funder,uint256 issuedAt)", fl, db, funder, issued + 2)
        st, body = http("POST", url + "/fleet/%s/db/%s/bucket/release" % (fl, dbx), json.dumps({"funder": funder, "issuedAt": issued + 2,
                        "signature": hx(eth.sign(fk, rel))}).encode(), {"Content-Type": "application/json"})
        check(st == 200 and body.get("state") == "parked", "ReleaseDb by the funder: the bucket parked (%s)" % body)
        st, body, _ = write(2, b"order two", chain)
        check(st == 402 and body.get("error") == "bucket_unbound", "a write after the release: 402 bucket_unbound, as an unbound rollup's (%s %s)" % (st, body.get("error")))
        print("fleet store selftest (charging store): %s" % ("ok" if not fails else "%d FAIL so far" % fails))
        return 1 if fails else 0
    finally:
        store.terminate()
        store.wait()


def run(chain_id, fl, url, owner, dbowner, seq):
    dom = F.domain(chain_id, fl)
    name = "notes"
    db = F.db_id(dbowner, name)
    dbx = hx(db)

    # join: the fleet owner's Admit and the sequencer's first Claim, one call (anyone submits it)
    issued, deadline = int(time.time()), int(time.time()) + 600
    inst_a = eth.keccak256(b"host-a")
    admit = eth.sign(kb(KEY0), eth.typed_digest(dom, F.ADMIT_TYPE, db, dbowner, seq, issued))
    claim = eth.sign(kb(KEY3), eth.typed_digest(dom, F.CLAIM_TYPE, db, inst_a, 0, deadline))
    cast("send", "--private-key", KEY2, fl, "joinFor(bytes32,address,address,bytes32,uint256,bytes,bytes32,uint256,uint256,bytes)",
         dbx, dbowner, seq, hx(F.name_hash(name)), str(issued), hx(admit), hx(inst_a), "0", str(deadline), hx(claim))
    st, view = http("GET", url + "/fleet/%s/db/%s" % (fl, dbx))
    check(st == 200 and view.get("epoch") == 1 and view.get("instance") == hx(inst_a) and view.get("sequencer") == seq,
          "joined: the store's view from the fleet's events, epoch 1, the lease's instance (%s %s)" % (st, view))

    chain = bytes(32)

    def write(height, data, instance, epoch, key=KEY3, chain_after=None, digest=None):
        nonlocal chain
        after = chain_after or fleet_store.chain_hash_next(chain, height, eth.keccak256(data))
        head = {"dbId": db, "instanceId": instance, "fromHeight": height, "toHeight": height, "chainHashAfter": after,
                "stateRoot": eth.keccak256(b"root-%d" % height), "manifestDigest": digest or hashlib.sha256(data).digest(), "epoch": epoch}
        sig = eth.sign(kb(key), F.head_digest(dom, head))
        hj = json.dumps({k: (hx(v) if isinstance(v, bytes) else v) for k, v in head.items()})
        r = http("POST", url + "/fleet/%s/name/%s-%d" % (fl, dbx, height), data + instance + sig, {"X-HPPLite-Head": hj})
        if r[0] == 200:
            chain = after
        return r

    st, body = write(1, b"batch one", inst_a, 1, digest=eth.keccak256(b"not the bytes' digest"))
    check(st == 400 and "manifestDigest" in body.get("error", ""), "a head whose manifestDigest is not sha-256 of the bytes: 400, nothing stored (%s %s)" % (st, body.get("error")))
    st, body = write(1, b"batch one", inst_a, 1)
    check(st == 200 and body.get("cid"), "batch 1 with its head: taken (%s %s)" % (st, body))
    st, body = write(2, b"batch two", inst_a, 1, chain_after=eth.keccak256(b"not the fold"))
    check(st == 400 and "chain hash" in body.get("error", ""), "a head whose chain hash does not follow from the bytes: 400 (%s)" % body.get("error"))
    st, body = write(2, b"batch two", inst_a, 1, key=KEY1)
    check(st == 403, "a head signed by another key than the database's sequencer: 403 (%s)" % st)
    st, body = write(2, b"batch two", inst_a, 1)
    check(st == 200, "batch 2: taken (%s %s)" % (st, body))
    st, data = http("GET", url + "/fleet/%s/name/%s-2" % (fl, dbx))
    check(st == 200 and data[:-97] == b"batch two", "batch 2 read back by its name")
    st, body = write(5, b"past a gap", inst_a, 1)
    check(st == 409 and body.get("expected") == 3, "a write past a gap: 409 with the height expected (%s %s)" % (st, body))

    # the fence: the owner expires the lease; with no hint and the tick an hour away, the old holder's next write is
    # refused once the store's view is older than --fleet-follow (5 s): the window is bounded by it (#317)
    cast("send", "--private-key", KEY1, fl, "forceExpire(bytes32)", dbx)
    time.sleep(6)
    st, body = write(3, b"from the old holder", inst_a, 1)
    check(st == 409, "the old holder's write 6 s after the takeover, no hint, the tick an hour away: 409, the fence (%s %s)" % (st, body.get("error")))
    st, view = http("GET", url + "/fleet/%s/db/%s" % (fl, dbx))
    check(view.get("epoch") == 2, "after forceExpire the store's view holds epoch 2 (%s)" % view.get("epoch"))
    # a new holder claims; its first write comes before the store polled: read on chain at once, taken
    inst_b = eth.keccak256(b"host-b")
    nonce = int(cast("call", fl, "db(bytes32)(address,address,bytes32,bytes32,uint64,uint64,uint64,uint64,bool,uint256,address,uint256,bytes32,bytes32,address)", dbx).split("\n")[9].split()[0])
    claim = eth.sign(kb(KEY3), eth.typed_digest(dom, F.CLAIM_TYPE, db, inst_b, nonce, deadline))
    cast("send", "--private-key", KEY2, fl, "claimFor(bytes32,bytes32,uint256,uint256,bytes)", dbx, hx(inst_b), str(nonce), str(deadline), hx(claim))
    st, body = write(3, b"batch three", inst_b, 3)
    check(st == 200, "the new holder's first write, before the store polled the claim: read on chain, taken (%s %s)" % (st, body))

    # the anchor: the heads that moved, a manifest put first, the root posted; the store indexes it from RootPosted
    check("anchored" not in http("GET", url + "/fleet/%s/db/%s" % (fl, dbx))[1], "before any root: no anchored in the view (absent, not zeros)")
    st, heads = http("GET", url + "/fleet/%s/heads" % fl)
    check(st == 200 and len(heads.get("heads", [])) == 1 and heads["heads"][0]["head"]["toHeight"] == 3,
          "GET heads: the database, at its head 3 (%s)" % [h["head"]["toHeight"] for h in heads.get("heads", [])])
    h = heads["heads"][0]
    items = [{"dbId": dbx, "owner": dbowner, "nameHash": hx(F.name_hash(name)), "head": h["head"], "signature": h["signature"]}]
    mdata, mdigest = F.manifest(chain_id, fl, 1, items)
    sdom = eth.domain_separator("HPPLiteStore", "2", chain_id, "0x" + "11" * 20)
    csig = eth.sign(kb(KEY2), eth.typed_digest(sdom, "Content(address da,uint256 height,bytes32 hash)", fl, 1, eth.keccak256(mdata)))
    st, body = http("POST", url + "/fleet/%s/manifest" % fl, mdata, {"X-HPPLite-Signature": hx(csig)})
    check(st == 200, "the manifest put by the fleet's anchor (%s %s)" % (st, body))
    st, body = http("POST", url + "/fleet/%s/manifest" % fl, mdata, {"X-HPPLite-Signature": hx(eth.sign(kb(KEY1), eth.keccak256(mdata)))})
    check(st == 401, "a manifest not signed by the anchor: 401 (%s)" % st)
    hd = F.head_digest(dom, fleet_store._head(h["head"]))
    root = F.root([(db, F.leaf(db, dbowner, F.name_hash(name), hd))])
    rsig = eth.sign(kb(KEY2), eth.typed_digest(dom, F.ROOT_TYPE, 1, root, mdigest, deadline))
    cast("send", "--private-key", KEY0, fl, "postRootFor(uint256,bytes32,bytes32,uint256,bytes)", "1", hx(root), hx(mdigest), str(deadline), hx(rsig))
    st, roots = http("POST", url + "/fleet/%s/root" % fl, b"")
    st, view = http("GET", url + "/fleet/%s/db/%s" % (fl, dbx))
    anch = view.get("anchored") or {}
    check(anch.get("seq") == 1 and anch.get("toHeight") == 3 and isinstance(anch.get("time"), int) and anch["time"] > 0,
          "the database's view carries anchored {seq 1, toHeight 3, RootPosted's block time} (%s)" % anch)
    check(roots.get("roots", {}).get("1", {}).get("state") == "ok" and view.get("latestRoot") == 1,
          "root 1 indexed from RootPosted, its manifest checked; the database's latestRoot 1 (%s, %s)" % (roots.get("roots"), view.get("latestRoot")))
    st, heads = http("GET", url + "/fleet/%s/heads" % fl)
    check(heads.get("heads") == [], "GET heads after the root: nothing moved (%s)" % heads.get("heads"))
    st, body = write(4, b"batch four", inst_b, 3)
    st, heads = http("GET", url + "/fleet/%s/heads" % fl)
    check(len(heads.get("heads", [])) == 1 and heads["heads"][0]["head"]["toHeight"] == 4, "a new write: the database listed again, at 4")
    st, got = http("GET", url + "/get/" + fleet_store._cid(hashlib.sha256(mdata).digest()))
    check(st == 200 and got == mdata, "the manifest read back by its CID (%s)" % st)
    print("fleet store selftest: %s" % ("ok" if not fails else "%d FAIL" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
