"""x402, the seller's side, shared by the store and the relay: quote a price in
a 402, verify and settle a payment through the facilitator ("mock" on anvil:
the token itself checks the signature, the seller sends the transaction).
Standard library only.
"""
import base64
import json
import subprocess
import sys
import time
import urllib.error
import urllib.request

from . import eth


class Payments:
    """Quote a price in a 402, verify and settle a payment through the facilitator."""

    def __init__(self, facilitator, network, asset, pay_to, asset_name, asset_version, rpc=None, settle_key=None):
        self.facilitator = facilitator.rstrip("/") if facilitator else None
        self.network = network
        self.asset = asset
        self.pay_to = pay_to
        self.extra = {"name": asset_name, "version": asset_version}
        self.rpc = rpc
        self.settle_key = settle_key
        self.settled = 0
        self.spent_nonces = set()
        self.channels = None        # channels.Channels when batch-settlement is offered (#135)

    @property
    def enabled(self):
        return self.facilitator is not None

    def requirements(self, amount: int, resource_url: str, description: str):
        return {
            "x402Version": 2,
            "error": "PAYMENT-SIGNATURE header is required",
            "resource": {"url": resource_url, "description": description, "mimeType": "application/json"},
            "accepts": [{
                "scheme": "exact", "network": self.network, "amount": str(amount), "asset": self.asset,
                "payTo": self.pay_to, "maxTimeoutSeconds": 120, "extra": self.extra,
            }] + ([self.channels.requirement(amount)] if self.channels else []),
        }

    def decode(self, header: str):
        try:
            return json.loads(base64.b64decode(header))
        except (ValueError, TypeError):
            return None

    def _facilitator_post(self, path, body):
        # a named User-Agent: HPP's facilitator sits behind a WAF that answers 403 to Python's default
        req = urllib.request.Request(self.facilitator + path, data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json", "Accept": "application/json",
                                              "User-Agent": "hpplite-pin-service/1 (+https://github.com/hpp-io/hpplite)"}, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return json.loads(r.read())
        except urllib.error.HTTPError as e:
            text = e.read().decode("utf-8", "replace")[:300]
            sys.stderr.write("pin_service: facilitator %s answered %d: %s\n" % (path, e.code, text))
            return {"isValid": False, "success": False, "invalidReason": "facilitator_error", "errorReason": "facilitator_error",
                    "invalidMessage": "%d %s" % (e.code, text), "errorMessage": "%d %s" % (e.code, text), "transaction": "", "network": self.network}
        except (OSError, ValueError) as e:
            sys.stderr.write("pin_service: facilitator %s unreachable: %s\n" % (path, e))
            return {"isValid": False, "success": False, "invalidReason": "facilitator_unreachable", "errorReason": "facilitator_unreachable",
                    "invalidMessage": str(e), "errorMessage": str(e), "transaction": "", "network": self.network}

    # --- the mock facilitator: anvil, the token itself checks the signature ---
    def _mock_calldata(self, auth, sig_hex):
        sig = bytes.fromhex(sig_hex[2:])
        if len(sig) != 65:
            raise ValueError("bad signature length")
        words = [eth.word(auth["from"]), eth.word(auth["to"]), eth.word(int(auth["value"])),
                 eth.word(int(auth["validAfter"])), eth.word(int(auth["validBefore"])),
                 bytes.fromhex(auth["nonce"][2:]), eth.word(sig[64]), sig[:32], sig[32:64]]
        selector = eth.keccak256(b"transferWithAuthorization(address,address,uint256,uint256,uint256,bytes32,uint8,bytes32,bytes32)")[:4]
        return "0x" + (selector + b"".join(words)).hex()

    def _eth_call(self, data):
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "eth_call",
                           "params": [{"to": self.asset, "data": data}, "latest"]}).encode()
        req = urllib.request.Request(self.rpc, data=body, headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read())

    def _mock_verify(self, payload, requirements):
        auth = payload["payload"]["authorization"]
        acc = payload.get("accepted", {})
        if acc.get("scheme") != "exact" or acc.get("network") != self.network or acc.get("asset", "").lower() != self.asset.lower():
            return {"isValid": False, "invalidReason": "unsupported_scheme"}
        if auth["to"].lower() != self.pay_to.lower() or int(auth["value"]) < int(requirements["amount"]):
            return {"isValid": False, "invalidReason": "invalid_exact_evm_payload_recipient_mismatch"}
        now = int(time.time())
        if not (int(auth["validAfter"]) < now < int(auth["validBefore"])):
            return {"isValid": False, "invalidReason": "invalid_exact_evm_payload_authorization_valid_before"}
        if auth["nonce"].lower() in self.spent_nonces:
            return {"isValid": False, "invalidReason": "invalid_exact_evm_payload_authorization_nonce"}
        try:
            r = self._eth_call(self._mock_calldata(auth, payload["payload"]["signature"]))
        except (OSError, ValueError, KeyError) as e:
            return {"isValid": False, "invalidReason": "rpc_error", "invalidMessage": str(e)}
        if "error" in r:
            msg = r["error"].get("message", "")
            data = r["error"].get("data")
            if isinstance(data, str) and data.startswith("0x08c379a0") and len(data) >= 10 + 128:
                try:                                       # Error(string): the token's require() message
                    n = int(data[10 + 64:10 + 128], 16)
                    msg = bytes.fromhex(data[10 + 128:10 + 128 + 2 * n]).decode("utf-8", "replace")
                except ValueError:
                    pass
            return {"isValid": False, "invalidReason": "invalid_exact_evm_payload_signature", "invalidMessage": msg}
        return {"isValid": True, "payer": auth["from"]}

    def _mock_settle(self, payload, requirements):
        auth = payload["payload"]["authorization"]
        data = self._mock_calldata(auth, payload["payload"]["signature"])
        cmd = ["cast", "send", "--rpc-url", self.rpc, "--private-key", self.settle_key, "--json", self.asset, data]
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=120, check=True).stdout
            tx = json.loads(out).get("transactionHash", "")
        except (subprocess.SubprocessError, ValueError, OSError) as e:
            return {"success": False, "errorReason": "settle_failed", "errorMessage": str(e), "transaction": "", "network": self.network}
        self.spent_nonces.add(auth["nonce"].lower())
        return {"success": True, "payer": auth["from"], "transaction": tx, "network": self.network}

    def verify(self, payload, requirements):
        if self.facilitator == "mock":
            return self._mock_verify(payload, requirements)
        return self._facilitator_post("/verify", {"x402Version": 2, "paymentPayload": payload, "paymentRequirements": requirements})

    def settle(self, payload, requirements):
        if self.facilitator == "mock":
            r = self._mock_settle(payload, requirements)
        else:
            r = self._facilitator_post("/settle", {"x402Version": 2, "paymentPayload": payload, "paymentRequirements": requirements})
        if r.get("success"):
            self.settled += 1
        return r
