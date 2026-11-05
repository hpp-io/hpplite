#!/usr/bin/env python3
"""A paid call whose settlement fails is taken back (#194): the work runs after
/verify and before /settle, so a put or a pin the facilitator then fails to
settle must not stay held or extended - else the same put is answered 200,
held, for nothing, and the pin's extension is free. A facilitator in this
process verifies every payment and settles or fails as told. ctest
test_store_settle.
"""
import base64
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
fails = 0
settle_ok = {"value": True}


def check(cond, what):
    global fails
    print(("  ok      " if cond else "  FAIL    ") + what, flush=True)
    if not cond:
        fails += 1


class Facilitator(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length") or 0))
        if self.path == "/verify":
            out = {"isValid": True, "payer": "0x" + "33" * 20}
        elif settle_ok["value"]:
            out = {"success": True, "transaction": "0x" + "ab" * 32, "network": "eip155:181228"}
        else:
            out = {"success": False, "errorReason": "invalid_exact_evm_nonce_used"}
        body = json.dumps(out).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def post(url, path, data=b"", paid=False):
    headers = {"PAYMENT-SIGNATURE": base64.b64encode(json.dumps({"x402Version": 2, "accepted": {"scheme": "exact"},
                                                                 "payload": {"authorization": "a buyer's"}}).encode()).decode()} if paid else {}
    req = urllib.request.Request(url + path, data=data, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def main():
    work = tempfile.mkdtemp(prefix="hpplite-settle.")
    fport = free_port()
    fac = http.server.ThreadingHTTPServer(("127.0.0.1", fport), Facilitator)
    threading.Thread(target=fac.serve_forever, daemon=True).start()
    with open(os.path.join(work, "signer.key"), "w") as f:
        f.write(secrets.token_bytes(32).hex())
    port = free_port()
    log = open(os.path.join(work, "service.log"), "w")
    p = subprocess.Popen([sys.executable, "-I", os.path.join(here, "pin_service.py"), "--port", str(port), "--dir", os.path.join(work, "store"),
                          "--signer-key", os.path.join(work, "signer.key"), "--pay-to", "0x" + "11" * 20,
                          "--facilitator", "http://127.0.0.1:%d" % fport, "--no-channels"], stdout=log, stderr=log)
    url = "http://127.0.0.1:%d" % port
    try:
        for _ in range(100):
            try:
                urllib.request.urlopen(url + "/status", timeout=2).read()
                break
            except OSError:
                time.sleep(0.1)
        # a put whose settlement fails: 402, and not held - the same put asks for payment again
        settle_ok["value"] = False
        data = b"settle " + secrets.token_bytes(32)
        st, body = post(url, "/put", data, paid=True)
        check(st == 402 and "settlement failed" in body.get("error", ""), "a put whose settlement fails: 402 (%d %s)" % (st, body.get("error")))
        st, body = post(url, "/put", data)
        check(st == 402, "the same put again, unpaid: 402, not held (%d%s)" % (st, ", held" if body.get("held") else ""))

        # a pin whose settlement fails extends nothing
        settle_ok["value"] = True
        data = b"pin " + secrets.token_bytes(32)
        st, put = post(url, "/put", data, paid=True)
        check(st == 200, "a paid put: 200")
        settle_ok["value"] = False
        st, body = post(url, "/pin/" + put.get("cid", ""), paid=True)
        check(st == 402, "a pin whose settlement fails: 402 (%d)" % st)
        st, held = post(url, "/put", data)
        check(st == 200 and held.get("expires") == put.get("expires") and held.get("receipt") == put.get("receipt"),
              "the expiry and receipt as before the pin (%s, was %s)" % (held.get("expires"), put.get("expires")))
    finally:
        p.terminate()
        p.wait()
        fac.shutdown()
    print("%s: %d failure(s)" % ("FAIL" if fails else "PASS", fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
