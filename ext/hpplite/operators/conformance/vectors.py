#!/usr/bin/env python3
"""Test vectors for the store and relay protocols (#189): fixed inputs and
what the reference computes from them - keccak, addresses, signatures
(RFC 6979, so deterministic), CIDs, the name's EIP-712 digest and
hpplite-store/2's (Bind, Release, Content, Prune, BucketStatement, #213),
MAC and the selectors it takes. vectors.json is what another implementation (the
canonical operators' TypeScript) tests itself against; this script checks
the reference still computes it.

    vectors.py            check the reference against vectors.json (ctest test_vectors)
    vectors.py --write    regenerate vectors.json (a protocol change: say so in the commit)
"""
import json
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, ".."))
sys.path.insert(0, os.path.join(here, "..", "store"))
sys.path.insert(0, os.path.join(here, "..", "relay"))
sys.path.insert(0, os.path.join(here, "..", "fleet"))
from x402seller import eth  # noqa: E402
import fleet  # noqa: E402
import hpplite_store  # noqa: E402
import relay  # noqa: E402

PATH = os.path.join(here, "vectors.json")
KEYS = ["0x" + (bytes([i]) * 32).hex() for i in (1, 2, 0x42)]


def key(i):
    return bytes.fromhex(KEYS[i][2:])


