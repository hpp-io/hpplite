#!/usr/bin/env python3 -I
"""The reference store for HPPLite nodes, hpplite-store/2 (#213,
doc/store-protocol.md): a bucket per rollup, kept while its account covers
it. An account belongs to a funding wallet and is paid two ways - x402
`exact`, started by a payer who is online, and an EIP-2612 permit the store
pulls with, so storage stays paid while the node is off. Standard library
only; x402seller/ beside it does keccak, secp256k1, EIP-712, x402 and the
chain.

    hpplite_store.py --port 8420 --dir ./store --signer-key /path/to/hexkey --pay-to 0x... \\
        --rpc https://sepolia.hpp.io --network eip155:181228 \\
        --facilitator https://facilitator-sepolia.hpp.io      # x402 top-ups; without it, permit pulls only

    hpplite_store.py ... --price-per-write 0 --price-per-mib-month 0   # free (development)

Without --rpc the store cannot ask the chain who a rollup's sequencer is: the
first binding's sequencer keeps the bucket (development and the node's unit
tests only - a real store reads the chain).

Layout (a directory standing for an object store):
  c/<cid>            content bytes;  c/<cid>.refs  {da: height} - the buckets holding it
  b/<da>/bucket.json the bucket;     b/<da>/n/<height>  a name: the instance (32), the signature (65), the bytes' CID
  (a name's bytes are content: a name and a content put of the same bytes hold them once)
  a/<funder>.json    the account
"""
import argparse
import base64
import hashlib
import http.server
import json
import os
import re
import shutil
import socketserver
import sys
import threading
import time
import urllib.parse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from x402seller import eth  # noqa: E402
from x402seller.chain import Chain, RpcError, revert_reason, selector, signed_tx  # noqa: E402
from x402seller.payments import Payments  # noqa: E402

PROTOCOL = "hpplite-store/2"
MIB = 1024 * 1024
MONTH = 30 * 24 * 3600
FOREVER = 2 ** 53 - 1           # coveredUntil when nothing is charged: exact in a double, an int64 and every JSON (#216)

BIND_TYPE = "Bind(address da,address funder,uint256 issuedAt)"
RELEASE_TYPE = "Release(address da,address funder,uint256 issuedAt)"
DELETE_TYPE = "Delete(address da,address funder,uint256 issuedAt)"      # a release that deletes now (#275)
CONTENT_TYPE = "Content(address da,uint256 height,bytes32 hash)"
PRUNE_TYPE = "Prune(address da,uint256 height,uint256 issuedAt)"
CAP_TYPE = "Cap(address da,uint256 writesPerDay,uint256 bytesPerDay,uint256 issuedAt)"   # the funder's daily bound (#274)
DAY = 86400
STATEMENT_TYPE = "BucketStatement(address da,uint256 height,uint256 pruneHeight,uint256 coveredUntil,uint256 issuedAt)"
BATCH_TYPE = "Batch(bytes32 instanceId,uint256 height,bytes32 batchHash)"
PERMIT_TYPE = "Permit(address owner,address spender,uint256 value,uint256 nonce,uint256 deadline)"

ADDR_RE = re.compile(r"^0x[0-9a-fA-F]{40}$")
NAME_RE = re.compile(r"^([0-9a-f]{40})-([0-9]{1,18})$")
CID_RE = re.compile(r"^b[a-z2-7]{20,100}$")

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
    return "b" + _base32_lower(b"\x01\x55\x12\x20" + hashlib.sha256(data).digest())


def sig_bytes(h):
    """A 65-byte signature from 0x-hex, or None"""
    try:
        b = bytes.fromhex(h[2:] if isinstance(h, str) and h.startswith("0x") else "")
    except ValueError:
        return None
    return b if len(b) == 65 else None


def same(a, b):
    return bool(a) and bool(b) and a.lower() == b.lower()

# ============ Objects on disk ============


class Disk:
    def __init__(self, directory):
        self.dir = directory
        for d in ("c", "b", "a"):
            os.makedirs(os.path.join(directory, d), exist_ok=True)

    def path(self, key):
        return os.path.join(self.dir, key)

    def write(self, key, data: bytes):
        p = self.path(key)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        tmp = "%s.tmp%d.%d" % (p, os.getpid(), threading.get_ident())
        with open(tmp, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, p)

    def read(self, key):
        try:
            with open(self.path(key), "rb") as f:
                return f.read()
        except FileNotFoundError:
            return None

    def json(self, key):
        data = self.read(key)
        return json.loads(data) if data is not None else None

    def set_json(self, key, obj):
        self.write(key, json.dumps(obj, sort_keys=True).encode())

    def delete(self, key):
        try:
            os.remove(self.path(key))
        except FileNotFoundError:
            pass

    def list(self, prefix):
        try:
            return sorted(os.listdir(self.path(prefix)))
        except FileNotFoundError:
            return []

# ============ The chain ============


class ChainUnreadable(Exception):
    """The chain could not be asked (unreachable, or answering an error): not the same as 'no rollup' (#198)"""


class Rollups:
    """Who a DA's rollup names: the sequencer key (bindings, content, prunes - it changes at once when the owner
    rotates it, #208), and the lease (names, #167). None when the DA has no coordinator."""

    def __init__(self, chain):
        self.chain = chain

    def _call(self, to, sig):
        try:
            return self.chain.call("eth_call", {"to": to, "data": "0x" + selector(sig)}, "latest") or ""
        except RpcError as e:
            raise ChainUnreadable(str(e)[:200])
        except (OSError, ValueError) as e:
            raise ChainUnreadable(str(e)[:200])

    def roles(self, da):
        coord = self._call(da, "coordinator()")
        if len(coord) < 66 or int(coord, 16) == 0:
            return None
        c = "0x" + coord[-40:]
        key, wallet, inst = self._call(c, "sequencer()"), self._call(c, "sequencerWallet()"), self._call(c, "sequencerInstance()")
        owner = self._call(c, "owner()")
        return {"sequencer": "0x" + key[-40:], "leaseWallet": "0x" + wallet[-40:], "leaseInstance": inst[:66].lower(),
                "owner": "0x" + owner[-40:]}


