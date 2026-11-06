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
from hpplite_store import BIND_TYPE, CONTENT_TYPE, batch_digest  # noqa: E402

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


class Rpc(Answer):
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)))
        if rpc_mode["value"] == "error":
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
