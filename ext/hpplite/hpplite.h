/*
** HPPLite - Verifiable SQLite
**
** Tracks every write made through a connection, batches the SQL, and posts
** the batches to an L1 contract for data availability. Anyone can rebuild
** the database from L1 alone and check the resulting state root.
**
** The local database file is the only durable state HPPLite keeps:
**
**   _hpplite_pending  SQL captured by the statement trace. The row is written
**                     from inside the user's own statement, so it commits or
**                     rolls back together with the user's transaction. Rows
**                     are deleted once the batch they belong to is confirmed.
**   _hpplite_meta     Next batch height, previous block hash, the state root
**                     after the last confirmed batch, and checkpoint tracking.
**
** The state root is a content hash of all user tables (schema + rows). It is a
** function of the file, not of the path taken to produce it, so it can be
** recomputed at any time and compared against what was posted to L1.
*/

#ifndef HPPLITE_H
#define HPPLITE_H

#include "hpplite_sqlite.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** State root is a 32-byte hash representing database state
*/
#define HPPLITE_HASH_SIZE 32

/* Forward declaration for batch */
struct HppliteBatch;

/*
** HPPLite context attached to a database connection
*/
#define HPPLITE_MAX_RUNS 16     /* application statements running at once that capture keeps track of */
typedef struct HppliteCtx HppliteCtx;
struct HppliteCtx {
  sqlite3 *db;                /* Application connection: trace + pending log */
  sqlite3 *bkDb;              /* Bookkeeping connection (== db for :memory:) */
  int bEnabled;               /* Tracking enabled flag */
  int bPages;                 /* page mode (#146): no hooks on the application's connection, the bookkeeping in a sidecar */
  uint64_t inflightHeight;    /* page mode: a batch posted and not yet finalized (0: none), with its root; survives the process */
  unsigned char inflightRoot[32];
  int bInternal;              /* Set while HPPLite runs its own statements */
  int bDbClosed;              /* db is a zombie: no SQLite calls on it any more */
  int bAppFlushing;           /* hpplite_flush() is running on the application thread */
  volatile int nAppStmtActive; /* Application statements between their STMT and PROFILE trace
                               ** events (mid-step). Written on the application thread only;
                               ** the timer thread reads it instead of touching db. */
  int bStatusGone;            /* hpplite_status was disconnected by a close that failed */
  sqlite3_int64 curTxn;       /* Id of the first logged row of the open transaction (0 = none) */
  sqlite3_stmt *pLogStmt;     /* Cached INSERT INTO _hpplite_pending */
  sqlite3_stmt *pRecTrigStmt; /* Cached PRAGMA recursive_triggers (read) */
  int bRecTrigSeen;           /* The application prepared PRAGMA recursive_triggers: read it per statement */
  int pragmaMode;             /* HPPLITE_MODE_PRAGMAS bits as the application last set them (#28) */
  int bCollectFns;            /* Authorizer is checking the functions of a write (#53, #54) */
  char zCollectWhy[160];      /* ...first reason it cannot replay ("" = none) */
  char **azReplicaFns;        /* Functions a replica has (fresh connection), sorted, lower case */
  int nReplicaFns;            /* ...count; -1 = unknown (pragma_function_list unavailable) */
  int nDdlGen;                /* DDL captured on this connection (invalidates the verdicts) */
  sqlite3_int64 openTxnId;    /* Writer: log id of the group the last posted batch left open (#57) */
  char zRollup[80];           /* "<chain id>:<contract>" the history is on ("" = not bound yet) */
  sqlite3_int64 fragId;       /* Writer: log id of the statement the last posted batch ended a piece of (#59) */
  size_t fragOff;             /* ...bytes of its text posted so far */
  int nCarry;                 /* Replica: statements held back from an open group (_hpplite_carry) */
  uint64_t nTxnsDropped;      /* Replica: transactions dropped because their other part never came */
  int collectKind;            /* ...what kind of reason (capture.c's TextVerdict) */
  struct HppliteFnVerdict {   /* Function check of a write, by statement text and schema */
    char *zSql; int schema; int kind; char zWhy[160];
  } aFnVerdict[64];
  struct HppliteStmtRun {        /* A run of an application statement, between its STMT and PROFILE trace */
    sqlite3_stmt *pS;
    sqlite3_int64 logId;         /* the log row it wrote (0 = none) */
    sqlite3_int64 now;           /* the time pinned for it (0 = none; #56) */
    unsigned clockSeen;          /* clock reads on the thread when it started */
    int vmSteps;                 /* its VM step count as it started (a later trace with the same
                                 ** count is a sub-program; another count, a new run) */
  } aRun[HPPLITE_MAX_RUNS];
  int nRun;
  int seenSchema;                /* schema_version this connection last loaded (-1: not yet) */
  sqlite3_stmt *pRerunStmt;      /* a statement prepared on a stale schema, logged in a transaction
                                 ** HPPLite opened for it: SQLite reruns it without a STMT trace */
  int rerunStage;                /* 1 logged, 2 its first run failed (SQLITE_SCHEMA) and the rerun runs */
  int rerunSchema;               /* schema_version when it was tracked: one it changed itself expired itself
                                 ** (-1: unknown, the read failed with the capture, #108) */
  int bCaptureDeferred;          /* its capture waits for its first run's end: HPPLite's statements could
                                 ** not run at its start, the stale schema not reset while it runs (#108) */
  int bDeferredJudged;           /* ...the text rules were applied at its start already */
  int bDenyReprepare;            /* the next prepare is the refused statement's reprepare: deny it (#89), known
                                 ** without the error code (HPPLite's own statements ran since, #108) */
  int bRebuildOwed;              /* a write found the schema stale and pinned under a held lock and failed
                                 ** SQLITE_BUSY: the triggers are rebuilt under the next write's lock (#108) */
  sqlite3_stmt *pRefusedStmt;    /* refused at its trace (sqlite3_interrupt); a first run failing stale
                                 ** (SQLITE_SCHEMA) reruns untraced with the interrupt cleared, so its
                                 ** reprepare is denied by the authorizer instead (#89) */
  enum HppliteOwnTxn {           /* The transaction HPPLite opens for a statement (rerunTrack, in autocommit): */
    HPPLITE_OWN_NONE = 0,        /* none: the application's, left alone */
    HPPLITE_OWN_OPEN,            /* open, committed at the statement's end (ownTxnEnd) */
    HPPLITE_OWN_COMMIT_PENDING   /* its COMMIT failed where it ended (a statement in progress): at the next end */
  } ownTxn;
  volatile int bAppTxnOpen;   /* The application connection is inside a transaction (app thread
                              ** writes, the timer thread reads: no bookkeeping commit then) */
  struct HppliteNoClock { sqlite3_int64 id, now; } *aNoClock; /* Log rows whose statement did not read
                              ** the clock: the batch leaves their time out (#56) */
  int nNoClock, nNoClockAlloc;
  sqlite3_mutex *pNoClockMutex;
  sqlite3_stmt *pZeroStmt;    /* Cached zero-row UPDATE: puts changes() back to 0 after our DELETE */
  volatile int txnGate;       /* 0 free, 1 the application (a statement or transaction open), 2 a
                              ** bookkeeping write: its commit never lands inside the application's */
  int bBkGate;                /* this bookkeeping transaction took the gate */
  volatile int bBkWantsGate;  /* a bookkeeping write is waiting: the application hands the gate over
                              ** when it goes idle instead of freeing it (#141) */
  sqlite3_stmt *pSeqStmt;     /* Cached fingerprint of sqlite_sequence (NULL: none yet) */
  int seqTriedGen;            /* nDdlGen + 1 when sqlite_sequence was last found missing */
  int bClockPin;              /* The connection's VFS is the clock VFS: 'now' can be recorded and replayed */
  sqlite3_stmt *pPinOwner;    /* The statement that pinned the clock (outermost write) */

