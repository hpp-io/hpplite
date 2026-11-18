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


PERMIT2 = "0x000000000022D473030F116dDEE9F6B43aC78BA3"
UPTO_PROXY = "0x4020A4f3b7b90ccA423B9fabCc0CE57C6C240002"


def permit2_upto_digest(chain_id, auth):
    """the EIP-712 digest of an upto Permit2 PermitWitnessTransferFrom (#358): Permit2's domain has no version"""
    k = eth.keccak256
    domain = k(k(b"EIP712Domain(string name,uint256 chainId,address verifyingContract)") + k(b"Permit2")
               + eth.word(chain_id) + eth.word(PERMIT2))
    tp = k(k(b"TokenPermissions(address token,uint256 amount)") + eth.word(auth["permitted"]["token"]) + eth.word(int(auth["permitted"]["amount"])))
    w = auth["witness"]
    wh = k(k(b"Witness(address to,address facilitator,uint256 validAfter)") + eth.word(w["to"]) + eth.word(w["facilitator"]) + eth.word(int(w["validAfter"])))
    typ = (b"PermitWitnessTransferFrom(TokenPermissions permitted,address spender,uint256 nonce,uint256 deadline,Witness witness)"
           b"TokenPermissions(address token,uint256 amount)Witness(address to,address facilitator,uint256 validAfter)")
    struct = k(k(typ) + tp + eth.word(auth["spender"]) + eth.word(int(auth["nonce"])) + eth.word(int(auth["deadline"])) + wh)
    return k(b"\x19\x01" + domain + struct)


class Buyer:
    def __init__(self, service, key: bytes, cap: int, valid_for: int = 120, asset=None, scheme="exact", rpc=None):
        u = urllib.parse.urlparse(service)
        self.host, self.port = u.hostname, u.port or 80
        self.key = key
        self.address = eth.address(key)
        self.cap = cap
        self.valid_for = valid_for   # the authorization's window; a test chain whose clock ran ahead needs a wide one
        self.paid = []          # settlements
        self.asset = asset      # pay in this token's offer when the 402 has several (#357); else the first
        self.scheme = scheme    # exact, or upto (#358: a Permit2 transfer the facilitator settles through x402's proxy)
        self.rpc = rpc          # upto: the chain, to read the allowance to Permit2 and the token's permit nonce

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
        acc = next((a for a in req.get("accepts", []) if a.get("scheme") == self.scheme
                    and (not self.asset or a.get("asset", "").lower() == self.asset.lower())), None)
        if not acc:
            raise ValueError("no %s-scheme offer in the 402" % self.scheme)
        amount = int(acc["amount"])
        if amount > self.cap:
            raise OverCap("the service asks %d, the cap is %d" % (amount, self.cap))
        if self.scheme == "upto":
            return base64.b64encode(json.dumps(self._upto(req, acc, amount)).encode()).decode()
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

    def _view(self, to, sig, *words):
        from x402seller.chain import Chain, selector
        out = Chain(self.rpc).call("eth_call", {"to": to, "data": "0x" + selector(sig) + "".join(eth.word(w).hex() for w in words)}, "latest")
        return int(out or "0x0", 16)

    def _upto(self, req, acc, amount):
        """the upto payload (#358), as @x402/evm's UptoEvmScheme makes it: a Permit2 PermitWitnessTransferFrom of the
        offer's amount to x402's upto proxy, the witness naming payTo and the facilitator; and, when the 402 declares
        the EIP-2612 extension and the allowance to Permit2 is short, the token's permit to Permit2 for the facilitator
        to send"""
        chain_id = int(acc["network"].split(":")[1])
        now = int(time.time())
        auth = {"from": self.address, "permitted": {"token": acc["asset"], "amount": str(amount)}, "spender": UPTO_PROXY,
                "nonce": str(int.from_bytes(eth.random_nonce(), "big")), "deadline": str(now + int(acc.get("maxTimeoutSeconds", 120))),
                "witness": {"to": acc["payTo"], "facilitator": acc["extra"]["facilitatorAddress"], "validAfter": str(now - 600)}}
        payload = {"x402Version": 2, "resource": req.get("resource"), "accepted": acc,
                   "payload": {"signature": "0x" + eth.sign(self.key, permit2_upto_digest(chain_id, auth)).hex(), "permit2Authorization": auth},
                   "extensions": {}}
        if (req.get("extensions") or {}).get("eip2612GasSponsoring") and self.rpc \
                and self._view(acc["asset"], "allowance(address,address)", self.address, PERMIT2) < amount:
            nonce = self._view(acc["asset"], "nonces(address)", self.address)
            domain = eth.domain_separator(acc["extra"]["name"], acc["extra"]["version"], chain_id, acc["asset"])
            digest = eth.typed_digest(domain, "Permit(address owner,address spender,uint256 value,uint256 nonce,uint256 deadline)",
                                      self.address, PERMIT2, amount, nonce, int(auth["deadline"]))
            payload["extensions"]["eip2612GasSponsoring"] = {"info": {
                "from": self.address, "asset": acc["asset"], "spender": PERMIT2, "amount": str(amount), "nonce": str(nonce),
                "deadline": auth["deadline"], "signature": "0x" + eth.sign(self.key, digest).hex(), "version": "1"}}
        return payload

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
