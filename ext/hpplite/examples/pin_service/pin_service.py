#!/usr/bin/env python3 -I
"""A reference storage provider for HPPLite nodes (#125): stores batches
content-addressed, charges per call over x402, signs a receipt for every
batch it holds. What an HPP operator reads before running the real thing,
and what HPPLite's IPFS adapter (#11) is tested against. Standard library
only; eth.py beside it does keccak, secp256k1 and EIP-712.

    pin_service.py --port 8420 --dir ./store --signer-key /path/to/hexkey \
        --pay-to 0x... --facilitator https://facilitator-sepolia.hpp.io \
        --network eip155:181228 --asset 0x401eCb1D350407f13ba348573E5630B83638E30D

    pin_service.py ... --facilitator mock --rpc http://127.0.0.1:8545 \
        --asset <MockUSDC> --settle-key <hex>          # anvil: verify by eth_call, settle with cast send

Endpoints
  POST /put            bytes in; {cid, bytes, expires, receipt} out. Paid: the 402 quotes
                       price-per-pin + price-per-mib-month * size in MiB * retention in 30-day
                       months (+ price-per-byte * size, 0 by default), rounded up to a whole unit,
                       for `retention` seconds (#172). A CID already held answers 200 with its
                       receipt and no 402 (idempotent by CID).
  POST /pin/<cid>      extend retention by `retention`; paid; a new receipt.
  GET  /get/<cid>      the bytes; free unless --paid-get.
  GET  /status         prices, retention, pinned count, the signer address and the receipt's
                       typed-data layout: where a node learns which key signs receipts.

The receipt is the provider's promise: EIP-712 over Receipt(bytes32 hash,
string cid, uint256 expires) in the domain ("HPPLitePinService", "1",
chainId, pay-to address), signed by --signer-key. HPPLite verifies it
against /status's signer and commits it with the batch (#33).

The x402 seller side is written out here on purpose: build the 402 with the
price, decode PAYMENT-SIGNATURE, POST it to the facilitator's /verify, do the
work, POST to /settle, answer with PAYMENT-RESPONSE. No signing happens in
the seller. `hpp-x402 serve --handler <url>` wraps a plain endpoint the same
way with no code; this file is for reading what it does.

Next to `exact`, each 402 offers x402's batch-settlement (#135, channels.py):
a deposit into x402's escrow once, then a voucher the service checks itself
per put, and one claim transaction for every channel each --claim-interval.

The CID is CIDv1, raw codec, sha-256, base32 ("bafkrei..."): the same bytes
give the same CID at any IPFS provider, so one commitment can be pinned at
several. --kubo http://127.0.0.1:5001 forwards put and pin to a Kubo node
(its `add` with raw-leaves and CIDv1 gives that CID too) instead of the
directory.
"""
import argparse
import re
import base64
import hashlib
import http.client
import http.server
import json
import math
import os
import secrets
import socketserver
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eth  # noqa: E402
import channels  # noqa: E402

# ============ CID ============

_B32 = "abcdefghijklmnopqrstuvwxyz234567"


def _base32_lower(b: bytes) -> str:
    out, bits, acc = [], 0, 0
    for byte in b:
        acc = (acc << 8) | byte
        bits += 8
        while bits >= 5:
            out.append(_B32[(acc >> (bits - 5)) & 31])
            bits -= 5
    if bits:
        out.append(_B32[(acc << (5 - bits)) & 31])
    return "".join(out)


def cid_of(data: bytes) -> str:
    """CIDv1, raw (0x55), sha2-256 multihash (0x12, 0x20), multibase 'b' (base32 lower)."""
    mh = b"\x12\x20" + hashlib.sha256(data).digest()
    return "b" + _base32_lower(b"\x01\x55" + mh)


# ============ The store ============