  /* Row capture (capture engine v2, #19; capture_rows.c) */
  int bRows;                  /* writes are captured as row records by the connection's TEMP triggers */
  int rowsBuiltFor;           /* schema_version the triggers were built for (-1: none) */
  int rowsTempBuiltFor;       /* temp's schema_version once built: a ROLLBACK brings the old ones back (#88) */
  int bRowsCapOff;            /* the running statement is logged as text: its triggers record nothing */
  sqlite3_stmt *pRowsTextStmt; /* ...that statement */
  sqlite3_stmt *pRowsSchemaStmt; /* Cached PRAGMA main.schema_version */
  sqlite3_stmt *pRowsTempSchemaStmt; /* Cached PRAGMA temp.schema_version */
  sqlite3_stmt *pRowsLockStmt; /* Cached no-op write: the write lock before that read */
  sqlite3_stmt *pProbeStmt;   /* Cached read of _hpplite_meta: can HPPLite's statements run, or is the
                              ** schema stale and pinned by a running statement (#108)? */
  char *zRowsSeq;             /* sqlite_sequence when last looked at: name '\0' seq '\0' ... '\0' */
  sqlite3_stmt *pVacuumStmt;  /* A VACUUM running: the ATTACH inside it is its own */
  int vacuumSchema;           /* ...schema_version as it started (VACUUM bumps it when done) */
  int vacuumFd;               /* <file>-hpplite-vacuum (#75): a VACUUM holds it exclusively from its start until its
                              ** marker is logged; a captured statement holds it shared while it runs. -1: none */
  int bBarrierHeld;           /* ...this connection's shared hold, for the statement in progress */
  int bVacuumBarrier;         /* ...its exclusive hold, for its VACUUM */
  char zDropTable[256];       /* The table the statement being prepared drops (its triggers go with it) */
  int nDropAllowed;           /* ...how many of its capture triggers that one prepare may still drop (#71) */
  char *zDbName;              /* This database's name among its wallet's rollups (#61), or NULL */
  int bExpandTruncates;       /* sqlite3_expanded_sql() cuts bound values (better-sqlite3): row
                              ** capture only, statements logged as text may bind nothing (#62) */
  char *zRowsTextOnly;        /* Names a write must not mention to be captured as rows: virtual
                              ** tables and what reaches them through triggers ('\0'-separated,
                              ** ends with an empty name) */
  sqlite3_stmt *pUnlogStmt;   /* Cached DELETE of a failed statement's row */
  struct HppliteLogged {      /* The statement whose row was logged last, and what its effect check compares */
    sqlite3_stmt *pS;
    sqlite3_int64 id;         /* its row */
    int keep;                 /* kept even without an effect (a header pragma) */
    sqlite3_int64 changes;    /* total_changes when it was logged */
    int schema;               /* schema_version when it was logged */
    int bSeq;                 /* seq taken (an INSERT on a database that has sqlite_sequence) */
    sqlite3_int64 seq[2];     /* sqlite_sequence's fingerprint when it was logged */
  } logged;
  sqlite3_stmt *pSchemaStmt;  /* Cached PRAGMA schema_version */

