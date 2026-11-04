#!/usr/bin/env python3
"""The pin service's batch-settlement selftest on anvil (#135), run by
tests/l1/anvil_test.sh with x402's escrow installed from HPP Sepolia's
bytecode. A buyer written out here (what the node does in C) holds MockUSDC
and no ETH: one deposit opens its channel, then each put is a voucher the
service checks itself, nothing on the chain; a wrong total gets the
corrective 402 and the buyer's own last voucher back; one claim transaction
pays the service for every put; the service, its state lost, starts again
from the chain's claimed total; a voucher past the balance makes the buyer
top up; an idle channel's remainder comes back by refund. The buyer's ETH
stays 0.

    HPPLITE_ANVIL_RPC=... HPPLITE_ANVIL_USDC=... HPPLITE_ANVIL_ESCROW=... \\
    HPPLITE_ANVIL_KEY0=... HPPLITE_ANVIL_KEY7=... python3 -I channel_selftest.py
"""
import base64
import json
import os
import secrets
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, ".."))
from x402seller import channels, eth  # noqa: E402

RPC = os.environ["HPPLITE_ANVIL_RPC"]
USDC = os.environ.get("HPPLITE_ANVIL_USDC", "")
ESCROW = os.environ.get("HPPLITE_ANVIL_ESCROW", "")
KEY0 = os.environ["HPPLITE_ANVIL_KEY0"]                  # holds the MockUSDC supply
FACILITATOR = os.environ["HPPLITE_ANVIL_FACILITATOR"]     # the mock facilitator anvil_test.sh starts (#183)
FACILITATOR_ADDR = os.environ["HPPLITE_ANVIL_FACILITATOR_ADDR"]
CHAIN_ID = 31337
fails = 0


def check(cond, what):
    global fails
    print(("  ok    " if cond else "  FAIL  ") + what, flush=True)
    if not cond:
        fails += 1


def cast(*args):
    return subprocess.run(["cast"] + list(args) + ["--rpc-url", RPC], capture_output=True, text=True, check=True).stdout.strip()


def values(out):
    """cast's decoded values, without its annotations ("100000 [1e5]")"""
    return [int(t) for t in out.split() if not t.startswith("[")]


def usdc_of(a):
    return values(cast("call", USDC, "balanceOf(address)(uint256)", a))[0]


def eth_of(a):
    return int(cast("balance", a))


def nonce_of(a):
    return int(cast("nonce", a))


def channel_on_chain(cid):
    out = values(cast("call", ESCROW, "channels(bytes32)(uint128,uint128)", cid))
    return out[0], out[1]


