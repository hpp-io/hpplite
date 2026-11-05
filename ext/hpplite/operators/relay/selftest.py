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


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


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
                          "--asset", USDC, "--eth-usd", eth_usd, "--dir", os.path.join(work, "relay_state")], stdout=log, stderr=log)
    for _ in range(100):
        if p.poll() is not None:
            return p, None
        try:
            with urllib.request.urlopen(url + "/status", timeout=2) as r:
                return p, json.loads(r.read())
        except OSError:
            time.sleep(0.1)
    return p, None


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
    finally:
        proc.terminate()
        proc.wait()
    if fails:
        print("--- relay log"); print(open(os.path.join(work, "relay.log")).read()[-3000:])
    print("relay selftest: %s" % ("PASS" if not fails else "%d FAIL" % fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
