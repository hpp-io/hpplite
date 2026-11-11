#!/usr/bin/env python3
"""hpplite-anchor: the reference anchor of a fleet (hpplite-fleet/1, doc/fleet-protocol.md "Building a root").

For anyone running a fleet without somaro (somarod anchors its own) and for conformance. Run by the fleet's anchor key,
it posts the fleet's roots: every --interval after the last RootPosted (a fleet's first root, --interval after its first
claim), when the builder's rule gives a leaf - an idle fleet sends nothing. One root:

  1. B, the latest block; seq = rootSeq() + 1
  2. the store's heads that moved (GET /fleet/<fleet>/heads), each database's record db(dbId) read at B
  3. fleet.build_root: the leaves, the manifest, the root (the same as any builder's from the same inputs)
  4. the manifest put at the store (POST /fleet/<fleet>/manifest, the anchor's Content signature), first
  5. postRootFor through the relay (sponsored by the fleet's Sponsor) - or, with --send, from the anchor key's own ETH
  6. the root hint (POST /fleet/<fleet>/root)

    anchor.py --rpc URL --fleet 0x.. --key-file F --store URL (--relay URL | --send) [--interval 86400] [--once] [--force]

Prints one JSON line per root posted (or per pass with nothing to post, with --once).
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x402seller import eth  # noqa: E402
from x402seller.chain import Chain, selector, signed_tx  # noqa: E402
import fleet as F  # noqa: E402

TOPIC_ROOT = "0x" + eth.keccak256(b"RootPosted(uint256,bytes32,bytes32)").hex()
TOPIC_CLAIMED = "0x" + eth.keccak256(b"Claimed(bytes32,bytes32,uint64)").hex()
HEAD_FIELDS = ("dbId", "instanceId", "fromHeight", "toHeight", "chainHashAfter", "stateRoot", "manifestDigest", "epoch")


def http(method, url, body=None, headers=None, raw=False):
    """(status, JSON or bytes); raw: the bytes as served (a manifest is checked byte for byte)"""
    req = urllib.request.Request(url, data=body, method=method, headers=dict(headers or {}, **({"Content-Type": "application/json"} if body is not None else {})))
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            data = r.read()
            return r.status, data if raw or data[:1] not in (b"{", b"[") else json.loads(data)
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            return e.code, json.loads(raw)
        except ValueError:
            return e.code, raw


def head_bytes(h):
    """a head as the store gives it (0x-hex) to the builder's form (bytes and ints)"""
    out = {}
    for k in HEAD_FIELDS:
        v = h[k]
        out[k] = bytes.fromhex(v[2:]) if isinstance(v, str) and v.startswith("0x") else int(v)
    return out


def w(r, i):
    return r[2 + 64 * i: 2 + 64 * (i + 1)]


def record_at(chain, fleet, db, block):
    """db(bytes32) at block: owner, sequencer, nameHash, instance, until, epoch, epochSince, admittedAt, retired, ..."""
    r = chain.call("eth_call", {"to": fleet, "data": "0x" + selector("db(bytes32)") + db.hex()}, hex(block))
    owner = "0x" + w(r, 0)[-40:]
    return {"owner": None if int(owner, 16) == 0 else owner, "sequencer": "0x" + w(r, 1)[-40:], "nameHash": bytes.fromhex(w(r, 2)),
            "epoch": int(w(r, 5), 16), "admittedAt": int(w(r, 7), 16), "retired": int(w(r, 8), 16) != 0}


def last_time(chain, fleet, topic):
    """the block time of the fleet's last log with this topic; None when there is none"""
    logs = chain.call("eth_getLogs", {"address": fleet, "fromBlock": "0x0", "toBlock": "latest", "topics": [topic]}) or []
    if not logs:
        return None
    blk = chain.call("eth_getBlockByNumber", logs[-1]["blockNumber"], False)
    return int(blk["timestamp"], 16)


def first_time(chain, fleet, topic):
    logs = chain.call("eth_getLogs", {"address": fleet, "fromBlock": "0x0", "toBlock": "latest", "topics": [topic]}) or []
    if not logs:
        return None
    return int(chain.call("eth_getBlockByNumber", logs[0]["blockNumber"], False)["timestamp"], 16)


def store_heads(store, fleet):
    out, after = [], None
    while True:
        st, body = http("GET", "%s/fleet/%s/heads%s" % (store, fleet, "?after=" + after if after else ""))
        if st != 200:
            raise RuntimeError("the store's heads: %s %s" % (st, body))
        page = body.get("heads", [])
        out += page
        if len(page) < 1000:
            return out
        after = page[-1]["head"]["dbId"]


def send(chain, key, to, data, chain_id):
    """the anchor key's own transaction (--send): no relay"""
    me = eth.address(key)
    nonce = int(chain.call("eth_getTransactionCount", me, "pending"), 16)
    gas = int(int(chain.call("eth_estimateGas", {"from": me, "to": to, "data": "0x" + data.hex()}), 16) * 1.25) + 10000
    base = int(chain.call("eth_getBlockByNumber", "latest", False).get("baseFeePerGas", "0x0"), 16)
    tip = 10 ** 6
    raw = signed_tx(key, chain_id, nonce, tip, 2 * base + tip, gas, to, data)
    tx = chain.call("eth_sendRawTransaction", "0x" + raw.hex())
    for _ in range(120):
        rc = chain.call("eth_getTransactionReceipt", tx)
        if rc:
            if int(rc["status"], 16) != 1:
                raise RuntimeError("postRootFor reverted: %s" % tx)
            return tx
        time.sleep(1)
    raise RuntimeError("no receipt for %s" % tx)