class Token:
    """The asset, read and pulled from: balance, allowance, the permit's nonce; permit and transferFrom sent by the
    store's spender, each simulated first so a refusal costs no gas"""

    def __init__(self, chain, asset, spender_key, chain_id, receipt_timeout=60):
        self.chain, self.asset, self.key, self.chain_id = chain, asset, spender_key, chain_id
        self.spender = eth.address(spender_key)
        self.receipt_timeout = receipt_timeout
        self.lock = threading.Lock()                 # the spender's nonces, one transaction at a time

    def _uint(self, sig, *words):
        try:
            return int(self.chain.view(self.asset, sig, *words) or "0x0", 16)
        except RpcError as e:
            raise ChainUnreadable(str(e)[:200])
        except (OSError, ValueError) as e:
            raise ChainUnreadable(str(e)[:200])

    def balance(self, a):
        return self._uint("balanceOf(address)", a)

    def allowance(self, a):
        return self._uint("allowance(address,address)", a, self.spender)

    def nonce(self, a):
        return self._uint("nonces(address)", a)

    @staticmethod
    def permit_data(owner, p):
        sig = sig_bytes(p["signature"])
        v, r, s = sig[64], sig[:32], sig[32:64]
        return "0x" + selector("permit(address,address,uint256,uint256,uint8,bytes32,bytes32)") + "".join(
            eth.word(w).hex() for w in (owner, p["spender"], int(p["value"]), int(p["deadline"]), int(v), r, s))

    def transfer_data(self, owner, to, amount):
        return "0x" + selector("transferFrom(address,address,uint256)") + "".join(eth.word(w).hex() for w in (owner, to, amount))

    def send(self, data):
        """Simulate, then send and wait. ('ok', tx) | ('refused', reason) | ('failed', reason): refused is the chain
        saying no to the call itself (the funder's balance, allowance, permit); failed is anything else"""
        call = {"from": self.spender, "to": self.asset, "data": data}
        try:
            self.chain.call("eth_call", call, "latest")
        except RpcError as e:
            # the call itself reverting is the funder's refusal; the RPC failing (a rate limit, an upstream error) is
            # the store's own trouble, which never parks a bucket (#220)
            if e.data or "revert" in str(e).lower():
                return "refused", revert_reason(e)
            return "failed", "simulation: %s" % revert_reason(e)
        except (OSError, ValueError) as e:
            return "failed", "simulation: %s" % e
        with self.lock:
            try:
                gas = int(self.chain.call("eth_estimateGas", call), 16) * 2
                base = int(self.chain.call("eth_getBlockByNumber", "latest", False).get("baseFeePerGas", "0x0"), 16)
                try:
                    tip = int(self.chain.call("eth_maxPriorityFeePerGas"), 16)
                except RpcError:
                    tip = 0
                nonce = int(self.chain.call("eth_getTransactionCount", self.spender, "pending"), 16)
                raw = signed_tx(self.key, self.chain_id, nonce, tip, 2 * base + tip, gas, self.asset, bytes.fromhex(data[2:]))
                tx = self.chain.call("eth_sendRawTransaction", "0x" + raw.hex())
            except RpcError as e:
                return "failed", "submission: %s" % revert_reason(e)
            except (OSError, ValueError) as e:
                return "failed", "submission: %s" % e
            deadline = time.time() + self.receipt_timeout
            while time.time() < deadline:
                try:
                    rcpt = self.chain.call("eth_getTransactionReceipt", tx)
                except (RpcError, OSError, ValueError):
                    rcpt = None
                if rcpt:
                    if int(rcpt.get("status", "0x0"), 16) == 1:
                        return "ok", tx
                    return "failed", "reverted on chain: %s" % tx   # passed the simulation: the store's to retry
                time.sleep(0.3)
            return "failed", "no receipt for %s" % tx

# ============ The ledger: buckets and accounts ============


