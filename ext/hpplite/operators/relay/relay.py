#!/usr/bin/env python3
"""A reference relay for HPPLite nodes (#170): submits a node's signed
contract calls and pays their gas, charged over x402 in USDC.e, so a node
whose wallet holds no ETH can anchor, take its lease and create its rollup.
The pin service's companion (operators/store): its own process because
it holds a funded hot key and an RPC, which the pin service does not.
Standard library only; ../x402seller has eth.py (keccak, secp256k1,
EIP-712) and Payments (the seller's side of x402).

    relay.py --port 8430 --rpc https://sepolia.hpp.io --key-file /path/to/hexkey \\
        --factory 0x... --facilitator https://facilitator-sepolia.hpp.io --pay-to 0x...

    relay.py ... --rpc http://127.0.0.1:8545 --facilitator http://127.0.0.1:8440 --asset <MockUSDC>   # anvil (#183)

Endpoints
  POST /commit   {"to": "0x...", "data": "0x..."}: one of the signed calls below, to the factory or
                 to a rollup it created. Simulated first (eth_estimateGas): a call that would revert
                 is answered 422 with the reason and costs nothing. Then 402 with the price (the gas
                 at the current fee, a margin, converted at --eth-usd, at least --min-fee; the quote
                 MACed in the offer's extra, honoured for two minutes by any process with the key,
                 #184); paid, it is
                 submitted and the answer waits for its receipt: 200 {tx, block, gasUsed}. The payment
                 is settled only once the transaction succeeded: a call that reverts on chain after
                 all (another one got in first) is answered 409 and not charged.
  GET  /status   the relay's address and balance, the chain, the factory, the prices.

The relay is trusted for liveness only: the contracts check every signature,
so it cannot forge or alter a call, only delay or drop one. A node that sees
its anchor not land falls back to another relay, or to its own ETH.

Calls it takes: the factory's createRollupFor; a rollup's claimSequencerFor,
renewLeaseFor, releaseLeaseFor, submitCheckpointFor, setCheckpointIntervalFor;
its DA's commitRangeFor, submitBatchFor, commitBatchFor. Anything else is
refused (400) before any simulation: the relay's ETH pays for HPPLite, not
for whatever a caller sends.
"""
import argparse
import base64
import hashlib
import hmac
import http.server
import json
import math
import os
import socketserver
import sys
import threading
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, ".."))
from x402seller import eth  # noqa: E402
from x402seller.chain import Chain, RpcError, revert_reason, selector, signed_tx  # noqa: E402
from x402seller.payments import Payments  # noqa: E402


FACTORY_CALLS = {selector("createRollupFor(address,uint256,uint256,uint256,bytes)"): "createRollupFor"}
ROLLUP_CALLS = {selector(s): s.split("(")[0] for s in (
    "claimSequencerFor(bytes32,uint256,uint256,bytes)",
    "renewLeaseFor(bytes32,uint256,uint256,bytes)",
    "releaseLeaseFor(bytes32,uint256,uint256,bytes)",
    "submitCheckpointFor(bytes32,uint256,uint256,bytes32,bytes)",
    "setCheckpointIntervalFor(uint256,uint256,uint256,bytes)")}
DA_CALLS = {selector(s): s.split("(")[0] for s in (
    "commitRangeFor(bytes32,uint256,uint256,bytes32,bytes32,bytes32,bytes,bytes)",
    "submitBatchFor(bytes32,uint256,bytes,bytes)",
    "commitBatchFor(bytes32,uint256,bytes32,bytes,bytes,bytes)")}


PROTOCOL = "hpplite-relay/1"        # the version of doc/relay-protocol.md this relay speaks (#188)


def quote_key(relay_key: bytes) -> bytes:
    """The MAC key of the relay's quotes, derived from its own: any process holding the relay's key honours them (#184)"""
    return eth.keccak256(b"hpplite-relay-quote" + relay_key)