def generate():
    v = {"version": 2, "about": "operators/conformance/vectors.py (#189); keys are test keys, never fund them"}
    v["keccak256"] = [{"in": h, "out": "0x" + eth.keccak256(bytes.fromhex(h[2:])).hex()}
                      for h in ("0x", "0x616263", "0x" + "ff" * 200)]
    v["addresses"] = [{"key": k, "address": eth.address(bytes.fromhex(k[2:]))} for k in KEYS]
    digests = ["0x" + eth.keccak256(t).hex() for t in (b"a", b"hpplite", b"")]
    v["signatures"] = [{"key": KEYS[i % 3], "digest": d, "signature": "0x" + eth.sign(key(i % 3), bytes.fromhex(d[2:])).hex()}
                       for i, d in enumerate(digests)]
    datas = [b"", b"hpplite", bytes(range(256)) * 5]
    v["cids"] = [{"data": "0x" + d.hex(), "cid": hpplite_store.cid_of(d)} for d in datas]

    # hpplite-store/2's messages (#213): in ("HPPLiteStore", "2", chainId, payTo), each signed by the key named
    pay_to = "0x" + "11" * 20
    sdom = eth.domain_separator("HPPLiteStore", "2", 181228, pay_to)
    da2, funder = "0x" + "ab" * 20, eth.address(key(2)).lower()
    fleet_addr = "0x" + "f1" * 20
    fleet_db = fleet.db_id(eth.address(key(1)).lower(), "notes")
    msgs = [("Bind", hpplite_store.BIND_TYPE, [da2, funder, 1800000000], 1),
            ("Release", hpplite_store.RELEASE_TYPE, [da2, funder, 1800000001], 2),
            ("Content", hpplite_store.CONTENT_TYPE, [da2, 7, eth.keccak256(b"first batch")], 1),
            ("Prune", hpplite_store.PRUNE_TYPE, [da2, 5, 1800000002], 1),
            ("BucketStatement", hpplite_store.STATEMENT_TYPE, [da2, 9, 5, hpplite_store.FOREVER, 1800000003], 0),
            # the funder's own words (#274, #275): its daily cap on the bucket, and a release that deletes it now
            ("Cap", hpplite_store.CAP_TYPE, [da2, 100, 1048576, 1800000004], 2),
            ("Delete", hpplite_store.DELETE_TYPE, [da2, funder, 1800000005], 2),
            # a fleet database's bucket (#310): BindDb signed by the funder and by the database's side (its sequencer, or
            # its owner alone - also before its record exists, with owner and nameHash beside it); the rest by the funder
            ("BindDb", hpplite_store.BINDDB_TYPE, [fleet_addr, fleet_db, funder, 1800000006], 2),
            ("BindDb", hpplite_store.BINDDB_TYPE, [fleet_addr, fleet_db, funder, 1800000006], 1),
            ("ReleaseDb", hpplite_store.RELEASEDB_TYPE, [fleet_addr, fleet_db, funder, 1800000007], 2),
            ("DeleteDb", hpplite_store.DELETEDB_TYPE, [fleet_addr, fleet_db, funder, 1800000008], 2),
            ("CapDb", hpplite_store.CAPDB_TYPE, [fleet_addr, fleet_db, 100, 1048576, 1800000009], 2),
            # the fleet's own bucket (its manifests) is a rollup bucket with the fleet in da, bound by the fleet's funder,
            # and the anchor's manifest is Content with the fleet in da and the root's sequence as its height
            ("Bind", hpplite_store.BIND_TYPE, [fleet_addr, funder, 1800000010], 2),
            ("Content", hpplite_store.CONTENT_TYPE, [fleet_addr, 7, eth.keccak256(b"a fleet's manifest")], 0)]
    v["bucketMessages"] = {"chainId": 181228, "payTo": pay_to, "messages": [
        {"type": t, "typeString": ts, "fields": [("0x" + f.hex()) if isinstance(f, bytes) else f for f in fields], "key": KEYS[k],
         "digest": "0x" + eth.typed_digest(sdom, ts, *fields).hex(),
         "signature": "0x" + eth.sign(key(k), eth.typed_digest(sdom, ts, *fields)).hex()}
        for t, ts, fields, k in msgs]}

    # a name's signed batch: Batch(bytes32 instanceId,uint256 height,bytes32 batchHash) in ("HPPLiteDA", "3", chainId, da)
    da = "ab" * 20
    instance = "0x" + eth.keccak256(b"instance").hex()
    v["names"] = []
    for h, d in ((1, b"first batch"), (42, b"")):
        name = "%s-%d" % (da, h)
        digest = hpplite_store.batch_digest(181228, "0x" + da, instance, h, d)
        v["names"].append({"chainId": 181228, "name": name, "instance": instance, "data": "0x" + d.hex(), "key": KEYS[1],
                           "digest": "0x" + digest.hex(), "signature": "0x" + eth.sign(key(1), digest).hex(),
                           "signer": eth.address(key(1))})

    # prices: the store's write quote (#172, #213), the relay's per call (#170); IEEE doubles in this order
    v["writePrices"] = [{"bytes": n, "pricePerWrite": pw, "pricePerMiBMonth": pm, "amount": hpplite_store.write_quote(n, pw, pm)}
                        for n, pw, pm in ((183, 10, 30.0), (40000, 10, 30.5), (3 * 1048576 + 17, 10, 30.5), (1048576, 0, 30.0),
                                          (1000, 1000, 0.0),
                                          # the -1e-9 decides: 55.00000000000001 in doubles (#193)
                                          (52428800, 10, 1.1))]
    v["relayPrices"] = [{"gas": g, "feeWei": f, "margin": m, "ethUsd": e, "minFee": mf, "amount": relay.call_price(g, f, m, e, mf)}
                        for g, f, m, e, mf in ((73500, 10000000, 0.2, 3000.0, 100), (3430000, 10000000, 0.2, 3000.0, 100),
                                               (100000, 1000000000, 0.2, 30.0, 100), (21000, 1, 0.0, 3000.0, 0))]
    qk = relay.quote_key(key(2))
    v["relayQuotes"] = {"relayKey": KEYS[2], "quoteKey": "0x" + qk.hex(), "quotes": [
        {"chainId": 181228, "to": "0x" + "Ab" * 20, "data": d, "amount": a, "gas": g, "until": u,
         "mac": relay.quote_mac(qk, 181228, "0x" + "Ab" * 20, d, a, g, u)}
        for d, a, g, u in (("0x12345678", 3032, 101000, 1800000000), ("0x" + "00" * 100, 1, 1, 0))]}
    # hpplite-relay/1's account messages (#276): in ("HPPLiteRelay", "1", chainId, payTo); a Sponsor is signed by the
    # funder and, as rollupSignature, by the rollup's owner or sequencer over the same digest
    rdom = eth.domain_separator("HPPLiteRelay", "1", 181228, pay_to)
    rollup = "0x" + "cd" * 20
    rmsgs = [("Sponsor", relay.SPONSOR_TYPE, [rollup, funder, 1800000000], 2),
             ("Sponsor", relay.SPONSOR_TYPE, [rollup, funder, 1800000000], 1),
             ("Unsponsor", relay.UNSPONSOR_TYPE, [rollup, funder, 1800000001], 2),
             ("Cap", relay.CAP_TYPE, [rollup, 3, 1800000002], 2),
             # a fleet database (#312): SponsorDb by the funder and by the database's side (its owner or sequencer);
             # UnsponsorDb and CapDb by the sponsor; the fleet's roots: Sponsor and Cap with the fleet in place of the rollup,
             # Sponsor's other side the fleet's owner
             ("SponsorDb", relay.SPONSORDB_TYPE, [fleet_addr, fleet_db, funder, 1800000003], 2),
             ("SponsorDb", relay.SPONSORDB_TYPE, [fleet_addr, fleet_db, funder, 1800000003], 1),
             ("UnsponsorDb", relay.UNSPONSORDB_TYPE, [fleet_addr, fleet_db, funder, 1800000004], 2),
             ("CapDb", relay.CAPDB_TYPE, [fleet_addr, fleet_db, 24, 1800000005], 2),
             ("Sponsor", relay.SPONSOR_TYPE, [fleet_addr, funder, 1800000006], 2),
             ("Cap", relay.CAP_TYPE, [fleet_addr, 48, 1800000007], 2)]
    v["relayMessages"] = {"chainId": 181228, "payTo": pay_to, "messages": [
        {"type": t, "typeString": ts, "fields": [("0x" + f.hex()) if isinstance(f, bytes) else f for f in fields], "key": KEYS[k],
         "digest": "0x" + eth.typed_digest(rdom, ts, *fields).hex(),
         "signature": "0x" + eth.sign(key(k), eth.typed_digest(rdom, ts, *fields)).hex()}
        for t, ts, fields, k in rmsgs]}
    v["fleet"] = fleet_vectors()
    v["relayCalls"] = {"factoryVersion": 10, "selectors": {name: "0x" + sel for sel, name in sorted(
        list(relay.FACTORY_CALLS.items()) + list(relay.ROLLUP_CALLS.items()) + list(relay.DA_CALLS.items()), key=lambda x: x[1])}}
    return v


