#!/usr/bin/env python3 -I
"""The payer's side of x402 `exact` (#125): what the node does in C, here in
Python for the selftests - a write's 402 at a hpplite-store/2 store (a top-up),
a relay's call.

A call answered with 402 is paid in the x402 `exact` scheme: the 402's
PAYMENT-REQUIRED names amount, asset, payTo and the token's EIP-712 domain;
the buyer signs an EIP-3009 TransferWithAuthorization for exactly that
amount (a random nonce, valid for the next two minutes), sends it back in
PAYMENT-SIGNATURE, and reads the settlement in PAYMENT-RESPONSE. Above
the cap the buyer refuses before signing: nothing is spent.
"""
import base64
import http.client
import json
import os
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from x402seller import eth  # noqa: E402

TRANSFER_TYPE = "TransferWithAuthorization(address from,address to,uint256 value,uint256 validAfter,uint256 validBefore,bytes32 nonce)"


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
