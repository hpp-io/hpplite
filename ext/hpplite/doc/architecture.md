# HPPLite Architecture

**One writer, a chain for the history, a hash for the state**

## Overview

HPPLite is a SQLite extension that keeps a statement-level write log inside
the database file, posts it to a chain in encrypted batches, and anchors a
content hash of the database at a fixed cadence. The shape is the one L2
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
| **Checkpoints** | Content hashes anchored at a fixed cadence; a rebuilt database is compared against them |
| **Lease** | One posting instance at a time; a crashed instance's lease expires in 5 minutes |

What it does not give you in v0.1: public verifiability (batches are
encrypted, so replay needs the key - viewing keys are v0.2), a penalty for
a sequencer that diverges (nothing is at stake on-chain), or a verifier
that runs without you (checking is on demand, by the key holder).

---

## Trust model

The sequencer is the application's own process and is trusted to execute
SQL. The chain is a witness, not a judge:

```
Sequencer executes SQL, logs it in the same transaction
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
sqlite3> SELECT height, state_root, replay_skipped FROM hpplite_status;
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

The local SQLite file is the only durable state HPPLite keeps. Nothing
lives only in memory, so a crash at any point loses nothing and a restart
needs no special recovery:

| Table | Holds | Written |
|-------|-------|---------|
| `_hpplite_pending` | SQL of every committed write, in order, grouped by transaction | From inside the user's own statement, so the row commits or rolls back **with** the user's transaction |
| `_hpplite_meta` | Next batch height, previous block hash, state root after the last confirmed batch, checkpoint progress | When a batch is confirmed on L1 |

How a statement gets logged: SQLite's statement trace fires at the start of
every statement on the connection. HPPLite inserts the statement text (with
bound parameters expanded) into `_hpplite_pending` through the same
connection. Because the insert runs inside the statement's own transaction
(implicit or explicit), a `ROLLBACK` discards the log row too, and in
autocommit so does a failed statement. No separate journal, no sync
interval, no durability model beyond SQLite's own.

Inside an explicit transaction a failed statement only undoes its own
changes, so when a logged statement completes HPPLite checks whether it had
any effect (rows changed or schema changed) and deletes the log row if not,
before the transaction commits: failed statements are never replayed. (In
autocommit a failed statement's rollback takes its row along. A no-op in
autocommit keeps its row: it committed with the statement and may already be
in a batch another connection is building, and removing it then would leave
that batch holding a row the log no longer has. A statement that changed
nothing on the writer changes nothing on a replica with the same state.)
SQLite reports a
statement's error only after the trace sees it finish, so effect is the
signal; its one blind spot is a statement whose triggers changed rows
before it aborted. Replay therefore skips any statement that fails
(`hpplite_status.replay_skipped`), which reproduces what happened at the
sequencer, and the checkpoint root remains the arbiter of equivalence.

Flushing a batch:

```
1. If L1 already holds a batch at our next height, it was produced from our
   pending rows by an earlier attempt (crash after submit, lost RPC reply):
   adopt it - delete those rows, advance meta.
2. Read pending rows in one snapshot; if this batch closes a checkpoint
   interval, compute the state root in the same snapshot.
3. Submit to L1 and wait for the transaction.
4. Confirmed  -> delete the rows, advance height / prev hash (and the root,
   when the batch carries one) in one transaction.
   Not confirmed -> leave everything; retry next interval at the same height.
5. If an interval of batches is now complete, submit the checkpoint
   (previous root -> this root) to the coordinator contract.
