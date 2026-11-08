"""x402, the seller's side, shared by the store and the relay: quote a price in
a 402, verify and settle a payment through the facilitator (on anvil,
operators/conformance/facilitator_mock.py: the seller never sends a
transaction itself, #183). Standard library only.
"""
import base64
import json
import sys
import urllib.error
import urllib.request


class Payments:
    """Quote a price in a 402, verify and settle a payment through the facilitator."""

    def __init__(self, facilitator, network, asset, pay_to, asset_name, asset_version, rpc=None):
        self.facilitator = facilitator.rstrip("/") if facilitator else None
        self.network = network
        self.asset = asset
        self.pay_to = pay_to
        self.extra = {"name": asset_name, "version": asset_version}
        self.rpc = rpc
        self.settled = 0

    @property
    def enabled(self):
        return self.facilitator is not None

    def requirements(self, amount: int, resource_url: str, description: str, extra=None):
        """The 402. extra: fields every offer carries besides the token's (the relay's signed quote, #184)"""
        out = {
            "x402Version": 2,
            "error": "PAYMENT-SIGNATURE header is required",
            "resource": {"url": resource_url, "description": description, "mimeType": "application/json"},
            "accepts": [{
                "scheme": "exact", "network": self.network, "amount": str(amount), "asset": self.asset,
                "payTo": self.pay_to, "maxTimeoutSeconds": 120, "extra": self.extra,
            }],
        }
        for a in out["accepts"]:
            a["extra"] = dict(a["extra"], **(extra or {}))
        return out

    def decode(self, header: str):
        try:
            return json.loads(base64.b64decode(header))
        except (ValueError, TypeError):
            return None

    def _facilitator_post(self, path, body):
        # a named User-Agent: HPP's facilitator sits behind a WAF that answers 403 to Python's default
        req = urllib.request.Request(self.facilitator + path, data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json", "Accept": "application/json",
                                              "User-Agent": "hpplite-operators/1 (+https://github.com/hpp-io/hpplite)"}, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return json.loads(r.read())
        except urllib.error.HTTPError as e:
            text = e.read().decode("utf-8", "replace")[:300]
            sys.stderr.write("x402seller: facilitator %s answered %d: %s\n" % (path, e.code, text))
            return {"isValid": False, "success": False, "invalidReason": "facilitator_error", "errorReason": "facilitator_error",
                    "invalidMessage": "%d %s" % (e.code, text), "errorMessage": "%d %s" % (e.code, text), "transaction": "", "network": self.network}
        except (OSError, ValueError) as e:
            sys.stderr.write("x402seller: facilitator %s unreachable: %s\n" % (path, e))
            return {"isValid": False, "success": False, "invalidReason": "facilitator_unreachable", "errorReason": "facilitator_unreachable",
                    "invalidMessage": str(e), "errorMessage": str(e), "transaction": "", "network": self.network}

    def verify(self, payload, requirements):
        return self._facilitator_post("/verify", {"x402Version": 2, "paymentPayload": payload, "paymentRequirements": requirements})

    def settle(self, payload, requirements):
        r = self._facilitator_post("/settle", {"x402Version": 2, "paymentPayload": payload, "paymentRequirements": requirements})
        if r.get("success"):
            self.settled += 1
        return r
