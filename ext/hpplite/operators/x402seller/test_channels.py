#!/usr/bin/env python3
"""The channels' background work as ticks (#185), with a facilitator that
only records what it is asked: a tick claims what was charged and settles,
with no thread started; a deposit the facilitator answers settlement_queued
is answered at once as pending, and taken (not sent again) once the
facilitator shows the channel holding it. ctest test_x402seller.
"""
import os
import secrets
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from x402seller import channels, eth  # noqa: E402

CHAIN_ID = 31337
ASSET = "0x" + "aa" * 20
fails = 0


def check(cond, what):
    global fails
    print(("  ok      " if cond else "  FAIL    ") + what, flush=True)
    if not cond:
        fails += 1


class Facilitator:
    """Payments as channels.py uses it; /verify answers the channel's state, /settle records the call"""

    def __init__(self, pay_to):
        self.network, self.asset, self.pay_to = "eip155:%d" % CHAIN_ID, ASSET, pay_to
        self.extra = {"name": "Bridged USDC", "version": "2"}
        self.facilitator, self.rpc = "http://facilitator.invalid", None
        self.balance = {}                 # channel id -> what the escrow holds
        self.queue = False
        self.pending = {}
        self.calls = []

    @staticmethod
    def cid(payload):
        return channels.channel_id(payload["payload"]["channelConfig"], CHAIN_ID)

    def verify(self, payload, req):
        return {"isValid": True, "extra": {"balance": str(self.balance.get(self.cid(payload), 0)), "totalClaimed": "0",
                                           "refundNonce": "0", "withdrawRequestedAt": 0}}

    def _facilitator_post(self, path, body):
        inner = body["paymentPayload"]["payload"]
        self.calls.append(inner.get("type"))
        if inner.get("type") == "deposit":
            cid = self.cid(body["paymentPayload"])
            if self.queue:
                self.pending[cid] = int(inner["deposit"]["amount"])
                return {"success": False, "errorReason": "settlement_queued", "transaction": "0xq"}
            self.balance[cid] = self.balance.get(cid, 0) + int(inner["deposit"]["amount"])
        return {"success": True, "transaction": "0x%02d" % len(self.calls)}


def payload(acc, buyer_key, salt, ceiling, deposit=0):
    x = acc["extra"]
    addr = eth.address(buyer_key)
    cfg = {"payer": addr, "payerAuthorizer": addr, "receiver": acc["payTo"], "receiverAuthorizer": x["receiverAuthorizer"],
           "token": acc["asset"], "withdrawDelay": int(x["withdrawDelay"]), "salt": salt}
    cid = channels.channel_id(cfg, CHAIN_ID)
    inner = {"type": "deposit" if deposit else "voucher", "channelConfig": cfg,
             "voucher": {"channelId": cid, "maxClaimableAmount": str(ceiling),
                         "signature": "0x" + eth.sign(buyer_key, channels.voucher_digest(cid, ceiling, CHAIN_ID)).hex()}}
    if deposit:
        inner["deposit"] = {"amount": str(deposit), "authorization": {"erc3009Authorization": {
            "validAfter": "0", "validBefore": "9999999999", "salt": "0x" + "11" * 32, "signature": "0x" + "22" * 65}}}
    return {"x402Version": 2, "accepted": acc, "payload": inner}


def main():
    work = tempfile.mkdtemp(prefix="hpplite-ticks.")
    seller_key, buyer_key = secrets.token_bytes(32), secrets.token_bytes(32)
    f = Facilitator(eth.address(secrets.token_bytes(32)))
    ch = channels.Channels(work, f, seller_key, withdraw_delay=900, min_deposit=500, claim_interval=60, refund_idle=3600,
                           log=lambda m: None)
    acc = ch.requirement(10)
    salt = "0x" + eth.keccak256(b"ticks").hex()

    # a deposit and two puts, then a tick past the claim interval: one claim, one settle, no thread
    ok, cid, err, tx = ch.verify(payload(acc, buyer_key, salt, 10, deposit=500), acc)
    check(ok and tx, "a deposit opens the channel (%s)" % err)
    ch.commit(payload(acc, buyer_key, salt, 10, deposit=500), acc, cid, tx)
    ok, cid, err, _ = ch.verify(payload(acc, buyer_key, salt, 20), acc)
    ch.commit(payload(acc, buyer_key, salt, 20), acc, cid, None)
    threads = threading.active_count()
    start = int(time.time())
    f.calls.clear()
    ch.tick(start + 1)
    check(f.calls == [], "a tick before the claim interval sends nothing (%s)" % f.calls)
    ch.tick(start + 61)
    check(f.calls == ["claim", "settle"], "a tick past it: one claim, one settle (%s)" % f.calls)
    check(threading.active_count() == threads, "no thread started")
    check(ch.book[cid]["totalClaimed"] == 20, "the 20 charged are claimed")

    # a deposit answered settlement_queued: pending at once, nothing waited for; not sent again; taken once it shows
    k2 = secrets.token_bytes(32)
    salt2 = "0x" + eth.keccak256(b"queued").hex()
    f.queue = True
    t0 = time.time()
    ok, cid2, err, tx = ch.verify(payload(acc, k2, salt2, 10, deposit=500), acc)
    check(not ok and err and "deposit_pending" in err and time.time() - t0 < 2,
          "a queued deposit: answered pending at once (%s, %.1f s)" % (err, time.time() - t0))
    f.calls.clear()
    ok, _, err, _ = ch.verify(payload(acc, k2, salt2, 10, deposit=500), acc)
    check(not ok and "deposit_pending" in (err or "") and f.calls == [], "asked again before it lands: still pending, no second deposit (%s)" % f.calls)
    f.balance[cid2], f.queue = f.pending[cid2], False         # the chain now shows it
    ok, _, err, tx = ch.verify(payload(acc, k2, salt2, 10, deposit=500), acc)
    check(ok and not tx and f.calls == [], "once it shows: the voucher taken, the deposit not sent again (%s %s)" % (err, f.calls))
    print("%s: %d failure(s)" % ("FAIL" if fails else "PASS", fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