  /* Mirror of _hpplite_meta (the row in the database is authoritative) */
  uint64_t blockHeight;       /* Next batch height to produce */
  unsigned char prevBlockHash[HPPLITE_HASH_SIZE];
  unsigned char chainHash[HPPLITE_HASH_SIZE];  /* the rollup's rolling hash over batches 1..blockHeight-1, as the DA contract keeps it (#33); zero = not known */
  unsigned char stateRoot[HPPLITE_HASH_SIZE];  /* Root after last confirmed batch */
  uint64_t lastCheckpointHeight;
  unsigned char lastCheckpointRoot[HPPLITE_HASH_SIZE];

  /* Cached content hash of the committed state */
  unsigned char curRoot[HPPLITE_HASH_SIZE];
  int bRootDirty;
  struct HppliteTree *pTree;  /* the maintained state trees of the version 2 root (#9) */

  size_t maxStmtBytes;        /* Longest statement the DA can carry (0 = unlimited) */
  int bDeterminismWarn;       /* Non-deterministic SQL: 1 = warn and count, 0 = refuse (default) */
  uint64_t nAuthDenied;       /* Statements the authorizer refused at prepare (our tables, ATTACH, pragmas) */
  const char *zDenyWhy;       /* Why the statement being prepared was denied (for the error message) */
  char zLastRefusal[400];     /* Reason and statement of the last refusal (hpplite_status.last_refusal) */
  char zLastError[512];       /* The last failure said on stderr, until the next success (hpplite_status.last_error, #32) */
  uint64_t nLogErrors;        /* Statements that could not be logged */
  uint64_t nReplaySkipped;    /* Replayed statements that failed and were skipped */
  uint64_t nForeignBatches;   /* L1 heights skipped because the batch there is not ours */
  uint64_t nHooksReplaced;    /* Times our trace/commit/rollback hook was found replaced and re-armed */
  uint64_t nForeignWrites;    /* Bookkeeping looks that found the file changed with no HPPLite write to show for it (#37) */
  unsigned bkDataVersion;     /* The bookkeeping connection's data version after its last transaction */
  int bBkDataVersionValid;
  int nCommitBegun;           /* The application connection's commits: counted at the commit hook
                              ** (before the commit is visible to other connections) ... */
  int nCommitDone;            /* ... and when a statement ends (after it is). Read from the
                              ** bookkeeping thread, written on the application's */
  int commitsDoneAtLook;      /* nCommitDone before the previous look's snapshot */
  int commitsDoneBefore;      /* ... before this one's */
  sqlite3_int64 aBkFinger[3]; /* HPPLite's own state then: pending min(id), max(id), a hash of _hpplite_meta */
  uint64_t nRootsCompared;    /* Posted roots compared with our content hash during replay */
  uint64_t nRootMismatches;   /* ... of which differed */
  uint64_t rootCmpHeight;     /* The last batch whose posted root was compared, and the rebuilt root there (#5) */
  unsigned char rootCmp[32];
  int bTrackRoots;            /* a verifier: the rebuilt root after each of the last HPPLITE_ROOT_RING batches, so an
                              ** on-chain checkpoint that lags the last posted root, or sits at a height with no posted
                              ** root (a retried checkpoint), is still checked (#120) */
#define HPPLITE_ROOT_RING 64
  struct { uint64_t height; unsigned char root[32]; } aRootAt[HPPLITE_ROOT_RING];
  int nRootAt;
  uint64_t nNondeterministic; /* Logged statements using random()/'now'/etc. */

