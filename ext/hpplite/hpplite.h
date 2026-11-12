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
typedef struct HppliteCtx HppliteCtx;
struct HppliteCtx {
  sqlite3 *db;                /* Application connection: trace + pending log */
  sqlite3 *bkDb;              /* Bookkeeping connection (== db for :memory:) */
  int bEnabled;               /* Tracking enabled flag */
  int bPages;                 /* page mode (#146): no commit hook on the application's connection, the bookkeeping in a sidecar */
  char *zWalKey;              /* page mode: the file's canonical path, the WAL reader's key (#155) */
  void (*xWalMaint)(void *);  /* page mode: the node's WAL maintenance (pins, checkpoint) when no timer runs it; NULL: SQLite's own checkpoint */
  void *pWalMaintArg;
  int bWalHookFired;          /* page mode: the wal hook ran during this statement (else its end polls the WAL) */
  uint64_t inflightHeight;    /* page mode: a batch posted and not yet finalized (0: none), with its root; survives the process */
  unsigned char inflightRoot[32];
  int bInternal;              /* Set while HPPLite runs its own statements */
  int bDbClosed;              /* db is a zombie: no SQLite calls on it any more */
  int bAppFlushing;           /* hpplite_flush() is running on the application thread */
  volatile int nAppStmtActive; /* Application statements between their STMT and PROFILE trace
                               ** events (mid-step). Written on the application thread only;
                               ** the timer thread reads it instead of touching db. */
  int bStatusGone;            /* hpplite_status was disconnected by a close that failed */
  sqlite3_int64 openTxnId;    /* Writer: log id of the group the last posted batch left open (#57) */
  char zRollup[80];           /* "<chain id>:<contract>" the history is on ("" = not bound yet) */
  sqlite3_int64 fragId;       /* Writer: log id of the statement the last posted batch ended a piece of (#59) */
  size_t fragOff;             /* ...bytes of its text posted so far */
  int nCarry;                 /* Replica: statements held back from an open group (_hpplite_carry) */
  uint64_t nTxnsDropped;      /* Replica: transactions dropped because their other part never came */
  int seenSchema;                /* schema_version this connection last loaded (-1: not yet) */
  volatile int bAppTxnOpen;   /* The application connection is inside a transaction (app thread
                              ** writes, the timer thread reads: no bookkeeping commit then) */
 /* Log rows whose statement did not read
                              ** the clock: the batch leaves their time out (#56) */
  volatile int txnGate;       /* 0 free, 1 the application (a statement or transaction open), 2 a
                              ** bookkeeping write: its commit never lands inside the application's */
  int bBkGate;                /* this bookkeeping transaction took the gate */
  volatile int bBkWantsGate;  /* a bookkeeping write is waiting: the application hands the gate over
                              ** when it goes idle instead of freeing it (#141) */

  sqlite3_stmt *pVacuumStmt;  /* A VACUUM running: the ATTACH inside it is its own */
  int vacuumFd;               /* <file>-hpplite-vacuum (#75): a VACUUM holds it exclusively from its start until its
                              ** marker is logged; a captured statement holds it shared while it runs. -1: none */
  char *zDbName;              /* This database's name among its wallet's rollups (#61), or NULL */

  /* Mirror of _hpplite_meta (the row in the database is authoritative) */
  uint64_t blockHeight;       /* Next batch height to produce */
  unsigned char prevBlockHash[HPPLITE_HASH_SIZE];
  unsigned char chainHash[HPPLITE_HASH_SIZE];  /* the rollup's rolling hash over batches 1..blockHeight-1, as the DA contract keeps it (#33); zero = not known */
  unsigned char stateRoot[HPPLITE_HASH_SIZE];  /* Root after last confirmed batch */
  uint64_t lastCheckpointHeight;
  unsigned char lastCheckpointRoot[HPPLITE_HASH_SIZE];
  const struct HppliteAnchorRow *pAnchorRow;   /* the batch being finalized is at a store, not yet anchored (#164): its row for _hpplite_unanchored */

  /* Cached content hash of the committed state */
  int bRootDirty;