class Store:
    """A directory: <cid> holds the bytes, index.json the expiry and receipt per CID."""

    def __init__(self, directory):
        self.dir = directory
        os.makedirs(directory, exist_ok=True)
        self.lock = threading.Lock()
        self.index_path = os.path.join(directory, "index.json")
        try:
            with open(self.index_path) as f:
                self.index = json.load(f)
        except (OSError, ValueError):
            self.index = {}

    def _save(self):
        tmp = self.index_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.index, f)
        os.replace(tmp, self.index_path)

    def has(self, cid):
        with self.lock:
            return cid in self.index and os.path.exists(os.path.join(self.dir, cid))

    def get(self, cid):
        try:
            with open(os.path.join(self.dir, cid), "rb") as f:
                return f.read()
        except OSError:
            return None

    def put(self, cid, data, expires, receipt):
        with self.lock:
            path = os.path.join(self.dir, cid)
            tmp = path + ".tmp"
            with open(tmp, "wb") as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, path)
            self.index[cid] = {"bytes": len(data), "expires": expires, "receipt": receipt}
            self._save()

    def extend(self, cid, expires, receipt):
        with self.lock:
            self.index[cid]["expires"] = expires
            self.index[cid]["receipt"] = receipt
            self._save()

    def entry(self, cid):
        with self.lock:
            return dict(self.index.get(cid) or {})

    # --- named entries (#165): <da>-<height> holds a batch's bytes with the sequencer's
    # signature appended; the first signer seen for a DA contract holds its names ---
    def name_path(self, name):
        return os.path.join(self.dir, "name-" + name)

    def name_signer(self, da):
        with self.lock:
            return (self.index.get("names") or {}).get(da.lower())

    def put_named(self, name, da, signer, data):
        with self.lock:
            path = self.name_path(name)
            tmp = path + ".tmp"
            with open(tmp, "wb") as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, path)
            names = self.index.setdefault("names", {})
            names.setdefault(da.lower(), signer)
            self._save()

    def get_named(self, name):
        try:
            with open(self.name_path(name), "rb") as f:
                return f.read()
        except OSError:
            return None

    def count(self):
        with self.lock:
            return len(self.index)


class KuboStore(Store):
    """The same, with the bytes pinned on a Kubo node (its HTTP RPC API, port 5001)."""

    def __init__(self, directory, api):
        super().__init__(directory)
        self.api = api.rstrip("/")

    def _rpc(self, path, data=None, query=""):
        url = "%s/api/v0/%s%s" % (self.api, path, query)
        if data is None:
            req = urllib.request.Request(url, method="POST")
        else:
            boundary = "----hpplite" + secrets.token_hex(8)
            body = (("--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"batch\"\r\n"
                     "Content-Type: application/octet-stream\r\n\r\n") % boundary).encode() + data + ("\r\n--%s--\r\n" % boundary).encode()
            req = urllib.request.Request(url, data=body, method="POST",
                                         headers={"Content-Type": "multipart/form-data; boundary=" + boundary})
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.read()

    def put(self, cid, data, expires, receipt):
        out = json.loads(self._rpc("add", data, "?cid-version=1&raw-leaves=true&pin=true").splitlines()[-1])
        if out.get("Hash") != cid:
            raise ValueError("Kubo computed %s, this service %s: chunking differs" % (out.get("Hash"), cid))
        super().put(cid, data, expires, receipt)

    def get(self, cid):
        try:
            return self._rpc("cat", None, "?arg=" + cid)
        except (OSError, ValueError):
            return super().get(cid)


# ============ Receipts ============

RECEIPT_TYPE = "Receipt(bytes32 hash,string cid,uint256 expires)"


class Signer:
    def __init__(self, key: bytes, chain_id: int, pay_to: str):
        self.key = key
        self.address = eth.address(key)
        self.domain = eth.domain_separator("HPPLitePinService", "1", chain_id, pay_to)
        self.chain_id = chain_id
        self.pay_to = pay_to

    def receipt(self, data_hash: bytes, cid: str, expires: int) -> dict:
        digest = eth.typed_digest(self.domain, RECEIPT_TYPE, data_hash, eth.keccak256(cid.encode()), expires)
        sig = eth.sign(self.key, digest)
        return {"hash": "0x" + data_hash.hex(), "cid": cid, "expires": expires,
                "signer": self.address, "signature": "0x" + sig.hex()}


# ============ x402: the seller's side ============

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
                                              "User-Agent": "hpplite-pin-service/1 (+https://github.com/shepelt/hpplite)"}, method="POST")
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


