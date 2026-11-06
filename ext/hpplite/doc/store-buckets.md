# Store buckets, `hpplite-store/2` (draft)

Status: draft for review (#207), 2026-11-06. Nothing built. The account side
(who holds which key, the console) is in hpp-io/hpplite-operators
docs/console.md. `hpplite-store/2` is a superset of
[`hpplite-store/1`](store-protocol.md): every /1 call stays as it is - the
per-item layer, for strangers and one-off use.

## The idea

A **bucket** is a rollup's space at a store, kept while its **funding
wallet** covers it - no per-item expiry, no renewal calls.

- **One bucket per rollup**, named by its DA address.
- **A funding wallet pays**: an address holding the asset (USDC.e), topped up
  by any transfer. It signs one EIP-2612 permit for the store's `spender`;
  the store pulls what it is owed with `transferFrom`. One funding wallet may
  pay for several buckets.
- **The binding**: a bucket names its funding wallet, signed by the rollup's
  sequencer and by the funding wallet.
- **Writes** into a bucket are the sequencer's: a name (already signed) or a
  content put signed for the bucket. Nothing is paid per call; charges
  accrue to the funding wallet.
- **Covered until**: what the funding wallet can still pay (the lesser of its
  balance and the remaining allowance, before the permit's deadline) over its
  buckets' daily cost.

A store offering buckets must read the chain (`--rpc`): who may write, bind
and prune is the DA's `sequencerWallet()`, as the store follows it (#208).

## Signatures

EIP-712 in the domain `("HPPLiteStore", "2", chainId, payTo)` as `/status`
gives it:

| Type | Signed by | For |
|------|-----------|-----|
| `Bind(address da,address funder,uint256 issuedAt)` | the sequencer and the funder | bind (or rebind) a bucket to a funding wallet |
| `Release(address da,address funder,uint256 issuedAt)` | the funder | stop paying for a bucket |
| `Content(address da,uint256 height,bytes32 hash)` | the sequencer | put bytes into the bucket (`hash` the keccak-256 of the bytes; `height` the batch they belong to, for pruning) |
| `Prune(address da,uint256 height,uint256 issuedAt)` | the sequencer | let go of the bucket's history before `height` |
| `BucketStatement(address da,uint256 height,uint256 pruneHeight,uint256 coveredUntil,uint256 issuedAt)` | the store's `signer` | what the store holds (below) |

`issuedAt` (seconds) orders a bucket's binding and prune messages: the store
takes one only if it is newer than the last it took, and not more than an
hour ahead of its clock.

And the asset's own `Permit(address owner,address spender,uint256 value,uint256 nonce,uint256 deadline)`
(USDC.e on HPP Sepolia and mainnet, `0x401eCb1D350407f13ba348573E5630B83638E30D`,
supports it: version 2).

## Calls

| Call | Signed by | Answer |
|------|-----------|--------|
| `POST /account/<funder>/permit`, body `{value, deadline, v, r, s}` | the funder (the permit) | `200` the account |
| `GET /account/<funder>` | - | `200 {funder, balance, allowance, deadline, owed, dailyCost, coveredUntil, buckets, state}` |
| `POST /bucket/<da>`, body `{funder, issuedAt, sequencerSignature, funderSignature}` | both | `200` the bucket |
| `POST /bucket/<da>/release`, body `{funder, issuedAt, signature}` | the funder | `200` the bucket (parked: no funder) |
| `POST /put?bucket=<da>&height=<n>`, the body the bytes, `X-HPPLite-Signature` the `Content` signature | the sequencer | `200 {cid, bytes, bucket}` |
| `POST /name/<da>-<height>` | the sequencer (the name's own signature) | as /1; for a bucketed DA the name *is* the write: its bytes are held in the bucket, no content put first |
| `POST /bucket/<da>/prune`, body `{height, issuedAt, signature}` | the sequencer | `200` the bucket |
| `GET /bucket/<da>` | - | `200 {da, funder, state, bytes, objects, height, pruneHeight, coveredUntil, owed, statement}` |

Reads are as /1 (`GET /get/<cid>`, `GET /name/...`): public, content
addressed. Errors as /1, and: `401` a signature that does not recover to the
sequencer (or the funder); `402` a write into a bucket that is not covered
(`error` names why: `bucket_unbound`, `bucket_parked`); `409` a binding or
prune older than the last taken; `410` a read of a parked bucket's bytes,
the body naming the bucket and what restores it.

### Binding

- The first binding creates the bucket; a newer one replaces the funder (the
  new funder pays from then on; what accrued before is owed by the old one).
  A rotated-in sequencer (#208) rebinds with its own signature.
- The permit is the funder's, not the bucket's: given once per funding
  wallet, it covers all its buckets. The store keeps the latest permit and
  submits it with its next pull (permit, then `transferFrom`) or when the
  on-chain allowance falls short - so a permit costs nothing until money
  moves. A newer permit replaces the older (the asset's nonce).
- `Release` leaves the bucket unbound: parked at once (below). Rebinding
  restores it.

### Charges

- **Storage** accrues daily per bucket: bytes held x `pricePerMiBMonth` /
  2^20 / 30, in the asset's atomic units with the fraction carried (rounded
  up once a day per funding wallet). At the reference price 1 GiB is 30,720
  units a month ($0.03).
- **Writes**: `pricePerPut` per name or content put, plus nothing per byte
  (bytes cost as storage).
- **Pulls**: the store pulls `owed` from the funding wallet, to `payTo`, when
  it passes `pullThreshold`, or `pullIntervalSeconds` after the last pull, or
  when it exceeds what the wallet can still pay (balance, allowance,
  deadline) - so a wallet running dry is found within a day, not at the
  threshold.
- **A refused pull** is one the chain refuses for the funder's reasons: the
  balance or the allowance below `owed`, the permit past its deadline or
  replaced, the allowance revoked. The store finds out by simulating the call
  first (no gas spent on a refusal). Anything else - the RPC unreachable, the
  store's own gas, a reorg - is the store's failure: it tries again, the
  bucket's state does not change, and it warns (as `/health/warnings` does
  for claims).

### States

```
            bind, pull ok                       pull refused, release
  (none) ───────────────▶ covered ──────────────────────────────────▶ parked
                             ▲                                          │
                             └──────── pull ok (topped up, rebound) ────┤
                                                                        │ parkSeconds (30 days)
                                                                        ▼
                                                                     deleted
```

- **covered**: served; writes accepted.
- **parked**: not served (`410`), writes refused (`402 bucket_parked`),
  storage still accrues. The store tries the pull daily; one that succeeds
  covers it again. Bytes may move to cheaper storage.
- **deleted**: the bucket's bytes and names are gone; what was owed is
  written off. A new binding starts an empty bucket.

Only a refused pull (or a release) parks a bucket, and only `parkSeconds`
after that deletes it. A covered bucket is never deleted, and a store does
not delete one for its own failures.

### Pruning

History is what makes a rollup verifiable, so a store never prunes at its
own judgement. A `Prune` at `height` lets go of the bucket's names below it
and its content put with a lower `height`: the node signs one after an
anchored snapshot at or below `height` (#203), as its setting says (keep
everything; or keep N days for point-in-time recovery, #205). The store
deletes them (or not - a store may keep more than asked) and reports
`pruneHeight`.

Bytes put into no bucket and named by nothing (an interrupted put, #200) are
the store's to sweep on a timer.

### The bucket statement

`statement` in `GET /bucket/<da>`: `{height, pruneHeight, coveredUntil,
issuedAt, signature}` - the store's signed word that it holds the DA's names
from `pruneHeight` to `height`, covered until `coveredUntil`. A store that
deletes a covered bucket's bytes, or bytes above a prune point, is caught by
its own statement. (What the node commits on chain with an anchor in place
of per-item receipts: open.)

## `GET /status`

/1's answer, `"protocol": "hpplite-store/2"`, and:

```json
"buckets": {"domain": {"name": "HPPLiteStore", "version": "2", "chainId": 190415, "verifyingContract": "0x…"},
            "spender": "0x…", "pricePerPut": 10, "pricePerMiBMonth": 30.0,
            "pullThreshold": 1000000, "pullIntervalSeconds": 2592000, "parkSeconds": 2592000}
```

`spender` is the address the permit names (the store's puller; it holds a
little ETH for the pulls' gas).

## The node

- A bucket store is given as today (`store=https://…`); the node uses
  buckets when `/status` offers them and it has a funder.
- **Self-provisioned**: the node creates a funding key beside its own
  (`<key>.funding`), shows its address (`hpplite_status.funder`), signs the
  binding and the permit (a cap, e.g. 20 USDC.e, and a deadline a year out;
  renewed when the allowance falls under a quarter or the deadline is within
  30 days).
- **Through the console**: given `funder=<address>`, the node signs only its
  half of the binding; the console's signer signs the rest and the permit.
- It writes names (no content put first) and its other content (manifests,
  snapshots) as signed bucket puts, and reports `covered_until` and the
  bucket's state in `hpplite_status`.

## Open

- What an anchor commits in bucket mode, in place of per-item receipts (the
  statement's height and signature?).
- Pull cadence and threshold for mainnet; whether parked storage costs less.
- Mirroring a bucket at two stores; moving one between stores.
- Migrating a rollup's per-item history into a bucket.
- The relay charging the funding wallet the same way (its own permit).
