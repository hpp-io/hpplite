/*
** HPPLite Node - Singleton Sequencer Implementation
**
** Single sequencer model - all writes go through sequencer.
** L1 contract handles coordination (lease/lock mechanism).
** No peer-to-peer networking - all data flows through L1.
*/

#ifndef HPPLITE_NODE_H
#define HPPLITE_NODE_H

#include "hpplite.h"
#include "batch.h"
#include "crypto.h"
#include "l1_interface.h"
#include "cipher.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** Node state
*/
typedef enum {
    HPPLITE_STATE_INIT = 0,
    HPPLITE_STATE_RUNNING,
    HPPLITE_STATE_SYNCING,
    HPPLITE_STATE_STOPPED,
    HPPLITE_STATE_ERROR
} HppliteNodeState;

/*
** Node configuration
*/
typedef struct HppliteNodeConfig {
    /* Identity */
    char *nodeId;                   /* Human-readable node ID */
    unsigned char privkey[32];      /* Node's private key */
    int hasPrivkey;                 /* Whether privkey is set */

    /* Paths */
    char *dataDir;                  /* Data directory */
    char *dbPath;                   /* SQLite database path */
    char *dbName;                   /* Its own rollup among the wallet's, by this name (#61);
                                    ** "{file}": the database file's name */

    /* L1 Connection */
    char *rpcUrl;                   /* L1 RPC endpoint */
    unsigned char factory[20];      /* Factory contract address */
    int hasFactory;                 /* Whether factory is set */
    unsigned char contract[20];     /* Direct contract address */
    int hasContract;                /* Whether contract is set */

    /* Sequencer Settings */
    int checkpointInterval;         /* Batches between L1 checkpoints */
    int batchIntervalMs;            /* Auto-flush interval (0 = disabled) */
    uint64_t chainId;               /* L1 chain id for signing (0 = library default, HPP Sepolia) */
    size_t maxBatchBytes;           /* DA size limit (0 = from L1 / default 128 KB) */
    int noEncrypt;                  /* Post plaintext batches (default: sealed when a key exists) */
    int noCompress;                 /* compress=off: post the batch JSON as is (#12); default: deflate when it shrinks */
    int determinismWarn;            /* Warn about non-deterministic SQL instead of refusing it */
    int captureText;                /* Capture statements, as v0.1 (default 0: rows, #19) */
    int captureSource;              /* ...2 when a URI said so: HPPLITE_CAPTURE does not override it (#80) */
    int localOnly;                  /* l1=local: finalize batches locally, keep no history (#38) */
    int verifyOnly;                 /* role=verify: rebuild and compare, never post, claim or create (#5) */
    size_t maxTxBytes;              /* Largest batch per transaction (0 = by chain id, else unknown) */
    char *storeUrl;                 /* store=: where batch bytes live instead of the chain (#11); NULL = on chain */
    uint64_t storeCap;              /* storecap=: the most to pay a store per call, atomic units (0 = pay nothing) */
    int storeFallback;              /* storefallback=: a store that does not answer lets the chain carry the bytes (default on) */
    uint64_t maxTxGas;              /* Gas cap per transaction (0 = by chain id, else unknown) */
} HppliteNodeConfig;

