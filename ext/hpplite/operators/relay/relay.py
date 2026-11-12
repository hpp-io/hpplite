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
                 is answered 422 with the reason and costs nothing. Then 402 with the price (the estimated
                 gas at the current fee, a margin, converted at --eth-usd, at least --min-fee; the quote
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
import re
import socketserver
import sys
import threading
import time
import urllib.parse

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, ".."))
from x402seller import eth  # noqa: E402
from x402seller.chain import Chain, RpcError, revert_reason, selector, signed_tx  # noqa: E402
from x402seller.payments import Payments  # noqa: E402
from x402seller.token import ChainUnreadable, Token, sig_bytes  # noqa: E402


FACTORY_CALLS = {selector("createRollupFor(address,uint256,uint256,uint256,bytes)"): "createRollupFor"}
# VERSION 11 (hpplite-fleet/1, #297, #313): the fleet factory's signed create and a fleet's signed calls
FLEET_FACTORY_CALLS = {selector("createFleetFor(address,address,uint64,uint256,uint256,bytes)"): "createFleetFor"}
FLEET_DB_CALLS = {selector(s): s.split("(")[0] for s in (            # sponsored per database: the dbId is the first word
    "joinFor(bytes32,address,address,bytes32,uint256,bytes,bytes32,uint256,uint256,bytes)",
    "claimFor(bytes32,bytes32,uint256,uint256,bytes)", "retireFor(bytes32,uint256,bytes)",
    "expireFor(bytes32,uint256,uint256,bytes)")}
FLEET_ROOT_CALLS = {selector("postRootFor(uint256,bytes32,bytes32,uint256,bytes)"): "postRootFor"}   # sponsored per fleet
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
# accounts and sponsorship (#276): a funder pays the relay for the rollups it names, in the relay's EIP-712 domain
SPONSOR_TYPE = "Sponsor(address rollup,address funder,uint256 issuedAt)"
UNSPONSOR_TYPE = "Unsponsor(address rollup,address funder,uint256 issuedAt)"
CAP_TYPE = "Cap(address rollup,uint256 callsPerDay,uint256 issuedAt)"
# a fleet database's sponsorship (hpplite-fleet/1, #297, #312): today's three over (fleet, dbId); a fleet's roots are
# sponsored with Sponsor and Cap themselves, the fleet in place of the rollup
SPONSORDB_TYPE = "SponsorDb(address fleet,bytes32 dbId,address funder,uint256 issuedAt)"
UNSPONSORDB_TYPE = "UnsponsorDb(address fleet,bytes32 dbId,address funder,uint256 issuedAt)"
CAPDB_TYPE = "CapDb(address fleet,bytes32 dbId,uint256 callsPerDay,uint256 issuedAt)"
PERMIT_TYPE = "Permit(address owner,address spender,uint256 value,uint256 nonce,uint256 deadline)"
ADDR_RE = re.compile(r"^0x[0-9a-fA-F]{40}$")
DAY = 86400


def same(a, b):
    return bool(a) and bool(b) and a.lower() == b.lower()


