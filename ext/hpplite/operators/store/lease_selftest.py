#!/usr/bin/env python3
"""A store with --rpc refuses a name it cannot check against the chain (#198): when the RPC is unreachable, or
answers with an error, a name put is answered 503 and nothing is held - accepting it unchecked would let the first
signer of a rollup take its names at the store for good. A DA with no coordinator (the chain answers, there is no
HPPLite rollup there) is taken as before. A fake RPC in this process answers as told. ctest test_store_lease.
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
sys.path.insert(0, here)
from x402seller import eth  # noqa: E402
from pin_service import batch_digest  # noqa: E402

CHAIN_ID = 181228
fails = 0
mode = {"value": "error"}


def check(cond, what):
    global fails
    print(("  ok      " if cond else "  FAIL    ") + what, flush=True)
    if not cond:
        fails += 1


class Rpc(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)))
        if mode["value"] == "error":
            out = {"jsonrpc": "2.0", "id": req.get("id"), "error": {"code": -32603, "message": "upstream unavailable"}}
        else:                                   # "none": the chain answers, no contract there
            out = {"jsonrpc": "2.0", "id": req.get("id"), "result": "0x"}
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


def named(key, da, height, data):
    name = "%s-%d" % (da, height)
    instance = "0x" + eth.keccak256(b"lease").hex()
    sig = eth.sign(key, batch_digest(CHAIN_ID, name, instance, data))
    return name, data + bytes.fromhex(instance[2:]) + sig


def post(url, path, body):
    req = urllib.request.Request(url + path, data=body, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def start(work, rpc):
    port = free_port()
    log = open(os.path.join(work, "service-%d.log" % port), "w")
    p = subprocess.Popen([sys.executable, "-I", os.path.join(here, "pin_service.py"), "--port", str(port), "--dir", os.path.join(work, "store-%d" % port),
                          "--signer-key", os.path.join(work, "signer.key"), "--pay-to", "0x" + "11" * 20, "--rpc", rpc], stdout=log, stderr=log)
    url = "http://127.0.0.1:%d" % port
    for _ in range(100):
        try:
            urllib.request.urlopen(url + "/status", timeout=2).read()
            return p, url
        except OSError:
            time.sleep(0.1)
    raise SystemExit("the store did not start")


def main():
    work = tempfile.mkdtemp(prefix="hpplite-lease.")
    with open(os.path.join(work, "signer.key"), "w") as f:
        f.write(secrets.token_bytes(32).hex())
    rport = free_port()
    rpc = http.server.ThreadingHTTPServer(("127.0.0.1", rport), Rpc)
    threading.Thread(target=rpc.serve_forever, daemon=True).start()
    procs = []
    try:
        for label, url_rpc, m in (("unreachable", "http://127.0.0.1:%d" % free_port(), "error"),
                                  ("answering an error", "http://127.0.0.1:%d" % rport, "error")):
            mode["value"] = m
            p, url = start(work, url_rpc)
            procs.append(p)
            key, da = secrets.token_bytes(32), secrets.token_bytes(20).hex()
            name, body = named(key, da, 1, b"batch one")
            st, out = post(url, "/name/" + name, body)
            check(st == 503, "the RPC %s: a name put answered 503 (%d %s)" % (label, st, out.get("error", "")))
            r = urllib.request.Request(url + "/name/" + name)
            try:
                held = urllib.request.urlopen(r, timeout=10).status
            except urllib.error.HTTPError as e:
                held = e.code
            check(held == 404, "nothing held (%d)" % held)
            mode["value"] = "none"
            other = secrets.token_bytes(32)
            name2, body2 = named(other, da, 2, b"another signer")
            st2, _ = post(url, "/name/" + name2, body2) if label == "answering an error" else (None, None)
            if label == "answering an error":
                check(st2 == 200, "once the chain answers (no coordinator there): taken (%s) - the refused signer did not take the rollup" % st2)
    finally:
        for p in procs:
            p.terminate()
            p.wait()
        rpc.shutdown()
    print("%s: %d failure(s)" % ("FAIL" if fails else "PASS", fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
