#!/usr/bin/env python3 -I
"""The fault proxy exercises every fault kind a mode lists (#127).

test_anvil's scenario 14 fails on its own coverage check when a phase sees
a handful of requests and the seeded roll stream happens to skip the
phase's kind (CI: "0 503s in 53s"). So: when the mode changes, the proxy
takes each listed kind once, in order (error, drop, timeout), before the
rolls decide. This test runs the proxy alone against a dummy upstream with
a seed whose first rolls would all miss, and expects the kinds anyway.

    python3 -I tests/l1/test_rpc_proxy.py [path/to/rpc_proxy.py]
"""
import http.client
import http.server
import os
import random
import subprocess
import sys
import tempfile
import threading
import time

here = os.path.dirname(os.path.abspath(__file__))
proxy_script = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "rpc_proxy.py")


class Upstream(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        self.rfile.read(n)
        body = b'{"jsonrpc":"2.0","id":1,"result":"0x1"}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def free_port():
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def seed_missing(kind_fraction, n):
    """A seed whose first n rolls are all >= kind_fraction: the rolls alone would never pick the kind."""
    for seed in range(1, 100000):
        r = random.Random(seed)
        if all(r.random() >= kind_fraction for _ in range(n)):
            return seed
    raise SystemExit("no such seed")


def request(port, timeout=3.0):
    """One JSON-RPC POST through the proxy: 'ok', 'error' (503), 'dropped' (no answer) or 'timeout'."""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    try:
        conn.request("POST", "/", b'{"jsonrpc":"2.0","id":1,"method":"eth_blockNumber","params":[]}',
                     {"Content-Type": "application/json"})
        resp = conn.getresponse()
        resp.read()
        return "error" if resp.status == 503 else "ok"
    except (http.client.RemoteDisconnected, ConnectionResetError, http.client.BadStatusLine):
        return "dropped"
    except (TimeoutError, OSError):
        return "timeout"
    finally:
        conn.close()


def stats(mode_file):
    out = {}
    try:
        with open(mode_file + ".stats") as f:
            for line in f:
                k, v = line.strip().split("=", 1)
                out[k] = int(v)
    except OSError:
        pass
    return out


def set_mode(mode_file, text):
    tmp = mode_file + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, mode_file)


def main():
    failures = []
    up_port, px_port = free_port(), free_port()
    upstream = http.server.ThreadingHTTPServer(("127.0.0.1", up_port), Upstream)
    threading.Thread(target=upstream.serve_forever, daemon=True).start()
    tmpdir = tempfile.mkdtemp(prefix="hpplite-proxy-")
    mode_file = os.path.join(tmpdir, "proxy.mode")
    set_mode(mode_file, "")
    proxy = subprocess.Popen([sys.executable, "-I", proxy_script, str(px_port), "127.0.0.1:%d" % up_port, mode_file])
    try:
        for _ in range(50):
            if request(px_port) == "ok":
                break
            time.sleep(0.1)
        else:
            raise SystemExit("proxy did not come up")

        # 1. a 503 phase whose rolls would all miss: the kind must still happen
        seed = seed_missing(0.4, 8)
        set_mode(mode_file, "error=0.4\nseed=%d\n" % seed)
        seen = [request(px_port) for _ in range(6)]
        st = stats(mode_file)
        print("  503 phase, seed %d (its first 8 rolls miss): %s -> errored=%d" % (seed, seen, st.get("errored", 0)))
        if st.get("errored", 0) < 1 or "error" not in seen:
            failures.append("the 503 phase produced no 503 in 6 requests")

        # 2. a combined phase: each listed kind happens at its start, in order
        seed = seed_missing(0.5, 8)
        set_mode(mode_file, "drop=0.3\ntimeout=0.2\nhold_ms=800\nseed=%d\n" % seed)
        before = stats(mode_file)
        seen = [request(px_port, timeout=0.3) for _ in range(4)]
        st = stats(mode_file)
        dropped = st.get("dropped", 0) - before.get("dropped", 0)
        timed = st.get("timed_out", 0) - before.get("timed_out", 0)
        print("  drop+timeout phase: %s -> dropped=%d timed_out=%d" % (seen, dropped, timed))
        if dropped < 1 or timed < 1:
            failures.append("the combined phase did not produce both a dropped answer and a timeout")
        if seen[:2] != ["dropped", "timeout"]:
            failures.append("the kinds are not taken in order at the start of the phase: %s" % seen[:2])

        # 3. a healthy phase stays healthy
        set_mode(mode_file, "")
        time.sleep(1.0)                                   # a held answer from phase 2 may still be in flight
        seen = [request(px_port) for _ in range(3)]
        if seen != ["ok", "ok", "ok"]:
            failures.append("the healthy phase faulted: %s" % seen)
    finally:
        proxy.kill()
        proxy.wait()
        upstream.shutdown()
    for f in failures:
        print("FAIL: " + f)
    print("%s" % ("PASS" if not failures else "FAILED"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
