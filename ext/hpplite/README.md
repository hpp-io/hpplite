# HPPLite

**A crash-safe, encrypted, replayable write log for SQLite, kept at a store in seconds and anchored on a chain.**

HPPLite is a SQLite extension. It captures what each commit wrote - the
database pages, read from the WAL - cuts that into batches and seals them
with a key derived from your wallet key.

- **Soft tier:** each batch goes to a store within seconds, under its
  height and signed by the sequencer.
- **Hard tier:** every 30 minutes, at HPP's own cadence, one transaction
  anchors everything since the last anchor on an Ethereum-compatible chain:
  the chain hash, the state root (a Merkle root over the database), and the
  manifest.

Open an empty file with the same key and the database rebuilds itself from
the chain and the store, checking every root on the way. A replica follows
the sequencer within a flush interval, not an anchor interval. A sequencer
that dies loses at most what it hadn't flushed: its successor recovers the
rest from the store.

Stock SQLite, loadable extension, no patches. One sequencer posts. The
store is paid in USDC.e by the rollup's funding wallet, by permit as the
bytes accrue. A wallet with no ETH can run a rollup through a relay that
sends its transactions, paid per call over x402.

What HPPLite is **not**:
- Not a multi-writer database: one sequencer per rollup.
- Not a public log: batches are encrypted, so only the wallet key or a
  granted viewing key can read and replay them.
