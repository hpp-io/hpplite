#!/usr/bin/env python3
"""The store's state as an object store holds it (#186), against two
reference services on one directory - what two Worker isolates on one
bucket are: the first signer of a rollup's names holds them whichever
process it reached, the other is refused at both; a batch past its
retention is gone after the expiry tick. Free mode, no chain.
ctest test_store_state.
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
sys.path.insert(0, here)
from x402seller import eth  # noqa: E402
from pin_service import batch_digest, cid_of  # noqa: E402

CHAIN_ID = 181228                  # the service's default network
fails = 0


def check(cond, what):
    global fails
    print(("  ok      " if cond else "  FAIL    ") + what, flush=True)
    if not cond:
        fails += 1


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def start(work, store_dir, extra=()):
    port = free_port()
    log = open(os.path.join(work, "service-%d.log" % port), "w")
    p = subprocess.Popen([sys.executable, "-I", os.path.join(here, "pin_service.py"), "--port", str(port), "--dir", store_dir,
                          "--signer-key", os.path.join(work, "signer.key"), "--pay-to", "0x" + "11" * 20] + list(extra),
                         stdout=log, stderr=log)
    url = "http://127.0.0.1:%d" % port
    for _ in range(100):
        try:
            with urllib.request.urlopen(url + "/status", timeout=2):
                return p, url
        except OSError:
            if p.poll() is not None:
                break
            time.sleep(0.1)
    raise SystemExit("the pin service did not start: " + open(log.name).read())


def post(url, body):
    req = urllib.request.Request(url, data=body, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code


def get(url):
    try:
        with urllib.request.urlopen(url, timeout=30) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, b""


def named(key, da, height, data):
    """A batch under <da>-<height>: the bytes, the instance, the sequencer's EIP-712 Batch signature"""
    name = "%s-%d" % (da, height)
    instance = "0x" + eth.keccak256(b"instance").hex()
    sig = eth.sign(key, batch_digest(CHAIN_ID, name, instance, data))
    return name, data + bytes.fromhex(instance[2:]) + sig


def main():
    work = tempfile.mkdtemp(prefix="hpplite-state.")
    with open(os.path.join(work, "signer.key"), "w") as f:
        f.write(secrets.token_bytes(32).hex())
    procs = []
    try:
        # 1. two processes on one store: the rollup's names go to the first signer, whichever process it reached
        shared = os.path.join(work, "shared")
        p1, u1 = start(work, shared)
        p2, u2 = start(work, shared)
        procs += [p1, p2]
        da = secrets.token_bytes(20).hex()
        a, b = secrets.token_bytes(32), secrets.token_bytes(32)
        name, body = named(a, da, 1, b"sequencer a's batch")
        check(post(u1 + "/name/" + name, body) == 200, "signer A puts %s-1 through the first process" % da[:8])
        name, body = named(b, da, 2, b"another key's batch")
        st = post(u2 + "/name/" + name, body)
        check(st == 403, "signer B, through the second process, is refused the same rollup's names (%d)" % st)
        st = post(u1 + "/name/" + name, body)
        check(st == 403, "and through the first (%d)" % st)
        st, data = get(u2 + "/name/%s-1" % da)
        check(st == 200 and data.startswith(b"sequencer a's batch"), "A's batch reads back through the second process")

        # 2. retention enforced: a batch past it is gone after the expiry tick
        short = os.path.join(work, "short")
        p3, u3 = start(work, short, ["--retention", "2", "--expire-interval", "1"])
        procs.append(p3)
        data = b"kept for two seconds " + secrets.token_bytes(16)
        cid = cid_of(data)
        check(post(u3 + "/put", data) == 200 and get(u3 + "/get/" + cid)[0] == 200, "a put held, read back")
        time.sleep(5)
        st, _ = get(u3 + "/get/" + cid)
        check(st == 404 and not os.path.exists(os.path.join(short, cid)), "past its retention: gone (%d), the bytes deleted" % st)
        # 3. a directory from before #186 (one index.json): its entries and its rollups' signers carried over
        old = os.path.join(work, "old")
        os.makedirs(old)
        data = b"held before the change"
        cid = cid_of(data)
        with open(os.path.join(old, cid), "wb") as f:
            f.write(data)
        da2 = secrets.token_bytes(20).hex()
        name, body = named(a, da2, 1, b"an old named batch")
        with open(os.path.join(old, "name-" + name), "wb") as f:
            f.write(body)
        with open(os.path.join(old, "index.json"), "w") as f:
            json.dump({cid: {"bytes": len(data), "expires": int(time.time()) + 3600, "receipt": {"cid": cid}},
                       "names": {"0x" + da2: eth.address(a)}}, f)
        p4, u4 = start(work, old)
        procs.append(p4)
        st, got = get(u4 + "/get/" + cid)
        check(st == 200 and got == data, "an old directory: its batch still served (%d)" % st)
        name, body = named(b, da2, 2, b"another key")
        check(post(u4 + "/name/" + name, body) == 403, "its rollup's names still held by their signer")
        check(not os.path.exists(os.path.join(old, "index.json")), "index.json migrated")
    finally:
        for p in procs:
            p.terminate()
            p.wait()
    if fails:
        for f in sorted(os.listdir(work)):
            if f.startswith("service-"):
                print("--- %s" % f)
                print(open(os.path.join(work, f)).read()[-2000:])
    print("%s: %d failure(s)" % ("FAIL" if fails else "PASS", fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