def fleet_vectors():
    """hpplite-fleet/1 (#297, doc/fleet-protocol.md): dbIds, the signed messages, leaves, roots of 1, 2, 3 and 5
    leaves (the odd node moving up), every leaf's Merkle path, and a manifest's digest"""
    rollup, chain_id = "0x" + "f1" * 20, 181228
    dom = fleet.domain(chain_id, rollup)
    hx = lambda b: "0x" + b.hex()
    owners = [eth.address(key(i)).lower() for i in range(3)]
    dbs = []
    for i, name in enumerate(["notes", "orders.db", "a", "somaro-app-7", "x" * 64]):
        owner = owners[i % 3]
        db = fleet.db_id(owner, name)
        head = {"dbId": db, "instanceId": eth.keccak256(b"instance-%d" % i), "fromHeight": 1 + 10 * i, "toHeight": 5 + 10 * i,
                "chainHashAfter": eth.keccak256(b"chain-%d" % i), "stateRoot": eth.keccak256(b"root-%d" % i),
                "manifestDigest": eth.keccak256(b"manifest-%d" % i), "epoch": i + 1}
        hd = fleet.head_digest(dom, head)
        sig = eth.sign(key(i % 3), hd)
        lf = fleet.leaf(db, owner, fleet.name_hash(name), hd)
        dbs.append({"name": name, "owner": owner, "dbId": db, "head": head, "headDigest": hd, "signature": sig, "leaf": lf,
                    "key": KEYS[i % 3]})
    roots = []
    for n in (1, 2, 3, 5):
        entries = [(d["dbId"], d["leaf"]) for d in dbs[:n]]
        r = fleet.root(entries)
        paths = {}
        for d in dbs[:n]:
            path = fleet.proof(entries, d["dbId"])
            assert fleet.verify(d["leaf"], path, r), "a path that does not reach its root"
            paths[hx(d["dbId"])] = [{"hash": hx(h), "left": left} for h, left in path]
        roots.append({"leaves": [hx(d["dbId"]) for d in dbs[:n]], "root": hx(r), "paths": paths})
    items = [{"dbId": hx(d["dbId"]), "owner": d["owner"], "nameHash": hx(fleet.name_hash(d["name"])),
              "head": {k: (hx(x) if isinstance(x, bytes) else x) for k, x in d["head"].items()}, "signature": hx(d["signature"])}
             for d in dbs[:3]]
    mdata, mdigest = fleet.manifest(chain_id, rollup, 7, items)
    admit = [dbs[0]["dbId"], dbs[0]["owner"], eth.address(key(1)).lower(), 1800000000]
    retire = [dbs[1]["dbId"], 1800000001]
    rootmsg = [7, fleet.root([(d["dbId"], d["leaf"]) for d in dbs[:3]]), mdigest, 1800000002]
    msgs = [("Head", fleet.HEAD_TYPE, [dbs[0]["head"][k] for k in ("dbId", "instanceId", "fromHeight", "toHeight", "chainHashAfter",
                                                                     "stateRoot", "manifestDigest", "epoch")], 0),
            ("Admit", fleet.ADMIT_TYPE, admit, 2), ("Retire", fleet.RETIRE_TYPE, retire, 1), ("Root", fleet.ROOT_TYPE, rootmsg, 2),
            ("Claim", fleet.CLAIM_TYPE, [dbs[0]["dbId"], dbs[0]["head"]["instanceId"], 0, 1800000003], 0)]
    factory = "0x" + "fa" * 20
    fdom = fleet.factory_domain(chain_id, factory)
    create = [owners[0], eth.address(key(2)).lower(), 604800, 0, 1800000004]
    return {"protocol": fleet.PROTOCOL, "chainId": chain_id, "rollup": rollup,
            "databases": [{"name": d["name"], "owner": d["owner"], "dbId": hx(d["dbId"]), "nameHash": hx(fleet.name_hash(d["name"])),
                           "head": {k: (hx(x) if isinstance(x, bytes) else x) for k, x in d["head"].items()},
                           "headDigest": hx(d["headDigest"]), "key": d["key"], "signature": hx(d["signature"]), "leaf": hx(d["leaf"])}
                          for d in dbs],
            "roots": roots,
            "manifest": {"seq": 7, "leaves": 3, "bytes": mdata.decode(), "sha256": hx(mdigest)},
            "messages": [{"type": t, "typeString": ts, "fields": [hx(f) if isinstance(f, bytes) else f for f in fields], "key": KEYS[k],
                          "digest": hx(eth.typed_digest(dom, ts, *fields)), "signature": hx(eth.sign(key(k), eth.typed_digest(dom, ts, *fields)))}
                         for t, ts, fields, k in msgs],
            "factory": factory,
            "factoryMessages": [{"type": "CreateFleet", "typeString": fleet.CREATE_FLEET_TYPE, "fields": create, "key": KEYS[0],
                                 "digest": hx(eth.typed_digest(fdom, fleet.CREATE_FLEET_TYPE, *create)),
                                 "signature": hx(eth.sign(key(0), eth.typed_digest(fdom, fleet.CREATE_FLEET_TYPE, *create)))}]}