class Ledger:
    """Everything that changes money or a bucket's state, under one lock (a Worker: Durable Objects)"""

    def __init__(self, args, disk, signer_key, chain_id, rollups, token):
        self.args, self.disk, self.key, self.chain_id = args, disk, signer_key, chain_id
        self.signer = eth.address(signer_key)
        self.domain = eth.domain_separator("HPPLiteStore", "2", chain_id, args.pay_to)
        self.rollups, self.token = rollups, token
        self.lock = threading.RLock()
        self.warnings = {}
        self.seen = {}          # funder -> (what its permit could pull, when): the last read that worked

    # --- records ---
    def bucket(self, da):
        return self.disk.json("b/%s/bucket.json" % da)

    def save_bucket(self, b):
        self.disk.set_json("b/%s/bucket.json" % b["da"], b)

    def account(self, funder, create=False):
        a = self.disk.json("a/%s.json" % funder.lower())
        if a is None and create:
            a = {"funder": funder.lower(), "credit": 0, "carry": 0.0, "permit": None, "permitSent": False,
                 "lastPull": int(time.time()), "lastTry": 0, "uncovered": False, "why": ""}
        return a

    def save_account(self, a):
        self.disk.set_json("a/%s.json" % a["funder"], a)

    def buckets_of(self, funder):
        out = []
        for da in self.disk.list("b"):
            b = self.bucket(da)
            if b and same(b.get("funder"), funder) and b["state"] != "deleted":
                out.append(b)
        return out

    # --- the funder's daily cap (#274): every new write counted in a rolling day, bounded when a cap is set ---
    def today(self, b, now=None):
        """the bucket's window: {writes, bytes, since}, reset once a day has passed since its first write"""
        now = now or int(time.time())
        t = b.get("today") or {"writes": 0, "bytes": 0, "since": now}
        if now - int(t.get("since", now)) >= DAY:
            t = {"writes": 0, "bytes": 0, "since": now}
        return t

    def capped(self, b, nbytes, now=None):
        """the 429 body when a write of nbytes would pass the bucket's cap, else None"""
        cap = b.get("cap")
        if not cap:
            return None
        now = now or int(time.time())
        t = self.today(b, now)
        w, by = int(cap.get("writesPerDay", 0)), int(cap.get("bytesPerDay", 0))
        if (w and t["writes"] + 1 > w) or (by and t["bytes"] + nbytes > by):
            left = int(t["since"]) + DAY - now
            return {"error": "bucket_capped", "cap": cap, "today": t, "retryAfter": max(left, 1),
                    "detail": "the bucket's daily cap is reached: %d of %s writes, %d of %s bytes today; the window opens again in %d s"
                              % (t["writes"], w or "-", t["bytes"], by or "-", max(left, 1))}
        return None

    def count_write(self, da, nbytes, now=None):
        now = now or int(time.time())
        b = self.bucket(da)
        t = self.today(b, now)
        t["writes"] += 1
        t["bytes"] += nbytes
        b["today"] = t
        self.save_bucket(b)

    @property
    def free(self):
        return self.args.price_per_write == 0 and self.args.price_per_mib_month == 0

    # --- what an account can pay ---
    def rate(self, funder):
        """units per second its buckets cost now"""
        return sum(b["bytes"] for b in self.buckets_of(funder)) * self.args.price_per_mib_month / MIB / MONTH

    def pullable(self, a, now=None):
        """what the permit can still pull: the wallet's balance, the allowance (or the permit not yet sent), before
        its deadline. 0 without a chain or a permit. When the chain cannot be read, what the last read said (#219: a
        failed read must not make the store sign or report less than it knows); None when it never could"""
        now = now or int(time.time())
        p = a.get("permit")
        if not self.token or not p or int(p["deadline"]) <= now:
            return 0
        try:
            bal = self.token.balance(a["funder"])
            allow = self.token.allowance(a["funder"]) if a.get("permitSent") else int(p["value"])
        except ChainUnreadable:
            seen = self.seen.get(a["funder"])
            return seen[0] if seen else None
        self.seen[a["funder"]] = (min(bal, allow), now)
        return min(bal, allow)

    def covered_until(self, a, now=None):
        now = now or int(time.time())
        rate = self.rate(a["funder"])
        p = a.get("permit")
        pull = self.pullable(a, now) or 0
        if rate <= 0:
            if a["credit"] >= 0 or pull >= -a["credit"]:
                return int(p["deadline"]) if p and a["credit"] < 0 else FOREVER
            return now
        t_credit = now + a["credit"] / rate
        if not p or pull <= 0:
            return max(now, int(t_credit)) if a["credit"] >= 0 else now
        t_pull = min(t_credit + pull / rate, int(p["deadline"]))
        return int(max(t_credit, t_pull, now))

    def can_pay(self, a, amount):
        """the write's fee: from credit, or from what a permit can pull"""
        if self.free:
            return True
        if a.get("uncovered"):
            return False
        have = a["credit"]
        if have >= amount:
            return True
        pull = self.pullable(a)
        return pull is not None and have + pull >= amount

    def top_up(self, a, fee):
        """what a 402 on a write asks: the fee and 30 days of the account's storage beyond its credit, at least minPay"""
        month = int(self.rate(a["funder"]) * MONTH + 0.999999)
        return max(self.args.min_pay, fee + month - a["credit"])

    # --- charging ---
    def charge(self, funder, units):
        a = self.account(funder, create=True)
        a["credit"] -= units
        self.save_account(a)
        return a

    def credit(self, funder, units):
        with self.lock:
            a = self.account(funder, create=True)
            a["credit"] += units
            if a["credit"] >= 0 and a.get("uncovered"):
                a["uncovered"], a["why"] = False, ""
            self.save_account(a)
            self.follow(a)
            return a

    def accrue(self, b, now):
        """storage since the bucket's last accrual, to its funder's account (the fraction carried)"""
        since = b.get("accruedAt") or now
        b["accruedAt"] = now
        if not b.get("funder") or b["state"] == "deleted" or now <= since:
            return
        a = self.account(b["funder"], create=True)
        a["carry"] += b["bytes"] * self.args.price_per_mib_month / MIB * (now - since) / MONTH
        whole = int(a["carry"])
        if whole:
            a["credit"] -= whole
            a["carry"] -= whole
        self.save_account(a)

    def follow(self, a, now=None):
        """the account's buckets follow it: covered while it is, parked once it is not (#204)"""
        now = now or int(time.time())
        for b in self.buckets_of(a["funder"]):
            if a.get("uncovered") and b["state"] == "covered":
                b["state"], b["parkedAt"] = "parked", now
                self.save_bucket(b)
            elif not a.get("uncovered") and b["state"] == "parked":
                b["state"], b["parkedAt"] = "covered", None
                self.save_bucket(b)

    def pull(self, a, now):
        """take what is owed. Refused: the account is uncovered and its buckets park. Failed (the store's own
        trouble): nothing changes; a warning"""
        owed = -a["credit"]
        if owed <= 0 or not self.token:
            return
        a["lastTry"] = now
        p = a.get("permit")
        if not p or int(p["deadline"]) <= now:
            self.uncover(a, "no permit" if not p else "the permit's deadline passed", now)
            return
        try:
            allowed = self.token.allowance(a["funder"])
        except ChainUnreadable as e:
            self.warn("pull", "%s: the chain cannot be read: %s" % (a["funder"], e))
            self.save_account(a)
            return
        if allowed < owed and not a.get("permitSent"):
            outcome, why = self.token.send(Token.permit_data(a["funder"], p))
            if outcome == "refused":
                self.uncover(a, "the permit was refused: %s" % why, now)
                return
            if outcome == "failed":
                self.warn("pull", "%s: permit: %s" % (a["funder"], why))
                self.save_account(a)
                return
            a["permitSent"] = True
        outcome, why = self.token.send(self.token.transfer_data(a["funder"], self.args.pay_to, owed))
        if outcome == "refused":
            self.uncover(a, "the pull was refused: %s" % why, now)
        elif outcome == "failed":
            self.warn("pull", "%s: %s" % (a["funder"], why))
            self.save_account(a)
        else:
            a["credit"] += owed
            a["lastPull"], a["uncovered"], a["why"] = now, False, ""
            self.warnings.pop("pull", None)
            self.save_account(a)
            self.follow(a, now)

    def uncover(self, a, why, now):
        a["uncovered"], a["why"] = True, why
        self.save_account(a)
        self.follow(a, now)

    def warn(self, what, text):
        self.warnings[what] = {"at": int(time.time()), "error": text}
        sys.stderr.write("hpplite_store: %s: %s\n" % (what, text))

    # --- the tick: accrue, pull, park, delete, sweep ---
    def tick(self, now=None):
        now = now or int(time.time())
        with self.lock:
            funders = set()
            for da in self.disk.list("b"):
                b = self.bucket(da)
                if not b:
                    continue
                if b["state"] == "parked" and b.get("parkedAt") and now - b["parkedAt"] >= self.args.park_seconds:
                    self.accrue(b, now)
                    self.delete_bucket(b)
                    continue
                if b["state"] != "deleted":
                    self.accrue(b, now)
                    self.save_bucket(b)
                    if b.get("funder"):
                        funders.add(b["funder"].lower())
            for f in funders:
                a = self.account(f, create=True)
                owed = -a["credit"]
                if owed <= 0:
                    continue
                if not a.get("permit"):
                    self.uncover(a, "credit spent and no permit to pull with", now)
                    continue
                if a.get("uncovered"):
                    if now - a.get("lastTry", 0) >= self.args.retry_seconds:
                        self.pull(a, now)
                    continue
                pull = self.pullable(a, now)
                if (owed >= self.args.pull_threshold or now - a.get("lastPull", 0) >= self.args.pull_interval
                        or (pull is not None and owed > pull)):
                    self.pull(a, now)
            self.sweep(now)

    def delete_bucket(self, b):
        da = b["da"]
        for cid in list(b.get("content", {})):
            self.unref(cid, da)
        shutil.rmtree(self.disk.path("b/%s/n" % da), ignore_errors=True)
        b.update({"state": "deleted", "bytes": 0, "objects": 0, "content": {}, "names": {}, "deletedAt": int(time.time())})
        self.save_bucket(b)

    def unref(self, cid, da):
        refs = self.disk.json("c/%s.refs" % cid) or {}
        refs.pop(da, None)
        if refs:
            self.disk.set_json("c/%s.refs" % cid, refs)
        else:
            self.disk.delete("c/%s.refs" % cid)
            self.disk.delete("c/%s" % cid)

    def sweep(self, now):
        """bytes in no bucket (an interrupted put, #200), once they are old enough not to be a put in flight"""
        for f in self.disk.list("c"):
            if f.endswith(".refs") or ".tmp" in f:
                continue
            if self.disk.read("c/%s.refs" % f) is None:
                try:
                    if now - os.path.getmtime(self.disk.path("c/%s" % f)) >= self.args.sweep_age:
                        self.disk.delete("c/%s" % f)
                except FileNotFoundError:
                    pass

    # --- the statement ---
    def statement(self, b, now=None):
        now = now or int(time.time())
        a = self.account(b["funder"]) if b.get("funder") else None
        covered = self.covered_until(a, now) if a and b["state"] == "covered" else 0
        height, prune = b.get("height", 0), b.get("pruneHeight", 0)
        digest = eth.typed_digest(self.domain, STATEMENT_TYPE, b["da"], height, prune, covered, now)
        return {"height": height, "pruneHeight": prune, "coveredUntil": covered, "issuedAt": now,
                "signer": self.signer, "signature": "0x" + eth.sign(self.key, digest).hex()}

