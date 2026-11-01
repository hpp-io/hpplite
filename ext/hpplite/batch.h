/*
** HPPLite Batch - Transaction batching for L2
**
** A batch packages multiple SQL transactions with their
** state transition (pre/post state roots).
*/

#ifndef HPPLITE_BATCH_H
#define HPPLITE_BATCH_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HPPLITE_HASH_SIZE 32

/*
** A single SQL transaction within a batch
*/
typedef struct HppliteTxn HppliteTxn;
struct HppliteTxn {
  char *zSql;              /* SQL statement */
  int64_t nRowsAffected;   /* Number of rows affected (-1 if unknown) */
  int64_t txn;             /* Transaction group within the batch (0, 1, ...) */
  int mode;                /* Connection state the statement ran under (HPPLITE_MODE_*) */
  int64_t now;             /* The time it read, Julian day ms (0 = it did not read the clock; #56) */
  int frag;                /* A piece of a statement that continues in the next entry/batch (#59) */
};

/*
** Connection state that changes what a statement does to other rows without
** appearing in its text (#52). Recorded per statement, applied on replay.
*/
#define HPPLITE_MODE_FOREIGN_KEYS        1   /* PRAGMA foreign_keys: cascades, SET NULL/DEFAULT */
#define HPPLITE_MODE_RECURSIVE_TRIGGERS  2   /* PRAGMA recursive_triggers */
#define HPPLITE_MODE_ROW                 4   /* not SQL: a row record (#19), applied with triggers off;
                                            ** a version without row capture refuses the batch */
/* Connection pragmas that change what a statement does without showing in
** its text (#28); tracked from the application's PRAGMA statements */
#define HPPLITE_MODE_LEGACY_ALTER        8   /* PRAGMA legacy_alter_table: RENAME leaves references as they are */
#define HPPLITE_MODE_WRITABLE_SCHEMA    16   /* PRAGMA writable_schema: writes to sqlite_schema */
#define HPPLITE_MODE_IGNORE_CHECKS      32   /* PRAGMA ignore_check_constraints */
#define HPPLITE_MODE_CASE_LIKE          64   /* PRAGMA case_sensitive_like */
#define HPPLITE_MODE_PRAGMAS (HPPLITE_MODE_LEGACY_ALTER | HPPLITE_MODE_WRITABLE_SCHEMA | HPPLITE_MODE_IGNORE_CHECKS | HPPLITE_MODE_CASE_LIKE)
#define HPPLITE_MODE_KNOWN (HPPLITE_MODE_FOREIGN_KEYS | HPPLITE_MODE_RECURSIVE_TRIGGERS | HPPLITE_MODE_ROW | HPPLITE_MODE_PRAGMAS)

/*
** A batch of transactions with state transition
*/
typedef struct HppliteBatch HppliteBatch;
struct HppliteBatch {
  uint64_t height;                           /* Sequential batch number */
  uint64_t timestamp;                        /* Unix timestamp (seconds) */
  unsigned char preStateRoot[HPPLITE_HASH_SIZE];  /* State before batch */
  unsigned char postStateRoot[HPPLITE_HASH_SIZE]; /* State after batch */
  int nTxns;                                 /* Number of transactions */
  HppliteTxn *aTxns;                         /* Array of transactions */
  int partial;                               /* More pending SQL follows in the next batch */
  int openTxn;                               /* Last group continues in the next batch (#57) */
  int contTxn;                               /* First group continues the previous batch's open one */
  int64_t openGroup;                         /* Writer only, not posted: log id of the open group */
  const uint8_t *pPosted;                    /* Writer only: the bytes posted for it (kept for a reorg) */
  size_t nPosted;
  int bOwnsPosted;                           /* ... malloc'd with the batch (a fetch): freed with it (#99) */
  int64_t fragRow;                           /* Writer only: log id of the statement it ends a piece of */
  size_t fragOff;                            /* ...and how many bytes of its text are posted after it */
  int hasRoot;                               /* postStateRoot is set (batch closes a checkpoint interval) */
  int rootVersion;                           /* how postStateRoot was computed: 0/1 one hash over every row, 2 per-table trees (#9) */
  char *zDbName;                             /* Batch 1 of a named database: the name (#61), else NULL */
};

