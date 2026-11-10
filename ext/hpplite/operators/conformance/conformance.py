#!/usr/bin/env python3
"""The conformance suite (#189, #213): what doc/store-protocol.md (hpplite-store/2)
and doc/relay-protocol.md say, checked call by call against any URL - the
reference in ctest, the canonical Workers under wrangler dev and on staging.

    conformance.py --store http://host:port [--da 0x... --sequencer-key 0x... --instance 0x...]
    conformance.py --relay http://host:port

A free store is checked through every call: binding, content and names,
the bucket and its statement, prune, release. A store that reads the chain
needs a real rollup (--da, its sequencer's key, the instance its lease
names). A paid one has its 402 checked as a top-up (its money paths are
the reference's anvil selftest, operators/store/bucket_selftest.py).
A relay is checked without spending: /status, the calls it refuses, and the
402's offers and quote for a call that would succeed (a createRollupFor by a
fresh wallet, signed). Exit 0 when everything conforms.
"""
import argparse
import base64
import http.client
import json
import os
import secrets
import sys
import time
import urllib.parse

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, ".."))
sys.path.insert(0, here)
from x402seller import eth  # noqa: E402


VECTORS = json.load(open(os.path.join(here, "vectors.json")))
fails = 0


def check(cond, what):
    global fails
    print(("  ok      " if cond else "  FAIL    ") + what, flush=True)
    if not cond:
        fails += 1
    return cond


def request(url, method, path, body=None, headers=None):
    u = urllib.parse.urlparse(url)
    conn = (http.client.HTTPSConnection if u.scheme == "https" else http.client.HTTPConnection)(u.hostname, u.port, timeout=120)
    try:
        conn.request(method, path, body=body, headers=headers or {})
        r = conn.getresponse()
        return r.status, {k.upper(): v for k, v in r.getheaders()}, r.read()
    finally:
        conn.close()


def as_json(data):
    try:
        return json.loads(data)
    except ValueError:
        return None


def cid_of(data):
    import hashlib
    alphabet = "abcdefghijklmnopqrstuvwxyz234567"
    raw = b"\x01\x55\x12\x20" + hashlib.sha256(data).digest()
    bits = "".join("{:08b}".format(b) for b in raw)
    bits += "0" * (-len(bits) % 5)
    return "b" + "".join(alphabet[int(bits[i:i + 5], 2)] for i in range(0, len(bits), 5))


BIND = "Bind(address da,address funder,uint256 issuedAt)"
RELEASE = "Release(address da,address funder,uint256 issuedAt)"
CONTENT = "Content(address da,uint256 height,bytes32 hash)"
PRUNE = "Prune(address da,uint256 height,uint256 issuedAt)"
STATEMENT = "BucketStatement(address da,uint256 height,uint256 pruneHeight,uint256 coveredUntil,uint256 issuedAt)"
BATCH = "Batch(bytes32 instanceId,uint256 height,bytes32 batchHash)"


def write_price(n, b):
    """what a node quotes for a write of n bytes: the fee and a month of the bytes' storage (doc/store-protocol.md)"""
    import math
    return b["pricePerWrite"] + math.ceil(b["pricePerMiBMonth"] * n / 1048576 - 1e-9)