def one(args, chain, key, chain_id):
    """one pass: a root when due and the rule gives a leaf. Returns the JSON line, or None when not due"""
    fleet = args.fleet.lower()
    now = int(chain.call("eth_getBlockByNumber", "latest", False)["timestamp"], 16)
    last = last_time(chain, fleet, TOPIC_ROOT)
    since = last if last is not None else first_time(chain, fleet, TOPIC_CLAIMED)
    if not args.force and (since is None or now < since + args.interval):
        return None
    block = int(chain.call("eth_blockNumber"), 16)
    seq = int(chain.call("eth_call", {"to": fleet, "data": "0x" + selector("rootSeq()")}, hex(block)), 16) + 1
    listed = store_heads(args.store, fleet)
    records, heads = {}, {}
    for h in listed:
        db = bytes.fromhex(h["head"]["dbId"][2:])
        rec = record_at(chain, fleet, db, block)
        records[db] = rec
        heads[db] = {"head": head_bytes(h["head"]), "signature": bytes.fromhex(h["signature"][2:]),
                     "admittedAt": h.get("admittedAt", rec["admittedAt"])}       # the store lists the current admission's
    items, entries, root, data, digest, left = F.build_root(chain_id, fleet, seq, records, heads, {})
    line = {"seq": seq, "block": block, "leaves": ["0x" + d.hex() for d, _ in sorted(entries, key=lambda e: int.from_bytes(e[0], "big"))],
            "left": {"0x" + d.hex(): why for d, why in left.items()}}
    if root is None:
        line["posted"] = False
        return line
    st, status = http("GET", args.store + "/status")
    dom = (status.get("domain") or {}) if st == 200 else {}
    sdom = eth.domain_separator(dom.get("name", "HPPLiteStore"), dom.get("version", "2"), int(dom["chainId"]), dom["verifyingContract"])
    csig = eth.sign(key, eth.typed_digest(sdom, "Content(address da,uint256 height,bytes32 hash)", fleet, seq, eth.keccak256(data)))
    st, body = http("POST", "%s/fleet/%s/manifest" % (args.store, fleet), data, {"X-HPPLite-Signature": "0x" + csig.hex()})
    if st != 200:
        raise RuntimeError("the manifest was refused: %s %s" % (st, body))
    deadline = now + 3600
    rsig = eth.sign(key, eth.typed_digest(F.domain(chain_id, fleet), F.ROOT_TYPE, seq, root, digest, deadline))
    calldata = bytes.fromhex(selector("postRootFor(uint256,bytes32,bytes32,uint256,bytes)")) + eth.word(seq) + eth.word(root) + eth.word(digest) \
        + eth.word(deadline) + eth.word(160) + eth.word(65) + rsig + bytes(31)
    if args.send:
        tx = send(chain, key, fleet, calldata, chain_id)
    else:
        st, body = http("POST", args.relay + "/commit", json.dumps({"to": fleet, "data": "0x" + calldata.hex()}).encode())
        if st != 200:
            raise RuntimeError("the relay did not take postRootFor: %s %s" % (st, body))
        tx = body.get("tx")
    http("POST", "%s/fleet/%s/root" % (args.store, fleet), b"")
    line.update({"posted": True, "root": "0x" + root.hex(), "manifestDigest": "0x" + digest.hex(), "tx": tx})
    return line


def main():
    ap = argparse.ArgumentParser(description="the reference anchor of an hpplite-fleet/1 fleet")
    ap.add_argument("--rpc", required=True)
    ap.add_argument("--fleet", required=True)
    ap.add_argument("--key-file", required=True, help="the fleet's anchor key (0x-hex), read from this file")
    ap.add_argument("--store", required=True)
    ap.add_argument("--relay", help="postRootFor through this relay (sponsored by the fleet's Sponsor)")
    ap.add_argument("--send", action="store_true", help="postRootFor from the anchor key's own ETH instead of a relay")
    ap.add_argument("--interval", type=int, default=86400, help="seconds after the last root (default 24 h)")
    ap.add_argument("--once", action="store_true", help="one pass, then exit")
    ap.add_argument("--force", action="store_true", help="build now, whatever the interval (tests)")
    args = ap.parse_args()
    if not args.send and not args.relay:
        ap.error("--relay or --send")
    with open(args.key_file) as f:
        k = f.read().strip()
    key = bytes.fromhex(k[2:] if k.startswith("0x") else k)
    chain = Chain(args.rpc)
    chain_id = int(chain.call("eth_chainId"), 16)
    while True:
        line = one(args, chain, key, chain_id)
        if line is not None:
            print(json.dumps(line), flush=True)
        if args.once:
            return 0
        time.sleep(min(600, max(60, args.interval // 24)))


if __name__ == "__main__":
    sys.exit(main())
