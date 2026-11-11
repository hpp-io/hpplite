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


def unit_manifest_sweep():
    """somaro's condition for free manifests: one no root on chain names is swept after --sweep-age, one a root names kept"""
    import hpplite_store
    work = tempfile.mkdtemp(prefix="fleet-sweep-")
    book = fleet_store.FleetBook(None, hpplite_store.Disk(work), 181228)
    fl = "0x" + "f2" * 20
    kept, orphan, fresh = b'{"seq":1}', b'{"seq":2}', b'{"seq":3}'
    cids = {}
    for name, body in (("kept", kept), ("orphan", orphan), ("fresh", fresh)):
        cids[name] = fleet_store._cid(hashlib.sha256(body).digest())
        book.disk.write("c/%s" % cids[name], body)
        book.disk.set_json("c/%s.refs" % cids[name], {fl: 1})
    book.disk.set_json(book._key(fl), {"fleet": fl, "block": 1, "dbs": {},
                                       "roots": {"1": {"root": "0x" + "00" * 32, "manifest": "0x" + hashlib.sha256(kept).hexdigest(), "state": "ok"}}})
    now = int(time.time())
    book.disk.set_json("f/%s/manifests.json" % fl, {"1": {"cid": cids["kept"], "at": now - 7200}, "2": {"cid": cids["orphan"], "at": now - 7200},
                                                    "3": {"cid": cids["fresh"], "at": now - 10}})
    book.sweep_manifests(now, 3600)
    have = {n: book.disk.read("c/%s" % c) is not None for n, c in cids.items()}
    check(have == {"kept": True, "orphan": False, "fresh": True},
          "free manifests: one its root names kept, one no root names swept after --sweep-age, a newer one kept (%s)" % have)