  uint64_t nAuthDenied;       /* Statements the authorizer refused at prepare (our tables, ATTACH, pragmas) */
  const char *zDenyWhy;       /* Why the statement being prepared was denied (for the error message) */
  char zLastRefusal[400];     /* Reason and statement of the last refusal (hpplite_status.last_refusal) */
  char zLastError[512];       /* The last failure said on stderr, until the next success (hpplite_status.last_error, #32) */
  int quiet;                  /* reading what is not this rollup (the named lookup, #228): failures are not said, only kept here: */
  char zQuietError[256];
  uint64_t nLogErrors;        /* Statements that could not be logged */
  uint64_t nReplaySkipped;    /* Replayed statements that failed and were skipped */
  uint64_t nForeignBatches;   /* L1 heights skipped because the batch there is not ours */
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

  /* Callbacks for external integration */
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

/* Page mode (#146): the context for a connection whose commits the VFS captures; the
** bookkeeping (meta, posted copies) in bkDb, a sidecar file; no hook on db */
int hpplite_init_pages(sqlite3 *db, sqlite3 *bkDb, HppliteCtx **ppCtx);
/* a page batch posted at height: the meta row advances, the posted copy is kept (#146) */
int hppliteAdvanceMetaPages(HppliteCtx *pCtx, uint64_t height, const unsigned char *posted, size_t nPosted, const unsigned char *root, int bSeriesOpen);   /* bSeriesOpen: a commit cut over batches has pieces still to come (#152) */
/* before a page batch is submitted: the sidecar remembers it, so a process that dies after the submit knows the batch is its own */
int hppliteMarkInflightPages(HppliteCtx *pCtx, uint64_t height, const unsigned char *root);
int hppliteSetMetaAt(HppliteCtx *pCtx, uint64_t height, const unsigned char *block, const unsigned char *chain, const unsigned char *root);
int hppliteResetMeta(HppliteCtx *pCtx);   /* the bookkeeping as a fresh file's: the file it described is gone (#330) */   /* the bookkeeping as of a height a snapshot carries (#222) */
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
** Current state root (32 bytes): content hash of committed user tables.
** All zeros for a database with no user schema.
*/
void hpplite_get_state_root(HppliteCtx *pCtx, unsigned char *pOut);

/* A batch the store holds and the chain has not anchored yet (#164): what
** the finalize writes to _hpplite_unanchored with the height, so the anchor
** can name it and a restart knows what is owed to the chain. */
typedef struct HppliteAnchorRow {
  unsigned char hash[HPPLITE_HASH_SIZE];   /* keccak256 of the posted bytes, as the chain hash folds it */
  const char *zRef;                        /* the store's reference */
  const unsigned char *receipt;            /* the store's compact receipt, or NULL */
  size_t nReceipt;
} HppliteAnchorRow;
/* A verifier's rebuilt root after batch height, when it is among the last HPPLITE_ROOT_RING replayed
** (bTrackRoots) or is the last posted root compared: 0, or -1 when not kept */
int hpplite_root_at(HppliteCtx *pCtx, uint64_t height, unsigned char out[32]);

/*
** Content hash of any connection's main database, same definition as the
** state root. Excludes sqlite_* and _hpplite_* tables.
*/
int hpplite_db_hash(sqlite3 *db, unsigned char *pOut);
int hpplite_db_hash_v1(sqlite3 *db, unsigned char *pOut);   /* what hpplite_db_hash is */

/*
** SHA-256 helper (used for state roots, block hashes, batch identity).
*/
void hpplite_sha256(const uint8_t *data, size_t len, uint8_t *out);


/*
** ============================================================================
** Block Builder API (Sequencer-style)
** ============================================================================
*/



/*
** Next block height to produce.
*/
uint64_t hpplite_get_block_height(HppliteCtx *pCtx);

/*
** Hash of the previous block.
*/
void hpplite_get_prev_block_hash(HppliteCtx *pCtx, unsigned char *pOut);

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
** The authorizer, the trace and the wal hook are one slot each per connection:
** an app, ORM or debugger that installs its own takes them over. This puts
** ours back, from every HPPLite entry point that runs on the application
** thread (status reads, hpplite_flush(), hpplite_sync(), close).
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

/* Anchor on the chain every batch the store holds since the last anchor (#164): the
** last height anchored, 0 when there was nothing to anchor or the chain did not take it */
uint64_t hpplite_anchor(sqlite3 *db);

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
