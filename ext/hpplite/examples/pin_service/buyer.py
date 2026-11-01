#!/usr/bin/env python3 -I
"""The buyer's side of the pin service (#125): what HPPLite's IPFS adapter
(#11) does in C, here in Python for the test and for operators to try.

    buyer.py --service http://127.0.0.1:8420 --key <hex> --cap 50000 put <file>
    buyer.py ... pin <cid>  |  get <cid> [out]  |  status
    buyer.py ... --selftest                  # drives every path against a running service

A call answered with 402 is paid in the x402 `exact` scheme: the 402's
PAYMENT-REQUIRED names amount, asset, payTo and the token's EIP-712 domain;
the buyer signs an EIP-3009 TransferWithAuthorization for exactly that
amount (a random nonce, valid for the next two minutes), sends it back in
PAYMENT-SIGNATURE, and reads the settlement in PAYMENT-RESPONSE. Above
--cap the buyer refuses before signing: nothing is spent. Every receipt is
checked against the signer /status names before it is believed.
"""
import argparse
import base64
import http.client
import json
import os
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eth  # noqa: E402

TRANSFER_TYPE = "TransferWithAuthorization(address from,address to,uint256 value,uint256 validAfter,uint256 validBefore,bytes32 nonce)"
RECEIPT_TYPE = "Receipt(bytes32 hash,string cid,uint256 expires)"


class OverCap(Exception):
    pass


