#!/usr/bin/env python3
"""The relay's v11 calls (#313, doc/fleet-protocol.md "The relay") on anvil: createFleetFor unsponsored (the 402);
SponsorDb before the record by a stranger 401, with a wrong nameHash 400, by the owner alone 200; a sponsored join
charged to the database's sponsor (what the call used); CapDb 1 a day, the next claim 402 with the why; retireFor
through over that cap, charged; an unsponsored fleet's root 402; the fleet's Sponsor (beside its owner) paying its root;
the database's sponsor never charged for a root, the fleet's never for a join."""
import json
import os
import secrets
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

RPC = os.environ["HPPLITE_ANVIL_RPC"]
FACTORY = os.environ["HPPLITE_ANVIL_FACTORY"]
# HPPLITE_RELAY_ASSET=chip: the relay paid in the chain's CHIP (#346: the real one on a fork of HPP Mainnet), else USDC.e
if os.environ.get("HPPLITE_RELAY_ASSET") == "chip":
    USDC, ASSET_NAME, ASSET_VERSION = os.environ.get("HPPLITE_ANVIL_CHIP", ""), "House Chip", "1"
else:
    USDC, ASSET_NAME, ASSET_VERSION = os.environ.get("HPPLITE_ANVIL_USDC", ""), "Bridged USDC", "2"
FACILITATOR = os.environ.get("HPPLITE_ANVIL_FACILITATOR", "")
KEY0, KEY1, KEY2, KEY3 = (os.environ["HPPLITE_ANVIL_KEY%d" % i] for i in range(4))
RELAY_KEY = os.environ["HPPLITE_ANVIL_KEY9"]
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


def http(method, url, obj=None):
    body = json.dumps(obj).encode() if obj is not None else None
    req = urllib.request.Request(url, data=body, method=method, headers={"Content-Type": "application/json"} if body else {})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {}


def calldata(sig, *args):
    return subprocess.run(["cast", "calldata", sig] + [str(a) for a in args], capture_output=True, text=True, check=True).stdout.strip()


def main():
    if not USDC or not FACILITATOR:
        print("SKIP: no token or facilitator on this chain")
        return 77
    chain_id = int(cast("chain-id"))
    out = subprocess.run(["forge", "create", "--root", CONTRACTS, "src/HPPLiteFleetFactory.sol:HPPLiteFleetFactory", "--rpc-url", RPC,
                          "--private-key", KEY0, "--broadcast"], capture_output=True, text=True)
    ff = next((l.split()[-1] for l in out.stdout.splitlines() if l.startswith("Deployed to:")), None)
    check(ff is not None, "the v11 factory deployed")
    if not ff:
        return 1
    owner, anchor = eth.address(kb(KEY0)).lower(), eth.address(kb(KEY3)).lower()
    work = tempfile.mkdtemp(prefix="hpplite-relay-fleet.")
    keyfile = os.path.join(work, "relay.key")
    with open(keyfile, "w") as f:
        f.write(RELAY_KEY)
    port = free_port()
    url = "http://127.0.0.1:%d" % port
    log = open(os.path.join(work, "relay.log"), "w")
    proc = subprocess.Popen([sys.executable, "-I", os.path.join(here, "relay.py"), "--port", str(port), "--rpc", RPC, "--key-file", keyfile,
                             "--factory", FACTORY, "--fleet-factory", ff, "--facilitator", FACILITATOR, "--asset", USDC, "--asset-name", ASSET_NAME,
                             "--asset-version", ASSET_VERSION, "--eth-usd", "3000",
                             "--dir", os.path.join(work, "relay")], stdout=log, stderr=log)
    try:
        for _ in range(100):
            try:
                if http("GET", url + "/status")[0] == 200:
                    break
            except OSError:
                time.sleep(0.1)
        return run(chain_id, ff, url, owner, anchor)
    finally:
        proc.terminate()
        proc.wait()


