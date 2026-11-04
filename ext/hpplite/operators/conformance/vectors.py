#!/usr/bin/env python3
"""Test vectors for the store and relay protocols (#189): fixed inputs and
what the reference computes from them - keccak, addresses, signatures
(RFC 6979, so deterministic), CIDs, the receipt's and the name's EIP-712
digests, x402 batch-settlement's channel id, voucher, refund and claim
digests, the store's and the relay's prices, the relay's quote MAC and the
selectors it takes. vectors.json is what another implementation (the
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
from x402seller import channels, eth  # noqa: E402
import pin_service  # noqa: E402
import relay  # noqa: E402

PATH = os.path.join(here, "vectors.json")
KEYS = ["0x" + (bytes([i]) * 32).hex() for i in (1, 2, 0x42)]


def key(i):
    return bytes.fromhex(KEYS[i][2:])


def generate():
    v = {"version": 1, "about": "operators/conformance/vectors.py (#189); keys are test keys, never fund them"}
    v["keccak256"] = [{"in": h, "out": "0x" + eth.keccak256(bytes.fromhex(h[2:])).hex()}
                      for h in ("0x", "0x616263", "0x" + "ff" * 200)]
    v["addresses"] = [{"key": k, "address": eth.address(bytes.fromhex(k[2:]))} for k in KEYS]
    digests = ["0x" + eth.keccak256(t).hex() for t in (b"a", b"hpplite", b"")]
    v["signatures"] = [{"key": KEYS[i % 3], "digest": d, "signature": "0x" + eth.sign(key(i % 3), bytes.fromhex(d[2:])).hex()}
                       for i, d in enumerate(digests)]
    datas = [b"", b"hpplite", bytes(range(256)) * 5]
    v["cids"] = [{"data": "0x" + d.hex(), "cid": pin_service.cid_of(d)} for d in datas]

    # the store's receipt: Receipt(bytes32 hash,string cid,uint256 expires) in ("HPPLitePinService", "1", chainId, payTo)
    pay_to = "0x" + "11" * 20
    signer = pin_service.Signer(key(0), 181228, pay_to)
    v["receipts"] = []
    for d in datas[1:]:
        cid = pin_service.cid_of(d)
        r = signer.receipt(eth.keccak256(d), cid, 1800000000)
        digest = eth.typed_digest(signer.domain, pin_service.RECEIPT_TYPE, eth.keccak256(d), eth.keccak256(cid.encode()), 1800000000)
        v["receipts"].append({"key": KEYS[0], "chainId": 181228, "payTo": pay_to, "data": "0x" + d.hex(),
                              "expires": 1800000000, "digest": "0x" + digest.hex(), "receipt": r})

    # a name's signed batch: Batch(bytes32 instanceId,uint256 height,bytes32 batchHash) in ("HPPLiteDA", "3", chainId, da)
    da = "ab" * 20
    instance = "0x" + eth.keccak256(b"instance").hex()
    v["names"] = []
    for h, d in ((1, b"first batch"), (42, b"")):
        name = "%s-%d" % (da, h)
        digest = pin_service.batch_digest(181228, name, instance, d)
        v["names"].append({"chainId": 181228, "name": name, "instance": instance, "data": "0x" + d.hex(), "key": KEYS[1],
                           "digest": "0x" + digest.hex(), "signature": "0x" + eth.sign(key(1), digest).hex(),
                           "signer": eth.address(key(1))})

    # x402 batch-settlement (#135)
    cfg = {"payer": eth.address(key(1)), "payerAuthorizer": eth.address(key(1)), "receiver": pay_to,
           "receiverAuthorizer": eth.address(key(0)), "token": "0x401eCb1D350407f13ba348573E5630B83638E30D",
           "withdrawDelay": 86400, "salt": "0x" + eth.keccak256(b"salt").hex()}
    cid = channels.channel_id(cfg, 181228)
    v["channels"] = {
        "chainId": 181228, "escrow": channels.ESCROW, "config": cfg, "channelId": cid,
        "vouchers": [{"maxClaimableAmount": m, "digest": "0x" + channels.voucher_digest(cid, m, 181228).hex(),
                      "signature": "0x" + eth.sign(key(1), channels.voucher_digest(cid, m, 181228)).hex()} for m in (10, 123456789)],
        "refund": {"nonce": 3, "amount": 500, "digest": "0x" + channels.refund_digest(cid, 3, 500, 181228).hex()},
        "claimBatch": {"entries": [[cid, 1000, 990], ["0x" + "cd" * 32, 5, 5]],
                       "digest": "0x" + channels.claim_batch_digest([(cid, 1000, 990), ("0x" + "cd" * 32, 5, 5)], 181228).hex()},
    }

    # prices: the store's per put (#172), the relay's per call (#170); IEEE doubles in this order
    v["storePrices"] = [{"bytes": n, "pricePerPin": pp, "pricePerByte": pb, "pricePerMiBMonth": pm, "retentionSeconds": r,
                         "amount": pin_service.put_price(n, pp, pb, pm, r)}
                        for n, pp, pb, pm, r in ((183, 10, 0, 30.0, 2592000), (40000, 10, 0, 30.5, 5184000),
                                                 (3 * 1048576 + 17, 10, 0, 30.5, 5184000), (1048576, 0, 0, 30.0, 2592000),
                                                 (1000, 1000, 1, 30.0, 60))]
    v["relayPrices"] = [{"gas": g, "feeWei": f, "margin": m, "ethUsd": e, "minFee": mf, "amount": relay.call_price(g, f, m, e, mf)}
                        for g, f, m, e, mf in ((73500, 10000000, 0.2, 3000.0, 100), (3430000, 10000000, 0.2, 3000.0, 100),
                                               (100000, 1000000000, 0.2, 30.0, 100), (21000, 1, 0.0, 3000.0, 0))]
    qk = relay.quote_key(key(2))
    v["relayQuotes"] = {"relayKey": KEYS[2], "quoteKey": "0x" + qk.hex(), "quotes": [
        {"chainId": 181228, "to": "0x" + "Ab" * 20, "data": d, "amount": a, "gas": g, "until": u,
         "mac": relay.quote_mac(qk, 181228, "0x" + "Ab" * 20, d, a, g, u)}
        for d, a, g, u in (("0x12345678", 3032, 101000, 1800000000), ("0x" + "00" * 100, 1, 1, 0))]}
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
    n = sum(len(x) if isinstance(x, list) else 1 for x in v.values())
    print("vectors.py: the reference computes vectors.json (%d sections, %d groups)" % (len(v), n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
