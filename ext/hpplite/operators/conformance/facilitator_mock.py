#!/usr/bin/env python3
"""An x402 facilitator for anvil (#183): /verify, /settle and /supported for
`exact` (EIP-3009 transferWithAuthorization), sending the transactions with
its own key. The services only ever talk to a facilitator URL; this one stands
in for HPP's on a test chain, and behaves as HPP's was found to:

- /settle answers a body it has seen settle with its earlier result and
  sends nothing (--no-settle-cache turns that off).

(The `batch-settlement` half went with the channels in v0.8, hpplite#251.)

    facilitator_mock.py --port 8440 --rpc http://127.0.0.1:8545 --key <hex>

Test infrastructure: it sends with foundry's `cast`, which a test chain has.
"""
import argparse
import http.server
import json
import os
import socketserver
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from x402seller import eth  # noqa: E402
from x402seller.chain import Chain, RpcError, revert_reason, selector  # noqa: E402

TRANSFER_SIG = "transferWithAuthorization(address,address,uint256,uint256,uint256,bytes32,uint8,bytes32,bytes32)"


class Facilitator:
    def __init__(self, rpc, key_hex, settle_cache=True):
        self.rpc = rpc
        self.chain = Chain(rpc)
        self.key_hex = key_hex if key_hex.startswith("0x") else "0x" + key_hex
        self.address = eth.address(bytes.fromhex(self.key_hex[2:]))
        self.chain_id = int(self.chain.call("eth_chainId"), 16)
        self.network = "eip155:%d" % self.chain_id
        self.settle_cache = settle_cache
        self.cache = {}
        self.spent = set()                     # exact: authorization nonces settled here
        self.lock = threading.Lock()           # one transaction at a time on this key
        self.sent = 0

    # --- the chain ---
    def cast(self, *args, send=False):
        cmd = ["cast", "send" if send else "call", "--rpc-url", self.rpc]
        cmd += ["--private-key", self.key_hex, "--json"] if send else ["--from", self.address]
        if send:
            with self.lock:
                r = subprocess.run(cmd + list(args), capture_output=True, text=True, timeout=120)
        else:
            r = subprocess.run(cmd + list(args), capture_output=True, text=True, timeout=120)
        if r.returncode != 0:
            text = (r.stderr or r.stdout).strip()
            raise RuntimeError(text.splitlines()[-1][:300] if text else "cast failed")
        if send:
            self.sent += 1
            return json.loads(r.stdout).get("transactionHash", "")
        return r.stdout.strip()

    # --- exact ---
    def _transfer_data(self, auth, sig_hex):
        sig = bytes.fromhex(sig_hex[2:])
        if len(sig) != 65:
            raise ValueError("bad signature length")
        words = [eth.word(auth["from"]), eth.word(auth["to"]), eth.word(int(auth["value"])),
                 eth.word(int(auth["validAfter"])), eth.word(int(auth["validBefore"])),
                 bytes.fromhex(auth["nonce"][2:]), eth.word(sig[64]), sig[:32], sig[32:64]]
        return "0x" + selector(TRANSFER_SIG) + b"".join(words).hex()

    def verify_exact(self, payload, req):
        auth = payload["payload"]["authorization"]
        acc = payload.get("accepted", {})
        if acc.get("scheme") != "exact" or acc.get("network") != self.network or acc.get("asset", "").lower() != req["asset"].lower():
            return {"isValid": False, "invalidReason": "unsupported_scheme"}
        if auth["to"].lower() != req["payTo"].lower() or int(auth["value"]) < int(req["amount"]):
            return {"isValid": False, "invalidReason": "invalid_exact_evm_payload_recipient_mismatch"}
        now = int(time.time())
        if not (int(auth["validAfter"]) < now < int(auth["validBefore"])):
            return {"isValid": False, "invalidReason": "invalid_exact_evm_payload_authorization_valid_before"}
        if auth["nonce"].lower() in self.spent:
            return {"isValid": False, "invalidReason": "invalid_exact_evm_payload_authorization_nonce"}
        try:
            self.chain.call("eth_call", {"to": req["asset"], "data": self._transfer_data(auth, payload["payload"]["signature"])}, "latest")
        except RpcError as e:
            return {"isValid": False, "invalidReason": "invalid_exact_evm_payload_signature", "invalidMessage": revert_reason(e)}
        except (OSError, ValueError, KeyError) as e:
            return {"isValid": False, "invalidReason": "rpc_error", "invalidMessage": str(e)}
        return {"isValid": True, "payer": auth["from"]}

    def settle_exact(self, payload, req):
        auth = payload["payload"]["authorization"]
        try:
            tx = self.cast(req["asset"], self._transfer_data(auth, payload["payload"]["signature"]), send=True)
        except (RuntimeError, ValueError, OSError, subprocess.SubprocessError) as e:
            return {"success": False, "errorReason": "settle_failed", "errorMessage": str(e), "transaction": "", "network": self.network}
        self.spent.add(auth["nonce"].lower())
        return {"success": True, "payer": auth["from"], "transaction": tx, "network": self.network}

    # --- the endpoints ---
    def verify(self, body):
        payload, req = body["paymentPayload"], body["paymentRequirements"]
        return self.verify_exact(payload, req)

    def settle(self, body):
        key = json.dumps(body, sort_keys=True)
        if self.settle_cache and key in self.cache:
            return self.cache[key]             # as HPP's: the same body, the earlier answer, nothing sent
        req = body["paymentRequirements"]
        out = self.settle_exact(body["paymentPayload"], req)
        if self.settle_cache and out.get("success"):
            self.cache[key] = out
        return out

    def supported(self):
        return {"kinds": [{"x402Version": 2, "scheme": "exact", "network": self.network}],
                "signers": {self.network: [self.address]}}