def run(chain_id, ff, url, owner, anchor):
    st = http("GET", url + "/status")[1]
    check(st.get("fleetFactory") == ff.lower() and "joinFor" in st.get("fleetCalls", []), "the relay names the v11 factory and its calls")
    rdom = eth.domain_separator("HPPLiteRelay", "1", chain_id, st["accounts"]["payTo"])
    spender = st["accounts"]["spender"]
    deadline = int(time.time()) + 3600

    # createFleetFor: no fleet yet, so no sponsor: the caller's 402
    fdom = F.factory_domain(chain_id, ff)
    cs = eth.sign(kb(KEY0), eth.typed_digest(fdom, F.CREATE_FLEET_TYPE, owner, anchor, 0, 0, deadline))
    code, body = http("POST", url + "/commit", {"to": ff, "data": calldata("createFleetFor(address,address,uint64,uint256,uint256,bytes)",
                                                                            owner, anchor, 0, 0, deadline, hx(cs))})
    check(code == 402 and "sponsor" not in body, "createFleetFor through the relay: the caller's 402, never sponsored (%s)" % code)
    cast("send", "--private-key", KEY0, ff, "createFleet(address,uint64)", anchor, "0")
    fl = cast("call", ff, "fleets(uint256)(address)", "0").lower()
    dom = F.domain(chain_id, fl)

    # two funders: A pays for a database, B for the fleet's roots
    def funder_with_permit():
        k = secrets.token_bytes(32)
        a = eth.address(k).lower()
        cast("send", "--private-key", KEY0, USDC, "transfer(address,uint256)", a, "5000000")
        nonce = int(cast("call", USDC, "nonces(address)(uint256)", a).split()[0])
        pd = eth.typed_digest(eth.domain_separator(ASSET_NAME, ASSET_VERSION, chain_id, USDC),
                              "Permit(address owner,address spender,uint256 value,uint256 nonce,uint256 deadline)", a, spender, 3000000, nonce, deadline)
        code, _ = http("POST", url + "/account/%s/permit" % a, {"value": 3000000, "deadline": deadline, "signature": hx(eth.sign(k, pd))})
        return k, a, code
    ka, fa, ca = funder_with_permit()
    kbb, fb, cb = funder_with_permit()
    check(ca == 200 and cb == 200, "two funders' permits taken")

    dbowner, seq = eth.address(kb(KEY1)).lower(), eth.address(kb(KEY2)).lower()
    name = "notes"
    db = F.db_id(dbowner, name)
    dbx, nh = hx(db), hx(F.name_hash(name))
    issued = int(time.time())

    def sponsor_db(db_key, name_hash=nh):
        d = eth.typed_digest(rdom, "SponsorDb(address fleet,bytes32 dbId,address funder,uint256 issuedAt)", fl, db, fa, issued)
        return http("POST", url + "/fleet/%s/db/%s/sponsor" % (fl, dbx), {"funder": fa, "issuedAt": issued, "owner": dbowner, "nameHash": name_hash,
                    "funderSignature": hx(eth.sign(ka, d)), "dbSignature": hx(eth.sign(kb(db_key), d))})
    code, body = sponsor_db(KEY3)
    check(code == 401, "SponsorDb before the record, signed by a stranger: 401 (%s)" % code)
    code, body = sponsor_db(KEY1, hx(F.name_hash("another")))
    check(code == 400, "SponsorDb before the record with a wrong nameHash: 400 (%s)" % code)
    code, body = sponsor_db(KEY1)
    check(code == 200 and body.get("funder") == fa and body.get("dbId") == dbx, "SponsorDb before the record, by the owner alone: 200 (%s)" % body)

    def credit(a):
        return http("GET", url + "/account/" + a)[1].get("credit", 0)

    # the join through the relay: sponsored, charged to A (what it used); B untouched
    inst = eth.keccak256(b"host-a")
    admit = eth.sign(kb(KEY0), eth.typed_digest(dom, F.ADMIT_TYPE, db, dbowner, seq, issued))
    claim = eth.sign(kb(KEY2), eth.typed_digest(dom, F.CLAIM_TYPE, db, inst, 0, deadline))
    b0 = credit(fb)
    code, body = http("POST", url + "/commit", {"to": fl, "data": calldata(
        "joinFor(bytes32,address,address,bytes32,uint256,bytes,bytes32,uint256,uint256,bytes)",
        dbx, dbowner, seq, nh, issued, hx(admit), hx(inst), 0, deadline, hx(claim))})
    check(code == 200 and body.get("sponsor") == fa and body.get("charged", 0) > 0, "joinFor through the relay, sponsored by the database's funder (%s %s)" % (code, body))
    check(credit(fb) == b0, "the fleet's sponsor not charged for a join")
    view = http("GET", url + "/fleet/%s/db/%s/sponsor" % (fl, dbx))[1]
    check(view.get("today", {}).get("calls") == 1, "the database's sponsorship counts the join (%s)" % view.get("today"))

    # CapDb: one a day; the renewal past it gets the 402 with the why; retireFor goes through over the cap, charged
    cap = eth.typed_digest(rdom, "CapDb(address fleet,bytes32 dbId,uint256 callsPerDay,uint256 issuedAt)", fl, db, 1, issued + 1)
    code, body = http("POST", url + "/fleet/%s/db/%s/sponsor/cap" % (fl, dbx), {"funder": fa, "callsPerDay": 1, "issuedAt": issued + 1,
                                                                                "signature": hx(eth.sign(ka, cap))})
    check(code == 200, "CapDb by the sponsor: one call a day (%s)" % code)
    claim2 = eth.sign(kb(KEY2), eth.typed_digest(dom, F.CLAIM_TYPE, db, inst, 1, deadline))
    code, body = http("POST", url + "/commit", {"to": fl, "data": calldata("claimFor(bytes32,bytes32,uint256,uint256,bytes)", dbx, hx(inst), 1, deadline, hx(claim2))})
    check(code == 402 and (body.get("sponsor") or {}).get("why", "").startswith("capped"), "the claim past the cap: 402 naming the sponsor and why (%s %s)" % (code, body.get("sponsor")))
    # expireFor (#321): the owner's signed forceExpire, through the relay over the cap: charged, not counted, as a retirement
    esig = eth.sign(kb(KEY1), eth.typed_digest(dom, F.EXPIRE_TYPE, db, 1, deadline))
    code, body = http("POST", url + "/commit", {"to": fl, "data": calldata("expireFor(bytes32,uint256,uint256,bytes)", dbx, 1, deadline, hx(esig))})
    view = http("GET", url + "/fleet/%s/db/%s/sponsor" % (fl, dbx))[1]
    check(code == 200 and body.get("sponsor") == fa and body.get("charged", 0) > 0 and view.get("today", {}).get("calls") == 1,
          "expireFor over the cap: through, charged to the database's sponsor, not counted (%s %s, today %s)" % (code, body.get("charged", body.get("error")), view.get("today", {}).get("calls")))
    rsig = eth.sign(kb(KEY1), eth.typed_digest(dom, F.RETIRE_TYPE, db, issued + 2))
    code, body = http("POST", url + "/commit", {"to": fl, "data": calldata("retireFor(bytes32,uint256,bytes)", dbx, issued + 2, hx(rsig))})
    view = http("GET", url + "/fleet/%s/db/%s/sponsor" % (fl, dbx))[1]
    check(code == 200 and body.get("sponsor") == fa and body.get("charged", 0) > 0 and view.get("today", {}).get("calls") == 1,
          "retireFor over the cap: through, charged to the database's sponsor, not counted (%s, charged %s, today %s)" % (code, body.get("charged"), view.get("today", {}).get("calls")))

    # the fleet's roots: unsponsored, the caller's 402; Sponsor(fleet) beside the fleet's owner, then the root paid by B
    root, man = eth.keccak256(b"root-1"), eth.keccak256(b"manifest-1")
    rootsig = eth.sign(kb(KEY3), eth.typed_digest(dom, F.ROOT_TYPE, 1, root, man, deadline))
    rdata = calldata("postRootFor(uint256,bytes32,bytes32,uint256,bytes)", 1, hx(root), hx(man), deadline, hx(rootsig))
    code, body = http("POST", url + "/commit", {"to": fl, "data": rdata})
    check(code == 402 and "sponsor" not in body, "an unsponsored fleet's root: the caller's 402 (%s)" % code)
    sp = eth.typed_digest(rdom, "Sponsor(address rollup,address funder,uint256 issuedAt)", fl, fb, issued)
    code, body = http("POST", url + "/sponsor/" + fl, {"funder": fb, "issuedAt": issued, "signature": hx(eth.sign(kbb, sp)),
                                                      "rollupSignature": hx(eth.sign(kb(KEY3), sp))})
    check(code == 401, "a fleet's Sponsor beside the anchor, not the owner: 401 (%s)" % code)
    code, body = http("POST", url + "/sponsor/" + fl, {"funder": fb, "issuedAt": issued, "signature": hx(eth.sign(kbb, sp)),
                                                      "rollupSignature": hx(eth.sign(kb(KEY0), sp))})
    check(code == 200, "the fleet's Sponsor beside the fleet's owner: 200 (%s %s)" % (code, body))
    a1 = credit(fa)
    code, body = http("POST", url + "/commit", {"to": fl, "data": rdata})
    check(code == 200 and body.get("sponsor") == fb, "the root through the relay, paid by the fleet's sponsor (%s %s)" % (code, body))
    check(credit(fa) == a1, "the database's sponsor not charged for a root")
    acct = http("GET", url + "/account/" + fa)[1]
    check({"fleet": fl, "dbId": dbx} in acct.get("databases", []) and acct.get("rollups") == [], "the account lists the database apart (%s)" % acct.get("databases"))
    print("relay fleet selftest: %s" % ("ok" if not fails else "%d FAIL" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