class Accounts:
    """Funders' accounts and the rollups they sponsor (#276), as JSON files under --dir. An account is credited by
    x402 top-ups or by pulls from the funder's EIP-2612 permit; a sponsored rollup's calls are charged to it."""

    def __init__(self, directory, token, pay_to, min_pull):
        self.dir, self.token, self.pay_to, self.min_pull = directory, token, pay_to, min_pull
        self.lock = threading.Lock()
        for sub in ("a", "s"):
            os.makedirs(os.path.join(directory, sub), exist_ok=True)

    def _path(self, sub, key):
        return os.path.join(self.dir, sub, key.lower() + ".json")

    def _load(self, sub, key):
        try:
            with open(self._path(sub, key)) as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def _save(self, sub, key, obj):
        tmp = self._path(sub, key) + ".tmp"
        with open(tmp, "w") as f:
            json.dump(obj, f)
        os.replace(tmp, self._path(sub, key))

    def account(self, funder, create=False):
        a = self._load("a", funder)
        if a is None and create:
            a = {"funder": funder.lower(), "credit": 0, "permit": None, "permitSent": False, "uncovered": False, "why": "", "lastPull": 0}
        return a

    def rollups_of(self, funder):
        out = []
        for name in sorted(os.listdir(os.path.join(self.dir, "s"))):
            if name.endswith(".json"):
                s = self._load("s", name[:-5])
                if s and same(s.get("funder"), funder) and not s.get("dbId"):
                    out.append(s["rollup"])
        return out

    def databases_of(self, funder):
        out = []
        for name in sorted(os.listdir(os.path.join(self.dir, "s"))):
            if name.endswith(".json"):
                s = self._load("s", name[:-5])
                if s and same(s.get("funder"), funder) and s.get("dbId"):
                    out.append({"fleet": s["fleet"], "dbId": s["dbId"]})
        return out

    @staticmethod
    def fresh(issued, last):
        return isinstance(issued, int) and issued > int(last or 0) and issued <= int(time.time()) + 3600

    def today(self, s, now=None):
        now = now or int(time.time())
        t = s.get("today") or {"calls": 0, "since": now}
        if now - int(t.get("since", now)) >= DAY:
            t = {"calls": 0, "since": now}
        return t

    def sponsor(self, rollup, da, funder, issued, extra=None):
        with self.lock:
            s = self._load("s", rollup) or dict({"rollup": rollup.lower(), "da": da, "funder": None, "sponsoredAt": 0, "cap": None, "capAt": 0},
                                                **(extra or {}))
            if not self.fresh(issued, s.get("sponsoredAt")):
                return 409, {"error": "a sponsorship no newer than the last taken", "last": s.get("sponsoredAt")}
            if s.get("funder") and not same(s["funder"], funder):
                s["cap"], s["capAt"] = None, 0                 # the cap goes with the funder
            s.update({"funder": funder.lower(), "da": da, "sponsoredAt": issued})
            self._save("s", rollup, s)
            self._save("a", funder, self.account(funder, create=True))
            return 200, self.view_sponsor(rollup)

    def release(self, rollup, funder, issued):
        with self.lock:
            s = self._load("s", rollup)
            if not s or not s.get("funder"):
                return 404, {"error": "no sponsored rollup", "rollup": rollup}
            if not same(s["funder"], funder):
                return 401, {"error": "not signed by the rollup's sponsor"}
            if not self.fresh(issued, s.get("sponsoredAt")):
                return 409, {"error": "a release no newer than the sponsorship"}
            s.update({"funder": None, "sponsoredAt": issued})
            self._save("s", rollup, s)
            return 200, self.view_sponsor(rollup)

    def cap(self, rollup, funder, calls, issued):
        with self.lock:
            s = self._load("s", rollup)
            if not s or not s.get("funder"):
                return 404, {"error": "no sponsored rollup", "rollup": rollup}
            if not same(s["funder"], funder):
                return 401, {"error": "not signed by the rollup's sponsor"}
            if not self.fresh(issued, s.get("capAt")):
                return 409, {"error": "a cap no newer than the last taken", "last": s.get("capAt")}
            s["cap"], s["capAt"] = ({"callsPerDay": calls} if calls else None), issued
            self._save("s", rollup, s)
            return 200, self.view_sponsor(rollup)

    def give_permit(self, funder, value, deadline, sig):
        with self.lock:
            a = self.account(funder, create=True)
            a["permit"] = {"value": value, "deadline": deadline, "spender": self.token.spender, "signature": "0x" + sig.hex()}
            a["permitSent"], a["uncovered"], a["why"] = False, False, ""
            self._save("a", funder, a)
            return a

    def credit(self, funder, amount):
        with self.lock:
            a = self.account(funder, create=True)
            a["credit"] += amount
            if a["credit"] >= 0:
                a["uncovered"], a["why"] = False, ""
            self._save("a", funder, a)
            return a

    def cover(self, rollup, amount, counted=True):
        """What the book says to a call on a rollup: (None, None) when nobody sponsors it; (funder, None) when its
        sponsor's account paid `amount` for one more call today - from its credit, or pulling at least min_pull from
        the funder's permit first; (funder, why) when it cannot. The price is taken here, before the call is sent:
        refund() when the call does not land"""
        now = int(time.time())
        with self.lock:
            s = self._load("s", rollup) if rollup else None
            if not s or not s.get("funder"):
                return None, None
            funder = s["funder"]
            t = self.today(s, now)
            cap = (s.get("cap") or {}).get("callsPerDay", 0) if counted else 0
            if cap and t["calls"] + 1 > cap:
                left = int(t["since"]) + DAY - now
                return funder, "capped: %d of %d calls today; the window opens again in %d s" % (t["calls"], cap, max(left, 1))
            a = self.account(funder, create=True)
            if a.get("uncovered"):
                return funder, "the sponsor's account is uncovered: %s" % a.get("why")
            if a["credit"] < amount:
                why = self._pull(a, amount, now)
                self._save("a", funder, a)
                if why:
                    return funder, why
            a["credit"] -= amount
            if counted:
                t["calls"] += 1
            s["today"] = t
            self._save("a", funder, a)
            self._save("s", rollup, s)
            return funder, None

    def refund(self, rollup, funder, amount, uncount=True):
        """the call did not land: nothing is charged for it, and it does not count in the day. uncount=False: the part
        of the price a call that landed did not use (#294), the day still counting it"""
        with self.lock:
            a = self.account(funder, create=True)
            a["credit"] += amount
            self._save("a", funder, a)
            s = self._load("s", rollup)
            if uncount and s and same(s.get("funder"), funder) and (s.get("today") or {}).get("calls", 0) > 0:
                s["today"]["calls"] -= 1
                self._save("s", rollup, s)

    def _pull(self, a, amount, now):
        """Pull what the call needs past the credit, at least min_pull: the permit first, when the asset does not yet
        allow the pull. None when the credit now covers the call; else why not. A refusal by the chain leaves the
        account uncovered until a new permit or a top-up; the relay's own trouble (the RPC, a transaction that did not
        land) does not"""
        p = a.get("permit")
        if not p or int(p["deadline"]) <= now:
            return "the sponsor's account holds %d and has no permit to pull from" % a["credit"]
        pull = max(self.min_pull, amount - a["credit"])

        def refused(why):
            a["uncovered"], a["why"] = True, why
            return why
        try:
            if not a.get("permitSent"):
                if self.token.allowance(a["funder"]) < pull:
                    outcome, why = self.token.send(self.token.permit_data(a["funder"], p))
                    if outcome == "refused":
                        return refused("the permit was refused: " + why)
                    if outcome != "ok":
                        return "the permit could not be sent: " + why
                a["permitSent"] = True
            outcome, why = self.token.send(self.token.transfer_data(a["funder"], self.pay_to, pull))
        except ChainUnreadable as e:
            return "the chain cannot be read: %s" % e
        if outcome == "refused":
            return refused("the pull was refused: " + why)
        if outcome != "ok":
            return "the pull could not be sent: " + why
        a["credit"] += pull
        a["lastPull"] = now
        return None

    def view_account(self, funder):
        a = self.account(funder)
        if not a:
            return None
        try:
            bal, allow, nonce = self.token.balance(funder), self.token.allowance(funder), self.token.nonce(funder)
        except ChainUnreadable:
            bal = allow = nonce = None
        p = a.get("permit")
        return {"funder": a["funder"], "credit": a["credit"], "balance": bal, "allowance": allow, "permitNonce": nonce,
                "permit": {"value": p["value"], "deadline": p["deadline"], "sent": a.get("permitSent", False)} if p else None,
                "rollups": self.rollups_of(funder), "databases": self.databases_of(funder), "state": "uncovered" if a.get("uncovered") else "covered", "why": a.get("why", "")}

    def view_sponsor(self, rollup):
        s = self._load("s", rollup)
        if not s:
            return None
        if s.get("dbId"):
            return {"fleet": s["fleet"], "dbId": s["dbId"], "funder": s.get("funder"), "cap": s.get("cap"), "today": self.today(s),
                    "state": "sponsored" if s.get("funder") else "released"}
        return {"rollup": s["rollup"], "da": s.get("da"), "funder": s.get("funder"), "cap": s.get("cap"), "today": self.today(s),
                "state": "sponsored" if s.get("funder") else "released"}