class ChannelBuyer:
    """The payer's side: a channel per service, the running total, a voucher per call"""

    def __init__(self, url, key: bytes, salt: str):
        self.url, self.key, self.salt = url, key, salt
        self.addr = eth.address(key)
        self.charged = None                     # what the service charged so far, as this buyer knows it
        self.balance = 0
        self.last = None                        # (status, body, headers) of the last call
        self.deposits = 0

    def _post(self, path, body, headers=None):
        req = urllib.request.Request(self.url + path, data=body, headers=headers or {}, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                return r.status, json.loads(r.read()), dict(r.headers)
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"{}"), dict(e.headers)

    def config(self, acc):
        x = acc["extra"]
        return {"payer": self.addr, "payerAuthorizer": self.addr, "receiver": acc["payTo"], "receiverAuthorizer": x["receiverAuthorizer"],
                "token": acc["asset"], "withdrawDelay": int(x["withdrawDelay"]), "salt": self.salt}

    def voucher(self, cid, ceiling):
        return {"channelId": cid, "maxClaimableAmount": str(ceiling),
                "signature": "0x" + eth.sign(self.key, channels.voucher_digest(cid, ceiling, CHAIN_ID)).hex()}

    def deposit_auth(self, acc, cid, amount):
        salt = "0x" + secrets.token_bytes(32).hex()
        nonce = eth.keccak256(bytes.fromhex(cid[2:]) + bytes.fromhex(salt[2:]))
        now = int(time.time())
        va, vb = now - 600, now + 3600
        dom = eth.domain_separator(acc["extra"]["name"], acc["extra"]["version"], CHAIN_ID, acc["asset"])
        digest = eth.typed_digest(dom, "ReceiveWithAuthorization(address from,address to,uint256 value,uint256 validAfter,uint256 validBefore,bytes32 nonce)",
                                  self.addr, channels.COLLECTOR, amount, va, vb, nonce)
        return {"validAfter": str(va), "validBefore": str(vb), "salt": salt, "signature": "0x" + eth.sign(self.key, digest).hex()}

    def pay(self, acc, kind, ceiling=None, deposit=0, bad_sig=False):
        cfg = self.config(acc)
        cid = channels.channel_id(cfg, CHAIN_ID)
        ceiling = (self.charged or 0) + int(acc["amount"]) if ceiling is None else ceiling
        inner = {"type": kind, "channelConfig": cfg, "voucher": self.voucher(cid, ceiling)}
        if bad_sig:
            inner["voucher"]["signature"] = "0x" + eth.sign(secrets.token_bytes(32), channels.voucher_digest(cid, ceiling, CHAIN_ID)).hex()
        if kind == "deposit":
            inner["deposit"] = {"amount": str(deposit), "authorization": {"erc3009Authorization": self.deposit_auth(acc, cid, deposit)}}
        return {"x402Version": 2, "accepted": acc, "payload": inner}, cid

    def put(self, data, **kw):
        """One paid put; the voucher's ceiling and kind chosen as the node does unless kw forces them"""
        st, body, h = self._post("/put", data)
        if st != 402:
            return st, body, h
        req = json.loads(base64.b64decode(h["PAYMENT-REQUIRED"]))
        acc = next(a for a in req["accepts"] if a["scheme"] == channels.SCHEME)
        if self.charged is None:
            self.charged = 0
        amount = int(acc["amount"])
        kind = kw.pop("kind", None) or ("deposit" if self.charged + amount > self.balance else "voucher")
        deposit = kw.pop("deposit", None) or (int(acc["extra"]["minDeposit"]) if kind == "deposit" else 0)
        payload, cid = self.pay(acc, kind, deposit=deposit, **kw)
        st, body, h = self._post("/put", data, {"PAYMENT-SIGNATURE": base64.b64encode(json.dumps(payload).encode()).decode()})
        self.last = (st, body, h)
        if st == 200 and "PAYMENT-RESPONSE" in h:
            r = json.loads(base64.b64decode(h["PAYMENT-RESPONSE"]))
            charged = int(r["extra"]["chargedAmount"])
            if charged <= amount:                   # the client's rule: its own state plus what was charged
                self.charged += charged
            if kind == "deposit":
                self.balance += deposit
                self.deposits += 1
        return st, body, h

    def adopt(self, req):
        """The corrective 402: believe the service's total only if the voucher it shows is this buyer's own"""
        acc = next(a for a in req["accepts"] if a["scheme"] == channels.SCHEME)
        cs, vs = acc["extra"].get("channelState"), acc["extra"].get("voucherState")
        if not cs or not vs:
            return False
        cid = cs["channelId"]
        signer = eth.recover(channels.voucher_digest(cid, int(vs["signedMaxClaimable"]), CHAIN_ID), bytes.fromhex(vs["signature"][2:]))
        if signer.lower() != self.addr.lower():
            return False
        self.charged = int(cs["chargedCumulativeAmount"])
        self.balance = int(cs["balance"])
        return True


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def mock_facilitator(**knobs):
    """The mock facilitator's behaviour, as HPP's was found to behave (facilitator_mock.py)"""
    req = urllib.request.Request(FACILITATOR + "/_mock", data=json.dumps(knobs).encode(), headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read())


def start_service(work, port, pay_to, extra=()):
    log = open(os.path.join(work, "service-%d.log" % port), "w")
    p = subprocess.Popen([sys.executable, "-I", os.path.join(here, "pin_service.py"), "--port", str(port), "--dir", os.path.join(work, "store"),
                          "--signer-key", os.path.join(work, "signer.key"), "--pay-to", pay_to, "--facilitator", FACILITATOR,
                          "--rpc", RPC, "--network", "eip155:%d" % CHAIN_ID, "--asset", USDC,
                          "--withdraw-delay", "900", "--claim-interval", "200", "--min-deposit", "500"] + list(extra), stdout=log, stderr=log)
    for _ in range(100):
        try:
            with urllib.request.urlopen("http://127.0.0.1:%d/status" % port, timeout=2) as r:
                if r.status == 200:
                    return p
        except OSError:
            time.sleep(0.1)
        if p.poll() is not None:
            break
    print(open(os.path.join(work, "service-%d.log" % port)).read())
    raise SystemExit("the pin service did not start")