- Not trustless storage: a batch that never reached a store is the
  sequencer's to lose, and one that reached a single store and wasn't
  anchored yet is only as safe as that store (#180).

See [Versions](#versions).

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

`l1=hpp-sepolia` selects the RPC endpoint, factory contract and batch interval (30 minutes, the chain's own cadence; `interval=5s` for development) for HPP Sepolia (see Contracts). On first open, the factory auto-deploys your rollup contracts (~3s). Without a store, the batches go on the chain itself. With `store=` (`on`, or an `hpplite-store/2` store's URL), they go to the store within seconds, and the chain gets one anchor per `anchor=` interval (see [Stores and commitments](#stores-and-commitments)).

The same works from the stock `sqlite3` shell after `.load ./hpplite`, and
from any language whose SQLite binding can load extensions.

## How it works

```
┌──────────────────────────────────────────────────────────────────────────┐
│  your process                                                            │
│                                                                          │
│  app ──► SQLite (WAL) ──► the commit's pages, read from the WAL file     │
│                                │                                         │
│               timer / close    ▼                                         │
│        batch = page diffs ──► deflate ──► ChaCha20-Poly1305              │
│                                │                                         │
│               ┌────────────────┴──────────────────┐                      │
│               ▼ seconds (soft)                    ▼ every 30 min (hard)  │
│     store: by content + under <rollup>-<height>,  chain: one range anchor│
│     signed by the sequencer, paid over x402       (chain hash, Merkle    │
│                                                   root, manifest)        │
└──────────────────────────────────────────────────────────────────────────┘
  replica: the chain up to the anchor, the store past it; rebuilds itself if
           the chain later commits another batch than the store served
```

- **Transparent** - `sqlite3_open_v2()` with URI parameters, or `.load`
- **Stock SQLite** - no core patches; loadable extension against any SQLite >= 3.24
- **Nothing to lose** - the capture reads each commit's pages from the WAL
  before they can be checkpointed away, and the sequencer keeps a read
  transaction pinned so SQLite never restarts the WAL over a frame it has
  not read. What the file holds and the chain does not is shipped at the
  next open. A crash or a failed submission never loses or duplicates a
  batch
- **Encrypted by default** - batches are sealed before they leave the
  process, with a key derived from your wallet key; the store and the chain
  see ciphertext. Readers can be granted their own viewing keys (#15)
- **Factory deploy** - one wallet, one rollup, created on first open; a
  named database (`dbname=`) gets its own rollup under the same wallet
- **Rebuild and check** - an empty file with the same key reconstructs from
  the chain and the store and compares every posted state root along the way;
  `hpplite-verify` does it in a separate process and reports the soft tier too

### The capture: pages (#146, the one capture since v0.8, #253)

HPPLite records *which pages* each commit wrote, as Litestream and LiteFS
do. After each commit the connection's WAL hook has HPPLite read the
commit's frames from the WAL file and queue the pages; the sequencer's
timer also reads the frames other connections and processes commit, and
keeps a read transaction pinned so SQLite never restarts the WAL over a
frame it has not read. A flush turns the queue into a **page batch**: every
page the batch's commits touched, once, as the whole page the first time a
replica sees it and as an XOR diff against the last posted image after,
deflated and sealed. The batch's root is a Merkle root over the hashes of
every page in the file (`root_version` 4), so a replica that applied the
same pages has the same root. Nothing is logged in the application's file:
the bookkeeping (height, roots, the batch in flight, the posted copies kept
for a reorg, the rows waiting for an anchor) lives in a sidecar beside it,
`<db>-hpplite.db`. The application's connection carries an authorizer that
keeps the file in WAL mode, keeps `wal_autocheckpoint` HPPLite's, refuses
`ATTACH` and reserves the `_hpplite_` prefix, and a trace that only tells a
`VACUUM` apart and polls the WAL at the end of a statement whose commit ran
without the hook (a `RETURNING` stepped once, a driver's `.get()`). A
replica writes the pages into its file under an exclusive lock, in
rollback-journal mode so its readers see every batch through the change
counter, and compares the root.

What this means for an application: a transaction costs what SQLite
charges for it plus about 30 µs (the OLTP numbers in
[doc/performance.md](doc/performance.md): within 1% of plain SQLite's write
rate, p99 0.16 ms against 0.14), and any SQL replicates - non-deterministic
functions, the clock, application-registered functions, virtual tables,
incremental blob writes, `VACUUM` - because the pages are what is shipped,
not the statements. Several connections and processes on one file work
alike: the sequencer captures what the others commit, a follower that is
the last connection out takes over and ships what the file holds beyond the
chain. A commit that dirties more pages than fit under the posting limit is
posted as a series of pieces, each under the limit, and a replica holds its
lock from the first piece to the last so its readers never see half a
commit (#152; a single page is the unit, so the limit must hold at least
one deflated page). A crash between a submit and its finalize is recovered
from the sidecar's record of the batch in flight; a sequencer reopened on a
file whose root is not the one its sidecar recorded (it died with commits
unposted, or something wrote the file while it was closed) ships the file
whole again. `hpplite_status.capture` says `pages`; a named database's
first batch carries the name.

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
| HPPLiteFactory | `0xbce61e56f84620bc4365e95126f82b51faed7b4e` | Creates rollups (one per wallet, plus one per named database); built into the `hpp-sepolia` alias. Contract VERSION 10 with DA version 3 (#171, deployed 2026-11-04: a deadline in lease signatures, `createRollupFor` and `setCheckpointIntervalFor` for a wallet with no ETH, the EIP-712 domain cached; VERSION 9's ranges and anchors as before). Rollups from the two factories before it keep working with the rules they were created with - give their factory with `factory=` (`0x7748552e…`: VERSION 9 with DA version 3, #164: range commitments, the anchor as the checkpoint, the lease duration the owner's to set, an hour by default, no storage that grows with the history; `0xa80365fd…`: VERSION 8 with DA version 2, #33: batch bytes in the log, the rolling chain hash, signed forms of every sequencer call, a five-minute lease; `0xac18df18…`: VERSION 8 too, deployed and superseded the same day, 2026-11-01: its DA recorded Ethereum Sepolia's block numbers (HPP is Arbitrum-style, #128), so no replica can fetch its logs). Factories before VERSION 8 (`0xc0fa50eb…` VERSION 7, `0xd1aaa8d1…` VERSION 6, `0x9c6abd03…` VERSION 5, `0x9cfacba5…` with the permissionless claim; batches in contract storage) are refused at connect since v0.8 (#252): v0.7 is the last release that reads them |
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
| `role` | No | `verify`: an audit connection - rebuilds and compares, never posts, claims the lease or creates a rollup (what `hpplite-verify` uses, #5). The only value: any other refuses the open (#248); a sequencer needs no `role=` |
| `snapshot` | No | With `store=` on a chain with ranges: how often the sequencer snapshots the database at an anchor - `24h`, `1h`; `0` (default) never (#221). The snapshot is the file at the anchored height, sealed like a batch, at the store in chunks under an index the anchor's manifest names, so a replica can start there instead of at batch 1. Also `HPPLITE_SNAPSHOT`. See Stores and commitments |
| `soft` | No | `off`: a `role=verify` connection with `store=` stays at the anchored height instead of following the store's batches past it (#166). Also `HPPLITE_SOFT` |
| `store` | No | `on` for the network's canonical store (`l1=hpp-sepolia`: `https://store.sepolia.hpplite.hpp.io`; a network without one refuses to open, #192), or `http://host:port` of a store speaking `hpplite-store/2` (`operators/store/hpplite_store.py`, or the canonical one): the rollup's bucket, kept while its funding wallet covers it (#214). The batch bytes go there and the chain holds the commitment - hash, reference, and with each anchor the store's signed bucket statement (#11, #33). A replica needs the same `store=` to rebuild. The protocol: [doc/store-protocol.md](doc/store-protocol.md). Also `HPPLITE_STORE` |
| `storecap` | No | The most the funding wallet pays the store by x402 at once (a top-up), in its asset's atomic units (USDC.e: 1e-6). A 402 above it is refused before anything is signed and the batch waits; 0 (default) pays nothing by x402. Also `HPPLITE_STORE_CAP` |
| `storepermit` | No | What the funding wallet's EIP-2612 permit lets the store pull for the rollup's bucket, in atomic units (default 20000000, 20 USDC.e; renewed as it runs low or within 30 days of its year). The funding wallet's key is derived from the node's (`store_funder` in `hpplite_status`: send USDC.e there, from any wallet or exchange; it needs no ETH). With a permit the store charges storage and writes as they accrue, so the bucket stays covered while the node is off; 0: no permit, each 402 is a top-up paid by x402 up to `storecap=` (#214). Also `HPPLITE_STORE_PERMIT` |
| `spendcap` | No | The most paid to the store and the relay together over a rolling window, in atomic units: `N/hour` or `N/day` (a bare `N` means per day; #173). It counts what was charged; a channel deposit isn't counted, because it is still the node's money. A put past it is refused before anything is signed, and the batch waits with the reason (`store_state` = `over cap`). The ledger is kept per wallet in the data directory (`spend-<wallet>.json`), so a restart doesn't reset the window. A node opened without its key keeps it in memory. Also `HPPLITE_SPEND_CAP` |
| `lowbalance` | No | Warn when the money left lasts fewer hours than this at the last day's rate (default 72; 0 turns it off; #173). USDC.e is the wallet's balance plus what is unspent in the store's channel, against the charges. ETH is the wallet's balance, against how fast it fell. The warning prints one line on stderr when it starts and when it clears, and shows in `spend_warning`. Also `HPPLITE_LOW_BALANCE` |
| `relay` | No | `on` for the network's canonical relay (`l1=hpp-sepolia`: `https://relay.sepolia.hpplite.hpp.io`, #192), or `http://host:port` of a relay ([operators/relay](operators/relay/README.md), #170): every write to the chain goes there as its EIP-712 signed form, paid over x402 like the store: creating the rollup (VERSION 10 factories), the lease, anchors, commitments, checkpoints. A wallet with USDC.e and no ETH runs this way. When the relay fails, the wallet sends the plain transaction itself if it holds ETH; otherwise the write waits and `last_error` says why (#126). The protocol: [doc/relay-protocol.md](doc/relay-protocol.md) (#188). Also `HPPLITE_RELAY` |
| `relaycap` | No | The most to pay the relay per call, in its asset's atomic units. With `relay=on` the default is 1000000 (1 USDC.e: a relayed call on HPP costs cents, #251); with a relay URL, 0 (default) pays nothing. Also `HPPLITE_RELAY_CAP` |
| `storefallback` | No | `on` (default): a store that does not answer lets the chain carry the bytes meanwhile, said once on stderr; `off`: the batch waits with the store's reason. Not past batches waiting for their anchor (#164): those wait for the store |
| `anchor` | No | With `store=` on a chain that takes ranges (VERSION 9 and later, #164, the `hpp-sepolia` factory): how often the batches the store holds are anchored on the chain in one commitment: `5m`, `30m` (default: HPP's cadence), `1h`; `0` = at close and on `hpplite_anchor()` only. Also `HPPLITE_ANCHOR`. See Stores and commitments |
| `datadir` | No | Where the per-wallet spending ledger `spend-<wallet>.json` lives (`spendcap=`, #173). It spans every database the wallet writes, so it cannot live in a database's sidecar, where everything else HPPLite keeps is. Default `/tmp/hpplite`. Also `HPPLITE_DATA_DIR` |
| `keep` | No | With `snapshot=`: how long the store keeps the history behind a snapshot - `all` (default: every batch, an audit log) or `7d`, `24h`, `30m` (#223). After each anchor the sequencer signs a `Prune` at the newest snapshot made at least that long ago; the store lets go of the batches and older snapshots below it. The snapshots kept are named in each manifest, at most 64 besides the latest, so `keep` divided by the snapshot cadence (the longer of `snapshot=` and `anchor=`: a snapshot is made at an anchor) must not exceed 64 (`snapshot=1h` with `keep=30d` would keep 720: the open refuses it and says so, #231; with anchors by hand, `anchor=0`, the anchor itself makes no snapshot while the list is full). The current state stays provable (that snapshot's root is anchored, then the batches after it); what is lost is replaying how the state got there, and `hpplite-verify --to`/`--at` before it. Also `HPPLITE_KEEP` |
| `interval` | No | Batch flush interval: `500ms`, `5s`, `1m`, `1h`; `0` = no timer (flush on `hpplite_flush()` or close). The default is 5 s, except that `l1=hpp-sepolia` (and `hpp-mainnet`) makes it 30 min for a node without a store, the chain's own cadence (HPP anchors to L1 every 30 minutes, so a batch posted more often only waits in the queue, and each batch costs its own commitment; see Gas Costs). With `store=` the chain's cadence goes to `anchor` instead and the interval stays 5 s: the batches reach the store in seconds, the chain gets one commitment per anchor (#164). Seconds for development, the chain's cadence in production. The timer also flushes early once the pending log reaches half the DA limit, so a busy writer posts as it goes (#143) |
| `encrypt` | No | `on` (default when a key is present) or `off` |
| `from` | No | Where a new replica (an empty file, `store=`) starts: `snapshot` (the default, except for `role=verify`) - the latest snapshot the anchors name (#222), checked against the root and chain hash the chain anchored at its height, then the batches after it; `earliest` - the oldest one still kept (`keep=`, #224: the most history verified; the latest when it is gone); or `genesis` - every batch from 1. A verifier replays from batch 1 unless given `from=snapshot`; `syncto=`/`syncat=` before the snapshot start from batch 1. Also `HPPLITE_FROM` |
| `compress` | No | `on` (default): the page batch is deflated before it is sealed or posted when that shrinks it (#12); `off`: posted as is. Every reader takes both forms. Also `HPPLITE_COMPRESS` |
| `checkpoint` | No | Batches per checkpoint, the batch that also carries a state root. Not given: the rollup contract's `checkpointInterval` (100 unless changed). Given: written to the contract by its owner at connect, so the two agree (#44). A history shorter than that has no root on chain to verify against yet (see Verification) |
| `dbname` | No | Give the database its own rollup among the wallet's, found by this name (#61): several databases on one key. Names are compared exactly and must be unique per wallet: two files with the same name on one key are one database on chain (the second, if empty, rebuilds the first's data into itself). `{file}` = the file's base name without its directory (`a/x.db` and `b/x.db` are both `x.db`; `:memory:` is `memory`), so one `HPPLITE_PARAMS` serves every file `HPPLITE_FILES` names. A name no rollup has yet creates one (~2.5M gas): a misspelt name on a replica is a new, empty database, and the message says which names the wallet has. The name is read from each rollup's first batch (or its snapshot) at the store; while the store does not answer for any of them, a sequencer on a fresh file creates nothing (#229: one of them may be this database) - writes stay local and each flush looks again; a store that answers 404 (the bytes gone) is no reason to wait. Also `HPPLITE_DBNAME` |

*Either `l1`, or both `rpc` and `factory` (or `rpc` and `contract`). The same settings can come from environment variables — see `.env.example`. `capture=` and `determinism=` (and `HPPLITE_CAPTURE`, `HPPLITE_DETERMINISM`) are gone since v0.8 (#253): a URI or environment that still names one refuses the open and says so.

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
| better-sqlite3 (Drizzle's and Knex's default) | a plain path with `HPPLITE_FILES` (`tests/orm/test_better_sqlite3.js`) | works. Its bundled SQLite is built with `SQLITE_TRACE_SIZE_LIMIT=32`, which cuts bound values in `sqlite3_expanded_sql()`; pages read no statement text, so it does not matter |
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
  cron job beside the server). The first to open is the *sequencer*; it
  reads every commit from the file's WAL, whoever made it, and the others
  leave posting to it. When the sequencer closes or its process dies,
  another connection takes over within a second, as the same sequencer to
  L1 (no lease transaction). `hpplite_status.sequencer` says which one
  posts.
- **Close flushes** if this connection is the sequencer: it posts what is
  pending and waits for L1. A follower closes at once - unless the
  sequencer is gone, in which case it takes over first (the last connection
  out posts). Bounded: an unreachable RPC costs one timeout (15 s,
  `HPPLITE_RPC_TIMEOUT`) and the commits stay in the file: the next open
  finds the file past the chain and ships what it holds. To be sure before
  exiting, `SELECT hpplite_flush()` and check `pending` is 0.
- **Short-lived connections** (PHP opens one per request, CGI, cron): run
  `hpplite-sequencer <file>` beside the app with the same `HPPLITE_PARAMS`.
  It holds the file as its sequencer, so every request is a follower and
  never waits for the chain (WordPress, 8 PHP workers, 16 concurrent
  clients: ~4% over plain SQLite; `tests/real_apps/wordpress`).
- **A connection opened without HPPLite** (`sqlite3 app.db` from a shell, a
  tool that opens the file plainly) is captured all the same while a
  sequencer is on the file: its commits are frames in the same WAL, read by
  the sequencer's timer and counted in `pending` at once
  (`tests/unit/test_pagemode.c`: a thousand commits, a checkpoint and a WAL
  restart by a plain connection, nothing lost). What such a connection
  writes while no sequencer runs is shipped at the next open, as the file
  whole. Open every writer with `hpplite=on` all the same: a plain
  connection can leave WAL mode or `ATTACH`, which the authorizer refuses
  on HPPLite's.
- **Everything that writes is replicated**, DDL included: `CREATE`, `ALTER`,
  `DROP`, the table rebuilds migration tools do, `VACUUM`, `PRAGMA
  user_version` and `application_id`. `changes()`, `last_insert_rowid()`
  and `RETURNING` report the application's statements as in plain SQLite.
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
or releases it when it is the last. The files belong with the database,
as the sidecar `<file>-hpplite.db` does (and need a local filesystem, as
SQLite's WAL does). Followers post nothing; the sequencer's flush posts
everyone's commits in WAL order (`tests/unit/test_concurrency.c`: threads
and processes writing at once, replica equal). Loading HPPLite twice into
one connection (static library plus module) is refused.

**Statically linked** with the SQLite built in this tree (what the tests use):

```bash
mkdir build && cd build && ../configure && make sqlite3.c   # from repo root
cmake -S ext/hpplite -B ext/hpplite/build_real -DHPPLITE_USE_REAL_L1=ON
cmake --build ext/hpplite/build_real
```

## Testing

```bash
# Unit tests (mock L1, fast, no network)
ctest                    # 43 tests, ~1 min: node, durability, page capture (the WAL reader,
                         # the pin, the page batch, the page mode), concurrency, reorg, blob
                         # I/O, cipher, envelope, keys, keystore, the store and the
                         # conformance vectors, the loadable extension, the crash harness
                         # (mock, store, directory), Python/CLI smoke, docs check.
                         # One capture, so each suite runs once (the _text/_pages twins
                         # went with row and statement capture, #253)

# Fault injection (#98): a random workload under injected failures - a post refused,
# a confirmation lost, a transaction pending past the timeout, a claim that reverts,
# a bookkeeping commit that fails as SQLITE_FULL, a reorg, the lease expiring - with
# the chain and a rebuilt replica as the oracle. In ctest with 60 seeds.
HPPLITE_FAULT_SEEDS=500 build_mock/test_faults          # more seeds
HPPLITE_FAULT_SEED=17 build_mock/test_faults            # one seed, verbose

# The same suite against SQLite trunk (the advisory sqlite-trunk CI job, #105): what
# the capture relies on that SQLite does not document is listed in
# doc/sqlite-dependencies.md

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
# sysbench-style OLTP through the loadable extension, per version (#140); see doc/performance.md
sh tools/bench_versions.sh

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
becomes an encrypted batch at a pin service within 90 s and the chain gets
one anchor per 30 minutes (#164, since 2026-11-03: `store=` and `anchor=30m`
on the VERSION 10 factory since 2026-11-04, page capture), a panel shows the ciphertext the
store holds while it waits for its anchor, with explorer links to the rollup
and the last anchor, and **Verify** rebuilds the database from the chain and
the store in a separate process and compares roots. `examples/demo/run_demo.sh`
serves it with `store=on` (HPP Sepolia's store, one anchor per 30 minutes,
paid from the node's funding wallet) and opens a Cloudflare quick tunnel (needs `cloudflared`, a real-L1 build and
`HPPLITE_PRIVATE_KEY`).

## Gas Costs (HPP Sepolia)

Measured on HPP Sepolia on 2026-11-01 with the VERSION 8 factory (DA
version 2, #33), where gas is 0.01 gwei (the chain's base fee; it is a
testnet, so there is no dollar price). Before #33 the same batch cost 658k:
the bytes went into contract storage at ~0.7k gas per byte.

| Operation | Gas | Cost (ETH @ 0.01 gwei) |
|-----------|-----|------------------------|
| Factory deploy (VERSION 8, optimizer on) | 3.40M | 0.000034 |
| Factory deploy (VERSION 10, 2026-11-04, `0xbce61e56…`) | 4.55M | 0.000045 |
| Rollup creation (`getOrCreateRollup`, `createRollup` for a named database; VERSION 10, the demo's on HPP Sepolia 2026-11-04; VERSION 8: 2.83M) | 3.43M | 0.000034 |
| Batch submit (932 B sealed, the demo's; ~24 gas per further byte) | 82.7k | 0.0000008 |
| Batch submit, deflated (#12): the demo's three timeline writes, 419 B sealed, measured 2026-11-01 | 73.9k | 0.0000007 |
| Batch submit (40 KB sealed, as cut on HPP Sepolia; from the per-byte cost) | ~1.0M | 0.00001 |
| Checkpoint | 50k | 0.0000005 |
| Lease claim / renew / release | 28-37k | 0.0000003 |
| Anchor (VERSION 9, #164: a range of batches at the store, root inside, 114-byte receipt; measured on HPP Sepolia 2026-11-03, the second anchor of a rollup; the first, its slots written for the first time, 124.8k) | 73.5k | 0.0000007 |

**Batch size on HPP Sepolia is set by the chain, not the contract.** The
contract allows 128 KB; the chain's RPC refuses more than ~84.9 KB of
calldata per transaction ("oversized data"), and a transaction may use at
most 32M gas (measured 2026-10-30; the block gas limit is 2^50, the
per-transaction cap binds), which with the storage DA of rollups before
version 8 meant ~44 KB. HPPLite knows both for HPP Sepolia (by chain id)
and cuts batches at 40 KB; elsewhere set `maxtx=` (bytes) and `maxtxgas=`,
or let it learn: a batch the RPC refuses as oversized, or whose gas estimate
is over the cap, is never sent, and later batches are cut smaller. Bulk
data costs ~0.00025 ETH per MB of sealed SQL at 0.01 gwei (was ~0.008).

**The number of batches is the cost, not their size.** A batch costs ~46k
gas before its first byte (the transaction, the contract's bookkeeping) and
24 gas per byte after, so a day of writes posted as 1,000 batches costs
about ten times what the same bytes cost as 100. HPP anchors to L1 every
30 minutes: a batch posted more often than that only waits in HPP's queue
until the next anchor, so `l1=hpp-sepolia` sets `interval=30m` unless the
URI says otherwise (48 batches a day at most from the timer; the half-limit
flush, #143, still posts a busy writer's batches as they fill). Shorter
intervals are for development, or for a replica that must follow within
seconds. A store (#125) with a chain commitment per range of batches (#162)
is the way to both: the bytes at the store as they are written, one
commitment per anchor.

| Usage | Batches/day | ETH/day on HPP Sepolia |
|-------|-------------|------------------------|
| Light (personal) | 10 | 0.000008 |
| Medium (small app) | 100 | 0.00008 |
| Heavy (production) | 1,000 | 0.0008 |

On a chain priced like Ethereum mainnet (10 gwei) every figure is 1000x,
and the bytes still live on the chain: the off-chain stores of #11 are the
next step down.

**Cost driver, and what #33 changed**: until version 8 `HPPLiteDA` kept
batch bytes in contract storage (`batchData[height]`), the most expensive
place to put them: ~0.7k gas per byte, the figures in the table above.
Since version 8 the bytes go into the transaction's log and storage keeps
only the commitment - the height, a rolling hash over every batch and the
block the log is in - so a batch costs about 21k gas for the transaction,
24 gas per byte (calldata and log data), and some 25k for the three slots:
the demo's 932-byte batch measured 82.7k gas on HPP Sepolia where storage
took 658k, eight times less, and the gap widens with size. A reader fetches
one block's log by the height topic (the chain's own block number: on an
Arbitrum-style chain such as HPP the contract asks ArbSys for it, #128),
which any archive node serves; pruned nodes cannot serve a rebuild.
Since #12 the batch is deflated before it is sealed (`compress=on`, the
default), on the chain and at a store alike (the deflated figure above was
measured on SQL, v0.3; today's batches are pages); the timer only flushes
when something is pending, so an idle database costs nothing.

## Components

| File | Description |
|------|-------------|
| `hpplite.c` | Lifecycle of a tracked connection (the sidecar's tables, the WAL hook and the trace of page mode), the connection registry, transparent API, extension entry point |
| `capture.c` | The authorizer on the application connection (#148): the `_hpplite_` prefix, `ATTACH`, `wal_autocheckpoint`, leaving WAL |
| `walread.c` | Reads a database's WAL file: a cursor over its frames, each commit's pages validated by salt and checksum and handed to page mode (#155) |
| `pagemode.c` | Page capture (#146): the queue of commits, the build of a page batch (whole pages, then XOR diffs against the last finalized image), the pieces of a large commit (#152), the file shipped whole |
| `pages.c` | The page root (a Merkle tree over the file's pages, `root_version` 4) and the page batch format (header, a record per dirty page, trailer with the root and a CRC64) |
| `bookkeeping.c` | The bookkeeping transaction on the sidecar and the foreign-write check |
| `finalize.c` | The meta row past a posted page batch, the posted copies kept for a reorg, the rows waiting for an anchor, the chain hash, the verifier's ring of roots, the checkpoint |
| `state_hash.c` | The content hash of a database (version 1: the schema and every row of every user table): what `hpplite-verify --db` compares and prints as the rebuilt state root; and the block hash (`sha256.c`) |
| `status.c` | `hpplite_status` table and the close sentinel |
| `cipher.c` | Batch encryption: ChaCha20-Poly1305, HKDF-SHA256 key derivation, envelope format, pluggable schemes |
| `node.c` | Node lifecycle, the sidecar connection, the file lock and the follower takeover, L1 sync and the apply of page batches, flush with retry, lease, anchors, snapshots, the soft tier |
| `batch.c` | Batch creation and serialization |
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
function createRollupFor(address owner, uint256 checkpointInterval, uint256 nonce, uint256 deadline, bytes sig)
    external returns (address);                                    // the owner's EIP-712 signature, anyone's gas (#171); createNonce(owner) is spent
function getRollupsByOwner(address owner) external view returns (address[] memory);

// HPPLite (per-rollup coordinator, VERSION 10)
function claimSequencer(bytes32 instanceId) external;
function renewLease(bytes32 instanceId) external;
function releaseLease(bytes32 instanceId) external;
function submitCheckpoint(bytes32 instanceId, uint256 from, uint256 to, bytes32 root) external;
// the same with the sequencer wallet's EIP-712 signature, submitted by anyone (#33, #126):
function claimSequencerFor(bytes32 instanceId, uint256 nonce, uint256 deadline, bytes sig) external;
    // renewLeaseFor, releaseLeaseFor alike; leaseNonce() is spent, refused after the deadline (#171)
function submitCheckpointFor(bytes32 instanceId, uint256 from, uint256 to, bytes32 root, bytes sig) external;
function setCheckpointIntervalFor(uint256 interval, uint256 nonce, uint256 deadline, bytes sig) external;   // the owner's (#171); ownerNonce()

// HPPLiteDA (per-rollup store, version 3): the bytes go into the log or a store, storage keeps the commitment
function submitBatch(bytes32 instanceId, uint256 height, bytes data) external;   // emits BatchSubmitted(height, keccak256(data), data, prevBlock)
function submitBatchFor(bytes32 instanceId, uint256 height, bytes data, bytes sig) external;
function commitBatch(bytes32 instanceId, uint256 height, bytes32 hash, bytes ref, bytes receipt) external;   // bytes held elsewhere (#11)
function commitRange(bytes32 instanceId, uint256 from, uint256 to, bytes32 chainHashAfter, bytes32 stateRoot,
                     bytes32 manifestDigest, bytes receipt) external;   // a range at the store, the anchor (#164); commitRangeFor with a signature
function chainHash() external view returns (bytes32);        // keccak256(abi.encode(prev, height, hash)) over every batch
function lastBlock() external view returns (uint256);        // the block of the last commitment; each event's prevBlock walks back
function getDAState() external view returns (uint256 lastBatchHeight, uint256, bytes32 chainHash);
```

HPPLite reads the coordinator's `VERSION` at connect and refuses one below
8 (#252): a rollup created before version 8 kept its batches in the DA
contract's storage, and v0.7 is the last release that reads it.

## Verification

A replica rebuilt from L1 compares the root of its file with the root
posted at each checkpoint (`roots_compared`, `root_mismatches`). Until the
first checkpoint (`checkpoint=`, default every 100 batches) there is no
posted root yet: compare `SELECT state_root FROM hpplite_status` of the
writer and the rebuilt file yourself, or use a smaller `checkpoint` in
development.

`hpplite_status` columns (43): `height` (next batch number), `pending`
(commits captured and not yet posted), `state_root` (the page root of the
file as of the last finalized batch), `prev_block_hash`,
`last_checkpoint_height`, `lease` (1 = this instance may post),
`encryption`, `keyed`, `contract`, `root_version` (4: a Merkle root over
the file's pages, #146), `da_contract`, `last_batch_tx`, `foreign_batches`,
`roots_compared`, `root_mismatches`, `last_refusal` (the last statement the
authorizer refused, and why), `sequencer` (1 = this connection posts the
file's batches; 0 = another connection, in this process or another, does),
`capture` (`pages`), `foreign_writes` (looks that found the bookkeeping
changed by something that is not HPPLite, #37), `state` and `last_error`
(#32), `chain_hash` (the DA contract's rolling hash over every batch as this
node tracks it, #33; NULL in a file from before it was kept), and, with
`store=` (#11): `store` (its URL), `store_state` (`ok`, `unreachable`, `over
cap`, `error`: the last put's outcome, so a waiting batch says
why), `store_spent` and `store_calls` (paid by x402 by this process in the asset's
atomic units, and the puts and gets that succeeded), `store_price` (what a
write of the last batch's size costs: the write fee and a month of its
storage; NULL when the store is free), `store_balance` (the funding
wallet's balance of the store's asset on the chain, read at most every
30 s; NULL without a paid store on this chain), `store_funder`,
`store_covered_until` and `store_bucket` (#214: the funding wallet's
address, when its account runs out at the current cost, from the last
bucket statement, and the bucket's state: `covered`, `parked`,
`deleted`), `anchored`
(the chain's last height, #164: above it the store alone holds the batches;
NULL without a chain), `last_anchor_tx` (the transaction of this node's
last anchor) and `last_checkpoint_root` (the root the last checkpoint or
anchor carried; a page-mode file keeps its bookkeeping in the sidecar, so
the row is the way to read it), `relay` and `relay_spent` (`relay=`, #126:
the URL and what this process paid it, in its asset's atomic units) 
`last_tx_path` (`relay` or `direct`: how the last write reached the chain),
and `spent_window` and `spend_warning` (#173: what the
store and the relay were paid in `spendcap=`'s window, a day without one,
and what runs out and when at the recent rate, NULL when nothing does), and
`soft_height` and `soft_disagreements` (#166: a verifier with a store, the
height its soft batches reach, with `height` staying the anchored one; and
how many times the chain committed another batch than the store served, each
followed by a rebuild), and `snapshot_height` (#221: the latest snapshot the
anchors name, NULL for none). `state` says what this node is doing about posting: `sequencing`,
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


The check is **rebuild and compare**: open an empty file with the
same key and `l1=`/`contract=`; every batch is fetched, opened and its pages
applied, and after each batch the root of the rebuilt file is compared with
the one the batch's trailer carries, and at each checkpoint with the posted
one (`batch N: the root after applying it differs from the posted one` on
stderr and in `last_error` when they differ, and the sync stops). The live
root of any database is `SELECT state_root FROM hpplite_status`.

**What the root is** (version 4, #146): SHA-256 over the page size, the
page count and the root of a Merkle tree whose leaves are
`SHA-256("HPP4" || page number || page)` for every page of the file, page
1 taken without its file-format bytes and change counters (a WAL-mode
sequencer and a rollback-mode replica hash to one root). The sequencer
keeps the tree in memory and updates one path per dirty page, so a root
costs what the batch changed; a node computes it from the file when it
opens. It is a function of the file's bytes, so two files with the same
pages have the same root, and a replica that applied the same batches
has the sequencer's. It is not a function of the content alone: the same
rows laid out differently (a `VACUUM`, a different page size) give another
root. The **content hash** (`hpplite_db_hash()` in C, `state_hash.c`, version
1: the schema and every row of every user table in rowid order) is that
other function, and `hpplite-verify --db` uses it to compare the live
database with the rebuilt one, two files whose pages differ while their
rows must not.

```bash
sqlite3> .load ./hpplite
sqlite3> .open 'file:rebuilt.db?hpplite=on&l1=hpp-sepolia&keyfile=/path/to/key'
sqlite3> SELECT height, state_root, roots_compared, root_mismatches FROM hpplite_status;
```

**`hpplite-verify`** (#5) packages this as a command:

```sh
HPPLITE_PARAMS="l1=hpp-sepolia&keyfile=/path/to/key" hpplite-verify --db /path/to/app.db
```

It opens an empty file with `role=verify` and the application's own
parameters (`l1=`/`rpc=`/`factory=`/`contract=`, the key, `dbname=`), rebuilds
from the chain and the store, compares every posted root with the rebuilt
file's page root, and compares the root the contract anchors at its last
checkpoint with the rebuilt file's root at that height. It prints the
rebuilt file's content hash too (`rebuilt state root`, version 1): that is
the hash `--db` compares the live database with, and when they differ and
nothing is pending, it names the tables that differ. Exit 0 verified,
1 not (a root mismatch, a sync that stopped short, a live file that
differs), 2 could not run. On a network with a canonical store (`l1=hpp-sepolia`)
the tool takes `store=on` itself when the parameters name no store: the bytes,
and the databases' names, are there; another store is named with `store=` as
usual. Without a store on a rollup whose bytes are at one, the tool says so and
exits 2 (a gap in the parameters, not a verdict), after reporting what the chain
anchors (#247). The rebuilt file is deleted unless `--keep <dir>`.
`--to <height>` stops after that batch (#206): with `--keep`, the database as of that
height stays, its own posted root matched; checkpoints past it are not compared.
`--at <time>` (unix seconds or `YYYY-MM-DDTHH:MM:SSZ`) is point-in-time recovery (#205):
every batch its writer wrote by then, by the time each batch carries; a transaction in
flight at that moment is completed, never left halfway; with `--keep`, the database as of
that time stays.
A history pruned at the store (`keep=`, #223) has no batch 1 to read: the
rebuild then starts from the oldest snapshot the last anchor still names
(`from=earliest`, #224) - its root checked against the one the chain anchored
at its height - replays the batches after it, and says `verified from the
snapshot at <h>`; `--to`/`--at` before it cannot be met, and it says so.
What that line claims, exactly (#232): the state at `h` is the one the chain
anchored at `h` (the snapshot's root and chain hash match the range
commitment ending there), and every batch after `h` was replayed and its
root checked. What it does not claim: how the rollup got to `h`. That
history is gone from the store, so a replay from batch 1 - the stronger
check, every step since genesis - is only possible on a rollup without
`keep=`, and `hpplite-verify` does it whenever batch 1 is there. The two
defaults follow from this: a replica starts from the latest snapshot (it
wants the state, fast); a verifier starts from batch 1 unless `from=` says
otherwise (then the report says `started from the snapshot at <h>`, and
claims nothing about a prune), and falls back to the oldest snapshot kept
only when batch 1 is pruned.
A verifier never posts, never claims the lease and never creates a rollup;
`role=verify` is also usable from any connection that only wants to audit.
It needs the wallet key or a viewing key the writer granted (batches are encrypted; see Keys).
There is no fraud proof or challenge period: the chain holds the log and
the roots, the key holder is the one who checks them.

## Stores and commitments

Three words the rest of this document uses precisely (#35, #11):

- A **store** is where the bytes of a batch live. Today that is the rollup's
  DA contract on the chain, a directory, or an `hpplite-store/2` store (#11,
  #214).
- A **commitment** is what the chain holds per height: HPPLite's own hash
  of the batch and a reference into the store (on chain, the height itself;
  later a CID or an object key), and, with each anchor, the store's signed
  bucket statement (#214). A replica reads the commitment, fetches
  by the reference, checks the hash, replays. Verification never depends on
  what the reference means.
- **Data availability** is a property, not a place: anyone can fetch the
  bytes needed to rebuild, without the writer, for as long as the chain
  lives. The on-chain store has it by construction. A store's bucket has
  it while the bucket is covered (#207), as its signed statement on chain
  says, with the fallback to posting on chain for when the store is down. A directory has none: it is a store with no availability claim.

### Ranges: the store in seconds, the chain per anchor (#164)

On a chain whose contracts take ranges (VERSION 9 and later, DA version 3:
the `hpp-sepolia` factory since 2026-11-03), a node with `store=` does not
commit each batch. A batch is finalized once the
store has it (its height moves on, `pending` drops, a row waits in
`_hpplite_unanchored`), and the **anchor**, every `anchor=` (30 min on HPP,
the chain's own cadence), at close, and on `SELECT hpplite_anchor()`, writes
a **manifest** to the store naming every batch since the last anchor (height,
hash, reference, receipt) and commits the range in one transaction: the
chain hash after it (the same fold the per-batch path keeps), the state root
after its last batch (the anchor is the checkpoint, and renews the lease: a
node that anchors sends no checkpoint or lease transactions of its own), the
manifest's sha-256 and the store's signed bucket statement (#214). A
replica reaching a height inside a range fetches the manifest by its
digest's CID, refuses one whose batches do not fold to the anchored chain
hash, and fetches each batch by the manifest's reference, checked against
its hash as before. `hpplite_status.anchored` is the chain's height; above
it the store alone holds the batches, for one anchor interval at most. What
a run leaves at the store, the next run anchors first.

With `snapshot=` (page capture, #221), an anchor whose last snapshot is
older than that makes a new one: the database as of the anchored height,
taken from the images the page mode keeps after the last finalized batch
(never a read of the live file, so it is that height's state exactly),
encoded as a page batch with every page whole, compressed, and sealed as a
batch at that height is (the same recipients: the wallet, the owner, the
readers). It goes to the rollup's bucket in chunks of 8 MiB, then an index,
`{v, height, root, chain, block, at, bytes, chunks: [{ref, sha256}], name}`: the
root and chain hash are the ones the range commitment ending at that height
carries. The manifest names it, `"snapshot": {height, index, at, sha256}`
(the index's hash: whatever store serves it, a reader checks it, and each
chunk by its own), and every later
manifest names the latest one, so the last anchor always does; a restart
finds it in the last anchor's manifest. An anchor the chain refuses drops
the snapshot it made. Each chunk is a write at a paid store.

A new replica (`from=snapshot`, the default but for a verifier, #222)
starts there: it reads the snapshot the last anchor's manifest names,
checks the index against the manifest's hash and each chunk against the
index's, the root and chain hash against the range commitment ending at
that height on the chain, and the image's root before writing anything;
then the file is the image, the bookkeeping is at that height (the root a
checkpoint), and the sync goes on with the batches after it - a replica of
a long history reads one image and an anchor interval or so of batches. A
snapshot that fails a check is said on stderr and the replica replays from
batch 1. `snapshot_height` in `hpplite_status` is the one it started from.

With `keep=<duration>` (#223) the history is bounded: the manifests also
name the older snapshots still kept (`"earlier": [...]`, oldest first),
and at each anchor the sequencer signs a `Prune` (hpplite-store/2) at the
height of the newest snapshot an earlier anchor named that is at least
`keep` old - decided before that anchor's manifest is written, so it names
only what is kept (#227), sent once the chain has accepted the anchor, so
an anchor the chain refuses leaves the store as the manifest on chain
describes it (#238) - and the store deletes the bucket's batches, names
and older snapshots below it, never on its own judgement. Batch 1, which
names a `dbname=` database (#160), goes too: the index's `name` - the name
sealed like a batch, as a page batch with no pages - is where a named
lookup finds it then (#226). The bucket then holds the database about once per
snapshot kept plus the window's batches. With `keep=all` (the default)
nothing is pruned and snapshots only speed replicas up - and each one
stays in the bucket, paid for, the replaced ones named by nothing (#242):
`snapshot=` without `keep=` multiplies the bucket by the number of
snapshots made, so give it a `keep=` unless the history must stay whole.

Each batch is also stored **under its rollup's name**, `<da contract hex>-<height>`,
with the sequencer's EIP-712 `Batch` signature appended (#165: the one
`commitBatchFor` takes, in the DA contract's domain, over the instance, the
height and the bytes' keccak), so a reader finds batches by height before the
anchor and knows who claims them; `hpplite_store_batch_signer()` recovers the
wallet. A `hpplite-store/2` store takes a name only into the rollup's bucket,
from the sequencer the chain names and the instance its lease names. The hard
tier does not depend on the name: a store that refuses it is said once on stderr.

**Recovery (#167).** A sequencer that dies with batches at the store and not
yet anchored loses none of them: the next sequencer with the same key, on
the same file or a fresh one on another machine, replays the chain, then
reads the names past the anchored height, takes each batch whose signature
recovers to its own wallet and that applies on its history, and anchors them
with its own. It stops at the first height without a name, or at one signed
by another wallet. What is lost is what never reached the store: at most one
flush interval. The successor waits out the dead one's lease (an hour since
VERSION 9) before it can anchor; its writes are captured meanwhile.

**The soft tier for a replica (#166).** A `role=verify` connection with
`store=` doesn't wait for the anchor. Past the anchored height, it reads the
batches the store holds under the rollup's names, as they arrive. It takes
each one signed by the coordinator's `sequencerWallet()` that applies on its
history, so it follows the sequencer within a flush interval. `height` stays
the anchored, hard height; `soft_height` is where the replica is with the
soft batches included. When an anchor lands, its range commitment must name
the chain hash the replica computed at that height, and if it does, the soft
batches up to it are hard.

If it names another hash, the store served batches the chain didn't commit:
a sequencer that equivocated, or a store that lost a batch and a successor
that wrote another at that height. The replica says so and rebuilds itself:
a second verifier rebuilds it from the chain and the store in
`<db>-rebuild.db`, and the result is copied into the replica's file: the
pages as they are (not through `sqlite3_backup`, which would move the
schema cookie the next diff of page 1 lands on), with page 1's change
counter moved so the replica's readers notice, and the sidecar by
`sqlite3_backup`. `soft_disagreements` counts these events. `soft=off` keeps
a replica at the anchored height.

`hpplite-verify` verifies the hard tier as before. It then reports the soft
tier ("stored to S, anchored to H"), or nothing past the anchor.

What it costs: one anchor on HPP Sepolia is 73.5k gas (the first of a
rollup 124.8k, its slots written for the first time), about what one
per-batch commitment cost, so a database writing all day pays 48 anchors instead of 17 000
commitments. A store that is down holds the anchor too: the fallback to
posting bytes on the chain applies only while nothing waits for an anchor,
since the chain takes only the next height. The lease lasts an hour by
default since VERSION 9 (`setLeaseDuration`, the owner's), longer than the
anchor interval; a sequencer that dies without releasing is replaced after
that long. `l1=file://` keeps the single cadence.

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

### No ETH: a relay (#126, #170)

With `store=` and `relay=`, a node needs no ETH at all: its wallet holds
USDC.e, the store is paid per put and the relay per transaction, both over
x402. The relay ([operators/relay](operators/relay/README.md)) takes each
write as the contract's signed form: `createRollupFor` at the first open
(VERSION 10 factories), `claimSequencerFor`/`renewLeaseFor`/`releaseLeaseFor`
(the lease signature carries a ten-minute deadline), `commitRangeFor` per
anchor, and the others where they apply. It pays the gas and settles the
payment once the transaction has landed. The contracts check every
signature, so a relay cannot forge, alter or misattribute a write; it can
only delay one. If it doesn't answer, an anchor waits with the relay's
reason in `last_error` and goes through at the next anchor; a wallet that
holds ETH sends the plain call itself instead. On anvil (scenario 27) a
wallet with MockUSDC and 0 ETH creates its rollup, writes, anchors and
releases, and a replica rebuilds.

### A storage provider: `operators/store/`

The reference store (`hpplite_store.py`, #213) speaks `hpplite-store/2`
([doc/store-protocol.md](doc/store-protocol.md)): a **bucket** per rollup,
named by its DA address, kept while its **funding wallet** covers it - no
per-item expiry, nothing to renew. The node binds the bucket with its own
key (the rollup's sequencer) and its funding key (derived from it), gives
the store an EIP-2612 permit (`storepermit=`), and writes: each batch's
bytes and its name, the anchors' manifests, each signed for the bucket.
Storage (bytes held x time) and a fee per write accrue to the funding
wallet's account; the store pulls what is owed with the permit, so the
bucket stays covered while the node is off for months. Without a permit
(`storepermit=0`), a write the account cannot cover answers 402 and the
node pays a top-up by x402 from the funding wallet, up to `storecap=`. A
refused pull (the wallet empty, the permit spent or revoked) parks the
bucket - not served, restorable by paying - for 30 days, then deletes it;
the store's own failures never do. The funding wallet needs no ETH: top it
up by sending USDC.e to `store_funder`. One Python file, standard library
only; ctest runs it free and without a chain (`test_store`), and on anvil
with real rollups, the permit and x402 (`test_store_buckets`, anvil
scenarios 19, 20 and 30).

With each anchor the chain gets the store's signed **bucket statement**
(138 bytes: version 2, the store's signer and domain, the heights held,
covered until, signature), checked against the signer the store publishes
before it is committed. A replica with the same `store=` reads the
commitment, fetches by the CID (a name's bytes are content too), checks
the hash, replays; one without says `batch N is in a store (...) and no
store= names one` and rebuilds nothing. A store that does not answer never
holds a batch back: the chain carries the bytes meanwhile
(`storefallback=on`, the default); anvil scenario 21 runs it with the store
down. A store you run yourself is `operators/store/hpplite_store.py` in
front of your own disk, at a price of zero: the same protocol, names,
bucket statement and snapshots as the canonical one (`store=kubo://` and
`store=s3://`, which took no payment, no names and gave no statement, were
removed in v0.8, #250).
`tools/topup.sh AMOUNT` tops the wallet up: it bridges that many USDC from
Ethereum Sepolia to USDC.e on HPP Sepolia through HPP's gateway with `cast`
(approve, then `outboundTransfer` with a retryable ticket it prices from the
chain; `--dry-run` prints the two transactions and sends nothing, `--to`
sends to a node's wallet). Its price is per MiB-month (default $0.03 per GiB-month)
plus a small fee per put, and the node's `store_price` is exactly what the next
402 will ask (#172; the costs in [its README](operators/store/README.md)).

## Versions

Scope is tied to versions. v0.1.1 is the last of the v0.1 line (end of
life). **v0.2.0 is tagged** (2026-11-01): row capture by default, `hpplite
verify`, several writers per file, the fault harness as the acceptance
test, an inventory of the SQLite behaviours the capture relies on with a CI
build against SQLite trunk. **v0.3.0 is tagged** (2026-11-02): batches
deflated, the state root kept as per-table Merkle trees so a checkpoint
costs what changed, the bytes of a batch in a store (a pin service paid
over x402, a Kubo node, an S3 bucket) with the commitment on the chain, the
status row saying what the store costs, the soak running through a store
too. Key roles and viewing keys moved to v0.4.
**v0.4.0 is tagged** (2026-11-03): key roles and provisioning (#17),
viewing keys and the public-key envelope (#15), page capture (#146) as the
default for a new rollup, the application suites (WordPress, Ghost, Rails 8,
Home Assistant) in containers and run under it, the OLTP benchmark per
version (#140) and the three writer stalls it found, fixed. **v0.5.0 is
tagged** (2026-11-04). It covers HPP economics:
- **Persistence and commitment (#162):** the store in seconds (soft), one
  range anchor every 30 minutes (hard); replicas that follow the soft tier
  and rebuild on a disagreement (#166); recovery of a lost sequencer from the
  store (#167).
- **Payments:** the store and the relay paid by voucher over x402's
  batch-settlement (#135, #179; channels removed again in v0.8, #251: the
  store is paid by permit, the relay per call); store pricing per MiB-month (#172).
- **A node that holds only USDC.e (#126):** its transactions go through a
  relay (#170) on factory VERSION 10 (#171).
- **Spending caps and low-balance warnings (#173).**

**v0.6** brought the operators (#181: the canonical store and relay, their
protocols and conformance vectors) and **v0.7** the buckets paid by permit
(#214) and snapshots with `keep=` (#203). **v0.8 is "One capture"**: the
persona review of 2026-11-08 took out what no deployment used. Pages are
the one capture (#253): row and statement capture, `capture=` and
`determinism=`, the clock VFS, the per-table trees and the in-file log are
gone, and with them the refusals of non-deterministic SQL; a v0.7 file
whose history is rows or statements is refused at open, and v0.7 is the
last release that reads it. Also gone: the `kubo://` and `s3://` store
kinds (#250), the x402 batch-settlement channels at the relay, which is
paid per call (#251), the factories before VERSION 8, refused at connect
(#252), the HPE1 envelope and the `hpplite-store/1` per-item receipt
(#249); `role=` takes `verify` and nothing else (#248), and
`hpplite-verify` exits 2 on a rollup whose bytes are at a store the
parameters do not name (#247).

Paying in the HPP token itself is open (#175).

| Version | Scope | Database size | DA |
|---------|-------|---------------|----|
| **v0.1** | Library + loadable extension on stock SQLite, crash-safe local file, reconstruction from L1, batches cut at the DA limit, root at checkpoint cadence, encrypted batches, one writer process | per-tenant / single-tenant, up to ~100 MB | on-chain (contract storage) |
| v0.2 | Row capture by default (#19), validated on suites and real applications (WordPress, Ghost, Rails 8), several writers per file, `hpplite verify` (#5) | same | same |
| v0.3 | Off-chain stores with L1 commitments (#11: pin service over x402, Kubo, S3), incremental (Merkle) state root (#9), batch compression (#12) | GB range | a store + L1 commitments |
| v0.4 | Key roles and provisioning (#17), viewing keys and the public-key envelope (#15), page capture (#146: the pages each commit wrote, a Merkle root over the file's pages, the default for a new rollup) | same | same |
| v0.5 | HPP economics: store-based persistence and soft commitment, one chain anchor every 30 minutes (#162, #164-#167); replicas on the soft tier (#166); a node with USDC.e and no ETH through a relay (#126, #170); store and relay paid by voucher (#135, #179), spending caps (#173) | same | a store (soft) + one L1 commitment per 30 min (hard) |
| v0.6 | Operators (#181): the canonical store and relay, `store=on` / `relay=on`, the protocols and their conformance vectors | same | same |
| v0.7 | Buckets paid by the funding wallet's permit (#214), snapshots and `keep=` (#203) | same | same |
| v0.8 | One capture (#253): pages, nothing else; the persona review's removals (#247-#252) | same | same |
| v1.0 | Frozen batch / meta / URI formats, language bindings | — | — |

Why the size bound in v0.1: its state root was a content hash, O(database
size) per computation (~9 ms/MB), computed once per checkpoint interval;
v0.3's per-table trees and v0.4's page root (one path per dirty page) lifted
it. See [doc/architecture.md](doc/architecture.md#state-root).

v0.1.x is end-of-life and v0.2-v0.7 with it as far as their histories go:
a rows or statements history is read by v0.7 and nothing later (#253), so
a database that must keep one stays on v0.7.

Issues are grouped by [milestone](https://github.com/hpp-io/hpplite/milestones).

## What is refused

The pages each commit wrote replicate whatever SQL produced them, so
nothing is refused for replay's sake: `random()`, the clock and
`'localtime'`, functions the application registered, `last_insert_rowid()`
in a write, virtual tables (FTS5, R*Tree), triggers and foreign-key
cascades, `PRAGMA` connection state, incremental blob writes
(`sqlite3_blob_write`, Python's `blobopen`), transactions and statements of
any size, `VACUUM` - all of it lands in the file as pages and ships as
pages. What the authorizer on the application's connection refuses, with
`SQLITE_AUTH` (`not authorized` to the driver: in Python an
`OperationalError`, in Node an `Error` with that text), a line on stderr
and the reason in `hpplite_status.last_refusal`:

| Refused | Why | Do this instead |
|---------|-----|-----------------|
| `ATTACH` of a named database | writes to another file are not captured (the unnamed one `VACUUM` attaches to copy through is let past) | one database per file |
| writes that name `_hpplite_*` tables, or creating one (`CREATE TABLE _hpplite_x`, a trigger on one, `RENAME TO _hpplite_x`) | the prefix is HPPLite's (#18) | any other name |
| `PRAGMA wal_autocheckpoint` | the connection's WAL hook is the capture's; the pragma would replace it with SQLite's own, and HPPLite checkpoints the file itself (#155) | leave it to HPPLite |
| `PRAGMA journal_mode` to anything but `wal` | the WAL frames are what is captured | the file stays in WAL |

```sql
SELECT last_refusal FROM hpplite_status;
-- ATTACH: writes to another file are not captured: /tmp/other.db (the driver reports "not authorized")
```

Two things are refused at open, with the reason in `sqlite3_errmsg()` and
on stderr: an in-memory database (`an in-memory database has no WAL to
capture: HPPLite needs a file`), and a file from v0.7 or earlier that
carries HPPLite's tables in itself (`this file's history is statements or
rows (v0.7 and earlier), which this release does not read (#253): v0.7 is
the last release that does; a page-capture database starts from a fresh
file`). A replica whose chain holds a batch that is not a page batch stops
with `batch N is not a page batch`. A URI or environment that still names
`capture=` or `determinism=` refuses the open as well, saying both are gone.

## Keys

One wallet key does everything by default: it owns the rollup contract, posts
batches (the sequencer) and the batch cipher key is derived from it. The
roles can be split (#17): the **owner** (cold: `transferOwnership`,
`setCheckpointInterval`, `forceExpireLease`, naming the sequencer) names a
**sequencer** wallet (hot: the lease, batches, checkpoints) with
`SELECT hpplite_set_sequencer('0x...')` on a connection keyed with the owner's
key; the running lease expires, the owner's own node posts no more, and a
node keyed with the sequencer's key (`privkey=`, a keystore, or a signer)
posts on the rollup named with `contract=`. The sequencer's key derives a
different batch cipher key, so the owner hands the **batch key** over
(`SELECT hpplite_batch_key('0x...')` on the sequencer's node, 32 bytes: what
`hpplite_cipher_derive_key()` gives for the owner's key, or any key chosen
when the rollup starts) and every replica with the owner's key still reads.
Viewing keys that read without any power to act are #15. A key reaches
HPPLite before open, in order of precedence:

| How | Use for |
|-----|---------|
| `keyfile=/path` or `HPPLITE_PRIVATE_KEY_FILE` | **production**: a file with the hex key, mode `0600`; group/world-readable files are refused (like ssh) |
| `HPPLITE_PRIVATE_KEY` | containers / process managers |
| `privkey=0x...` in the URI | **development only**: SQLite keeps the query string next to the filename, so anyone with the `sqlite3*` can read it back via `sqlite3_uri_parameter()`, and URIs end up in logs |
| `keystore=/path.json` or `HPPLITE_KEYSTORE`, passphrase in `HPPLITE_KEYSTORE_PASSWORD`; or `SELECT hpplite_keystore('/path.json', 'passphrase')` after open | **an Ethereum keystore V3 file** (#17): what `cast wallet new`, geth and MetaMask exports write; scrypt or PBKDF2-SHA256, AES-128-CTR, the Keccak MAC checked before anything is used (a wrong passphrase is refused as such). Vendored, like the batch cipher. |
| `hpplite_node_set_signer()` (C API) with `SELECT hpplite_batch_key('0x...')` | **the sequencer key in a KMS or HSM** (#17): a callback signs the lease, batch and checkpoint transactions, the key never in the process; the batch cipher key cannot be derived from it, so it is given apart (32 bytes), or `encrypt=off`. A signer creates no rollup (create it once with the key, or name `contract=`) and pays no store. |
| `viewkey=0x...`, `HPPLITE_VIEW_KEY`, or `SELECT hpplite_viewkey('0x...')` after open | **a reader's viewing key** (#15): a secp256k1 private key whose public key the writer granted; opens batches sealed to it, rebuilds and verifies, signs nothing. See Privacy. |
| `ownerkey=0x02...` (or `HPPLITE_OWNER_KEY`) on a sequencer that is not the owner | **the owner's public key** (#209, 33 bytes compressed): every batch is sealed to it too - a recovery key no node holds - so the owner reads the whole history with its own key (a verifier with `privkey=` the owner's), whichever sequencer wrote it and whatever readers were granted. A successor sequencer that must read its predecessor's batches is given the owner's key as its `viewkey=`. |
| `SELECT hpplite_key('0x...')` or `hpplite_keyfile('/path')` after open | **applications that hold keys themselves** (#17): the SQLite-extension idiom, nothing in the URI or the environment. Until then the connection captures and posts nothing; with the call the node is keyed, a chain named at open is connected and its rollup resolved, the lease taken, and the next flush posts. The same file rules as `keyfile=`. A different key while keyed is refused (rotation is not this). |

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
makes `sqlite3_open_v2()` fail rather than open without capture, and so does
a file this SQLite cannot read: one encrypted at the page level (SQLCipher,
SEE) is refused at open with a message that says so (#139 is the support
for keying HPPLite's own connection; until then, HPPLite runs on files the
library it is loaded into can read). Keys can
also be given after open (`hpplite_key()`, `hpplite_keyfile()`, above). v0.4
splits the roles (owner / sequencer / data / viewing keys) and adds Ethereum
keystore V3 files and KMS-held signers - see issues #17 and #15.

## Privacy

Everything posted to L1 is public, so batch payloads are **encrypted before
posting** whenever a private key is present (`encrypt=off` or
`HPPLITE_ENCRYPT=off` to disable). The scheme is ChaCha20-Poly1305 with a key
derived from the wallet private key (`HKDF-SHA256`), so whoever can write the
rollup can read it and there is no extra secret to manage; reconstruction and
verification work from the same `privkey=`. Each batch is sealed with a fresh
nonce and bound to its height. `SELECT encryption FROM hpplite_status` shows
the active scheme; schemes are pluggable (`cipher.h`).

**Readers and viewing keys** (#15): `SELECT hpplite_grant('0x<33-byte
compressed public key>')` on the writer names a reader; from the next batch
series on, batches are sealed to the wallet and to every reader (envelope
`HPE3`: one random data key per series seals the payload, wrapped once per
recipient with an ephemeral secp256k1 key, ECDH and HKDF-SHA256; recipients
are named by a 4-byte fingerprint of their key). A reader opens them with
its own private key alone, `viewkey=0x...` (or `HPPLITE_VIEW_KEY`, or
`SELECT hpplite_viewkey('0x...')` after open), and can rebuild and verify
with `role=verify` but never sign, post or claim: read without the power to
act. `hpplite_revoke()` ends it at the next series; history stays as it was
sealed, readable by whoever could read it, and a reader granted later cannot
read what came before (sealed under the symmetric key). The wallet is always
a recipient, so `privkey=` alone still rebuilds everything.
`hpplite_status.readers` counts them. A node whose key lives in a signer
(#17) seals with its batch key and no readers.

What stays public on L1: heights, batch sizes and timing, batch hashes, the
state roots in checkpoints (content hashes that reveal *when* content
changed, not what), and, with readers, how many there are and their
fingerprints.

## Durability

HPPLite writes nothing into your database file. What it keeps is in a
sidecar beside it, `<db>-hpplite.db`: the height, the roots, the batch in
flight (height and root, so a submit the process did not survive is
recognised at the next open), the posted copies kept for a reorg, the rows
waiting for an anchor, the readers. The commits themselves are in the
database and its WAL, where SQLite put them; the capture reads them from
there and the sequencer's pinned read transaction keeps SQLite from
checkpointing a frame away before it has been read. Kill the process at
any point and reopen: the sidecar says where the chain is, the file says
what it holds, and what the file holds beyond the chain is shipped, whole
if need be; nothing is posted twice. A batch that L1 does not confirm stays
pending and is retried at the same height.

File databases are switched to WAL mode on open and get a 5 s busy
timeout; the sidecar is a second connection. The application's WAL hook is
HPPLite's: it reads the commit's frames, and at 1000 frames - where
SQLite's own hook would checkpoint - it runs the sequencer's WAL upkeep
instead (the pin moved on, the WAL polled, a passive checkpoint on a plain
connection of the node's own, once a second at most), so the application's
commit never pays for a checkpoint and the pins keep the frames a batch
still needs; that is why `PRAGMA wal_autocheckpoint` is refused (#144, #155).
A write that cannot take the write lock within the
timeout (another process holds it) fails with `SQLITE_BUSY` after one wait,
as it would without HPPLite. An in-memory database is refused at open:
it has no WAL to capture.

See [doc/architecture.md](doc/architecture.md#local-durability).

## Trust model

The sequencer is your process; it is trusted to execute SQL. What the chain
adds is a history it cannot quietly rewrite: batches are posted in order
under a lease, roots are anchored, and anyone holding the key can rebuild
from the chain and the store and compare. Nothing here punishes a divergent
sequencer - there are no funds, bonds or fraud proofs here - it makes the
divergence detectable, and a lost disk recoverable. The lease keeps two
copies of your process from posting at once, and only the rollup's
sequencer wallet (the owner, unless set apart) can take it - a rollup can
sleep for days and nobody else can claim it. A batch posted by someone
else (it fails our authentication, so it is provably not ours - a rotated
sequencer's, #208) is stepped over by every key holder the same way instead
of stopping reconstruction.

**Soft and hard.** A batch at the store carries the sequencer's signature
over its height and bytes, and the store's signed bucket statement. That is the
sequencer's claim, checkable by anyone with the key against the wallet the
coordinator names, but not yet settled. Two signed batches at one height are
proof of equivocation. A pin service with `--rpc` refuses names from an
instance that doesn't hold the lease, so a partitioned old sequencer can't
write over its successor.

The anchor settles the claim. A replica on the soft tier that finds the chain
committing something else says so (`soft_disagreements`) and rebuilds itself
to what the chain committed. What the anchor rules out is the sequencer
changing its story at all.

See [doc/architecture.md](doc/architecture.md) for details.

## License

See parent SQLite project for license terms.
