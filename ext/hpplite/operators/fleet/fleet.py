#!/usr/bin/env python3
"""hpplite-fleet/1, the shared rollup's messages and tree (#297, doc/fleet-protocol.md): the reference the
conformance vectors are computed from. Pure functions; no chain, no store.

A fleet is one shared rollup holding many databases. Each database signs its own head at every flush with its
own sequencer key; an anchor operator collects the heads, orders the leaves by dbId, builds the root and posts it
with the manifest's digest. An owner checks their database against a root with their leaf and its Merkle path.
"""
import hashlib
import json
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, ".."))
from x402seller import eth  # noqa: E402

PROTOCOL = "hpplite-fleet/1"

# EIP-712, in the domain ("HPPLiteFleet", "1", chainId, <the shared rollup>)
HEAD_TYPE = ("Head(bytes32 dbId,bytes32 instanceId,uint256 fromHeight,uint256 toHeight,bytes32 chainHashAfter,"
             "bytes32 stateRoot,bytes32 manifestDigest,uint256 epoch)")                 # the database's sequencer
ADMIT_TYPE = "Admit(bytes32 dbId,address owner,address sequencer,uint256 issuedAt)"     # the fleet's owner, or its admitter
RETIRE_TYPE = "Retire(bytes32 dbId,uint256 issuedAt)"                                   # the database's owner
ROOT_TYPE = "Root(uint256 seq,bytes32 root,bytes32 manifestDigest,uint256 deadline)"     # the fleet's anchor key
CLAIM_TYPE = "Claim(bytes32 dbId,bytes32 instanceId,uint256 nonce,uint256 deadline)"     # the database's sequencer; nonce: the record's claimNonce
EXPIRE_TYPE = "Expire(bytes32 dbId,uint256 nonce,uint256 deadline)"     # the database's owner: forceExpire, relayable (#321); nonce: the claimNonce
# in the factory's domain ("HPPLiteFleetFactory", "11", chainId, <the factory>): the fleet's owner; nonce: the factory's createNonce(owner)
CREATE_FLEET_TYPE = "CreateFleet(address owner,address anchor,uint64 leaseDuration,uint256 nonce,uint256 deadline)"


def factory_domain(chain_id, factory):
    return eth.domain_separator("HPPLiteFleetFactory", "11", chain_id, factory)


def domain(chain_id, rollup):
    return eth.domain_separator("HPPLiteFleet", "1", chain_id, rollup)


def db_id(owner, name):
    """keccak256(abi.encode(owner, keccak256(name))): a database is its owner and its name in the fleet"""
    return eth.keccak256(eth.word(owner) + eth.word(eth.keccak256(name.encode())))


def store_name(db, admitted_at, height):
    """a database's batch at the store, by its admission (#318): <dbId>-<admittedAt>-<height>, dbId 0x-hex lowercase,
    admittedAt the record's (the Admit's issuedAt) and the height in decimal; a re-admission starts a new life at 1"""
    return "0x%s-%d-%d" % ((db.hex() if isinstance(db, bytes) else db[2:]).lower(), admitted_at, height)


def name_hash(name):
    return eth.keccak256(name.encode())


def head_digest(dom, h):
    return eth.typed_digest(dom, HEAD_TYPE, h["dbId"], h["instanceId"], h["fromHeight"], h["toHeight"],
                            h["chainHashAfter"], h["stateRoot"], h["manifestDigest"], h["epoch"])


def leaf(db, owner, nhash, hdigest):
    """keccak256(abi.encode(dbId, owner, nameHash, headDigest))"""
    return eth.keccak256(eth.word(db) + eth.word(owner) + eth.word(nhash) + eth.word(hdigest))


def _levels(leaves):
    """the tree bottom up: pairs hashed in order, keccak256(left || right); an odd last node moves up unchanged"""
    levels = [list(leaves)]
    while len(levels[-1]) > 1:
        cur = levels[-1]
        nxt = [eth.keccak256(cur[i] + cur[i + 1]) for i in range(0, len(cur) - 1, 2)]
        if len(cur) % 2:
            nxt.append(cur[-1])
        levels.append(nxt)
    return levels


