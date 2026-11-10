#!/usr/bin/env python3
"""hpplite-store/2's selftest on anvil (#213, doc/store-protocol.md), run by
tests/l1/anvil_test.sh: real rollups from the factory, MockUSDC with
EIP-2612 permit (#212), the mock facilitator for x402. A funding wallet with
USDC.e and no ETH binds a rollup's bucket and gives a permit; the sequencer
writes names and content; storage accrues and the store pulls it; the wallet
emptied, a pull is refused and the bucket parks; refilled, it is covered
again. A second funder pays by x402 only, its credit runs out, its bucket
parks and is deleted. Prune, release, the statement, the sweep.
"""
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
from x402seller import eth  # noqa: E402
from buyer import Buyer  # noqa: E402

RPC = os.environ["HPPLITE_ANVIL_RPC"]
USDC = os.environ["HPPLITE_ANVIL_USDC"]
FACTORY = os.environ["HPPLITE_ANVIL_FACTORY"]
FACILITATOR = os.environ["HPPLITE_ANVIL_FACILITATOR"]
KEY0, KEY1, KEY2, KEY3 = (os.environ["HPPLITE_ANVIL_KEY%d" % i] for i in range(4))
CHAIN_ID = 31337
fails = 0

BIND = "Bind(address da,address funder,uint256 issuedAt)"
RELEASE = "Release(address da,address funder,uint256 issuedAt)"
CONTENT = "Content(address da,uint256 height,bytes32 hash)"
PRUNE = "Prune(address da,uint256 height,uint256 issuedAt)"
STATEMENT = "BucketStatement(address da,uint256 height,uint256 pruneHeight,uint256 coveredUntil,uint256 issuedAt)"
PERMIT = "Permit(address owner,address spender,uint256 value,uint256 nonce,uint256 deadline)"
BATCH = "Batch(bytes32 instanceId,uint256 height,bytes32 batchHash)"


def check(cond, what):
    global fails
    print(("  ok    " if cond else "  FAIL  ") + what, flush=True)
    if not cond:
        fails += 1


def cast(*args):
    return subprocess.run(["cast"] + list(args) + ["--rpc-url", RPC], capture_output=True, text=True, check=True).stdout.strip()


def send(key, to, sig, *args):
    cast("send", "--private-key", key, to, sig, *[str(a) for a in args])


def usdc_of(a):
    return int(cast("call", USDC, "balanceOf(address)(uint256)", a).split()[0])


