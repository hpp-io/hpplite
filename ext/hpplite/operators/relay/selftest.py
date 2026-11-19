#!/usr/bin/env python3
"""The relay's selftest on anvil (#170), run by tests/l1/anvil_test.sh: a
wallet holding MockUSDC (EIP-3009, as USDC.e) and no ETH at all creates its
rollup, takes the lease, anchors a range, renews and releases, every call
through the relay and paid over x402 (the mock facilitator anvil_test.sh
starts, #183: the token itself checks the authorization). What the relay refuses costs nothing. The
wallet's ETH balance is zero at the end.

    HPPLITE_ANVIL_RPC=... HPPLITE_ANVIL_FACTORY=... HPPLITE_ANVIL_USDC=... HPPLITE_ANVIL_KEY0=... \\
    HPPLITE_ANVIL_KEY9=... python3 -I selftest.py
"""
import base64
import json
import math
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
sys.path.insert(0, os.path.join(here, "..", "conformance"))
sys.path.insert(0, here)
from x402seller import eth  # noqa: E402
from buyer import Buyer  # noqa: E402
from x402seller.chain import Chain, selector  # noqa: E402

RPC = os.environ["HPPLITE_ANVIL_RPC"]
FACTORY = os.environ["HPPLITE_ANVIL_FACTORY"]
USDC = os.environ.get("HPPLITE_ANVIL_USDC", "")
KEY0 = os.environ["HPPLITE_ANVIL_KEY0"]                  # holds the MockUSDC supply
RELAY_KEY = os.environ["HPPLITE_ANVIL_KEY9"]             # the relay's funded hot key
CHIP = os.environ.get("HPPLITE_ANVIL_CHIP", "")
HPP = os.environ.get("HPPLITE_ANVIL_HPP", "")            # an 18-decimal stand-in for HPP (#361)
chain = Chain(RPC)
fails = 0


def check(cond, what):
    global fails
    print(("  ok      " if cond else "  FAIL    ") + what, flush=True)
    if not cond:
        fails += 1


# ---- ABI: static words and trailing dynamic bytes ----
def encode(sig, *args):
    types = sig[sig.index("(") + 1:-1].split(",")
    head, tail = [], b""
    for t, a in zip(types, args):
        if t == "bytes":
            head.append(None)
        else:
            head.append(eth.word(a))
    n = 32 * len(types)
    out = b""
    for t, a, h in zip(types, args, head):
        if t == "bytes":
            out += (n + len(tail)).to_bytes(32, "big")
            pad = (32 - len(a) % 32) % 32
            tail += len(a).to_bytes(32, "big") + a + b"\0" * pad
        else:
            out += h
    return "0x" + selector(sig) + (out + tail).hex()


def call_view(to, sig, *words):
    return chain.view(to, sig, *words)


def balance_eth(addr):
    return int(chain.call("eth_getBalance", addr, "latest"), 16)


def balance_usdc(addr):
    return int(call_view(USDC, "balanceOf(address)", addr), 16)