/*
** Checkpoint - covers a range of batches
** Posted to L1 by the sequencer
*/
typedef struct HppliteCheckpoint HppliteCheckpoint;
struct HppliteCheckpoint {
  uint64_t fromHeight;                       /* First batch in checkpoint */
  uint64_t toHeight;                         /* Last batch in checkpoint */
  unsigned char preStateRoot[HPPLITE_HASH_SIZE];  /* State before fromHeight */
  unsigned char postStateRoot[HPPLITE_HASH_SIZE]; /* State after toHeight */
  unsigned char batchesHash[HPPLITE_HASH_SIZE];   /* Hash of all batch refs */
  uint64_t timestamp;                        /* When checkpoint created */
};

/*
** Finality levels for batches
*/
typedef enum {
  HPPLITE_FINALITY_NONE = 0,      /* Not yet produced */
  HPPLITE_FINALITY_SEQUENCED,     /* Sequencer produced */
  HPPLITE_FINALITY_L1             /* Committed to L1 */
} HppliteFinalityLevel;

/*
** Create a new empty batch
*/
HppliteBatch *hpplite_batch_new(uint64_t height);

/*
** The longest prefix of zIn whose JSON-escaped size is at most budget,
** ending on a UTF-8 character boundary (a statement cut into pieces, #59).
*/
size_t hpplite_json_prefix_fit(const char *zIn, size_t budget);

/*
** Free a batch and all its transactions
*/
void hpplite_batch_free(HppliteBatch *pBatch);

/*
** Add a statement to a batch as its own transaction group
*/
int hpplite_batch_add_txn(HppliteBatch *pBatch, const char *zSql, int64_t nRowsAffected);

/*
** Add a statement to a batch with an explicit transaction group.
** Statements sharing a group are replayed inside one transaction.
*/
int hpplite_batch_add_stmt(HppliteBatch *pBatch, const char *zSql, int64_t nRowsAffected, int64_t txn);

/*
** Set pre-state root
*/
void hpplite_batch_set_pre_state(HppliteBatch *pBatch, const unsigned char *pRoot);

/*
** Set post-state root
*/
void hpplite_batch_set_post_state(HppliteBatch *pBatch, const unsigned char *pRoot);

/*
** Serialize batch to JSON string
** Caller must free returned string with sqlite3_free()
*/
char *hpplite_batch_to_json(const HppliteBatch *pBatch);

/*
** Parse batch from JSON string
** Caller must free returned batch with hpplite_batch_free()
*/
HppliteBatch *hpplite_batch_from_json(const char *zJson);

/* === Checkpoint Functions === */

/*
** Create a new checkpoint
*/
HppliteCheckpoint *hpplite_checkpoint_new(
  uint64_t fromHeight,
  uint64_t toHeight
);

/*
** Free a checkpoint
*/
void hpplite_checkpoint_free(HppliteCheckpoint *pCheckpoint);

/*
** Serialize checkpoint to JSON
** Caller must free returned string with sqlite3_free()
*/
char *hpplite_checkpoint_to_json(const HppliteCheckpoint *pCheckpoint);

/*
** Parse checkpoint from JSON
** Caller must free returned checkpoint with hpplite_checkpoint_free()
*/
HppliteCheckpoint *hpplite_checkpoint_from_json(const char *zJson);

/*
** Compute hash for checkpoint signing
** hash = keccak256(fromHeight || toHeight || postStateRoot || batchesHash)
*/
void hpplite_checkpoint_hash(
  const HppliteCheckpoint *pCheckpoint,
  unsigned char *pHashOut  /* 32 bytes */
);

/*
** Number of bytes a statement occupies in the batch JSON (escaped text plus
** the per-statement framing). Used to cut batches at the DA size limit
** without serializing.
*/
size_t hpplite_batch_stmt_json_size(const char *zSql, int64_t nRowsAffected, int64_t txn);

/*
** Bytes of batch JSON outside the statements (header, roots, brackets).
*/
size_t hpplite_batch_json_overhead(void);

/*
** Helper: convert bytes to hex string
** Caller must provide buffer of at least (len*2 + 1) bytes
*/
void hpplite_to_hex(const unsigned char *pData, int len, char *zOut);

/*
** Helper: convert hex string to bytes
** Returns 0 on success, -1 on error
*/
int hpplite_from_hex(const char *zHex, unsigned char *pOut, int len);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_BATCH_H */