  /* Callbacks for external integration */
  void *pCallbackArg;
  void (*xOnCommit)(void*, unsigned char*, int);  /* root is NULL; use hpplite_get_state_root() */
  void (*xOnRollback)(void*);
};

/*
** Initialize HPPLite tracking on a database connection.
** Creates _hpplite_meta / _hpplite_pending if missing and installs hooks.
** Returns SQLITE_OK on success.
*/
/* Around HPPLite's own opens of a database file (see HPPLITE_FILES, #55) */
void hpplite_internal_open_begin(void);
void hpplite_internal_open_end(void);
int hpplite_internal_open_active(void);

/* Step a write of HPPLite's to its end. With PRAGMA count_changes=1 on the
** connection (deprecated, still supported) an INSERT/UPDATE/DELETE returns
** its change count as a row before SQLITE_DONE */
static inline int hpplite_step_write(sqlite3_stmt *pStmt) {
  int rc;
  while ((rc = sqlite3_step(pStmt)) == SQLITE_ROW) {}
  return rc;
}

int hpplite_init(sqlite3 *db, HppliteCtx **ppCtx);
/* ...for a connection that will capture rows: a SQLite whose sqlite3_expanded_sql()
** cuts bound values (better-sqlite3) is accepted, its text statements may bind nothing (#62) */
int hpplite_init_ex(sqlite3 *db, HppliteCtx **ppCtx, int bRowsCapture);
/* Page mode (#146): the context for a connection whose commits the VFS captures; the
** bookkeeping (meta, posted copies) in bkDb, a sidecar file; no hook on db */
int hpplite_init_pages(sqlite3 *db, sqlite3 *bkDb, HppliteCtx **ppCtx);
/* a page batch posted at height: the meta row advances, the posted copy is kept (#146) */
int hppliteAdvanceMetaPages(HppliteCtx *pCtx, uint64_t height, const unsigned char *posted, size_t nPosted, const unsigned char *root);
/* before a page batch is submitted: the sidecar remembers it, so a process that dies after the submit knows the batch is its own */
int hppliteMarkInflightPages(HppliteCtx *pCtx, uint64_t height, const unsigned char *root);
/* Batches the writer keeps as posted, to post again after a reorg (#49) */
#define HPPLITE_POSTED_KEEP 32
/* A statement cut into pieces (#59): what the flag adds to an entry, what an
** entry's framing costs, the smallest piece worth starting in a batch */
#define HPPLITE_FRAG_FLAG_BYTES 9
#define HPPLITE_FRAG_ENTRY_BYTES 128
#define HPPLITE_FRAG_MIN 512
uint8_t *hpplite_posted_batch(HppliteCtx *pCtx, uint64_t height, size_t *pLen);
/* The rollup this database's history is on: binds it when unbound (1),
** confirms it (0), or refuses another (-1); -2 when it cannot record now */
int hpplite_bind_rollup(HppliteCtx *pCtx, const char *zRollup);
/* Why the last hpplite_init() failed, when it knows (NULL otherwise) */
const char *hpplite_init_error(void);
void hpplite_set_init_error(const char *zWhy);   /* a static string: the reason the open fails */