# ============ The service ============


class Service:
    def __init__(self, args):
        self.args = args
        self.puts = 0                     # content puts seen (the --fail-put test hook)
        self.disk = Disk(args.dir)
        with open(args.signer_key) as f:
            key = bytes.fromhex(f.read().strip().replace("0x", ""))
        spender_key = key
        if args.spender_key:
            with open(args.spender_key) as f:
                spender_key = bytes.fromhex(f.read().strip().replace("0x", ""))
        self.chain_id = int(args.network.split(":")[1])
        chain = Chain(args.rpc, "hpplite-store/2") if args.rpc else None
        self.rollups = Rollups(chain) if chain else None
        token = Token(chain, args.asset, spender_key, self.chain_id) if chain else None
        self.ledger = Ledger(args, self.disk, key, self.chain_id, self.rollups, token)
        self.payments = Payments(args.facilitator, args.network, args.asset, args.pay_to,
                                 args.asset_name, args.asset_version, args.rpc)
        self.spender = eth.address(spender_key)
        self.started = int(time.time())

    def start(self):
        def loop():
            while True:
                time.sleep(self.args.tick_interval)
                try:
                    self.ledger.tick()
                except (OSError, ValueError) as e:
                    self.ledger.warn("tick", str(e))
        threading.Thread(target=loop, daemon=True).start()

    def status(self):
        a = self.args
        return {
            "service": "hpplite-store", "protocol": a.advertise_protocol or PROTOCOL,
            "signer": self.ledger.signer,
            "domain": {"name": "HPPLiteStore", "version": "2", "chainId": self.chain_id, "verifyingContract": a.pay_to},
            "buckets": {"asset": a.asset, "assetDomain": {"name": a.asset_name, "version": a.asset_version},
                        "payTo": a.pay_to, "spender": self.spender, "pricePerWrite": a.price_per_write,
                        "pricePerMiBMonth": a.price_per_mib_month, "pullThreshold": a.pull_threshold,
                        "pullIntervalSeconds": a.pull_interval, "parkSeconds": a.park_seconds},
            "payment": {"scheme": "exact", "network": a.network, "facilitator": a.facilitator, "minPay": a.min_pay,
                        "extra": {"name": a.asset_name, "version": a.asset_version}} if self.payments.enabled else None,
            "chain": bool(a.rpc), "warnings": self.ledger.warnings, "uptime": int(time.time()) - self.started,
        }

    # --- who may act for a bucket ---
    def roles(self, da):
        """the chain's roles for the DA, or None; raises ChainUnreadable"""
        return self.rollups.roles(da) if self.rollups else None

    def sequencer_of(self, da, b):
        """the key that may bind, write and prune: the chain's sequencer, or (no chain) the bucket's first binder"""
        if self.rollups:
            r = self.roles(da)
            if r is None:
                return None
            return r["sequencer"]
        return b.get("sequencer") if b else None


