# HPPLite

**A crash-safe, encrypted, replayable write log for SQLite, mirrored to a chain.**

HPPLite is a SQLite extension. It captures every write statement inside the
transaction that runs it, cuts the log into batches, seals them with a key
derived from your wallet key and posts them to an Ethereum-compatible chain
for data availability. Every N batches it anchors a SHA-256 of the whole
database (the *state root*) on the same chain. Open an empty file with the
same key and the database rebuilds itself from the chain; replay compares
every posted root with what it computes and says so when they differ.

Stock SQLite, loadable extension, no patches. One sequencer posts; the chain
keeps the history; whoever holds the key can rebuild and check it.

What v0.1 is **not**: not a multi-writer database, not a public verifier
(batches are encrypted, so only key holders can replay; viewing keys are
v0.2), not cheap DA (every batch is posted on a testnet chain; since #33 as
log data rather than contract storage, eight times cheaper, but still on
chain), and not for databases much beyond 100 MB. See [Versions](#versions).

## Quick Start

```c
// Just use SQLite with a special URI - that's it!
sqlite3 *db;
sqlite3_open_v2(
    "file:myapp.db?hpplite=on&l1=hpp-sepolia"
    "&privkey=0xYOUR_PRIVATE_KEY",
    &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);

// Use SQLite normally - batches auto-submit to L1
sqlite3_exec(db, "CREATE TABLE users(id INT, name TEXT)", NULL, NULL, NULL);
sqlite3_exec(db, "INSERT INTO users VALUES(1, 'alice')", NULL, NULL, NULL);

// Close - state persists on L1
sqlite3_close(db);
```

`l1=hpp-sepolia` selects the RPC endpoint and factory contract for HPP Sepolia (see Contracts). On first open, the factory auto-deploys your rollup contracts (~3s). All writes are batched and submitted to L1 for data availability.

The same works from the stock `sqlite3` shell after `.load ./hpplite`, and
from any language whose SQLite binding can load extensions.

## How it works

```
┌──────────────────────────────────────────────────────────────────────┐
│  your process                                                        │
│                                                                      │
│  app ──► SQLite ──► _hpplite_pending (row records, inside your txn)  │
│                           │                                          │
│            timer / close  ▼                                          │
│                     batch = JSON ──► ChaCha20-Poly1305 ──► L1 DA     │
│                     every N batches: SHA-256(database) ──► L1 root   │
└──────────────────────────────────────────────────────────────────────┘
      rebuild: open empty file + same key ──► fetch, open, replay, compare roots
```

- **Transparent** - `sqlite3_open_v2()` with URI parameters, or `.load`
- **Stock SQLite** - no core patches; loadable extension against any SQLite >= 3.24
- **Nothing to lose** - the log row is written inside your own transaction;
  it commits and rolls back with your statement. A crash or a failed
  submission never loses or duplicates a batch. A statement that *cannot*
  be logged is refused (`SQLITE_INTERRUPT`) rather than applied unlogged
- **Encrypted by default** - batches are sealed before they leave the
  process with a key derived from your wallet key; the chain sees ciphertext
- **Factory deploy** - one wallet, one rollup, created on first open; a
  named database (`dbname=`) gets its own rollup under the same wallet
- **Rebuild and check** - an empty file with the same key reconstructs from
  the chain and compares every posted state root along the way

## Why one writer?

SQLite apps expect read-your-writes. Distributed writes would need write
forwarding, eventual consistency or append-only restrictions; HPPLite keeps
full SQL by having one sequencer process per database. The chain is the
history, not a consensus layer: it makes the log durable, ordered and
checkable, and it is what a second machine rebuilds from.

## What is HPP?

HPP is the Ethereum-compatible network HPPLite is built for; its Sepolia
testnet is chain id **181228**, RPC `https://sepolia.hpp.io`, explorer
`https://sepolia-explorer.hpp.io`. The `hpp-sepolia` alias bakes those in
together with the factory below. Any EVM chain works with `rpc=` plus
`factory=` (or `contract=`) once the contracts in `contracts/` are deployed
there. You need a funded wallet for gas; the integration runner derives a
fresh test wallet from the key in `.env` for each run.

## Contracts

Deployed on **HPP Sepolia** (Chain ID: 181228, RPC: `https://sepolia.hpp.io`):

| Contract | Address | Description |
|----------|---------|-------------|
| HPPLiteFactory | `0xc0fa50eb3076ed0e1ce73639097e40784950b8d8` | Creates rollups (one per wallet, plus one per named database); built into the `hpp-sepolia` alias. Contract VERSION 7 (a checkpoint cannot reach past the DA's height, #44; sequencer-only lease claim; rotating the sequencer ends its lease); rollups from earlier factories keep working with the rules they were created with - give their factory with `factory=` (`0xd1aaa8d1…`: VERSION 6, `0x9c6abd03…`: VERSION 5, `0x9cfacba5…`: permissionless claim) |
| HPPLite | *(per rollup)* | Your rollup's coordination contract: sequencer lease (only the owner-set sequencer wallet can claim it), checkpoints |
| HPPLiteDA | *(per rollup)* | Your rollup's data-availability contract: batch data |

The factory address lives in one place in code (`da_uri.c`, `HPPLITE_NETWORKS`); `ctest` fails if this README disagrees with it.

## URI Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `hpplite` | Yes | Set to `on` to enable |
| `l1` | Yes* | Network alias (`hpp-sepolia`): sets `rpc` and `factory`. `file:///path`: a directory instead of a chain - lease, checkpoints and batches as files, no wallet needed (#35, see Stores and commitments). `local`: no chain, batches are finalized in the file and the history is kept nowhere (#38) |
| `rpc` | Yes* | L1 RPC URL (if no `l1`) |
| `factory` | Yes* | Factory contract address (if no `l1`) |
| `contract` | No | Connect to an existing rollup instead of using the factory |
| `privkey` | Yes | Private key for signing (or `keyfile=/path`; see Keys) |
| `chainid` | With `rpc` | Chain id of the RPC (set by `l1`) |
| `role` | No | `verify`: an audit connection - rebuilds and compares, never posts, claims the lease or creates a rollup (what `hpplite-verify` uses, #5). Other values are accepted and unused |
| `datadir` | No | Accepted and unused: HPPLite keeps all its state in the database file itself (`_hpplite_*` tables) |
| `interval` | No | Batch flush interval: `500ms`, `5s` (default), `1m`, `1h`; `0` = no timer (flush on `hpplite_flush()` or close) |
| `encrypt` | No | `on` (default when a key is present) or `off` |
| `checkpoint` | No | Batches per checkpoint, the batch that also carries a state root. Not given: the rollup contract's `checkpointInterval` (100 unless changed). Given: written to the contract by its owner at connect, so the two agree (#44). A history shorter than that has no root on chain to verify against yet (see Verification) |
| `determinism` | No | `strict` (default): refuse SQL that replay cannot reproduce; `warn`: warn and count |
| `capture` | No | `rows` (default): log what each write changed (#19, below); `text`: log each write statement, as v0.1. Also `HPPLITE_CAPTURE` |
| `dbname` | No | Give the database its own rollup among the wallet's, found by this name (#61): several databases on one key. Names are compared exactly and must be unique per wallet: two files with the same name on one key are one database on chain (the second, if empty, rebuilds the first's data into itself). `{file}` = the file's base name without its directory (`a/x.db` and `b/x.db` are both `x.db`; `:memory:` is `memory`), so one `HPPLITE_PARAMS` serves every file `HPPLITE_FILES` names. A name no rollup has yet creates one (~2.5M gas): a misspelt name on a replica is a new, empty database, and the message says which names the wallet has. Also `HPPLITE_DBNAME` |

*Either `l1`, or both `rpc` and `factory` (or `rpc` and `contract`). The same settings can come from environment variables — see `.env.example`.

## Building

HPPLite is an ordinary SQLite extension: **no patches to SQLite, no special
compile options**, any amalgamation from 3.24 on. Two ways to use it.

**As a loadable extension** (`hpplite.dylib` / `hpplite.so`) against the
SQLite you already have:

```bash
brew install secp256k1 curl pkg-config  # macOS
# apt install libsecp256k1-dev libcurl4-openssl-dev  # Linux

cmake -S ext/hpplite -B build -DHPPLITE_USE_REAL_L1=ON \
      -DHPPLITE_SQLITE_SRC=/path/to/sqlite-amalgamation   # dir with sqlite3.c/.h/ext.h
cmake --build build
```

```
sqlite> .load ./build/hpplite
sqlite> .open 'file:app.db?hpplite=on&l1=hpp-sepolia&privkey=0x...'
sqlite> CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT);
sqlite> INSERT INTO users VALUES(1,'alice');
sqlite> SELECT * FROM hpplite_status;      -- height, pending, state_root, ..., contract, last_batch_tx
sqlite> SELECT hpplite_flush();            -- or just wait for the interval / close
```

Programs call `sqlite3_load_extension()` once; every later connection in the
process that opens with `?hpplite=on` is handled automatically.

**Drivers.** HPPLite is turned on per connection, by the URI
(`file:app.db?hpplite=on&...`) or - for code that opens a plain path - by
the environment: `HPPLITE_FILES` names the files (`:`-separated globs,
matched against the full path and the file name) and `HPPLITE_PARAMS`
carries what the URI would (`l1=hpp-sepolia&interval=5s`). `HPPLITE_PARAMS`
applies to URI opens too, under the URI's own parameters. Load the module
once per process before opening. Checked by `tests/orm/run_orm.sh`:

| Driver / ORM | How | Status |
|---|---|---|
| Python `sqlite3`: SQLAlchemy, Alembic, Django | `sqlite3.connect(uri, uri=True)`; Django `NAME` = the URI | works |
| Node `node:sqlite`: Drizzle (`sqlite-proxy`) | URI, or a plain path with `HPPLITE_FILES` | works |
| npm `sqlite3`: Knex | `connection: { filename: uri, flags: ['OPEN_URI', 'OPEN_READWRITE', 'OPEN_CREATE'] }` | works |
| better-sqlite3 (Drizzle's and Knex's default) | a plain path with `HPPLITE_FILES` (`tests/orm/test_better_sqlite3.js`) | works with row capture (the default). Its bundled SQLite is built with `SQLITE_TRACE_SIZE_LIMIT=32`, which cuts bound strings in `sqlite3_expanded_sql()`: `capture=text` is refused at open, and a statement still logged as text (a schema change, a write reaching a virtual table) is refused when it binds values |
| Prisma | - | not supported: its engine has its own SQLite and loads no extensions |
| Ruby `sqlite3` gem: Rails 8 (Solid Queue, Solid Cache) | `extensions: [<module>]` in `database.yml`, `HPPLITE_FILES` for the files and `dbname={file}` in `HPPLITE_PARAMS` (`tests/real_apps/rails`) | works (macOS, forking Puma: `OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES`) |

One SQLite library per process: loading the module into a second one
(better-sqlite3's copy and Node's own) is refused.

### Using it from an application

Load the module once per process, through any connection (a throwaway
`:memory:` one is fine), before opening the database; then open the
database the HPPLite way. Connections opened afterwards in the process are
covered automatically.

```python
# Python sqlite3 (SQLAlchemy: pass a creator; Django: NAME = uri)
boot = sqlite3.connect(":memory:"); boot.enable_load_extension(True)
boot.load_extension("/path/to/hpplite"); boot.close()      # no suffix needed
uri = "file:app.db?hpplite=on&l1=hpp-sepolia&keyfile=/etc/app/key"
db = sqlite3.connect(uri, uri=True)
engine = create_engine("sqlite://", creator=lambda: sqlite3.connect(uri, uri=True, check_same_thread=False),
                       poolclass=QueuePool)
```

```js
// Node node:sqlite (Drizzle: drizzle-orm/sqlite-proxy over it)
const boot = new DatabaseSync(":memory:", { allowExtension: true });
boot.loadExtension("/path/to/hpplite.dylib"); boot.close();
const db = new DatabaseSync("file:app.db?hpplite=on&l1=hpp-sepolia");
// npm sqlite3 (Knex): new sqlite3.Database(":memory:").loadExtension(path, cb) first
```

- **One connection to a file posts, whatever opens it**: threads with their
  own connections, a pool, several processes (php-fpm, gunicorn workers, a
  cron job beside the server). The first to open is the *sequencer*; the
  others capture into the same log in the file and leave posting to it.
  When the sequencer closes or its process dies, another connection takes
  over within a second, as the same sequencer to L1 (no lease transaction).
  `hpplite_status.sequencer` says which one posts.
- **Close flushes** if this connection is the sequencer: it posts what is
  pending and waits for L1. A follower closes at once - unless the
  sequencer is gone, in which case it takes over first (the last connection
  out posts). Bounded: an unreachable RPC costs one timeout (15 s,
  `HPPLITE_RPC_TIMEOUT`) and the rows stay in the file for the next open.
  To be sure before exiting, `SELECT hpplite_flush()` and check `pending`
  is 0.
- **Short-lived connections** (PHP opens one per request, CGI, cron): run
  `hpplite-sequencer <file>` beside the app with the same `HPPLITE_PARAMS`.
  It holds the file as its sequencer, so every request is a follower and
  never waits for the chain (WordPress, 8 PHP workers, 16 concurrent
  clients: ~4% over plain SQLite; `tests/real_apps/wordpress`).
- **A connection opened without HPPLite writes past the log** (`sqlite3
  app.db` from a shell, a tool that opens the file plainly). The roots the
  sequencer posts still match - it hashes the live file - and the difference
  shows only on a rebuild. It is detected, best effort: when a bookkeeping
  look finds the file changed while no connection of this process committed
  and HPPLite's own tables are as they were, `hpplite_status.foreign_writes`
  goes up and one line says so on stderr (#37). Another HPPLite process's
  writes leave log rows and are not counted. Open every writer with
  `hpplite=on`.
- **Everything that writes is replayed**, DDL included: `CREATE`, `ALTER`,
  `DROP`, the table rebuilds migration tools do, `PRAGMA user_version` and
  `application_id`. `changes()`, `last_insert_rowid()` and `RETURNING`
  report the application's statements as in plain SQLite.
- **One wallet is one database on chain**, plus one per `dbname=`. A new,
  empty file opened with the same key (and the same name) rebuilds that
  database from L1 (that is how you verify, or move to another machine). In the same process as the live writer it says
  `another sequencer instance holds the lease`: expected - it is a reader.

**Every connection that writes must open with `hpplite=on`** (or be named
by `HPPLITE_FILES`) - a plain connection's writes are not captured. Within
a process the sequencer is the first `hpplite=on` connection to the file;
across processes it is whoever holds an flock on `<file>-hpplite`, a small
file beside the database that also keeps the seed of the sequencer's id
(the id is the file's and the host's: every process on the host is the same
sequencer to L1; a copy of the file on another machine is not). Every
connection also holds `<file>-hpplite-users` shared while open, so a closing
sequencer knows whether someone will take over: it keeps the lease for them,
or releases it when it is the last. Both files belong with the database (and
need a local filesystem, as SQLite's WAL does). Followers capture into the
shared log and post nothing; the sequencer's flush posts everyone's rows in
commit order, transaction groups intact (`tests/unit/test_concurrency.c`:
threads and processes writing at once, replica equal). Loading HPPLite twice
into one connection (static library plus module) is refused.

**Statically linked** with the SQLite built in this tree (what the tests use):

```bash
mkdir build && cd build && ../configure && make sqlite3.c   # from repo root
cmake -S ext/hpplite -B ext/hpplite/build_real -DHPPLITE_USE_REAL_L1=ON
cmake --build ext/hpplite/build_real
```

## Testing

```bash
# Unit tests (mock L1, fast, no network)
ctest                    # 42 tests, ~1 min: core, batch, node, open API, cipher, keys,
                         # durability, loadable extension, crash harness, row capture,
                         # Python/CLI smoke, docs check; every suite again with capture=text

# Fault injection (#98): a random workload under injected failures - a post refused,
# a confirmation lost, a transaction pending past the timeout, a claim that reverts,
# a bookkeeping commit that fails as SQLITE_FULL, a reorg, the lease expiring - with
# the chain and a rebuilt replica as the oracle. In ctest with 60 seeds per mode.
HPPLITE_FAULT_SEEDS=500 build_mock/test_faults          # more seeds
HPPLITE_FAULT_SEED=17 build_mock/test_faults            # one seed, verbose

# Real contracts on a local chain (foundry: anvil + forge + cast; no network, no funds).
# Lease claim/renew/release transactions, a second instance of the same wallet, a
# stranger claiming after expiry, sleeping for days (evm_increaseTime). Built with
# the real-L1 backend; skipped when foundry is missing.
cmake -S ext/hpplite -B build_real -DHPPLITE_USE_REAL_L1=ON -DHPPLITE_SQLITE_SRC=... && cmake --build build_real
(cd build_real && ctest -R test_anvil --output-on-failure)

# The same on Linux, in a container (Docker or Podman; needs nothing else installed)
ext/hpplite/tools/linux_test.sh          # Ubuntu 24.04, Release, ctest
ext/hpplite/tools/linux_test.sh asan     # AddressSanitizer + UBSan
ext/hpplite/tools/linux_test.sh stress   # + the 100 MB stress test

# ORM smoke tests on anvil: SQLAlchemy + Alembic, Django (migrate, ORM), Drizzle, Knex;
# a replica rebuilt from the chain must match each writer. First run pip/npm installs
# into $TMPDIR/hpplite-orm (network), so not in ctest or CI
tests/orm/run_orm.sh build_real/hpplite.dylib [sqlalchemy|django|drizzle|knex ...]

# Integration tests (real L1, requires funded wallet); also runs the examples
export HPPLITE_PRIVATE_KEY=0x...
./tests/integration/run_integration.sh

# Microbenchmark (write overhead, flush, replay, root hash); see doc/performance.md
make bench

# Stress: 100 MB through encryption, DA chunking, checkpoints with roots, reconstruction
make stress            # ./stress_hpplite [--mb N] [--checkpoint N] [--limit BYTES]

# Crash injection (also in ctest): kill -9 at deterministic flush points and at random
# moments while the timer flushes; reopen, check nothing is lost or duplicated
./crash_hpplite --rounds 20
```

## Demo

`examples/demo/` is a public timeline page shared by visitors and a cast of
resident AI agents (written by a local model via LM Studio/Ollama, else
Claude, else templates): stock Python `sqlite3` loads the module, every entry
becomes an encrypted batch on HPP Sepolia with explorer links, a panel shows the ciphertext the chain
holds, and **Verify** rebuilds the database from the chain alone in a
separate process and compares roots. `examples/demo/run_demo.sh` serves it
and opens a Cloudflare quick tunnel (needs `cloudflared`, a real-L1 build
and `HPPLITE_PRIVATE_KEY`).

## Gas Costs (HPP Sepolia)

Measured on HPP Sepolia on 2026-10-28, where gas is 0.01 gwei (the chain's
base fee; it is a testnet, so there is no dollar price). The share by
contract, from the demo wallet's transactions: `submitBatch` 94% of all
gas, `submitCheckpoint` 3%, lease claim/renew/release 3%.

| Operation | Gas | Cost (ETH @ 0.01 gwei) |
|-----------|-----|------------------------|
| Factory deploy (VERSION 7, optimizer on) | 2.8M | 0.000028 |
| Rollup creation (`getOrCreateRollup`, `createRollup` for a named database) | ~2.5M | 0.000025 |
| Batch submit (~250 B sealed; ~0.7k gas per further byte) | 323k | 0.0000032 |
| Batch submit (29 KB sealed, as cut on HPP Sepolia) | 20.7M | 0.00023 |
| Checkpoint | 50k | 0.0000005 |
| Lease claim / renew / release | 28-37k | 0.0000003 |

**Batch size on HPP Sepolia is set by the chain, not the contract.** The
contract allows 128 KB, but a transaction may use at most 32M gas there
(measured 2026-10-30; the block gas limit is 2^50, the per-transaction cap
binds), and contract storage costs ~0.7k gas per byte: ~44 KB is the most a
batch can carry. Its RPC also refuses more than ~84.9 KB of calldata
("oversized data"). HPPLite knows both for HPP Sepolia (by chain id) and
cuts batches at 40 KB; elsewhere set `maxtx=` (bytes) and `maxtxgas=`, or
let it learn: a batch the RPC refuses as oversized, or whose gas estimate
is over the cap, is never sent, and later batches are cut smaller. Bulk
data costs ~0.008 ETH per MB of SQL at 0.01 gwei.

| Usage | Batches/day | ETH/day on HPP Sepolia |
|-------|-------------|------------------------|
| Light (personal) | 10 | 0.00003 |
| Medium (small app) | 100 | 0.0003 |
| Heavy (production) | 1,000 | 0.003 |

On a chain priced like Ethereum mainnet (10 gwei) every figure is 1000x,
which is why on-chain storage DA is a testnet design: see #11/#33.

**Cost driver, and what #33 changed**: until version 8 `HPPLiteDA` kept
batch bytes in contract storage (`batchData[height]`), the most expensive
place to put them: ~0.7k gas per byte, the figures in the table above.
Since version 8 the bytes go into the transaction's log and storage keeps
only the commitment - the height, a rolling hash over every batch and the
block the log is in - so a batch costs about 21k gas for the transaction,
24 gas per byte (calldata and log data), and some 25k for the three slots:
a sealed 1 KB batch measured 97.5k gas on anvil where storage took ~765k,
eight times less, and the gap widens with size. A reader fetches one block's
log by the height topic, which any archive node serves; pruned nodes cannot
serve a rebuild. The table above is re-measured on HPP Sepolia with the
version 8 factory below. Compression (#12) still applies, and the timer only
flushes when something is pending, so an idle database costs nothing.

## Components

| File | Description |
|------|-------------|
| `hpplite.c` | Lifecycle of a tracked connection, the connection registry, transparent API, extension entry point |
| `capture.c` | Capture into `_hpplite_pending`: the trace, authorizer, commit and rollback hooks; the text rules; the stale-rerun machinery |
| `bookkeeping.c` | The bookkeeping transaction and the foreign-write check |
| `finalize.c` | Batch build, size cut, finalize; the posted batches kept for a reorg |
| `replay.c` | Applying batches from the chain; adopt, skipped heights, checkpoints |
| `state_hash.c` | Content-hash state root and block hash (`sha256.c`) |
| `status.c` | `hpplite_status` table and the close sentinel |
| `capture_rows.c` | Row capture (#19): the TEMP triggers, the record format, what stays text, `sqlite_sequence`, applying records on a replica |
| `cipher.c` | Batch encryption: ChaCha20-Poly1305, HKDF-SHA256 key derivation, envelope format, pluggable schemes |
| `node.c` | Node lifecycle, bookkeeping connection, L1 sync, flush with retry, lease |
| `batch.c` | Batch creation and serialization |
| `clock.c` | Pass-through VFS that records and pins SQLite's "now" so clock SQL replays (#56) |
| `crypto.c` | secp256k1 signing |
| `config.c` | URI parsing, configuration |
| `l1.c` | L1 dispatch: every build has every backend, the endpoint picks one (`"mock"` or an RPC URL); `l1_backend.h` is the function table |
| `l1_eth.c` | Ethereum backend: the rollup contracts over JSON-RPC |
| `l1_mock.c` | Mock backend: in memory, or in a directory for the crash harness |
| `eth/` | Web3 primitives (RLP, ABI, keccak256) |

## Contract Interface

```solidity
// HPPLiteFactory
function getOrCreateRollup() external returns (address);          // the wallet's primary
function getRollup(address owner) external view returns (address);
function createRollup(uint256 checkpointInterval) external returns (address);   // one more, for a named database (#61)
function getRollupsByOwner(address owner) external view returns (address[] memory);

// HPPLite (per-rollup coordinator, VERSION 8)
function claimSequencer(bytes32 instanceId) external;
function renewLease(bytes32 instanceId) external;
function releaseLease(bytes32 instanceId) external;
function submitCheckpoint(bytes32 instanceId, uint256 from, uint256 to, bytes32 root) external;
// the same with the sequencer wallet's EIP-712 signature, submitted by anyone (#33, #126):
function claimSequencerFor(bytes32 instanceId, uint256 nonce, bytes sig) external;   // renewLeaseFor, releaseLeaseFor alike; leaseNonce() is spent
function submitCheckpointFor(bytes32 instanceId, uint256 from, uint256 to, bytes32 root, bytes sig) external;

// HPPLiteDA (per-rollup store, version 2): the bytes go into the log, storage keeps the commitment
function submitBatch(bytes32 instanceId, uint256 height, bytes data) external;   // emits BatchSubmitted(height, keccak256(data), data)
function submitBatchFor(bytes32 instanceId, uint256 height, bytes data, bytes sig) external;
function commitBatch(bytes32 instanceId, uint256 height, bytes32 hash, bytes ref, bytes receipt) external;   // bytes held elsewhere (#11)
function chainHash() external view returns (bytes32);        // keccak256(abi.encode(prev, height, hash)) over every batch
function blockOf(uint256 height) external view returns (uint64);   // the block whose log carries the batch
function getDAState() external view returns (uint256 lastBatchHeight, uint256, bytes32 chainHash);
```

A rollup created before version 8 keeps its batches in the DA contract's
storage (`getBatch(height)`); HPPLite reads the coordinator's `VERSION`
and uses whichever it finds, so older rollups stay readable and verifiable.

## Verification

A replica rebuilt from L1 compares its content hash with the root posted at
each checkpoint (`roots_compared`, `root_mismatches`). Until the first
checkpoint (`checkpoint=`, default every 100 batches) there is no posted
root yet: compare `SELECT state_root FROM hpplite_status` of the writer and
the rebuilt file yourself, or use a smaller `checkpoint` in development.

`hpplite_status` columns: `height` (next batch number), `pending` (log rows -
records or statements - not yet posted), `state_root`, `prev_block_hash`,
`last_checkpoint_height`, `lease` (1 = this instance may post), `log_errors`,
`replay_skipped`, `encryption`, `keyed`, `nondeterministic`, `contract`,
`da_contract`, `last_batch_tx`, `foreign_batches`, `hooks_replaced`,
`roots_compared`, `root_mismatches`, `txns_dropped`, `last_refusal`,
`sequencer` (1 = this connection posts the file's batches; 0 = another
connection, in this process or another, does), `capture` (`rows` or `text`:
what this connection logs), `foreign_writes` (looks that found the file
changed by something that is not HPPLite, #37), `state` and `last_error`
(#32), `chain_hash` (the DA contract's rolling hash over every batch as this
node tracks it, #33; NULL in a file from before it was kept). `state` says what this node is doing about posting: `sequencing`,
`retrying` (the last post failed, backing off), `waiting` (another instance
holds the lease), `follower` (another connection on this file posts),
`verify` (`role=verify`: an audit, never posts), `unposted` (no chain configured: the log waits for one), `local` (`l1=local`:
finalized here, no history kept), `disconnected` (a chain was configured but
is not reachable), `error` (sync stopped: the history and the file do not
agree), `stopped`. `last_error` is the last failure said on stderr - a post
not confirmed, a batch that cannot be decoded or replayed, a root mismatch,
the lease held elsewhere - and is NULL again once a batch is posted and
finalized or a sync completes. A program polling `height` can tell why it
stopped from these two columns alone.


What exists in v0.1 is **rebuild and compare**: open an empty file with the
same key and `l1=`/`contract=`; every batch is fetched, opened and replayed,
and after each batch that carries a state root the content hash of the
rebuilt database is compared with the posted one (`hpplite: state root
mismatch after replaying batch N` on stderr when they differ). The live root
of any database is `SELECT state_root FROM hpplite_status` (or
`hpplite_db_hash()` from C).

```bash
sqlite3> .load ./hpplite
sqlite3> .open 'file:rebuilt.db?hpplite=on&l1=hpp-sepolia&keyfile=/path/to/key'
sqlite3> SELECT height, state_root, roots_compared, root_mismatches, replay_skipped FROM hpplite_status;
```

**`hpplite-verify`** (#5) packages this as a command:

```sh
HPPLITE_PARAMS="l1=hpp-sepolia&keyfile=/path/to/key" hpplite-verify --db /path/to/app.db
```

It opens an empty file with `role=verify` and the application's own
parameters (`l1=`/`rpc=`/`factory=`/`contract=`, the key, `dbname=`),
rebuilds from the chain alone, compares every posted root with the rebuilt
content, and compares the root the contract anchors at its last checkpoint
with the rebuilt content at that height. With `--db` it also compares the
live database (the same content hash as the state root) and, when they
differ and nothing is pending, names the tables that differ. Exit 0 verified,
1 not (a root mismatch, a sync that stopped short, a live file that
differs), 2 could not run. The rebuilt file is deleted unless `--keep <dir>`.
A verifier never posts, never claims the lease and never creates a rollup;
`role=verify` is also usable from any connection that only wants to audit.
It needs the wallet key (batches are encrypted); a viewing key is v0.3 (#15).
There is no fraud proof or challenge period: the chain holds the log and
the roots, the key holder is the one who checks them.

## Stores and commitments

Three words the rest of this document uses precisely (#35, #11):

- A **store** is where the bytes of a batch live. Today that is the rollup's
  DA contract on the chain, or a directory. S3 and IPFS stores follow (#11).
- A **commitment** is what the chain holds per height: HPPLite's own hash
  of the batch and a reference into the store (on chain, the height itself;
  later a CID or an object key), and, where a pinning service holds the
  bytes, its signed receipt (#33). A replica reads the commitment, fetches
  by the reference, checks the hash, replays. Verification never depends on
  what the reference means.
- **Data availability** is a property, not a place: anyone can fetch the
  bytes needed to rebuild, without the writer, for as long as the chain
  lives. The on-chain store has it by construction. A pinned store has it
  only as far as the pinner's receipt and the fallback to posting on chain
  carry it. A directory has none: it is a store with no availability claim.

Every build carries every backend; the `l1=` value picks one at run time
(`l1.c`, `l1_backend.h`): a network alias or `rpc=` is the Ethereum client,
`file:///path` the directory, `mock` the tests' mock.

### A directory instead of a chain: `l1=file:///path`

```
sqlite> .open 'file:app.db?hpplite=on&l1=file:///var/lib/app/log&encrypt=off'
```

The directory is the rollup: the lease, the latest checkpoint, the height
and every batch are files in it (`lease`, `checkpoint`, `height`,
`batch-N.bin`, `config` with the checkpoint interval and lease duration),
each written to a temporary name and renamed into place, so a crash leaves
the old or the new content and never a torn file. Nothing is cached: a
writer, replicas (`role=verify`, or a second file opened on the same
directory after the writer's lease ends) and `hpplite-verify` read the same
files and see one state, as they would see one chain. The lease works as
on chain - one instance posts, by the wall clock, a crashed writer's lease
expires after `leaseDuration` (300 s by default; the owner key can expire it
at once) - and `hpplite-verify` audits a directory the way it audits a
rollup:

```
HPPLITE_PARAMS="l1=file:///var/lib/app/log&encrypt=off" hpplite-verify --db /var/lib/app/app.db
```

No wallet is needed: without a key the batches are plaintext (`encrypt=off`
must say so), with one they are sealed exactly as on chain, and the key is
also what makes a connection the directory's owner. There is no factory and
`dbname=` does not apply: one directory, one database. What it is for: a
Litestream-like log of a database with a content-hash checkpoint and no
chain, the offline fixture for `hpplite-verify`, and the crash harness
(`crash_hpplite --l1 dir`). What it is not: data availability - the
directory is as available as the disk it is on. The crash harness and the
directory test (`test_l1_dir`) run against it in every build.

## Versions

Scope is tied to versions. v0.1.1 is the last of the v0.1 line (end of life:
upgrade to v0.2; `capture=text` keeps v0.1's capture). **v0.2.0 is tagged**
(2026-11-01): row capture by default, `hpplite verify`, several writers per
file, the fault harness as the gate, an inventory of the SQLite behaviours
the capture relies on with a CI build against SQLite trunk.

| Version | Scope | Database size | DA |
|---------|-------|---------------|----|
| **v0.1** | Library + loadable extension on stock SQLite, crash-safe local file, reconstruction from L1, batches cut at the DA limit, root at checkpoint cadence, encrypted batches, one writer process | per-tenant / single-tenant, up to ~100 MB | on-chain (contract storage) |
| v0.2 | Row capture by default (#19), validated on suites and real applications (WordPress, Ghost, Rails 8), several writers per file, `hpplite verify` (#5) | same | same |
| v0.3 | S3-backed DA with L1 commitments, incremental (Merkle) state root, batch compression (#12), key roles and viewing keys (#17, #15) | GB range | S3 + L1 commitments |
| v1.0 | Frozen batch / meta / URI formats, language bindings | — | — |

Why the size bound in v0.1: the state root is a content hash, O(database size) per
computation (~9 ms/MB), computed once per checkpoint interval (default 100 batches);
see [doc/architecture.md](doc/architecture.md#state-root).
Beyond that, shard per tenant (the factory can create several rollups per wallet)
until v0.3.

**v0.1.x is end-of-life.** There will be no v0.1.2: everything v0.1 did, v0.2
does with `capture=text`, and the bugs found in code the two share (`PRAGMA
count_changes` refusing every write, the rerun of a refused statement, a
rerun transaction left open after a `CREATE VIRTUAL TABLE`, configuration
errors ignored at open, among others - #96 lists them) are fixed only on
v0.2. Upgrading: a v0.2 replica reads v0.1 history, so upgrade the
replicas first, then the writer. A v0.2 writer logs records by default,
which v0.1 replicas cannot read; give it `capture=text` until every replica
is on v0.2. Do not run a v0.1 binary on a file that has records pending
(#81, below).

Issues are grouped by [milestone](https://github.com/shepelt/hpplite/milestones).

## What is refused

By default (row capture, below) a replica applies what each write changed,
so what a statement computed - random values, the clock, application
functions - does not matter. What is still logged as SQL *text* must produce
the same rows wherever it runs: schema changes, writes that reach a virtual
table, and every write with `capture=text` (v0.1's capture). For those, what
cannot be reproduced is **refused at the write**
(`SQLITE_INTERRUPT`, a line on stderr, counted in
`hpplite_status.nondeterministic`) rather than discovered as a root mismatch
a hundred batches later:

| Refused | Why | Do this instead |
|---------|-----|-----------------|
| `random()`, `randomblob()` in a write | different value on replay | generate in the application, bind it; reads (`ORDER BY random()`) are fine |
| `'localtime'` / `'utc'` modifiers in a write (`datetime('now', 'localtime')`) | the machine's time zone differs between replicas | store UTC, convert when reading (reads may use them) |
| `last_insert_rowid()`, `changes()`, `total_changes()`, `sqlite_version()`, `sqlite_source_id()`, `sqlite_compileoption_*()`, `load_extension()` in a write | connection or build state | read the value (`SELECT last_insert_rowid()` is fine), then bind it |
| a write that calls a function the application registered (`sqlite3_create_function`, Python `create_function`; Django's `django_date_extract` behind `filter(due__month=3).update(...)`), directly or through a view, trigger, generated column or expression index | a replica has SQLite's functions only, so the statement could not run there (#53) | compute the value in the application and bind it, or filter with a built-in (`due__year`, a range); reads may call it |
| `ATTACH` | writes to another file are not captured | one database per file |
| writes that name `_hpplite_*` tables (`INSERT INTO _hpplite_pending`, a trigger on it, `RENAME TO _hpplite_x`, `INSERT INTO t SELECT ... FROM _hpplite_pending`) | the log and meta are per-replica, not data (#18) | reads are fine (`SELECT`), writes go through HPPLite |

**What a refusal looks like.** The driver reports only SQLite's code:
`interrupted` (`SQLITE_INTERRUPT`) or `not authorized` (`SQLITE_AUTH`) - in
Python an `OperationalError`, in Node an `Error` with that text - and the
transaction it was in is rolled back. If a write fails that way, the reason
and the statement are on stderr (`hpplite: ... statement refused`) and one
query away:

```sql
SELECT last_refusal FROM hpplite_status;
-- non-deterministic function random() in a write; replay cannot reproduce it; ... : INSERT INTO t VALUES(random())
```

`determinism=warn` (URI) or `HPPLITE_DETERMINISM=warn` restores warn-and-count
for people who know what they are doing (the `_hpplite_*`, size and
application-function rules stay). Detection is the parser's
authorizer (each write's functions, through views and triggers, checked
against what a fresh connection has) plus a scan of the text outside string
literals. Statements that cannot write (`SELECT`, `WITH ... SELECT`) are
never refused: they are not logged (#54).
`tests/unit/test_abuse.c` is the corpus of spellings it must catch and of
look-alikes it must let through.

**Row capture (the default since v0.2; #19).** Instead of each write's
text, HPPLite logs what it changed: one record per inserted, updated or
deleted row, written by TEMP triggers inside the application's transaction.
A replica applies the records, so `random()`, the clock, `'localtime'`,
application functions such as WordPress's `REGEXP`, and `CURRENT_TIMESTAMP`
defaults all replay exactly and are no longer refused. Schema changes, and
writes that reach a virtual table (FTS5, R*Tree - directly, or through the
sync triggers of the table written), are still logged as statements, under
the rules above. Batches with records cannot be read by v0.1: it stops at
the first with "statement N ran under connection state this version cannot
apply (mode 4)" - upgrade the replica. A v0.2 replica reads v0.1 history.
`capture=text` keeps v0.1's capture. Do not run a v0.1 binary on a file
that has records pending (flush first, or close every connection): v0.1
would post them as empty statements, and that history cannot be read by
either version (#81). Design and measurements: `doc/design-row-capture.md`.

**The clock is supported.** `CURRENT_TIMESTAMP`, `datetime('now')`,
`unixepoch()`, `strftime('%f', 'now')` and columns with `DEFAULT
CURRENT_TIMESTAMP` - the timestamp defaults every ORM template uses - work in
writes. SQLite takes "now" from the VFS; HPPLite registers a pass-through VFS
as the default that records the time each write read and pins that same time
when the write is replayed, so a replica computes identical values (#56).
Reads see the real clock. The one condition: the connection must be on that
VFS, which it is unless the application names another one (`vfs=` in the URI,
or a VFS registered as default after HPPLite was loaded); there, clock SQL in
writes is still refused.

**Transactions and statements of any size are supported.** One larger than
a DA batch (a seed script, `bulk_create` of thousands of rows, a data
migration, a multi-row `INSERT` an ORM builds from a list - one statement
can be larger than a batch, #59) is posted across several batches: the batch it starts in is marked open, the ones it
continues in say so, and a replica applies it only when its last part has
arrived - a reader never sees part of it. If the writer dies with the rest
in a log nobody else has, replicas drop the held part whole and count it in
`hpplite_status.txns_dropped`, as if it had never been posted (#57).

**Foreign keys and recursive triggers are supported.** `PRAGMA foreign_keys`
and `PRAGMA recursive_triggers` are connection state: they change what a
statement does to other rows (`ON DELETE CASCADE`, `SET NULL`, `SET DEFAULT`,
`ON UPDATE CASCADE`, a trigger that fires itself) without showing in its
text. HPPLite records the state each statement ran under and replays it in
that state, so an application that turns foreign keys on at connect (as
Django and Rails do) needs no change for it, and switching the pragmas
mid-session is fine (#52). The same goes for `legacy_alter_table`,
`writable_schema`, `ignore_check_constraints` and `case_sensitive_like`
(#28): each statement and record carries the state they were in, replay
sets it before applying, and `PRAGMA schema_version = N` (the reload after
editing `sqlite_schema`) is logged like `user_version`. Set them on the
connection itself; a pragma set before `hpplite_init()` is read then,
except `case_sensitive_like`, which SQLite cannot report. One edit under
`writable_schema` is refused: an `UPDATE` or `DELETE` on `sqlite_schema`
with no `WHERE`, which would rewrite HPPLite's own schema rows along with
the application's and leave the file unreadable; name the table (#124).

Not detectable, so not refused - know these:

- **Bound `REAL` values** are expanded with 15 significant digits
  (`sqlite3_expanded_sql`), so `0.1 + 0.2` replays as `0.3`. Store exact
  decimals as `INTEGER` or `TEXT`, or write the literal yourself. (Node's
  `node:sqlite` binds every JS number as `REAL`, so its integers appear as
  `1.0` in the captured text; the writer stored the same value, so replay
  agrees - bind `BigInt` if you want integers stored as integers.)
- **`TEXT` with an embedded NUL** is cut at the NUL in the captured text.
- **`INSERT ... SELECT` without `ORDER BY`** into a rowid table takes its row
  order from the query planner.

Row capture (the default since v0.2, #19) lifts all of this for the writes
it records; it still applies to `capture=text`, and to the statements row
capture logs as text (schema changes, writes reaching a virtual table).

## Keys

One wallet key does everything in v0.1: it owns the rollup contracts, posts
batches (sequencer), and the batch encryption key is derived from it. It
reaches HPPLite before open, in order of precedence:

| How | Use for |
|-----|---------|
| `keyfile=/path` or `HPPLITE_PRIVATE_KEY_FILE` | **production**: a file with the hex key, mode `0600`; group/world-readable files are refused (like ssh) |
| `HPPLITE_PRIVATE_KEY` | containers / process managers |
| `privkey=0x...` in the URI | **development only**: SQLite keeps the query string next to the filename, so anyone with the `sqlite3*` can read it back via `sqlite3_uri_parameter()`, and URIs end up in logs |

Without a key the database works and keeps capturing, but nothing is posted
(`hpplite_status.keyed = 0`): with an L1 configured the batches stay pending
until a key arrives. With no L1 configured at all (`hpplite=on` alone) the
log is kept pending too - the first flush says so once on stderr, and
`hpplite_status.state` is `unposted` - and posts in full once `l1=` names a
chain. `l1=local` is the explicit development mode: batches are finalized in
the file and the history is kept nowhere (#38). `l1=file:///path` needs no
key at all: the directory is the chain, and `encrypt=off` posts plaintext
there, or a key seals the batches as on chain. `encrypt=off`
is the only way to post plaintext and must be explicit. A URI with
`hpplite=on` that cannot be honoured (unreadable key file, malformed key)
makes `sqlite3_open_v2()` fail rather than open without capture. v0.2 splits the roles (owner / sequencer / data /
viewing keys), accepts keys at runtime (`hpplite_key()`), Ethereum keystore V3
files and KMS-held signers - see issues #17 and #15.

## Privacy

Everything posted to L1 is public, so batch payloads are **encrypted before
posting** whenever a private key is present (`encrypt=off` or
`HPPLITE_ENCRYPT=off` to disable). The scheme is ChaCha20-Poly1305 with a key
derived from the wallet private key (`HKDF-SHA256`), so whoever can write the
rollup can read it and there is no extra secret to manage; reconstruction and
verification work from the same `privkey=`. Each batch is sealed with a fresh
nonce and bound to its height. `SELECT encryption FROM hpplite_status` shows
the active scheme; schemes are pluggable (`cipher.h`).

What stays public on L1: heights, batch sizes and timing, batch hashes, and
the state roots in checkpoints (content hashes that reveal *when* content
changed, not what). Only key holders can replay and verify; sharing read
access without the wallet key is the public-key scheme planned for v0.2.

## Durability

HPPLite keeps no state outside your database file. Captured records and
SQL go into `_hpplite_pending` from inside your own transaction (rolled back with it,
committed with it); height and roots go into `_hpplite_meta` once a batch is
confirmed on L1. Kill the process at any point and reopen: pending SQL
flushes, height continues, nothing is replayed twice. A batch that L1 does
not confirm stays pending and is retried at the same height.

File databases are switched to WAL mode on open (bookkeeping runs on a
second connection) and get a 5 s busy timeout. A write that cannot take the
write lock within that timeout (another process holds it) fails with
`SQLITE_BUSY` after one wait, as it would without HPPLite, and the driver's
retry runs it again through capture; it is never refused for that. In-memory
databases work through the C API (`hpplite_node_create` with `:memory:`),
with bookkeeping on the same connection and no timer: flush explicitly or on
close.

Three things a statement can be refused for, all with `SQLITE_INTERRUPT`
and a line on stderr, so the log never silently disagrees with the data:
its log row could not be written (disk full), its text is longer than one
DA batch can carry (a 64 KB blob literal is ~128 KB of hex), or its bound
parameters could not be expanded. Inside an explicit transaction SQLite
rolls the transaction back, as it would for a disk-full error.

See [doc/architecture.md](doc/architecture.md#local-durability).

## Trust model

The sequencer is your process; it is trusted to execute SQL. What the chain
adds is a history it cannot quietly rewrite: batches are posted in order
under a lease, roots are anchored, and anyone holding the key can rebuild
from the chain alone and compare. Nothing in v0.1 punishes a divergent
sequencer - there are no funds, bonds or fraud proofs here - it makes the
divergence detectable, and a lost disk recoverable. The lease keeps two
copies of your process from posting at once, and only the rollup's
sequencer wallet (the owner, unless set apart) can take it - a rollup can
sleep for days and nobody else can claim it. Rollups from the previous
factory keep the old permissionless claim; there the node evicts a
stranger's lease as owner, and a batch posted by someone else (it fails our
authentication, so it is provably not ours) is stepped over by every key
holder the same way instead of stopping reconstruction.

See [doc/architecture.md](doc/architecture.md) for details.

## License

See parent SQLite project for license terms.