MIB = 1024 * 1024
MONTH = 30 * 24 * 3600

# ============ The HTTP service ============

NAME_RE = re.compile(r"^[0-9a-f]{40}-[0-9]{1,18}$")
BATCH_TYPE = "Batch(bytes32 instanceId,uint256 height,bytes32 batchHash)"


def batch_digest(chain_id, name, instance, data):
    """What HPPLiteDA.batchDigest() computes (DA version 3): the sequencer's claim about one height."""
    da, height = name.split("-")
    domain = eth.domain_separator("HPPLiteDA", "3", chain_id, "0x" + da)
    return eth.typed_digest(domain, BATCH_TYPE, bytes.fromhex(instance[2:]), int(height), eth.keccak256(data))


def sequencer_lease(rpc, da):
    """The rollup's sequencer wallet and the instance its lease names, asked of the chain: HPPLiteDA.coordinator(),
    then HPPLite.sequencerWallet() and sequencerInstance(); ('', '') when unreachable."""
    def call(to, selector):
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "eth_call", "params": [{"to": to, "data": selector}, "latest"]}).encode()
        req = urllib.request.Request(rpc, data=body, headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read()).get("result") or ""
    try:
        coord = call(da, "0x" + eth.keccak256(b"coordinator()")[:4].hex())
        if len(coord) < 66:
            return "", ""
        wallet = call("0x" + coord[-40:], "0x" + eth.keccak256(b"sequencerWallet()")[:4].hex())
        instance = call("0x" + coord[-40:], "0x" + eth.keccak256(b"sequencerInstance()")[:4].hex())
        return ("0x" + wallet[-40:] if len(wallet) >= 66 else "", instance[:66].lower() if len(instance) >= 66 else "")
    except (OSError, ValueError):
        return "", ""


class Service:
    def __init__(self, args):
        self.args = args
        self.store = KuboStore(args.dir, args.kubo) if args.kubo else Store(args.dir)
        with open(args.signer_key) as f:
            key = bytes.fromhex(f.read().strip().replace("0x", ""))
        chain_id = int(args.network.split(":")[1])
        self.chain_id = chain_id
        self.signer = Signer(key, chain_id, args.pay_to)
        self.payments = Payments(args.facilitator, args.network, args.asset, args.pay_to,
                                 args.asset_name, args.asset_version, args.rpc, args.settle_key)
        if self.payments.enabled and args.channels:
            # the receipt key authorizes claims and refunds too: one key speaks for this service
            self.payments.channels = channels.Channels(args.dir, self.payments, key, args.withdraw_delay, args.min_deposit,
                                                       args.claim_interval, args.refund_idle)
            self.payments.channels.mock_queued = args.mock_deposit_queued
            self.payments.channels.start()
        self.started = int(time.time())

    def price_put(self, nbytes):
        """Whole units: the per-put fee, the storage for the retention bought, the legacy per-byte price (#172)."""
        months = self.args.retention / MONTH
        storage = self.args.price_per_mib_month * nbytes / MIB * months
        return self.args.price_per_pin + self.args.price_per_byte * nbytes + math.ceil(storage - 1e-9)

    def status(self):
        return {
            "service": "hpplite-pin-service", "version": 1,
            "signer": self.signer.address, "receipt": {"domain": {"name": "HPPLitePinService", "version": "1",
                                                                   "chainId": self.signer.chain_id, "verifyingContract": self.signer.pay_to},
                                                        "type": RECEIPT_TYPE},
            "payment": {"scheme": "exact", "network": self.args.network, "asset": self.args.asset, "payTo": self.args.pay_to,
                        "pricePerByte": self.args.price_per_byte, "pricePerPin": self.args.price_per_pin,
                        "pricePerMiBMonth": self.args.price_per_mib_month,
                        "retentionSeconds": self.args.retention, "paidGet": bool(self.args.paid_get),
                        "facilitator": self.args.facilitator or "none (free)",
                        "schemes": ["exact"] + ([channels.SCHEME] if self.payments.channels else []),
                        "channels": self.payments.channels.status() if self.payments.channels else None} if self.payments.enabled else None,
            "pinned": self.store.count(), "settled": self.payments.settled, "uptime": int(time.time()) - self.started,
        }