def main():
    v = generate()
    if "--write" in sys.argv:
        with open(PATH, "w") as f:
            json.dump(v, f, indent=1, sort_keys=True)
            f.write("\n")
        print("vectors.py: wrote %s" % PATH)
        return 0
    with open(PATH) as f:
        stored = json.load(f)
    bad = [k for k in sorted(set(v) | set(stored)) if json.dumps(v.get(k), sort_keys=True) != json.dumps(stored.get(k), sort_keys=True)]
    if bad:
        print("vectors.py: the reference no longer computes vectors.json for: %s" % ", ".join(bad))
        print("  (a protocol change? regenerate with --write and say so in the commit; a bug? fix the reference)")
        return 1
    # every message the reference operators sign or verify in their own domains has a vector (#279); Batch is in the
    # DA's domain (names) and Permit in the asset's
    missing = [ts for mod, sect in ((hpplite_store, "bucketMessages"), (relay, "relayMessages"), (fleet, "fleet"))
               for name, ts in sorted(vars(mod).items())
               if name.endswith("_TYPE") and name not in ("BATCH_TYPE", "PERMIT_TYPE")
               and ts not in [m["typeString"] for m in v[sect]["messages"] + v[sect].get("factoryMessages", [])]]
    import re
    fleet_types = [m["typeString"] for m in v["fleet"]["messages"] + v["fleet"]["factoryMessages"]]
    sol = os.path.join(here, "..", "..", "contracts", "src")
    for f in ("HPPLiteFleet.sol", "HPPLiteFleetFactory.sol"):
        text = open(os.path.join(sol, f)).read()
        for name, ts in re.findall(r'(\w+_TYPEHASH)\s*=\s*keccak256\(\s*"([^"]+)"\s*\)', text):
            if ts not in fleet_types:
                missing.append("%s %s (%s)" % (f, name, ts))
    if missing:
        print("vectors.py: no vector for: %s" % "; ".join(missing))
        return 1
    n = sum(len(x) if isinstance(x, list) else 1 for x in v.values())
    print("vectors.py: the reference computes vectors.json (%d sections, %d groups)" % (len(v), n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