class Handler(http.server.BaseHTTPRequestHandler):
    f = None  # type: Facilitator

    def log_message(self, fmt, *args):
        pass

    def reply(self, status, obj):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/supported":
            return self.reply(200, self.f.supported())
        if self.path == "/status":
            return self.reply(200, {"address": self.f.address, "chainId": self.f.chain_id, "sent": self.f.sent,
                                    "settleCache": self.f.settle_cache})
        return self.reply(404, {"error": "not found"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(n) if n else b"{}")
        except ValueError:
            return self.reply(400, {"error": "not JSON"})
        if self.path == "/_mock":
            if "settleCache" in body:
                self.f.settle_cache = bool(body["settleCache"])
            return self.reply(200, {"settleCache": self.f.settle_cache})
        if self.path not in ("/verify", "/settle"):
            return self.reply(404, {"error": "not found"})
        try:
            return self.reply(200, self.f.verify(body) if self.path == "/verify" else self.f.settle(body))
        except (KeyError, TypeError, ValueError) as e:
            return self.reply(400, {"error": "malformed request: %s" % e})


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser(description="x402 facilitator for anvil (#183)")
    ap.add_argument("--port", type=int, default=8440)
    ap.add_argument("--host", default="127.0.0.1", help="the address to listen on (default 127.0.0.1; 0.0.0.0 for every interface)")
    ap.add_argument("--rpc", required=True)
    ap.add_argument("--key", required=True, help="hex key that sends the settlements (a funded anvil account)")
    ap.add_argument("--settle-cache", action=argparse.BooleanOptionalAction, default=True,
                    help="answer a /settle body seen before with the earlier result, as HPP's does")
    args = ap.parse_args()
    Handler.f = Facilitator(args.rpc, args.key, args.settle_cache)
    srv = Server((args.host, args.port), Handler)
    print("facilitator_mock: listening on %s:%d, address %s, chain %d" % (args.host, args.port, Handler.f.address, Handler.f.chain_id), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
