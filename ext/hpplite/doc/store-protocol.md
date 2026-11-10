# The store protocol, `hpplite-store/2`

What an HPPLite node expects of a store given as `store=http(s)://host:port`
(#207, #213, #214). The reference implementation is
[operators/store/hpplite_store.py](../operators/store/README.md); the
canonical store (hpp-io/hpplite-operators) speaks the same, and both are
checked by the same conformance suite (`operators/conformance`, #189). The
account side - who holds which key, the console - is hpp-io/hpplite-operators
docs/console.md.

`hpplite-store/2` replaced `hpplite-store/1` (per-item retention, pins and
receipts, 2026-11-06) before either was announced, so nothing was kept for
compatibility (see [Removed](#removed)). A service says which protocol it
speaks in `GET /status` (`"protocol": "hpplite-store/2"`); a node writes
nothing to a service that names another, or none.

## The idea

- **A bucket** is a rollup's space at a store, named by its DA address, kept
  while its account covers it. No per-item expiry, no renewal.
- **An account** belongs to a **funding wallet** (an address holding the
  asset, USDC.e) and may pay for several buckets. Its `credit` is what was
  paid in minus what was charged; negative is owed. Money comes in two ways:
  - **x402** (`exact`), started by a payer who is online: an agent, a
    stranger, an "extend now" button. The facilitator pays the gas.
  - **a permit pull**, started by the store: the funding wallet signs one
    EIP-2612 permit, and the store takes what is owed with `transferFrom` -
    so storage stays paid while the node is off for months.
- **Charges** accrue to the account: storage (bytes held x time) and a fee
  per write.
- **Writes** are the rollup's sequencer's, signed: a name (as today) or a
  content put signed for the bucket. A write into a bucket its account
  cannot cover answers `402` - pay it and the write goes through.
- **Covered until**: when the account's credit, plus what the permit can
  still pull, runs out at the current daily cost. What the permit can pull is
  read from the chain; when a read fails the store goes by its last good
  read, never signing or reporting less than it knows (#219).

A store must read the chain (`--rpc`): who may bind, write and prune is the
rollup's `sequencer()` - the key its owner names, so a rotation takes effect
at once (#208); a name also needs the lease (`sequencerWallet()` and
`sequencerInstance()`, #167). The reference store without `--rpc` keeps a
bucket for its first binder's sequencer key: development and unit tests
only.

## Signatures

EIP-712 in the domain `("HPPLiteStore", "2", chainId, payTo)` as `/status`
gives it:

| Type | Signed by | For |
|------|-----------|-----|
| `Bind(address da,address funder,uint256 issuedAt)` | the sequencer and the funder; or the funder alone when it is the rollup's `owner()` on chain (#273) | bind (or rebind) a bucket to an account |
| `Release(address da,address funder,uint256 issuedAt)` | the funder | stop paying for a bucket (parked; deleted `parkSeconds` later) |
| `Delete(address da,address funder,uint256 issuedAt)` | the funder | stop paying and delete the bucket now (#275): `POST /bucket/<da>/release` with `delete: true` |
| `Content(address da,uint256 height,bytes32 hash)` | the sequencer | put bytes into the bucket (`hash` the keccak-256 of the bytes; `height` the batch they belong to, for pruning) |
| `Prune(address da,uint256 height,uint256 issuedAt)` | the sequencer | let go of the bucket's history before `height` |
| `Cap(address da,uint256 writesPerDay,uint256 bytesPerDay,uint256 issuedAt)` | the funder | bound what the bucket may take in a day (#274); 0 = no bound on that axis |
| `BucketStatement(address da,uint256 height,uint256 pruneHeight,uint256 coveredUntil,uint256 issuedAt)` | the store's `signer` | what the store holds (below) |

`issuedAt` (seconds) orders a bucket's bindings and prunes: the store takes
one only if it is newer than the last it took and at most an hour ahead of
its clock.

The permit is the asset's own `Permit(address owner,address spender,uint256
value,uint256 nonce,uint256 deadline)` (USDC.e,
`0x401eCb1D350407f13ba348573E5630B83638E30D` on HPP Sepolia and mainnet,
supports it: version 2).

## Calls

| Call | Signed / paid | Answer |
|------|---------------|--------|
| `POST /bucket/<da>`, body `{funder, issuedAt, sequencerSignature, funderSignature}` | the sequencer and the funder | `200` the bucket |
| `POST /bucket/<da>/release`, body `{funder, issuedAt, signature[, delete]}` (`delete: true` signs `Delete`, not `Release`) | the funder | `200` the bucket (`parked`, or `deleted` with `delete`) |
| `POST /bucket/<da>/prune`, body `{height, issuedAt, signature}` | the sequencer | `200` the bucket |
| `POST /bucket/<da>/cap`, body `{funder, writesPerDay, bytesPerDay, issuedAt, signature}` | the funder | `200 {da, cap, today}` |
| `GET /bucket/<da>` | - | `200 {da, funder, state, bytes, objects, height, pruneHeight, coveredUntil, statement, cap, today}` |
| `POST /name/<da>-<height>`, the body the bytes, the instance (32 bytes) and the signature (65 bytes) | the sequencer; `402` if not covered | `200 {name, bytes, signer}`: the name is the write - its bytes are held in the bucket |
| `POST /put?bucket=<da>&height=<n>`, the body the bytes, `X-HPPLite-Signature` the `Content` signature | the sequencer; `402` if not covered | `200 {cid, bytes}` |
| `GET /name/<da>-<height>`, `GET /get/<cid>` | - | the bytes, as /1 (`X-HPPLite-Signer` on names); a name's batch bytes are also read by their CID |
| `POST /account/<funder>/permit`, body `{value, deadline, signature}` (65 bytes hex; `{v, r, s}` taken too) | the funder (the permit) | `200` the account |
| `POST /account/<funder>/pay?amount=<units>` | x402 `exact` | `200` the account: `credit` raised by the amount |
| `GET /account/<funder>` | - | `200 {funder, credit, balance, allowance, deadline, dailyCost, coveredUntil, buckets, state}` |
| `GET /status` | - | below |

Names keep /1's rules: `<da>-<height>`, the body the batch's bytes followed
by the sequencer's instance id and its EIP-712 `Batch` signature in the DA's
domain; a signer other than the DA's sequencer `403`, an instance other than
the lease's `409`, the chain unreadable `503` (#198); the same body again
`200`. The first-signer rule and "content before name" (#199) are gone: a
name is taken only from the chain's sequencer, into a bucket it bound.

Errors: `400` malformed; `401` a signature that does not recover to the
sequencer (or the funder); `402` a write its account cannot cover, or an
unpaid `/pay` (x402 requirements, below); `403`/`409`/`503` as above; `404`
not held, or no such bucket; `409` a binding or prune older than the last
taken; `410` a read of a parked bucket (the body says what restores it);
`413` over the size limit; `429 bucket_capped` a write past the bucket's
daily cap (below), nothing charged, `Retry-After` and `retryAfter` the
seconds to the next window; a `429` with any other `error` is the store's
own rate limit, with `Retry-After`: nothing was charged and the write is
sent again later. JSON bodies with `error`; for `402` also
`bucket_unbound`, `bucket_parked` or `account_uncovered`.

### Binding

- The first binding creates the bucket. A newer one changes the funder: the
  new account pays from then on; what accrued before stays with the old one.
  A rotated-in sequencer (#208) rebinds with its own signature.
- `Release` leaves the bucket unbound, which parks it at once; a binding
  restores it.
- One funder signing both halves is the self-paying case (an agent, or a
  self-provisioned node whose funding key signs as well).
- A funder that is the rollup's `owner()` on chain binds alone (#273): the
  owner names the sequencer, so its authority over the rollup covers the
  sequencer's consent. The bucket's sequencer is the chain's. This is how a
  platform that owns its apps' rollups pays for their buckets from one
  treasury without holding the apps' keys; the node runs with `funder=`
  (hpplite #272) and binds nothing itself.

### The daily cap

- The funder may bound a bucket (#274): `Cap(da, writesPerDay, bytesPerDay,
  issuedAt)`, 0 on an axis meaning no bound there, a newer `issuedAt`
  replacing the last; the cap goes with the funder (a binding to another
  funder drops it). A platform paying for many apps from one account bounds
  what an app's key - which sits in the app's process - can cost it.
- The store counts every new name and content put (bytes held already count
  nothing; a node's batch is two writes, its bytes and its name) in a rolling
  window of 86400 s from the first write in it:
  `today {writes, bytes, since}` on `GET /bucket/<da>`, kept whether or not
  there is a cap. A write that would pass the cap answers `429
  bucket_capped` with `cap`, `today`, `retryAfter` (seconds to the window's
  end) and a `Retry-After` header; nothing is charged and the bucket's state
  does not change. A node keeps the batch pending and tries again at its
  next flush, saying why once (#288).

### The permit

- One per account, not per bucket. The store keeps the latest and submits it
  with its next pull (permit, then `transferFrom`) or when the chain's
  allowance falls short, so a permit costs nothing until money moves. A
  newer permit replaces an older one (the asset's nonce).
- The asset keeps one permit nonce per wallet, whoever the spender is. A
  wallet's permit held here and not yet sent, and another held by a second
  operator (a relay with accounts, another store), carry the same nonce:
  the first sent voids the other, and that operator's pull then fails and
  leaves the account uncovered. A funder that pays two operators from one
  wallet gives its permit to one and tops the other up over x402 (`/pay`),
  or signs the second permit once the first is on chain (`sent`) (#281).
- The permit's `spender` is `/status`'s `buckets.spender` (the store's
  puller, which holds a little ETH for gas); `transferFrom` pays `payTo`.

### Charges and pulls

- **Storage** accrues per bucket for the time its bytes are held: bytes x
  `pricePerMiBMonth` / 2^20 per 30 days, booked to the account at each of
  the store's ticks (at least daily), the fraction carried. At 30
  units per MiB-month, 1 GiB is 30,720 units a month ($0.03).
- **Writes**: `pricePerWrite` per name or content put.
- **Pulls**, when the account has a permit: the store pulls what is owed
  (`-credit`) once it passes `pullThreshold`, or `pullIntervalSeconds` after
  the last pull, or as soon as the owed exceeds what the wallet can still
  pay (its balance, the allowance, before the deadline) - so a wallet
  running dry is found within a day.
- **Credit first**: x402 payments add credit; charges spend it; a pull
  happens only when credit is below zero.

### When it cannot pay

The account is **uncovered** when it owes and cannot pay: no permit, or a
pull **refused** - one the chain refuses for the funder's reasons (the
balance or the allowance short, the permit past its deadline or replaced,
the allowance revoked). The store finds out by simulating the pull first, so
a refusal costs no gas. Anything else - the RPC unreachable, the store's own
gas, a reorg - is the store's failure: it tries again, nothing changes for
the account, and it warns (`/health/warnings`).

An x402 account (no permit) is uncovered once its credit is spent.

### Bucket states

```
          bind (covered account)                 account uncovered, release
  (none) ───────────────────────▶ covered ────────────────────────────▶ parked
                                     ▲                                    │
                                     └──── covered again (paid, rebound) ─┤
                                                                          │ parkSeconds (30 days)
                                                                          ▼
                                                                       deleted
```

- **covered**: served; writes accepted.
- **parked**: not served (`410`), writes refused (`402 bucket_parked`);
  storage still accrues. The store retries a pull daily; paying (`/pay`, or
  a pull that succeeds) covers it again. Its bytes may move to cheaper
  storage.
- **deleted**: the bucket's bytes and names are gone; what it owed is
  written off. A new binding starts an empty bucket.

Only an uncovered account or a release parks a bucket, and only
`parkSeconds` later is it deleted - unless its funder asks for the deletion
itself (`delete: true` on the release, signed over `Delete`, #275: a
platform removing an app): then the names and the records go at once, the
bytes shared with no other bucket are swept, the bucket reads `deleted`, and
a new binding starts it empty. A covered bucket is never deleted by the
store, and a store never deletes for its own failures.

### Pruning and sweeping

History is what makes a rollup verifiable, so a store never prunes at its
own judgement. A `Prune` at `height` lets go of the bucket's names below it
and its content put with a lower `height`. The node signs one after an
anchored snapshot at or below `height` (#203), as its setting says (`keep=all`,
everything; or `keep=<duration>`, #223: the newest snapshot at least that old,
the window behind it kept for point-in-time recovery, #205). The store
deletes them (it may keep more) and reports `pruneHeight`.

Bytes that are in no bucket (an interrupted put, #200) are the store's to
sweep on a timer.

### The bucket statement

`statement` in `GET /bucket/<da>`: `{height, pruneHeight, coveredUntil,
issuedAt, signer, signature}` - the store's signed word that it holds the
DA's names from `pruneHeight` to `height`, covered until `coveredUntil`
(seconds; 2^53-1, 9007199254740991, when nothing is charged: a number every
JSON encoder prints and every parser reads back exactly, #216). An
anchor commits the latest statement in its `receipt` field (the contract
takes opaque bytes: a compact form, version byte 2; /1's per-item receipt,
version 1, is neither written nor read since v0.8, #249), so a store that deletes a covered bucket's bytes, or
bytes above a prune point, is caught by its own signature on chain.

## `GET /status`

```json
{
  "service": "hpplite-store", "protocol": "hpplite-store/2",
  "signer": "0x…",
  "domain": {"name": "HPPLiteStore", "version": "2", "chainId": 190415, "verifyingContract": "0x…"},
  "buckets": {"asset": "0x…", "payTo": "0x…", "spender": "0x…",
              "pricePerWrite": 10, "pricePerMiBMonth": 30.0,
              "pullThreshold": 1000000, "pullIntervalSeconds": 2592000, "parkSeconds": 2592000},
  "payment": {"scheme": "exact", "network": "eip155:190415", "facilitator": "https://…", "minPay": 1000000}
}
```

`payment` is `null` for a store without x402 (permit pulls only). A free
store (development) has `buckets` with zero prices and charges nothing.
Amounts are whole numbers of the asset's atomic units, but
`pricePerMiBMonth` is a number that may carry a fraction: `30` and `30.0`
are the same quote, and a client reads it as a decimal (#282).

## Payment (x402 v2, `exact` only)

A `402` carries x402 v2 requirements, in the body and base64 in
`PAYMENT-REQUIRED`: `exact` (EIP-3009 `TransferWithAuthorization`) to
`payTo`, `extra` naming the token's EIP-712 domain. For a write the amount
is a top-up - the larger of `minPay` and what covers the write and 30 days
of the account's storage - so an agent pays now and then, not per write;
for `/pay`, the amount asked. The payer repeats the request with
`PAYMENT-SIGNATURE`; the store verifies, credits the account, does the work
and settles. A settlement that fails takes the credit back (the write stays
charged to the account as owed).

## The node

- `store=https://…` as today; the node needs a funder (below) to write.
- **Self-provisioned**: the node creates a funding key beside its own
  (`<key>.funding`), reports its address (`hpplite_status.funder`) - send
  USDC.e there to fund it - signs the binding with both keys and the permit
  (a cap, e.g. 20 USDC.e, a deadline a year out; renewed when the allowance
  falls under a quarter or the deadline is within 30 days). With no permit
  (`store_permit=0`) it pays the `402`s from the funding key instead.
- **Through the console**: given `funder=<address>`, the node signs only
  its half of the binding; the console's signer signs the rest and the
  permit.
- It writes names (the bytes once) and other content (manifests, snapshots)
  as signed bucket puts, commits the bucket statement with each anchor, and
  reports `covered_until` and the bucket's state in `hpplite_status`.
- `kubo://` and `s3://` stores: removed in v0.8 (#250); a store speaks this protocol.

## Removed

From `hpplite-store/1`, in hpplite (the node, operators/store, the
conformance suite; #213, #214) and in hpp-io/hpplite-operators (the store
Worker, operators#16):

- per-item retention, `/pin`, stacked expiries, the pin cap (operators#13);
  renewal by manifest (#202);
- unsigned paid `/put`, the per-item receipt (`Receipt`, version 1) and its
  checks in the node and verify;
- the first-signer rule and content-before-name (#199) for names;
- `batch-settlement` at the store: channels, deposits, vouchers, claims, the
  Ledger and its warnings; the node's `storedeposit=`; undo on a failed
  settle (#194) becomes "the credit taken back". The relay dropped its
  channels in v0.8 too (#251): it is paid per call by `exact`;
- version negotiation with /1, and migrating per-item history (the Sepolia
  store is reset, operators#12).

## Open

- Pull cadence and threshold for mainnet; whether parked storage costs less.
- Mirroring a bucket at two stores; moving one between stores.
- The relay charging the account the same way (its own permit).