def price_units(gas, fee, eth_usd, margin=0.2, min_fee=100):
    """the protocol's price (doc/relay-protocol.md; relay.py's call_price), at the relay's default margin and minimum"""
    return max(math.ceil(gas * fee * (1 + margin) * eth_usd * 1e6 / 1e18), min_fee)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def http_get(url):
    try:
        with urllib.request.urlopen(url, timeout=30) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def post_raw(url, body):
    req = urllib.request.Request(url, data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def start_relay(work, port, keyfile, log, eth_usd="30"):
    """The relay under test, on port: (process, its /status or None). Module level so another relay can be checked
    by replacing it (hpp-io/hpplite-operators runs this selftest against the canonical relay Worker)"""
    url = "http://127.0.0.1:%d" % port
    p = subprocess.Popen([sys.executable, "-I", os.path.join(here, "relay.py"), "--port", str(port), "--rpc", RPC,
                          "--key-file", keyfile, "--factory", FACTORY, "--facilitator", os.environ["HPPLITE_ANVIL_FACILITATOR"],
                          "--asset", USDC, "--eth-usd", eth_usd, "--dir", os.path.join(work, "relay")]
                         # the other tokens it takes (#361): CHIP at $1 with x402, HPP at $0.01 (10000 units) by permit
                         + (["--extra-asset", "%s,House Chip,1,6,1000000,exact" % CHIP] if CHIP else [])
                         + (["--extra-asset", "%s,HousePartyProtocol,1,18,10000,upto" % HPP] if HPP else []), stdout=log, stderr=log)   # HPP by upto too (#358)
    for _ in range(100):
        if p.poll() is not None:
            return p, None
        try:
            with urllib.request.urlopen(url + "/status", timeout=2) as r:
                return p, json.loads(r.read())
        except OSError:
            time.sleep(0.1)
    return p, None


def queue_section(work, keyfile, chain_id):
    """operators#45: a call that waits past --queue-wait for its turn on the relay's key is answered 503 with a
    Retry-After, not sent after its caller gave up; tried again after it, it lands. Two creates at once against a free
    relay whose sends take 3 s and whose queue waits 1 s. HPPLITE_QUEUE_WAIT=0 runs it on an unbounded relay (red)"""
    import threading
    port = free_port()
    log = open(os.path.join(work, "relay_queue.log"), "w")
    qw = os.environ.get("HPPLITE_QUEUE_WAIT", "1")
    p = subprocess.Popen([sys.executable, "-I", os.path.join(here, "relay.py"), "--port", str(port), "--rpc", RPC, "--key-file", keyfile,
                          "--factory", FACTORY, "--dir", os.path.join(work, "relay_queue"), "--slow-submit", "3", "--queue-wait", qw,
                          "--queue-retry", "7"], stdout=log, stderr=log)
    url = "http://127.0.0.1:%d" % port
    try:
        for _ in range(100):
            try:
                urllib.request.urlopen(url + "/status", timeout=2).read()
                break
            except OSError:
                time.sleep(0.1)
        deadline = int(chain.call("eth_getBlockByNumber", "latest", False)["timestamp"], 16) + 3600
        fdomain = eth.domain_separator("HPPLiteFactory", "10", chain_id, FACTORY)
        calls = []
        for _ in range(2):
            k = secrets.token_bytes(32)
            w = eth.address(k)
            sig = eth.sign(k, eth.typed_digest(fdomain, "CreateRollup(address owner,uint256 checkpointInterval,uint256 nonce,uint256 deadline)", w, 100, 0, deadline))
            calls.append(json.dumps({"to": FACTORY, "data": encode("createRollupFor(address,uint256,uint256,uint256,bytes)", w, 100, 0, deadline, sig)}).encode())
        got = [None, None]

        def post(i):
            req = urllib.request.Request(url + "/commit", data=calls[i], headers={"Content-Type": "application/json"})
            try:
                with urllib.request.urlopen(req, timeout=60) as r:
                    got[i] = (r.status, r.headers.get("Retry-After"))
            except urllib.error.HTTPError as e:
                got[i] = (e.code, e.headers.get("Retry-After"))
        ts = [threading.Thread(target=post, args=(i,)) for i in range(2)]
        for t in ts:
            t.start()
            time.sleep(0.3)
        for t in ts:
            t.join()
        late = [i for i in range(2) if got[i] and got[i][0] == 503]
        check(sorted(g[0] for g in got if g) == [200, 503] and late and got[late[0]][1] == "7",
              "two calls at once on a relay whose turn takes 3 s: one sent, the other answered 503 + Retry-After 7 past --queue-wait (%s)" % (got,))
        if late:
            post(late[0])
        check(late and got[late[0]][0] == 200, "the refused call, tried again, lands (%s)" % (got[late[0]] if late else None,))
    finally:
        p.terminate()
        p.wait()


def main():
    if not USDC:
        print("SKIP: no MockUSDC on this chain")
        return 77
    chain_id = int(chain.call("eth_chainId"), 16)
    work = tempfile.mkdtemp(prefix="hpplite-relay.")
    keyfile = os.path.join(work, "relay.key")
    with open(keyfile, "w") as f:
        f.write(RELAY_KEY)
    relay_addr = eth.address(bytes.fromhex(RELAY_KEY[2:]))
    port = free_port()
    log = open(os.path.join(work, "relay.log"), "w")
    url = "http://127.0.0.1:%d" % port

    proc, status = start_relay(work, port, keyfile, log)            # anvil's gas costs ~100x HPP's (1 gwei vs 0.01)
    try:
        if proc.poll() is not None or not status:
            print("FAIL: the relay did not start"); print(open(os.path.join(work, "relay.log")).read()); return 1
        check(status["address"].lower() == relay_addr.lower() and status["chainId"] == chain_id, "GET /status names the relay's address and the chain")

        # a wallet with USDC.e and no ETH
        wkey = secrets.token_bytes(32)
        wallet = eth.address(wkey)
        subprocess.run(["cast", "send", "--rpc-url", RPC, "--private-key", KEY0, USDC, "transfer(address,uint256)",
                        wallet, "10000000"], check=True, capture_output=True)
        check(balance_eth(wallet) == 0 and balance_usdc(wallet) == 10_000_000, "a fresh wallet: 10 USDC.e, 0 ETH")
        buyer = Buyer(url, wkey, cap=1_000_000, valid_for=3600)

        def commit(to, data):
            u0, r0 = balance_usdc(wallet), balance_usdc(relay_addr)
            st, body, settlement = buyer.call("POST", "/commit", json.dumps({"to": to, "data": data}).encode())
            paid = u0 - balance_usdc(wallet)
            return st, body, settlement, paid, balance_usdc(relay_addr) - r0

        deadline = int(chain.call("eth_getBlockByNumber", "latest", False)["timestamp"], 16) + 3600

        # 1. the rollup
        fdomain = eth.domain_separator("HPPLiteFactory", "10", chain_id, FACTORY)
        sig = eth.sign(wkey, eth.typed_digest(fdomain, "CreateRollup(address owner,uint256 checkpointInterval,uint256 nonce,uint256 deadline)",
                                             wallet, 100, 0, deadline))
        st, body, settlement, paid, got = commit(FACTORY, encode("createRollupFor(address,uint256,uint256,uint256,bytes)",
                                                                 wallet, 100, 0, deadline, sig))
        rollup = "0x" + call_view(FACTORY, "getRollup(address)", wallet)[-40:]
        check(st == 200 and int(rollup, 16) != 0, "createRollupFor through the relay: rollup %s (gas %s)" % (rollup, body.get("gasUsed") if isinstance(body, dict) else body))
        check(paid > 0 and paid == got and settlement and settlement.get("success"), "paid in USDC.e on inclusion: %d units (%.6f), to the relay" % (paid, paid / 1e6))
        da = "0x" + call_view(FACTORY, "getDA(address)", rollup)[-40:]
        hdomain = eth.domain_separator("HPPLite", "10", chain_id, rollup)
        ddomain = eth.domain_separator("HPPLiteDA", "3", chain_id, da)
        lease_type = "Lease(string action,bytes32 instanceId,uint256 nonce,uint256 deadline)"
        instance = eth.keccak256(b"relay selftest instance")

        def lease(action, nonce):
            return eth.sign(wkey, eth.typed_digest(hdomain, lease_type, eth.keccak256(action.encode()), instance, nonce, deadline))

        # 2. the lease
        st, body, settlement, paid, _ = commit(rollup, encode("claimSequencerFor(bytes32,uint256,uint256,bytes)", instance, 0, deadline, lease("claim", 0)))
        holder = "0x" + call_view(rollup, "sequencerWallet()")[-40:]
        check(st == 200 and holder.lower() == wallet.lower() and paid > 0, "claimSequencerFor through the relay (%d units)" % paid)

        # 3. an anchor: heights 1-3 at a store, one commitment
        chain_after, root, manifest = eth.keccak256(b"chain"), eth.keccak256(b"root"), eth.keccak256(b"manifest")
        rsig = eth.sign(wkey, eth.typed_digest(ddomain, "Range(bytes32 instanceId,uint256 fromHeight,uint256 toHeight,bytes32 chainHashAfter,bytes32 stateRoot,bytes32 manifestDigest)",
                                               instance, 1, 3, chain_after, root, manifest))
        rdata = encode("commitRangeFor(bytes32,uint256,uint256,bytes32,bytes32,bytes32,bytes,bytes)", instance, 1, 3, chain_after, root, manifest, b"", rsig)
        st, body, settlement, paid, _ = commit(da, rdata)
        height = int(call_view(da, "lastBatchHeight()"), 16)
        check(st == 200 and height == 3 and paid > 0, "commitRangeFor through the relay: anchored to 3 (gas %s, %d units)" % (body.get("gasUsed") if isinstance(body, dict) else "?", paid))

        # 4. what is refused costs nothing
        u0 = balance_usdc(wallet)
        st, body, _, paid, _ = commit(da, rdata)
        check(st == 422 and paid == 0, "the same range again: 422 (%s), nothing charged" % (body.get("error") if isinstance(body, dict) else body))
        bad = bytearray(rsig); bad[5] ^= 1
        st, body, _, paid, _ = commit(da, encode("commitRangeFor(bytes32,uint256,uint256,bytes32,bytes32,bytes32,bytes,bytes)", instance, 4, 4, chain_after, root, manifest, b"", bytes(bad)))
        check(st == 422 and paid == 0, "a bad signature: 422, nothing charged")
        st, body = post_raw(url + "/commit", {"to": USDC, "data": encode("transfer(address,uint256)", relay_addr, 1)})
        check(st == 400, "a call outside HPPLite (a token transfer): 400 (%s)" % body.get("error"))
        st, body = post_raw(url + "/commit", {"to": rollup, "data": "0x" + selector("forceExpireLease()")})
        check(st == 400, "an owner call without a signed form: 400")
        check(balance_usdc(wallet) == u0, "the wallet's USDC.e unchanged by the refusals")

        # 5. renew, release
        st, _, _, paid, _ = commit(rollup, encode("renewLeaseFor(bytes32,uint256,uint256,bytes)", instance, 1, deadline, lease("renew", 1)))
        check(st == 200 and paid > 0, "renewLeaseFor through the relay")
        st, _, _, paid, _ = commit(rollup, encode("releaseLeaseFor(bytes32,uint256,uint256,bytes)", instance, 2, deadline, lease("release", 2)))
        active = int(call_view(rollup, "isLeaseActive()"), 16)
        check(st == 200 and active == 0, "releaseLeaseFor through the relay: the lease is free")

        # 6. the 402's price holds for the paid retry, whatever reaches it (#184): the relay restarted between the
        # two (another Worker isolate has no memory of the quote) at a price 100 times higher; the payment for the
        # quote is taken, and a quote the buyer changed is not
        cbody = json.dumps({"to": rollup, "data": encode("claimSequencerFor(bytes32,uint256,uint256,bytes)", instance, 3, deadline,
                                                         lease("claim", 3))}).encode()
        st, hdrs, _ = buyer._request("POST", "/commit", cbody)
        hdr = {k.upper(): v for k, v in hdrs.items()}.get("PAYMENT-REQUIRED", "")
        offered = json.loads(base64.b64decode(hdr)) if hdr else {}
        quoted = int(offered["accepts"][0]["amount"]) if offered else 0
        check(st == 402 and quoted > 0, "a 402 for the claim: %d units" % quoted)
        proc.terminate(); proc.wait()
        proc, status = start_relay(work, port, keyfile, log, eth_usd="3000")
        check(status is not None, "the relay restarted, at 100 times the price")
        # the quote prices the estimate; the headroom (x1.25 + 10000) is the transaction's limit only (#293)
        st2, hdrs2, _ = buyer._request("POST", "/commit", cbody)
        hdr2 = {k.upper(): v for k, v in hdrs2.items()}.get("PAYMENT-REQUIRED", "")
        offer2 = json.loads(base64.b64decode(hdr2))["accepts"][0] if hdr2 else {}
        est = int(chain.call("eth_estimateGas", {"from": relay_addr, "to": rollup, "data": json.loads(cbody)["data"]}), 16)
        base = int(chain.call("eth_getBlockByNumber", "latest", False).get("baseFeePerGas", "0x0"), 16)
        try:
            tip = int(chain.call("eth_maxPriorityFeePerGas"), 16)
        except Exception:
            tip = 0
        want = price_units(est, base + tip, 3000)
        check(st2 == 402 and int(offer2.get("amount", 0)) == want and offer2.get("extra", {}).get("quote", {}).get("gas") == int(est * 1.25) + 10000,
              "the quote at 3000: the estimate's price, %d units for %d gas (offered %s, quote gas %s)" % (want, est, offer2.get("amount"), offer2.get("extra", {}).get("quote", {}).get("gas")))
        forged = dict(offered, accepts=[dict(offered["accepts"][0], amount="1")])
        u0 = balance_usdc(wallet)
        st, _, _ = buyer._request("POST", "/commit", cbody, {"PAYMENT-SIGNATURE": buyer._payment(base64.b64encode(json.dumps(forged).encode()).decode())})
        check(st == 402 and balance_usdc(wallet) == u0, "a quote changed by the buyer (1 unit): refused, nothing paid (%d)" % st)
        st, _, _ = buyer._request("POST", "/commit", cbody, {"PAYMENT-SIGNATURE": buyer._payment(hdr)})
        paid = u0 - balance_usdc(wallet)
        holder = "0x" + call_view(rollup, "sequencerWallet()")[-40:]
        check(st == 200 and paid == quoted and holder.lower() == wallet.lower(),
              "paid at the quote after the restart: %d, the price quoted (%d units), the lease taken" % (st, paid))

        # 7. the conformance suite against this relay (#189)
        import conformance
        conformance.relay(url)
        check(conformance.fails == 0, "the conformance suite (doc/relay-protocol.md) passes against the relay")

        spent = 10_000_000 - balance_usdc(wallet)
        check(balance_eth(wallet) == 0, "the wallet's ETH balance is still 0; it spent %d units (%.4f USDC.e) in all" % (spent, spent / 1e6))

        # 8. accounts and sponsorship (#276): a treasury with USDC.e and no ETH pays for the rollup's calls; the caller
        # pays nothing and sends no payment; a cap and a release send the caller back to its own 402
        tkey = secrets.token_bytes(32)
        treasury = eth.address(tkey).lower()
        subprocess.run(["cast", "send", "--rpc-url", RPC, "--private-key", KEY0, USDC, "transfer(address,uint256)", treasury, "5000000"], check=True, capture_output=True)
        tkeyfile = os.path.join(work, "treasury.key")
        with open(tkeyfile, "w") as f:
            f.write("0x" + tkey.hex())

        wkeyfile = os.path.join(work, "wallet.key")          # the rollup's owner and sequencer
        with open(wkeyfile, "w") as f:
            f.write("0x" + wkey.hex())

        def tool(*args, **kw):
            r = subprocess.run([sys.executable, "-I", os.path.join(here, "sponsor.py"), url, "--key-file", kw.get("key", tkeyfile)]
                               + (["--rollup-key-file", kw["rollup_key"]] if kw.get("rollup_key") else []) + list(args), capture_output=True, text=True)
            try:
                return r.returncode, json.loads(r.stdout or "{}")
            except ValueError:
                return r.returncode, {"stdout": r.stdout, "stderr": r.stderr}
        rc, out = tool("permit", "5000000")
        check(rc == 0 and out.get("permit", {}).get("sent") is False, "the treasury's permit taken by the relay, not yet sent (%s %s)" % (rc, out))
        # who pays for a rollup is named from the rollup's side: a funder that is neither its owner nor its sequencer
        # needs one of them to sign beside it, so nobody can take a rollup's sponsorship over with an empty account
        rc, out = tool("sponsor", rollup)
        check(rc == 1 and "rollup's side" in out.get("error", ""), "the treasury's signature alone: 401, the rollup's side has not agreed (%s %s)" % (rc, out.get("error")))
        rc, out = tool("sponsor", rollup, rollup_key=tkeyfile)
        check(rc == 1 and "rollup's side" in out.get("error", ""), "co-signed by a key that is neither owner nor sequencer: 401 (%s)" % rc)
        rc, out = tool("sponsor", rollup, rollup_key=wkeyfile)
        check(rc == 0 and out.get("funder") == treasury and out.get("state") == "sponsored", "the treasury sponsors the rollup, the rollup's owner signing beside it (%s %s)" % (rc, out))
        skeyfile = os.path.join(work, "stranger.key")
        with open(skeyfile, "w") as f:
            f.write("0x" + secrets.token_bytes(32).hex())
        time.sleep(1.1)                                           # a newer issuedAt
        rc, out = tool("sponsor", rollup, key=skeyfile)
        rc2, now_ = tool("sponsor-of", rollup)
        check(rc == 1 and now_.get("funder") == treasury, "a stranger's newer sponsorship of the same rollup: refused, the treasury still pays (%s)" % out.get("error"))
        u0, t0 = balance_usdc(wallet), balance_usdc(treasury)
        st, body = post_raw(url + "/commit", {"to": rollup, "data": encode("renewLeaseFor(bytes32,uint256,uint256,bytes)", instance, 4, deadline, lease("renew", 4))})
        check(st == 200 and body.get("sponsor") == treasury and body.get("charged", 0) > 0,
              "renewLeaseFor on a sponsored rollup, no payment sent: 200, charged to the sponsor (%s %s)" % (st, body))
        rcpt = chain.call("eth_getTransactionReceipt", body.get("tx"))
        used = price_units(int(rcpt["gasUsed"], 16), int(rcpt["effectiveGasPrice"], 16), 3000)
        check(body.get("charged") == used,
              "the sponsor is charged the gas the call used (#294): %d units for %d gas, not the quote (charged %s)" % (used, int(rcpt["gasUsed"], 16), body.get("charged")))
        check(balance_usdc(wallet) == u0 and t0 - balance_usdc(treasury) >= 1_000_000,
              "the caller paid nothing; the relay pulled at least minPull from the treasury's permit (%d units)" % (t0 - balance_usdc(treasury)))
        st, acct = http_get(url + "/account/" + treasury)
        check(st == 200 and acct.get("credit", 0) > 0 and acct.get("permit", {}).get("sent") is True and rollup.lower() in acct.get("rollups", []),
              "the account view: credit left from the pull, the permit sent, the rollup listed (%s)" % acct)
        t1 = balance_usdc(treasury)
        st, body = post_raw(url + "/commit", {"to": rollup, "data": encode("renewLeaseFor(bytes32,uint256,uint256,bytes)", instance, 5, deadline, lease("renew", 5))})
        check(st == 200 and body.get("sponsor") == treasury and balance_usdc(treasury) == t1,
              "the next call is paid from the account's credit: nothing pulled (%s, credit %s)" % (st, http_get(url + "/account/" + treasury)[1].get("credit")))
        # the anchor, on the rollup's DA: sponsored through its coordinator
        rsig4 = eth.sign(wkey, eth.typed_digest(ddomain, "Range(bytes32 instanceId,uint256 fromHeight,uint256 toHeight,bytes32 chainHashAfter,bytes32 stateRoot,bytes32 manifestDigest)",
                                                instance, 4, 4, chain_after, root, manifest))
        st, body = post_raw(url + "/commit", {"to": da, "data": encode("commitRangeFor(bytes32,uint256,uint256,bytes32,bytes32,bytes32,bytes,bytes)", instance, 4, 4, chain_after, root, manifest, b"", rsig4)})
        check(st == 200 and body.get("sponsor") == treasury and int(call_view(da, "lastBatchHeight()"), 16) == 4 and balance_usdc(wallet) == u0,
              "commitRangeFor on the rollup's DA: sponsored too, anchored to 4, the caller still paid nothing (%s %s)" % (st, body))
        rc, out = tool("cap", rollup, "3")
        check(rc == 0 and out.get("cap", {}).get("callsPerDay") == 3 and out.get("today", {}).get("calls") == 3, "the sponsor caps the rollup at three calls a day (%s)" % out)
        renew6 = {"to": rollup, "data": encode("renewLeaseFor(bytes32,uint256,uint256,bytes)", instance, 6, deadline, lease("renew", 6))}
        st, body = post_raw(url + "/commit", renew6)
        check(st == 402 and body.get("sponsor", {}).get("why", "").startswith("capped"), "past the cap the caller gets its 402, saying the sponsor is capped (%s %s)" % (st, body.get("sponsor")))
        t1 = balance_usdc(treasury)
        st, body, settlement, paid, _ = commit(renew6["to"], renew6["data"])
        check(st == 200 and paid > 0 and "sponsor" not in body and balance_usdc(treasury) == t1, "a caller that pays its own 402 goes through, at its own cost (%s, %d units)" % (st, paid))
        rc, out = tool("release", rollup)
        check(rc == 0 and out.get("funder") is None and out.get("state") == "released", "the sponsor releases the rollup (%s)" % out)
        st, body = post_raw(url + "/commit", {"to": rollup, "data": encode("renewLeaseFor(bytes32,uint256,uint256,bytes)", instance, 7, deadline, lease("renew", 7))})
        check(st == 402 and "sponsor" not in body, "released: a plain 402 again (%s)" % st)
        rc, out = tool("sponsor-of", rollup)
        check(rc == 0 and out.get("state") == "released", "GET /sponsor/<rollup> says released")
        # an account topped up over x402 instead of a permit: the wallet pays 1 USDC.e into its own account
        u1 = balance_usdc(wallet)
        st, body, settlement = buyer.call("POST", "/account/%s/pay?amount=1000000" % wallet.lower(), b"")
        check(st == 200 and isinstance(body, dict) and body.get("credit") == 1_000_000 and u1 - balance_usdc(wallet) == 1_000_000,
              "an x402 top-up of 1 USDC.e credits the payer's account (%s %s)" % (st, body))
        st, body = post_raw(url + "/account/%s/pay?amount=5" % wallet.lower(), {})
        check(st == 400 and body.get("minPull") == 1_000_000, "a top-up below minPull: 400 (%s)" % st)
        check(balance_eth(treasury) == 0, "the treasury holds no ETH")
        # 8b. a creation paid by the treasury (#376): a key that holds nothing creates its rollup through the relay,
        # charged to the treasury's account, and the rollup is the treasury's from its first call; one message, one
        # creation
        akey, bkey = secrets.token_bytes(32), secrets.token_bytes(32)
        app = eth.address(akey).lower()
        akeyfile = os.path.join(work, "app.key")
        with open(akeyfile, "w") as f:
            f.write("0x" + akey.hex())
        cdom = eth.domain_separator("HPPLiteRelay", "1", chain_id, status["accounts"]["payTo"])
        ctype = "SponsorCreate(address owner,address funder,uint256 nonce,uint256 issuedAt)"

        def create_call(key, nonce):
            owner = eth.address(key)
            csig = eth.sign(key, eth.typed_digest(fdomain, "CreateRollup(address owner,uint256 checkpointInterval,uint256 nonce,uint256 deadline)",
                                                  owner, 100, nonce, deadline))
            return {"to": FACTORY, "data": encode("createRollupFor(address,uint256,uint256,uint256,bytes)", owner, 100, nonce, deadline, csig)}

        def sponsor_create(owner_key, nonce, cosigner=None, fkey=tkey):
            owner, funder, issued = eth.address(owner_key).lower(), eth.address(fkey).lower(), int(time.time())
            digest = eth.typed_digest(cdom, ctype, owner, funder, nonce, issued)
            b = {"owner": owner, "funder": funder, "nonce": nonce, "issuedAt": issued, "signature": "0x" + eth.sign(fkey, digest).hex()}
            if cosigner:
                b["ownerSignature"] = "0x" + eth.sign(cosigner, digest).hex()
            return post_raw(url + "/sponsor/create", b)
        st, body = post_raw(url + "/commit", create_call(akey, 0))
        check(st == 402 and "sponsor" not in body, "a creation no funder signed for: its 402 (%s)" % st)
        st, body = sponsor_create(akey, 0)
        check(st == 401 and "owner" in body.get("error", ""), "a SponsorCreate by the treasury alone: 401, the owner has not agreed (%s %s)" % (st, body.get("error")))
        st, body = sponsor_create(akey, 0, cosigner=bkey)
        check(st == 401, "signed beside the treasury by a key that is not the owner: 401 (%s)" % st)
        st, body = sponsor_create(wkey, 0, cosigner=wkey)
        check(st == 409 and body.get("createNonce") == 1, "a nonce the factory already spent for its owner: 409, saying the createNonce (%s %s)" % (st, body))
        # a creation is the dearest call (4.3M gas, about 13 USDC.e at this relay's 3000 USD an ETH): the treasury's
        # account is given what pays it
        subprocess.run(["cast", "send", "--rpc-url", RPC, "--private-key", KEY0, USDC, "transfer(address,uint256)", treasury, "40000000"], check=True, capture_output=True)
        rc, out = tool("permit", "40000000")
        check(rc == 0 and out.get("permit", {}).get("value") == 40000000, "the treasury's permit for a creation's price (%s)" % rc)
        rc, out = tool("create", rollup_key=akeyfile)
        check(rc == 0 and out.get("funder") == treasury and out.get("owner") == app and out.get("state") == "waiting",
              "sponsor.py create: the treasury signs for the app key's creation, the app's key beside it (%s %s)" % (rc, out))
        st, body = post_raw(url + "/commit", create_call(bkey, 0))
        check(st == 402 and "sponsor" not in body, "another owner's creation is not the treasury's: its 402 (%s)" % st)
        c0, tb = http_get(url + "/account/" + treasury)[1].get("credit", 0), balance_usdc(treasury)
        st, body = post_raw(url + "/commit", create_call(akey, 0))
        rollup2 = "0x" + call_view(FACTORY, "getRollup(address)", app)[-40:]
        c1 = http_get(url + "/account/" + treasury)[1]
        check(st == 200 and body.get("sponsor") == treasury and body.get("charged", 0) > 0 and int(rollup2, 16) != 0,
              "the app key's createRollupFor, no payment sent: 200, rollup %s, charged to the treasury (%s %s)" % (rollup2, st, body))
        check(balance_eth(app) == 0 and balance_usdc(app) == 0 and c0 + (tb - balance_usdc(treasury)) - c1.get("credit", 0) == body.get("charged"),
              "the app's key holds nothing, before and after; the treasury's account paid %s units" % body.get("charged"))
        st, sp = http_get(url + "/sponsor/" + rollup2.lower())
        check(st == 200 and sp.get("funder") == treasury and sp.get("state") == "sponsored" and rollup2.lower() in c1.get("rollups", []),
              "the rollup it created is sponsored by the treasury already (%s %s)" % (st, sp))
        hd2, inst2 = eth.domain_separator("HPPLite", "10", chain_id, rollup2), eth.keccak256(b"a sponsored creation")
        claim2 = eth.sign(akey, eth.typed_digest(hd2, lease_type, eth.keccak256(b"claim"), inst2, 0, deadline))
        st, body = post_raw(url + "/commit", {"to": rollup2, "data": encode("claimSequencerFor(bytes32,uint256,uint256,bytes)", inst2, 0, deadline, claim2)})
        check(st == 200 and body.get("sponsor") == treasury and ("0x" + call_view(rollup2, "sequencerWallet()")[-40:]).lower() == app,
              "its first call, the claim: sponsored, no second step (%s %s)" % (st, body))
        st, body = post_raw(url + "/commit", create_call(akey, 1))
        check(st == 402 and "sponsor" not in body, "a second creation by the same key: its 402, the message paid one (%s)" % st)
        st, body = sponsor_create(akey, 0, cosigner=akey)
        check(st == 409 and body.get("createNonce") == 1, "the same nonce signed for again: 409, spent (%s)" % st)
        ekey = secrets.token_bytes(32)                         # a funder with an empty account
        st, body = sponsor_create(bkey, 0, cosigner=bkey, fkey=ekey)
        st2, body2 = post_raw(url + "/commit", create_call(bkey, 0))
        check(st == 200 and st2 == 402 and body2.get("sponsor", {}).get("funder") == eth.address(ekey).lower() and body2["sponsor"].get("why"),
              "a funder whose account cannot pay: the creation's 402 says so (%s %s %s)" % (st, st2, body2.get("sponsor")))
        # 9. a smart wallet as the sponsor (#356): its ERC-1271 signatures (66 bytes) give the permit and the sponsorship,
        # the rollup's owner signing beside it; the relay pulls from the wallet, its permit sent on chain as bytes
        import re
        st, status = http_get(url + "/status")
        acc = status["accounts"]
        W = secrets.token_bytes(32)
        made = subprocess.run(["forge", "create", "--rpc-url", RPC, "--private-key", KEY0, "--broadcast", "--json",
                               "src/test/MockWallet1271.sol:MockWallet1271", "--constructor-args", eth.address(W)],
                              cwd=os.path.join(here, "..", "..", "contracts"), capture_output=True, text=True)
        m = re.search(r'"deployedTo":\s*"(0x[0-9a-fA-F]{40})"', made.stdout)
        swallet = m.group(1).lower() if m else ""
        check(len(swallet) == 42, "a smart wallet (MockWallet1271) deployed (%s)" % (swallet or made.stderr[-200:]))
        subprocess.run(["cast", "send", "--rpc-url", RPC, "--private-key", KEY0, USDC, "transfer(address,uint256)", swallet, "5000000"], check=True, capture_output=True)
        wsig = lambda d: b"\x00" + eth.sign(W, d)          # the wallet's form: a mode byte, then its owner's signature
        nonce, dl = int(call_view(USDC, "nonces(address)", swallet), 16), int(time.time()) + 86400
        pd = eth.typed_digest(eth.domain_separator(acc["assetDomain"]["name"], acc["assetDomain"]["version"], int(status["chainId"]), acc["asset"]),
                              "Permit(address owner,address spender,uint256 value,uint256 nonce,uint256 deadline)", swallet, acc["spender"], 5_000_000, nonce, dl)
        st, out = post_raw(url + "/account/%s/permit" % swallet, {"value": 5_000_000, "deadline": dl, "signature": "0x" + wsig(pd).hex()})
        check(st == 200 and out.get("permit", {}).get("sent") is False, "the smart wallet's permit taken by the relay (%s %s)" % (st, out))
        dom = acc["domain"]
        sd = eth.typed_digest(eth.domain_separator(dom["name"], dom["version"], int(dom["chainId"]), dom["verifyingContract"]),
                              "Sponsor(address rollup,address funder,uint256 issuedAt)", rollup.lower(), swallet, int(time.time()) + 2)
        st, out = post_raw(url + "/sponsor/" + rollup.lower(), {"funder": swallet, "issuedAt": int(time.time()) + 2,
                                                                "signature": "0x" + wsig(sd).hex(), "rollupSignature": "0x" + eth.sign(wkey, sd).hex()})
        check(st == 200 and out.get("funder") == swallet and out.get("state") == "sponsored",
              "the smart wallet sponsors the rollup by its ERC-1271 signature, the owner signing beside it (%s %s)" % (st, out))
        check(out.get("cap") is None, "a new sponsor after a release starts uncapped: the cap went with the treasury (#359) (%s)" % out.get("cap"))
        # a Cap signed by the wallet (ERC-1271) is taken too: it sets none
        ct = int(time.time()) + 3
        cd = eth.typed_digest(eth.domain_separator(dom["name"], dom["version"], int(dom["chainId"]), dom["verifyingContract"]),
                              "Cap(address rollup,uint256 callsPerDay,uint256 issuedAt)", rollup.lower(), 0, ct)
        st, out = post_raw(url + "/sponsor/%s/cap" % rollup.lower(), {"funder": swallet, "callsPerDay": 0, "issuedAt": ct, "signature": "0x" + wsig(cd).hex()})
        check(st == 200 and out.get("cap") is None, "the smart wallet lifts the rollup's cap by its ERC-1271 signature (%s %s)" % (st, out))
        s0 = balance_usdc(swallet)
        st, body = post_raw(url + "/commit", {"to": rollup, "data": encode("renewLeaseFor(bytes32,uint256,uint256,bytes)", instance, 7, deadline, lease("renew", 7))})
        check(st == 200 and body.get("sponsor") == swallet and s0 - balance_usdc(swallet) >= 1_000_000,
              "a sponsored call charged to the smart wallet, the relay pulling from it (%s, %d units)" % (st, s0 - balance_usdc(swallet)))
        # 10. one relay, several tokens (#361): a sponsor paying by an HPP permit (18 decimals: a pull of minPull, 1000000
        # units, is 100 HPP, past 2^53), and a wallet holding only CHIP paying its call over x402
        if CHIP and HPP:
            assets = {x["asset"]: x for x in acc.get("assets") or []}
            check(list(assets)[:1] == [USDC.lower()] and assets.get(HPP.lower(), {}).get("unitsPerToken") == "10000"
                  and assets.get(CHIP.lower(), {}).get("exact") is True, "/status lists the tokens: USDC.e first, CHIP exact, HPP at 10000 units (%s)" % acc.get("assets"))
            hkey = secrets.token_bytes(32)
            hfund = eth.address(hkey).lower()
            total = 10_000 * 10 ** 18
            subprocess.run(["cast", "send", "--rpc-url", RPC, "--private-key", KEY0, HPP, "transfer(address,uint256)", hfund, str(total)], check=True, capture_output=True)
            hpp_of = lambda a: int(call_view(HPP, "balanceOf(address)", a), 16)
            value, dl = 10 ** 21 + 1, int(time.time()) + 86400
            pd = eth.typed_digest(eth.domain_separator("HousePartyProtocol", "1", int(status["chainId"]), HPP),
                                  "Permit(address owner,address spender,uint256 value,uint256 nonce,uint256 deadline)", hfund, acc["spender"], value, 0, dl)
            permit = {"value": str(value), "deadline": dl, "signature": "0x" + eth.sign(hkey, pd).hex()}
            st, out = post_raw(url + "/account/%s/permit" % hfund, permit)
            check(st == 401, "an HPP permit without its asset is checked in USDC.e's domain: 401 (%s)" % st)
            st, out = post_raw(url + "/account/%s/permit" % hfund, dict(permit, asset="0x" + "22" * 20))
            check(st == 400, "a permit in a token the relay does not take: 400 (%s)" % st)
            st, out = post_raw(url + "/account/%s/permit" % hfund, dict(permit, asset=HPP))
            check(st == 200 and out.get("asset") == HPP.lower() and (out.get("permit") or {}).get("value") == str(value) and out.get("balance") == str(total),
                  "an HPP permit taken: value and balance exact, decimal strings past 2^53 (%s %s)" % (st, out))
            it = int(time.time()) + 5
            sd = eth.typed_digest(eth.domain_separator(dom["name"], dom["version"], int(dom["chainId"]), dom["verifyingContract"]),
                                  "Sponsor(address rollup,address funder,uint256 issuedAt)", rollup.lower(), hfund, it)
            st, out = post_raw(url + "/sponsor/" + rollup.lower(), {"funder": hfund, "issuedAt": it, "signature": "0x" + eth.sign(hkey, sd).hex(),
                                                                    "rollupSignature": "0x" + eth.sign(wkey, sd).hex()})
            check(st == 200 and out.get("funder") == hfund, "the HPP holder sponsors the rollup (%s %s)" % (st, out))
            r0 = hpp_of(relay_addr)
            st, body = post_raw(url + "/commit", {"to": rollup, "data": encode("renewLeaseFor(bytes32,uint256,uint256,bytes)", instance, 8, deadline, lease("renew", 8))})
            pulled = hpp_of(relay_addr) - r0
            check(st == 200 and body.get("sponsor") == hfund and pulled == 10 ** 20 and hpp_of(hfund) == total - 10 ** 20,
                  "a sponsored call charged to the HPP account: minPull pulled as 100 HPP exactly, past 2^53 (%s, %d)" % (st, pulled))
            st, out = http_get(url + "/account/" + hfund)
            check(st == 200 and out.get("credit", 0) > 0 and (out.get("permit") or {}).get("sent") is True,
                  "the account keeps the rest as credit; the HPP permit sent (%s)" % out)
            ckey = secrets.token_bytes(32)
            cwallet = eth.address(ckey)
            subprocess.run(["cast", "send", "--rpc-url", RPC, "--private-key", KEY0, CHIP, "transfer(address,uint256)", cwallet, "50000000"], check=True, capture_output=True)
            chip_of = lambda a: int(call_view(CHIP, "balanceOf(address)", a), 16)
            cdl = int(chain.call("eth_getBlockByNumber", "latest", False)["timestamp"], 16) + 3600
            csig = eth.sign(ckey, eth.typed_digest(fdomain, "CreateRollup(address owner,uint256 checkpointInterval,uint256 nonce,uint256 deadline)",
                                                  cwallet, 100, 0, cdl))
            cdata = encode("createRollupFor(address,uint256,uint256,uint256,bytes)", cwallet, 100, 0, cdl, csig)
            st, body = post_raw(url + "/commit", {"to": FACTORY, "data": cdata})
            offers = [o for o in body.get("accepts", []) if o.get("scheme") == "exact"] if isinstance(body, dict) else []
            check(st == 402 and [o["asset"].lower() for o in offers] == [USDC.lower(), CHIP.lower()] and offers[0]["amount"] == offers[1]["amount"]
                  and offers[1]["extra"].get("name") == "House Chip" and offers[1]["extra"].get("quote"),
                  "an unsponsored call's 402 offers exact in USDC.e, then CHIP at the same amount with its own quote; not HPP (%s)" % offers)
            c0, rc0 = chip_of(cwallet), chip_of(relay_addr)
            st, body, settlement = Buyer(url, ckey, cap=50_000_000, valid_for=3600, asset=CHIP).call("POST", "/commit", json.dumps({"to": FACTORY, "data": cdata}).encode())
            check(st == 200 and settlement and settlement.get("success") and c0 - chip_of(cwallet) == chip_of(relay_addr) - rc0 > 0
                  and balance_usdc(cwallet) == 0,
                  "a wallet holding only CHIP pays its call in CHIP over x402 (%s, %d units)" % (st, c0 - chip_of(cwallet)))
            # x402 upto in HPP (#358): a wallet holding only HPP, no ETH, never having approved Permit2, pays its call
            ukey = secrets.token_bytes(32)
            uwallet = eth.address(ukey)
            subprocess.run(["cast", "send", "--rpc-url", RPC, "--private-key", KEY0, HPP, "transfer(address,uint256)", uwallet, str(10_000 * 10 ** 18)], check=True, capture_output=True)
            udl = int(chain.call("eth_getBlockByNumber", "latest", False)["timestamp"], 16) + 3600
            usig = eth.sign(ukey, eth.typed_digest(fdomain, "CreateRollup(address owner,uint256 checkpointInterval,uint256 nonce,uint256 deadline)",
                                                  uwallet, 100, 0, udl))
            udata = encode("createRollupFor(address,uint256,uint256,uint256,bytes)", uwallet, 100, 0, udl, usig)
            st, body = post_raw(url + "/commit", {"to": FACTORY, "data": udata})
            uoffer = next((o for o in (body.get("accepts") or []) if o.get("scheme") == "upto"), None) if isinstance(body, dict) else None
            check(st == 402 and uoffer and uoffer["asset"].lower() == HPP.lower() and uoffer["extra"].get("facilitatorAddress") and uoffer["extra"].get("quote")
                  and (body.get("extensions") or {}).get("eip2612GasSponsoring"),
                  "the 402 offers upto in HPP, naming the facilitator, with its quote, and declares the EIP-2612 extension (%s)" % uoffer)
            u0 = hpp_of(uwallet)
            st, body, settlement = Buyer(url, ukey, cap=10_000 * 10 ** 18, valid_for=3600, asset=HPP, scheme="upto", rpc=RPC).call(
                "POST", "/commit", json.dumps({"to": FACTORY, "data": udata}).encode())
            check(st == 200 and settlement and settlement.get("success") and u0 - hpp_of(uwallet) == int(uoffer["amount"]) > 2 ** 53
                  and balance_eth(uwallet) == 0,
                  "a wallet holding only HPP pays its call by x402 upto, Permit2 approved through the facilitator: the quote, past 2^53, and no ETH (%s, %d)" % (st, u0 - hpp_of(uwallet)))
        queue_section(work, keyfile, chain_id)
    finally:
        proc.terminate()
        proc.wait()
    if fails:
        print("--- relay log"); print(open(os.path.join(work, "relay.log")).read()[-3000:])
    print("relay selftest: %s" % ("PASS" if not fails else "%d FAIL" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
