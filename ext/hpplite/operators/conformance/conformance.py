#!/usr/bin/env python3
"""The conformance suite (#189): what doc/store-protocol.md and
doc/relay-protocol.md say, checked call by call against any URL - the
reference in ctest, the canonical Workers under wrangler dev and on staging.

    conformance.py --store http://host:port [--buyer-key 0x...]
    conformance.py --relay http://host:port

A free store is checked through every call. A paid one has its 402 checked
against its /status (the price, the offers); with --buyer-key (a wallet
holding the asset) the content calls are paid with `exact` and checked too.
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
from buyer import Buyer, receipt_ok  # noqa: E402

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


def put_price(n, pay):
    months = pay["retentionSeconds"] / 2592000
    storage = pay["pricePerMiBMonth"] * n / 1048576 * months
    import math
    return pay["pricePerPin"] + pay.get("pricePerByte", 0) * n + math.ceil(storage - 1e-9)


# ---------------- the store ----------------

def store(url, buyer_key=None):
    print("store %s" % url)
    st, _, data = request(url, "GET", "/status")
    status = as_json(data) or {}
    check(st == 200 and status.get("protocol") == "hpplite-store/1", "/status: protocol hpplite-store/1 (%r)" % status.get("protocol"))
    dom = (status.get("receipt") or {}).get("domain") or {}
    check(isinstance(status.get("signer"), str) and len(status["signer"]) == 42, "/status: the receipt signer")
    check(dom.get("name") == "HPPLitePinService" and dom.get("version") == "1" and dom.get("chainId") and len(dom.get("verifyingContract", "")) == 42,
          "/status: the receipt's domain (HPPLitePinService, 1, chainId, payTo)")
    pay = status.get("payment")
    if pay is not None:
        check(all(k in pay for k in ("network", "asset", "payTo", "pricePerPin", "pricePerMiBMonth", "retentionSeconds")),
              "/status: payment names the network, asset, payTo and prices")

    # the 402, priced as /status says
    data = b"conformance " + secrets.token_bytes(64)
    cid = cid_of(data)
    paid = pay is not None
    if paid:
        st, h, body = request(url, "POST", "/put", data)
        req = json.loads(base64.b64decode(h.get("PAYMENT-REQUIRED", ""))) if h.get("PAYMENT-REQUIRED") else {}
        check(st == 402 and req.get("x402Version") == 2 and req == as_json(body), "a put unpaid: 402, the requirements in the body and PAYMENT-REQUIRED")
        accepts = req.get("accepts") or []
        exact = next((a for a in accepts if a.get("scheme") == "exact"), None)
        check(exact is not None and int(exact["amount"]) == put_price(len(data), pay), "the exact offer asks the price /status implies (%s)" % (exact or {}).get("amount"))
        check(exact is not None and exact["asset"].lower() == pay["asset"].lower() and exact["payTo"].lower() == pay["payTo"].lower()
              and exact["network"] == pay["network"] and "name" in exact.get("extra", {}) and "version" in exact.get("extra", {}),
              "the exact offer: asset, payTo, network as /status; the token's domain in extra")
        if "batch-settlement" in (pay.get("schemes") or []):
            bs = next((a for a in accepts if a.get("scheme") == "batch-settlement"), None)
            check(bs is not None and all(k in bs.get("extra", {}) for k in ("receiverAuthorizer", "withdrawDelay", "minDeposit")),
                  "the batch-settlement offer: receiverAuthorizer, withdrawDelay, minDeposit")
        if not buyer_key:
            print("  (paid, no --buyer-key: the content calls are not checked)")
            return
    b = Buyer(url, bytes.fromhex(buyer_key[2:]) if buyer_key else secrets.token_bytes(32), cap=10 ** 9, valid_for=600)

    # content
    st, out, _ = b.call("POST", "/put", data)
    out = out if isinstance(out, dict) else {}
    check(st == 200 and out.get("cid") == cid and out.get("bytes") == len(data) and out.get("held") is False,
          "a put: 200, the CIDv1 raw sha-256 of the bytes, held false")
    check(st == 200 and receipt_ok(out.get("receipt") or {}, status, data), "the receipt recovers to /status's signer over Receipt(keccak, cid, expires)")
    retention = (pay or {}).get("retentionSeconds")
    if retention:
        check(abs(int(out.get("expires", 0)) - (time.time() + retention)) < 120, "expires: now + retentionSeconds")
    st2, h2, body2 = request(url, "POST", "/put", data)
    again = as_json(body2) or {}
    check(st2 == 200 and again.get("held") is True and again.get("receipt") == out.get("receipt"), "the same bytes again: 200, held, the same receipt, nothing asked")
    st, _, got = request(url, "GET", "/get/" + cid)
    check(st == 200 and got == data, "get by CID: the bytes")
    st, _, _ = request(url, "GET", "/get/" + cid_of(secrets.token_bytes(8)))
    check(st == 404, "get of a CID never put: 404")
    st, _, _ = request(url, "POST", "/put", b"")
    check(st == 400, "an empty put: 400")
    st, out, _ = b.call("POST", "/pin/" + cid)
    out = out if isinstance(out, dict) else {}
    check(st == 200 and int(out.get("expires", 0)) > int(again.get("expires", 0)) and receipt_ok(out.get("receipt") or {}, status, data),
          "pin: retention extended, a new receipt")

    # names
    chain_id = int(dom.get("chainId", 0))
    da = secrets.token_bytes(20).hex()
    a, other = secrets.token_bytes(32), secrets.token_bytes(32)
    instance = "0x" + eth.keccak256(b"conformance").hex()

    def named(k, height, payload):
        name = "%s-%d" % (da, height)
        dom = eth.domain_separator("HPPLiteDA", "3", chain_id, "0x" + da)
        digest = eth.typed_digest(dom, "Batch(bytes32 instanceId,uint256 height,bytes32 batchHash)",
                                  bytes.fromhex(instance[2:]), height, eth.keccak256(payload))
        return name, payload + bytes.fromhex(instance[2:]) + eth.sign(k, digest)

    name, body = named(a, 1, b"named batch")
    st, _, out = request(url, "POST", "/name/" + name, body)
    out = as_json(out) or {}
    check(st == 200 and (out.get("signer") or "").lower() == eth.address(a), "a name put by its sequencer: 200, the signer recovered")
    st, h, got = request(url, "GET", "/name/" + name)
    check(st == 200 and got == body and h.get("X-HPPLITE-SIGNER", "").lower() == eth.address(a), "the name reads back as put, X-HPPLite-Signer the signer")
    st, _, _ = request(url, "POST", "/name/" + name, body)
    check(st == 200, "the same name and body again: 200")
    name2, body2 = named(other, 2, b"another key")
    st, _, _ = request(url, "POST", "/name/" + name2, body2)
    check(st == 403, "another signer for the same rollup: 403")
    st, _, _ = request(url, "GET", "/name/%s-999" % da)
    check(st == 404, "a name never put: 404")
    st, _, _ = request(url, "POST", "/name/not-a-name", body)
    check(st == 400, "a malformed name: 400")


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
    ap.add_argument("--buyer-key", default=None, help="a wallet holding the store's asset: pay the content calls with exact")
    args = ap.parse_args()
    if not args.store and not args.relay:
        ap.error("--store and/or --relay")
    for u in args.store:
        store(u.rstrip("/"), args.buyer_key)
    for u in args.relay:
        relay(u.rstrip("/"))
    print("conformance: %s" % ("PASS" if not fails else "%d FAIL" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