class Handler(http.server.BaseHTTPRequestHandler):
    service = None  # type: Service

    def log_message(self, fmt, *args):
        if self.service.args.verbose:
            sys.stderr.write("hpplite_store: " + fmt % args + "\n")

    def reply(self, status, obj, headers=None):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def reply_bytes(self, data, headers=None):
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def body_json(self, body):
        try:
            obj = json.loads(body or b"{}")
            return obj if isinstance(obj, dict) else None
        except ValueError:
            return None

    @property
    def L(self):
        return self.service.ledger

    # --- x402: a payment that credits an account ---
    def x402(self, funder, amount, description, work):
        """402 until a payment for `amount` verifies; then the account is credited, the work done, the payment
        settled; a settlement that fails takes the credit back (the work stays, owed)"""
        p = self.service.payments
        requirements = p.requirements(amount, "http://%s%s" % (self.headers.get("Host", "localhost"), self.path), description)
        header = self.headers.get("PAYMENT-SIGNATURE")
        if not header:
            requirements["error"] = getattr(self, "why402", None) or requirements["error"]
            return self.reply(402, requirements, {"PAYMENT-REQUIRED": base64.b64encode(json.dumps(requirements).encode()).decode()})
        payload = p.decode(header)
        if not payload or "payload" not in payload:
            return self.reply(400, {"error": "PAYMENT-SIGNATURE is not a payment payload"})
        v = p.verify(payload, requirements["accepts"][0])
        if not v.get("isValid"):
            requirements["error"] = v.get("invalidReason", "invalid payment")
            return self.reply(402, requirements, {"PAYMENT-REQUIRED": base64.b64encode(json.dumps(requirements).encode()).decode()})
        self.L.credit(funder, amount)
        status, obj = work()
        s = p.settle(payload, requirements["accepts"][0])
        if not s.get("success"):
            self.L.credit(funder, -amount)
            return self.reply(402, {"error": "settlement failed: " + s.get("errorReason", "?"), "settlement": s})
        return self.reply(status, obj, {"PAYMENT-RESPONSE": base64.b64encode(json.dumps(s).encode()).decode()})

    # --- a write into a bucket: covered, or a 402 for a top-up ---
    def write(self, b, nbytes, work):
        if not b or b["state"] == "deleted":
            return self.reply(402, {"error": "bucket_unbound", "detail": "bind the bucket first"})
        if not b.get("funder"):
            return self.reply(402, {"error": "bucket_unbound", "detail": "the bucket has no funder: bind it"})
        fee = self.service.args.price_per_write
        with self.L.lock:
            b = self.L.bucket(b["da"])
            capped = self.L.capped(b, nbytes)
            if capped:
                return self.reply(429, capped, {"Retry-After": str(capped["retryAfter"])})
            a = self.L.account(b["funder"], create=True)
            if b["state"] == "covered" and self.L.can_pay(a, fee):
                if fee:
                    self.L.charge(b["funder"], fee)
                status, obj = work()
                if status == 200 and not obj.get("held"):
                    self.L.count_write(b["da"], nbytes)
                return self.reply(status, obj)
        why = "bucket_parked" if b["state"] == "parked" else "account_uncovered"
        if not self.service.payments.enabled:
            return self.reply(402, {"error": why, "detail": a.get("why") or "the account cannot pay: top it up or give a permit",
                                    "funder": b["funder"]})
        self.why402 = why

        def paid():
            with self.L.lock:
                self.L.charge(b["funder"], fee)
                status, obj = work()
                if status == 200 and not obj.get("held"):
                    self.L.count_write(b["da"], nbytes)
                return status, obj
        return self.x402(b["funder"], self.L.top_up(a, fee), "top up %s's account" % b["funder"], paid)

    # --- GET ---
    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        path = url.path
        if path == "/status":
            return self.reply(200, self.service.status())
        if path.startswith("/bucket/"):
            da = path[8:].lower()
            b = self.L.bucket(da) if ADDR_RE.match(da) else None
            if not b:
                return self.reply(404, {"error": "no such bucket", "da": da})
            a = self.L.account(b["funder"]) if b.get("funder") else None
            out = {k: b.get(k) for k in ("da", "funder", "state", "bytes", "objects", "height", "pruneHeight", "parkedAt")}
            out["coveredUntil"] = self.L.covered_until(a) if a and b["state"] == "covered" else 0
            out["statement"] = self.L.statement(b) if b["state"] != "deleted" else None
            out["cap"], out["today"] = b.get("cap"), self.L.today(b)
            return self.reply(200, out)
        if path.startswith("/account/"):
            funder = path[9:].lower()
            a = self.L.account(funder) if ADDR_RE.match(funder) else None
            if not a:
                return self.reply(404, {"error": "no such account", "funder": funder})
            return self.reply(200, self.account_view(a))
        if path.startswith("/name/"):
            m = NAME_RE.match(path[6:])
            if not m:
                return self.reply(404, {"error": "no such name"})
            da, height = "0x" + m.group(1), int(m.group(2))
            b = self.L.bucket(da)
            rec = self.service.disk.read("b/%s/n/%d" % (da, height)) if b and b["state"] != "deleted" else None
            data = self.service.disk.read("c/%s" % rec[97:].decode()) if rec else None
            if data is None:
                return self.reply(404, {"error": "no such name", "name": path[6:]})
            if b["state"] == "parked":
                return self.reply(410, {"error": "bucket_parked", "da": da, "funder": b.get("funder"),
                                        "detail": "the bucket's account cannot pay: paying restores it"})
            signer = eth.recover(batch_digest(self.service.chain_id, da, "0x" + rec[:32].hex(), height, data), rec[32:97])
            return self.reply_bytes(data + rec[:97], {"X-HPPLite-Signer": signer})
        if path.startswith("/get/"):
            cid = path[5:]
            refs = self.service.disk.json("c/%s.refs" % cid) if CID_RE.match(cid) else None
            data = self.service.disk.read("c/%s" % cid) if refs else None
            if data is None:
                return self.reply(404, {"error": "not held here", "cid": cid})
            states = [(self.L.bucket(da) or {}).get("state") for da in refs]
            if "covered" not in states:
                return self.reply(410, {"error": "bucket_parked", "cid": cid})
            return self.reply_bytes(data)
        return self.reply(404, {"error": "no such endpoint"})

    def account_view(self, a):
        L = self.L
        try:
            bal = L.token.balance(a["funder"]) if L.token else None
            allow = L.token.allowance(a["funder"]) if L.token else None
            nonce = L.token.nonce(a["funder"]) if L.token else 0
        except ChainUnreadable:
            bal = allow = nonce = None
        p = a.get("permit")
        return {"funder": a["funder"], "credit": a["credit"], "balance": bal, "allowance": allow,
                "permit": {"value": p["value"], "deadline": p["deadline"], "sent": a.get("permitSent", False)} if p else None,
                "permitNonce": nonce,
                "dailyCost": int(L.rate(a["funder"]) * 86400 + 0.999999), "coveredUntil": L.covered_until(a),
                "buckets": [b["da"] for b in L.buckets_of(a["funder"])],
                "state": "uncovered" if a.get("uncovered") else "covered", "why": a.get("why", "")}

    # --- POST ---
    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        path, query = url.path, urllib.parse.parse_qs(url.query)
        n = int(self.headers.get("Content-Length") or 0)
        if n > self.service.args.max_bytes + 4096:
            return self.reply(413, {"error": "too large", "max": self.service.args.max_bytes})
        body = self.rfile.read(n) if n else b""
        try:
            if path.startswith("/name/"):
                return self.post_name(path[6:], body)
            if path == "/put":
                return self.post_put(query, body)
            m = re.match(r"^/bucket/(0x[0-9a-fA-F]{40})(/release|/prune|/cap)?$", path)
            if m:
                da = m.group(1).lower()
                if m.group(2) == "/release":
                    return self.post_release(da, body)
                if m.group(2) == "/prune":
                    return self.post_prune(da, body)
                if m.group(2) == "/cap":
                    return self.post_cap(da, body)
                return self.post_bind(da, body)
            m = re.match(r"^/account/(0x[0-9a-fA-F]{40})/(permit|pay)$", path)
            if m:
                funder = m.group(1).lower()
                return self.post_permit(funder, body) if m.group(2) == "permit" else self.post_pay(funder, query)
        except ChainUnreadable as e:
            return self.reply(503, {"error": "the chain cannot be read to check the rollup: %s" % e})
        return self.reply(404, {"error": "no such endpoint"})

    def fresh(self, issued, last):
        """an issuedAt newer than the last taken, at most an hour ahead"""
        return isinstance(issued, int) and issued > (last or 0) and issued <= int(time.time()) + 3600

    def post_bind(self, da, body):
        req = self.body_json(body)
        if not req or not ADDR_RE.match(str(req.get("funder", ""))) or not isinstance(req.get("issuedAt"), int):
            return self.reply(400, {"error": "a binding is {funder, issuedAt, sequencerSignature, funderSignature}"})
        funder, issued = req["funder"].lower(), req["issuedAt"]
        digest = eth.typed_digest(self.L.domain, BIND_TYPE, da, funder, issued)
        s_sig, f_sig = sig_bytes(req.get("sequencerSignature")), sig_bytes(req.get("funderSignature"))
        signer = eth.recover(digest, s_sig) if s_sig else ""
        if not f_sig or not same(eth.recover(digest, f_sig), funder):
            return self.reply(401, {"error": "the funder's signature does not recover to the funder"})
        with self.L.lock:
            b = self.L.bucket(da)
            if self.service.rollups:
                r = self.service.roles(da)
                if r is None:
                    return self.reply(404, {"error": "no HPPLite rollup at this DA", "da": da})
                seq = r["sequencer"]
                if not s_sig and same(funder, r["owner"]):
                    signer = seq          # the funder alone, being the rollup's owner on chain (#273): its authority covers the sequencer's consent
            else:
                seq = b.get("sequencer") if b and b["state"] != "deleted" else signer   # no chain: the first binder
            if not same(signer, seq):
                return self.reply(401, {"error": "the sequencer's signature does not recover to the rollup's sequencer",
                                        "signer": signer, "sequencer": seq})
            if b and b["state"] != "deleted" and not self.fresh(issued, b.get("boundAt")):
                return self.reply(409, {"error": "a binding no newer than the last taken", "last": b.get("boundAt")})
            now = int(time.time())
            if not b or b["state"] == "deleted":
                b = {"da": da, "bytes": 0, "objects": 0, "height": 0, "pruneHeight": 0, "content": {}, "names": {},
                     "created": now, "accruedAt": now, "pruneAt": 0}
            else:
                self.L.accrue(b, now)                   # what accrued so far stays with the old funder
            if b.get("funder") and not same(b.get("funder"), funder):
                b["cap"], b["capAt"] = None, 0                 # the cap goes with the funder (#274)
            b.update({"funder": funder, "sequencer": signer.lower(), "boundAt": issued, "state": "covered", "parkedAt": None})
            self.L.save_bucket(b)
            a = self.L.account(funder, create=True)
            self.L.save_account(a)
            if a.get("uncovered") or (not self.L.free and a["credit"] < 0 and not a.get("permit")):
                b["state"], b["parkedAt"] = "parked", now
                self.L.save_bucket(b)
            return self.reply(200, {"da": da, "funder": funder, "state": b["state"]})

    def post_cap(self, da, body):
        req = self.body_json(body)
        try:
            writes, nbytes, issued = int(req["writesPerDay"]), int(req["bytesPerDay"]), int(req["issuedAt"])
            funder = str(req["funder"]).lower()
        except (KeyError, TypeError, ValueError):
            return self.reply(400, {"error": "a cap is {funder, writesPerDay, bytesPerDay, issuedAt, signature}"})
        if writes < 0 or nbytes < 0 or not ADDR_RE.match(funder):
            return self.reply(400, {"error": "a cap is {funder, writesPerDay, bytesPerDay, issuedAt, signature}"})
        with self.L.lock:
            b = self.L.bucket(da)
            if not b or b["state"] == "deleted" or not b.get("funder"):
                return self.reply(404, {"error": "no bound bucket", "da": da})
            digest = eth.typed_digest(self.L.domain, CAP_TYPE, da, writes, nbytes, issued)
            sig = sig_bytes(req.get("signature"))
            if not sig or not same(funder, b["funder"]) or not same(eth.recover(digest, sig), b["funder"]):
                return self.reply(401, {"error": "not signed by the bucket's funder"})
            if not self.fresh(issued, b.get("capAt")):
                return self.reply(409, {"error": "a cap no newer than the last taken", "last": b.get("capAt")})
            b["cap"] = {"writesPerDay": writes, "bytesPerDay": nbytes} if (writes or nbytes) else None
            b["capAt"] = issued
            self.L.save_bucket(b)
            return self.reply(200, {"da": da, "cap": b["cap"], "today": self.L.today(b)})

    def post_release(self, da, body):
        req = self.body_json(body)
        if not req or not isinstance(req.get("issuedAt"), int):
            return self.reply(400, {"error": "a release is {funder, issuedAt, signature}"})
        with self.L.lock:
            b = self.L.bucket(da)
            if not b or b["state"] == "deleted" or not b.get("funder"):
                return self.reply(404, {"error": "no bound bucket", "da": da})
            delete = req.get("delete") is True                      # delete now (#275): signed over Delete, not Release
            digest = eth.typed_digest(self.L.domain, DELETE_TYPE if delete else RELEASE_TYPE, da, b["funder"], req["issuedAt"])
            sig = sig_bytes(req.get("signature"))
            if not sig or not same(eth.recover(digest, sig), b["funder"]) or not same(req.get("funder"), b["funder"]):
                return self.reply(401, {"error": "not signed by the bucket's funder" + (" over Delete(da, funder, issuedAt)" if delete else "")})
            if not self.fresh(req["issuedAt"], b.get("boundAt")):
                return self.reply(409, {"error": "a release no newer than the binding"})
            now = int(time.time())
            self.L.accrue(b, now)
            b.update({"funder": None, "boundAt": req["issuedAt"], "state": "parked", "parkedAt": now})
            self.L.save_bucket(b)
            if delete:
                self.L.delete_bucket(b)                          # what it owed stays booked to the account that released it
                return self.reply(200, {"da": da, "funder": None, "state": "deleted"})
            return self.reply(200, {"da": da, "funder": None, "state": "parked"})

    def check_sequencer(self, da, b, signer):
        """(status, error) when `signer` may not act for the bucket, else None"""
        seq = self.service.sequencer_of(da, b)
        if not same(signer, seq):
            return 403, {"error": "not the rollup's sequencer", "signer": signer, "sequencer": seq}
        return None

    def post_name(self, name, body):
        m = NAME_RE.match(name)
        if not m or len(body) <= 97 or len(body) > self.service.args.max_bytes + 97:
            return self.reply(400, {"error": "a name is <da>-<height>, the body the bytes, a 32-byte instance and a 65-byte signature"})
        da, height = "0x" + m.group(1), int(m.group(2))
        instance, data = "0x" + body[-97:-65].hex(), body[:-97]
        signer = eth.recover(batch_digest(self.service.chain_id, da, instance, height, data), body[-65:])
        if not signer:
            return self.reply(400, {"error": "the signature does not recover"})
        b = self.L.bucket(da)
        if not b or b["state"] == "deleted":
            return self.reply(402, {"error": "bucket_unbound", "detail": "bind the rollup's bucket first"})
        bad = self.check_sequencer(da, b, signer)
        if bad:
            return self.reply(*bad)
        roles = self.service.roles(da)
        if roles and (not same(roles["leaseWallet"], signer) or roles["leaseInstance"] != instance.lower()):
            # the same wallet, another process: a sequencer that lost the lease (#167) must not write over the names
            # of the one the chain now names
            return self.reply(409, {"error": "not the instance the rollup's lease names", "instance": instance,
                                    "lease": roles["leaseInstance"]})
        key = "b/%s/n/%d" % (da, height)
        cid = cid_of(data)
        rec = body[-97:] + cid.encode()
        if self.service.disk.read(key) == rec:
            return self.reply(200, {"name": name, "bytes": len(data), "signer": signer, "held": True})

        def work():
            self.hold(da, cid, data, height)
            b2 = self.L.bucket(da)
            if self.service.disk.read(key) is None:
                b2["bytes"] += 97
                b2["objects"] += 1
            self.service.disk.write(key, rec)
            b2["height"] = max(b2.get("height", 0), height)
            self.L.save_bucket(b2)
            return 200, {"name": name, "bytes": len(data), "signer": signer, "cid": cid}
        return self.write(b, len(body), work)

    def hold(self, da, cid, data, height):
        """the bytes in the bucket, once: written first, then the reference (without one, a sweep takes them)"""
        b = self.L.bucket(da)
        if cid in b.get("content", {}):
            return
        if self.service.disk.read("c/%s" % cid) is None:
            self.service.disk.write("c/%s" % cid, data)
        refs = self.service.disk.json("c/%s.refs" % cid) or {}
        refs[da] = height
        self.service.disk.set_json("c/%s.refs" % cid, refs)
        b.setdefault("content", {})[cid] = {"height": height, "bytes": len(data)}
        b["bytes"] += len(data)
        b["objects"] += 1
        self.L.save_bucket(b)

    def post_put(self, query, body):
        da = (query.get("bucket") or [""])[0].lower()
        self.service.puts += 1
        if self.service.args.fail_put and self.service.puts == self.service.args.fail_put:
            return self.reply(503, {"error": "test hook: this put is refused (--fail-put)"})
        try:
            height = int((query.get("height") or [""])[0])
        except ValueError:
            height = -1
        if not ADDR_RE.match(da) or height < 0 or not body:
            return self.reply(400, {"error": "a put is /put?bucket=<da>&height=<n> with the bytes and X-HPPLite-Signature"})
        if len(body) > self.service.args.max_bytes:
            return self.reply(413, {"error": "too large", "max": self.service.args.max_bytes})
        sig = sig_bytes(self.headers.get("X-HPPLite-Signature"))
        signer = eth.recover(eth.typed_digest(self.L.domain, CONTENT_TYPE, da, height, eth.keccak256(body)), sig) if sig else ""
        if not signer:
            return self.reply(401, {"error": "X-HPPLite-Signature: the sequencer's Content signature"})
        b = self.L.bucket(da)
        if not b or b["state"] == "deleted":
            return self.reply(402, {"error": "bucket_unbound", "detail": "bind the rollup's bucket first"})
        bad = self.check_sequencer(da, b, signer)
        if bad:
            return self.reply(*bad)
        cid = cid_of(body)
        if cid in b.get("content", {}):
            return self.reply(200, {"cid": cid, "bytes": len(body), "held": True})

        def work():
            self.hold(da, cid, body, height)
            return 200, {"cid": cid, "bytes": len(body), "held": False}
        return self.write(b, len(body), work)

    def post_prune(self, da, body):
        req = self.body_json(body)
        if not req or not isinstance(req.get("height"), int) or not isinstance(req.get("issuedAt"), int):
            return self.reply(400, {"error": "a prune is {height, issuedAt, signature}"})
        sig = sig_bytes(req.get("signature"))
        signer = eth.recover(eth.typed_digest(self.L.domain, PRUNE_TYPE, da, req["height"], req["issuedAt"]), sig) if sig else ""
        with self.L.lock:
            b = self.L.bucket(da)
            if not b or b["state"] == "deleted":
                return self.reply(404, {"error": "no such bucket", "da": da})
            bad = self.check_sequencer(da, b, signer)
            if bad:
                return self.reply(401 if not signer else bad[0], bad[1])
            if not self.fresh(req["issuedAt"], b.get("pruneAt")):
                return self.reply(409, {"error": "a prune no newer than the last taken"})
            h = req["height"]
            for f in self.service.disk.list("b/%s/n" % da):
                if f.isdigit() and int(f) < h:
                    self.service.disk.delete("b/%s/n/%s" % (da, f))
                    b["bytes"] -= 97
                    b["objects"] -= 1
            for cid, e in list(b.get("content", {}).items()):
                if e["height"] < h:
                    self.L.unref(cid, da)
                    del b["content"][cid]
                    b["bytes"] -= e["bytes"]
                    b["objects"] -= 1
            self.L.accrue(b, int(time.time()))
            b["pruneHeight"], b["pruneAt"] = max(b.get("pruneHeight", 0), h), req["issuedAt"]
            self.L.save_bucket(b)
            return self.reply(200, {"da": da, "pruneHeight": b["pruneHeight"], "bytes": b["bytes"]})

    def post_permit(self, funder, body):
        req = self.body_json(body)
        try:
            value, deadline = int(req["value"]), int(req["deadline"])
            sig = sig_bytes(req["signature"]) if "signature" in req else \
                bytes.fromhex(req["r"][2:]) + bytes.fromhex(req["s"][2:]) + bytes([int(req["v"])])
        except (KeyError, TypeError, ValueError):
            return self.reply(400, {"error": "a permit is {value, deadline, signature} (or v, r, s)"})
        if not sig or len(sig) != 65 or deadline <= int(time.time()):
            return self.reply(400, {"error": "a permit with a 65-byte signature and a deadline ahead"})
        L = self.L
        if L.token:
            try:
                nonce = L.token.nonce(funder)
            except ChainUnreadable as e:
                return self.reply(503, {"error": "the chain cannot be read: %s" % e})
            domain = eth.domain_separator(self.service.args.asset_name, self.service.args.asset_version,
                                          self.service.chain_id, self.service.args.asset)
            digest = eth.typed_digest(domain, PERMIT_TYPE, funder, L.token.spender, value, nonce, deadline)
            if not same(eth.recover(digest, sig), funder):
                return self.reply(401, {"error": "the permit does not recover to the funder (spender, nonce, domain?)",
                                        "spender": L.token.spender, "nonce": nonce})
        with L.lock:
            a = L.account(funder, create=True)
            a["permit"] = {"value": value, "deadline": deadline, "spender": L.token.spender if L.token else self.service.spender,
                           "signature": "0x" + sig.hex()}
            a["permitSent"] = False
            if a.get("uncovered"):
                a["lastTry"] = 0            # try a pull at the next tick
            L.save_account(a)
            return self.reply(200, self.account_view(a))

    def post_pay(self, funder, query):
        try:
            amount = int((query.get("amount") or ["0"])[0])
        except ValueError:
            amount = 0
        if not self.service.payments.enabled:
            return self.reply(404, {"error": "this store takes no x402 payments: give a permit"})
        if amount < self.service.args.min_pay:
            return self.reply(400, {"error": "amount below minPay", "minPay": self.service.args.min_pay})

        def work():
            return 200, self.account_view(self.L.account(funder, create=True))
        return self.x402(funder, amount, "top up %s's account" % funder, work)