/*
** Main node structure (simplified - sequencer only)
*/
typedef struct HppliteNode {
    /* Configuration */
    HppliteNodeConfig config;

    /* State */
    HppliteNodeState state;

    /* Instance ID for lease (unique per process) */
    unsigned char instanceId[32];
    int hasLease;                   /* We claimed the lease and have not released it */
    int64_t leaseRecheckAt;         /* Next time to read the lease from L1 (ms, monotonic) */
    uint64_t leaseDuration;         /* Seconds, learned from the contract (default 300) */
    int warnedForeignLease;         /* "someone else holds the lease" said once */
    int leaseHeldElsewhere;         /* The last look found another instance's lease (hpplite_status.state = waiting) */
    int lastFetchForeign;           /* The last fetch found a batch that fails our authentication */
    int failStreak;                 /* Consecutive flushes that did not reach L1 */
    uint8_t *postedCopy;            /* The bytes of the batch being finalized (kept for a reorg, #49) */
    int wrongRollup;                /* The database's history is on another rollup: never post or replay */
    int64_t retryAt;                /* Timer does not retry before this (ms, monotonic) */

    /* Database and context */
    sqlite3 *db;
    sqlite3 *bkDb;                  /* Bookkeeping connection (NULL for :memory:) */
    HppliteCtx *ctx;

    /* L1 connection */
    HppliteL1 *l1;
    struct HppliteStore *store;     /* the batch bytes' store when store= is given (#11); the chain commits hash, ref, receipt */
    int warnedStoreDown;            /* said once per outage that batches go on the chain meanwhile */
    uint8_t storeBalance[32];       /* the wallet's balance of the store's asset, as last read (status row); 30 s old at most */
    int storeBalanceOk;
    long storeBalanceAt;            /* seconds (time()) */

    /* Crypto */
    HppliteCrypto *crypto;
    HppliteKeypair keypair;

    /* Batch encryption: key derived from the wallet key, NULL cipher = plaintext */
    const HppliteCipher *cipher;
    unsigned char encKey[HPPLITE_CIPHER_KEY_SIZE];
    int hasEncKey;
    int warnedNoKey;                /* "no key, not posting" said once */
    int wantsL1;                    /* Config named an L1 (contract/factory): local-only flush is not OK */
    char lastBatchTx[80];           /* Transaction hash of the last confirmed batch (status table) */
    int warnedNoL1;                 /* "L1 requested but not connected" said once */
    int warnedNoChain;              /* "no chain configured, the log is kept" said once (#38) */
    int checkpointGiven;            /* The configuration named an interval (else the contract's is used, #44) */
    int checkpointSynced;           /* ... and it was compared with the contract's once */

    /* Checkpoint tracking */
    uint64_t lastCheckpointHeight;
    unsigned char lastCheckpointRoot[HPPLITE_HASH_SIZE];
    int batchesSinceCheckpoint;

    /* Callbacks */
    void *callbackArg;
    void (*onBatchPosted)(void *arg, uint64_t height, const char *txHash);
    void (*onCheckpointSubmitted)(void *arg, uint64_t fromHeight, uint64_t toHeight, const char *txHash);

    /* Statistics */
    uint64_t batchesProduced;

    /* Auto-flush timer thread */
    void *timerThread;              /* pthread_t (opaque) */
    void *flushMutex;               /* pthread_mutex_t */
    volatile int timerRunning;
    void *timerWake;                /* pthread_cond_t: stop wakes the timer at once */
    void *timerWakeMutex;           /* pthread_mutex_t for it */
    int64_t lastFlushTime;

    /* Ownership flags */
    int ownsDb;
    int ownsL1;

    /* One sequencer per file: later hpplite=on connections to the same file,
    ** in this process or another, capture into the shared log and post
    ** nothing. Across processes the sequencer is whoever holds an flock on
    ** <file>-hpplite; a follower takes over when it is gone. */
    char *canonicalPath;
    int follower;
    int lockFd;                     /* the held lock file, -1 if none */
    int usersFd;                    /* shared lock on <file>-hpplite-users while open, -1 if none */
    int64_t promoteAt;              /* follower: next try to take over */
} HppliteNode;

/*
** Create a node with configuration.
** Returns NULL on failure.
*/
HppliteNode *hpplite_node_create(const HppliteNodeConfig *config);

/*
** Create a node using an existing database connection.
** The node does NOT own the db (won't close it on destroy).
*/
HppliteNode *hpplite_node_create_with_db(sqlite3 *db, const HppliteNodeConfig *config);

/*
** Free a node and all resources.
*/
void hpplite_node_destroy(HppliteNode *node);

/*
** Start auto-flush timer thread.
** Periodically flushes pending SQL based on config.batchIntervalMs.
*/
void hpplite_node_start_timer(HppliteNode *node);

/*
** Stop auto-flush timer thread.
*/
void hpplite_node_stop_timer(HppliteNode *node);

/*
** Claim sequencer lease on L1.
** Returns 0 on success, -1 if lease unavailable.
*/
int hpplite_node_claim_lease(HppliteNode *node);