def root(entries):
    """entries: [(dbId, leaf)], each dbId once; ordered by dbId ascending (as unsigned 256-bit numbers) before hashing.
    One leaf: the root is that leaf. None for no entries: no root is posted when no head moved."""
    ids = [d for d, _ in entries]
    if len(set(ids)) != len(ids):
        raise ValueError("a dbId twice in one root")
    if not entries:
        return None
    return _levels([l for _, l in sorted(entries, key=lambda e: e[0])])[-1][0]


def proof(entries, db):
    """the Merkle path of db's leaf: [(sibling, sibling_is_left)], bottom up; a level where the node moved up
    unchanged adds nothing"""
    ordered = sorted(entries, key=lambda e: e[0])
    i = [d for d, _ in ordered].index(db)
    path = []
    for level in _levels([l for _, l in ordered])[:-1]:
        if i % 2 == 1:
            path.append((level[i - 1], True))
        elif i + 1 < len(level):
            path.append((level[i + 1], False))
        i //= 2
    return path


def verify(leaf_hash, path, want_root):
    h = leaf_hash
    for sibling, is_left in path:
        h = eth.keccak256(sibling + h) if is_left else eth.keccak256(h + sibling)
    return h == want_root


def build_root(chain_id, rollup, seq, records, heads, held):
    """The manifest builder's rule (doc/fleet-protocol.md, "Building a root"): which databases a root holds, the same for
    every builder from the same inputs. records: {dbId: {owner, sequencer, nameHash, epoch, admittedAt, retired}} - the
    fleet's records read at the block the build starts from; heads: {dbId: {head, signature, admittedAt}} - the store's
    latest head of each database (bytes fields as bytes); held: {dbId: headDigest} - the head in each database's latest
    root. Returns (items, entries, root, manifest bytes, manifest digest, left): left names why each other database is
    not in this root. No leaf: root None, nothing to post."""
    dom = domain(chain_id, rollup)
    items, entries, left = [], [], {}
    hx = lambda b: "0x" + b.hex()
    for db in sorted(records, key=lambda d: int.from_bytes(d, "big")):
        r, h = records[db], heads.get(db)
        why = None
        if r.get("retired") or not r.get("owner"):
            why = "retired"
        elif not h:
            why = "no head"
        elif h.get("admittedAt") != r["admittedAt"]:
            why = "another admission"
        elif h["head"]["epoch"] != r["epoch"]:
            why = "not the record's epoch: the next root"
        hd = head_digest(dom, h["head"]) if h and not why else None
        if not why and eth.recover(hd, h["signature"]).lower() != r["sequencer"].lower():
            why = "not the database's sequencer"
        if not why and held.get(db) == hd:
            why = "unchanged since its latest root"
        if why:
            left[db] = why
            continue
        entries.append((db, leaf(db, r["owner"], r["nameHash"], hd)))
        items.append({"dbId": hx(db), "owner": r["owner"].lower(), "nameHash": hx(r["nameHash"]),
                      "head": {k: (hx(v) if isinstance(v, bytes) else v) for k, v in h["head"].items()}, "signature": hx(h["signature"])})
    if not entries:
        return [], [], None, None, None, left
    data, digest = manifest(chain_id, rollup, seq, items)
    return items, entries, root(entries), data, digest, left


def manifest(chain_id, rollup, seq, items):
    """the manifest a root's digest names, as bytes: canonical JSON (sorted keys, no spaces), leaves by dbId.
    items: [{dbId, owner, nameHash, head, signature}] with bytes as 0x-hex. Its digest is sha-256: the store keeps
    it under that CID, as it keeps a range's manifest"""
    body = {"protocol": PROTOCOL, "chainId": chain_id, "rollup": rollup.lower(), "seq": seq,
            "leaves": sorted(items, key=lambda x: int(x["dbId"], 16))}
    data = json.dumps(body, sort_keys=True, separators=(",", ":")).encode()
    return data, hashlib.sha256(data).digest()