def db_key(fleet, db):
    """the book's key for a fleet database's sponsorship (#313): apart from every rollup's and fleet's"""
    return "%s-%s" % (fleet.lower(), db.lower())



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

class LockHeld:
    """a lock already acquired, released at the block's end"""
    def __init__(self, lock):
        self.lock = lock

    def __enter__(self):
        return self.lock

    def __exit__(self, *exc):
        self.lock.release()


class Relay:
    def __init__(self, args):
        self.args = args
        self.chain = Chain(args.rpc, "hpplite-relay/1")
        with open(args.key_file) as f:
            self.key = bytes.fromhex(f.read().strip().replace("0x", ""))
        self.address = eth.address(self.key)
        self.chain_id = int(self.chain.call("eth_chainId"), 16)
        self.factory = args.factory.lower()
        self.fleet_factory = (args.fleet_factory or "").lower() or None   # VERSION 11 (#313); none: no fleet calls
        self.busy_commits = getattr(args, "busy_commits", 0)             # the test hook's count left
        self.per_minute = getattr(args, "per_minute", 0)                 # POSTs a minute from one address (0: no limit)
        self.posts = {}                                                   # address -> the times of its POSTs in the last minute
        self.posts_lock = threading.Lock()
        pay_to = args.pay_to or self.address
        self.payments = Payments(args.facilitator, "eip155:%d" % self.chain_id, args.asset, pay_to,
                                 args.asset_name, args.asset_version, args.rpc)
        self.lock = threading.Lock()          # one transaction at a time on the relay's key: its nonce
        # the 402's price holds for the paid retry with nothing remembered between them (#184): the quote travels in
        # the offer's extra, MACed under a key derived from the relay's own, so any process holding that key honours it
        self.quote_key = quote_key(self.key)
        # accounts and sponsorship (#276): the funder's messages in the relay's domain; pulls by the relay's own key
        self.pay_to = pay_to.lower()
        self.domain = eth.domain_separator("HPPLiteRelay", "1", self.chain_id, self.pay_to)
        self.token = Token(self.chain, args.asset, self.key, self.chain_id, args.receipt_timeout, self.lock)   # one lock: one nonce
        self.accounts = Accounts(args.dir, self.token, self.pay_to, args.min_pull)
        self.submitted = 0
        self.started = int(time.time())

    # --- what it takes ---
    def classify(self, to: str, data: str):
        """(name, None, rollup) for a call the relay takes - rollup the rollup it is for, None on the factory - or
        (None, why, None) when it does not."""
        sel = data[2:10]
        to = to.lower()
        try:
            if self.fleet_factory and to == self.fleet_factory:
                return FLEET_FACTORY_CALLS.get(sel), "not a call the relay takes on the fleet factory", None
            if self.fleet_factory and (sel in FLEET_DB_CALLS or sel in FLEET_ROOT_CALLS) and self.is_fleet(to):
                if sel in FLEET_ROOT_CALLS:
                    return FLEET_ROOT_CALLS[sel], None, to                        # the fleet's sponsor: roots
                return FLEET_DB_CALLS[sel], None, db_key(to, "0x" + data[10:74])  # the database's sponsor
            if to == self.factory:
                return FACTORY_CALLS.get(sel), "not a call the relay takes on the factory", None
            if sel in ROLLUP_CALLS:
                da = self.chain.view(self.factory, "daOf(address)", to)
                if int(da, 16) == 0:
                    return None, "not a rollup of factory %s" % self.factory, None
                return ROLLUP_CALLS[sel], None, to
            if sel in DA_CALLS:
                coord = "0x" + self.chain.view(to, "coordinator()")[-40:]
                da = self.chain.view(self.factory, "daOf(address)", coord)
                if da[-40:].lower() != to[2:]:
                    return None, "not the DA of a rollup of factory %s" % self.factory, None
                return DA_CALLS[sel], None, coord
        except (RpcError, OSError, ValueError) as e:
            return None, "could not check the target: %s" % e, None
        return None, "not a call the relay takes", None

    def is_fleet(self, to):
        """a fleet of the v11 factory this relay serves, by the factory's own record (never by probing views)"""
        if not self.fleet_factory:
            return False
        try:
            return int(self.chain.view(self.fleet_factory, "isFleet(address)", to) or "0x0", 16) != 0
        except (RpcError, OSError, ValueError):
            return False

    def fleet_record(self, fleet, db):
        """a fleet database's (owner, sequencer) on chain, lowercase; (None, None) before its record exists"""
        r = self.chain.view(fleet, "db(bytes32)", bytes.fromhex(db[2:]))[2:]
        owner, seq = "0x" + r[24:64], "0x" + r[88:128]
        return (None, None) if int(owner, 16) == 0 else (owner.lower(), seq.lower())

    def da_of(self, rollup):
        """the DA of a rollup of this factory, lowercase, or None"""
        try:
            da = self.chain.view(self.factory, "daOf(address)", rollup)
        except (RpcError, OSError, ValueError):
            return None
        return ("0x" + da[-40:]).lower() if int(da, 16) else None

    def roles_of(self, rollup):
        """the rollup's (owner, sequencer) on chain, lowercase; raises when the chain cannot be read"""
        owner, seq = self.chain.view(rollup, "owner()"), self.chain.view(rollup, "sequencer()")
        return ("0x" + owner[-40:]).lower(), ("0x" + seq[-40:]).lower()

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
        if getattr(self.args, "slow_submit", 0):
            time.sleep(self.args.slow_submit)            # a test hook: a slow chain's turn (operators#45)
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
            "fleetFactory": self.fleet_factory,
            "fleetCalls": sorted(set(FLEET_FACTORY_CALLS.values()) | set(FLEET_DB_CALLS.values()) | set(FLEET_ROOT_CALLS.values()))
                          if self.fleet_factory else [],
            "payment": {"scheme": "exact", "network": self.payments.network, "asset": self.args.asset,
                        "payTo": self.payments.pay_to, "ethUsd": self.args.eth_usd, "margin": self.args.margin,
                        "minFee": self.args.min_fee, "facilitator": self.args.facilitator}
                       if self.payments.enabled else None,
            "accounts": {"spender": self.address, "payTo": self.pay_to, "minPull": self.args.min_pull,
                         "domain": {"name": "HPPLiteRelay", "version": "1", "chainId": self.chain_id, "verifyingContract": self.pay_to},
                         "asset": self.args.asset, "assetDomain": {"name": self.args.asset_name, "version": self.args.asset_version}}
                        if self.payments.enabled else None,
            "uptime": int(time.time()) - self.started,
        }