/*
** Renew sequencer lease on L1.
** Returns 0 on success, -1 on failure.
*/
int hpplite_node_renew_lease(HppliteNode *node);

/*
** Release our sequencer lease on L1 (called on graceful shutdown so a
** restart does not have to wait for the lease to expire).
** Returns 0 on success, -1 on failure.
*/
int hpplite_node_release_lease(HppliteNode *node);

/*
** Check if our lease is still active.
** Returns 1 if active, 0 if expired/not held.
*/
int hpplite_node_has_lease(HppliteNode *node);

/* The wallet's balance of the store's asset on the chain (#11), 32 big-endian bytes, read at
** most every 30 s and after every put. -1 without a store that charges, a chain it is on, or a key. */
int hpplite_node_store_balance(HppliteNode *node, uint8_t out[32]);

/*
** Start the node (claim lease and begin operations).
*/
int hpplite_node_start(HppliteNode *node);

/*
** Stop the node.
*/
void hpplite_node_stop(HppliteNode *node);

/*
** Execute SQL (queued for next batch).
*/
int hpplite_node_exec(HppliteNode *node, const char *sql, char **errMsg);

/*
** Flush pending SQL to create a batch and post to L1. Several batches are
** produced when the DA size limit splits the pending SQL. When the batch
** closes a checkpoint interval it carries the state root and the checkpoint
** is submitted right after it.
** Returns the last confirmed height, or 0 if nothing was pending or the
** batch could not be confirmed on L1 (pending rows are kept and retried).
*/
uint64_t hpplite_node_flush_batch(HppliteNode *node);

/*
** Fetch the batch at `height` from L1 and decode it: opens the envelope
** with this node's key when the batch is sealed, parses plaintext JSON
** otherwise. NULL if absent, if sealed and this node has no key, or if the
** key does not open it. Replay, adoption and tests all go through this.
*/
HppliteBatch *hpplite_node_fetch_batch(HppliteNode *node, uint64_t height);

/*
** Bring the local database up to date with L1: batches already on L1 that
** match local pending rows are finalized; batches beyond local history are
** replayed. With useTxn, each transaction group is replayed in its own
** transaction (use 0 when called from inside a statement).
** Returns the number of batches processed, or -1 if local and L1 history
** have diverged.
*/
int hpplite_node_sync_from_l1(HppliteNode *node, int useTxn);

/*
** Get number of pending SQL statements.
*/
int hpplite_node_pending_count(HppliteNode *node);

/*
** Check if node should create a checkpoint.
** Returns 1 if checkpoint interval reached.
*/
int hpplite_node_should_checkpoint(HppliteNode *node);

/*
** Create and submit checkpoint to L1.
** Returns transaction hash or NULL on error.
*/
char *hpplite_node_submit_checkpoint(HppliteNode *node);

/*
** Set the L1 connection for this node.
*/
void hpplite_node_set_l1(HppliteNode *node, HppliteL1 *l1);

/* The key given after open (#17): the node signs, seals and pays with it from
** here on; a chain the configuration named but could not be reached without
** a key is connected now, the rollup resolved, the lease taken. 0 keyed;
** -1 not a key; -2 keyed already with another key (rotation is not this). */
int hpplite_node_set_key(HppliteNode *node, const unsigned char privkey[32]);

/*
** Get current block height.
*/
uint64_t hpplite_node_get_height(HppliteNode *node);

/*
** Get current state root.
*/
void hpplite_node_get_state_root(HppliteNode *node, unsigned char *out);

/*
** Get node's public key.
*/
void hpplite_node_get_pubkey(HppliteNode *node, unsigned char *out);

/*
** Get node's Ethereum address.
*/
void hpplite_node_get_address(HppliteNode *node, unsigned char *out);

/*
** Get node's instance ID (32 bytes).
*/
void hpplite_node_get_instance_id(HppliteNode *node, unsigned char *out);

/*
** Get node statistics as JSON string.
** Caller must free with sqlite3_free().
*/
char *hpplite_node_get_stats(HppliteNode *node);

/*
** Register node with global registry for SQL functions.
** Called internally during node creation.
*/
void hpplite_register_node(sqlite3 *db, HppliteNode *node);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_NODE_H */