```

The DA contract caps a batch at `batchSizeLimit` (128 KB by default, read
from the contract's system config on connect); the chain may cap it lower
(HPP Sepolia's RPC takes ~84.9 KB of calldata per transaction; with the
storage DA of rollups before version 8, 32M gas per transaction meant ~44 KB;
HPPLite cuts at 40 KB there, `maxtx=`/`maxtxgas=` elsewhere, and shrinks
batches the RPC refuses as oversized). Since version 8 (#33) the DA contract
keeps the bytes in the transaction's log, not in storage: storage holds the
height, a rolling hash over every batch (`chainHash`) and the block each
batch's log is in; a reader fetches one block's log by the height topic and
recomputes the rolling hash as it rebuilds (`_hpplite_meta.chain_hash`,
`hpplite_status.chain_hash`), and a mismatch at the tip stops the rebuild
in the error state. Pending SQL beyond that is
submitted as a series of batches, each cut at a transaction boundary when
possible and marked `"partial": 1` except the last. Only the last batch of
a series carries a state root, so verification compares roots at non-partial
batches only. The timer flushes early once pending SQL reaches half the
limit, so series are the exception. A single statement that could not fit
on its own is refused when it runs (see below) rather than left to block
every later batch.

Capture is fail-closed. If the log row for a statement cannot be written
(disk full), if the statement text is longer than one DA batch can carry,
or if its bound parameters cannot be expanded, the statement itself is
refused: HPPLite calls `sqlite3_interrupt()` from the trace callback and the
statement fails with `SQLITE_INTERRUPT` before it changes anything (inside
an explicit transaction SQLite rolls the transaction back). The count is in
`hpplite_status.log_errors` and the reason on stderr. The alternative -
apply the write and lose the log row - is the one thing the design must
never do.

Bookkeeping (steps 1, 2, 4) runs on a second connection to the same file,
so the background flush never touches the application's transaction state.
File databases are switched to WAL and both connections get a 5 s busy
timeout; readers and the writer then never block each other. Opening a
file whose bookkeeping tables exist and are current reads only, so a
connection opened while another is inside a write transaction (one
connection per request) starts at once; the first open of a file, and the
upgrade of an older file, create or alter the tables and take the write
lock (#123).

The hooks are one slot per connection, so another library installing its
own trace or commit hook would silently end capture. Every HPPLite entry
point that runs on the application thread (a `hpplite_status` read,
`hpplite_flush()`, `hpplite_sync()`, close) re-installs ours and counts a
replacement in `hpplite_status.hooks_replaced` - writes made in between are
not captured and the next checkpoint root says so; a chaining API for apps
that need their own trace is v0.2.

Several connections, one log. A transaction is named by the rowid of its
first logged row (`txn = 0` in the row means "my own id"), so transaction
groups are unique across every connection and process that writes the
file - there is no counter to keep in step. Within a process the first
`hpplite=on` connection to a file is the sequencer; later ones detect it
(canonical path in the connection registry) and become followers: hooks
installed, same `_hpplite_pending`, no bookkeeping connection, L1, lease
or timer. The sequencer's flush posts everyone's rows. Across processes
the lease does the same job: both capture, the lease holder posts. A plain
connection (no `hpplite=on`) is not captured, so every writer must open
with it.

Reopening a file continues at the recorded height. If L1 is ahead (a fresh
file, or history written elsewhere), the missing batches are replayed with
tracking disabled and are not logged again. If local pending SQL and the
batch on L1 disagree, the node refuses to continue rather than fork.

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
close the lease is released (`releaseLease` by the instance itself on
VERSION 5 rollups, `forceExpireLease` by the owner on older ones). After a
crash the old lease blocks submission until it expires (5 minutes); writes
keep accumulating in `_hpplite_pending` and flush once the lease is free.

Only the owner-set sequencer wallet can claim a VERSION 5 rollup's lease.
Rollups created by the previous factory still allow any wallet to claim an
expired lease; for them the node, as owner, expires a stranger's lease and
reclaims, and a batch a stranger managed to post is stepped over: it fails
our authentication tag, so it is provably not ours, every key holder skips
that height identically, and its raw bytes are hashed into the chain of
block hashes (`hpplite_status.foreign_batches`). A tag failure at height 1
with no history is the wrong key, never an intruder, and is an error.

### Determinism

Replay re-executes captured SQL text. That is only sound when a statement's
effect is a function of the database and the statement - plus whatever
state is recorded with it and reapplied on replay:

- connection state that changes what a statement does to other rows
  (`foreign_keys`, `recursive_triggers`) is recorded per statement (#52);
- the clock (`datetime('now')`, `CURRENT_TIMESTAMP`, column defaults) is
  recorded per statement that read it and pinned on replay through a
  pass-through VFS (`clock.c`, #56).

What cannot be recorded - `random()`, `randomblob()`, `last_insert_rowid()`
and friends, the machine's time zone (`'localtime'`), functions the
application registered (a replica does not have them, #53), `ATTACH` - v0.1
**refuses** in writes (`SQLITE_INTERRUPT`; `determinism=warn` to only count
the non-deterministic ones): the application learns at the statement, not
from a root mismatch later. Reads are never refused (#54). The convention is to compute such values in the application
and bind them; `sqlite3_expanded_sql()` writes the bound literal into the
captured text. The structural fix is v0.2's row-level capture (#19): log
the row changes the preupdate hook reports instead of the text, and replay
becomes deterministic by construction.

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
- Payloads that do not start with `HPE` are plaintext JSON (history written
  before encryption, or `encrypt=off`); a node reads both (and the earlier
  `HPE1` envelope without the encoding byte), writes `HPE2`.
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

The state root is a **content hash** of all user tables: SHA-256 over the
schema entries (type, name, table, SQL) and every row of every table in
rowid order, with typed values. `sqlite_*` and `_hpplite_*` tables are
excluded; a database with no user schema has the zero root.

It is a function of the file, not of the history that produced it. Two
databases with the same content have the same root whatever statements
built them, and anyone holding a reconstructed database can recompute it
(`hpplite_db_hash()` in C, `SELECT state_root FROM hpplite_status` in
SQL) and compare with what the sequencer posted.

Computing it is O(database size) (~9 ms/MB in a Release build), so it is computed at
**checkpoint cadence**, not per batch: only the batch that closes a
checkpoint interval (`checkpointInterval`, default 100) carries
`"state_root"`, and the checkpoint submitted right after it anchors that
root on L1. Other batches are data only. This is the same split every L2
makes - data is posted often, state is committed rarely - and it does not
touch durability (every COMMIT is durable locally, every batch is on L1);
it only sets how finely a verifier can localize a divergence. Deployments
that want finer auditing lower `checkpointInterval` and pay proportionally.

### L1 Contract

The L1 contract stores:
- **Batch data** - full DA, enables reconstruction
- **Checkpoints** - state root commitments
- **Configuration** - sequencer address, parameters

```solidity
interface IHPPLite {                       // coordinator, VERSION 8
    function claimSequencer(bytes32 instanceId) external;
    function renewLease(bytes32 instanceId) external;
    function submitCheckpoint(bytes32 instanceId, uint256 from, uint256 to, bytes32 root) external;
    function claimSequencerFor(bytes32 instanceId, uint256 nonce, bytes sig) external;   // the sequencer's EIP-712 signature, anyone's gas (#126)
}
interface IHPPLiteDA {                     // store, version 2 (#33)
    function submitBatch(bytes32 instanceId, uint256 height, bytes data) external;        // bytes in the log, hash in chainHash
    function commitBatch(bytes32 instanceId, uint256 height, bytes32 hash, bytes ref, bytes receipt) external;   // bytes elsewhere (#11)
    function chainHash() external view returns (bytes32);
    function blockOf(uint256 height) external view returns (uint64);
}
```

### Verification

A key holder checks the whole history by rebuilding it:

```
1. Fetch all batches from L1 and open them (the key)
2. Replay SQL in order (each batch's transaction groups as transactions; a
   group a batch leaves open is held until the batch that ends it, #57)
3. After each batch that carries a root, compute the content hash
4. Compare against the posted root; report any mismatch
```

Opening an empty file with `hpplite=on` and the key does all four and
reports on stderr and in `hpplite_status.roots_compared` / `root_mismatches`; `hpplite-verify` (#5) packages it as a command with
the on-chain checkpoint compared and per-table reporting. It is an on-demand audit, not a running node: `role=verify` never
posts, claims or creates.

---

## Comparison with Other Systems

| System | Writers | History | Check |
|--------|---------|---------|-------|
| **HPPLite** | One process | Encrypted SQL batches on-chain + content-hash roots | Rebuild with the key, compare roots |
| **Litestream** | One process | WAL pages to S3 | Restore; no content commitment |
| **cr-sqlite** | Many (CRDT) | Per-row versions | Convergence, not verification |
| **AergoLite** | Many (multi-master) | Blocks with VRF consensus | Consensus |
| **Optimism / Arbitrum** | One sequencer | Calldata/blobs on L1 | Fraud proofs, challenge period |

Closest in spirit to Litestream, with the log at the statement level, a
content hash committed on-chain, and encryption as the default.

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
│ COMMITTED    │ In the local file, with its log row (SQLite's ACID) │
│ ON L1        │ In a confirmed batch; survives losing the disk      │
│ CHECKPOINTED │ Covered by an anchored state root; checkable        │
└──────────────┴─────────────────────────────────────────────────────┘
```

Typical latencies: COMMITTED immediately; ON L1 within the batch interval
(5 s default) plus L1 confirmation; CHECKPOINTED after `checkpoint`
batches (100 default).

The COMMITTED stage holds because the log row is written from the trace
callback of the application's statement and joins its transaction, and
because a statement SQLite reruns after a foreign schema change is caught
without a second trace. Those are SQLite behaviours outside its documented
API; [`sqlite-dependencies.md`](sqlite-dependencies.md) lists each one with
its SQLite source location, the HPPLite code that relies on it and the
test that fails if it changes, and the `sqlite-trunk` CI job builds against
upstream `master` so a change there is seen before a release moves to it.
Its first run found one (SQLite 3.55 defers the schema reset while a
statement runs), and the capture now has a path for a stale, pinned schema:
the capture of such a statement is done at its first run's end, when the
statement's own schema check has reset the schema (#108, entry 10 there).

---

## Configuration

| Parameter | Description | Default |
|-----------|-------------|---------|
| `interval` | Time between batch submissions | 5s |
| `checkpoint` | Batches between checkpoints / state roots | 100 |
| `batchSizeLimit` | Largest batch the DA contract accepts (read from the contract) | 128 KB |
| lease | Sequencer lease length (contract constant) | 5 min |

---

## Summary

1. **One writer** - strong consistency, full SQL, nothing changes for the app
2. **The chain holds the history** - encrypted batches, reconstructable with the key
3. **A hash holds the state** - content roots at a fixed cadence, compared on rebuild
4. **On demand** - no node to run; open an empty file when you want to check

```
"Your write log on-chain, encrypted; rebuild and compare when you like"
```
