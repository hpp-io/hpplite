#!/usr/bin/env python3
"""hpplite-store/2's edges, without a chain (#213): what the /1 store's
settle and lease selftests (#194, #198) checked, as buckets have them.

- A top-up whose settlement fails is taken back: the work runs after
  /verify and before /settle, so the credit the payment gave is removed
  again when the facilitator fails to settle; the write stays, owed - the
  account is not covered by money it never got. A facilitator in this
  process verifies everything and settles or fails as told.
- A store with --rpc that cannot read the chain refuses what needs it: a
  binding and a name are answered 503 and nothing is held or taken; a DA
  with no coordinator (the chain answers, no HPPLite rollup there) is not
  bound (404). A fake RPC in this process answers as told.
- A store does not take an RPC failure for the funder's refusal (#220): a
  pull whose simulation the RPC rate-limits leaves the bucket covered.
- A store whose chain read fails does not sign less than it knows (#219):
  with a permit read once (the wallet's balance), the bucket statement and
  the account still say covered by it while the chain cannot be read.

ctest test_store_edges.
"""
import http.server
import json
import os
import secrets
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, ".."))
sys.path.insert(0, os.path.join(here, "..", "conformance"))
sys.path.insert(0, here)
from x402seller import eth  # noqa: E402
from buyer import Buyer  # noqa: E402
from hpplite_store import BIND_TYPE, CONTENT_TYPE, PRUNE_TYPE, batch_digest, cid_of  # noqa: E402

CHAIN_ID = 181228                  # the store's default network
fails = 0
settle_ok = {"value": True}
rpc_mode = {"value": "error"}


def check(cond, what):
    global fails
    print(("  ok      " if cond else "  FAIL    ") + what, flush=True)
    if not cond:
        fails += 1


class Answer(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def answer(self, out):
        body = json.dumps(out).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class Facilitator(Answer):
    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length") or 0))
        if self.path == "/verify":
            return self.answer({"isValid": True, "payer": "0x" + "33" * 20})
        if settle_ok["value"]:
            return self.answer({"success": True, "transaction": "0x" + "ab" * 32, "network": "eip155:%d" % CHAIN_ID})
        return self.answer({"success": False, "errorReason": "invalid_exact_evm_nonce_used"})


ROLLUP = {}      # "rollup" mode: what the chain says - coordinator, roles, the token


def word(v):
    return "0x" + (v if isinstance(v, str) else "%x" % v).replace("0x", "").rjust(64, "0")


class Rpc(Answer):
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)))
        if rpc_mode["value"] == "rollup" and req.get("method") == "eth_call":
            call = req["params"][0]
            to, sel = call["to"].lower(), call["data"][:10]
            if ROLLUP.get("limit_sends") and call.get("from") and sel in ("0x" + eth.keccak256(b"permit(address,address,uint256,uint256,uint8,bytes32,bytes32)")[:4].hex(),
                                                                         "0x" + eth.keccak256(b"transferFrom(address,address,uint256)")[:4].hex()):
                return self.answer({"jsonrpc": "2.0", "id": req.get("id"),
                                    "error": {"code": -32005, "message": "Rate Limit Exceeded. Please get an api key"}})
            sels = {"0x" + eth.keccak256(n.encode())[:4].hex(): n for n in
                    ("coordinator()", "sequencer()", "sequencerWallet()", "sequencerInstance()", "balanceOf(address)",
                     "allowance(address,address)", "nonces(address)")}
            name = sels.get(sel, "")
            out = {"coordinator()": word(ROLLUP["coordinator"][2:]), "sequencer()": word(ROLLUP["sequencer"][2:]),
                   "sequencerWallet()": word(ROLLUP["sequencer"][2:]), "sequencerInstance()": ROLLUP["instance"],
                   "balanceOf(address)": word(ROLLUP["balance"]), "allowance(address,address)": word(0),
                   "nonces(address)": word(0)}.get(name, "0x")
            return self.answer({"jsonrpc": "2.0", "id": req.get("id"), "result": out})
        if rpc_mode["value"] in ("error", "rollup"):
            return self.answer({"jsonrpc": "2.0", "id": req.get("id"), "error": {"code": -32603, "message": "upstream unavailable"}})
        return self.answer({"jsonrpc": "2.0", "id": req.get("id"), "result": "0x"})   # the chain answers, no contract there


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def serve(handler):
    port = free_port()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, "http://127.0.0.1:%d" % port


