#!/usr/bin/env python3
"""Verify a fleet database's anchoring against the chain (hpplite-fleet/1, doc/fleet-protocol.md): one call, given a
dbId, that answers which head of the database the chain holds, in which root, at which block - trusting the store for
nothing. The store's anchored is only where to look:

  1. GET /fleet/<fleet>/db/<dbId> at the store: anchored {seq, root, toHeight, path} - a hint
  2. RootPosted(seq) on chain: the root and the manifest digest, its block and time; rootIs(seq, root, digest) on chain
  3. the manifest by its CID: its sha-256 the chain's digest, the database's leaf in it (its head, owner, nameHash)
  4. the database's record on chain: the leaf's owner and nameHash its own
  5. the leaf rebuilt from the head, and the path into the chain's root

    verify.py --rpc URL --fleet 0x.. --store URL --db 0x..

Prints one JSON object: {verified, why, dbId, seq, root, manifestDigest, block, time, toHeight, head, path}; exit 0 when
verified, 1 otherwise. somaro's `somaro verify` shows from it "this row's history is anchored at block N".
"""
import argparse
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x402seller import eth  # noqa: E402
from x402seller.chain import Chain, selector  # noqa: E402
import fleet as F  # noqa: E402
from anchor import TOPIC_ROOT, head_bytes, http, w  # noqa: E402


def cid_of_digest(digest):
    """CIDv1, raw, sha2-256: the store's key for bytes of this sha-256"""
    alphabet = "abcdefghijklmnopqrstuvwxyz234567"
    data, bits, n, out = b"\x01\x55\x12\x20" + digest, 0, 0, ""
    for byte in data:
        n = (n << 8) | byte
        bits += 8
        while bits >= 5:
            out += alphabet[(n >> (bits - 5)) & 31]
            bits -= 5
    if bits:
        out += alphabet[(n << (5 - bits)) & 31]
    return "b" + out


def verify(rpc, fleet, store, dbx):
    chain, fleet, dbx = Chain(rpc), fleet.lower(), dbx.lower()
    db = bytes.fromhex(dbx[2:])
    out = {"verified": False, "dbId": dbx}
    st, view = http("GET", "%s/fleet/%s/db/%s" % (store, fleet, dbx))
    anch = (view or {}).get("anchored") if st == 200 else None
    if not anch:
        out["why"] = "the store names no root holding this database (not anchored yet, or not this store's)"
        return out
    seq = int(anch["seq"])
    out.update({"seq": seq, "toHeight": anch.get("toHeight"), "path": anch.get("path", [])})
    logs = chain.call("eth_getLogs", {"address": fleet, "fromBlock": "0x0", "toBlock": "latest",
                                      "topics": [TOPIC_ROOT, "0x" + eth.word(seq).hex()]}) or []
    if not logs:
        out["why"] = "no RootPosted(%d) on chain" % seq
        return out
    data = logs[-1]["data"][2:]
    root, digest = bytes.fromhex(data[:64]), bytes.fromhex(data[64:128])
    blk = chain.call("eth_getBlockByNumber", logs[-1]["blockNumber"], False)
    out.update({"root": "0x" + root.hex(), "manifestDigest": "0x" + digest.hex(), "block": int(logs[-1]["blockNumber"], 16),
                "time": int(blk["timestamp"], 16)})
    ok = chain.call("eth_call", {"to": fleet, "data": "0x" + selector("rootIs(uint256,bytes32,bytes32)") + eth.word(seq).hex() + root.hex() + digest.hex()}, "latest")
    if int(ok, 16) != 1:
        out["why"] = "the fleet does not hold root %d as RootPosted names it" % seq
        return out
    st, manifest = http("GET", "%s/get/%s" % (store, cid_of_digest(digest)), raw=True)
    if st != 200:
        out["why"] = "the manifest of root %d is not at the store (%s)" % (seq, st)
        return out
    if hashlib.sha256(manifest).digest() != digest:
        out["why"] = "the store's manifest is not the one root %d names (sha-256)" % seq
        return out
    leaf = next((l for l in json.loads(manifest)["leaves"] if l["dbId"].lower() == dbx), None)
    if not leaf:
        out["why"] = "root %d's manifest has no leaf for this database" % seq
        return out
    r = chain.call("eth_call", {"to": fleet, "data": "0x" + selector("db(bytes32)") + db.hex()}, "latest")
    owner, nh = "0x" + w(r, 0)[-40:], "0x" + w(r, 2)
    if leaf["owner"].lower() != owner.lower() or leaf["nameHash"].lower() != nh.lower():
        out["why"] = "the leaf's owner or name is not the database's record"
        return out
    head = head_bytes(leaf["head"])
    lf = F.leaf(db, owner, bytes.fromhex(nh[2:]), F.head_digest(F.domain(int(chain.call("eth_chainId"), 16), fleet), head))
    path = [(bytes.fromhex(p["hash"][2:]), bool(p["left"])) for p in anch.get("path", [])]
    if not F.verify(lf, path, root):
        out["why"] = "the path does not lead from the leaf to root %d" % seq
        return out
    out.update({"verified": True, "why": "anchored in root %d at block %d" % (seq, out["block"]), "toHeight": int(leaf["head"]["toHeight"]),
                "head": leaf["head"]})
    return out


def main():
    ap = argparse.ArgumentParser(description="verify a fleet database's anchoring against the chain")
    ap.add_argument("--rpc", required=True)
    ap.add_argument("--fleet", required=True)
    ap.add_argument("--store", required=True)
    ap.add_argument("--db", required=True, help="the database's dbId")
    a = ap.parse_args()
    out = verify(a.rpc, a.fleet, a.store, a.db)
    print(json.dumps(out))
    return 0 if out["verified"] else 1


if __name__ == "__main__":
    sys.exit(main())
