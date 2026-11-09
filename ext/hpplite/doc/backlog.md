# HPPLite Backlog

## Completed

### v0.1 hardening (issues #20-#23, #29, #30; persona reviews)
- Lost captures under the bookkeeping thread (#20): the flag that hides
  HPPLite's own statements from the trace is never touched from the timer
  thread; bookkeeping and replay run on their own connection
- Capture is fail-closed: a statement whose log row cannot be written, that
  is too long for one DA batch, or whose parameters cannot be expanded is
  refused with `SQLITE_INTERRUPT`; `_hpplite_` inside a string literal is
  data; `without rowid` in any spelling hashes
- Lease (#21, client side): read before every submit, renewed past half
  life, never posted into a foreign lease, exponential backoff on failures
- Checkpoint cadence survives restarts (#22); registry mutex, node-lifetime
  flush mutex, once-only curl/secp256k1 init (#23); undecodable batches and
  halted sync are reported (#29); envelope truncation OOB read (#30)
- `hpplite=on` that cannot be honoured fails the open; a second copy of
  HPPLite in one connection is refused; `:memory:` never gets a timer; a
  configured-but-unreachable L1 keeps batches pending
- `tools/linux_test.sh`: build + ctest on Ubuntu in a container (plain,
  asan, tsan, stress); first run found the malloc/sqlite3_free mismatch on
  tx hashes that macOS tolerated

### M1: Single-Process State Tracking
- Core state tracking (`hpplite.c`)
- Batch serialization (`batch.c`)
- Crypto signing with secp256k1 (`crypto.c`)
- Node module (`node.c`)
- L1 mock implementation (`l1_mock.c`)

### M2: L1 Integration
- HPPLiteDA.sol contract with on-chain DA
- HPPLiteFactory.sol for one-click rollup deployment
- Web3 primitives (`eth/` - RLP, ABI, keccak256)
- Ethereum JSON-RPC client (`l1_eth.c`)
- EIP-191 signature verification
- Batch submission to L1
- State reconstruction from L1 batches

### M3: Transparent SQLite API
- `sqlite3_close_hook` for cleanup on close
- Auto-extension for `?hpplite=on` URI parameter
- L1 auto-connect from URI parameters (`rpc`, `contract`, `factory`)
- Factory auto-deploy on first open
- Timer thread for time-based batch flushing

### Zero-patch extension (issue #2)
- `sqlite3_close_hook` core patch replaced by the `hpplite_status` eponymous
  virtual table: its `xDisconnect` runs inside `sqlite3_close()` on stock SQLite
- Loadable module `hpplite.dylib` / `.so` (`.load ./hpplite`), entry point
  `sqlite3_hpplite_init`, pinned in memory, auto-extension for later connections
- `HPPLITE_SQLITE_SRC` to build against any amalgamation; `test_loadable`
  links only stock SQLite and loads the module

### Several connections to one file (issue #24, v0.1 slice)
- Transaction ids come from the log rowids (unique across connections and
  processes); later `hpplite=on` connections in a process are capture-only
  followers of the first; the sequencer posts everyone's rows
- Left for v0.2: follower promotion when the sequencer closes, a
  `sequencer` status column, cross-process `flock` sidecar

### Crash harness and chain-free mock (issue #26)
- `l1_mock` can live in a directory (`hpplite_l1_create_mock_dir`): batches,
  lease, checkpoints and clock survive the process (atomic rename); the seed
  of the directory backend `l1=file:///path` (`l1_dir.c`, #35), which the
  harness also runs against (`crash_hpplite --l1 dir`, ctest `test_crash_dir`)
- `crash_hpplite` (ctest `test_crash`): the writer dies at
  `HPPLITE_TEST_CRASH_AT=after_submit|after_finalize|mid_series` and at random
  moments under the timer; a fresh process reopens and checks every statement
  is on L1 or pending exactly once, flushes, rebuilds from L1 and compares roots

### Stress test and determinism
- `tests/stress/stress_hpplite.c` (`make stress`): 100 MB through encryption,
  DA chunking, checkpoints with roots, reconstruction compared by root
- Capture counts non-deterministic statements (`random()`, `'now'`, ...);
  row-level capture is #19 (v0.3)
- `checkpoint=` URI / `HPPLITE_CHECKPOINT_INTERVAL`; the integration test now
  anchors a real checkpoint root on HPP Sepolia and compares it with the
  local content root

### Keys in v0.1 (issue #16)
- Key file loader refuses group/world-readable or non-regular files, accepts
  `0x`/bare hex with whitespace; a node without a key never posts (plaintext
  requires explicit `encrypt=off`); `hpplite_status.keyed`
- `test_keys`: URI / env / keyfile on both open paths, precedence, rejections,
  no-key behaviour, encrypt=off
- v0.2: #17 key roles, runtime `hpplite_key()`, keystore V3, KMS signer,
  SQLCipher page-key passthrough; v1.0: #18 shadow tables + defensive mode

### Encrypted batches (issue #14)
- `cipher.c`: ChaCha20-Poly1305 (RFC 8439 vectors in `test_cipher`), HMAC/HKDF-SHA256,
  binary envelope bound to height; key derived from the wallet key; on by default,
  `encrypt=off` / `HPPLITE_ENCRYPT=off`; plaintext history still replays
- Reads go through `hpplite_node_fetch_batch()` (opens the envelope); a node
  without the key sees ciphertext and syncs nothing
- Release build type is now the CMake default

### Microbenchmark
- `tests/bench/bench_hpplite.c` (`make bench`), reference numbers in
  `doc/performance.md`; found the O(n^2) size-limit cut and the
  failed-statement-in-transaction capture hole

### v0.1 wrap-up (issues #10, #13)
- State root at checkpoint cadence: only the interval-closing batch carries
  `state_root`; the flush loop submits the checkpoint automatically (before,
  nothing in production ever called `submit_checkpoint`)
- Examples run in the integration suite; found and fixed `l1=` alias not
  applying the factory on the `sqlite3_open_v2` path (the README quick-start)

### Batch size limit and test coverage (issues #7, #8)
- Batches are cut at the DA `batchSizeLimit` (`partial` series); early flush
  at half the limit; oversized single statements reported, not retried blindly
- `test_hpplite` / `test_batch` have real assertions (they always exited 0)
- Durability suite: adopt after lost confirmation / after crash, divergence
  refused, timer flush vs open transaction, checkpoint persistence, `:memory:`
  node, `hpplite_sync()`, size-limit chunking + reconstruction
- Mock L1 enforces sequential heights and the size limit like the contract
- JSON parser decodes `\uXXXX` (control characters in SQL round-trip)

### Durability (issue #6)
- Pending SQL in `_hpplite_pending`, written inside the user's transaction
- `_hpplite_meta` for height / prev hash / root / checkpoint
- Content-hash state root (`hpplite_db_hash`)
- Retry-safe flush: adopt batches already on L1, keep pending on failure
- Bookkeeping connection + WAL; lease released on close
- Bound parameters expanded in captured SQL

### Testing
- Unit tests with mock L1
- Integration tests with real L1 (HPP Sepolia)
- State reconstruction test

---

## Architecture Decision: Singleton Sequencer

After evaluating distributed witness models, we settled on **singleton sequencer** for simplicity:

**Why not active witnesses?**
- Write routing from witness to sequencer is complex
- Eventual consistency breaks SQLite app expectations
- 24/7 witness nodes add infrastructure burden

**The chosen model:**
- Single sequencer serves all clients (strong consistency)
- All data on L1 (verifiable, reconstructable)
- On-demand verification (no 24/7 watchers needed)
- The chain is a witness, not a judge: ordered encrypted history + roots; key holder rebuilds and compares

See [architecture.md](architecture.md) for details.

---

## Upstream Tracking

HPPLite needs **no SQLite core patches** (issue #2): it is a plain extension
and also builds as a loadable module against any stock amalgamation >= 3.24
(`cmake -DHPPLITE_SQLITE_SRC=...`, then `.load ./hpplite`). The `hpplite`
branch sits on an upstream SQLite **release tag** (currently
`version-3.54.1`, recorded in `patches/UPSTREAM_BASE`) with only
`ext/hpplite/` commits above it. Move to a new release with:

```bash
ext/hpplite/tools/sync_upstream.sh version-3.55.0
```

Rebase, never merge, so `git log version-X..hpplite` always answers "what did we change".

What the extension relies on in SQLite beyond its documented API - the
reprepare inside `sqlite3_step()`, the trace callback's transaction, the
busy handler's silence under a snapshot, the data version - is inventoried
in [`sqlite-dependencies.md`](sqlite-dependencies.md) (#105): each
behaviour with its SQLite source location, the HPPLite code that uses it
and the test that fails if it changes. The `sqlite-trunk` CI job builds
against upstream `master` and runs the mock suite, advisory
(`continue-on-error`), so a change upstream is seen there first; its first
run found trunk's deferred schema reset (2026-10-26/27) breaking the stale
rerun path (entry 10 there), handled in #108: the capture is deferred to
the statement's first run's end when its start finds the schema pinned.
The nightly `hpplite soak` workflow (#120) runs scenario 16 of the anvil
suite for an hour: a demo-like workload against the real contracts, a
fresh replica rebuilt from the chain and `hpplite-verify` every ten
batches; `workflow_dispatch` takes a duration.

---

## Versions (GitHub milestones)

- **v0.1 — Small databases, on-chain DA** (complete, end of life): scope: databases up to ~100 MB, batches in contract storage on L1 (the cost driver; #11/#33).
- **v0.2 — Verification** (v0.2.0 tagged 2026-11-01): #19 row capture by default, #5 `hpplite verify`, several writers per file, the fault harness (#98) as the gate, the SQLite-dependence inventory and trunk CI job (#105, #108). Still open in the milestone: #49 realistic singleton tests on anvil, #62 the SQLite TCL suite with HPPLite on every connection.
- **v0.3 — Large databases, economical DA** (v0.3.0 tagged 2026-11-02): #11 off-chain stores with L1 commitments (pin service over x402, Kubo, S3) and the status row for them, #9 incremental state root, #12 batch compression, the soak through a store. These shipped together: cheap DA invites the sizes where a full-scan root is the wall. Open flakes #132, #133.
- **v0.4 — Keys and page capture** (v0.4.0 tagged 2026-11-03): #17 key roles and provisioning (runtime injection, keystore V3, signer interface, owner/sequencer split), #15 viewing keys and the public-key envelope, #146 page capture (the pages each commit wrote, read from the WAL file; a Merkle root over the file's pages; the default for a new rollup).
- **v0.5 — HPP economics** (v0.5.0 tagged 2026-11-04): #162 store-based persistence and soft commitment, chain-based hard commitment (batches to the store in seconds, one range commitment on the chain every 30 minutes): #164 the contract's range commitment and the anchoring node (done 2026-11-03: VERSION 9 / DA v3 in `contracts/`, `anchor=`, `hpplite_anchor()`, the manifest, the replica through it; the HPP Sepolia factory still VERSION 8), #165 the named store entry, #166 the soft replica, #167 recovery by key and lease; then #126 the HPP-only node (commitments and the lease by relay over x402, pinning priced in HPP) and #135 one payment authorization for many puts, which wait on HPP's side.
- **v0.6 — Operators** (not tagged; in v0.8.0): #181 the canonical store and relay on Cloudflare (hpp-io/hpplite-operators), `store=on` / `relay=on`, the protocols hpplite-store/2 and hpplite-relay/1 with their conformance vectors.
- **v0.7 — Data availability** (not tagged; in v0.8.0): #214 buckets paid by the funding wallet's permit, #203 snapshots and `keep=`, point-in-time `hpplite-verify --to/--at` (#205, #206).
- **v0.8 — One capture** (v0.8.0 tagged 2026-11-09): the persona review of 2026-11-08 - pages the one capture (#253), the store kinds, channels, old factories, the /1 receipt and the HPE1 envelope gone (#247-#252), the budgets in the environment alone (#254), `from=`/`soft=` gone (#255), `checkpoint=` under a store (#256), the manifest's `earliest` (#257), the acceptance script (#258, #261).
- **v1.0 — Stable formats**: frozen batch JSON / `_hpplite_meta` / `hppda://`, bindings.

---

## Future

### Cost Optimization
- [ ] Off-chain DA beyond S3 (IPFS, Arweave) with L1 commitment
- [ ] Batch aggregation (multiple txs per batch)

### Security
- [ ] Challenge submission to L1
- [ ] Sequencer bonding/slashing
- [ ] Fraud proof generation

### Light Clients
- [ ] Merkle proofs for state queries
- [ ] Verify specific rows without full replay
- [ ] Mobile-friendly verification

### Scaling (If Needed)
- [ ] Read replicas (opt-in stale reads)
- [ ] Explicit `mode=replica` for read-only access
- [ ] Bounded staleness guarantees

### Developer Experience
- [ ] `@hpplite/js` - JavaScript/TypeScript SDK
- [ ] `@hpplite/next` - Next.js integration
- [ ] Better error messages
- [ ] Dashboard/explorer

---

## Removed from Scope

These were considered but removed for simplicity:

- **Active witness nodes** - replaced with on-demand verification
- **ZMQ P2P networking** - not needed for singleton sequencer
- **Witness attestations** - replaced with challenge-based security
- **Auto-discovery** - not needed without distributed nodes
- **Write forwarding** - clients connect directly to sequencer

The code for these features still exists but is not part of the primary architecture.

---

## File Structure

```
ext/hpplite/
├── hpplite.c/h          # Lifecycle, registry, transparent API (hpplite_int.h lists the files below)
├── capture.c            # Trace/authorizer/commit/rollback hooks, text rules, stale reruns
├── bookkeeping.c        # Bookkeeping transaction, foreign-write check
├── finalize.c           # Batch build, size cut, finalize
├── replay.c             # Replay, adopt, skip, checkpoint
├── state_hash.c         # State root, block hash
├── status.c             # hpplite_status table
├── batch.c/h            # Batch serialization
├── crypto.c/h           # secp256k1 signing
├── node.c/h             # Node lifecycle, timer
├── config.c/h           # URI parsing, configuration
├── l1_interface.h       # L1 contract interface
├── l1.c, l1_backend.h   # L1 dispatch: the endpoint picks a backend's function table (#35)
├── l1_mock.c            # Mock backend (in memory or a directory)
├── l1_eth.c             # Ethereum backend
├── eth/                 # Web3 primitives
│   ├── eth_client.c/h   # JSON-RPC client
│   ├── keccak256.c/h
│   ├── rlp.c/h
│   ├── abi.c/h
│   └── cJSON.c/h
├── contracts/           # Solidity
│   └── src/
│       ├── HPPLiteDA.sol
│       └── HPPLiteFactory.sol
├── tests/
│   ├── unit/            # Mock L1 tests
│   └── integration/     # Real L1 tests
├── examples/            # Usage examples
├── patches/             # UPSTREAM_BASE (no core patches)
├── tools/               # sync_upstream.sh, check_docs.sh, linux_test.sh + linux/Dockerfile
└── doc/                 # Documentation
    ├── architecture.md
    └── backlog.md
```