def main():
    unit_manifest_sweep()
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
            st, body = http("POST", url + "/fleet/%s/name/%s" % (fl, F.store_name(db, issued, height)), data + inst + sig, {"X-HPPLite-Head": hj})
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
        ocid = fleet_store._cid(hashlib.sha256(b"order one").digest())
        check(http("GET", url + "/get/" + ocid)[0] == 200, "the database's batch served at /get while its bucket is covered")
        rel = eth.typed_digest(sdom, "ReleaseDb(address fleet,bytes32 dbId,address funder,uint256 issuedAt)", fl, db, funder, issued + 2)
        st, body = http("POST", url + "/fleet/%s/db/%s/bucket/release" % (fl, dbx), json.dumps({"funder": funder, "issuedAt": issued + 2,
                        "signature": hx(eth.sign(fk, rel))}).encode(), {"Content-Type": "application/json"})
        check(st == 200 and body.get("state") == "parked", "ReleaseDb by the funder: the bucket parked (%s)" % body)
        st, body = http("GET", url + "/get/" + ocid)
        check(st == 410 and body.get("error") == "bucket_parked", "after the release its content is not served: 410 bucket_parked, as a parked rollup's (#323) (%s %s)" % (st, body))
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
    life = issued                                     # the admission the names are written under (#318)

    def write(height, data, instance, epoch, key=KEY3, chain_after=None, digest=None, admitted=None):
        nonlocal chain
        after = chain_after or fleet_store.chain_hash_next(chain, height, eth.keccak256(data))
        head = {"dbId": db, "instanceId": instance, "fromHeight": height, "toHeight": height, "chainHashAfter": after,
                "stateRoot": eth.keccak256(b"root-%d" % height), "manifestDigest": digest or hashlib.sha256(data).digest(), "epoch": epoch}
        sig = eth.sign(kb(key), F.head_digest(dom, head))
        hj = json.dumps({k: (hx(v) if isinstance(v, bytes) else v) for k, v in head.items()})
        r = http("POST", url + "/fleet/%s/name/%s" % (fl, F.store_name(db, admitted or life, height)), data + instance + sig, {"X-HPPLite-Head": hj})
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
    st, data = http("GET", url + "/fleet/%s/name/%s" % (fl, F.store_name(db, issued, 2)))
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
    st, body = http("POST", url + "/fleet/%s/manifest" % fl, mdata, {"X-HPPLite-Signature": hx(csig)})
    check(st == 409 and body.get("rootSeq") == 1, "a manifest for a root already posted: 409, one per root (somaro) (%s %s)" % (st, body))
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
    an1 = http("GET", url + "/fleet/%s/db/%s" % (fl, dbx))[1].get("anchored") or {}
    p1 = [(bytes.fromhex(x["hash"][2:]), x["left"]) for x in an1.get("path", [])]
    check(an1.get("root") == hx(root) and F.verify(F.leaf(db, dbowner, F.name_hash(name), hd), p1, root),
          "anchored names root 1 and the leaf's path into it, which verifies (%s)" % an1.get("path"))
    # root 2 holds another database only (a delta): the first stays anchored in root 1, never moved back
    oname = "ledger"
    odb = F.db_id(dbowner, oname)
    odbx, onh = hx(odb), F.name_hash(oname)
    oissued, oinst = issued + 2, eth.keccak256(b"host-orders")
    oadmit = eth.sign(kb(KEY0), eth.typed_digest(dom, F.ADMIT_TYPE, odb, dbowner, seq, oissued))
    oclaim = eth.sign(kb(KEY3), eth.typed_digest(dom, F.CLAIM_TYPE, odb, oinst, 0, deadline))
    cast("send", "--private-key", KEY2, fl, "joinFor(bytes32,address,address,bytes32,uint256,bytes,bytes32,uint256,uint256,bytes)",
         odbx, dbowner, seq, hx(onh), str(oissued), hx(oadmit), hx(oinst), "0", str(deadline), hx(oclaim))
    ohead = {"dbId": odb, "instanceId": oinst, "fromHeight": 1, "toHeight": 1, "chainHashAfter": fleet_store.chain_hash_next(bytes(32), 1, eth.keccak256(b"order 1")),
             "stateRoot": eth.keccak256(b"orders-root-1"), "manifestDigest": hashlib.sha256(b"order 1").digest(), "epoch": 1}
    osig = eth.sign(kb(KEY3), F.head_digest(dom, ohead))
    time.sleep(6)                                     # the store's view of the new database follows within --fleet-follow
    st, body = http("POST", url + "/fleet/%s/name/%s" % (fl, F.store_name(odb, oissued, 1)), b"order 1" + oinst + osig,
                    {"X-HPPLite-Head": json.dumps({k: (hx(v) if isinstance(v, bytes) else v) for k, v in ohead.items()})})
    check(st == 200, "a second database's batch 1 (%s %s)" % (st, body))
    oitems = [{"dbId": odbx, "owner": dbowner, "nameHash": hx(onh), "head": {k: (hx(v) if isinstance(v, bytes) else v) for k, v in ohead.items()}, "signature": hx(osig)}]
    m2, d2 = F.manifest(chain_id, fl, 2, oitems)
    csig2 = eth.sign(kb(KEY2), eth.typed_digest(sdom, "Content(address da,uint256 height,bytes32 hash)", fl, 2, eth.keccak256(m2)))
    check(http("POST", url + "/fleet/%s/manifest" % fl, m2, {"X-HPPLite-Signature": hx(csig2)})[0] == 200, "root 2's manifest put")
    ohd = F.head_digest(dom, ohead)
    root2 = F.root([(odb, F.leaf(odb, dbowner, onh, ohd))])
    rsig2b = eth.sign(kb(KEY2), eth.typed_digest(dom, F.ROOT_TYPE, 2, root2, d2, deadline))
    cast("send", "--private-key", KEY0, fl, "postRootFor(uint256,bytes32,bytes32,uint256,bytes)", "2", hx(root2), hx(d2), str(deadline), hx(rsig2b))
    http("POST", url + "/fleet/%s/root" % fl, b"")
    an1b = http("GET", url + "/fleet/%s/db/%s" % (fl, dbx))[1].get("anchored") or {}
    an2 = http("GET", url + "/fleet/%s/db/%s" % (fl, odbx))[1].get("anchored") or {}
    check(an1b.get("seq") == 1 and an1b.get("toHeight") == 3 and an1b.get("root") == hx(root),
          "a database absent from root 2 keeps root 1's anchored, never moved back (%s)" % {k: an1b.get(k) for k in ("seq", "toHeight")})
    check(an2.get("seq") == 2 and an2.get("toHeight") == 1 and an2.get("root") == hx(root2) and an2.get("path") == [],
          "the database in root 2: anchored seq 2, its one leaf the root, an empty path (%s)" % {k: an2.get(k) for k in ("seq", "toHeight", "path")})
    # root 3 posted before its manifest reached the store: kept "manifest missing"; its own manifest put late is taken
    # and indexed - any other manifest for a posted root is 409 (free manifests, bounded: somaro)
    ohead2 = dict(ohead, fromHeight=2, toHeight=2, chainHashAfter=fleet_store.chain_hash_next(ohead["chainHashAfter"], 2, eth.keccak256(b"order 2")),
                  stateRoot=eth.keccak256(b"orders-root-2"), manifestDigest=hashlib.sha256(b"order 2").digest())
    osig2 = eth.sign(kb(KEY3), F.head_digest(dom, ohead2))
    st, body = http("POST", url + "/fleet/%s/name/%s" % (fl, F.store_name(odb, oissued, 2)), b"order 2" + oinst + osig2,
                    {"X-HPPLite-Head": json.dumps({k: (hx(v) if isinstance(v, bytes) else v) for k, v in ohead2.items()})})
    oitems3 = [{"dbId": odbx, "owner": dbowner, "nameHash": hx(onh), "head": {k: (hx(v) if isinstance(v, bytes) else v) for k, v in ohead2.items()}, "signature": hx(osig2)}]
    m3, d3 = F.manifest(chain_id, fl, 3, oitems3)
    root3 = F.root([(odb, F.leaf(odb, dbowner, onh, F.head_digest(dom, ohead2)))])
    cast("send", "--private-key", KEY0, fl, "postRootFor(uint256,bytes32,bytes32,uint256,bytes)", "3", hx(root3), hx(d3), str(deadline),
         hx(eth.sign(kb(KEY2), eth.typed_digest(dom, F.ROOT_TYPE, 3, root3, d3, deadline))))
    st, roots = http("POST", url + "/fleet/%s/root" % fl, b"")
    check(roots.get("roots", {}).get("3", {}).get("state") == "manifest missing", "root 3 posted first: manifest missing (%s)" % roots.get("roots", {}).get("3"))
    other3 = m2.replace(b'"seq":2', b'"seq":3')
    csig3 = eth.sign(kb(KEY2), eth.typed_digest(sdom, "Content(address da,uint256 height,bytes32 hash)", fl, 3, eth.keccak256(other3)))
    st, body = http("POST", url + "/fleet/%s/manifest" % fl, other3, {"X-HPPLite-Signature": hx(csig3)})
    check(st == 409, "another manifest for posted root 3, signed by the anchor: 409 (%s %s)" % (st, body))
    csig3 = eth.sign(kb(KEY2), eth.typed_digest(sdom, "Content(address da,uint256 height,bytes32 hash)", fl, 3, eth.keccak256(m3)))
    st, body = http("POST", url + "/fleet/%s/manifest" % fl, m3, {"X-HPPLite-Signature": hx(csig3)})
    an3 = http("GET", url + "/fleet/%s/db/%s" % (fl, odbx))[1].get("anchored") or {}
    check(st == 200 and an3.get("seq") == 3 and an3.get("toHeight") == 2, "root 3's own manifest put late: taken, root 3 indexed (%s %s)" % (st, an3.get("seq")))
    st, got = http("GET", url + "/get/" + fleet_store._cid(hashlib.sha256(mdata).digest()))
    check(st == 200 and got == mdata, "the manifest read back by its CID (%s)" % st)
    # a new life (#318): retired, then admitted again - names keyed by the admission, the view the new life's, heights from 1
    rsig2 = eth.sign(kb(KEY1), eth.typed_digest(dom, F.RETIRE_TYPE, db, issued + 1))
    cast("send", "--private-key", KEY2, fl, "retireFor(bytes32,uint256,bytes)", dbx, str(issued + 1), hx(rsig2))
    issued2 = issued + 900
    inst_c = eth.keccak256(b"host-c")
    nonce = int(cast("call", fl, "db(bytes32)(address,address,bytes32,bytes32,uint64,uint64,uint64,uint64,bool,uint256,address,uint256,bytes32,bytes32,address)", dbx).split("\n")[9].split()[0])
    admit2 = eth.sign(kb(KEY0), eth.typed_digest(dom, F.ADMIT_TYPE, db, dbowner, seq, issued2))
    claim2 = eth.sign(kb(KEY3), eth.typed_digest(dom, F.CLAIM_TYPE, db, inst_c, nonce, deadline))
    cast("send", "--private-key", KEY2, fl, "joinFor(bytes32,address,address,bytes32,uint256,bytes,bytes32,uint256,uint256,bytes)",
         dbx, dbowner, seq, hx(F.name_hash(name)), str(issued2), hx(admit2), hx(inst_c), str(nonce), str(deadline), hx(claim2))
    time.sleep(6)                                     # the store's view follows within --fleet-follow (#317)
    st, view = http("GET", url + "/fleet/%s/db/%s" % (fl, dbx))
    check(st == 200 and view.get("admittedAt") == issued2 and view.get("latestHead") is None and not view.get("retired"),
          "re-admitted: the view is the new life's - admittedAt %d, no head yet (%s, %s)" % (issued2, view.get("admittedAt"), view.get("latestHead")))
    ep = view.get("epoch")
    st, body = write(5, b"the old life's next", inst_c, ep)
    check(st == 409 and body.get("error") == "not the database's current admission" and body.get("leaseAdmittedAt") == issued2,
          "a head under the old admission: 409, not the current admission (%s %s)" % (st, body))
    chain = bytes(32)
    life = issued2
    st, body = write(1, b"a new life's first", inst_c, ep)
    check(st == 200, "the new life's first batch at height 1, its chain hash from zero: taken (%s %s)" % (st, body))
    st, view = http("GET", url + "/fleet/%s/db/%s" % (fl, dbx))
    check((view.get("latestHead") or {}).get("head", {}).get("toHeight") == 1, "the view's latest head is the new life's, at 1 (%s)" % view.get("latestHead"))
    st, data = http("GET", url + "/fleet/%s/name/%s" % (fl, F.store_name(db, issued, 2)))
    check(st == 200 and data[:-97] == b"batch two", "the old life's names stay readable (verify): its batch 2 (%s)" % st)
    st, heads = http("GET", url + "/fleet/%s/heads" % fl)
    check([hh["head"]["toHeight"] for hh in heads.get("heads", [])] == [1], "GET heads lists the current life's head (%s)" % [hh["head"]["toHeight"] for hh in heads.get("heads", [])])

    # the reference hpplite-anchor (operators/fleet/anchor.py) and verify (operators/fleet/verify.py), as somaro's are
    here = os.path.dirname(os.path.abspath(__file__))
    kf = os.path.join(tempfile.mkdtemp(prefix="fleet-anchor-"), "anchor.key")
    with open(kf, "w") as f:
        f.write(KEY2)
    def tool(name, *a):
        r = subprocess.run([sys.executable, "-I", os.path.join(here, "..", "fleet", name), "--rpc", RPC, "--fleet", fl, "--store", url] + list(a),
                           capture_output=True, text=True)
        try:
            return r.returncode, json.loads(r.stdout.strip().splitlines()[-1])
        except (ValueError, IndexError):
            return r.returncode, {"stderr": r.stderr[-300:]}
    rc, line = tool("anchor.py", "--key-file", kf, "--send", "--once", "--force")
    check(rc == 0 and line.get("posted") and line.get("seq") == 4 and line.get("leaves") == [dbx],
          "hpplite-anchor: root 4 holds the one database that moved - the re-admitted one's new life (%s)" % line)
    rc, line = tool("anchor.py", "--key-file", kf, "--send", "--once", "--force")
    check(rc == 0 and line.get("posted") is False and line.get("leaves") == [], "hpplite-anchor again, nothing moved: nothing posted (%s)" % line)
    rc, v = tool("verify.py", "--db", dbx)
    check(rc == 0 and v.get("verified") and v.get("seq") == 4 and v.get("toHeight") == 1 and v.get("block"),
          "verify: the database's head in root 4 at block %s, checked on chain (%s)" % (v.get("block"), v.get("why")))
    rc, v = tool("verify.py", "--db", odbx)
    check(rc == 0 and v.get("verified") and v.get("seq") == 3 and v.get("toHeight") == 2, "verify: the other database's head in root 3 (%s)" % v.get("why"))
    rc, v = tool("verify.py", "--db", "0x" + "ab" * 32)
    check(rc == 1 and not v.get("verified") and "no root" in v.get("why", ""), "verify: a database no root holds - not verified, why said (%s)" % v.get("why"))
    print("fleet store selftest: %s" % ("ok" if not fails else "%d FAIL" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