def quote_mac(key: bytes, chain_id, to, data, amount, gas, until) -> str:
    msg = "%d|%s|%s|%d|%d|%d" % (int(chain_id), to.lower(), eth.keccak256(bytes.fromhex(data[2:])).hex(), int(amount), int(gas), int(until))
    return hmac.new(key, msg.encode(), hashlib.sha256).hexdigest()


def call_price(gas, fee, margin, eth_usd, min_fee) -> int:
    """Atomic units (USDC.e: 6 decimals) for gas at fee wei, the margin on top, converted at eth_usd; at least min_fee.
    The arithmetic in this order, in IEEE doubles, is the protocol's (doc/relay-protocol.md; the vectors, #189)"""
    wei = gas * fee * (1 + margin)
    units = math.ceil(wei * eth_usd * 1e6 / 1e18)
    return max(units, min_fee)

# ============ The relay ============

class Relay:
    def __init__(self, args):
        self.args = args
        self.chain = Chain(args.rpc, "hpplite-relay/1")
        with open(args.key_file) as f:
            self.key = bytes.fromhex(f.read().strip().replace("0x", ""))
        self.address = eth.address(self.key)
        self.chain_id = int(self.chain.call("eth_chainId"), 16)
        self.factory = args.factory.lower()
        pay_to = args.pay_to or self.address
        self.payments = Payments(args.facilitator, "eip155:%d" % self.chain_id, args.asset, pay_to,
                                 args.asset_name, args.asset_version, args.rpc)
        self.lock = threading.Lock()          # one transaction at a time on the relay's key: its nonce
        # the 402's price holds for the paid retry with nothing remembered between them (#184): the quote travels in
        # the offer's extra, MACed under a key derived from the relay's own, so any process holding that key honours it
        self.quote_key = quote_key(self.key)
        self.submitted = 0
        self.started = int(time.time())

    # --- what it takes ---
    def classify(self, to: str, data: str):
        """The call's name, or (None, why) when the relay does not take it."""
        sel = data[2:10]
        to = to.lower()
        try:
            if to == self.factory:
                return FACTORY_CALLS.get(sel), "not a call the relay takes on the factory"
            if sel in ROLLUP_CALLS:
                da = self.chain.view(self.factory, "daOf(address)", to)
                if int(da, 16) == 0:
                    return None, "not a rollup of factory %s" % self.factory
                return ROLLUP_CALLS[sel], None
            if sel in DA_CALLS:
                coord = "0x" + self.chain.view(to, "coordinator()")[-40:]
                da = self.chain.view(self.factory, "daOf(address)", coord)
                if da[-40:].lower() != to[2:]:
                    return None, "not the DA of a rollup of factory %s" % self.factory
                return DA_CALLS[sel], None
        except (RpcError, OSError, ValueError) as e:
            return None, "could not check the target: %s" % e
        return None, "not a call the relay takes"

    # --- what it costs ---
    def _quote_mac(self, to, data, amount, gas, until):
        return quote_mac(self.quote_key, self.chain_id, to, data, amount, gas, until)

    def quote_extra(self, to, data, amount, gas, until):
        return {"quote": {"gas": gas, "until": until, "mac": self._quote_mac(to, data, amount, gas, until)}}

    def honoured(self, accepted, to, data, now):
        """(amount, gas, until) of the quote the payment names, when this relay made it for this call and it holds; else None"""
        try:
            q = (accepted.get("extra") or {}).get("quote") or {}
            amount, gas, until = int(accepted["amount"]), int(q["gas"]), int(q["until"])
            ok = until >= now and hmac.compare_digest(str(q["mac"]), self._quote_mac(to, data, amount, gas, until))
        except (AttributeError, KeyError, TypeError, ValueError):
            return None
        return (amount, gas, until) if ok else None

    def fees(self):
        base = int(self.chain.call("eth_getBlockByNumber", "latest", False).get("baseFeePerGas", "0x0"), 16)
        try:
            tip = int(self.chain.call("eth_maxPriorityFeePerGas"), 16)
        except RpcError:
            tip = 0
        return tip, 2 * base + tip, base + tip      # the tip, the cap the transaction carries, what it is expected to pay

    def price(self, gas, fee):
        return call_price(gas, fee, self.args.margin, self.args.eth_usd, self.args.min_fee)

    # --- doing it ---
    def submit(self, to, data, gas):
        """Under the lock: send, wait for the receipt. (receipt or None, error)."""
        tip, max_fee, _ = self.fees()
        nonce = int(self.chain.call("eth_getTransactionCount", self.address, "pending"), 16)
        raw = signed_tx(self.key, self.chain_id, nonce, tip, max_fee, gas, to, bytes.fromhex(data[2:]))
        try:
            tx = self.chain.call("eth_sendRawTransaction", "0x" + raw.hex())
        except RpcError as e:
            return None, "submission refused: %s" % revert_reason(e)
        deadline = time.time() + self.args.receipt_timeout
        while time.time() < deadline:
            rcpt = self.chain.call("eth_getTransactionReceipt", tx)
            if rcpt:
                self.submitted += 1
                return rcpt, None
            time.sleep(0.5)
        return {"transactionHash": tx, "status": None}, "no receipt after %d s" % self.args.receipt_timeout

    def status(self):
        try:
            bal = int(self.chain.call("eth_getBalance", self.address, "latest"), 16)
        except (RpcError, OSError):
            bal = None
        return {
            "service": "hpplite-relay", "version": 1, "protocol": PROTOCOL,     # doc/relay-protocol.md (#188)
            "address": self.address, "chainId": self.chain_id,
            "factory": self.factory, "balanceWei": bal, "submitted": self.submitted, "settled": self.payments.settled,
            "calls": sorted(set(FACTORY_CALLS.values()) | set(ROLLUP_CALLS.values()) | set(DA_CALLS.values())),
            "payment": {"scheme": "exact", "network": self.payments.network, "asset": self.args.asset,
                        "payTo": self.payments.pay_to, "ethUsd": self.args.eth_usd, "margin": self.args.margin,
                        "minFee": self.args.min_fee, "facilitator": self.args.facilitator}
                       if self.payments.enabled else None,
            "uptime": int(time.time()) - self.started,
        }