def main():
    if not USDC or not ESCROW:
        print("SKIP: no MockUSDC or x402 escrow on this chain")
        return 77
    work = tempfile.mkdtemp(prefix="hpplite-chan.")
    signer_key = secrets.token_bytes(32)
    with open(os.path.join(work, "signer.key"), "w") as f:
        f.write(signer_key.hex())
    pay_to = eth.address(secrets.token_bytes(32))
    buyer_key = secrets.token_bytes(32)
    buyer = eth.address(buyer_key)
    cast("send", "--private-key", KEY0, USDC, "transfer(address,uint256)", buyer, "5000000")
    settle_addr = FACILITATOR_ADDR
    port = free_port()
    svc = start_service(work, port, pay_to)
    url = "http://127.0.0.1:%d" % port
    b = ChannelBuyer(url, buyer_key, "0x" + eth.keccak256(b"hpplite" + bytes(20)).hex())
    procs = [svc]
    try:
        print("x402 batch-settlement on anvil (#135): buyer %s, service pays to %s" % (buyer, pay_to))
        st, body, h = b._post("/put", b"probe")
        req = json.loads(base64.b64decode(h.get("PAYMENT-REQUIRED", "e30=")))
        schemes = [a["scheme"] for a in req.get("accepts", [])]
        check(st == 402 and schemes == ["exact", "batch-settlement"], "the 402 offers exact and batch-settlement (%s)" % schemes)

        # 1. the first put opens the channel: one deposit transaction, sent by the facilitator's key
        n0 = nonce_of(settle_addr)
        st, body, h = b.put(b"put number 0")
        cid = channels.channel_id(b.config(next(a for a in req["accepts"] if a["scheme"] == channels.SCHEME)), CHAIN_ID)
        bal, claimed = channel_on_chain(cid)
        check(st == 200 and body.get("cid"), "first put: 200 with a CID (%d %s)" % (st, body.get("error", "")))
        check(bal == b.balance and bal > 0 and claimed == 0, "the channel holds the deposit on the chain (%d)" % bal)
        check(nonce_of(settle_addr) == n0 + 1, "one transaction: the deposit")
        check(usdc_of(buyer) == 5000000 - bal, "the deposit left the buyer's USDC")

        # 2. puts 1..5: vouchers only, nothing on the chain
        n1 = nonce_of(settle_addr)
        ok = all(b.put(b"put number %d" % i)[0] == 200 for i in range(1, 6))
        check(ok, "puts 1-5 paid by voucher")
        check(nonce_of(settle_addr) == n1, "no transaction for them (the facilitator's nonce did not move)")
        check(channel_on_chain(cid) == (bal, 0), "the chain has not seen them")

        # 3. a voucher for the wrong total: the corrective 402, with the buyer's own last voucher
        b.charged -= 7
        st, body, h = b.put(b"put number 6")
        check(st == 402 and body.get("error") == channels.MISMATCH, "a wrong total: 402 %s" % body.get("error"))
        check(b.adopt(body), "the corrective 402 carries the buyer's own last voucher: total adopted")
        check(b.put(b"put number 6")[0] == 200, "the put again, at the right total: 200")

        # 4. a voucher signed by another key: refused
        st, body, h = b.put(b"put number 7", bad_sig=True)
        check(st == 402 and "voucher_signature" in body.get("error", ""), "a voucher another key signed: 402 (%s)" % body.get("error"))

        # 5. one claim transaction pays the service for every put
        charged = b.charged
        svc.terminate(); svc.wait()
        svc = start_service(work, port, pay_to, ["--claim-interval", "1", "--withdraw-delay", "900"])
        procs.append(svc)
        deadline = time.time() + 30
        while time.time() < deadline and usdc_of(pay_to) < charged:
            time.sleep(0.5)
        check(usdc_of(pay_to) == charged, "claimed and settled: the service got exactly the %d charged (%d)" % (charged, usdc_of(pay_to)))
        check(channel_on_chain(cid) == (bal, charged), "the chain's claimed total is the charges")

        # 6. the service loses its book: it starts again from the chain's claimed total
        svc.terminate(); svc.wait()
        os.remove(os.path.join(work, "store", "channels.json"))
        svc = start_service(work, port, pay_to)
        procs.append(svc)
        n2 = nonce_of(settle_addr)
        check(b.put(b"put number 8")[0] == 200, "after the service lost its book: the next voucher is taken (baseline: the chain)")
        check(nonce_of(settle_addr) == n2, "still nothing on the chain")

        # 7. past the balance: the buyer tops up with a deposit, one transaction
        pushed = 0
        while b.charged + 20 <= b.balance and pushed < 400:     # small puts cost 11 here
            if b.put(os.urandom(8))[0] != 200:
                break
            pushed += 1
        big = os.urandom(400 * 1024)                            # 10 + 12 for storage: more than what is left
        st, body, h = b.put(big, kind="voucher")
        check(st == 402 and "exceeds_balance" in body.get("error", ""), "a voucher past the balance: 402 (%s)" % body.get("error"))
        n3 = nonce_of(settle_addr)
        st, body, h = b.put(big)
        check(st == 200 and b.deposits == 2 and nonce_of(settle_addr) == n3 + 1, "the top-up: a second deposit, one transaction, the put done")

        # 8. the idle channel's remainder comes back by refund (the way out for a buyer with no ETH)
        svc.terminate(); svc.wait()
        svc = start_service(work, port, pay_to, ["--claim-interval", "1", "--refund-idle", "1"])
        procs.append(svc)
        deadline = time.time() + 40
        while time.time() < deadline and channel_on_chain(cid)[0] != b.charged:
            time.sleep(0.5)
        bal_end, claimed_end = channel_on_chain(cid)
        check(bal_end == b.charged and claimed_end == b.charged, "refunded: the channel holds only what was charged (%d/%d)" % (bal_end, claimed_end))
        check(usdc_of(buyer) == 5000000 - b.charged, "the buyer has back all but the %d charged" % b.charged)
        check(usdc_of(pay_to) == b.charged, "the service has the %d" % b.charged)
        check(eth_of(buyer) == 0, "the buyer's ETH: 0")

        # 9. a deposit the facilitator answers "queued" (HPP's does past its synchronous window): answered at once as
        # pending, nothing waits for the chain (#185); the node's retry finds it landed, takes the voucher and sends no
        # second deposit; a buyer that deposits again into a channel that already covers the put sends nothing (#135)
        svc.terminate(); svc.wait()
        svc = start_service(work, port, pay_to)
        procs.append(svc)
        mock_facilitator(queueDeposits=True)
        k2 = secrets.token_bytes(32)
        cast("send", "--private-key", KEY0, USDC, "transfer(address,uint256)", eth.address(k2), "1000000")
        b2 = ChannelBuyer(url, k2, "0x" + eth.keccak256(b"hpplite" + bytes([2]) * 20).hex())
        n5 = nonce_of(settle_addr)
        t0 = time.time()
        st, body, h = b2.put(b"queued deposit")
        took = time.time() - t0
        check(st == 402 and "deposit_pending" in str(body.get("error", "")) and took < 10 and nonce_of(settle_addr) == n5 + 1,
              "a deposit answered queued: 402 pending at once (%d, %.1f s), the deposit sent once" % (st, took))
        mock_facilitator(queueDeposits=False)
        st, body, h = b2.put(b"queued deposit")
        check(st == 200 and nonce_of(settle_addr) == n5 + 1, "the retry: the put done, no second deposit (%d %s)" % (st, body.get("error", "")))
        cid2 = json.loads(base64.b64decode(h["PAYMENT-RESPONSE"]))["extra"]["channelState"]["channelId"] if st == 200 else None
        bal2 = channel_on_chain(cid2)[0] if cid2 else -1
        check(bal2 == 500, "the channel holds one deposit (%d)" % bal2)
        b2.balance = b2.charged                         # as a node that lost count would: it deposits again
        st, body, h = b2.put(b"deposit again")
        r = json.loads(base64.b64decode(h["PAYMENT-RESPONSE"])) if st == 200 else {}
        check(st == 200 and nonce_of(settle_addr) == n5 + 1 and r.get("transaction") == "",
              "a deposit into a channel that covers the put: not sent, the put done (%d, tx %r)" % (st, r.get("transaction")))
        check(channel_on_chain(cid2)[0] == 500, "still one deposit in the channel")
    finally:
        mock_facilitator(queueDeposits=False)
        for p in procs:
            if p.poll() is None:
                p.terminate()
                p.wait()
    if fails:
        for f in sorted(os.listdir(work)):
            if f.startswith("service-"):
                print("--- %s" % f)
                print(open(os.path.join(work, f)).read()[-3000:])
    print("%s: %d failure(s)" % ("FAIL" if fails else "PASS", fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