def key_bytes(k):
    return bytes.fromhex(k[2:] if k.startswith("0x") else k)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def http(method, url, body=None, headers=None):
    req = urllib.request.Request(url, data=body, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            data = r.read()
            return r.status, (json.loads(data) if r.headers.get("Content-Type", "").startswith("application/json") else data), dict(r.headers)
    except urllib.error.HTTPError as e:
        data = e.read()
        try:
            return e.code, json.loads(data), dict(e.headers)
        except ValueError:
            return e.code, data, dict(e.headers)


def jpost(url, obj):
    return http("POST", url, json.dumps(obj).encode(), {"Content-Type": "application/json"})


def rollup_of(owner_key):
    """a rollup created by the owner (its sequencer too, by default), and its DA"""
    send(owner_key, FACTORY, "createRollup(uint256)", 100)
    owner = eth.address(key_bytes(owner_key))
    rollup = cast("call", FACTORY, "getRollup(address)(address)", owner)
    da = cast("call", FACTORY, "getDA(address)(address)", rollup)
    return rollup, da.lower()


class Store:
    def __init__(self, url, status):
        self.url = url
        s = status
        self.domain = eth.domain_separator("HPPLiteStore", "2", CHAIN_ID, s["domain"]["verifyingContract"])
        self.spender = s["buckets"]["spender"]
        self.signer = s["signer"]

    def digest(self, typ, *fields):
        return eth.typed_digest(self.domain, typ, *fields)

    def bind(self, da, seq_key, funder_key, issued=None):
        funder = eth.address(funder_key).lower()
        issued = issued or int(time.time())
        d = self.digest(BIND, da, funder, issued)
        return jpost(self.url + "/bucket/" + da, {"funder": funder, "issuedAt": issued,
                                                  "sequencerSignature": "0x" + eth.sign(seq_key, d).hex(),
                                                  "funderSignature": "0x" + eth.sign(funder_key, d).hex()})

    def bind_alone(self, da, funder_key, issued=None):
        """Bind with the funder's signature only (#273): taken when the funder is the rollup's owner on chain"""
        funder = eth.address(funder_key).lower()
        issued = issued or int(time.time())
        d = self.digest(BIND, da, funder, issued)
        return jpost(self.url + "/bucket/" + da, {"funder": funder, "issuedAt": issued, "funderSignature": "0x" + eth.sign(funder_key, d).hex()})

    def permit(self, funder_key, value, deadline):
        funder = eth.address(funder_key)
        nonce = int(cast("call", USDC, "nonces(address)(uint256)", funder).split()[0])
        domain = eth.domain_separator("Bridged USDC", "2", CHAIN_ID, USDC)
        d = eth.typed_digest(domain, PERMIT, funder, self.spender, value, nonce, deadline)
        return jpost(self.url + "/account/%s/permit" % funder.lower(),
                     {"value": value, "deadline": deadline, "signature": "0x" + eth.sign(funder_key, d).hex()})


def name_body(da, height, data, instance, key):
    domain = eth.domain_separator("HPPLiteDA", "3", CHAIN_ID, da)
    d = eth.typed_digest(domain, BATCH, instance, height, eth.keccak256(data))
    return data + instance + eth.sign(key, d)


def wait_for(cond, seconds=20):
    end = time.time() + seconds
    while time.time() < end:
        v = cond()
        if v:
            return v
        time.sleep(0.25)
    return cond()


# hpp-io/hpplite-operators runs this selftest against its store Worker: it replaces start_store (its store under
# wrangler dev, these flags as vars) and leaves out the sweep check, which writes into the reference's directory
CHECK_SWEEP_FILES = True


def start_store(work, port, args):
    """the reference store with these flags, its log in work/store.log: an object with terminate() and wait()"""
    log = open(os.path.join(work, "store.log"), "w")
    return subprocess.Popen([sys.executable, "-I", os.path.join(here, "hpplite_store.py"), "--port", str(port)] + args, stdout=log, stderr=log)


def main():
    k1, k3 = key_bytes(KEY1), key_bytes(KEY3)
    _, da = rollup_of(KEY1)
    _, da2 = rollup_of(KEY3)
    instance = secrets.token_bytes(32)
    send(KEY1, cast("call", FACTORY, "getRollup(address)(address)", eth.address(k1)), "claimSequencer(bytes32)", "0x" + instance.hex())
    F, G = secrets.token_bytes(32), secrets.token_bytes(32)        # funding wallets: USDC.e, no ETH
    fa, ga = eth.address(F).lower(), eth.address(G).lower()
    send(KEY0, USDC, "transfer(address,uint256)", fa, 5_000_000)
    send(KEY0, USDC, "transfer(address,uint256)", ga, 5_000_000)
    pay_to = eth.address(secrets.token_bytes(32)).lower()

    work = tempfile.mkdtemp(prefix="hpplite-buckets.")
    with open(os.path.join(work, "signer.key"), "w") as f:
        f.write(KEY2)                                   # signs statements and pulls: anvil's ETH pays the gas
    port = free_port()
    url = "http://127.0.0.1:%d" % port
    proc = start_store(work, port, ["--dir", os.path.join(work, "store"), "--signer-key", os.path.join(work, "signer.key"),
                                    "--pay-to", pay_to, "--rpc", RPC, "--facilitator", FACILITATOR, "--network", "eip155:%d" % CHAIN_ID,
                                    "--asset", USDC, "--tick-interval", "0.5", "--price-per-mib-month", "1e12", "--price-per-write", "10",
                                    "--pull-threshold", "2000", "--pull-interval", "100000", "--retry-seconds", "1", "--park-seconds", "4",
                                    "--min-pay", "3000", "--sweep-age", "2"])
    try:
        st = wait_for(lambda: http("GET", url + "/status")[1] if _up(url) else None)
        check(isinstance(st, dict) and st.get("protocol") == "hpplite-store/2", "the store speaks hpplite-store/2")
        S = Store(url, st)

        # --- binding ---
        code, out, _ = S.bind(da, k3, F)
        check(code == 401, "a binding signed by another key than the rollup's sequencer: 401 (%s)" % code)
        code, out, _ = S.bind("0x" + "11" * 20, k1, F)
        check(code in (404, 503), "a binding for a DA with no rollup: refused (%s)" % code)
        code, out, _ = S.bind_alone(da, k3, issued=int(time.time()) - 60)
        check(code == 401, "a binding signed by the funder alone, a funder that is not the rollup's owner: 401 (%s)" % code)
        code, out, _ = S.bind_alone(da, k1, issued=int(time.time()) - 50)
        check(code == 200 and out.get("state") == "covered" and out.get("funder") == eth.address(k1).lower(),
              "a binding signed by the funder alone when the funder is the rollup's owner (#273): covered, the owner the funder (%s %s)" % (code, out))
        code, out, _ = S.bind(da, k1, F)
        check(code == 200 and out.get("state") == "covered", "bound by the sequencer and the funder: covered (%s %s)" % (code, out))
        code, out, _ = S.bind(da, k1, F, issued=int(time.time()) - 100)
        check(code == 409, "an older binding again: 409 (%s)" % code)

        # --- no money: a write answers 402 ---
        body1 = name_body(da, 1, b"batch one " * 20, instance, k1)
        code, out, hdr = http("POST", "%s/name/%s-1" % (url, da[2:]), body1)
        check(code == 402 and out.get("error") == "account_uncovered" and "PAYMENT-REQUIRED" in {k.upper() for k in hdr},
              "a name into a bucket whose account has nothing: 402 account_uncovered, with x402 requirements")

        # --- a permit: writes go through, paid later by a pull ---
        code, out, _ = S.permit(F, 1_000_000, int(time.time()) + 86400)
        check(code == 200 and out.get("permit", {}).get("sent") is False, "the funder's permit taken, not yet sent (%s)" % code)
        code, out, _ = http("POST", "%s/name/%s-1" % (url, da[2:]), body1)
        check(code == 200, "a name, the account able to pull: 200 (%s %s)" % (code, out))
        code, data, hdr = http("GET", "%s/name/%s-1" % (url, da[2:]))
        check(code == 200 and data == body1 and hdr.get("X-HPPLite-Signer", "").lower() == eth.address(k1).lower(),
              "the name reads back, with its signer")
        code, data, _ = http("GET", "%s/get/%s" % (url, _cid(body1[:-97])))
        check(code == 200 and data == body1[:-97], "a name's bytes read by their CID")
        code, out, _ = http("POST", "%s/name/%s-2" % (url, da[2:]), name_body(da, 2, b"x" * 50, secrets.token_bytes(32), k1))
        check(code == 409, "a name from another instance than the lease's: 409 (%s)" % code)
        code, out, _ = http("POST", "%s/name/%s-2" % (url, da[2:]), name_body(da, 2, b"x" * 50, instance, k3))
        check(code == 403, "a name signed by another key: 403 (%s)" % code)
        for h in (2, 3, 4):
            http("POST", "%s/name/%s-%d" % (url, da[2:], h), name_body(da, h, (b"batch %d " % h) * 30, instance, k1))
        blob = b"a manifest" * 10
        sig = eth.sign(k1, S.digest(CONTENT, da, 4, eth.keccak256(blob)))
        code, out, _ = http("POST", "%s/put?bucket=%s&height=4" % (url, da), blob, {"X-HPPLite-Signature": "0x" + sig.hex()})
        check(code == 200 and out.get("cid") == _cid(blob), "a content put signed by the sequencer: 200")
        code, out, _ = http("POST", "%s/put?bucket=%s&height=4" % (url, da), b"other", {"X-HPPLite-Signature": "0x" + sig.hex()})
        check(code in (401, 403), "a content put whose signature is not over its bytes: refused (%s)" % code)
        code, data, _ = http("GET", "%s/get/%s" % (url, _cid(blob)))
        check(code == 200 and data == blob, "the content reads back by CID")
        before_bytes = http("GET", url + "/bucket/" + da)[1]["bytes"]
        same_bytes = (b"batch %d " % 4) * 30
        sig = eth.sign(k1, S.digest(CONTENT, da, 4, eth.keccak256(same_bytes)))
        code, out, _ = http("POST", "%s/put?bucket=%s&height=4" % (url, da), same_bytes, {"X-HPPLite-Signature": "0x" + sig.hex()})
        check(code == 200 and out.get("held") and http("GET", url + "/bucket/" + da)[1]["bytes"] == before_bytes,
              "a content put of a name's bytes: held already, counted once")
        acct = http("GET", url + "/account/" + fa)[1]
        check(acct.get("permitNonce") == 0 and st["buckets"].get("assetDomain", {}).get("name") == "Bridged USDC",
              "the account view gives the permit nonce; /status the token's domain")

        # --- the store pulls what accrues ---
        before = usdc_of(pay_to)
        pulled = wait_for(lambda: usdc_of(pay_to) > before, 30)
        check(pulled, "storage accrued past the threshold: the store pulled from the funding wallet (%d units)" % (usdc_of(pay_to) - before))
        # the transfer is on the chain before the store has seen its receipt: wait for the store's own record of the pull
        acct = wait_for(lambda: (lambda a: a if (a.get("permit") or {}).get("sent") else None)(http("GET", url + "/account/" + fa)[1]), 10) \
            or http("GET", url + "/account/" + fa)[1]
        check(acct.get("permit", {}).get("sent") is True and acct.get("state") == "covered", "the permit was sent with the pull; covered")
        check(int(cast("balance", fa)) == 0, "the funding wallet still holds no ETH")

        # --- the wallet emptied: the pull is refused, the bucket parks ---
        cast("rpc", "anvil_setBalance", fa, "0x2386f26fc10000")          # gas for the funder's own transfer away
        send("0x" + F.hex(), USDC, "transfer(address,uint256)", eth.address(key_bytes(KEY0)), usdc_of(fa))
        parked = wait_for(lambda: http("GET", url + "/bucket/" + da)[1].get("state") == "parked", 30)
        check(parked, "the wallet empty: a pull refused, the bucket parked")
        acct = http("GET", url + "/account/" + fa)[1]
        check(acct.get("state") == "uncovered" and "refused" in acct.get("why", ""), "the account says why: %s" % acct.get("why"))
        code, out, _ = http("GET", "%s/name/%s-1" % (url, da[2:]))
        check(code == 410, "a parked bucket's name: 410 (%s)" % code)
        code, out, _ = http("POST", "%s/name/%s-5" % (url, da[2:]), name_body(da, 5, b"five", instance, k1))
        check(code == 402 and out.get("error") == "bucket_parked", "a write into a parked bucket: 402 bucket_parked (%s)" % code)

        # --- refilled: covered again at the next try ---
        send(KEY0, USDC, "transfer(address,uint256)", fa, 2_000_000)
        covered = wait_for(lambda: http("GET", url + "/bucket/" + da)[1].get("state") == "covered", 30)
        check(covered, "the wallet refilled: the retried pull succeeds, the bucket covered again")
        code, data, _ = http("GET", "%s/name/%s-1" % (url, da[2:]))
        check(code == 200 and data == body1, "its names read again")

        # --- the statement ---
        b = http("GET", url + "/bucket/" + da)[1]
        s = b.get("statement") or {}
        d = S.digest(STATEMENT, da, s.get("height", 0), s.get("pruneHeight", 0), s.get("coveredUntil", 0), s.get("issuedAt", 0))
        check(s.get("height") == 4 and eth.recover(d, bytes.fromhex(s["signature"][2:])).lower() == S.signer.lower()
              and s.get("coveredUntil", 0) > time.time(), "the bucket statement: height 4, covered ahead, signed by the store's signer")

        # --- prune ---
        issued = int(time.time())
        psig = eth.sign(k1, S.digest(PRUNE, da, 3, issued))
        code, out, _ = jpost(url + "/bucket/%s/prune" % da, {"height": 3, "issuedAt": issued, "signature": "0x" + psig.hex()})
        check(code == 200 and out.get("pruneHeight") == 3, "a prune at 3 by the sequencer: 200")
        check(http("GET", "%s/name/%s-2" % (url, da[2:]))[0] == 404 and http("GET", "%s/name/%s-3" % (url, da[2:]))[0] == 200,
              "names below 3 gone, 3 kept")
        psig = eth.sign(k3, S.digest(PRUNE, da, 4, issued + 1))
        code, _, _ = jpost(url + "/bucket/%s/prune" % da, {"height": 4, "issuedAt": issued + 1, "signature": "0x" + psig.hex()})
        check(code in (401, 403), "a prune by another key: refused (%s)" % code)

        # --- x402 only: a funder with no permit pays a 402, its credit runs out, the bucket parks, then is deleted ---
        code, out, _ = S.bind(da2, k3, G)
        check(code == 200, "a second rollup bound to a funder with no permit")
        inst2 = secrets.token_bytes(32)
        send(KEY3, cast("call", FACTORY, "getRollup(address)(address)", eth.address(k3)), "claimSequencer(bytes32)", "0x" + inst2.hex())
        buyer = Buyer(url, G, cap=100_000)
        g_before = usdc_of(ga)
        code, out, settled = buyer.call("POST", "/name/%s-1" % da2[2:], name_body(da2, 1, b"agent batch " * 20, inst2, k3))
        check(code == 200 and settled and settled.get("success"), "the write's 402 paid by x402 from the funder: 200 (%s)" % code)
        check(g_before - usdc_of(ga) >= 3000, "the top-up moved at least minPay from the funder")
        acct = http("GET", url + "/account/" + ga)[1]
        check(acct.get("credit", 0) > 0, "the account holds credit (%s)" % acct.get("credit"))
        gone = wait_for(lambda: http("GET", url + "/bucket/" + da2)[1].get("state") == "parked", 40)
        check(gone, "its credit spent by storage and no permit: parked")
        deleted = wait_for(lambda: http("GET", url + "/bucket/" + da2)[1].get("state") == "deleted", 20)
        check(deleted and http("GET", "%s/name/%s-1" % (url, da2[2:]))[0] == 404, "parkSeconds later: deleted, its names gone")

        # --- the owner rotates the sequencer key (#208): the new key takes the bucket, the old one is refused ---
        rollup1 = cast("call", FACTORY, "getRollup(address)(address)", eth.address(k1))
        k2 = key_bytes(KEY2)
        send(KEY1, rollup1, "setSequencer(address)", eth.address(k2))
        code, out, _ = S.bind(da, k2, F)
        check(code == 200, "after setSequencer: the new key rebinds the bucket with its funder (%s %s)" % (code, out))
        inst_new = secrets.token_bytes(32)
        send(KEY2, rollup1, "claimSequencer(bytes32)", "0x" + inst_new.hex())
        code, _, _ = http("POST", "%s/name/%s-5" % (url, da[2:]), name_body(da, 5, b"by the new key", inst_new, k2))
        check(code == 200, "the new key's name at 5: 200 (%s)" % code)
        code, _, _ = http("POST", "%s/name/%s-6" % (url, da[2:]), name_body(da, 6, b"by the rotated-out key", instance, k1))
        check(code in (403, 409), "the rotated-out key's name: refused (%s)" % code)
        old_blob = b"content by the rotated-out key"
        sig = eth.sign(k1, S.digest(CONTENT, da, 6, eth.keccak256(old_blob)))
        code, _, _ = http("POST", "%s/put?bucket=%s&height=6" % (url, da), old_blob, {"X-HPPLite-Signature": "0x" + sig.hex()})
        check(code == 403, "the rotated-out key's content put: 403 (%s)" % code)

        # --- release ---
        issued = int(time.time()) + 1
        rsig = eth.sign(F, S.digest(RELEASE, da, fa, issued))
        code, out, _ = jpost(url + "/bucket/%s/release" % da, {"funder": fa, "issuedAt": issued, "signature": "0x" + rsig.hex()})
        check(code == 200 and out.get("state") == "parked", "released by its funder: parked (%s)" % code)

        # --- sweep ---
        if CHECK_SWEEP_FILES:
            junk = os.path.join(work, "store", "c", _cid(b"interrupted"))
            with open(junk, "wb") as f:
                f.write(b"interrupted")
            check(wait_for(lambda: not os.path.exists(junk), 15), "bytes in no bucket swept")
    finally:
        proc.terminate()
        proc.wait()
        if fails:
            sys.stdout.write(open(os.path.join(work, "store.log")).read()[-3000:])
    print("bucket selftest: %s" % ("FAIL (%d)" % fails if fails else "ok"))
    return 1 if fails else 0


def _up(url):
    try:
        with urllib.request.urlopen(url + "/status", timeout=2):
            return True
    except OSError:
        return False


def _cid(data):
    sys.path.insert(0, here)
    from hpplite_store import cid_of
    return cid_of(data)


if __name__ == "__main__":
    sys.exit(main())