/*
** Shutdown HPPLite tracking and free resources.
** Does not close db or the bookkeeping connection.
*/
void hpplite_shutdown(HppliteCtx *pCtx);

/*
** Use a second connection to the same file for bookkeeping (reading pending
** rows, computing roots, finalizing batches). This keeps the flush path off
** the application's connection, so a background flush never interferes with
** the application's transaction. Pass NULL to fall back to the main connection.
*/
int hpplite_set_bookkeeping_db(HppliteCtx *pCtx, sqlite3 *bkDb);

/*
** Enable/disable tracking.
*/
void hpplite_enable(HppliteCtx *pCtx, int bEnable);

/*
** Current state root (32 bytes): content hash of committed user tables.
** All zeros for a database with no user schema.
*/
void hpplite_get_state_root(HppliteCtx *pCtx, unsigned char *pOut);
/* A verifier's rebuilt root after batch height, when it is among the last HPPLITE_ROOT_RING replayed
** (bTrackRoots) or is the last posted root compared: 0, or -1 when not kept */
int hpplite_root_at(HppliteCtx *pCtx, uint64_t height, unsigned char out[32]);

/*
** Content hash of any connection's main database, same definition as the
** state root. Excludes sqlite_* and _hpplite_* tables.
*/
int hpplite_db_hash(sqlite3 *db, unsigned char *pOut);
/* Root version 1 (histories from before #9): one hash over every row in order */
int hpplite_db_hash_v1(sqlite3 *db, unsigned char *pOut);
/* Root version 2 with the table roots from xTableRoot (the maintained trees, or a scan) */
typedef int (*HppliteTableRootFn)(void *pArg, sqlite3 *db, const char *zTab, unsigned char out[HPPLITE_HASH_SIZE]);
int hpplite_db_hash_v2(sqlite3 *db, HppliteTableRootFn xTableRoot, void *pArg, unsigned char *pOut);
#define HPPLITE_ROOT_VERSION 2
/* test hooks: rows read for a root (a no-op in other builds) */
void hpplite_test_rows_hashed_add(long n);

