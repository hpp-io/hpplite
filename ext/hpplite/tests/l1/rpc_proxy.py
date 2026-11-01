#!/usr/bin/env python3 -I
"""A faulty JSON-RPC endpoint for test_anvil (#49): an HTTP proxy in front of
anvil that misbehaves as a public RPC under load does, by phase.

    rpc_proxy.py <listen_port> <upstream_host:port> <mode_file>

The mode file is read for every request (the test rewrites it per phase);
each line is key=value, missing keys are 0:

    latency_ms=400   every response waits this long first
    drop=0.3         this fraction of requests is forwarded, then the
                     connection is closed with no response (the answer lost)
    timeout=0.2      this fraction is forwarded, then the response is held
                     for hold_ms (the client gives up first)
    error=0.4        this fraction gets HTTP 503 with an empty body, and is
                     not forwarded (a gateway that refused it)
    hold_ms=4000     how long a timed-out response is held
    seed=1           the random stream (one per proxy start)

When the mode changes, the first requests take each fault kind the mode
lists once, in order (error, drop, timeout); the rolls decide after that.
A phase of a few requests then always shows its kind (#127: on CI the 503
phase saw no 503 and the test failed on its own coverage check).

What happened is counted in <mode_file>.stats (requests, forwarded,
delayed, dropped, timed_out, errored), rewritten after every request, so
the test can tell the faults were exercised. Only the standard library.
The proxy exits on its own when its parent is gone.
"""
import http.client
import http.server
import os
import random
import socketserver
import sys
import threading
import time

listen_port = int(sys.argv[1])
upstream = sys.argv[2]
mode_file = sys.argv[3]
up_host, up_port = upstream.rsplit(":", 1)
up_port = int(up_port)

stats_lock = threading.Lock()
stats = {"requests": 0, "forwarded": 0, "delayed": 0, "dropped": 0, "timed_out": 0, "errored": 0}
rng = None
rng_seed = None
mode_seen = None       # the mode the last request saw
forced = []            # fault kinds still owed at the start of this mode


def read_mode():
    mode = {}
    try:
        with open(mode_file) as f:
            for line in f:
                line = line.strip()
                if "=" in line and not line.startswith("#"):
                    k, v = line.split("=", 1)
                    mode[k.strip()] = float(v.strip())
    except OSError:
        pass
    return mode


def count(key):
    with stats_lock:
        stats[key] += 1
        tmp = mode_file + ".stats.tmp"
        with open(tmp, "w") as f:
            for k, v in stats.items():
                f.write("%s=%d\n" % (k, v))
        os.replace(tmp, mode_file + ".stats")


def forward(body):
    conn = http.client.HTTPConnection(up_host, up_port, timeout=60)
    try:
        conn.request("POST", "/", body, {"Content-Type": "application/json"})
        r = conn.getresponse()
        return r.status, r.read()
    finally:
        conn.close()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def reply(self, status, data):
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            if data:
                self.wfile.write(data)
            self.wfile.flush()
        except OSError:
            self.close_connection = True

    def do_POST(self):
        global rng, rng_seed, mode_seen, forced
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        mode = read_mode()
        with stats_lock:
            seed = int(mode.get("seed", 1))
            if rng is None or seed != rng_seed:
                rng = random.Random(seed)
                rng_seed = seed
            sig = tuple(sorted(mode.items()))
            if sig != mode_seen:
                mode_seen = sig
                forced = [k for k in ("error", "drop", "timeout") if mode.get(k, 0) > 0]
            roll = rng.random()
            if forced:
                # the roll that lands in this kind's band, below
                kind = forced.pop(0)
                roll = {"error": 0.0, "drop": mode.get("error", 0),
                        "timeout": mode.get("error", 0) + mode.get("drop", 0)}[kind]
        count("requests")
        if roll < mode.get("error", 0):
            count("errored")
            self.reply(503, b"")
            return
        roll -= mode.get("error", 0)
        if mode.get("latency_ms", 0) > 0:
            count("delayed")
            time.sleep(mode["latency_ms"] / 1000.0)
        try:
            status, data = forward(body)
        except OSError as e:
            self.reply(502, b"")
            return
        count("forwarded")
        if roll < mode.get("drop", 0):
            count("dropped")
            self.close_connection = True       # the answer never arrives
            return
        roll -= mode.get("drop", 0)
        if roll < mode.get("timeout", 0):
            count("timed_out")
            time.sleep(mode.get("hold_ms", 4000) / 1000.0)
        self.reply(status, data)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def watch_parent(parent):
    while True:
        time.sleep(0.5)
        if os.getppid() != parent:
            os._exit(0)


threading.Thread(target=watch_parent, args=(os.getppid(),), daemon=True).start()
Server(("127.0.0.1", listen_port), Handler).serve_forever()