class Buyer:
    def __init__(self, service, key: bytes, cap: int, valid_for: int = 120):
        u = urllib.parse.urlparse(service)
        self.host, self.port = u.hostname, u.port or 80
        self.key = key
        self.address = eth.address(key)
        self.cap = cap
        self.valid_for = valid_for   # the authorization's window; a test chain whose clock ran ahead needs a wide one
        self.paid = []          # settlements

    def _request(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection(self.host, self.port, timeout=120)
        try:
            h = {"Content-Type": "application/octet-stream"} if body is not None else {}
            h.update(headers or {})
            conn.request(method, path, body=body, headers=h)
            r = conn.getresponse()
            return r.status, dict(r.getheaders()), r.read()
        finally:
            conn.close()

    def _payment(self, requirements_header: str) -> str:
        req = json.loads(base64.b64decode(requirements_header))
        acc = next((a for a in req.get("accepts", []) if a.get("scheme") == "exact"), None)
        if not acc:
            raise ValueError("no exact-scheme offer in the 402")
        amount = int(acc["amount"])
        if amount > self.cap:
            raise OverCap("the service asks %d, the cap is %d" % (amount, self.cap))
        chain_id = int(acc["network"].split(":")[1])
        domain = eth.domain_separator(acc["extra"]["name"], acc["extra"]["version"], chain_id, acc["asset"])
        now = int(time.time())
        auth = {"from": self.address, "to": acc["payTo"], "value": str(amount),
                "validAfter": str(now - 60), "validBefore": str(now + self.valid_for), "nonce": "0x" + eth.random_nonce().hex()}
        digest = eth.typed_digest(domain, TRANSFER_TYPE, auth["from"], auth["to"], amount,
                                  int(auth["validAfter"]), int(auth["validBefore"]), bytes.fromhex(auth["nonce"][2:]))
        sig = eth.sign(self.key, digest)
        payload = {"x402Version": 2, "resource": req.get("resource"), "accepted": acc,
                   "payload": {"signature": "0x" + sig.hex(), "authorization": auth}, "extensions": {}}
        return base64.b64encode(json.dumps(payload).encode()).decode()

    def call(self, method, path, body=None):
        """The request; on 402, paid and repeated. (status, json-or-bytes, settlement-or-None)."""
        status, headers, data = self._request(method, path, body)
        if status != 402:
            return status, self._decode(headers, data), None
        hdr = {k.upper(): v for k, v in headers.items()}.get("PAYMENT-REQUIRED")
        if not hdr:
            return status, self._decode(headers, data), None
        payment = self._payment(hdr)
        status, headers, data = self._request(method, path, body, {"PAYMENT-SIGNATURE": payment})
        settlement = None
        resp = {k.upper(): v for k, v in headers.items()}.get("PAYMENT-RESPONSE")
        if resp:
            settlement = json.loads(base64.b64decode(resp))
            self.paid.append(settlement)
        return status, self._decode(headers, data), settlement

    @staticmethod
    def _decode(headers, data):
        ct = {k.lower(): v for k, v in headers.items()}.get("content-type", "")
        if ct.startswith("application/json"):
            try:
                return json.loads(data)
            except ValueError:
                return data
        return data

    def status(self):
        return self.call("GET", "/status")[1]

    def put(self, data: bytes):
        return self.call("POST", "/put", data)

    def pin(self, cid):
        return self.call("POST", "/pin/" + cid)

    def get(self, cid):
        return self.call("GET", "/get/" + cid)


def receipt_ok(receipt: dict, status: dict, data: bytes = None) -> bool:
    """The receipt's signature is the status's signer's, over the fields it states (and the bytes, if given)."""
    try:
        dom = status["receipt"]["domain"]
        domain = eth.domain_separator(dom["name"], dom["version"], int(dom["chainId"]), dom["verifyingContract"])
        h = bytes.fromhex(receipt["hash"][2:])
        if data is not None and eth.keccak256(data) != h:
            return False
        digest = eth.typed_digest(domain, RECEIPT_TYPE, h, eth.keccak256(receipt["cid"].encode()), int(receipt["expires"]))
        signer = eth.recover(digest, bytes.fromhex(receipt["signature"][2:]))
        return signer.lower() == status["signer"].lower() == receipt["signer"].lower()
    except (KeyError, ValueError, TypeError):
        return False


def selftest(b: Buyer, expect_paid: bool) -> int:
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)
        print("  %s %s" % ("ok  " if cond else "FAIL", what))

    st = b.status()
    check(isinstance(st, dict) and st.get("signer"), "status names the signer")
    data = ("batch " + os.urandom(8).hex() + " " * 500).encode()
    try:
        status, r, settled = b.put(data)
    except OverCap as e:
        print("  put refused: %s" % e)
        status, r, settled = 0, {}, None
    if status != 200:
        print("  put answered %s: %s" % (status, json.dumps(r)[:600] if isinstance(r, dict) else r[:600]))
    check(status == 200 and isinstance(r, dict) and r.get("cid", "").startswith("bafkrei"), "put: 200 with a CIDv1 raw sha-256")
    if status != 200:
        print("buyer selftest: FAILED (the first put failed; nothing else can be checked)")
        return 1
    check((settled is not None) == expect_paid, "put: %s" % ("paid through the facilitator" if expect_paid else "free"))
    cid = r.get("cid") if isinstance(r, dict) else None
    check(cid and receipt_ok(r.get("receipt", {}), st, data), "put: the receipt is the signer's over hash, cid, expires")
    status2, r2, settled2 = b.put(data)
    check(status2 == 200 and r2.get("held") is True and settled2 is None, "put again: held, no 402, nothing paid (idempotent by CID)")
    status3, back, _ = b.get(cid)
    check(status3 == 200 and back == data, "get: the bytes back")
    status4, r4, settled4 = b.pin(cid)
    check(status4 == 200 and r4.get("expires", 0) > r.get("expires", 0) and receipt_ok(r4.get("receipt", {}), st, data),
          "pin: retention extended, a new receipt")
    check((settled4 is not None) == expect_paid, "pin: %s" % ("paid" if expect_paid else "free"))
    bad = dict(r["receipt"])
    bad["expires"] = int(bad["expires"]) + 1
    check(not receipt_ok(bad, st, data), "a receipt with a changed expiry does not verify")
    status5, r5, _ = b.get("bafkreiaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
    check(status5 == 404, "get of an unknown CID: 404")
    if expect_paid:
        cap, b.cap = b.cap, 1
        try:
            b.put(("over the cap " + os.urandom(8).hex()).encode())
            check(False, "over the cap: refused before signing")
        except OverCap:
            check(True, "over the cap: refused before signing, nothing spent")
        b.cap = cap
        st2 = b.status()
        check(st2.get("settled", 0) >= 2, "the service counts the settlements (%s)" % st2.get("settled"))
    print("buyer selftest: %s (%d paid calls)" % ("PASS" if not failures else "FAILED", len(b.paid)))
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--service", default="http://127.0.0.1:8420")
    ap.add_argument("--key", required=True, help="hex private key of the paying wallet")
    ap.add_argument("--cap", type=int, default=100000, help="refuse any single payment above this (atomic units)")
    ap.add_argument("--valid-for", type=int, default=120, help="seconds an authorization stays valid (wide on a test chain whose clock ran ahead)")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--expect-paid", action="store_true", help="selftest: the service charges (a facilitator is configured)")
    ap.add_argument("cmd", nargs="?", choices=["put", "pin", "get", "status"])
    ap.add_argument("arg", nargs="?")
    ap.add_argument("out", nargs="?")
    a = ap.parse_args()
    b = Buyer(a.service, bytes.fromhex(a.key.replace("0x", "")), a.cap, a.valid_for)
    if a.selftest:
        return selftest(b, a.expect_paid)
    if a.cmd == "status":
        print(json.dumps(b.status(), indent=2))
    elif a.cmd == "put":
        with open(a.arg, "rb") as f:
            data = f.read()
        status, r, settled = b.put(data)
        print(json.dumps({"status": status, "result": r if isinstance(r, dict) else None, "settlement": settled}, indent=2))
        return 0 if status == 200 else 1
    elif a.cmd == "pin":
        status, r, settled = b.pin(a.arg)
        print(json.dumps({"status": status, "result": r if isinstance(r, dict) else None, "settlement": settled}, indent=2))
        return 0 if status == 200 else 1
    elif a.cmd == "get":
        status, data, _ = b.get(a.arg)
        if status != 200:
            print(data)
            return 1
        if a.out:
            with open(a.out, "wb") as f:
                f.write(data)
        else:
            sys.stdout.buffer.write(data)
    else:
        ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