class Handler(http.server.BaseHTTPRequestHandler):
    relay = None  # type: Relay

    def log_message(self, fmt, *args):
        if self.relay.args.verbose:
            sys.stderr.write("relay: " + fmt % args + "\n")

    def queue_full(self):
        """operators#45: a call that waited --queue-wait for its turn on the relay's key is not sent late (its caller has
        likely given up and tries again); 503 with a Retry-After, nothing sent or charged"""
        return self.reply(503, {"error": "the relay's send queue is long: try again in %d s" % self.relay.args.queue_retry},
                          {"Retry-After": str(self.relay.args.queue_retry)})

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
        if re.match(r"^/(account|sponsor|fleet)/", self.path) and not self.relay.payments.enabled:
            return self.reply(404, {"error": "a free relay keeps no accounts"})
        m = re.match(r"^/account/(0x[0-9a-fA-F]{40})$", self.path)
        if m:
            a = self.relay.accounts.view_account(m.group(1).lower())
            return self.reply(200, a) if a else self.reply(404, {"error": "no such account", "funder": m.group(1).lower()})
        m = re.match(r"^/sponsor/(0x[0-9a-fA-F]{40})$", self.path)
        if m:
            s = self.relay.accounts.view_sponsor(m.group(1).lower())
            return self.reply(200, s) if s else self.reply(404, {"error": "not a sponsored rollup", "rollup": m.group(1).lower()})
        m = re.match(r"^/fleet/(0x[0-9a-fA-F]{40})/db/(0x[0-9a-fA-F]{64})/sponsor$", self.path)
        if m:
            s = self.relay.accounts.view_sponsor(db_key(m.group(1), m.group(2)))
            return self.reply(200, s) if s else self.reply(404, {"error": "not a sponsored database", "fleet": m.group(1).lower(), "dbId": m.group(2).lower()})
        return self.reply(404, {"error": "not found"})

    def body_json(self):
        try:
            n = int(self.headers.get("Content-Length", "0"))
            return json.loads(self.rfile.read(n) if n > 0 else b"{}")
        except (ValueError, TypeError):
            return None

    # --- accounts and sponsorship (#276) ---
    def post_permit(self, funder):
        req = self.body_json() or {}
        r = self.relay
        try:
            value, deadline = int(req["value"]), int(req["deadline"])
            sig = sig_bytes(req["signature"]) if "signature" in req else \
                bytes.fromhex(req["r"][2:]) + bytes.fromhex(req["s"][2:]) + bytes([int(req["v"])])
        except (KeyError, TypeError, ValueError):
            return self.reply(400, {"error": "a permit is {value, deadline, signature} (or v, r, s)"})
        if not sig or len(sig) != 65 or deadline <= int(time.time()):
            return self.reply(400, {"error": "a permit with a 65-byte signature and a deadline ahead"})
        try:
            nonce = r.token.nonce(funder)
        except ChainUnreadable as e:
            return self.reply(503, {"error": "the chain cannot be read: %s" % e})
        domain = eth.domain_separator(r.args.asset_name, r.args.asset_version, r.chain_id, r.args.asset)
        digest = eth.typed_digest(domain, PERMIT_TYPE, funder, r.token.spender, value, nonce, deadline)
        if not same(eth.recover(digest, sig), funder):
            return self.reply(401, {"error": "the permit does not recover to the funder (spender, nonce, domain?)", "spender": r.token.spender, "nonce": nonce})
        r.accounts.give_permit(funder, value, deadline, sig)
        return self.reply(200, r.accounts.view_account(funder))

    def post_pay(self, funder, query):
        r, p = self.relay, self.relay.payments
        try:
            amount = int((query.get("amount") or ["0"])[0])
        except ValueError:
            amount = 0
        if not p.enabled:
            return self.reply(404, {"error": "this relay takes no x402 payments: give a permit"})
        if amount < r.args.min_pull:
            return self.reply(400, {"error": "amount below minPull", "minPull": r.args.min_pull})
        requirements = p.requirements(amount, "http://%s%s" % (self.headers.get("Host", "localhost"), self.path), "top up %s's account at the relay" % funder)
        header = self.headers.get("PAYMENT-SIGNATURE")
        if not header:
            return self.reply(402, requirements, {"PAYMENT-REQUIRED": base64.b64encode(json.dumps(requirements).encode()).decode()})
        payload = p.decode(header)
        if not payload or "payload" not in payload:
            return self.reply(400, {"error": "PAYMENT-SIGNATURE is not a payment payload"})
        v = p.verify(payload, requirements["accepts"][0])
        if not v.get("isValid"):
            requirements["error"] = v.get("invalidReason", "invalid payment")
            return self.reply(402, requirements, {"PAYMENT-REQUIRED": base64.b64encode(json.dumps(requirements).encode()).decode()})
        st = p.settle(payload, requirements["accepts"][0])
        if not st.get("success"):
            return self.reply(402, {"error": "settlement failed: " + st.get("errorReason", "?"), "settlement": st})
        r.accounts.credit(funder, amount)
        return self.reply(200, r.accounts.view_account(funder), {"PAYMENT-RESPONSE": base64.b64encode(json.dumps(st).encode()).decode()})

    def post_sponsor(self, rollup, action):
        req = self.body_json() or {}
        r = self.relay
        funder, issued = str(req.get("funder", "")).lower(), req.get("issuedAt")
        sig = sig_bytes(req.get("signature"))
        if not ADDR_RE.match(funder) or not isinstance(issued, int) or not sig:
            return self.reply(400, {"error": "a sponsorship is {funder, issuedAt, signature}" + (" with callsPerDay" if action == "/cap" else "")})
        if action == "/cap":
            calls = req.get("callsPerDay")
            if not isinstance(calls, int) or calls < 0:
                return self.reply(400, {"error": "a cap is {funder, callsPerDay, issuedAt, signature}"})
            digest = eth.typed_digest(r.domain, CAP_TYPE, rollup, calls, issued)
        else:
            digest = eth.typed_digest(r.domain, UNSPONSOR_TYPE if action == "/release" else SPONSOR_TYPE, rollup, funder, issued)
        if not same(eth.recover(digest, sig), funder):
            return self.reply(401, {"error": "the signature does not recover to the funder", "domain": r.status()["accounts"]["domain"]})
        if action == "/cap":
            return self.reply(*r.accounts.cap(rollup, funder, calls, issued))
        if action == "/release":
            return self.reply(*r.accounts.release(rollup, funder, issued))
        if r.is_fleet(rollup):
            # a fleet's roots (#313): its Sponsor's other side is the fleet's owner, alone
            try:
                fowner = ("0x" + r.chain.view(rollup, "owner()")[-40:]).lower()
            except (RpcError, OSError, ValueError) as e:
                return self.reply(503, {"error": "the chain cannot be read for the fleet's owner: %s" % e})
            agreed = eth.recover(digest, sig_bytes(req.get("rollupSignature"))) if req.get("rollupSignature") is not None else funder
            if not same(agreed, fowner):
                return self.reply(401, {"error": "a fleet's Sponsor is signed beside the funder by the fleet's owner", "owner": fowner})
            return self.reply(*r.accounts.sponsor(rollup, None, funder, issued, {"fleet": rollup}))
        da = r.da_of(rollup)
        if not da:
            return self.reply(404, {"error": "not a rollup of factory %s" % r.factory, "rollup": rollup})
        # who pays for a rollup is named from the rollup's side: its owner or its sequencer signs the same Sponsor
        # beside the funder, unless the funder is one of them. A funder alone could take any rollup's sponsorship
        # over with a newer issuedAt and an empty account, and the rollup's calls would stop being paid.
        try:
            owner, seq = r.roles_of(rollup)
        except (RpcError, OSError, ValueError) as e:
            return self.reply(503, {"error": "the chain cannot be read for the rollup's owner and sequencer: %s" % e})
        agreed = funder
        if req.get("rollupSignature") is not None:
            rsig = sig_bytes(req.get("rollupSignature"))
            if not rsig:
                return self.reply(400, {"error": "rollupSignature is a 65-byte signature, hex"})
            agreed = eth.recover(digest, rsig)
        if not (same(agreed, owner) or same(agreed, seq)):
            return self.reply(401, {"error": "the rollup's side has not agreed: a Sponsor is signed by the funder and, in rollupSignature, by the "
                                             "rollup's owner or sequencer, unless the funder is one of them",
                                    "owner": owner, "sequencer": seq})
        return self.reply(*r.accounts.sponsor(rollup, da, funder, issued))

    def post_sponsor_db(self, fleet, db, action):
        """SponsorDb / UnsponsorDb / CapDb over (fleet, dbId) (#312, #313): the store's BindDb rules for the database's side"""
        req = self.body_json() or {}
        r = self.relay
        if not r.is_fleet(fleet):
            return self.reply(404, {"error": "not a fleet of factory %s" % r.fleet_factory, "fleet": fleet})
        funder, issued = str(req.get("funder", "")).lower(), req.get("issuedAt")
        if not ADDR_RE.match(funder) or not isinstance(issued, int):
            return self.reply(400, {"error": "a database's sponsorship is {funder, issuedAt, ...}"})
        key, dbb = db_key(fleet, db), bytes.fromhex(db[2:])
        if action == "/cap":
            calls, sig = req.get("callsPerDay"), sig_bytes(req.get("signature"))
            if not isinstance(calls, int) or calls < 0 or not sig:
                return self.reply(400, {"error": "a cap is {funder, callsPerDay, issuedAt, signature}"})
            if not same(eth.recover(eth.typed_digest(r.domain, CAPDB_TYPE, fleet, dbb, calls, issued), sig), funder):
                return self.reply(401, {"error": "the signature does not recover to the funder"})
            return self.reply(*r.accounts.cap(key, funder, calls, issued))
        if action == "/release":
            sig = sig_bytes(req.get("signature"))
            if not sig or not same(eth.recover(eth.typed_digest(r.domain, UNSPONSORDB_TYPE, fleet, dbb, funder, issued), sig), funder):
                return self.reply(401, {"error": "the signature does not recover to the funder"})
            return self.reply(*r.accounts.release(key, funder, issued))
        digest = eth.typed_digest(r.domain, SPONSORDB_TYPE, fleet, dbb, funder, issued)
        f_sig, d_sig = sig_bytes(req.get("funderSignature")), sig_bytes(req.get("dbSignature"))
        if not f_sig or not same(eth.recover(digest, f_sig), funder):
            return self.reply(401, {"error": "the funder's signature does not recover to the funder"})
        signer = eth.recover(digest, d_sig) if d_sig else ""
        try:
            owner, seq = r.fleet_record(fleet, db)
        except (RpcError, OSError, ValueError) as e:
            return self.reply(503, {"error": "the chain cannot be read for the database's record: %s" % e})
        if owner:
            if not (same(signer, owner) or same(signer, seq)):
                return self.reply(401, {"error": "the database's side: its owner or its sequencer must sign", "signer": signer})
        else:
            o, nh = str(req.get("owner", "")).lower(), str(req.get("nameHash", ""))
            if not ADDR_RE.match(o) or not re.match(r"^0x[0-9a-fA-F]{64}$", nh):
                return self.reply(400, {"error": "a database the fleet has no record of yet: name owner and nameHash beside the owner's signature"})
            if "0x" + eth.keccak256(eth.word(o) + eth.word(bytes.fromhex(nh[2:]))).hex() != db:
                return self.reply(400, {"error": "dbId is not keccak256(abi.encode(owner, nameHash))"})
            if not same(signer, o):
                return self.reply(401, {"error": "before its record, the database's owner alone sponsors it", "signer": signer})
        return self.reply(*r.accounts.sponsor(key, None, funder, issued, {"fleet": fleet, "dbId": db}))

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        if self.relay.per_minute > 0:
            # the canonical relay's limit per address (somaro#48): every POST counts, a refused one too; over it, a 429
            # with Retry-After: 60
            now, who = time.time(), self.client_address[0]
            with self.relay.posts_lock:
                seen = [t for t in self.relay.posts.get(who, []) if t > now - 60] + [now]
                self.relay.posts[who] = seen
            if len(seen) > self.relay.per_minute:
                return self.reply(429, {"error": "too many requests from this address: try again in a minute"}, {"Retry-After": "60"})
        if re.match(r"^/(account|sponsor|fleet)/", u.path) and not self.relay.payments.enabled:
            return self.reply(404, {"error": "a free relay keeps no accounts"})
        m = re.match(r"^/account/(0x[0-9a-fA-F]{40})/(permit|pay)$", u.path)
        if m:
            return self.post_permit(m.group(1).lower()) if m.group(2) == "permit" else self.post_pay(m.group(1).lower(), urllib.parse.parse_qs(u.query))
        m = re.match(r"^/sponsor/(0x[0-9a-fA-F]{40})(/release|/cap)?$", u.path)
        if m:
            return self.post_sponsor(m.group(1).lower(), m.group(2) or "")
        m = re.match(r"^/fleet/(0x[0-9a-fA-F]{40})/db/(0x[0-9a-fA-F]{64})/sponsor(/release|/cap)?$", u.path)
        if m:
            return self.post_sponsor_db(m.group(1).lower(), m.group(2).lower(), m.group(3) or "")
        if u.path != "/commit":
            return self.reply(404, {"error": "not found"})
        if self.relay.busy_commits > 0:                 # a test hook: the first k calls answer 429 as an operator's rate limit does
            self.relay.busy_commits -= 1
            return self.reply(429, {"error": "too many requests from this address: try again later"},
                              {"Retry-After": str(self.relay.args.busy_retry_after)})
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
        name, why, rollup = r.classify(to, data)
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
            est, gas = gas, int(gas * 1.25) + 10000              # headroom: the state moves between estimate and inclusion
            quote = (r.price(est, r.fees()[2]), gas, now + 120)  # priced at the estimate; the headroom is the limit only (#293)
        amount, gas, until = quote
        # a sponsored rollup (#276): its sponsor's account pays, taken before the call is sent and given back when it
        # does not land, and the caller is asked nothing; when the account cannot pay, the 402 says why and the caller
        # may still pay itself. A caller that sends a payment pays
        sponsor, sponsor_why = (r.accounts.cover(rollup, amount, counted=(name not in ("retireFor", "expireFor")))   # charged, outside the cap (#313, #321)
                                if (rollup and p.enabled and not header) else (None, None))
        if sponsor and not sponsor_why:
            if not r.lock.acquire(timeout=r.args.queue_wait if r.args.queue_wait > 0 else -1):
                r.accounts.refund(rollup, sponsor, amount)
                return self.queue_full()
            try:
                rcpt, err = r.submit(to, data, gas)
            except Exception:
                r.accounts.refund(rollup, sponsor, amount)
                raise
            finally:
                r.lock.release()
            if err or int(rcpt.get("status") or "0x0", 16) != 1:
                r.accounts.refund(rollup, sponsor, amount)
                tx = rcpt.get("transactionHash") if rcpt else None
                return self.reply(409 if rcpt else 502, {"error": err or "the transaction reverted", "tx": tx, "call": name, "charged": False})
            # the account pays what the call used (#294): the quote was taken before, the rest is given back
            used = min(amount, r.price(int(rcpt["gasUsed"], 16), int(rcpt.get("effectiveGasPrice") or "0x0", 16)))
            if used < amount:
                r.accounts.refund(rollup, sponsor, amount - used, uncount=False)
            return self.reply(200, {"tx": rcpt["transactionHash"], "block": int(rcpt["blockNumber"], 16), "gasUsed": int(rcpt["gasUsed"], 16),
                                    "call": name, "sponsor": sponsor, "charged": used})
        requirements = None
        if p.enabled:
            requirements = p.requirements(amount, "http://%s/commit" % self.headers.get("Host", "localhost"),
                                          "%s, %d gas, submitted and paid for by the relay" % (name, gas),
                                          r.quote_extra(to, data, amount, gas, until))
            if sponsor_why:
                requirements["sponsor"] = {"funder": sponsor, "why": sponsor_why}
            if not header:
                return self.reply(402, requirements, {"PAYMENT-REQUIRED": base64.b64encode(json.dumps(requirements).encode()).decode()})
        if p.enabled:
            v = p.verify(payload, requirements["accepts"][0])
            if not v.get("isValid"):
                requirements["error"] = v.get("invalidReason", "invalid payment") + (
                    ": " + str(v["invalidMessage"])[:200] if v.get("invalidMessage") else "")
                return self.reply(402, requirements, {"PAYMENT-REQUIRED": base64.b64encode(json.dumps(requirements).encode()).decode()})
        if not r.lock.acquire(timeout=r.args.queue_wait if r.args.queue_wait > 0 else -1):
            return self.queue_full()                       # nothing charged: the payment is settled only after a send
        with LockHeld(r.lock):
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
    ap.add_argument("--host", default="127.0.0.1", help="the address to listen on (default 127.0.0.1; 0.0.0.0 for every interface)")
    ap.add_argument("--rpc", required=True)
    ap.add_argument("--key-file", required=True, help="file with the hex key that pays the gas")
    ap.add_argument("--factory", required=True, help="the HPPLite factory whose rollups the relay serves (VERSION 10)")
    ap.add_argument("--busy-commits", type=int, default=0, help=argparse.SUPPRESS)    # a test hook: the first k commits answer 429
    ap.add_argument("--busy-retry-after", type=int, default=60, help=argparse.SUPPRESS)  # ...with this Retry-After (somaro#48)
    ap.add_argument("--queue-wait", type=float, default=45, help="seconds a call waits for its turn on the relay's key before a 503 (operators#45); 0: no bound")
    ap.add_argument("--slow-submit", type=float, default=0, help=argparse.SUPPRESS)   # a test hook: each send this much slower
    ap.add_argument("--queue-retry", type=int, default=30, help="the Retry-After of that 503, seconds")
    ap.add_argument("--per-minute", type=int, default=0, help="POSTs a minute from one address, as the canonical relay limits them (0: no limit)")
    ap.add_argument("--fleet-factory", default=None, help="the VERSION 11 fleet factory whose fleets the relay serves (#313); none: no fleets")
    ap.add_argument("--pay-to", default=None, help="where payments land (default: the relay's own address)")
    ap.add_argument("--facilitator", default=None, help="x402 facilitator URL, or none: free")
    ap.add_argument("--asset", default="0x401eCb1D350407f13ba348573E5630B83638E30D", help="the EIP-3009 token (USDC.e on HPP)")
    ap.add_argument("--asset-name", default="Bridged USDC")
    ap.add_argument("--asset-version", default="2")
    ap.add_argument("--eth-usd", type=float, default=3000.0, help="the rate the gas is charged at")
    ap.add_argument("--margin", type=float, default=0.2, help="on top of the gas, for the relay's risk and upkeep")
    ap.add_argument("--min-fee", type=int, default=100, help="atomic units (USDC.e: 1e-6) at least per call")
    ap.add_argument("--receipt-timeout", type=int, default=90)
    ap.add_argument("--dir", default="./hpplite_relay", help="where accounts and sponsorships are kept (#276)")
    ap.add_argument("--min-pull", type=int, default=1000000, help="the least a pull from a sponsor's permit, or an x402 top-up, moves (atomic units)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    Handler.relay = Relay(args)
    srv = Server((args.host, args.port), Handler)
    print("relay: listening on %s:%d, address %s, chain %d, factory %s, %s" % (
        args.host, args.port, Handler.relay.address, Handler.relay.chain_id, args.factory,
        ("paid via " + args.facilitator) if args.facilitator else "free"), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