/*
** SHA-256 helper (used for state roots, block hashes, batch identity).
*/
void hpplite_sha256(const uint8_t *data, size_t len, uint8_t *out);

/*
** Set callbacks for commit/rollback notifications.
*/
void hpplite_set_callbacks(
  HppliteCtx *pCtx,
  void *pArg,
  void (*xOnCommit)(void*, unsigned char*, int),
  void (*xOnRollback)(void*)
);

/*
** ============================================================================
** Block Builder API (Sequencer-style)
** ============================================================================
*/

/*
** Execute SQL (tracked like any other statement on the connection).
*/
int hpplite_exec(
  HppliteCtx *pCtx,
  const char *zSql,
  char **pzErrMsg
);

/*
** Execute SQL with callback (like sqlite3_exec).
*/
int hpplite_exec_cb(
  HppliteCtx *pCtx,
  const char *zSql,
  int (*xCallback)(void*, int, char**, char**),
  void *pArg,
  char **pzErrMsg
);

/*
** Build a batch from the committed pending SQL, with the content-hash state
** root. Has no side effects: the pending rows stay until
** hpplite_finalize_batch(). *pMaxId receives the id of the last pending row
** included. Returns NULL if nothing is pending.
*/
struct HppliteBatch *hpplite_build_batch(HppliteCtx *pCtx, sqlite3_int64 *pMaxId);

/*
** Like hpplite_build_batch() but keeps the serialized batch within maxBytes
** (0 = unlimited), cutting at a transaction boundary when possible, and
** computes the state root only when withRoot is set. Computing the root is
** O(database size), so the node asks for it only on the batch that closes a
** checkpoint interval. When pending SQL remains after the cut the batch is
** marked partial and carries no root (the last batch of the series does). A
** single statement that cannot fit on its own is left pending and reported
** (returns NULL, nLogErrors++).
*/
struct HppliteBatch *hpplite_build_batch_limited(HppliteCtx *pCtx, size_t maxBytes,
                                                 int withRoot, sqlite3_int64 *pMaxId);

/*
** Total bytes of pending SQL text (cheap size estimate for early flushing).
*/
sqlite3_int64 hpplite_pending_bytes(HppliteCtx *pCtx);

/*
** Mark a batch confirmed: delete its pending rows (id <= maxId) and advance
** height / previous block hash in _hpplite_meta, atomically. The recorded
** state root advances only when the batch carries one.
*/
int hpplite_finalize_batch(HppliteCtx *pCtx, const struct HppliteBatch *pBatch, sqlite3_int64 maxId);

/*
** Build and immediately finalize a batch (no L1). Returns NULL if nothing
** is pending. Caller must free returned batch with hpplite_batch_free().
*/
struct HppliteBatch *hpplite_flush_block(HppliteCtx *pCtx);

/*
** Apply a batch fetched from L1 with tracking disabled, then advance meta.
** The batch height must equal the current block height. With useTxn, each
** transaction group in the batch is applied in its own transaction.
** Returns SQLITE_OK, SQLITE_DONE if already applied, or an error code.
*/
int hpplite_replay_batch(HppliteCtx *pCtx, const struct HppliteBatch *pBatch, int useTxn);

/*
** A batch at the current height already exists on L1 and was produced from
** this file's pending rows (e.g. the process died after submitting but before
** finalizing). If the batch's SQL matches the first pending rows, finalize
** them. Returns SQLITE_OK on adoption, SQLITE_MISMATCH if the batch was not
** produced from these pending rows.
*/
int hpplite_adopt_batch(HppliteCtx *pCtx, const struct HppliteBatch *pBatch);

