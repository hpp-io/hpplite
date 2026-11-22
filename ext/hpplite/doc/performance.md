# Performance

**Read this first.** The sections up to and including "After the fixes of
2026-11-02" measure statement capture (v0.1-v0.2) and row capture
(v0.2-v0.7): the log rows in `_hpplite_pending`, the per-table trees, the
content-hash root. That code was removed in v0.8 (#253); the numbers stay
as the record of what each version cost and why page capture replaced them.
Only the last section, "Page capture", describes the current code - and its
`--uri "&capture=pages"` is no longer needed (or accepted): pages are the
one capture.

Reference run of `tests/bench/bench_hpplite.c` (mock L1, no network).
Build with the unit tests and run `make bench`, or
`./bench_hpplite [--rows N] [--mb 1,10,100] [--json out.json]`.

Machine: Apple M1 Max, macOS 26.6, SQLite 3.54.1, WAL mode, default
`synchronous`, Release build (the CMake default since this run; a Debug
build is roughly 2x slower across the board). 2026-10-28.

## Write overhead

Cost of capturing statements into `_hpplite_pending`: trace callback,
`sqlite3_expanded_sql`, one extra INSERT in the same transaction, effect
check on completion.

| Mode | plain SQLite | hpplite=on | overhead |
|------|-------------:|-----------:|---------:|
| autocommit (one fsync per statement) | 19 000 stmt/s | 12 600 stmt/s | 1.5x |
| 1000 statements per transaction | 785 000 stmt/s | 288 000 stmt/s | 2.7x |

Autocommit is fsync-bound, so the extra row barely shows. Inside large
transactions each user row carries a second row (the SQL text, ~118 bytes
here), the effect check, the size check and the per-write function check
(#53: a cached verdict per statement shape, a prepare on a miss).
That is the price of "nothing lives only in memory"; 290k captured
statements per second is still far above what any single-writer app does.

Re-measured 2026-10-30 after #53/#54/#57. The 1000-statement transaction
(~140 KB as posted) is larger than one DA batch: until #57 capture refused
it at statement ~960, the bench ignored the error and timed the rest in
autocommit; it now stops on any failed statement.

## Flush, encryption, replay (20 000 statements, one batch, 2.3 MB JSON)

| Step | Time | Rate |
|------|-----:|-----:|
| build + serialize + seal + submit (mock) + finalize, no root | 61 ms | 330 000 stmt/s |
| same, with state root + checkpoint | 81 ms | |
| posted batch size | 118 B/stmt | |
| seal (ChaCha20-Poly1305, vendored C) | 5.2 ms | 455 MB/s |
| open | 5.0 ms | |
| replay into a fresh file (2 batches, 40 000 stmts, incl. open) | 136 ms | 295 000 stmt/s |
| reopen with 20 000 pending statements | 3 ms | |
| first flush after that reopen | 39 ms | |

Encryption is ~8% of a flush and ~4% of a replay; it is not where time goes.

## State root (content hash)

`hpplite_db_hash()` walks every user table in rowid order and hashes
schema + typed values with SHA-256. Linear in database size; from the OS
page cache it is CPU-bound.

| Database | Root | Per MB |
|---------:|-----:|-------:|
| 1 MB | 6 ms | 8.5 ms/MB |
| 10 MB | 64 ms | 8.6 ms/MB |
| 100 MB (75 MB on disk) | 0.65 s | 8.6 ms/MB |

The root is computed once per checkpoint interval (default 100 batches),
not per batch, so at the v0.1 size bound (~100 MB) it costs under a second
every few minutes of continuous writing. Issue #9 (incremental per-table
roots) is the plan beyond that.

## Stress test: 100 MB end to end

`tests/stress/stress_hpplite.c` (`make stress`): writes through HPPLite with
encryption on, the 128 KB DA limit (so batches are cut into series), a
checkpoint every 20 batches (roots computed and checkpoints submitted), then
rebuilds a fresh database from the mock L1 and compares content roots.

| | |
|---|---:|
| rows / size | 1 729 000 rows, 100 MB on disk, ~500 rows per transaction |
| batches | 2 075 (1 729 partial, largest 112 805 B sealed), 86 with a root |
| checkpoints | 86, last root == live content root |
| write | 5.2 s (333 000 rows/s) |
| flush total | 50 s, of which ~45 s is the 86 root computations (one root at 100 MB: 1.04 s) |
| reconstruct from L1 | 50 s for 2 075 batches; dominated by verifying the root at each of the 86 checkpoints |
| result | row count, height and content root identical; 0 statements skipped |

Two things this run established: the root is the cost at this size (with the
default interval of 100 batches it would be ~10 s of the flush total instead
of ~45 s), and replay verification costs the same hashes again. Both are the
#9 incremental-root item; nothing else in the pipeline is size-sensitive.

It also caught two defects before the numbers above: a series of chunks
computed and discarded the root on every partial chunk once an interval was
reached (flush 232 s instead of 50 s), and a generator using `randomblob()`
showed that SQL-text replay cannot reproduce non-deterministic statements
(now detected and counted, #19).

## What the benchmark has caught

- `hpplite_build_batch_limited` cut over-limit batches by re-serializing
  the whole JSON per dropped statement: O(n^2), minutes for a 20 000-row
  import. Now sized from per-statement lengths, O(n).
- A statement that fails inside an explicit transaction kept its log row
  (only autocommit failures rolled it back). Capture now removes rows of
  statements with no effect; replay skips statements that fail.
- The default build was unoptimized; CMake now defaults to Release.

## OLTP benchmark per version (2026-11-02, #140)

`tests/bench/oltp_bench.c` is sysbench's `oltp_*` workload against a plain
SQLite connection with a loadable extension loaded first, so one harness
measures every version of HPPLite through its public surface.
`tools/bench_versions.sh` builds each tag's `hpplite.dylib` in a worktree and
runs the matrix. Four tables of 10 000 rows (`id`, `k` indexed, `c` 120
chars, `pad` 60), one writer, WAL, Release, 10 s per run, median of three.
Every tag is on SQLite 3.54.1, so the differences are HPPLite's. The database
is opened with `l1=local` (no chain, no network); the default 5 s timer flush
runs during the measurement, as it would in production. Same machine as
above. "plain" is the same binary with no extension loaded.

read_write, one transaction each (18 statements: 14 reads, 4 writes):

| version | tps | stmt/s | p50 ms | p95 ms | p99 ms | max ms | batches | flush ms | db MB |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| plain | 4472 | 80501 | 0.187 | 0.208 | 0.247 | 7.1 | 0 | 0 | 7.9 |
| v0.1.0 | 3692 | 66463 | 0.210 | 0.249 | 0.406 | 9.2 | 112 | 655 | 25.6 |
| v0.1.1 | 3677 | 66187 | 0.211 | 0.250 | 0.440 | 9.4 | 118 | 640 | 25.3 |
| v0.2.0 | 3815 | 68669 | 0.204 | 0.242 | 0.289 | 9.4 | 2 | 6448 | 24.5 |
| v0.3.0 | 3814 | 68648 | 0.205 | 0.242 | 0.290 | 9.6 | 0 | 11277 | 34.5 |
| tip (c0f39b70c0) | 3812 | 68608 | 0.205 | 0.243 | 0.296 | 9.4 | 0 | 11250 | 34.5 |

write_only, one transaction each (index update, non-index update, delete + insert):

| version | tps | stmt/s | p50 ms | p95 ms | p99 ms | max ms | batches | flush ms | db MB |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| plain | 7836 | 31346 | 0.091 | 0.114 | 0.143 | 9.8 | 0 | 0 | 8.0 |
| v0.1.0 | 5321 | 21286 | 0.127 | 0.165 | 0.239 | 9.6 | 156 | 1033 | 33.4 |
| v0.1.1 | 5267 | 21066 | 0.128 | 0.167 | 0.241 | 9.2 | 162 | 1018 | 32.9 |
| v0.2.0 | 5672 | 22687 | 0.118 | 0.155 | 0.190 | 10.7 | 1 | 13618 | 32.6 |
| v0.3.0 | 3362 | 13446 | 0.119 | 0.156 | 0.191 | 5749.4 | 1 | 10046 | 38.7 |
| tip | 3368 | 13471 | 0.119 | 0.155 | 0.191 | 5829.1 | 1 | 9733 | 39.0 |

read_write, autocommit (each statement its own transaction):

| version | tps | stmt/s | p50 ms | p95 ms | p99 ms | max ms | batches | flush ms |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| plain | 2890 | 52022 | 0.307 | 0.337 | 0.381 | 8.1 | 0 | 0 |
| v0.1.0 | 2315 | 41665 | 0.361 | 0.402 | 5.860 | 8.3 | 78 | 372 |
| v0.2.0 | 2330 | 41932 | 0.363 | 0.401 | 5.762 | 7.2 | 1 | 2495 |
| v0.3.0 | 2007 | 36127 | 0.364 | 0.401 | 5.731 | 1364.3 | 1 | 3776 |
| tip | 1758 | 31644 | 0.364 | 0.401 | 5.761 | 1435.4 | 1 | 1792 |

read_only, one transaction each (14 reads): plain 11 213 tps, every HPPLite
version 12 600-12 700 tps, p99 0.09 ms. Reads cost nothing; the extension's
build is in fact 12% faster, reproducibly (re-run plain and tip back to back:
11 114 vs 12 532). Not explained; the bookkeeping connection keeping the WAL
index open is the suspect.

What the numbers say:

- **Per-statement write overhead is small and has not moved since v0.2.**
  Transactions of 18 statements run at 85% of plain SQLite's rate, p99 0.29
  vs 0.25 ms. Write-only transactions run at 72%. v0.1 was a few percent
  slower per statement (the per-statement function check #53 and the
  O(n) size cut #57 came with v0.2).
- **Since the per-table trees (#9, v0.3), the timer flush stalls the writer
  for as long as the flush takes.** write_only max latency went from 10 ms
  (v0.2.0) to 5.7 s (v0.3.0), and throughput fell 40% over a 10 s run
  because the writer sat out one flush. `git bisect` on the write_only run
  (max latency < 1 s = good) names ce866fb75a. v0.1 posted as it went (150
  small batches during the run, 10 ms max); v0.2 flushed once but did not
  hold the writer. The flush's bookkeeping transaction holds the transaction
  gate while `hppliteTreeSync` takes the log in. Issue #141.
- **A flush moves 17 000-19 000 captured rows per second** (v0.2.0: 226 000
  rows in 13.6 s; tip: 194 000 in 9.7 s), far below the 330 000 stmt/s single
  batch of the v0.1 table above: the DA size cut (#57) makes ~100 batches per
  flush, and `sample` shows both halves of each batch (building it in
  `hpplite_build_batch_limited`, finalizing it in `hpplite_finalize_batch`)
  in `sqlite3_step` walking the pending log with `pread` underneath, so each
  batch re-reads log pages that the bookkeeping connection's page cache no
  longer holds. Compression and encryption are not where the time goes
  (`compress=off` and `encrypt=off` change the flush rate by less than the
  run-to-run spread). Issue #142.
- **The 5.8 ms p99 in autocommit is SQLite's WAL auto-checkpoint, not
  HPPLite.** The plain run has the same 5-7 ms outliers (59 of 8 355
  statements in a 2 s run); with the extension each write also inserts its
  log row, the checkpoint comes twice as often, and the outliers cross the
  1% line. Transactions show no such p99 because the checkpoint lands on one
  commit in many.
- **The file is larger.** 26-39 MB against 8 MB: the pending log lives in the
  file until a flush, the trees (#9) stay, and freed pages are not returned
  (no auto_vacuum).

Reproduce: `cmake --build build_mock -t oltp_bench && sh tools/bench_versions.sh`
(the raw runs land in `build_mock/bench/*.jsonl`).

## After the fixes of 2026-11-02 (#143, #141, #142; tip 2012cc3777)

The run above found three things, all fixed the same day: the size-triggered
flush had been blind to row records since v0.2 (#143), the timer's flush held
the writer for its whole duration (#141), and a flush paid a tree write and
an fsync per batch under the gate (#142). Same harness, same machine, 10 s
runs, median of three:

| configuration | version | tps | stmt/s | p50 ms | p99 ms | max ms | batches in 10 s | rows left pending |
|---|---|--:|--:|--:|--:|--:|--:|--:|
| write_only, trx | plain | 7728 | 30 914 | 0.095 | 0.133 | 7 | | |
| write_only, trx | v0.2.0 | 5672 | 22 687 | 0.118 | 0.190 | 11 | 1 | 226 000 |
| write_only, trx | tip before | 3368 | 13 471 | 0.119 | 0.191 | 5829 | 1 | 194 000 |
| write_only, trx | tip after | 4065 | 16 260 | 0.132 | 2.499 | 435 | 742 | ~5 000 |
| read_write, trx | plain | 4374 | 78 733 | 0.194 | 0.260 | 7 | | |
| read_write, trx | v0.2.0 | 3815 | 68 669 | 0.204 | 0.289 | 9 | 2 | |
| read_write, trx | tip after | 2462 | 44 318 | 0.216 | 1.225 | 152 | 532 | ~1 000 |
| write_only, autocommit | plain | 4198 | 16 791 | 0.196 | 0.265 | 56 | | |
| write_only, autocommit | tip after | 2043 | 8 171 | 0.272 | 2.662 | 112 | 438 | |
| read_only, trx | plain | 11 131 | 155 833 | 0.090 | 0.103 | 1 | | |
| read_only, trx | tip after | 12 656 | 177 180 | 0.079 | 0.092 | 1 | 0 | |

How to read it:

- **The writer is never held for a flush any more.** The 5.8 s stall is
  gone; what remains is a few milliseconds per batch while the log rows are
  pruned, and the tree write when a checkpoint closes or 65 536 touched
  nodes wait in memory (the 150-435 ms maxima).
- **A busy writer now posts as it goes.** 500-750 batches in 10 s with a few
  thousand rows left, where every version since v0.2 posted once per
  interval and left 200 000 rows. v0.2's higher tps bought nothing: its log
  was unbounded and the final flush took 13 s.
- **Throughput is 72% of v0.2's in a tight write loop**, because the writer
  shares the file with a bookkeeping thread that takes each row into the
  trees at ~100 µs per row (`touchRecord` reads tree nodes from SQLite per
  level). That is the next lever, if an application ever writes at this
  rate; the demo writes a few rows a minute. With 1 ms between
  transactions, what applications do, the writer's max latency is under
  50 ms and nothing waits.
- **Bulk load** (the harness's prepare phase, 500-row transactions) runs at
  ~100 000 rows/s with the extension against 210 000 before #143, for the
  same reason: it is posted as it goes now.
- Reads are unchanged (and still 12% faster with the extension loaded).

## Page capture (2026-11-03, #146)

The same harness on `capture=pages`, the default for a new rollup since
f081e7bdfe: after each commit the connection's WAL hook has HPPLite read the
commit's frames from the WAL file and queue the pages (#155), the flush
posts page batches (whole pages the first time, XOR diffs after, deflated at
level 1), nothing is logged in the file. `--ext build_mock/hpplite.dylib
--uri "&capture=pages"`, 10 s runs, median of three, one transaction each,
the mock L1, no encryption. Measured on the WAL reader (the first page-mode
numbers, on the VFS watch it replaced, were within 3% of these).

| configuration | capture | tps | stmt/s | p50 ms | p99 ms | max ms | batches in 10 s | flush ms | db MB |
|---|---|---|---|---|---|---|---|---|---|
| write_only, trx | plain SQLite | 7719 | 30 877 | 0.096 | 0.135 | 7 | | | 8.0 |
| write_only, trx | pages | 7669 | 30 675 | 0.127 | 0.162 | 14 | 1634 | 31 | 8.0 |
| write_only, trx | rows | 3875 | 15 498 | 0.132 | 2.144 | 433 | 805 | 258 | 23.9 |
| read_write, trx | plain SQLite | 4431 | 79 765 | 0.192 | 0.240 | 7 | | | 7.9 |
| read_write, trx | pages | 4506 | 81 108 | 0.216 | 0.273 | 13 | 957 | 12 | 7.9 |
| read_write, trx | rows | 2441 | 43 944 | 0.216 | 1.328 | 137 | 528 | 47 | 23.4 |

Rerun before the v0.4.0 tag (2026-11-03, 86b85963d9, same harness, median of three): write_only plain 7757 tps, pages 7592, rows 3835; read_write plain 4385, pages 4600, rows 2373. Every figure within 3% of the table.

Rerun for v0.5 (2026-11-04, 42a7bada24, same harness, median of three, with the demo running on the machine): write_only plain 7812 tps, pages 7523, rows 3864; read_write plain 4423, pages 4627, rows 2414. Every figure is within 2% of v0.4.0's. v0.5's additions on the write path include the pre-update hook on every row change under row and statement capture (#169, active here: the bench's SQLite has `SQLITE_ENABLE_PREUPDATE_HOOK`), the trace's own-text check (#176) and the spend check after a flush (#173); none of them shows.

How to read it:

- **Page capture costs the writer about 30 µs per transaction** (p50 0.127
  ms against 0.096; read_write within the noise of plain), which is the
  read-back of the commit's frames from the WAL file on the committing
  thread. The batch is built and posted on the timer thread from that copy;
  the writer never waits for it. Row capture pays 1.3-2.1 ms at p99 and
  loses half the rate: its log rows are written inside the application's
  transaction and the trees take each row in afterwards.
- **What the WAL reader must not do:** poll the WAL at the end of every
  statement. The first reader did, for any statement whose wal hook had not
  run (SQLite runs the hook only when a step returns SQLITE_DONE in
  autocommit: doc/sqlite-dependencies.md, entry 11), which is every
  statement inside a transaction but COMMIT, and it cost 40 µs per
  statement: write_only fell to 7 019 tps. The poll now runs only for a
  statement that can have committed (autocommit, not read-only).
- **The file stays the application's size.** 8 MB with pages against 24
  MB with rows: no pending log, no trees in the file (the bookkeeping is a
  sidecar, a few KB).
- **A flush is a few tens of milliseconds** (31 ms for the biggest batches
  of a tight write loop) against 50-260 ms with rows, and it holds no
  gate: the writer's max latency is 14 ms, plain SQLite's 7.
- A page batch of this workload is 50-100 KB deflated (160 batches/s at
  the 128 KB default limit), where a row batch of the same commits is
  smaller: pages carry every byte of a touched page, a record only the row.
  Storage per commit is the trade (#146): for a write-heavy workload on few
  pages the diffs stay small; for scattered writes each touched page costs
  a page.

Reproduce: `cmake --build build_mock -t oltp_bench`, then `oltp_bench --ext
build_mock/hpplite.dylib --uri "&capture=pages" --mode write_only --seconds 10`.

## Snapshots (2026-11-18, #233)

*Superseded the next day: since #364 a snapshot streams, and its memory is a chunk's (see "Memory against the database's size" below).*

A snapshot (#221) is the whole database as one page batch, deflated, sealed and put in 8 MiB chunks at an anchor.
`tests/bench/snapshot_bench.c` writes a database of random 4 KB rows (deflate's worst case: nothing compresses)
through a node into the reference store, then anchors once with `snapshot=` on; Apple M-series, mock chain.

| Database | Peak RSS before the anchor | Added at the anchor | The node's CPU for it |
|---|---|---|---|
| 200 MB, before #233 | 207 MB | **+619 MB** (three copies) | 6.3 s |
| 200 MB, sealed in place | 235 MB | **+399 MB** (two copies) | 6.5 s |
| 1 GB, sealed in place (after #363) | 718 MB | **+1438 MB** | 12.1 s |

- **Before the anchor:** about the database's size. Page mode keeps every page's last posted image to diff against (`aPrev`).
- **Before #233, the anchor added three copies:** the encoded image, the deflated copy and the sealed envelope, all alive at once.
- **Now:** the image is encoded with room for the envelope's header and tag, freed as soon as its deflated copy replaces it, and sealed in place (`node_seal_owned`, `hpplite_envelope_seal_inplace`).
  - With incompressible pages the peak is two copies (the image and its deflated attempt); with real pages it is nearer one.
  - The bytes are the same as before, so readers and the index and chunk format are unchanged.
- **At 1 GB:** before #363 no snapshot was made at all: the database crossed SQLite's lock-byte page, which no commit carries. Now it is made.
  - Peak RSS is macOS's resident count: the 718 MB before the anchor is below the database's size, so some pages were compressed or swapped, and the +1438 MB is measured the same way. It is within the two-copy bound.
  - Wall time: 2519 s, again the reference store's pure-Python hashing.
- **Wall time:** the bench's anchor took about 490 s. That is the reference store verifying each put's keccak hash in pure Python (about 2 s a MB), not the node, whose CPU time is the column above.
- **What remains:** a fully streamed snapshot (sealed and put chunk by chunk from the pages) would bound the peak to a chunk. It needs an incremental AEAD, and the deflate-or-not decision taken before the whole batch is deflated.

Reproduce: `cmake --build build_mock -t snapshot_bench`, then `build_mock/snapshot_bench --mb 200 --store operators/store`.

## Memory against the database's size (2026-11-19, #364, #365)

SQLite runs a database of any size in a few MB of page cache. Until #364 a node held the whole database in memory:
- every page's last posted image (`aPrev`), read at open;
- the whole file again when it was queued as one commit;
- the backlog of commits not yet posted (#365), and each transaction while it was read from the WAL;
- at a snapshot, the image built in RAM, deflated and sealed; and on a replica starting from one, every chunk joined in memory, then opened and inflated.

Now:
- the posted images live in a file beside the database (`<db>-hpplite-posted`);
- the queue keeps up to `HPPLITE_QUEUE_MEM` (32 MiB) in memory and spills the rest to `<db>-hpplite-queue`;
- `walread` records frame numbers and the pages are read from the WAL;
- snapshots stream both ways: from the posted file through zlib and a streamed ChaCha20-Poly1305 into 8 MiB chunks, and back through a temporary file whose tag is checked before any page is applied.

The index, chunk and batch formats are unchanged.

`tests/unit/test_memory.c` runs each case in a child process and measures the rise of its peak RSS:

| Case | Before | Now |
|---|---|---|
| A. Open and post a 64 MB database | +145 MB | +13 MB |
| B. 128 MB written with nothing posted | +126 MB | +31 MB (the queue's constant) |
| C. A 32 MB database anchored with a snapshot | +64 MB | +9 MB (one chunk) |
| D. A new replica starting from that snapshot | +191 MB | +9 MB |

`snapshot_bench` at 200 MB (incompressible rows):

| | Before #364 | Now |
|---|---|---|
| Peak RSS before the anchor | 207 MB | 39 MB |
| Added by the anchor's snapshot | +619 MB (+399 MB with #233's in-place seal) | +0 MB (below the peak the writes set) |
| The node's CPU for the anchor | 6.3 s | 6.4 s |

Costs:
- **Disk:** one more copy of the database (the posted file). The backlog spills to disk during an outage. A replica starting from a snapshot keeps the sealed bytes and the image on disk while it applies them.
- **Snapshots are always deflated** (stored blocks cost incompressible pages 0.03%). The one-shot deflate refused inputs over 256 MiB, so larger snapshots used to go uncompressed; now any image up to the form's 4 GiB is deflated.
- **Throughput:** oltp_bench write_only is within 1-2% (7,690 vs 7,600 tps). Opening a 64 MB database and posting it whole went from 34 s to 10 s in test_memory, because a pieced commit's pages now enter the root once instead of at every piece.

Reproduce: `ctest -R test_memory`; `build_mock/snapshot_bench --mb 200 --store operators/store`.

## A rebuild's time on a long history (2026-11-21, #391)

How long a rebuild to a point takes: `tests/bench/rebuild_bench.c` writes a small, busy database (5 MB, two rows
rewritten a batch) through a node into the reference store on the mock chain, an anchor every 360 batches and a
snapshot every 17,280 - a batch every 5 s with `anchor=30m&snapshot=24h`, nothing pruned - and at 7 and at 30 days of
batches rebuilds it with a verifier on an empty file. Apple M-series, the store on the same machine, nothing else
running. With a stop a rebuild begins at the newest snapshot at or before it (#391); without one at batch 1.

| Rebuild | 7 days (120,960 batches) | 30 days (518,400 batches) |
|---|---|---|
| No stop: from batch 1, every batch checked | 79.87 s (38.50 s CPU; 121,298 requests) | 409.47 s (167.10 s CPU; 519,842 requests) |
| To a stop a day's batches less one past a snapshot (the worst) | 11.53 s (5.50 s CPU) | 13.06 s (5.49 s CPU) |
| To a stop half a day past a snapshot | 5.69 s (2.75 s CPU) | 6.67 s (2.78 s CPU) |
| To a stop at a snapshot's height | 0.08 s | 0.09 s |

Every rebuild ended in state `verify` with no root mismatch, on nine connections (one, and eight for the reads ahead, #405).

A second run with the history pruned: ten days of batches with a `keep=` of seven days (172,800 batches, `--keep-days 7`).
Without a stop the rebuild begins at the oldest snapshot the store still keeps, at batch 51,840, and replays the
120,960 batches after it: 94.35 s (38.23 s CPU; 121,302 requests). To the three stops as above: 14.08 s, 6.68 s and
0.08 s. The store held 1.5 GB in 363,594 files, what seven days make, at ten.

- **A stop costs what lies between it and the snapshot before it,** not the history's length: at most a day's batches with `snapshot=24h`, 11 to 13 s here whether the history is 7 or 30 days. `snapshot=6h` would make it a quarter (computed).
- **From batch 1, or from the oldest snapshot kept, the time follows the history read:** about 1,300 to 1,500 batches a second (the same 120,960 batches took 79.87 s in one run and 94.35 s in the other), four tenths to a half of it the rebuilding process's CPU, the rest its wait for the store's answers.
- **This is a store on the same machine and a chain in the process.** Against a far store and a real chain the rate is theirs. Measured on HPP Sepolia from a Mac in Korea (3d6039b2b4, 2026-11-21), `hpplite-verify` from batch 1 of a rollup of 14,401 batches in about 720 anchored ranges of 20, a canonical store and the public RPC: 854 s with 32 reads in flight (`HPPLITE_STORE_PARALLEL=32`), 16.9 batches a second, verified; its first 2,000 batches with the default eight in 212.66 s, 9.4 a second. Sampled, the time is the store's reads; the chain's one log a range is about a tenth. By those rates a day's batches (17,280) take about 17 minutes at 32 in flight and about 31 at eight, and 30 days from batch 1 about 8.5 hours at 32 (computed from these two runs: one store, one place, ranges of 20). v0.11.0 took 0.8 s a batch there, 11,518 s for the same rollup: the chain client asked the chain four times a batch and walked the index a request a commitment at every open (#410, #411) - the mock chain answers in the process, so no bench here showed it. The canonical stores limit reads at 6,000 requests a minute an address (600 until hpp-io/hpplite-operators#77): the run with 32 in flight made about 1,060 a minute; the node waits a limit out since #402, and #404 asks for several batches a request. A stop near a snapshot is what stays fast there: the stop at a snapshot's height is 12 to 14 requests. With the reads running ahead across the ranges and a range's log asked for with those after it (#412, #413; 7a5a2747ac, 2026-11-22) the same rollup, 15,353 batches by then, took 387 s from batch 1 with 32 in flight, 39.7 batches a second, verified, and its first 2,000 batches 139 s with the default eight and 61 s with 32; an open alone (`--to 1`) took 16.3 s, so without it the first 2,000 came at about 16 batches a second with eight in flight and 45 with 32, where the store alone gave 20.7 and 70 reads a second (#404). By the whole run's rate a day's batches take about 7 minutes at 32 in flight and 30 days from batch 1 about 3.6 hours; at eight a day's take about 18 minutes (computed from these runs: one store, one place, ranges of 20). The run with 32 in flight made about 2,500 store reads a minute (computed from what it read). Not measured: a history of short ranges on a real chain, the whole history with eight in flight.
- **Writing it:** 42 batches a second throughout (7 days in 2,847 s, 30 in 12,387 s), the reference store's rate; the store held 1.5 GB in 363,585 files at 7 days and 6.2 GB in 1,558,205 at 30.
- **What its runs found:** the reference store rewrote a bucket's whole record at every put (#401); the store client made a connection a request (#405); the reference store's hourly sweep held every put up (#409). And what a rebuild meets at a store that limits reads: #402, and with its test #403. The 30-day run completed on the fourth try, after these.

Reproduce: `cmake -S . -B build_bench -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-DHPPLITE_MOCK_MAX_BATCHES=600000`,
`cmake --build build_bench -t rebuild_bench`, then
`build_bench/rebuild_bench --batches 518400 --marks 120960,518400 --dir /tmp/rebuild` (about three and a half hours);
the pruned run: `--batches 172800 --marks 172800 --keep-days 7` (about 70 minutes).

## A day in one range (2026-11-21, #394)

`anchor=24h` with a batch every 5 s puts 17,280 batches in one range: one manifest, one commitment. `tests/bench/range_bench.c`
writes that range through a node into the reference store on the mock chain (a 5 MB database, two rows rewritten a
batch), anchors it, reads it back, loses the host a day later and prunes. Apple M-series, nothing else running; the
day is written in seven minutes, so what depends on a real day's clock (the lease's renewals) is not shown.

| | Wall | The node's CPU | Node memory |
|---|---|---|---|
| The day's 17,280 batches written, none anchored | 402 s (43 a second: the store's rate) | | peak 35 MB; the bookkeeping file 3.5 MB |
| The anchor of the range | 5.95 s | 0.06 s | +10 MB |
| A verifier from batch 1 through the range | 15.94 s (1,087 batches a second) | 7.12 s | +6 MB |
| A lost host's successor reading the next day's 17,280 batches by name | 359.75 s (48 a second) | 16.05 s | +0 MB |
| Its anchor of that range, with a snapshot | 12.67 s | 0.26 s | +4 MB |
| The anchor that prunes behind that snapshot | 15.18 s | 0.00 s | +0 MB |
| A verifier after the prune, from the snapshot | 0.07 s | 0.06 s | +0 MB |

- **The manifest:** 2,685,057 bytes for 17,280 batches, about 155 bytes a batch, one object. A store's limit of 16 MiB an object is reached near 108,000 batches a range (computed): six days at a batch every 5 s. Since v0.12.0 an entry also names its batch's time and bytes (#397), about 28 bytes more (computed, not measured again): about 183 bytes a batch, the limit near 91,000 batches a range, five days. A listing (`hpplite-verify --list`) reads the manifests and no batch: on HPP Sepolia from a Mac in Korea, the demo rollup's 15,414 batches - 771 ranges written by a v0.11 sequencer, a line each, and the 13 batches past the anchor - in 60 s with the default eight reads in flight. Not measured: a listing of manifests that name their batches' times, a range of thousands of batches.
- **The anchor's time is the store's:** the node's part is 0.06 s; the rest is the reference store hashing the manifest in pure Python. While an anchor runs, the node's flush waits behind it (read from the code, not measured): batches written meanwhile are posted after it.
- **Reading the range:** a verifier reads eight batches at once over the connections it keeps (#405). Against a far store its round trips are the rate, not this: a canonical store on HPP Sepolia gave 9.4 batches a second with eight in flight and 16.9 with 32 (measured, in the section above), about 31 and 17 minutes for this range (computed from those rates); the canonical stores' limit of 6,000 requests a minute an address was not near (#402 makes the node wait a limit out, #404 asks for several batches a request).
- **The lost host:** the 48 names a second are the reference store's, which recovers a signer at every read of a name (#406); the node's own work for the 17,280 batches is 16 s.
- **The prune:** the store's files went from 103,815 to 25, its names from 34,600 to 4.
- **What this bench led to:** #406 (the reference store's rate for names), and #407 (a snapshot's chunk refused by a store's write limit ends the snapshot).

Reproduce: `cmake -S . -B build_bench -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-DHPPLITE_MOCK_MAX_BATCHES=600000`,
`cmake --build build_bench -t range_bench`, then `build_bench/range_bench --dir /tmp/range`.

## What a snapshot costs at a canonical store (2026-11-21, computed)

Computed from the mainnet store's configuration (a new write $0.00001, a MiB held a month $0.00003: about $0.031 a GiB
a month), not measured; the sizes are the upper bound, a database that does not deflate.

A snapshot is a put for every 8 MiB of the deflated database and one for its index, and its bytes are held until an
anchor with `keep=` prunes behind a newer one.

| Database | Puts a snapshot | Its write fees | Held a month, each | `snapshot=24h`, `keep=30d` (about 30 held) | `snapshot=6h`, `keep=30d` (about 120 held) |
|---|---|---|---|---|---|
| 10 MB | 3 | $0.00003 | $0.0003 | $0.009 a month | $0.036 a month |
| 1 GB | 129 | $0.0013 | $0.031 | $0.92 a month, 30 GiB | $3.70 a month, 120 GiB |

- **The bytes are the cost,** the writes are not; an account's quota (1 GiB unless the operator set another) is reached long before the money matters.
- **Time:** the anchor that made a snapshot of the 5 MB database above took 12.67 s against the reference store, 0.26 s of it the node's. The flush waits behind an anchor (read from the code).
- **Past about 60 chunks (480 MiB deflated)** a snapshot put inside a minute would meet the canonical stores' limit of 60 writes a minute a bucket: the chunk that is refused waits the store's `Retry-After` out and is put again, as the snapshot's name, its index and the anchor's manifest are (#407; up to five such answers in a row for one put, two minutes a wait at most, not while the node is closing): a snapshot of 1 GiB deflated, 128 chunks, takes two such waits, about two minutes at the canonical stores, and the flushes wait behind the anchor for them (computed from the limit, not measured against a canonical store; the wait is shown against the reference store's 429 hook). Before #407 the refused chunk ended the snapshot and the next anchor began another from its first.
