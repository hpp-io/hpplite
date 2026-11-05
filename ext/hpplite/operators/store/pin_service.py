#!/usr/bin/env python3 -I
"""A reference storage provider for HPPLite nodes (#125): stores batches
content-addressed, charges per call over x402, signs a receipt for every
batch it holds. What an HPP operator reads before running the real thing,
and what HPPLite's IPFS adapter (#11) is tested against. Standard library
only; eth.py beside it does keccak, secp256k1 and EIP-712.

    pin_service.py --port 8420 --dir ./store --signer-key /path/to/hexkey \
        --pay-to 0x... --facilitator https://facilitator-sepolia.hpp.io \
        --network eip155:181228 --asset 0x401eCb1D350407f13ba348573E5630B83638E30D

    pin_service.py ... --facilitator http://127.0.0.1:8440 --rpc http://127.0.0.1:8545 \
        --asset <MockUSDC>          # anvil: operators/conformance/facilitator_mock.py (#183)

Endpoints
  POST /put            bytes in; {cid, bytes, expires, receipt} out. Paid: the 402 quotes
                       price-per-pin + price-per-mib-month * size in MiB * retention in 30-day
                       months (+ price-per-byte * size, 0 by default), rounded up to a whole unit,
                       for `retention` seconds (#172). A CID already held answers 200 with its
                       receipt and no 402 (idempotent by CID).
  POST /pin/<cid>      extend retention by `retention`; paid; a new receipt.
  GET  /get/<cid>      the bytes; free unless --paid-get.
  GET  /status         prices, retention, the signer address and the receipt's
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
import sys
import threading
import time
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from x402seller import channels, eth  # noqa: E402
from x402seller.payments import Payments  # noqa: E402

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
    """A directory standing for an object store (R2), each object's metadata beside it and nothing global (#186), so
    several processes on one directory (Worker isolates on one bucket) agree:
      <cid>                 the bytes;  <cid>.json        {bytes, expires, receipt}
      name-<da>-<height>    a signed batch (#165);  .json  {signer, expires}
      rollup-<da>           the first signer of the rollup's names, created only if absent
      expiry/<day>/<key>    what expires that day (UTC days since the epoch), for the expiry tick
    """

    def __init__(self, directory):
        self.dir = directory
        os.makedirs(os.path.join(directory, "expiry"), exist_ok=True)
        self._migrate()

    # --- objects ---
    def _path(self, key):
        return os.path.join(self.dir, key)

    def _write(self, key, data: bytes):
        path = self._path(key)
        tmp = "%s.tmp%d.%d" % (path, os.getpid(), threading.get_ident())
        with open(tmp, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)

    def _read(self, key):
        try:
            with open(self._path(key), "rb") as f:
                return f.read()
        except OSError:
            return None

    def _meta(self, key):
        raw = self._read(key + ".json")
        try:
            return json.loads(raw) if raw else None
        except ValueError:
            return None

    def _set_meta(self, key, meta):
        self._write(key + ".json", json.dumps(meta).encode())

    def _mark(self, key, expires):
        day = os.path.join(self.dir, "expiry", str(int(expires) // 86400))
        os.makedirs(day, exist_ok=True)
        open(os.path.join(day, key), "w").close()

    def _delete(self, key):
        for k in (key, key + ".json"):
            try:
                os.remove(self._path(k))
            except OSError:
                pass

    # --- content by CID ---
    def has(self, cid):
        return self._meta(cid) is not None and os.path.exists(self._path(cid))

    def get(self, cid):
        return self._read(cid)

    def put(self, cid, data, expires, receipt):
        self._write(cid, data)
        self._set_meta(cid, {"bytes": len(data), "expires": expires, "receipt": receipt})
        self._mark(cid, expires)

    def extend(self, cid, expires, receipt):
        meta = self._meta(cid) or {}
        meta.update(expires=expires, receipt=receipt)
        self._set_meta(cid, meta)
        self._mark(cid, expires)

    def entry(self, cid):
        return dict(self._meta(cid) or {})

    def remove(self, cid):
        """A put taken back, its payment not settled (#194); its expiry marker goes with the next tick"""
        self._delete_content(cid)

    def restore(self, cid, meta):
        """An extension taken back: the metadata as it was"""
        self._set_meta(cid, meta)

    # --- named entries (#165): <da>-<height> holds a batch's bytes with the sequencer's signature appended; the
    # first signer seen for a DA contract holds its names, decided by a create-if-absent (R2's conditional put) ---
    def name_signer(self, da):
        raw = self._read("rollup-" + da.lower()[2:])
        return raw.decode() if raw else None

    def claim_rollup(self, da, signer):
        """The rollup's holder after this signer asks for it: itself if first, else whoever was"""
        path = self._path("rollup-" + da.lower()[2:])
        try:
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
        except FileExistsError:
            return self.name_signer(da)
        with os.fdopen(fd, "w") as f:
            f.write(signer.lower())
            f.flush()
            os.fsync(f.fileno())
        return signer.lower()

    def put_named(self, name, signer, data, expires):
        self._write("name-" + name, data)
        self._set_meta("name-" + name, {"signer": signer, "expires": expires})
        self._mark("name-" + name, expires)

    def get_named(self, name):
        return self._read("name-" + name)

    def named_signer(self, name):
        return (self._meta("name-" + name) or {}).get("signer")

    # --- retention (#185, #186) ---
    def expire(self, now=None):
        """Delete what is past its expiry; the keys looked at are those marked for today or earlier. Returns the count"""
        now = int(now or time.time())
        root = os.path.join(self.dir, "expiry")
        gone = 0
        for day in sorted(os.listdir(root)):
            if not day.isdigit() or int(day) > now // 86400:
                continue
            dpath = os.path.join(root, day)
            for key in os.listdir(dpath):
                meta = self._meta(key)
                if meta is not None and int(meta.get("expires", 0)) >= now:
                    if int(meta["expires"]) // 86400 == int(day):
                        continue                      # today's, not yet: looked at again by a later tick
                elif meta is not None:
                    self._delete_content(key)
                    gone += 1
                os.remove(os.path.join(dpath, key))   # done with, or extended (marked again under its new day)
            if int(day) < now // 86400 and not os.listdir(dpath):
                os.rmdir(dpath)
        return gone

    def _delete_content(self, key):
        self._delete(key)

    def _migrate(self):
        """A directory from before #186 kept one index.json: its entries become metadata beside each object"""
        path = self._path("index.json")
        try:
            with open(path) as f:
                index = json.load(f)
        except (OSError, ValueError):
            return
        for da, signer in (index.pop("names", None) or {}).items():
            if not self.name_signer(da):
                self.claim_rollup(da, signer)
        for cid, e in index.items():
            if isinstance(e, dict) and "expires" in e and self._meta(cid) is None:
                self._set_meta(cid, e)
                self._mark(cid, e["expires"])
        os.replace(path, path + ".migrated")


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

    def _delete_content(self, key):
        if not key.startswith("name-"):
            try:
                self._rpc("pin/rm", None, "?arg=" + key)        # Kubo's garbage collection frees it
            except (OSError, ValueError):
                pass
        super()._delete_content(key)


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


PROTOCOL = "hpplite-store/1"        # the version of doc/store-protocol.md this service speaks (#188)
MIB = 1024 * 1024
MONTH = 30 * 24 * 3600


def put_price(nbytes, per_pin, per_byte, per_mib_month, retention) -> int:
    """Whole units: the per-put fee, the storage for the retention bought, the legacy per-byte price (#172). The
    arithmetic in this order, in IEEE doubles, is the protocol's (doc/store-protocol.md; the vectors, #189)"""
    months = retention / MONTH
    storage = per_mib_month * nbytes / MIB * months
    return per_pin + per_byte * nbytes + math.ceil(storage - 1e-9)

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
                                 args.asset_name, args.asset_version, args.rpc)
        if self.payments.enabled and args.channels:
            # the receipt key authorizes claims and refunds too: one key speaks for this service
            self.payments.channels = channels.Channels(args.dir, self.payments, key, args.withdraw_delay, args.min_deposit,
                                                       args.claim_interval, args.refund_idle)
            self.payments.channels.start()
        self.started = int(time.time())

    def start(self):
        """The expiry tick from a timer thread; a Worker runs Store.expire from a Cron Trigger (#185, #186)"""
        def loop():
            while True:
                time.sleep(self.args.expire_interval)
                try:
                    gone = self.store.expire()
                    if gone and self.args.verbose:
                        sys.stderr.write("pin_service: %d past their retention deleted\n" % gone)
                except OSError as e:
                    sys.stderr.write("pin_service: expiry tick failed: %s\n" % e)
        threading.Thread(target=loop, daemon=True).start()

    def price_put(self, nbytes):
        return put_price(nbytes, self.args.price_per_pin, self.args.price_per_byte, self.args.price_per_mib_month, self.args.retention)

    def status(self):
        return {
            "service": "hpplite-pin-service", "version": 1,
            "protocol": self.args.advertise_protocol or PROTOCOL,           # doc/store-protocol.md (#188)
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
            "settled": self.payments.settled, "uptime": int(time.time()) - self.started,
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
        """402 until a payment for `amount` verifies; then the work, then settle, then the answer. work() returns
        (status, obj) or (status, obj, undo): undo takes the work back when the payment does not settle (#194)"""
        p = self.service.payments
        url = "http://%s%s" % (self.headers.get("Host", "localhost"), self.path)
        if not p.enabled or amount <= 0:
            status, obj = work()[:2]
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
        done = work()
        status, obj = done[:2]
        if status != 200:
            return self.reply(status, obj)          # nothing done, nothing charged
        s = p.settle(payload, requirements["accepts"][0])
        if not s.get("success"):
            if len(done) > 2:
                done[2]()                           # not paid: not held, not extended (#194)
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
        status, obj = work()[:2]
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
            signer = self.service.store.named_signer(name) or (      # recorded at the put; recovered for one from before #186
                eth.recover(batch_digest(self.service.chain_id, name, "0x" + data[-97:-65].hex(), data[:-97]), data[-65:]) if len(data) > 97 else "")
            self.send_header("X-HPPLite-Signer", signer)
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
            if self.service.args.rpc:
                wallet, leased = sequencer_lease(self.service.args.rpc, da)
                if wallet and wallet.lower() != signer.lower():
                    return self.reply(403, {"error": "not the rollup's sequencer wallet", "signer": signer, "sequencer": wallet})
                if leased and leased != instance.lower():
                    # the same wallet, another process: a sequencer that lost the lease (partitioned, #167) must not
                    # write over the names of the one the chain now names
                    sys.stderr.write("pin_service: refused %s from instance %s: the lease names %s\n" % (name, instance, leased))
                    return self.reply(409, {"error": "not the instance the rollup's lease names", "instance": instance, "lease": leased})
            holder = self.service.store.claim_rollup(da, signer)
            if holder.lower() != signer.lower():
                return self.reply(403, {"error": "another signer holds this rollup's names", "signer": signer, "holder": holder})
            self.service.store.put_named(name, signer, body, int(time.time()) + self.service.args.retention)
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
                return 200, {"cid": cid, "bytes": len(body), "expires": expires, "receipt": receipt, "held": False}, \
                    lambda: self.service.store.remove(cid)
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
                return 200, {"cid": cid, "bytes": len(data), "expires": expires, "receipt": receipt}, \
                    lambda: self.service.store.restore(cid, entry)
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
    ap.add_argument("--facilitator", default=None, help="x402 facilitator URL, or none: free")
    ap.add_argument("--network", default="eip155:181228")
    ap.add_argument("--asset", default="0x401eCb1D350407f13ba348573E5630B83638E30D", help="the EIP-3009 token (USDC.e on HPP)")
    ap.add_argument("--asset-name", default="Bridged USDC", help="the token's EIP-712 domain name (USDC.e on HPP: name() is \"Bridged USDC\", version \"2\")")
    ap.add_argument("--asset-version", default="2")
    ap.add_argument("--rpc", default=None, help="the chain, read: the rollup's lease for names, the channels' balances and refund nonces")
    ap.add_argument("--price-per-mib-month", type=float, default=30.0,
                    help="atomic units (USDC.e: 1e-6) per MiB per 30 days, fractions allowed; the default, $0.03 per GiB-month,"
                         " is R2's $0.015 plus 100%% (#172)")
    ap.add_argument("--price-per-pin", type=int, default=10, help="atomic units per put or pin, whatever its size: the settlement")
    ap.add_argument("--price-per-byte", type=int, default=0, help="atomic units per byte on top (before #172 the only size price: 1)")
    ap.add_argument("--retention", type=int, default=30 * 24 * 3600, help="seconds a put or a pin buys")
    ap.add_argument("--expire-interval", type=int, default=3600, help="seconds between expiry ticks: what is past its retention is deleted (#186)")
    ap.add_argument("--max-bytes", type=int, default=16 * 1024 * 1024)
    ap.add_argument("--paid-get", action="store_true")
    ap.add_argument("--channels", action=argparse.BooleanOptionalAction, default=True,
                    help="offer x402 batch-settlement beside exact (#135): a deposit once, a signed voucher per put, claims in batches")
    ap.add_argument("--withdraw-delay", type=int, default=24 * 3600, help="batch-settlement: seconds a payer's timed withdrawal waits (900 to 30 days)")
    ap.add_argument("--min-deposit", type=int, default=100000, help="batch-settlement: the deposit asked for (atomic units; at least 10 puts' worth)")
    ap.add_argument("--claim-interval", type=int, default=3600, help="batch-settlement: seconds between claims (one transaction for all channels) and settlement")
    ap.add_argument("--refund-idle", type=int, default=7 * 24 * 3600, help="batch-settlement: give back what a channel idle this long holds beyond its charges")
    ap.add_argument("--kubo", default=None, help="a Kubo node's RPC API (http://127.0.0.1:5001): pin there too")
    ap.add_argument("--advertise-protocol", default=None, help=argparse.SUPPRESS)   # tests: claim another protocol (#188)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    if not 900 <= args.withdraw_delay <= 30 * 24 * 3600:
        ap.error("--withdraw-delay is 900 seconds to 30 days (the escrow's bounds)")
    if args.claim_interval * 4 > args.withdraw_delay:
        ap.error("--claim-interval must be well inside --withdraw-delay: vouchers not claimed when a withdrawal finishes are lost")
    Handler.service = Service(args)
    Handler.service.start()
    srv = Server(("127.0.0.1", args.port), Handler)
    print("pin_service: listening on 127.0.0.1:%d, signer %s, %s" % (
        args.port, Handler.service.signer.address, ("paid via " + args.facilitator) if args.facilitator else "free"), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