class Handler(http.server.BaseHTTPRequestHandler):
    service = None  # type: Service

    def log_message(self, fmt, *args):
        if self.service.args.verbose:
            sys.stderr.write("pin_service: " + fmt % args + "\n")

    # --- replies ---
    def reply(self, status, obj, headers=None):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def reply_bytes(self, data):
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    # --- x402, around one piece of work ---
    def paid(self, amount, description, work):
        """402 until a payment for `amount` verifies; then the work, then settle, then the answer."""
        p = self.service.payments
        url = "http://%s%s" % (self.headers.get("Host", "localhost"), self.path)
        if not p.enabled or amount <= 0:
            status, obj = work()
            return self.reply(status, obj)
        requirements = p.requirements(amount, url, description)
        header = self.headers.get("PAYMENT-SIGNATURE")
        if not header:
            return self.reply(402, requirements, {"PAYMENT-REQUIRED": base64.b64encode(json.dumps(requirements).encode()).decode()})
        payload = p.decode(header)
        if not payload or "payload" not in payload:
            return self.reply(400, {"error": "PAYMENT-SIGNATURE is not a payment payload"})
        if (payload.get("accepted") or {}).get("scheme") == channels.SCHEME and p.channels:
            return self.paid_by_voucher(requirements, payload, work)
        v = p.verify(payload, requirements["accepts"][0])
        if not v.get("isValid"):
            requirements["error"] = v.get("invalidReason", "invalid payment")
            if v.get("invalidMessage"):
                requirements["error"] += ": " + str(v["invalidMessage"])[:200]
            return self.reply(402, requirements, {"PAYMENT-REQUIRED": base64.b64encode(json.dumps(requirements).encode()).decode()})
        status, obj = work()
        if status != 200:
            return self.reply(status, obj)          # nothing done, nothing charged
        s = p.settle(payload, requirements["accepts"][0])
        if not s.get("success"):
            return self.reply(402, {"error": "settlement failed: " + s.get("errorReason", "?"), "settlement": s})
        return self.reply(200, obj, {"PAYMENT-RESPONSE": base64.b64encode(json.dumps(s).encode()).decode()})

    def paid_by_voucher(self, requirements, payload, work):
        """batch-settlement (#135): the voucher checked here, nothing on the chain unless it opens or tops up the channel"""
        ch = self.service.payments.channels
        req = requirements["accepts"][-1]
        ok, cid, err, deposit_tx = ch.verify(payload, req)
        if not ok:
            if err == channels.MISMATCH:
                requirements = ch.corrective(requirements, cid)
            else:
                requirements["error"] = err
            return self.reply(402, requirements, {"PAYMENT-REQUIRED": base64.b64encode(json.dumps(requirements).encode()).decode()})
        status, obj = work()
        if status != 200:
            return self.reply(status, obj)          # nothing done, nothing charged (a deposit stays the node's, in its channel)
        r = ch.commit(payload, req, cid, deposit_tx)
        self.service.payments.settled += 1 if deposit_tx else 0
        return self.reply(200, obj, {"PAYMENT-RESPONSE": base64.b64encode(json.dumps(r).encode()).decode()})

    # --- the endpoints ---
    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path == "/status":
            return self.reply(200, self.service.status())
        if path.startswith("/name/"):
            name = path[6:]
            data = self.service.store.get_named(name) if NAME_RE.match(name) else None
            if data is None:
                return self.reply(404, {"error": "no such name", "name": name})
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("X-HPPLite-Signer", eth.recover(batch_digest(self.service.chain_id, name, "0x" + data[-97:-65].hex(), data[:-97]), data[-65:]) if len(data) > 97 else "")
            self.end_headers()
            self.wfile.write(data)
            return
        if path.startswith("/get/"):
            cid = path[5:]
            if not self.service.store.has(cid):
                return self.reply(404, {"error": "not pinned here", "cid": cid})

            def work():
                data = self.service.store.get(cid)
                return (200, data) if data is not None else (404, {"error": "gone"})
            if self.service.args.paid_get and self.service.payments.enabled:
                # a paid read: the same dance, the body being bytes
                entry = self.service.store.entry(cid)
                return self.paid(self.service.price_put(entry.get("bytes", 0)), "read " + cid, lambda: self._bytes_as_json(cid))
            data = self.service.store.get(cid)
            if data is None:
                return self.reply(404, {"error": "gone"})
            return self.reply_bytes(data)
        return self.reply(404, {"error": "no such endpoint"})

    def _bytes_as_json(self, cid):
        data = self.service.store.get(cid)
        if data is None:
            return 404, {"error": "gone"}
        return 200, {"cid": cid, "data": base64.b64encode(data).decode()}

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        if path.startswith("/name/"):
            # a batch under its rollup's name (#165): the bytes, then the sequencer instance (32 bytes) and its
            # EIP-712 Batch signature (65 bytes) the instance names. Free: the content put paid.
            name = path[6:]
            if not NAME_RE.match(name) or len(body) <= 97 or len(body) > self.service.args.max_bytes + 97:
                return self.reply(400, {"error": "a name is <da>-<height>, the body the bytes, a 32-byte instance and a 65-byte signature"})
            instance = "0x" + body[-97:-65].hex()
            da = "0x" + name.split("-")[0]
            signer = eth.recover(batch_digest(self.service.chain_id, name, instance, body[:-97]), body[-65:])
            if not signer:
                return self.reply(400, {"error": "the signature does not recover"})
            holder = self.service.store.name_signer(da)
            if holder and holder.lower() != signer.lower():
                return self.reply(403, {"error": "another signer holds this rollup's names", "signer": signer, "holder": holder})
            if self.service.store.get_named(name) == body:
                return self.reply(200, {"name": name, "bytes": len(body) - 97, "signer": signer, "held": True})
            if self.service.args.rpc and self.service.args.rpc != "mock":
                wallet, leased = sequencer_lease(self.service.args.rpc, da)
                if wallet and wallet.lower() != signer.lower():
                    return self.reply(403, {"error": "not the rollup's sequencer wallet", "signer": signer, "sequencer": wallet})
                if leased and leased != instance.lower():
                    # the same wallet, another process: a sequencer that lost the lease (partitioned, #167) must not
                    # write over the names of the one the chain now names
                    sys.stderr.write("pin_service: refused %s from instance %s: the lease names %s\n" % (name, instance, leased))
                    return self.reply(409, {"error": "not the instance the rollup's lease names", "instance": instance, "lease": leased})
            self.service.store.put_named(name, da, signer, body)
            return self.reply(200, {"name": name, "bytes": len(body) - 97, "signer": signer})
        if path == "/put":
            if not body:
                return self.reply(400, {"error": "empty body"})
            if len(body) > self.service.args.max_bytes:
                return self.reply(413, {"error": "too large", "max": self.service.args.max_bytes})
            cid = cid_of(body)
            if self.service.store.has(cid):
                e = self.service.store.entry(cid)              # idempotent: held already, nothing to pay
                return self.reply(200, {"cid": cid, "bytes": e["bytes"], "expires": e["expires"], "receipt": e["receipt"], "held": True})

            def work():
                expires = int(time.time()) + self.service.args.retention
                receipt = self.service.signer.receipt(eth.keccak256(body), cid, expires)
                try:
                    self.service.store.put(cid, body, expires, receipt)
                except (OSError, ValueError) as e:
                    return 500, {"error": str(e)}
                return 200, {"cid": cid, "bytes": len(body), "expires": expires, "receipt": receipt, "held": False}
            return self.paid(self.service.price_put(len(body)), "pin %d bytes for %d s" % (len(body), self.service.args.retention), work)
        if path.startswith("/pin/"):
            cid = path[5:]
            if not self.service.store.has(cid):
                return self.reply(404, {"error": "not pinned here: put it first", "cid": cid})
            entry = self.service.store.entry(cid)

            def work():
                base = max(int(time.time()), int(entry.get("expires", 0)))
                expires = base + self.service.args.retention
                data = self.service.store.get(cid)
                if data is None:
                    return 404, {"error": "gone"}
                receipt = self.service.signer.receipt(eth.keccak256(data), cid, expires)
                self.service.store.extend(cid, expires, receipt)
                return 200, {"cid": cid, "bytes": len(data), "expires": expires, "receipt": receipt}
            return self.paid(self.service.price_put(entry.get("bytes", 0)), "extend " + cid, work)
        return self.reply(404, {"error": "no such endpoint"})


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser(description="HPPLite reference pin service (#125)")
    ap.add_argument("--port", type=int, default=8420)
    ap.add_argument("--dir", default="./pin_store")
    ap.add_argument("--signer-key", required=True, help="file with the hex key that signs receipts")
    ap.add_argument("--pay-to", required=True, help="where payments land; also the receipt domain's verifyingContract")
    ap.add_argument("--facilitator", default=None, help="x402 facilitator URL, 'mock' (anvil), or none: free")
    ap.add_argument("--network", default="eip155:181228")
    ap.add_argument("--asset", default="0x401eCb1D350407f13ba348573E5630B83638E30D", help="the EIP-3009 token (USDC.e on HPP)")
    ap.add_argument("--asset-name", default="Bridged USDC", help="the token's EIP-712 domain name (USDC.e on HPP: name() is \"Bridged USDC\", version \"2\")")
    ap.add_argument("--asset-version", default="2")
    ap.add_argument("--rpc", default=None, help="mock facilitator: the chain to verify and settle on")
    ap.add_argument("--settle-key", default=None, help="mock facilitator: the key that submits transferWithAuthorization")
    ap.add_argument("--price-per-mib-month", type=float, default=30.0,
                    help="atomic units (USDC.e: 1e-6) per MiB per 30 days, fractions allowed; the default, $0.03 per GiB-month,"
                         " is R2's $0.015 plus 100%% (#172)")
    ap.add_argument("--price-per-pin", type=int, default=10, help="atomic units per put or pin, whatever its size: the settlement")
    ap.add_argument("--price-per-byte", type=int, default=0, help="atomic units per byte on top (before #172 the only size price: 1)")
    ap.add_argument("--retention", type=int, default=30 * 24 * 3600, help="seconds a put or a pin buys")
    ap.add_argument("--max-bytes", type=int, default=16 * 1024 * 1024)
    ap.add_argument("--paid-get", action="store_true")
    ap.add_argument("--channels", action=argparse.BooleanOptionalAction, default=True,
                    help="offer x402 batch-settlement beside exact (#135): a deposit once, a signed voucher per put, claims in batches")
    ap.add_argument("--withdraw-delay", type=int, default=24 * 3600, help="batch-settlement: seconds a payer's timed withdrawal waits (900 to 30 days)")
    ap.add_argument("--min-deposit", type=int, default=100000, help="batch-settlement: the deposit asked for (atomic units; at least 10 puts' worth)")
    ap.add_argument("--claim-interval", type=int, default=3600, help="batch-settlement: seconds between claims (one transaction for all channels) and settlement")
    ap.add_argument("--refund-idle", type=int, default=7 * 24 * 3600, help="batch-settlement: give back what a channel idle this long holds beyond its charges")
    ap.add_argument("--mock-deposit-queued", action="store_true", help=argparse.SUPPRESS)   # tests: a deposit answered "queued" (#135)
    ap.add_argument("--kubo", default=None, help="a Kubo node's RPC API (http://127.0.0.1:5001): pin there too")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    if args.facilitator == "mock" and not (args.rpc and args.settle_key):
        ap.error("--facilitator mock needs --rpc and --settle-key")
    if not 900 <= args.withdraw_delay <= 30 * 24 * 3600:
        ap.error("--withdraw-delay is 900 seconds to 30 days (the escrow's bounds)")
    if args.claim_interval * 4 > args.withdraw_delay:
        ap.error("--claim-interval must be well inside --withdraw-delay: vouchers not claimed when a withdrawal finishes are lost")
    Handler.service = Service(args)
    srv = Server(("127.0.0.1", args.port), Handler)
    print("pin_service: listening on 127.0.0.1:%d, signer %s, %s" % (
        args.port, Handler.service.signer.address, ("paid via " + args.facilitator) if args.facilitator else "free"), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