/*
** Number of committed pending SQL statements.
*/
int hpplite_pending_count(HppliteCtx *pCtx);

/* Row capture: log what moved in sqlite_sequence that no record carries
** (before building a batch; the flusher's) */
int hpplite_rows_seq_flush(HppliteCtx *pCtx);

/*
** Next block height to produce.
*/
uint64_t hpplite_get_block_height(HppliteCtx *pCtx);

/*
** Hash of the previous block.
*/
void hpplite_get_prev_block_hash(HppliteCtx *pCtx, unsigned char *pOut);

/*
** Block hash: sha256(height || prevHash || [stateRoot] || sql...)
*/
void hpplite_block_hash(const struct HppliteBatch *pBatch, const unsigned char *pPrevHash, unsigned char *pOut);

/*
** Persist checkpoint progress.
*/
int hpplite_set_checkpoint(HppliteCtx *pCtx, uint64_t height, const unsigned char *pRoot);

/*
** Step past a height whose L1 batch fails our authentication: it was posted
** under a lease we do not hold and no key holder can read it, so every key
** holder skips it the same way. prev_block_hash becomes the hash of the
** raw bytes, which keeps the chain of hashes deterministic for all of them.
*/
int hpplite_skip_height(HppliteCtx *pCtx, uint64_t height, const unsigned char *pRawHash, const unsigned char *pRawKeccak);

/*
** The DA contract's rolling hash (#33): chainHash = keccak256(abi.encode(prev,
** height, keccak256(bytes))) over every batch from bytes32(0). A node keeps
** it in _hpplite_meta.chain_hash as it finalizes, replays or skips batches,
** and a rebuild compares it with the contract's at the tip: a batch the
** contract never committed to, or one it did that the reader did not get,
** shows there.
*/
void hpplite_chain_hash_next(const unsigned char *pPrev, uint64_t height, const unsigned char *pBatchKeccak, unsigned char *pOut);

/*
** sqlite3_trace_v2 / commit_hook / rollback_hook are one slot per connection:
** an app, ORM or debugger that installs its own silently ends capture. This
** re-installs ours, counts replacements (hpplite_status.hooks_replaced) and
** says so once. Called from every HPPLite entry point that runs on the
** application thread (status reads, hpplite_flush(), hpplite_sync(), close).
*/
void hpplite_rearm_hooks(HppliteCtx *pCtx);

/* A failure: one line on stderr ("hpplite: ..."), kept as
** hpplite_status.last_error until hpplite_error_clear() at the next success (#32) */
void hpplite_error(HppliteCtx *pCtx, const char *zFmt, ...);
void hpplite_error_clear(HppliteCtx *pCtx);
/* A follower reloads the meta row when the file's height moved (#100) */
void hpplite_refresh_meta(HppliteCtx *pCtx);
#ifdef HPPLITE_TEST_HOOKS
extern int hpplite_test_fail_commits;   /* mock builds: the next n bookkeeping COMMITs fail as SQLITE_FULL */
extern long hpplite_test_rows_hashed;   /* rows read for a state root since the counter was last zeroed (#9) */
/* called after a VACUUM released the file and before its marker is logged (#75): another connection's turn */
extern void (*hpplite_test_before_vacuum_log)(void *pArg);
extern void *hpplite_test_before_vacuum_log_arg;
/* called as the state trees take in the first log row of a sync (#141) */
extern void (*hpplite_test_in_tree_sync)(void *pArg);
extern void *hpplite_test_in_tree_sync_arg;
#endif

/*
** Register HPPLite as a loadable extension.
*/
#ifdef HPPLITE_EXTENSION
int sqlite3_hpplite_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
);
#endif