class Handler(http.server.BaseHTTPRequestHandler):
    relay = None  # type: Relay

    def log_message(self, fmt, *args):
        if self.relay.args.verbose:
            sys.stderr.write("relay: " + fmt % args + "\n")

    def reply(self, status, obj, headers=None):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/status":
            return self.reply(200, self.relay.status())
        return self.reply(404, {"error": "not found"})

    def do_POST(self):
        if self.path != "/commit":
            return self.reply(404, {"error": "not found"})
        try:
            n = int(self.headers.get("Content-Length", "0"))
            req = json.loads(self.rfile.read(n) if n > 0 else b"{}")
            to, data = req["to"], req["data"]
            if not (isinstance(to, str) and to.startswith("0x") and len(to) == 42 and isinstance(data, str)
                    and data.startswith("0x") and len(data) >= 10 and len(data) % 2 == 0):
                raise ValueError("to/data")
            bytes.fromhex(to[2:] + data[2:])
        except (ValueError, KeyError, TypeError):
            return self.reply(400, {"error": "the body is {\"to\": address, \"data\": calldata}"})
        r = self.relay
        name, why = r.classify(to, data)
        if not name:
            return self.reply(400, {"error": why})
        now = int(time.time())
        p = r.payments
        header = self.headers.get("PAYMENT-SIGNATURE") if p.enabled else None
        payload = p.decode(header) if header else None
        if header and (not payload or "payload" not in payload):
            return self.reply(400, {"error": "PAYMENT-SIGNATURE is not a payment payload"})
        # a payment for a quote this relay made, still good: its price and gas, as offered. Otherwise a fresh quote
        # (and a payment for something else is checked against it, as any)
        quote = r.honoured(payload.get("accepted"), to, data, now) if payload else None
        if not quote:
            try:
                gas = int(r.chain.call("eth_estimateGas", {"from": r.address, "to": to, "data": data}), 16)
            except RpcError as e:
                return self.reply(422, {"error": "the call would revert: %s" % revert_reason(e), "call": name})
            gas = int(gas * 1.25) + 10000                        # headroom: the state moves between estimate and inclusion
            quote = (r.price(gas, r.fees()[2]), gas, now + 120)
        amount, gas, until = quote
        requirements = None
        if p.enabled:
            requirements = p.requirements(amount, "http://%s/commit" % self.headers.get("Host", "localhost"),
                                          "%s, %d gas, submitted and paid for by the relay" % (name, gas),
                                          r.quote_extra(to, data, amount, gas, until))
            if not header:
                return self.reply(402, requirements, {"PAYMENT-REQUIRED": base64.b64encode(json.dumps(requirements).encode()).decode()})
        if p.enabled:
            v = p.verify(payload, requirements["accepts"][0])
            if not v.get("isValid"):
                requirements["error"] = v.get("invalidReason", "invalid payment") + (
                    ": " + str(v["invalidMessage"])[:200] if v.get("invalidMessage") else "")
                return self.reply(402, requirements, {"PAYMENT-REQUIRED": base64.b64encode(json.dumps(requirements).encode()).decode()})
        with r.lock:
            rcpt, err = r.submit(to, data, gas)
            if err or int(rcpt.get("status") or "0x0", 16) != 1:
                tx = rcpt.get("transactionHash") if rcpt else None
                return self.reply(409 if rcpt else 502, {"error": err or "the transaction reverted", "tx": tx, "call": name,
                                                         "charged": False})
            settlement = None
            if p.enabled:
                settlement = p.settle(payload, requirements["accepts"][0])
        out = {"tx": rcpt["transactionHash"], "block": int(rcpt["blockNumber"], 16), "gasUsed": int(rcpt["gasUsed"], 16),
               "call": name}
        headers = {}
        if settlement is not None:
            if not settlement.get("success"):
                sys.stderr.write("relay: %s landed (%s) but the payment did not settle: %s\n"
                                 % (name, out["tx"], settlement.get("errorReason")))
                out["settlement"] = settlement
            headers["PAYMENT-RESPONSE"] = base64.b64encode(json.dumps(settlement).encode()).decode()
        return self.reply(200, out, headers)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser(description="HPPLite reference relay (#170)")
    ap.add_argument("--port", type=int, default=8430)
    ap.add_argument("--rpc", required=True)
    ap.add_argument("--key-file", required=True, help="file with the hex key that pays the gas")
    ap.add_argument("--factory", required=True, help="the HPPLite factory whose rollups the relay serves (VERSION 10)")
    ap.add_argument("--pay-to", default=None, help="where payments land (default: the relay's own address)")
    ap.add_argument("--facilitator", default=None, help="x402 facilitator URL, or none: free")
    ap.add_argument("--asset", default="0x401eCb1D350407f13ba348573E5630B83638E30D", help="the EIP-3009 token (USDC.e on HPP)")
    ap.add_argument("--asset-name", default="Bridged USDC")
    ap.add_argument("--asset-version", default="2")
    ap.add_argument("--eth-usd", type=float, default=3000.0, help="the rate the gas is charged at")
    ap.add_argument("--margin", type=float, default=0.2, help="on top of the gas, for the relay's risk and upkeep")
    ap.add_argument("--min-fee", type=int, default=100, help="atomic units (USDC.e: 1e-6) at least per call")
    ap.add_argument("--receipt-timeout", type=int, default=90)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    Handler.relay = Relay(args)
    srv = Server(("127.0.0.1", args.port), Handler)
    print("relay: listening on 127.0.0.1:%d, address %s, chain %d, factory %s, %s" % (
        args.port, Handler.relay.address, Handler.relay.chain_id, args.factory,
        ("paid via " + args.facilitator) if args.facilitator else "free"), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