def http_(method, url, body=None, headers=None):
    req = urllib.request.Request(url, data=body, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            data = r.read()
            return r.status, json.loads(data) if r.headers.get("Content-Type", "").startswith("application/json") else data
    except urllib.error.HTTPError as e:
        data = e.read()
        try:
            return e.code, json.loads(data)
        except ValueError:
            return e.code, data


def start_store(work, name, *extra):
    port = free_port()
    url = "http://127.0.0.1:%d" % port
    log = open(os.path.join(work, name + ".log"), "w")
    p = subprocess.Popen([sys.executable, "-I", os.path.join(here, "hpplite_store.py"), "--port", str(port),
                          "--dir", os.path.join(work, name), "--signer-key", os.path.join(work, "signer.key"),
                          "--pay-to", "0x" + "11" * 20, "--tick-interval", "3600"] + list(extra), stdout=log, stderr=log)
    for _ in range(50):
        try:
            with urllib.request.urlopen(url + "/status", timeout=2):
                return p, url
        except OSError:
            time.sleep(0.2)
    p.kill()
    raise SystemExit("the store did not start: " + open(os.path.join(work, name + ".log")).read()[-500:])


def bind_body(domain, da, seq, fund, issued):
    f = eth.address(fund).lower()
    d = eth.typed_digest(domain, BIND_TYPE, da, f, issued)
    return json.dumps({"funder": f, "issuedAt": issued, "sequencerSignature": "0x" + eth.sign(seq, d).hex(),
                       "funderSignature": "0x" + eth.sign(fund, d).hex()}).encode()


def main():
    work = tempfile.mkdtemp(prefix="hpplite-edges.")
    with open(os.path.join(work, "signer.key"), "w") as f:
        f.write("0x" + "20" * 32)
    fac, fac_url = serve(Facilitator)
    rpc, rpc_url = serve(Rpc)
    procs = []
    try:
        # --- a top-up whose settlement fails ---
        p, url = start_store(work, "paid", "--facilitator", fac_url, "--price-per-write", "1000", "--price-per-mib-month", "0",
                             "--min-pay", "3000")
        procs.append(p)
        st = http_("GET", url + "/status")[1]
        domain = eth.domain_separator("HPPLiteStore", "2", CHAIN_ID, st["domain"]["verifyingContract"])
        seq, fund = secrets.token_bytes(32), secrets.token_bytes(32)
        fa = eth.address(fund).lower()
        da = eth.address(secrets.token_bytes(32)).lower()
        code, _ = http_("POST", url + "/bucket/" + da, bind_body(domain, da, seq, fund, int(time.time())))
        check(code == 200, "bound (no chain: the first binder's sequencer)")
        buyer = Buyer(url, fund, cap=10 ** 9)

        def put(data, height):
            sig = eth.sign(seq, eth.typed_digest(domain, CONTENT_TYPE, da, height, eth.keccak256(data)))
            status, headers, body = buyer._request("POST", "/put?bucket=%s&height=%d" % (da, height), data,
                                                   {"X-HPPLite-Signature": "0x" + sig.hex()})
            if status != 402:
                return status, body
            hdr = {k.upper(): v for k, v in headers.items()}.get("PAYMENT-REQUIRED")
            status, headers, body = buyer._request("POST", "/put?bucket=%s&height=%d" % (da, height), data,
                                                   {"X-HPPLite-Signature": "0x" + sig.hex(), "PAYMENT-SIGNATURE": buyer._payment(hdr)})
            return status, body

        settle_ok["value"] = True
        code, _ = put(b"a write, its top-up settled", 1)
        credit = http_("GET", url + "/account/" + fa)[1].get("credit")
        check(code == 200 and credit == 2000, "a top-up settled: the write done, the credit the top-up less the fee (%s %s)" % (code, credit))
        settle_ok["value"] = False
        code, _ = put(b"a write paid by itself", 2)            # covered by the credit: no 402, nothing to settle
        code, _ = put(b"a write paid by itself, again", 3)
        credit = http_("GET", url + "/account/" + fa)[1].get("credit")
        check(code == 200 and credit == 0, "two writes from the credit: no payment asked (%s, credit %s)" % (code, credit))
        code, body = put(b"a write whose top-up does not settle", 4)
        acct = http_("GET", url + "/account/" + fa)[1]
        check(code == 402 and b"settlement failed" in (body if isinstance(body, bytes) else json.dumps(body).encode()),
              "the top-up's settlement fails: 402 settlement failed (%s)" % code)
        check(acct.get("credit") == -1000, "the credit taken back: the write stays, owed (credit %s)" % acct.get("credit"))
        settle_ok["value"] = True
        code, _ = put(b"the next write", 5)
        acct = http_("GET", url + "/account/" + fa)[1]
        check(code == 200 and acct.get("credit") == 2000 - 1000, "the next top-up pays the owed too (credit %s)" % acct.get("credit"))

        # --- the chain cannot be read ---
        p2, url2 = start_store(work, "chained", "--rpc", rpc_url, "--price-per-write", "0", "--price-per-mib-month", "0")
        procs.append(p2)
        st2 = http_("GET", url2 + "/status")[1]
        domain2 = eth.domain_separator("HPPLiteStore", "2", CHAIN_ID, st2["domain"]["verifyingContract"])
        da2 = eth.address(secrets.token_bytes(32)).lower()
        rpc_mode["value"] = "error"
        code, out = http_("POST", url2 + "/bucket/" + da2, bind_body(domain2, da2, seq, fund, int(time.time())))
        check(code == 503, "a binding the chain cannot check: 503 (%s)" % code)
        code, _ = http_("GET", url2 + "/bucket/" + da2)
        check(code == 404, "nothing bound")
        instance = eth.keccak256(b"lease")
        data = b"a batch the chain cannot check"
        body = data + instance + eth.sign(seq, batch_digest(CHAIN_ID, da2, "0x" + instance.hex(), 1, data))
        code, _ = http_("POST", "%s/name/%s-1" % (url2, da2[2:]), body)
        check(code in (402, 503), "a name for that rollup: refused, its bucket not bound (%s)" % code)
        check(http_("GET", "%s/name/%s-1" % (url2, da2[2:]))[0] == 404, "nothing held under the name")
        rpc_mode["value"] = "none"
        code, _ = http_("POST", url2 + "/bucket/" + da2, bind_body(domain2, da2, seq, fund, int(time.time()) + 1))
        check(code == 404, "the chain answers, no HPPLite rollup at the DA: not bound, 404 (%s)" % code)

        # --- a read once, then the chain unreadable: covered-until is what the store knew, not now (#219) ---
        asset = "0x" + "77" * 20
        p3, url3 = start_store(work, "covered", "--rpc", rpc_url, "--asset", asset, "--price-per-write", "10",
                               "--price-per-mib-month", "30")
        procs.append(p3)
        st3 = http_("GET", url3 + "/status")[1]
        domain3 = eth.domain_separator("HPPLiteStore", "2", CHAIN_ID, st3["domain"]["verifyingContract"])
        da3, inst3 = eth.address(secrets.token_bytes(32)).lower(), "0x" + secrets.token_bytes(32).hex()
        ROLLUP.update({"coordinator": eth.address(secrets.token_bytes(32)).lower(), "sequencer": eth.address(seq).lower(),
                       "instance": inst3, "balance": 5000000})
        rpc_mode["value"] = "rollup"
        code, _ = http_("POST", url3 + "/bucket/" + da3, bind_body(domain3, da3, seq, fund, int(time.time())))
        check(code == 200, "bound, the rollup's roles from the chain")
        deadline = int(time.time()) + 365 * 86400
        pd = eth.typed_digest(eth.domain_separator("Bridged USDC", "2", CHAIN_ID, asset),
                              "Permit(address owner,address spender,uint256 value,uint256 nonce,uint256 deadline)",
                              fa, st3["buckets"]["spender"], 20000000, 0, deadline)
        code, _ = http_("POST", url3 + "/account/%s/permit" % fa, json.dumps({"value": 20000000, "deadline": deadline,
                        "signature": "0x" + eth.sign(fund, pd).hex()}).encode())
        check(code == 200, "the permit taken")
        data = b"a batch the permit pays for"
        body = data + bytes.fromhex(inst3[2:]) + eth.sign(seq, batch_digest(CHAIN_ID, da3, inst3, 1, data))
        code, _ = http_("POST", "%s/name/%s-1" % (url3, da3[2:]), body)
        known = http_("GET", url3 + "/bucket/" + da3)[1].get("coveredUntil", 0)
        check(code == 200 and known > time.time() + 30 * 86400, "a write the permit covers; covered well ahead (%s)" % known)
        rpc_mode["value"] = "error"
        b = http_("GET", url3 + "/bucket/" + da3)[1]
        acct = http_("GET", url3 + "/account/" + fa)[1]
        print("          (the chain unreadable: the statement's coveredUntil %s, the account's %s)" % ((b.get("statement") or {}).get("coveredUntil"), acct.get("coveredUntil")))
        check((b.get("statement") or {}).get("coveredUntil", 0) > time.time() + 30 * 86400 and acct.get("coveredUntil", 0) > time.time() + 30 * 86400,
              "the chain unreadable: the statement and the account still say covered by the permit, as last read (#219)")

        # --- a pull's simulation rate-limited: the store's failure, not the funder's refusal (#220) ---
        p4, url4 = start_store(work, "pulls", "--rpc", rpc_url, "--asset", asset, "--price-per-write", "1000",
                               "--price-per-mib-month", "30", "--pull-threshold", "500", "--tick-interval", "0.3")
        procs.append(p4)
        st4 = http_("GET", url4 + "/status")[1]
        domain4 = eth.domain_separator("HPPLiteStore", "2", CHAIN_ID, st4["domain"]["verifyingContract"])
        fund4 = secrets.token_bytes(32)
        fa4 = eth.address(fund4).lower()
        da4 = eth.address(secrets.token_bytes(32)).lower()
        rpc_mode["value"] = "rollup"
        ROLLUP["limit_sends"] = True
        code, _ = http_("POST", url4 + "/bucket/" + da4, bind_body(domain4, da4, seq, fund4, int(time.time())))
        pd4 = eth.typed_digest(eth.domain_separator("Bridged USDC", "2", CHAIN_ID, asset),
                               "Permit(address owner,address spender,uint256 value,uint256 nonce,uint256 deadline)",
                               fa4, st4["buckets"]["spender"], 20000000, 0, deadline)
        http_("POST", url4 + "/account/%s/permit" % fa4, json.dumps({"value": 20000000, "deadline": deadline,
              "signature": "0x" + eth.sign(fund4, pd4).hex()}).encode())
        data4 = b"a write the store will try to pull for"
        body4 = data4 + bytes.fromhex(inst3[2:]) + eth.sign(seq, batch_digest(CHAIN_ID, da4, inst3, 1, data4))
        code, _ = http_("POST", "%s/name/%s-1" % (url4, da4[2:]), body4)
        check(code == 200, "a write the permit covers, owing past the pull threshold")
        time.sleep(2.5)                                   # several ticks: each tries the pull, its simulation rate-limited
        acct4 = http_("GET", url4 + "/account/" + fa4)[1]
        b4 = http_("GET", url4 + "/bucket/" + da4)[1]
        warned = "pull" in (http_("GET", url4 + "/status")[1].get("warnings") or {})
        print("          (the account: %s %s; the bucket: %s; a warning: %s)" % (acct4.get("state"), acct4.get("why"), b4.get("state"), warned))
        check(acct4.get("state") == "covered" and b4.get("state") == "covered" and warned,
              "a pull whose simulation the RPC rate-limits: the store's failure - covered, a warning, nothing parked (#220)")
        ROLLUP["limit_sends"] = False

        # --- a bucket of many objects: a put costs what it costs into an empty one (#401) ---
        # A store's directory as it was before #401 - every object of the bucket in its record's "content" map - is
        # taken over at the start: 50,000 objects, each still found by its CID and by its height; then a put into
        # that bucket is timed beside a put into an empty one, turn about.
        rpc_mode["value"] = "error"
        p5, url5 = start_store(work, "many", "--price-per-write", "0", "--price-per-mib-month", "0")
        procs.append(p5)
        dom5 = eth.domain_separator("HPPLiteStore", "2", CHAIN_ID, http_("GET", url5 + "/status")[1]["domain"]["verifyingContract"])
        seq5, fund5 = secrets.token_bytes(32), secrets.token_bytes(32)
        big, small = eth.address(secrets.token_bytes(32)).lower(), eth.address(secrets.token_bytes(32)).lower()

        def put5(url, da, data, height):
            sig = eth.sign(seq5, eth.typed_digest(dom5, CONTENT_TYPE, da, height, eth.keccak256(data)))
            t = time.monotonic()
            code, out = http_("POST", "%s/put?bucket=%s&height=%d" % (url, da, height), data, {"X-HPPLite-Signature": "0x" + sig.hex()})
            return code, out if isinstance(out, dict) else {}, time.monotonic() - t

        N = 50000
        for da in (big, small):
            code, _ = http_("POST", url5 + "/bucket/" + da, bind_body(dom5, da, seq5, fund5, int(time.time())))
            check(code == 200, "bound")
        code, out, _ = put5(url5, big, b"held before the restart", N + 1)
        check(code == 200 and out.get("held") is False, "one object in the bucket (%s)" % code)
        p5.terminate()
        p5.wait()
        procs.remove(p5)
        rec_path = os.path.join(work, "many", "b", big, "bucket.json")
        rec = json.load(open(rec_path))
        seeded = {cid_of(b"object %d" % i): {"height": i, "bytes": len(b"object %d" % i)} for i in range(1, N + 1)}
        rec.setdefault("content", {}).update(seeded)
        rec["objects"] += N
        rec["bytes"] += sum(e["bytes"] for e in seeded.values())
        want_objects, want_bytes = rec["objects"], rec["bytes"]
        with open(rec_path, "w") as f:
            json.dump(rec, f, sort_keys=True)
        p5, url5 = start_store(work, "many", "--price-per-write", "0", "--price-per-mib-month", "0")
        procs.append(p5)
        b5 = http_("GET", url5 + "/bucket/" + big)[1]
        check(b5.get("objects") == want_objects and b5.get("bytes") == want_bytes,
              "a directory with the objects in the bucket's record is taken over: %s objects, %s bytes (%d, %d)"
              % (b5.get("objects"), b5.get("bytes"), want_objects, want_bytes))
        held = [put5(url5, big, d, h)[1].get("held") for d, h in ((b"held before the restart", N + 1), (b"object 17", 17), (b"object %d" % N, N))]
        check(held == [True, True, True], "each is found by its CID: the one put before, the 17th, the last (%s)" % held)
        first = sorted(list(seeded) + [cid_of(b"held before the restart")])[:5]
        pg1 = http_("GET", "%s/bucket/%s/objects?limit=3" % (url5, big))[1]
        pg2 = http_("GET", "%s/bucket/%s/objects?limit=2&after=%s" % (url5, big, pg1.get("next") or ""))[1]
        listed = [o["cid"] for o in pg1.get("objects", []) + pg2.get("objects", [])]
        check(listed == first and pg2.get("next") == first[4], "the listing pages through them by CID")
        tb, ts, done = [], [], True
        for i in range(15):
            cb, ob, t1 = put5(url5, big, b"a new object %d" % i, N + 2 + i)
            cs, osm, t2 = put5(url5, small, b"a new object %d" % i, 1 + i)
            done = done and cb == 200 and cs == 200 and ob.get("held") is False and osm.get("held") is False
            tb.append(t1)
            ts.append(t2)
        check(done, "15 new objects put into each bucket")
        mb, ms = sorted(tb)[7], sorted(ts)[7]
        print("          (a put, the median of 15: %.1f ms into the bucket of %d, %.1f ms into the empty one)" % (mb * 1000, want_objects, ms * 1000))
        check(mb < 3 * ms, "a put into a bucket of %d objects costs what one into an empty bucket does: %.1f ms and %.1f ms"
              % (want_objects, mb * 1000, ms * 1000))
        issued5 = int(time.time())
        psig = eth.sign(seq5, eth.typed_digest(dom5, PRUNE_TYPE, big, N - 9, issued5))
        code, out = http_("POST", "%s/bucket/%s/prune" % (url5, big), json.dumps({"height": N - 9, "issuedAt": issued5, "signature": "0x" + psig.hex()}).encode())
        b5 = http_("GET", url5 + "/bucket/" + big)[1]
        kept = sorted([cid_of(b"object %d" % i) for i in range(N - 9, N + 1)] + [cid_of(b"held before the restart")]
                      + [cid_of(b"a new object %d" % i) for i in range(15)])
        left, after = [], ""
        while after is not None and len(left) < 100:
            pg = http_("GET", "%s/bucket/%s/objects?limit=7&after=%s" % (url5, big, after))[1]
            left += [o["cid"] for o in pg.get("objects", [])]
            after = pg.get("next")
        check(code == 200 and b5.get("objects") == 26 and left == kept,
              "a prune lets go of them by height: %s objects left of %d, the 26 at or past the prune's height (%s)" % (b5.get("objects"), want_objects + 15, code))

        # --- a put does not wait for the tick's sweep (#409) ---
        # The sweep reads a file for every object the store holds; under the tick's lock every put waited for it. A
        # store that ticks every 0.3 s and whose sweep is made slow (0.3 s an object, a test hook, so 3.6 s for the 12
        # objects put first): bytes no bucket holds, two hours old, are in its directory; then five more puts, half a
        # second apart, each timed.
        p6, url6 = start_store(work, "swept", "--price-per-write", "0", "--price-per-mib-month", "0", "--tick-interval", "0.3", "--sweep-delay", "0.3")
        procs.append(p6)
        dom6 = eth.domain_separator("HPPLiteStore", "2", CHAIN_ID, http_("GET", url6 + "/status")[1]["domain"]["verifyingContract"])
        seq6, fund6 = secrets.token_bytes(32), secrets.token_bytes(32)
        da6 = eth.address(secrets.token_bytes(32)).lower()
        code, _ = http_("POST", url6 + "/bucket/" + da6, bind_body(dom6, da6, seq6, fund6, int(time.time())))
        check(code == 200, "bound")
        orphan = os.path.join(work, "swept", "c", "a-put-that-never-ended")
        with open(orphan, "wb") as f:
            f.write(b"bytes no bucket holds")
        os.utime(orphan, (time.time() - 7200, time.time() - 7200))

        def put6(i):
            data = b"swept past %d" % i
            sig = eth.sign(seq6, eth.typed_digest(dom6, CONTENT_TYPE, da6, i + 1, eth.keccak256(data)))
            t = time.monotonic()
            code, _ = http_("POST", "%s/put?bucket=%s&height=%d" % (url6, da6, i + 1), data, {"X-HPPLite-Signature": "0x" + sig.hex()})
            return code, time.monotonic() - t

        check(all(put6(i)[0] == 200 for i in range(12)), "12 objects put")
        t6, done = [], True
        for i in range(12, 17):
            time.sleep(0.5)
            code, t = put6(i)
            t6.append(t)
            done = done and code == 200
        check(done, "five more put while the store sweeps")
        print("          (a put beside a sweep of 3.6 s and more: the slowest of five %.2f s, their sum %.2f s)" % (max(t6), sum(t6)))
        check(max(t6) < 1.0, "a put does not wait for the sweep: the slowest of five took %.2f s" % max(t6))
        for _ in range(100):
            if not os.path.exists(orphan):
                break
            time.sleep(0.1)
        check(not os.path.exists(orphan), "the sweep still takes bytes no bucket holds")
        b6 = http_("GET", url6 + "/bucket/" + da6)[1]
        held6 = sum(1 for i in range(17) if os.path.exists(os.path.join(work, "swept", "c", cid_of(b"swept past %d" % i))))
        check(b6.get("objects") == 17 and held6 == 17, "and leaves the 17 that a bucket holds (%s in the bucket, %d files)" % (b6.get("objects"), held6))

        # --- a run of names (#404): where it ends, and the reads a test counts ---
        os.environ["HPPLITE_STORE_COUNT_READS"] = "1"
        p7, url7 = start_store(work, "runs", "--price-per-write", "0", "--price-per-mib-month", "0", "--names-bytes", "4096")
        del os.environ["HPPLITE_STORE_COUNT_READS"]
        procs.append(p7)
        st7 = http_("GET", url7 + "/status")[1]
        check("names" in (st7.get("reads") or []), "/status names the run of names (reads: %r)" % st7.get("reads"))
        dom7 = eth.domain_separator("HPPLiteStore", "2", CHAIN_ID, st7["domain"]["verifyingContract"])
        seq7, fund7, inst7 = secrets.token_bytes(32), secrets.token_bytes(32), secrets.token_bytes(32)
        da7 = eth.address(secrets.token_bytes(32)).lower()
        code, _ = http_("POST", url7 + "/bucket/" + da7, bind_body(dom7, da7, seq7, fund7, int(time.time())))
        check(code == 200, "bound")
        rec7 = {}

        def name7(height, data):
            rec7[height] = data + inst7 + eth.sign(seq7, batch_digest(CHAIN_ID, da7, "0x" + inst7.hex(), height, data))
            return http_("POST", "%s/name/%s-%d" % (url7, da7[2:], height), rec7[height])[0]

        def run7(query):
            code, got = http_("GET", "%s/names/%s?%s" % (url7, da7[2:], query))
            out = []
            while code == 200 and len(got) >= 4 and len(got) >= 4 + int.from_bytes(got[:4], "big"):
                n = int.from_bytes(got[:4], "big")
                out.append(got[4:4 + n])
                got = got[4 + n:]
            return code, out, got

        # 1-3: 1500 bytes each (a body of 4096 holds two); 4: 6000, more than a body; 5-70: small
        sizes = {1: 1500, 2: 1500, 3: 1500, 4: 6000}
        check(all(name7(h, bytes([h]) * sizes.get(h, 20)) == 200 for h in range(1, 71)), "70 batches named")
        reads0 = http_("GET", url7 + "/status")[1].get("test", {}).get("reads")
        code, out, rest = run7("from=1")
        check(code == 200 and out == [rec7[1], rec7[2]] and not rest, "a run ends before the batch that would pass the body's bytes: 1 and 2 of 1-3 (%d, %d records)" % (code, len(out)))
        code, out, rest = run7("from=3")
        check(code == 200 and out == [rec7[3]] and not rest, "...and 3 alone before the large one (%d records)" % len(out))
        code, out, rest = run7("from=4")
        check(code == 200 and out == [rec7[4]] and not rest, "the first batch of a run is given whatever its size: 6000 bytes where a body is 4096 (%d records)" % len(out))
        code, out, rest = run7("from=5")
        check(code == 200 and out == [rec7[h] for h in range(5, 38)] and not rest, "small ones to the body's bytes: 5-37, 33 of 121 bytes each behind its length (%d records)" % len(out))
        reads1 = http_("GET", url7 + "/status")[1].get("test", {}).get("reads")
        check(reads0 is not None and reads1 == reads0 + 4, "the store counts a run as one read: four runs, four reads (%s then %s)" % (reads0, reads1))
        procs.remove(p7)
        p7.terminate()
        p7.wait()
        # the same bucket at a store with the protocol's bytes: the most batches a run gives
        p7, url7 = start_store(work, "runs")
        procs.append(p7)
        code, out, rest = run7("from=5")
        code2, out2, _ = run7("from=5&limit=1000")
        check(code == 200 and out == [rec7[h] for h in range(5, 69)] and out2 == out and not rest,
              "a run gives 64 batches at most, asked for none or for 1000: 5-68 (%d, %d records)" % (len(out), len(out2)))
        code, out, rest = run7("from=69")
        check(code == 200 and out == [rec7[69], rec7[70]] and not rest, "and ends at the last name: 69 and 70 (%d records)" % len(out))
    finally:
        for p in procs:
            p.terminate()
            p.wait()
        fac.shutdown()
        rpc.shutdown()
    print("edges selftest: %s" % ("FAIL (%d)" % fails if fails else "ok"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
