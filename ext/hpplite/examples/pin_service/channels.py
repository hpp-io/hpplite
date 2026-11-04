"""x402 batch-settlement, the seller's side (#135): a node deposits into a
payment channel once and pays each call with a signed cumulative voucher; the
seller checks the voucher itself and claims many at once on the chain.

The escrow is x402's x402BatchSettlement, at the same address on every chain
(HPP Sepolia has it; tests/l1/install_x402_escrow.sh puts it on anvil). A
channel is ChannelConfig(payer, payerAuthorizer, receiver, receiverAuthorizer,
token, withdrawDelay, salt), its id the EIP-712 hash of that config in the
escrow's domain ("x402 Batch Settlement", "1"). The node signs
Voucher(channelId, maxClaimableAmount) for its running total; the seller,
receiverAuthorizer here, signs ClaimBatch to claim and Refund to give back.
The spec: specs/schemes/batch-settlement/scheme_batch_settlement_evm.md in
github.com/coinbase/x402.

Who sends the transactions: the facilitator (deposit, claim, settle, refund,
all through its /verify and /settle) or, with --facilitator mock on anvil,
the seller itself with --settle-key through `cast`. Standard library only.
"""
import json
import os
import subprocess
import sys
import threading
import time

import eth

ESCROW = "0x4020074e9dF2ce1deE5A9C1b5c3f541D02a10003"
COLLECTOR = "0x4020806089470a89826cB9fB1f4059150b550004"     # ERC3009DepositCollector
SCHEME = "batch-settlement"

CONFIG_TYPE = ("ChannelConfig(address payer,address payerAuthorizer,address receiver,address receiverAuthorizer,"
               "address token,uint40 withdrawDelay,bytes32 salt)")
VOUCHER_TYPE = "Voucher(bytes32 channelId,uint128 maxClaimableAmount)"
REFUND_TYPE = "Refund(bytes32 channelId,uint256 nonce,uint128 amount)"
ENTRY_TYPE = "ClaimEntry(bytes32 channelId,uint128 maxClaimableAmount,uint128 totalClaimed)"
BATCH_TYPE = "ClaimBatch(ClaimEntry[] claims)" + ENTRY_TYPE

CONFIG_TUPLE = "(address,address,address,address,address,uint40,bytes32)"
MISMATCH = "invalid_batch_settlement_evm_cumulative_amount_mismatch"


def _b32(h):
    b = bytes.fromhex(h[2:] if h.startswith("0x") else h)
    if len(b) != 32:
        raise ValueError("not 32 bytes")
    return b


def domain(chain_id):
    return eth.domain_separator("x402 Batch Settlement", "1", chain_id, ESCROW)


def channel_id(config, chain_id):
    struct = (eth.keccak256(CONFIG_TYPE.encode()) + eth.word(config["payer"]) + eth.word(config["payerAuthorizer"])
              + eth.word(config["receiver"]) + eth.word(config["receiverAuthorizer"]) + eth.word(config["token"])
              + eth.word(int(config["withdrawDelay"])) + _b32(config["salt"]))
    return "0x" + eth.keccak256(b"\x19\x01" + domain(chain_id) + eth.keccak256(struct)).hex()


def voucher_digest(cid, amount, chain_id):
    return eth.typed_digest(domain(chain_id), VOUCHER_TYPE, _b32(cid), int(amount))


def refund_digest(cid, nonce, amount, chain_id):
    return eth.typed_digest(domain(chain_id), REFUND_TYPE, _b32(cid), int(nonce), int(amount))


def claim_batch_digest(entries, chain_id):
    """entries: (channelId, maxClaimableAmount, totalClaimed); the escrow's getClaimBatchDigest"""
    hashes = b"".join(eth.keccak256(eth.keccak256(ENTRY_TYPE.encode()) + _b32(c) + eth.word(int(m)) + eth.word(int(t)))
                      for c, m, t in entries)
    root = eth.keccak256(hashes)
    return eth.keccak256(b"\x19\x01" + domain(chain_id) + eth.keccak256(eth.keccak256(BATCH_TYPE.encode()) + root))


def _config_arg(c):
    return "(%s,%s,%s,%s,%s,%d,%s)" % (c["payer"], c["payerAuthorizer"], c["receiver"], c["receiverAuthorizer"],
                                      c["token"], int(c["withdrawDelay"]), c["salt"])