def write_quote(nbytes, per_write, per_mib_month) -> int:
    """what a node quotes for a write (store_price): the fee and a month of the bytes' storage, rounded up to a whole
    unit, in IEEE doubles in this order (the node's hpplite_store_quote; the vectors)"""
    import math
    return per_write + math.ceil(per_mib_month * nbytes / MIB * 1.0 - 1e-9)


def batch_digest(chain_id, da, instance, height, data):
    """What HPPLiteDA.batchDigest() computes (DA version 3): the sequencer's claim about one height."""
    domain = eth.domain_separator("HPPLiteDA", "3", chain_id, da)
    return eth.typed_digest(domain, BATCH_TYPE, bytes.fromhex(instance[2:]), height, eth.keccak256(data))


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser(description="HPPLite reference store, hpplite-store/2 (#213)")
    ap.add_argument("--port", type=int, default=8420)
    ap.add_argument("--dir", default="./hpplite_store")
    ap.add_argument("--signer-key", required=True, help="file with the hex key that signs bucket statements")
    ap.add_argument("--spender-key", default=None, help="file with the hex key that pulls (holds a little ETH); the signer's by default")
    ap.add_argument("--pay-to", required=True, help="where payments land; also the store domain's verifyingContract")
    ap.add_argument("--rpc", default=None, help="the chain: rollups' sequencers and leases, the asset, pulls")
    ap.add_argument("--facilitator", default=None, help="x402 facilitator URL for top-ups; none: permit pulls only")
    ap.add_argument("--network", default="eip155:181228")
    ap.add_argument("--asset", default="0x401eCb1D350407f13ba348573E5630B83638E30D", help="the token (USDC.e on HPP): EIP-3009 and EIP-2612")
    ap.add_argument("--asset-name", default="Bridged USDC")
    ap.add_argument("--asset-version", default="2")
    ap.add_argument("--price-per-mib-month", type=float, default=30.0, help="atomic units per MiB per 30 days ($0.03 per GiB-month)")
    ap.add_argument("--price-per-write", type=int, default=10, help="atomic units per name or content put")
    ap.add_argument("--min-pay", type=int, default=1000000, help="the smallest x402 top-up (atomic units)")
    ap.add_argument("--pull-threshold", type=int, default=1000000, help="pull once this much is owed")
    ap.add_argument("--pull-interval", type=int, default=MONTH, help="or this long after the last pull")
    ap.add_argument("--retry-seconds", type=int, default=24 * 3600, help="how often an uncovered account's pull is tried again")
    ap.add_argument("--park-seconds", type=int, default=MONTH, help="a parked bucket is deleted after this long")
    ap.add_argument("--tick-interval", type=float, default=3600, help="seconds between ticks: accrue, pull, park, delete, sweep")
    ap.add_argument("--sweep-age", type=int, default=3600, help="bytes in no bucket older than this are swept")
    ap.add_argument("--max-bytes", type=int, default=16 * 1024 * 1024)
    ap.add_argument("--advertise-protocol", default=None, help=argparse.SUPPRESS)
    ap.add_argument("--fail-put", type=int, default=0, help=argparse.SUPPRESS)   # a test hook: the k-th content put answers 503
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    args.pay_to = args.pay_to.lower()
    Handler.service = Service(args)
    Handler.service.start()
    srv = Server(("127.0.0.1", args.port), Handler)
    print("hpplite_store: listening on 127.0.0.1:%d, signer %s, %s" % (
        args.port, Handler.service.ledger.signer, "chain " + args.rpc if args.rpc else "no chain (development)"), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