def store(url, da=None, sequencer_key=None, instance=None):
    """hpplite-store/2. A store that reads the chain needs a real rollup: --da, --sequencer-key (its sequencer) and
    --instance (the one its lease names, for the names); without them a fresh DA and key, which only a store without a
    chain takes"""
    print("store %s" % url)
    st, _, data = request(url, "GET", "/status")
    status = as_json(data) or {}
    check(st == 200 and status.get("protocol") == "hpplite-store/2", "/status: protocol hpplite-store/2 (%r)" % status.get("protocol"))
    dom = status.get("domain") or {}
    b = status.get("buckets") or {}
    check(isinstance(status.get("signer"), str) and len(status["signer"]) == 42, "/status: the statement signer")
    check(dom.get("name") == "HPPLiteStore" and dom.get("version") == "2" and dom.get("chainId") and len(dom.get("verifyingContract", "")) == 42,
          "/status: the store's domain (HPPLiteStore, 2, chainId, payTo)")
    check(all(k in b for k in ("asset", "assetDomain", "payTo", "spender", "pricePerWrite", "pricePerMiBMonth",
                                "pullThreshold", "pullIntervalSeconds", "parkSeconds")), "/status: the bucket terms")
    pay = status.get("payment")
    if pay is not None:
        check(pay.get("scheme") == "exact" and all(k in pay for k in ("network", "facilitator", "minPay")), "/status: x402 exact, minPay")
    if not check(st == 200 and dom and b, "a /2 status to go on with"):
        return
    domain = eth.domain_separator("HPPLiteStore", "2", int(dom["chainId"]), dom["verifyingContract"])
    free = b["pricePerWrite"] == 0 and b["pricePerMiBMonth"] == 0
    given = bool(da and sequencer_key)
    seq = bytes.fromhex(sequencer_key[2:]) if sequencer_key else secrets.token_bytes(32)
    da = (da or eth.address(secrets.token_bytes(32))).lower()
    funder_key = secrets.token_bytes(32)
    funder = eth.address(funder_key).lower()
    other = secrets.token_bytes(32)

    def bind(skey, fkey, issued, f=None):
        f = f or eth.address(fkey).lower()
        d = eth.typed_digest(domain, BIND, da, f, issued)
        return request(url, "POST", "/bucket/" + da, json.dumps({"funder": f, "issuedAt": issued,
                       "sequencerSignature": "0x" + eth.sign(skey, d).hex(), "funderSignature": "0x" + eth.sign(fkey, d).hex()}).encode(),
                       {"Content-Type": "application/json"})

    def content(key, height, payload, sign_over=None):
        sig = eth.sign(key, eth.typed_digest(domain, CONTENT, da, height, eth.keccak256(sign_over or payload)))
        return request(url, "POST", "/put?bucket=%s&height=%d" % (da, height), payload, {"X-HPPLite-Signature": "0x" + sig.hex()})

    def named(key, height, payload, instance):
        d = eth.typed_digest(eth.domain_separator("HPPLiteDA", "3", int(dom["chainId"]), da), BATCH, instance, height, eth.keccak256(payload))
        body = payload + instance + eth.sign(key, d)
        return body, request(url, "POST", "/name/%s-%d" % (da[2:], height), body)

    now = int(time.time())
    # --- binding ---
    d = eth.typed_digest(domain, BIND, da, funder, now)
    st, _, out = request(url, "POST", "/bucket/" + da, json.dumps({"funder": funder, "issuedAt": now,
                         "sequencerSignature": "0x" + eth.sign(seq, d).hex(), "funderSignature": "0x" + eth.sign(other, d).hex()}).encode(),
                         {"Content-Type": "application/json"})
    check(st == 401, "a binding the funder did not sign: 401 (%d)" % st)
    st, _, out = request(url, "POST", "/put?bucket=%s&height=1" % da, b"before any binding",
                         {"X-HPPLite-Signature": "0x" + eth.sign(seq, eth.typed_digest(domain, CONTENT, da, 1, eth.keccak256(b"before any binding"))).hex()})
    check(st == 402 and (as_json(out) or {}).get("error") == "bucket_unbound", "a write into a bucket never bound: 402 bucket_unbound (%d)" % st)
    st, _, out = bind(seq, funder_key, now)
    if st == 404 and not given and status.get("chain"):
        # a store that reads the chain binds only a real rollup's bucket: what it must answer for a DA with none
        check("rollup" in str((as_json(out) or {}).get("error", "")), "a store that reads the chain: a DA with no rollup is not bound (404)")
        print("          the calls past the binding need a real rollup: --da, --sequencer-key, --instance")
        return
    if not check(st == 200 and (as_json(out) or {}).get("funder") == funder,
                 "bound by the sequencer and the funder: 200 (%d %s)" % (st, (as_json(out) or {}).get("error", ""))):
        if st in (401, 404):
            print("          a store that reads the chain takes a real rollup's bucket: --da and --sequencer-key")
        return
    st, _, _ = bind(seq, funder_key, now)
    check(st == 409, "the same binding again (issuedAt not newer): 409 (%d)" % st)
    st, _, _ = bind(other, funder_key, now + 1)
    check(st in (401, 403), "a binding signed by another key than the sequencer: refused (%d)" % st)

    # --- writes ---
    data = b"conformance " + secrets.token_bytes(64)
    if not free:
        st, h, body = content(seq, 1, data)
        req = json.loads(base64.b64decode(h["PAYMENT-REQUIRED"])) if h.get("PAYMENT-REQUIRED") else {}
        check(st == 402 and (as_json(body) or {}).get("error") in ("account_uncovered", "bucket_parked") or req.get("x402Version") == 2,
              "a write the account cannot cover: 402 (%d %s)" % (st, (as_json(body) or {}).get("error", "")))
        if pay is not None:
            exact = next((a for a in req.get("accepts") or [] if a.get("scheme") == "exact"), None)
            check(exact is not None and int(exact["amount"]) >= int(pay["minPay"]) and exact["payTo"].lower() == b["payTo"].lower()
                  and "name" in exact.get("extra", {}), "the 402 is a top-up: exact, at least minPay, to payTo, the token's domain")
        print("          a paid store: the writes past the 402 need money (a permit or an x402 top-up); the reference's are in bucket_selftest.py")
        return
    st, _, out = content(seq, 1, data)
    out = as_json(out) or {}
    check(st == 200 and out.get("cid") == cid_of(data) and out.get("held") is False, "a content put signed by the sequencer: 200, the CID, held false")
    st, _, out = content(seq, 1, data)
    check(st == 200 and (as_json(out) or {}).get("held") is True, "the same bytes again: 200, held")
    st, _, got = request(url, "GET", "/get/" + cid_of(data))
    check(st == 200 and got == data, "get by CID: the bytes")
    st, _, _ = content(seq, 1, b"other bytes", sign_over=data)
    check(st in (401, 403), "a Content signature over other bytes: refused (%d)" % st)
    st, _, _ = request(url, "POST", "/put?bucket=%s&height=1" % da, b"unsigned")
    check(st == 401, "a content put without X-HPPLite-Signature: 401 (%d)" % st)
    st, _, _ = content(other, 1, b"from another key")
    check(st in (401, 403), "a content put by another key: refused (%d)" % st)
    st, _, _ = request(url, "GET", "/get/" + cid_of(b"never put " + secrets.token_bytes(8)))
    check(st == 404, "get of a CID never put: 404")

    # --- names ---
 
    instance = bytes.fromhex(instance[2:]) if instance else secrets.token_bytes(32)
    batch = b"a batch " + secrets.token_bytes(32)
    body, (st, _, out) = named(seq, 2, batch, instance)
    check(st == 200 and (as_json(out) or {}).get("signer", "").lower() == eth.address(seq).lower(),
          "a name, no content put first: 200, the signer recovered (%d)" % st)
    st, h, got = request(url, "GET", "/name/%s-2" % da[2:])
    check(st == 200 and got == body and h.get("X-HPPLITE-SIGNER", "").lower() == eth.address(seq).lower(), "the name reads back, X-HPPLite-Signer the signer")
    st, _, got = request(url, "GET", "/get/" + cid_of(batch))
    check(st == 200 and got == batch, "a name's bytes read by their CID")
    _, (st, _, _) = named(seq, 2, batch, instance)
    check(st == 200, "the same name and body again: 200")
    _, (st, _, _) = named(other, 3, b"another signer", instance)
    check(st == 403, "a name by another signer: 403 (%d)" % st)
    st, _, _ = request(url, "GET", "/name/%s-99" % da[2:])
    check(st == 404, "a name never put: 404")
    st, _, _ = request(url, "POST", "/name/not-a-name", b"x" * 100)
    check(st == 400, "a malformed name: 400")
    named(seq, 3, b"batch three", instance)

    # --- the bucket, its statement ---
    st, _, out = request(url, "GET", "/bucket/" + da)
    bk = as_json(out) or {}
    s = bk.get("statement") or {}
    dig = eth.typed_digest(domain, STATEMENT, da, s.get("height", 0), s.get("pruneHeight", 0), s.get("coveredUntil", 0), s.get("issuedAt", 0))
    sig = bytes.fromhex(s.get("signature", "0x")[2:]) if s.get("signature") else b""
    check(st == 200 and bk.get("state") == "covered" and bk.get("funder") == funder, "the bucket: covered, its funder")
    check(len(sig) == 65 and eth.recover(dig, sig).lower() == status["signer"].lower() and s.get("height") == 3 and s.get("coveredUntil", 0) > now,
          "the statement: height 3, covered ahead, signed by /status's signer over BucketStatement")
    st, _, out = request(url, "GET", "/account/" + funder)
    acct = as_json(out) or {}
    check(st == 200 and acct.get("funder") == funder and da in (acct.get("buckets") or []) and "coveredUntil" in acct,
          "the account: its funder, its buckets, covered until")

    # --- prune ---
    issued = int(time.time())
    psig = eth.sign(seq, eth.typed_digest(domain, PRUNE, da, 3, issued))
    st, _, out = request(url, "POST", "/bucket/%s/prune" % da, json.dumps({"height": 3, "issuedAt": issued, "signature": "0x" + psig.hex()}).encode())
    check(st == 200 and (as_json(out) or {}).get("pruneHeight") == 3, "a prune at 3 by the sequencer: 200")
    check(request(url, "GET", "/name/%s-2" % da[2:])[0] == 404 and request(url, "GET", "/name/%s-3" % da[2:])[0] == 200, "names below 3 gone, 3 kept")
    st, _, _ = request(url, "POST", "/bucket/%s/prune" % da, json.dumps({"height": 3, "issuedAt": issued, "signature": "0x" + psig.hex()}).encode())
    check(st == 409, "the same prune again: 409 (%d)" % st)
    osig = eth.sign(other, eth.typed_digest(domain, PRUNE, da, 4, issued + 1))
    st, _, _ = request(url, "POST", "/bucket/%s/prune" % da, json.dumps({"height": 4, "issuedAt": issued + 1, "signature": "0x" + osig.hex()}).encode())
    check(st in (401, 403), "a prune by another key: refused (%d)" % st)

    # --- release ---
    issued = int(time.time()) + 2
    rsig = eth.sign(funder_key, eth.typed_digest(domain, RELEASE, da, funder, issued))
    st, _, out = request(url, "POST", "/bucket/%s/release" % da, json.dumps({"funder": funder, "issuedAt": issued, "signature": "0x" + rsig.hex()}).encode())
    check(st == 200 and (as_json(out) or {}).get("state") == "parked", "released by its funder: parked (%d)" % st)
    st, _, _ = request(url, "GET", "/name/%s-3" % da[2:])
    check(st == 410, "a parked bucket's name: 410 (%d)" % st)
    _, (st, _, out) = named(seq, 4, b"into a parked bucket", instance)
    check(st == 402, "a write into a parked bucket: 402 (%d)" % st)


