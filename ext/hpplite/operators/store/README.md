# The reference store for HPPLite, `hpplite-store/2` (#213)

What a storage provider for HPPLite nodes looks like, in one Python file
with no dependencies beyond the standard library (`../x402seller/` does
keccak-256, secp256k1, EIP-712, x402 and the chain in pure Python). The
protocol is [doc/store-protocol.md](../../doc/store-protocol.md); the
canonical store (hpp-io/hpplite-operators) speaks the same and is checked by
the same conformance suite (`../conformance`).

```
hpplite_store.py --port 8420 --dir /var/lib/hpplite-store --signer-key /etc/hpplite-store/key \
    --pay-to 0xYourWallet --rpc https://sepolia.hpp.io --network eip155:181228 \
    --facilitator https://facilitator-sepolia.hpp.io     # x402 top-ups; without it, permit pulls only

hpplite_store.py ... --price-per-write 0 --price-per-mib-month 0        # free (development)
```

## The model

- **A bucket per rollup**, named by its DA address, kept while its
  **account** covers it - no per-item expiry, nothing to renew.
- **An account per funding wallet**, which may pay for several buckets. Its
  credit is what was paid in less what was charged. Money comes in by x402
  `exact` (a payer online: an agent, an "extend now", a write's 402) or by
  an EIP-2612 permit the store pulls with (the store's own move, so storage
  stays paid while the node is off). Pulls are simulated first: a refusal
  for the funder's reasons parks the account's buckets; the store's own
  failures only warn (`/status` `warnings`).
- **Writes are the rollup's sequencer's**, signed: a name (`Batch`, as the
  DA contract takes it), or a content put (`Content` over the bucket, the
  height and the bytes). A name and a content put of the same bytes are
  held once.
- **Charges**: storage (bytes x time at `--price-per-mib-month`, booked at
  each tick) and `--price-per-write` per write.
- **States**: covered; parked (not served, 410; writes 402) when the account
  cannot pay or the funder released it, restorable by paying; deleted
  `--park-seconds` later.
- **Pruning** only at a prune point the sequencer signs; bytes in no bucket
  (an interrupted put) are swept after `--sweep-age`.
- **The bucket statement**, signed by `--signer-key`: the heights held and
  covered-until. A node commits it with each anchor.

With `--rpc` the store asks the chain who a rollup's sequencer is (bindings,
writes, prunes: `sequencer()`, so a rotated key takes over at once, #208)
and whether a name's instance holds the lease (#167); it refuses with 503
what it cannot check (#198). Without `--rpc` the first binding's sequencer
keeps the bucket: development and the node's unit tests only. The puller
(`--spender-key`, the signer's by default) holds a little ETH for the
pulls' gas.

## Layout

A directory standing for an object store:

| Key | Holds |
|-----|-------|
| `c/<cid>`, `c/<cid>.refs` | content bytes; `{da: height}`, the buckets holding them |
| `b/<da>/bucket.json` | the bucket: funder, state, bytes, height, prune point |
| `b/<da>/n/<height>` | a name: the instance (32 bytes), the signature (65), the bytes' CID |
| `a/<funder>.json` | the account: credit, the permit, the last pull, why it is uncovered |

The reference is one process (its ledger under one lock); the canonical
store keeps the ledger in Durable Objects.

## Tests

- `bucket_selftest.sh` (ctest `test_store_buckets`): on anvil, real rollups,
  MockUSDC's permit (#212) and the mock facilitator - binding, writes, a
  pull from a wallet with no ETH, a refused pull parking the bucket and a
  refill covering it again, an x402-only account parked then deleted, the
  statement, prune, release, the sweep.
- `edges_selftest.py` (ctest `test_store_edges`): a top-up whose settlement
  fails is taken back; an unreadable chain answers 503 with nothing held.
- `../conformance/selftest.sh` (ctest `test_conformance`): the suite, free
  and paid.
- The node's store client against it: ctest `test_store`, anvil scenarios
  19, 20 and 30.

`kubo_mock.py` and `s3_mock.py` stand in for a Kubo node and an S3 bucket,
the node's other two kinds of store.
