#!/usr/bin/env python3
"""An x402 facilitator for anvil (#183): /verify, /settle and /supported for
`exact` (EIP-3009 transferWithAuthorization) and `batch-settlement` (x402's
escrow: deposit, claim, settle, refund), sending the transactions with its
own key. The services only ever talk to a facilitator URL; this one stands
in for HPP's on a test chain, and behaves as HPP's was found to (#135):

- /verify of a batch-settlement payload answers the channel's state in
  `extra` (balance, totalClaimed, refundNonce, withdrawRequestedAt);
- a deposit can be answered `settlement_queued` though it was sent
  (--queue-deposits, or POST /_mock {"queueDeposits": true});
- /settle answers a body it has seen settle with its earlier result and
  sends nothing (--no-settle-cache turns that off).

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
from x402seller import channels, eth  # noqa: E402
from x402seller.chain import Chain, RpcError, revert_reason, selector  # noqa: E402

TRANSFER_SIG = "transferWithAuthorization(address,address,uint256,uint256,uint256,bytes32,uint8,bytes32,bytes32)"
CONFIG_TUPLE = channels.CONFIG_TUPLE
ESCROW = channels.ESCROW


class Facilitator:
    def __init__(self, rpc, key_hex, queue_deposits=False, settle_cache=True):
        self.rpc = rpc
        self.chain = Chain(rpc)
        self.key_hex = key_hex if key_hex.startswith("0x") else "0x" + key_hex
        self.address = eth.address(bytes.fromhex(self.key_hex[2:]))
        self.chain_id = int(self.chain.call("eth_chainId"), 16)
        self.network = "eip155:%d" % self.chain_id
        self.queue_deposits = queue_deposits
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

    def channel_state(self, cid):
        return channels.chain_state(self.chain, cid)

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

    # --- batch-settlement ---
    def _deposit_args(self, inner):
        config = inner["channelConfig"]
        dep = inner["deposit"]
        auth = dep["authorization"]["erc3009Authorization"]
        data = subprocess.run(["cast", "abi-encode", "f(uint256,uint256,uint256,bytes)", str(auth["validAfter"]),
                               str(auth["validBefore"]), str(int(auth["salt"], 16)), auth["signature"]],
                              capture_output=True, text=True, check=True).stdout.strip()
        return (ESCROW, "deposit(%s,uint128,address,bytes)" % CONFIG_TUPLE, channels.config_arg(config),
                str(int(dep["amount"])), channels.COLLECTOR, data)

    def verify_batch(self, payload, req):
        inner = payload.get("payload") or {}
        try:
            cid = channels.channel_id(inner["channelConfig"], self.chain_id)
            bal, claimed, nonce, wr = self.channel_state(cid)
        except (KeyError, ValueError, TypeError, RpcError, OSError) as e:
            return {"isValid": False, "invalidReason": "invalid_batch_settlement_evm_channel_state_read_failed", "invalidMessage": str(e)}
        if inner.get("type") == "deposit":
            try:
                self.cast(*self._deposit_args(inner))            # the token's and the escrow's own checks, nothing sent
            except (RuntimeError, KeyError, ValueError, subprocess.SubprocessError) as e:
                return {"isValid": False, "invalidReason": "invalid_batch_settlement_evm_deposit_payload", "invalidMessage": str(e)}
        return {"isValid": True, "payer": inner["channelConfig"]["payer"],
                "extra": {"channelId": cid, "balance": str(bal), "totalClaimed": str(claimed), "refundNonce": str(nonce),
                          "withdrawRequestedAt": wr}}

    def _claim(self, claims, auth_sig):
        arg = "[" + ",".join("((%s,%d),%s,%d)" % (channels.config_arg(c["voucher"]["channel"]), int(c["voucher"]["maxClaimableAmount"]),
                                                  c["signature"], int(c["totalClaimed"])) for c in claims) + "]"
        return self.cast(ESCROW, "claimWithSignature(((%s,uint128),bytes,uint128)[],bytes)" % CONFIG_TUPLE, arg, auth_sig, send=True)

    def settle_batch(self, payload, req):
        inner = payload.get("payload") or {}
        kind = inner.get("type")
        try:
            if kind == "deposit":
                tx = self.cast(*self._deposit_args(inner), send=True)
                if self.queue_deposits:
                    return {"success": False, "errorReason": "settlement_queued",
                            "errorMessage": "processing exceeded the synchronous wait window", "transaction": tx, "network": self.network}
            elif kind == "claim":
                tx = self._claim(inner["claims"], inner["claimAuthorizerSignature"])
            elif kind == "settle":
                tx = self.cast(ESCROW, "settle(address,address)", inner["receiver"], inner["token"], send=True)
            elif kind == "refund":
                if inner.get("claims"):
                    self._claim(inner["claims"], inner["claimAuthorizerSignature"])
                tx = self.cast(ESCROW, "refundWithSignature(%s,uint128,uint256,bytes)" % CONFIG_TUPLE,
                               channels.config_arg(inner["channelConfig"]), str(int(inner["amount"])),
                               str(int(inner["refundNonce"])), inner["refundAuthorizerSignature"], send=True)
            else:
                return {"success": False, "errorReason": "invalid_batch_settlement_evm_payload_type", "transaction": "", "network": self.network}
        except (RuntimeError, KeyError, ValueError, TypeError, subprocess.SubprocessError) as e:
            return {"success": False, "errorReason": "settle_failed", "errorMessage": str(e), "transaction": "", "network": self.network}
        return {"success": True, "transaction": tx, "network": self.network}

    # --- the endpoints ---
    def verify(self, body):
        payload, req = body["paymentPayload"], body["paymentRequirements"]
        if req.get("scheme") == channels.SCHEME:
            return self.verify_batch(payload, req)
        return self.verify_exact(payload, req)

    def settle(self, body):
        key = json.dumps(body, sort_keys=True)
        if self.settle_cache and key in self.cache:
            return self.cache[key]             # as HPP's: the same body, the earlier answer, nothing sent
        req = body["paymentRequirements"]
        out = self.settle_batch(body["paymentPayload"], req) if req.get("scheme") == channels.SCHEME \
            else self.settle_exact(body["paymentPayload"], req)
        if self.settle_cache and out.get("success"):
            self.cache[key] = out
        return out

    def supported(self):
        return {"kinds": [{"x402Version": 2, "scheme": s, "network": self.network} for s in ("exact", channels.SCHEME)],
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
                                    "queueDeposits": self.f.queue_deposits, "settleCache": self.f.settle_cache})
        return self.reply(404, {"error": "not found"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(n) if n else b"{}")
        except ValueError:
            return self.reply(400, {"error": "not JSON"})
        if self.path == "/_mock":
            if "queueDeposits" in body:
                self.f.queue_deposits = bool(body["queueDeposits"])
            if "settleCache" in body:
                self.f.settle_cache = bool(body["settleCache"])
            return self.reply(200, {"queueDeposits": self.f.queue_deposits, "settleCache": self.f.settle_cache})
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
    ap.add_argument("--rpc", required=True)
    ap.add_argument("--key", required=True, help="hex key that sends the settlements (a funded anvil account)")
    ap.add_argument("--queue-deposits", action="store_true", help="answer deposits settlement_queued, as HPP's does past its window")
    ap.add_argument("--settle-cache", action=argparse.BooleanOptionalAction, default=True,
                    help="answer a /settle body seen before with the earlier result, as HPP's does")
    args = ap.parse_args()
    Handler.f = Facilitator(args.rpc, args.key, args.queue_deposits, args.settle_cache)
    srv = Server(("127.0.0.1", args.port), Handler)
    print("facilitator_mock: listening on 127.0.0.1:%d, address %s, chain %d" % (args.port, Handler.f.address, Handler.f.chain_id), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
