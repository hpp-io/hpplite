# HPPLite Architecture

**One writer, a chain for the history, a hash for the state**

## Overview

HPPLite is a SQLite extension that captures the pages each commit writes,
posts them to a chain (or a store, with the chain holding the commitment) in
encrypted batches, and anchors a root over the database's pages at a fixed
cadence. The shape is the one L2
rollups use - a single sequencer, data availability on L1, periodic state
commitments - borrowed because it is the simplest shape that gives a SQLite
application an ordered, durable, checkable history without changing how the
application talks to SQLite. The comparison stops there: there are no
funds, no fraud proofs and no challenge period in HPPLite.

```
┌─────────────────────────────────────────────────────────────┐
│                         HPPLite                             │
│                                                             │
│   App ──────► Sequencer (your process) ──► L1 contracts     │
│                      │                     - batch DA       │
│                      │                     - checkpoints    │
│                      ▼                     - state roots    │
│                   SQLite                                    │
│                                                             │
│   Rebuild and compare: open an empty file with the key      │
└─────────────────────────────────────────────────────────────┘
```

---

## Core Design

### Why Singleton Sequencer?

SQLite requires strong consistency - apps expect read-your-writes semantics:

```c
sqlite3_exec(db, "INSERT INTO users VALUES (1, 'alice')", ...);
sqlite3_exec(db, "SELECT * FROM users WHERE id=1", ...);
// App expects 'alice' immediately
```

Distributed writes break this. Solutions like multi-master (AergoLite) require append-only restrictions. HPPLite keeps full SQL by using a single sequencer.

The same choice every production L2 makes (Optimism, Arbitrum, Base, zkSync
all run one sequencer), for the same reason.

### What the chain gives you

| Layer | What It Provides |
|-------|------------------|
| **Data availability** | Every batch on-chain; a second machine with the key rebuilds the database from nothing else |
| **Checkpoints** | Roots over the database's pages anchored at a fixed cadence; a rebuilt database is compared against them |
| **Lease** | One posting instance at a time; a crashed instance's lease expires (an hour by default since VERSION 9, the owner's to set) |

What it does not give you in v0.1: public verifiability (batches are
encrypted, so replay needs the key - viewing keys are v0.2), a penalty for
a sequencer that diverges (nothing is at stake on-chain), or a verifier
that runs without you (checking is on demand, by the key holder).

---

## Trust model

The sequencer is the application's own process and is trusted to execute
SQL. The chain is a witness, not a judge:

```
Sequencer executes SQL; HPPLite reads each commit's pages from the WAL
    ↓
Batches posted in order under the lease; roots anchored every N batches
    ↓
Key holder rebuilds from the chain whenever they like, compares the roots
```

A sequencer that lost its disk recovers from the chain. A sequencer whose
file was tampered with produces a root that does not match the chain's and
the next rebuild says so. That is the whole guarantee; it is the one a
single-operator database can honestly make.

Rebuild and compare, today:

```bash
sqlite3> .load ./hpplite
sqlite3> .open 'file:rebuilt.db?hpplite=on&l1=hpp-sepolia&keyfile=/path/to/key'
sqlite3> SELECT height, state_root, roots_compared, root_mismatches FROM hpplite_status;
```