# ---------------- the relay ----------------

def relay(url):
    print("relay %s" % url)
    st, _, data = request(url, "GET", "/status")
    status = as_json(data) or {}
    check(st == 200 and status.get("protocol") == "hpplite-relay/1", "/status: protocol hpplite-relay/1 (%r)" % status.get("protocol"))
    calls = set(VECTORS["relayCalls"]["selectors"])
    check(set(status.get("calls") or []) == calls, "/status: the calls taken are factory VERSION 10's signed forms")
    check(all(k in status for k in ("address", "chainId", "factory")), "/status: address, chainId, factory")

    def commit(obj, headers=None):
        st, h, body = request(url, "POST", "/commit", json.dumps(obj).encode(), dict({"Content-Type": "application/json"}, **(headers or {})))
        return st, h, as_json(body) or {}

    st, _, _ = commit({"to": "nope"})
    check(st == 400, "a body that is not {to, data}: 400")
    st, _, _ = commit({"to": "0x" + "22" * 20, "data": "0xa9059cbb" + "00" * 64})
    check(st == 400, "a call outside HPPLite (a token transfer): 400")
    st, _, _ = commit({"to": status.get("factory", "0x" + "00" * 20), "data": "0x12345678"})
    check(st == 400, "an unknown call on the factory: 400")

    # accounts and sponsorship (#276), where the relay keeps them: what it says of itself and what it refuses, nothing spent
    acc = status.get("accounts")
    if acc:
        dom = acc.get("domain") or {}
        check(all(k in acc for k in ("spender", "payTo", "minPull", "asset", "assetDomain")) and dom.get("name") == "HPPLiteRelay"
              and dom.get("version") == "1" and str(dom.get("verifyingContract", "")).lower() == str(acc.get("payTo", "")).lower(),
              "/status.accounts: spender, payTo, minPull, asset, assetDomain, and the domain (HPPLiteRelay, 1, chainId, payTo)")
        fk, nobody = secrets.token_bytes(32), "0x" + secrets.token_bytes(20).hex()
        funder, now = eth.address(fk).lower(), int(time.time())
        rdom = eth.domain_separator("HPPLiteRelay", "1", int(status.get("chainId", 0)), acc.get("payTo", "0x" + "00" * 20))
        stype = "Sponsor(address rollup,address funder,uint256 issuedAt)"

        def sponsor(rollup, key):
            sig = eth.sign(key, eth.typed_digest(rdom, stype, rollup, funder, now))
            st, _, body = request(url, "POST", "/sponsor/" + rollup, json.dumps({"funder": funder, "issuedAt": now, "signature": "0x" + sig.hex()}).encode(),
                                  {"Content-Type": "application/json"})
            return st
        check(sponsor(nobody, secrets.token_bytes(32)) == 401, "a Sponsor not signed by its funder: 401")
        check(sponsor(nobody, fk) == 404, "a Sponsor of an address that is no rollup of the factory: 404")
        st, _, _ = request(url, "GET", "/account/" + funder)
        check(st == 404, "GET /account/<funder> of a wallet the relay never heard of: 404")
        st, _, _ = request(url, "GET", "/sponsor/" + nobody)
        check(st == 404, "GET /sponsor/<rollup> of a rollup nobody sponsors: 404")
        st, _, _ = request(url, "POST", "/account/%s/permit" % funder, json.dumps({"value": "1000000", "deadline": now + 3600, "signature": "0x" + "11" * 65}).encode(),
                           {"Content-Type": "application/json"})
        check(st == 401, "a permit not signed by the funder: 401 (%d)" % st)

    # a call that would succeed: a fresh wallet's createRollupFor, signed
    k = secrets.token_bytes(32)
    owner = eth.address(k)
    chain_id, factory = int(status.get("chainId", 0)), status.get("factory", "")
    deadline = int(time.time()) + 3600
    digest = eth.typed_digest(eth.domain_separator("HPPLiteFactory", "10", chain_id, factory),
                              "CreateRollup(address owner,uint256 checkpointInterval,uint256 nonce,uint256 deadline)", owner, 100, 0, deadline)
    sig = eth.sign(k, digest)
    head = eth.word(owner) + eth.word(100) + eth.word(0) + eth.word(deadline) + eth.word(32 * 5)
    data = "0x" + VECTORS["relayCalls"]["selectors"]["createRollupFor"][2:] + (head + eth.word(65) + sig + b"\0" * 31).hex()
    st, h, body = commit({"to": factory, "data": data})
    if status.get("payment") is None:
        print("  (a free relay: the 402 is not checked)")
        return
    req = json.loads(base64.b64decode(h.get("PAYMENT-REQUIRED", ""))) if h.get("PAYMENT-REQUIRED") else {}
    accepts = req.get("accepts") or []
    check(st == 402 and accepts and req == body, "a call that would succeed: 402, the requirements in the body and PAYMENT-REQUIRED (%d)" % st)
    check(all(int(a.get("amount", 0)) >= int(status["payment"].get("minFee", 0)) for a in accepts), "every offer at least minFee")
    q = [(a.get("extra") or {}).get("quote") or {} for a in accepts]
    check(accepts and all(isinstance(x.get("gas"), int) and int(x.get("until", 0)) > time.time() and len(str(x.get("mac", ""))) == 64 for x in q),
          "every offer carries extra.quote {gas, until in the future, mac}")
    if accepts:
        forged = dict(accepts[0], amount="1")
        payload = {"x402Version": 2, "accepted": forged, "payload": {}}
        st, h, body = commit({"to": factory, "data": data}, {"PAYMENT-SIGNATURE": base64.b64encode(json.dumps(payload).encode()).decode()})
        check(st in (400, 402), "a payment naming a quote it changed is not taken (%d)" % st)


def main():
    ap = argparse.ArgumentParser(description="HPPLite operators' conformance suite (#189)")
    ap.add_argument("--store", action="append", default=[])
    ap.add_argument("--relay", action="append", default=[])
    ap.add_argument("--da", default=None, help="a rollup's DA address, for a store that reads the chain")
    ap.add_argument("--sequencer-key", default=None, help="...its sequencer's key")
    ap.add_argument("--instance", default=None, help="...the instance its lease names (for the names)")
    args = ap.parse_args()
    if not args.store and not args.relay:
        ap.error("--store and/or --relay")
    for u in args.store:
        store(u.rstrip("/"), args.da, args.sequencer_key, args.instance)
    for u in args.relay:
        relay(u.rstrip("/"))
    print("conformance: %s" % ("PASS" if not fails else "%d FAIL" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