class Channels:
    """The seller's book of channels: what each has been charged, the latest voucher, the chain's view."""

    def __init__(self, directory, payments, receiver_key: bytes, withdraw_delay, min_deposit,
                 claim_interval, refund_idle, log=None, send_lock=None):
        self.p = payments                   # pin_service.Payments: network, asset, pay_to, facilitator, rpc, settle_key
        self.key = receiver_key
        self.authorizer = eth.address(receiver_key)
        self.chain_id = int(payments.network.split(":")[1])
        self.withdraw_delay = withdraw_delay
        self.min_deposit = min_deposit
        self.claim_interval = claim_interval
        self.refund_idle = refund_idle
        self.path = os.path.join(directory, "channels.json")
        self.lock = threading.RLock()
        self.log = log or (lambda m: sys.stderr.write("channels: %s\n" % m))
        self.claimed_txs = 0
        self.settled_txs = 0
        self.mock_queued = False        # test knob: the mock facilitator sends a deposit and answers "queued", as HPP's may
        self.send_lock = send_lock      # held around every transaction sent with --settle-key (the relay's own nonce)
        try:
            with open(self.path) as f:
                self.book = json.load(f)
        except (OSError, ValueError):
            self.book = {}

    def _save(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.book, f)
        os.replace(tmp, self.path)

    # --- the 402 ---
    def requirement(self, amount):
        return {"scheme": SCHEME, "network": self.p.network, "amount": str(amount), "asset": self.p.asset,
                "payTo": self.p.pay_to, "maxTimeoutSeconds": 120,
                "extra": dict(self.p.extra, receiverAuthorizer=self.authorizer, withdrawDelay=self.withdraw_delay,
                              minDeposit=str(max(self.min_deposit, 10 * int(amount))))}

    def _snapshot(self, cid, st):
        return {"channelId": cid, "balance": str(st["balance"]), "totalClaimed": str(st["totalClaimed"]),
                "withdrawRequestedAt": st.get("withdrawRequestedAt", 0), "refundNonce": str(st.get("refundNonce", 0)),
                "chargedCumulativeAmount": str(st["charged"])}

    def corrective(self, requirements, cid):
        """The 402 that tells a node where its total stands: the state and the node's own last voucher"""
        st = self.book.get(cid)
        for a in requirements["accepts"]:
            if a["scheme"] == SCHEME and st:
                a["extra"]["channelState"] = self._snapshot(cid, st)
                if st.get("signature"):
                    a["extra"]["voucherState"] = {"signedMaxClaimable": str(st["signedMax"]), "signature": st["signature"]}
        requirements["error"] = MISMATCH
        return requirements

    # --- the chain's view ---
    def _cast(self, *args, send=False):
        cmd = ["cast", "send" if send else "call", "--rpc-url", self.p.rpc]
        if send:
            cmd += ["--private-key", self.p.settle_key, "--json"]
        elif self.p.settle_key:
            cmd += ["--from", eth.address(bytes.fromhex(self.p.settle_key[2:] if self.p.settle_key.startswith("0x") else self.p.settle_key))]
        if send and self.send_lock is not None:
            with self.send_lock:
                r = subprocess.run(cmd + list(args), capture_output=True, text=True, timeout=120)
        else:
            r = subprocess.run(cmd + list(args), capture_output=True, text=True, timeout=120)
        if r.returncode != 0:
            raise RuntimeError((r.stderr or r.stdout).strip().splitlines()[-1][:300] if (r.stderr or r.stdout).strip() else "cast failed")
        return json.loads(r.stdout).get("transactionHash", "") if send else r.stdout.strip()

    def _chain_state(self, cid):
        """(balance, totalClaimed, refundNonce, withdrawRequestedAt) from the escrow (mock mode)"""
        vals = lambda out: [int(t) for t in out.split() if not t.startswith("[")]     # cast annotates: "100000 [1e5]"
        out = vals(self._cast(ESCROW, "channels(bytes32)(uint128,uint128)", cid))
        nonce = vals(self._cast(ESCROW, "refundNonce(bytes32)(uint256)", cid))[0]
        pw = vals(self._cast(ESCROW, "pendingWithdrawals(bytes32)(uint128,uint40)", cid))
        return out[0], out[1], nonce, pw[1]

    def _mock_collector_data(self, auth):
        return subprocess.run(["cast", "abi-encode", "f(uint256,uint256,uint256,bytes)", str(auth["validAfter"]),
                               str(auth["validBefore"]), str(int(auth["salt"], 16)), auth["signature"]],
                              capture_output=True, text=True, check=True).stdout.strip()

    def _mock_deposit(self, config, amount, auth, send):
        return self._cast(ESCROW, "deposit(%s,uint128,address,bytes)" % CONFIG_TUPLE, _config_arg(config), str(amount),
                          COLLECTOR, self._mock_collector_data(auth), send=send)

    # --- one paid call ---
    def verify(self, payload, req):
        """Before the work: (ok, cid, error, deposit_tx). A deposit lands on the chain here, so the work is never free."""
        inner = payload.get("payload") or {}
        kind = inner.get("type")
        config = inner.get("channelConfig") or {}
        voucher = inner.get("voucher") or {}
        try:
            cid = channel_id(config, self.chain_id)
        except (KeyError, ValueError, TypeError):
            return False, None, "invalid_batch_settlement_evm_voucher_payload", None
        if cid.lower() != str(voucher.get("channelId", "")).lower():
            return False, cid, "invalid_batch_settlement_evm_channel_id_mismatch", None
        if config["receiver"].lower() != self.p.pay_to.lower() or config["token"].lower() != self.p.asset.lower():
            return False, cid, "invalid_batch_settlement_evm_receiver_mismatch", None
        if config["receiverAuthorizer"].lower() != self.authorizer.lower():
            return False, cid, "invalid_batch_settlement_evm_receiver_authorizer_mismatch", None
        if int(config["withdrawDelay"]) != self.withdraw_delay:
            return False, cid, "invalid_batch_settlement_evm_withdraw_delay_mismatch", None
        if kind not in ("voucher", "deposit"):
            return False, cid, "invalid_batch_settlement_evm_payload_type", None
        signer = config["payerAuthorizer"]
        if int(signer, 16) == 0:
            return False, cid, "invalid_batch_settlement_evm_voucher_signature: this seller takes EOA-authorized vouchers only", None
        try:
            ceiling = int(voucher["maxClaimableAmount"])
            sig = bytes.fromhex(voucher["signature"][2:])
        except (KeyError, ValueError, TypeError):
            return False, cid, "invalid_batch_settlement_evm_voucher_payload", None
        if eth.recover(voucher_digest(cid, ceiling, self.chain_id), sig).lower() != signer.lower():
            return False, cid, "invalid_batch_settlement_evm_voucher_signature", None
        with self.lock:
            st = self.book.get(cid)
            if st is None:
                # a channel this seller has no record of: the chain's claimed total is where the count starts (state lost, or new)
                try:
                    bal, claimed, nonce, wr = self._onchain(payload, req, cid, kind)
                except RuntimeError as e:
                    return False, cid, "invalid_batch_settlement_evm_channel_state_read_failed: %s" % e, None
                st = {"config": config, "charged": claimed, "signedMax": claimed, "signature": "", "balance": bal,
                      "totalClaimed": claimed, "refundNonce": nonce, "withdrawRequestedAt": wr, "last": 0}
                self.book[cid] = st
            if ceiling != st["charged"] + int(req["amount"]):
                return False, cid, MISMATCH, None
            deposit_tx = None
            if kind == "deposit":
                dep = inner.get("deposit") or {}
                amount = int(dep.get("amount", 0))
                auth = (dep.get("authorization") or {}).get("erc3009Authorization")
                if amount <= 0 or not auth:
                    return False, cid, "invalid_batch_settlement_evm_deposit_payload", None
                # the chain may hold more than this book: a deposit that landed after it was reported queued,
                # or one the node sent again. Enough there: the deposit is not sent, the voucher is taken.
                try:
                    st["balance"] = max(st["balance"], self._onchain(payload, req, cid, kind)[0])
                except RuntimeError:
                    pass
                if ceiling <= st["balance"]:
                    self._save()
                    return True, cid, None, None
                if ceiling > st["balance"] + amount:
                    return False, cid, "invalid_batch_settlement_evm_cumulative_exceeds_balance", None
                before = st["balance"]
                try:
                    deposit_tx = self._deposit(payload, req, config, amount, auth, cid, before)
                except RuntimeError as e:
                    return False, cid, "invalid_batch_settlement_evm_deposit_transaction_failed: %s" % e, None
                st["balance"] = before + amount
                self._save()
            elif ceiling > st["balance"]:
                return False, cid, "invalid_batch_settlement_evm_cumulative_exceeds_balance", None
            return True, cid, None, deposit_tx

    def _onchain(self, payload, req, cid, kind):
        if self.p.facilitator == "mock" or self.p.rpc:
            try:
                return self._chain_state(cid)
            except (RuntimeError, ValueError, IndexError, OSError, subprocess.SubprocessError) as e:
                raise RuntimeError(str(e))
        v = self.p.verify(payload, req)
        x = v.get("extra") or {}
        if "totalClaimed" not in x:
            if kind == "voucher":
                raise RuntimeError(v.get("invalidReason") or "the facilitator returned no channel state")
            return 0, 0, 0, 0             # a deposit opening a channel the chain has not seen
        return int(x.get("balance", 0)), int(x["totalClaimed"]), int(x.get("refundNonce", 0)), int(x.get("withdrawRequestedAt", 0))

    def _deposit(self, payload, req, config, amount, auth, cid, before):
        if self.p.facilitator == "mock":
            self._mock_deposit(config, amount, auth, send=False)     # simulate: the token's own check, nothing sent
            tx = self._mock_deposit(config, amount, auth, send=True)
            s = {"success": True, "transaction": tx}
            if self.mock_queued:
                s = {"success": False, "errorReason": "settlement_queued", "transaction": tx}
        else:
            v = self.p.verify(payload, req)
            if not v.get("isValid"):
                raise RuntimeError("%s %s" % (v.get("invalidReason", "?"), v.get("invalidMessage", "")))
            s = self.p._facilitator_post("/settle", {"x402Version": 2, "paymentPayload": payload, "paymentRequirements": req})
        if s.get("success"):
            return s.get("transaction", "")
        if s.get("errorReason") == "settlement_queued":
            # HPP's facilitator answers this when the deposit takes longer than its synchronous window: it is
            # being sent, not refused. Wait for the chain to show it; a node told "failed" deposits again (#135)
            for _ in range(40):
                try:
                    if self._onchain(payload, req, cid, "deposit")[0] >= before + amount:
                        self.log("deposit for %s landed after it was queued" % cid)
                        return s.get("transaction") or "queued"
                except RuntimeError:
                    pass
                time.sleep(3)
        raise RuntimeError("%s %s" % (s.get("errorReason", "?"), s.get("errorMessage", "")))

    def commit(self, payload, req, cid, deposit_tx, charge=None):
        """After the work: the charge counts, the voucher is the one to claim. The PAYMENT-RESPONSE body.
        charge: what the work actually cost, at most the price asked (the scheme's usage pricing; the relay's gas)"""
        voucher = payload["payload"]["voucher"]
        amount = int(req["amount"]) if charge is None else min(int(charge), int(req["amount"]))
        with self.lock:
            st = self.book[cid]
            st["charged"] += amount
            st["signedMax"] = int(voucher["maxClaimableAmount"])
            st["signature"] = voucher["signature"]
            st["last"] = int(time.time())
            self._save()
            snap = self._snapshot(cid, st)
        dep = payload["payload"].get("deposit") or {}
        return {"success": True, "transaction": deposit_tx or "", "network": self.p.network,
                "payer": payload["payload"]["channelConfig"]["payer"], "amount": str(dep.get("amount", "")) if deposit_tx else "",
                "extra": {"chargedAmount": str(amount), "channelState": snap}}

    # --- claims, settlement, refunds ---
    def claim_and_settle(self):
        """Claim every channel charged past what the chain has claimed, in one transaction, then settle to pay-to"""
        with self.lock:
            rows = [(cid, st) for cid, st in self.book.items() if st["charged"] > st["totalClaimed"] and st.get("signature")]
            if not rows:
                return None
            entries = [(cid, st["signedMax"], st["charged"]) for cid, st in rows]
            auth_sig = "0x" + eth.sign(self.key, claim_batch_digest(entries, self.chain_id)).hex()
            try:
                if self.p.facilitator == "mock":
                    arg = "[" + ",".join("((%s,%d),%s,%d)" % (_config_arg(st["config"]), st["signedMax"], st["signature"], st["charged"])
                                         for _, st in rows) + "]"
                    tx = self._cast(ESCROW, "claimWithSignature(((%s,uint128),bytes,uint128)[],bytes)" % CONFIG_TUPLE, arg, auth_sig, send=True)
                    stx = self._cast(ESCROW, "settle(address,address)", self.p.pay_to, self.p.asset, send=True)
                else:
                    claims = [{"voucher": {"channel": st["config"], "maxClaimableAmount": str(st["signedMax"])},
                               "signature": st["signature"], "totalClaimed": str(st["charged"])} for _, st in rows]
                    tx = self._settle_call({"type": "claim", "claims": claims, "claimAuthorizerSignature": auth_sig})
                    stx = self._settle_call({"type": "settle", "receiver": self.p.pay_to, "token": self.p.asset})
            except RuntimeError as e:
                self.log("claim failed: %s" % e)
                return None
            for _, st in rows:
                st["totalClaimed"] = st["charged"]
            self.claimed_txs += 1
            self.settled_txs += 1
            self._save()
            self.log("claimed %d channel(s) in %s, settled in %s" % (len(rows), tx, stx))
            return tx, stx

    def refund_idle_channels(self, now=None):
        """Give back what idle channels hold beyond their charges (the node's way out when it has no ETH)"""
        now = now or int(time.time())
        out = []
        with self.lock:
            for cid, st in list(self.book.items()):
                if now - st.get("last", 0) < self.refund_idle or not st.get("signature"):
                    continue
                if self.p.facilitator == "mock" or self.p.rpc:
                    # the chain's word on what the channel holds and the refund nonce it expects: a refund
                    # by anyone else (or a deposit this book missed) moves them (#135)
                    try:
                        st["balance"], _, st["refundNonce"], _ = self._chain_state(cid)
                    except RuntimeError as e:
                        self.log("refund of %s: the chain could not be read: %s" % (cid, e))
                        continue
                left = st["balance"] - st["charged"]
                if left <= 0:
                    continue
                nonce = st.get("refundNonce", 0)
                sig = "0x" + eth.sign(self.key, refund_digest(cid, nonce, left, self.chain_id)).hex()
                try:
                    if self.p.facilitator == "mock":
                        if st["charged"] > st["totalClaimed"]:
                            self.claim_and_settle()
                        tx = self._cast(ESCROW, "refundWithSignature(%s,uint128,uint256,bytes)" % CONFIG_TUPLE,
                                        _config_arg(st["config"]), str(left), str(nonce), sig, send=True)
                    else:
                        claims = ([{"voucher": {"channel": st["config"], "maxClaimableAmount": str(st["signedMax"])},
                                    "signature": st["signature"], "totalClaimed": str(st["charged"])}]
                                  if st["charged"] > st["totalClaimed"] else [])
                        body = {"type": "refund", "channelConfig": st["config"],
                                "voucher": {"channelId": cid, "maxClaimableAmount": str(st["signedMax"]), "signature": st["signature"]},
                                "amount": str(left), "refundNonce": str(nonce), "claims": claims, "refundAuthorizerSignature": sig}
                        if claims:
                            body["claimAuthorizerSignature"] = "0x" + eth.sign(self.key, claim_batch_digest(
                                [(cid, st["signedMax"], st["charged"])], self.chain_id)).hex()
                        tx = self._settle_call(body)
                except RuntimeError as e:
                    self.log("refund of %s failed: %s" % (cid, e))
                    continue
                st["totalClaimed"] = st["charged"]
                st["balance"] = st["charged"]
                st["refundNonce"] = nonce + 1
                self._save()
                self.log("refunded %d to %s (%s)" % (left, st["config"]["payer"], tx))
                out.append((cid, left, tx))
        return out

    def _settle_call(self, inner):
        # HPP's facilitator answers a /settle body it has seen with its earlier result, without sending anything:
        # a settle(receiver, token) is the same body every time, so each call is made distinct (#135)
        req = {"scheme": SCHEME, "network": self.p.network, "asset": self.p.asset, "amount": "0", "payTo": self.p.pay_to,
               "maxTimeoutSeconds": 0, "extra": {"requestedAt": time.time_ns()}}
        s = self.p._facilitator_post("/settle", {"x402Version": 2, "paymentPayload": {"x402Version": 2, "payload": inner, "accepted": req},
                                                 "paymentRequirements": req})
        if not s.get("success"):
            raise RuntimeError("%s %s" % (s.get("errorReason", "?"), s.get("errorMessage", "")))
        return s.get("transaction", "")

    def start(self):
        """Claim on the interval (the window before a withdrawal can finish is withdrawDelay, much longer); refund the idle"""
        def loop():
            while True:
                time.sleep(self.claim_interval)
                try:
                    self.claim_and_settle()
                    self.refund_idle_channels()
                except Exception as e:                  # the loop outlives one bad round
                    self.log("round failed: %s" % e)
        threading.Thread(target=loop, daemon=True).start()

    def status(self):
        with self.lock:
            return {"count": len(self.book), "charged": sum(s["charged"] for s in self.book.values()),
                    "claimed": sum(s["totalClaimed"] for s in self.book.values()),
                    "claimTxs": self.claimed_txs, "receiverAuthorizer": self.authorizer, "escrow": ESCROW,
                    "withdrawDelay": self.withdraw_delay, "claimInterval": self.claim_interval}