`hpplite-verify` (#5) packages this: a `role=verify` open into a temporary
file, the on-chain checkpoint root compared as well, and with `--db` the
live database compared, with per-table reporting.

---

## Architecture

### Sequencer Role

The sequencer is the single node that:
1. Receives SQL from clients
2. Executes against local SQLite
3. Batches transactions
4. Submits to L1 (data availability)
5. Creates checkpoints (state commitments)

```c
// Client connects directly to sequencer
sqlite3_open_v2(
    "file:myapp.db?hpplite=on&l1=hpp-sepolia&privkey=0x...",
    &db, ...);

// All reads and writes go to sequencer
sqlite3_exec(db, "INSERT INTO ...", ...);  // Batched, submitted to L1
sqlite3_exec(db, "SELECT ...", ...);        // Strong consistency
```

### Local Durability

The durable state is the application's database file, as SQLite keeps it,
and a sidecar beside it, `<db>-hpplite.db`. HPPLite writes nothing into the
database file. Nothing that matters lives only in memory, so a crash at any
point loses nothing and a restart needs no special recovery:

| Where | Holds | Written |
|-------|-------|---------|
| the database and its WAL | every commit, as SQLite wrote it | by the application, through SQLite |
| the sidecar's `_hpplite_meta` | next batch height, previous block hash, the page root after the last finalized batch, checkpoint progress, the chain hash, the batch in flight (height and root) | when a batch is submitted (in flight) and when it is finalized |
| the sidecar's `_hpplite_posted` | the posted batches, kept for a reorg | at finalize |
| the sidecar's `_hpplite_unanchored` | the batches at the store that no anchor names yet (#164) | at finalize, cleared at the anchor |
| the sidecar's `_hpplite_readers` | the viewing keys granted (#15) | on grant and revoke |

How a commit gets captured (#146, #155): after each commit SQLite runs the
connection's WAL hook, and HPPLite's hook has the WAL reader (`walread.c`)
read the commit's frames from `<db>-wal` - validated by the WAL's salts and
the running checksum - and queue the pages in memory (`pagemode.c`). The
sequencer's timer polls the reader too, with no frame hint, for frames
other connections and other processes wrote, so one sequencer captures
every writer of the file, HPPLite connection or not. A read transaction
the sequencer keeps pinned at or before the reader's cursor keeps SQLite
from backfilling past what was read and from restarting the WAL over a
frame the reader has not seen (`doc/sqlite-dependencies.md`, entry 12),
so a thousand commits and a checkpoint by a plain connection between two
ticks lose nothing. The queue is the one thing in memory, and it is
recoverable: the file holds every commit, the sidecar says which root the
chain has, and a sequencer that opens a file whose root is not the one its
sidecar recorded ships the file whole as its next batch.

Flushing a batch:

```
1. If L1 already holds a batch at our next height, it was built from our
   queue by an earlier attempt (crash after submit, lost RPC reply): the
   sidecar's record of the batch in flight says so; adopt it - advance meta.
2. Take the queued commits into a page batch: each page the commits
   touched, once, as its last image - whole the first time a replica sees
   the page, an XOR diff against the image the last finalized batch left
   after that - the file's size after, and the root after (one tree path
   per dirty page). Deflate, seal.
3. Submit to L1 (or put at the store) and wait.
4. Confirmed  -> advance height / prev hash / root, keep the posted copy,
   in one sidecar transaction. The images the diffs were taken against
   move on.
   Not confirmed -> keep the commits queued; retry next interval at the
   same height.
5. If an interval of batches is now complete, submit the checkpoint
   (previous root -> this root) to the coordinator contract; with a store,
   the anchor is the checkpoint (#164).
```

The DA contract caps a batch at `batchSizeLimit` (128 KB by default, read
from the contract's system config on connect); the chain may cap it lower
(HPP Sepolia's RPC takes ~84.9 KB of calldata per transaction; with the
storage DA of rollups before version 8, 32M gas per transaction meant ~44 KB;
HPPLite cuts at 40 KB there, `maxtx=`/`maxtxgas=` elsewhere, and shrinks
batches the RPC refuses as oversized). Since version 8 (#33) the DA contract
keeps the bytes in the transaction's log, not in storage: storage holds the
height, a rolling hash over every batch (`chainHash`) and the block each
batch's log is in (the chain's own block: on an Arbitrum-style chain such as
HPP, `block.number` is the parent chain's, so the contract asks
`ArbSys(0x64).arbBlockNumber()` where that precompile answers, #128); a
reader fetches one block's log by the height topic and
recomputes the rolling hash as it rebuilds (`_hpplite_meta.chain_hash`,
`hpplite_status.chain_hash`), and a mismatch at the tip stops the rebuild
in the error state. A batch is cut so that it fits; a commit that dirties
more pages than fit is posted as a series of pieces, each under the limit,
the root carried by the last, and a replica applies the series under one
lock so its readers never see half a commit (#152). The timer flushes early
once the queue reaches half the limit, so series are the exception; a
single page is the unit, so the limit must hold at least one deflated page.

The bookkeeping (steps 1, 4) runs on the sidecar's connection, so the
background flush never touches the application's transaction state. The
database is switched to WAL at open (the frames are what is captured;
leaving WAL is refused) and gets a 5 s busy timeout. The WAL hook is the
capture's, so `PRAGMA wal_autocheckpoint` is refused on a HPPLite
connection, and where SQLite's own hook would checkpoint at 1000 frames
the sequencer's WAL upkeep runs instead (the pin moved on, the WAL
polled, a passive checkpoint on a plain connection of the node's own, once
a second at most): the file's checkpointing is HPPLite's, under the pin,
never on the application's commit (#144, #155). The one other hook on the
application's connection is a trace that recognises a `VACUUM` (the
authorizer lets its unnamed `ATTACH` through) and polls the WAL at the end
of a statement whose commit ran without the WAL hook (`RETURNING` stepped
once and reset; entry 11).

Several connections, one WAL. Within a process the first `hpplite=on`
connection to a file is the sequencer; later ones detect it (canonical path
in the connection registry) and become followers, with no sidecar
connection, L1, lease or timer. Across processes the sequencer is whoever
holds an `flock` on `<file>-hpplite`; every connection holds
`<file>-hpplite-users` shared, so a closing sequencer knows whether someone
is left to take over. A follower whose sequencer is gone takes the lock,
reloads the sidecar as the sequencer left it and goes on as the same
instance to L1. A plain connection (no `hpplite=on`) is captured like any
other while a sequencer runs; it only lacks the authorizer.

Reopening a file continues at the recorded height. If L1 is ahead (a fresh
file, or history written elsewhere), the missing batches are applied as
pages, under an exclusive lock and with the capture paused. If the file is
ahead of L1, it is shipped whole. A replica whose file is not at the root
its sidecar recorded (the file restored, the sidecar not) refuses the next
batch and says so rather than apply diffs onto the wrong pages.

Close is handled without touching SQLite core: `hpplite_status` is an
eponymous virtual table whose instance lives for the connection; SQLite
disconnects it at the start of `sqlite3_close()` while the connection is
still usable, and that `xDisconnect` flushes, releases the lease and frees
the node. The same code builds as a loadable extension against stock
SQLite.

The lease is taken at open and read from L1 before every submission: the
node renews its own once past half its life, claims a free one, and never
posts into a sibling instance's (another process of the same wallet: the
batch stays pending and is retried after expiry - there is no takeover
between instances of one wallet). A claim is a transaction; it counts only
once the receipt is in and `getState()` names our instance. Failed
submissions back off exponentially, up to the lease length. On graceful
close the lease is released (`releaseLease` by the instance itself). After
a crash the old lease blocks submission until it expires; writes keep
accumulating in the file and are posted once the lease is free.

Only the wallet the chain names as sequencer can claim the lease (VERSION
5 and later; earlier factories are refused at connect since v0.8, #252), so
a lease held by another wallet is a rotated sequencer's (#208): the node
says so and waits. A batch posted by another wallet is stepped over: it
fails our authentication tag, so it is provably not ours, every key holder
skips that height identically, and its raw bytes are hashed into the chain
of block hashes (`hpplite_status.foreign_batches`). A tag failure at height
1 with no history is the wrong key, never an intruder, and is an error.

### Determinism

There is no determinism question. A replica does not re-execute anything:
it writes the pages the sequencer's commits wrote, so `random()`, the
clock, the machine's time zone, functions the application registered,
virtual tables, triggers and foreign-key cascades, `PRAGMA` connection
state and incremental blob writes all replicate exactly, and nothing is
refused for replay's sake. (v0.1-v0.7 replayed statement text or row
records and had to refuse or record what replay could not reproduce;
that machinery went with them, #253.)

### Keys

v0.1 uses a single wallet key for four roles: contract **owner**, **sequencer**
(the hot key that claims the lease and posts), the derived batch **encryption**
key, and **identity** (the factory maps one wallet to one rollup). It is
provided before open via key file (`0600` enforced), environment or URI
(development only). A node without a key captures locally and never posts.

That is the single-key shortcut every L2 avoids in production: the key that
must be online (poster) should not be the key that can change the rules
(owner), and hot-key compromise should be survivable by rotation. The
contract already separates `owner` from `sequencerWallet`. v0.2 (#17, #15)
introduces the split, with the TDE two-tier structure for data: a random
data key per rollup wrapped to each reader's public key (owner, sequencer,
and **viewing keys** that can read but not sign), runtime key injection
through SQL functions as SQLite encryption extensions do, Ethereum keystore
V3 for human-held keys, and a signer callback so the sequencer key can live
in a KMS.

### Privacy: encrypted batches

L1 is public, so a batch is sealed before it is posted whenever the node has
a private key:

```
"HPE2" | scheme id | encoding | nonce (12) | ChaCha20-Poly1305(key, nonce, ad = scheme || encoding || height, batch JSON) || tag (16)
key = HKDF-SHA256(ikm = wallet private key, salt = "hpplite", info = "hpplite/batch-encryption/v1")
```

- The key is derived, not stored: whoever can write the rollup can read it,
  and reconstruction/verification need only the same `privkey=`.
- The height is associated data, so a ciphertext cannot be presented at
  another height. Each batch gets a fresh random nonce.
- Payloads that do not start with `HPE` are plaintext (history written
  before encryption, or `encrypt=off`); a node reads both, writes `HPE2`
  (or `HPE3`, the public-key envelope, once readers are granted, #15).
- The scheme is pluggable (`HppliteCipher` in `cipher.h`); the envelope
  names it, so new schemes can be added without touching batch or replay
  code. The v0.1 scheme is vendored ChaCha20-Poly1305 (RFC 8439): ~400
  lines of constant-time plain C, no platform crypto dependency, which
  matters for the loadable extension.
- The DA size limit applies to the posted bytes, so the batch cut leaves
  room for the 33-byte envelope.

What remains visible on L1: heights, sizes, timing, batch hashes, and the
checkpoint roots (content hashes: they show that content changed, not what).
Consequence for verification: only key holders can replay. Per-reader
access without the wallet key (encrypt to public keys, grant/revoke) is the
v0.2 public-key scheme.

### State Root

The state root is a **page root** (`root_version` 4, `pages.c`): SHA-256
over the page size, the page count and the root of a Merkle tree whose
leaves are `SHA-256("HPP4" || pgno || page)` for pages 1..n of the file.
Page 1's file-format bytes and change counters are left out, so a WAL-mode
sequencer and a rollback-mode replica hash to one root. The tree lives in
memory (about 2n nodes of 32 bytes), a dirty page costs one path, and a
node computes it from the file when it opens; so every batch carries the
root after it in its trailer, a replica checks it after every batch, and
the checkpoint (or the anchor, #164) commits the one at its height on L1.

It is a function of the file's bytes, not of the history that produced
them: a replica that applied the same batches has the same pages and the
same root. It is not a function of the content alone - the same rows laid
out differently (a `VACUUM`, another page size) give another root - which
is why a replica applies pages rather than re-executing anything. The
**content hash** (`hpplite_db_hash()`, `state_hash.c`, version 1: the
schema entries and every row of every user table in rowid order, typed
values, `sqlite_*` and `_hpplite_*` tables excluded) is that other
function, O(database size); `hpplite-verify --db` uses it to compare the
live database with the rebuilt one, two files whose pages differ while
their rows must not, and prints it as the rebuilt state root.

### L1 Contract

The L1 contract stores:
- **Batch data** - full DA, enables reconstruction
- **Checkpoints** - state root commitments
- **Configuration** - sequencer address, parameters

```solidity
interface IHPPLite {                       // coordinator, VERSION 10
    function claimSequencer(bytes32 instanceId) external;
    function renewLease(bytes32 instanceId) external;
    function submitCheckpoint(bytes32 instanceId, uint256 from, uint256 to, bytes32 root) external;
    function claimSequencerFor(bytes32 instanceId, uint256 nonce, uint256 deadline, bytes sig) external;   // the sequencer's EIP-712 signature, anyone's gas (#126, #171)
}
interface IHPPLiteDA {                     // store, version 3 (#164)
    function submitBatch(bytes32 instanceId, uint256 height, bytes data) external;        // bytes in the log, hash in chainHash
    function commitBatch(bytes32 instanceId, uint256 height, bytes32 hash, bytes ref, bytes receipt) external;   // bytes elsewhere (#11)
    function commitRange(bytes32 instanceId, uint256 from, uint256 to, bytes32 chainHashAfter, bytes32 root, bytes32 manifestDigest, bytes receipt) external;   // the anchor (#164)
    function chainHash() external view returns (bytes32);
    function lastBlock() external view returns (uint256);
}
```

### Verification

A key holder checks the whole history by rebuilding it:

```
1. Fetch all batches from L1 (or the store the chain's commitments name)
   and open them (the key)
2. Apply each batch's pages in order (a series of pieces under one lock,
   #152), under SQLite's exclusive lock, in rollback-journal mode
3. After each batch, compute the page root of the file
4. Compare against the root in the batch's trailer, and at a checkpoint
   against the posted root; report any mismatch and stop
```

Opening an empty file with `hpplite=on` and the key does all four and
reports on stderr and in `hpplite_status.roots_compared` / `root_mismatches`; `hpplite-verify` (#5) packages it as a command with
the on-chain checkpoint compared and per-table reporting. It is an on-demand audit, not a running node: `role=verify` never
posts, claims or creates.

---

## Comparison with Other Systems

| System | Writers | History | Check |
|--------|---------|---------|-------|
| **HPPLite** | One process | Encrypted page batches at a store and on-chain + page roots anchored | Rebuild with the key, compare roots |
| **Litestream** | One process | WAL pages to S3 | Restore; no content commitment |
| **cr-sqlite** | Many (CRDT) | Per-row versions | Convergence, not verification |
| **AergoLite** | Many (multi-master) | Blocks with VRF consensus | Consensus |
| **Optimism / Arbitrum** | One sequencer | Calldata/blobs on L1 | Fraud proofs, challenge period |

Closest in spirit to Litestream and LiteFS - the log is the pages each
commit wrote, read from the WAL - with a root over the pages committed
on-chain, and encryption as the default.

---

## Why Not Witnesses?

We considered active witness nodes that:
- Sync from sequencer in real-time
- Attest to checkpoints
- Serve read traffic

**Problems:**
1. **Write routing** - if witness receives write, must forward to sequencer (complex)
2. **Consistency** - reads from witness may be stale (breaks SQLite semantics)
3. **Infrastructure** - requires 24/7 witness nodes

**Solution:** Skip client-facing witnesses. Verification is on-demand, not continuous.

Future optimization: read replicas (explicit opt-in for stale reads) if needed for scale.

---

## Where a write is

```
┌──────────────┬─────────────────────────────────────────────────────┐
│ Stage        │ Meaning                                             │
├──────────────┼─────────────────────────────────────────────────────┤
│ COMMITTED    │ In the local file and its WAL (SQLite's ACID)       │
│ ON L1        │ In a confirmed batch; survives losing the disk      │
│ CHECKPOINTED │ Covered by an anchored state root; checkable        │
└──────────────┴─────────────────────────────────────────────────────┘
```

Typical latencies: COMMITTED immediately; ON L1 within the batch interval
(5 s default) plus L1 confirmation; CHECKPOINTED after `checkpoint`
batches (100 default).

The step from COMMITTED to ON L1 holds because the WAL hook tells HPPLite
of each commit (and the statement's end covers the commits it does not),
and because a pinned read transaction keeps every frame in the WAL until
it has been read. Those are SQLite behaviours outside its documented API;
[`sqlite-dependencies.md`](sqlite-dependencies.md) lists each one with its
SQLite source location, the HPPLite code that relies on it and the test
that fails if it changes, and the `sqlite-trunk` CI job builds against
upstream `master` so a change there is seen before a release moves to it.

---

## Configuration

| Parameter | Description | Default |
|-----------|-------------|---------|
| `interval` | Time between batch submissions | 5s |
| `checkpoint` | Batches between checkpoints / state roots | 100 |
| `batchSizeLimit` | Largest batch the DA contract accepts (read from the contract) | 128 KB |
| lease | Sequencer lease length (the owner's to set since VERSION 9) | 1 h |

---

## Summary

1. **One writer** - strong consistency, full SQL, nothing changes for the app
2. **The chain holds the history** - encrypted page batches, at a store within seconds and anchored on the chain, reconstructable with the key
3. **A hash holds the state** - a root over the file's pages after every batch, anchored at a fixed cadence, compared on rebuild
4. **On demand** - no node to run; open an empty file when you want to check

```
"The pages your database wrote, encrypted, at a store and on-chain; rebuild and compare when you like"
```