/*
** ============================================================================
** Simple Open API (M3 - URI-based configuration)
** ============================================================================
**
** Usage:
**   sqlite3 *db = hpplite_open("file:state.db?hpplite=on&l1=hpp-sepolia&privkey=0x...");
**
** URI Parameters:
**   hpplite=on        Enable HPPLite (required)
**   l1=hpp-sepolia    L1 network alias (sets rpc + factory)
**   rpc=https://...   L1 RPC endpoint
**   factory=0x...     Factory contract (auto-creates a rollup per wallet)
**   contract=0x...    Existing rollup contract
**   privkey=0x...     Private key (or keyfile=/path)
**   interval=5s       Batch interval (default: 5s)
**   datadir=/path     Data directory
**
** Environment variables (lower priority than URI):
**   HPPLITE_PRIVATE_KEY, HPPLITE_L1_RPC, HPPLITE_FACTORY_ADDRESS, etc. (see config.h)
**
** On open HPPLite switches a file database to WAL mode and sets a 5 s busy
** timeout, so bookkeeping on the second connection never blocks the
** application and vice versa.
*/

/* Forward declaration */
struct HppliteConfig;

/*
** Open database with HPPLite enabled via URI parameters.
** Returns sqlite3* handle or NULL on error.
**
** The returned handle works like normal sqlite3* - just use sqlite3_exec(), etc.
** Batches are created automatically based on the interval setting.
**
** When done, either sqlite3_close(db) or hpplite_close(db) will work.
** The close hook automatically flushes pending batches and cleans up.
*/
sqlite3 *hpplite_open(const char *uri);

/*
** Close HPPLite database.
** This is equivalent to sqlite3_close() when HPPLite close hook is registered.
** Kept for API compatibility.
*/
int hpplite_close(sqlite3 *db);

/*
** Get HPPLite context from an open database.
** Returns NULL if HPPLite is not active on the connection.
*/
HppliteCtx *hpplite_context(sqlite3 *db);

/*
** Get the configuration for an open database.
** Returns NULL if database was not opened with hpplite_open().
*/
struct HppliteConfig *hpplite_get_config(sqlite3 *db);

/* Forward declaration for L1 interface */
struct HppliteL1;
typedef struct HppliteL1 HppliteL1;

/*
** Get the L1 connection for an open database.
** Returns NULL if L1 was not configured or the connection failed.
*/
HppliteL1 *hpplite_get_l1(sqlite3 *db);

/*
** Get the node for an open database.
** Returns NULL if db is not an HPPLite database.
*/
struct HppliteNode *hpplite_get_node(sqlite3 *db);

/*
** Register a node with the global registry and set up SQL functions.
** Called automatically by hpplite_node_create() and hpplite_node_create_with_db().
*/
void hpplite_register_node(sqlite3 *db, struct HppliteNode *node);

/* The registered node sequencing this file (by canonical path), or NULL */
struct HppliteNode *hpplite_find_sequencer(const char *zCanonicalPath);

/*
** Remove a connection from the registry (no-op if not registered).
** hpplite_node_destroy() calls this before closing a database it owns, so
** the close does not re-enter the node teardown.
*/
void hpplite_unregister_node(sqlite3 *db);

/*
** Force flush pending changes to a batch immediately.
** Returns batch height, or 0 if nothing was pending or the batch could not
** be confirmed on L1 (it stays pending and is retried).
*/
uint64_t hpplite_flush(sqlite3 *db);

/*
** Same as hpplite_flush(); kept for API compatibility.
*/
uint64_t hpplite_check_flush(sqlite3 *db);

/*
** Get current state root.
*/
void hpplite_state_root(sqlite3 *db, unsigned char *out);

/*
** DEPRECATED: HPPLite auto-registers via a constructor when the library is
** loaded. These functions are retained for backwards compatibility and do
** nothing.
*/
void hpplite_register(void);
void hpplite_unregister(void);

/*
** Pubkey size for signatures
*/
#define HPPLITE_PUBKEY_SIZE 33

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_H */
