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
sys.path.insert(0, here)
from x402seller import eth  # noqa: E402
import hpplite_store  # noqa: E402
import relay  # noqa: E402
from buyer import TRANSFER_TYPE, PERMIT2, UPTO_PROXY, permit2_upto_digest  # noqa: E402

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
    msgs = [("Bind", hpplite_store.BIND_TYPE, [da2, funder, 1800000000], 1),
            ("Release", hpplite_store.RELEASE_TYPE, [da2, funder, 1800000001], 2),
            ("Content", hpplite_store.CONTENT_TYPE, [da2, 7, eth.keccak256(b"first batch")], 1),
            ("Prune", hpplite_store.PRUNE_TYPE, [da2, 5, 1800000002], 1),
            ("BucketStatement", hpplite_store.STATEMENT_TYPE, [da2, 9, 5, hpplite_store.FOREVER, 1800000003], 0),
            # the funder's own words (#274, #275): its daily cap on the bucket, and a release that deletes it now
            ("Cap", hpplite_store.CAP_TYPE, [da2, 100, 1048576, 1800000004], 2),
            ("Delete", hpplite_store.DELETE_TYPE, [da2, funder, 1800000005], 2),
            # a key's bucket lets go of CIDs (#349): cids the keccak-256 of the CIDs joined by "\n"
            ("Unpin", hpplite_store.UNPIN_TYPE, [da2, eth.keccak256("\n".join([hpplite_store.cid_of(b"first batch"),
                                                                                 hpplite_store.cid_of(b"second")]).encode()), 1800000010], 1)]

    v["bucketMessages"] = {"chainId": 181228, "payTo": pay_to, "messages": [
        {"type": t, "typeString": ts, "fields": [("0x" + f.hex()) if isinstance(f, bytes) else f for f in fields], "key": KEYS[k],
         "digest": "0x" + eth.typed_digest(sdom, ts, *fields).hex(),
         "signature": "0x" + eth.sign(key(k), eth.typed_digest(sdom, ts, *fields)).hex()}
        for t, ts, fields, k in msgs]}

    # what a funding wallet signs in a token's own domain (#342): the EIP-2612 permit a store or relay pulls with, and
    # x402 exact's EIP-3009 TransferWithAuthorization; the C node's signer is checked against these
    usdc, chip = "0x401eCb1D350407f13ba348573E5630B83638E30D", "0x0Ac610E77ec39AB5c6B402B376548Ac02ACc5a48"
    spender, pay = "0x" + "5c" * 20, "0x" + "97" * 20
    pmsgs = [("Permit", hpplite_store.PERMIT_TYPE, ("Bridged USDC", "2", usdc), [funder, spender, 20000000, 0, 1831536000], 2),
             ("Permit", hpplite_store.PERMIT_TYPE, ("House Chip", "1", chip), [funder, spender, 2 ** 53 - 1, 7, 1831536000], 2),
             ("TransferWithAuthorization", TRANSFER_TYPE, ("Bridged USDC", "2", usdc),
              [funder, pay, 1000000, 1800000000, 1800000120, eth.keccak256(b"nonce one")], 2),
             ("TransferWithAuthorization", TRANSFER_TYPE, ("House Chip", "1", chip),
              [funder, pay, 3000, 0, 1800000120, eth.keccak256(b"nonce two")], 2)]
    v["paymentMessages"] = {"chainId": 181228, "messages": [
        {"type": t, "typeString": ts, "domain": {"name": n, "version": ver, "verifyingContract": a},
         "fields": [("0x" + f.hex()) if isinstance(f, bytes) else f for f in fields], "key": KEYS[k],
         "digest": "0x" + eth.typed_digest(eth.domain_separator(n, ver, 181228, a), ts, *fields).hex(),
         "signature": "0x" + eth.sign(key(k), eth.typed_digest(eth.domain_separator(n, ver, 181228, a), ts, *fields)).hex()}
        for t, ts, (n, ver, a), fields, k in pmsgs]}

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
             # a creation paid by a funder (#376): the funder's signature, and the owner's over the same digest
             ("SponsorCreate", relay.SPONSOR_CREATE_TYPE, [eth.address(key(1)).lower(), funder, 0, 1800000003], 2),
             ("SponsorCreate", relay.SPONSOR_CREATE_TYPE, [eth.address(key(1)).lower(), funder, 0, 1800000003], 1)]

    # x402 upto (#358): what a payer signs - Permit2's PermitWitnessTransferFrom of a token to x402's upto proxy, the
    # witness naming who is paid and the facilitator; Permit2's own domain (a name, no version). The C node's signer is
    # checked against these; amounts and nonces are decimals, past 2^64 (HPP has 18 decimals)
    hpp, facilitator = "0xb48334e7938367bc24fe1f19000d6f06c622e6c7", "0xaA03c7BD2fAf554db507D78CD3bF126a3DD2E033"
    umsgs = [({"from": funder, "permitted": {"token": hpp, "amount": str(3 * 10 ** 17)}, "spender": UPTO_PROXY,
               "nonce": str(int.from_bytes(eth.keccak256(b"nonce three")[:16], "big")), "deadline": "1800000120",
               "witness": {"to": pay, "facilitator": facilitator, "validAfter": "1799999400"}}, 2),
             ({"from": funder, "permitted": {"token": chip, "amount": str(10 ** 20)}, "spender": UPTO_PROXY,
               "nonce": "1", "deadline": "1831536000",
               "witness": {"to": pay, "facilitator": facilitator, "validAfter": "0"}}, 1)]
    v["permit2Messages"] = {"chainId": 181228, "permit2": PERMIT2, "messages": [
        {"type": "PermitWitnessTransferFrom", "authorization": a, "key": KEYS[k],
         "digest": "0x" + permit2_upto_digest(181228, a).hex(),
         "signature": "0x" + eth.sign(key(k), permit2_upto_digest(181228, a)).hex()}
        for a, k in umsgs]}

    v["relayMessages"] = {"chainId": 181228, "payTo": pay_to, "messages": [
        {"type": t, "typeString": ts, "fields": [("0x" + f.hex()) if isinstance(f, bytes) else f for f in fields], "key": KEYS[k],
         "digest": "0x" + eth.typed_digest(rdom, ts, *fields).hex(),
         "signature": "0x" + eth.sign(key(k), eth.typed_digest(rdom, ts, *fields)).hex()}
        for t, ts, fields, k in rmsgs]}
    v["relayCalls"] = {"factoryVersion": 10, "selectors": {name: "0x" + sel for sel, name in sorted(
        list(relay.FACTORY_CALLS.items()) + list(relay.ROLLUP_CALLS.items()) + list(relay.DA_CALLS.items()), key=lambda x: x[1])}}
    return v



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
    missing = [ts for mod, sect in ((hpplite_store, "bucketMessages"), (relay, "relayMessages"))
               for name, ts in sorted(vars(mod).items())
               if name.endswith("_TYPE") and name not in ("BATCH_TYPE", "PERMIT_TYPE")
               and ts not in [m["typeString"] for m in v[sect]["messages"] + v[sect].get("factoryMessages", [])]]
    if missing:
        print("vectors.py: no vector for: %s" % "; ".join(missing))
        return 1
    n = sum(len(x) if isinstance(x, list) else 1 for x in v.values())
    print("vectors.py: the reference computes vectors.json (%d sections, %d groups)" % (len(v), n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
