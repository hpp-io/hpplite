/*
** HPPLite Node - Singleton Sequencer Implementation
**
** Single sequencer model - all writes go through sequencer.
** L1 contract handles coordination (lease/lock mechanism).
** No peer-to-peer networking - all data flows through L1.
**
** Durability: the node keeps nothing in memory that is not also in the
** database. Pending SQL lives in _hpplite_pending (written inside the user's
** own transactions), height/roots in _hpplite_meta. Bookkeeping runs on a
** second connection so the background flush never touches the application's
** transaction state.
*/

#include "node.h"
#include "envelope_pk.h"
#include "hpplite_int.h"
#include "store.h"
#include "compress.h"
#include "pagemode.h"
#include "pages.h"
#include "walread.h"

static int node_apply_pages(HppliteNode *node, const HppliteBatch *batch);
static int node_start_pages(HppliteNode *node, const char *zFile);
static int node_adopt_pages(HppliteNode *node, const HppliteBatch *batch);
static void node_ck_open(HppliteNode *node);
static void node_ck_close(HppliteNode *node);
static void node_pins_close(HppliteNode *node);
static void node_wal_maintenance(void *pArg);
static int node_ranges(HppliteNode *node);
static int node_anchor(HppliteNode *node);
static uint64_t node_unanchored_above(HppliteNode *node, uint64_t lastBatch);
static uint8_t *node_fetch_from_range(HppliteNode *node, HppliteL1 *l1, uint64_t height, size_t *pLen);
static int node_switch_to_rows(HppliteNode *node);
static int node_check_rollup(HppliteNode *node);
static void node_checkpoint_wal(HppliteNode *node, int64_t now);
#include "da_uri.h"
#include "capture_rows.h"
#include "cJSON.h"
static void node_cap_batch_limit(HppliteNode *node);
static uint8_t *node_fetch_bytes(HppliteNode *node, HppliteL1 *l1, uint64_t height, size_t *pLen);
static int node_check_rollup(HppliteNode *node);
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/file.h>
#include <fcntl.h>
#include "eth/keccak256.h"

/*
** Helper: duplicate a string using sqlite3_malloc
*/
static char *node_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *dup = sqlite3_malloc((int)len);
    if (dup) memcpy(dup, s, len);
    return dup;
}

/*
** Helper: get current time in milliseconds
*/
static int64_t node_current_time_ms(void) {
    /* Monotonic: a wall-clock step must not delay or double a flush */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
** Generate a unique instance ID for this process.
** Combines: timestamp + pid + random bytes
*/
static void generate_instance_id(unsigned char *out) {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    /* First 8 bytes: timestamp */
    uint64_t ts = (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    for (int i = 0; i < 8; i++) {
        out[i] = (ts >> (56 - i * 8)) & 0xFF;
    }

    /* Next 4 bytes: PID */
    pid_t pid = getpid();
    out[8] = (pid >> 24) & 0xFF;
    out[9] = (pid >> 16) & 0xFF;
    out[10] = (pid >> 8) & 0xFF;
    out[11] = pid & 0xFF;

    /* Remaining 20 bytes: random */
    FILE *urandom = fopen("/dev/urandom", "rb");
    size_t got = 0;
    if (urandom) {
        got = fread(out + 12, 1, 20, urandom);
        fclose(urandom);
    }
    if (got < 20) {
        /* Fallback: use more timestamp bits */
        for (int i = 12; i < 32; i++) {
            out[i] = (unsigned char)(rand() ^ (tv.tv_usec >> (i % 20)));
        }
    }
}

/*
** Initialize common node fields from config.
*/
static int node_init_common(HppliteNode *node, const HppliteNodeConfig *config) {
    int rc;

    /* Copy configuration */
    node->config.nodeId = node_strdup(config->nodeId);
    memcpy(node->config.privkey, config->privkey, 32);
    node->config.hasPrivkey = config->hasPrivkey;
    memcpy(node->config.viewkey, config->viewkey, 32);
    node->config.hasViewKey = config->hasViewKey;
    if (config->hasViewKey) { memcpy(node->viewKey, config->viewkey, 32); node->hasViewKey = 1; }
    node->config.dataDir = node_strdup(config->dataDir);
    node->config.dbPath = node_strdup(config->dbPath);
    node->config.dbName = node_strdup(config->dbName);
    node->config.rpcUrl = node_strdup(config->rpcUrl);
    memcpy(node->config.factory, config->factory, 20);
    node->config.hasFactory = config->hasFactory;
    memcpy(node->config.contract, config->contract, 20);
    node->config.hasContract = config->hasContract;
    node->config.checkpointInterval = config->checkpointInterval > 0 ? config->checkpointInterval : 100;
    node->checkpointGiven = config->checkpointInterval > 0;
    node->config.batchIntervalMs = config->batchIntervalMs;
    node->config.maxBatchBytes = config->maxBatchBytes ? config->maxBatchBytes : 128 * 1024;
    node->config.chainId = config->chainId;
    node->config.maxTxBytes = config->maxTxBytes;
    node->config.maxTxGas = config->maxTxGas;
    node->config.storeUrl = node_strdup(config->storeUrl);
    node->config.storeCap = config->storeCap;
    node->config.storeFallback = config->storeFallback;
    node->config.anchorIntervalMs = config->anchorIntervalMs;
    if (node->config.storeUrl) {
        /* the batch bytes' store (#11): the chain will hold commitments only */
        node->store = hpplite_store_open(node->config.storeUrl, config->hasPrivkey ? config->privkey : NULL, config->storeCap);
        if (!node->store) {
            fprintf(stderr, "hpplite: store=%s: not a store HPPLite knows (http://host:port of a pin service, kubo://host:port of a Kubo node, s3://bucket/prefix)\n", node->config.storeUrl);
            return -1;
        }
    }
    node_cap_batch_limit(node);
    node->config.noEncrypt = config->noEncrypt;
    node->config.noCompress = config->noCompress;
    node->config.determinismWarn = config->determinismWarn;
    node->config.captureText = config->captureText;
    node->config.capturePages = config->capturePages;
    node->config.captureAuto = config->captureAuto && !config->capturePages && !config->captureText;
    node->config.localOnly = config->localOnly;
    node->config.verifyOnly = config->verifyOnly;
    /* the environment chooses for any node whose configuration did not say
    ** (every suite runs in both modes); a URI's capture= stands (#80) */
    if (getenv("HPPLITE_CAPTURE") && config->captureSource < 2) {
        const char *z = getenv("HPPLITE_CAPTURE");
        if (strcasecmp(z, "text") == 0) { node->config.captureText = 1; node->config.capturePages = 0; node->config.captureAuto = 0; }
        else if (strcasecmp(z, "rows") == 0) { node->config.captureText = 0; node->config.capturePages = 0; node->config.captureAuto = 0; }
        else if (strcasecmp(z, "pages") == 0) { node->config.captureText = 0; node->config.capturePages = 1; node->config.captureAuto = 0; }
        else if (strcasecmp(z, "auto") == 0) { node->config.captureText = 0; node->config.capturePages = 0; node->config.captureAuto = 1; }
        else fprintf(stderr, "hpplite: HPPLITE_CAPTURE=%s is none of auto, rows, text, pages; ignored\n", z);
    }

    /* Generate unique instance ID (a file's sequencer replaces it with the
    ** file's own, node_take_file_lock) */
    generate_instance_id(node->instanceId);
    node->lockFd = -1;
    node->usersFd = -1;
    node->leaseDuration = 300;

    /* One flush at a time, timer or not (two hpplite_flush() calls from two
    ** threads used to run concurrently on a timer-less node) */
    {
        pthread_mutex_t *mutex = sqlite3_malloc(sizeof(pthread_mutex_t));
        if (!mutex) return -1;
        pthread_mutex_init(mutex, NULL);
        node->flushMutex = mutex;
    }
    {
        pthread_mutex_t *m = sqlite3_malloc(sizeof(pthread_mutex_t));
        if (!m) return -1;
        pthread_mutex_init(m, NULL);
        node->walMaintMutex = m;
    }

    /* Initialize state */
    node->state = HPPLITE_STATE_INIT;

    /* Initialize crypto */
    node->crypto = hpplite_crypto_init();
    if (!node->crypto) {
        return -1;
    }

    /* Initialize keypair from private key */
    if (config->hasPrivkey) {
        rc = hpplite_crypto_keypair_from_privkey(node->crypto, &node->keypair, config->privkey);
        if (rc != 0) {
            return -1;
        }
        /* Batch key: whoever can write the rollup can read it */
        hpplite_cipher_derive_key(config->privkey, node->encKey);
        node->hasEncKey = 1;
        if (!config->noEncrypt) node->cipher = hpplite_cipher_default();
    }

    return 0;
}

/*
** Set up HPPLite tracking on node->db plus the bookkeeping connection.
**
** File databases are switched to WAL so the bookkeeping connection's reads
** never block the application's writes (and vice versa), and both connections
** get a busy timeout so the brief bookkeeping writes wait instead of failing.
*/
/* Tell the capture path how long a statement may be: what fits in one DA
** batch after the envelope and the JSON framing. */
/* What the envelope adds: the symmetric one, or the public-key one when readers are granted (#15) */
static size_t node_envelope_overhead(HppliteNode *node) {
    if (!node->cipher) return 0;
    if (node->nReaders > 0 && node->config.hasPrivkey) return hpplite_envelope_pk_overhead(node->cipher, node->nReaders + 1);
    return hpplite_envelope_overhead(node->cipher);
}

static void node_set_stmt_limit(HppliteNode *node) {
    size_t limit = node->config.maxBatchBytes;
    if (!node->ctx) return;
    if (limit > 0) {
        size_t overhead = hpplite_batch_json_overhead();
        if (node->cipher) overhead += node_envelope_overhead(node);
        /* capture compares the statement's JSON-escaped size (what is posted)
        ** against this, the same arithmetic the batch builder uses */
        limit = limit > overhead ? (limit - overhead) : 1;
    }
    node->ctx->maxStmtBytes = limit;
}

/*
** Across processes, a file's sequencer is whoever holds an exclusive flock on
** <file>-hpplite: a file of its own, since SQLite's POSIX locks on the
** database are dropped when any descriptor of it is closed. The OS lets go
** when the holder exits or crashes. The lock file keeps a random seed, and
** the instance id is keccak(seed, host name): every process on this host is
** the same sequencer to L1 - a handover needs no lease transaction - while a
** copy of the file on another host is a different one. Returns 1 if this
** node may sequence (lock held, or no lock possible: then as before), 0 if
** another process holds it.
*/
static void node_file_instance_id(HppliteNode *node, const unsigned char seed[32]) {
    unsigned char buf[32 + 256];
    memcpy(buf, seed, 32);
    if (gethostname((char *)buf + 32, 255) != 0) buf[32] = 0;
    buf[32 + 255] = 0;
    keccak256(buf, 32 + strlen((char *)buf + 32), node->instanceId);
}
static int node_take_file_lock(HppliteNode *node) {
    char path[PATH_MAX + 16];
    unsigned char seed[32];
    int fd;
    if (node->lockFd >= 0 || !node->canonicalPath) return 1;
    snprintf(path, sizeof(path), "%s-hpplite", node->canonicalPath);
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return 1;                     /* no lock possible here (read-only directory) */
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        /* a follower: the file's id already, for when it takes over (the
        ** holder wrote the seed when it took the lock) */
        if (pread(fd, seed, 32, 0) == 32) node_file_instance_id(node, seed);
        close(fd);
        return 0;
    }
    if (pread(fd, seed, 32, 0) != 32) {
        generate_instance_id(seed);
        if (pwrite(fd, seed, 32, 0) != 32) fprintf(stderr, "hpplite: could not write %s\n", path);
        fsync(fd);
    }
    node_file_instance_id(node, seed);
    node->lockFd = fd;
    return 1;
}

/*
** Is another connection already sequencing this file - in this process (the
** registry) or in another (the lock file)? Then this one only captures: the
** log lives in the file and is shared, the sequencer's flush posts everyone's
** rows. No L1 or lease here; a timer, if any, only waits to take over.
*/
static void node_detect_follower(HppliteNode *node) {
    const char *zFile = sqlite3_db_filename(node->db, "main");
    char resolved[PATH_MAX], users[PATH_MAX + 24];
    if (!zFile || !zFile[0]) return;
    if (!realpath(zFile, resolved)) return;
    node->canonicalPath = node_strdup(resolved);
    /* Every connection holds <file>-hpplite-users shared while open: a
    ** sequencer that closes can tell whether anyone is left to take over */
    snprintf(users, sizeof(users), "%s-hpplite-users", resolved);
    node->usersFd = open(users, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (node->usersFd >= 0 && flock(node->usersFd, LOCK_SH | LOCK_NB) != 0) {
        /* a closing sequencer is looking: wait for it rather than be missed */
        if (flock(node->usersFd, LOCK_SH) != 0) { close(node->usersFd); node->usersFd = -1; }
    }
    if (hpplite_find_sequencer(resolved) || !node_take_file_lock(node)) node->follower = 1;
}

/* Page mode (#146): the application's connection gets no hook; the bookkeeping
** lives in a sidecar file beside the database (<db>-hpplite.db); the VFS
** hands the connection's commits to the page mode, started on the file as it
** is after a checkpoint. */
static int node_init_pages(HppliteNode *node, const char *zFile) {
    char *zSide = sqlite3_mprintf("%s-hpplite.db", zFile);
    char resolved[PATH_MAX];
    int rc;
    if (!zSide) return SQLITE_NOMEM;
    hpplite_internal_open_begin();
    rc = sqlite3_open_v2(zSide, &node->bkDb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    hpplite_internal_open_end();
    sqlite3_free(zSide);
    if (rc != SQLITE_OK) { if (node->bkDb) sqlite3_close(node->bkDb); node->bkDb = NULL; return rc; }
    sqlite3_busy_timeout(node->bkDb, 5000);
    sqlite3_exec(node->bkDb, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(node->bkDb, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    rc = hpplite_init_pages(node->db, node->bkDb, &node->ctx);
    if (rc != SQLITE_OK) return rc;
    node->ctx->bTrackRoots = node->config.verifyOnly;
    if (node->config.dbName && node->config.dbName[0]) {
        /* {file}: the file's own name, as the row path reads it (#61) */
        if (strcmp(node->config.dbName, "{file}") == 0) {
            const char *slash = strrchr(zFile, '/');
            node->ctx->zDbName = sqlite3_mprintf("%s", slash ? slash + 1 : zFile);
        } else {
            node->ctx->zDbName = sqlite3_mprintf("%s", node->config.dbName);
        }
    }
    (void)resolved;
    if (node->config.verifyOnly) {
        /* a replica: pages are written into its file under an exclusive lock and
        ** its readers see them by the change counter, which rollback-journal
        ** mode checks at every transaction and WAL mode does not */
        hppliteInternalExec(node->ctx, node->db, "PRAGMA journal_mode=DELETE");
    } else {
        /* the frames are what is captured: the file must be in WAL mode, and a
        ** switch another connection's transaction prevents (busy past the
        ** timeout) must not leave a connection that captures nothing */
        sqlite3_stmt *q = NULL;
        int wal = 0;
        hppliteInternalExec(node->ctx, node->db, "PRAGMA journal_mode=WAL");
        if (sqlite3_prepare_v2(node->db, "PRAGMA journal_mode", -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW)
            wal = sqlite3_column_text(q, 0) && strcasecmp((const char *)sqlite3_column_text(q, 0), "wal") == 0;
        sqlite3_finalize(q);
        if (!wal) {
            hpplite_set_init_error("page capture needs the database in WAL mode and it could not be switched (another connection holds it?)");
            return SQLITE_BUSY;
        }
    }
    return SQLITE_OK;
}

/* Once, with the chain seen: a new rollup's first batch is the file as it is;
** a sequencer back on a file whose root is not the last it posted (commits
** captured and lost with the process, a file changed while it was away)
** ships the file whole again: the chain can always rebuild it. Decided here
** so the file counts as pending before any flush asks. */
static void node_pages_decide_snapshot(HppliteNode *node) {
    unsigned char have[32];
    if (!node->pm || node->pagesSnapshotDecided || node->config.verifyOnly || node->follower) return;
    hpplite_pagemode_root(node->pm, have);
    if (node->ctx->blockHeight <= 1 || memcmp(have, node->ctx->stateRoot, 32) != 0) hpplite_pagemode_snapshot(node->pm);
    node->pagesSnapshotDecided = 1;
}

/* The page mode on this node's file: a sequencer's (its commits watched), or
** a verifier's (applying what it is sent). A follower has none: its commits
** go through the same VFS to the same file, and the sequencer's watch sees
** them; a follower that takes over starts its own then. */
static int node_start_pages(HppliteNode *node, const char *zFile) {
    char resolved[PATH_MAX];
    if (node->pm) return SQLITE_OK;
    if (!realpath(zFile, resolved)) snprintf(resolved, sizeof resolved, "%s", zFile);
    if (node->config.verifyOnly) {
        node->pm = hpplite_pagemode_start(resolved, 0, 0);
    } else {
        /* the file holds everything once checkpointed: the page mode reads it
        ** once; what a checkpoint could not fold in (another connection's
        ** read) stays in the WAL, and the reader delivers it from the frame
        ** after the last one the file holds */
        int nLog = 0, nCkpt = 0;
        if (sqlite3_wal_checkpoint_v2(node->db, "main", SQLITE_CHECKPOINT_TRUNCATE, &nLog, &nCkpt) != SQLITE_OK) sqlite3_wal_checkpoint_v2(node->db, "main", SQLITE_CHECKPOINT_PASSIVE, &nLog, &nCkpt);
        node->pm = hpplite_pagemode_start(resolved, 1, nCkpt > 0 ? (unsigned)nCkpt : 0);
        if (node->pm && node->ctx) { node->ctx->xWalMaint = node_wal_maintenance; node->ctx->pWalMaintArg = node; }
        if (node->pm && node->ctx) hpplite_walread_poll(resolved, 0);        /* what the WAL holds beyond the file, now */
        if (node->pm && node->ctx) node_wal_maintenance(node);                /* the first pin: from here no restart drops an unread frame */
    }
    if (!node->pm) { fprintf(stderr, "hpplite: page mode could not start on %s\n", zFile); return SQLITE_ERROR; }
    if (node->config.localOnly) node_pages_decide_snapshot(node);      /* no chain to sync from: decided now */
    return SQLITE_OK;
}

/* Row or text capture on the application's connection: the hooks, the
** bookkeeping tables in the file, a bookkeeping connection of its own */
static int node_init_rows(HppliteNode *node, const char *zFile) {
    int rc;
    if (zFile && zFile[0]) {
        sqlite3_exec(node->db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
        sqlite3_busy_timeout(node->db, 5000);
    }

    /* a SQLite that cuts bound values in expanded SQL serves row capture (#62) */
    rc = hpplite_init_ex(node->db, &node->ctx, !node->config.captureText);
    if (rc != SQLITE_OK) return rc;
    node->ctx->bDeterminismWarn = node->config.determinismWarn;
    node->ctx->bTrackRoots = node->config.verifyOnly;          /* hpplite-verify checks the chain's checkpoint at any recent height (#120) */
    if (node->config.dbName && node->config.dbName[0]) {
        /* {file}: the file's own name, so one setting names every database
        ** of an application (HPPLITE_FILES, #61) */
        if (strcmp(node->config.dbName, "{file}") == 0) {
            const char *z = zFile && zFile[0] ? zFile : "memory";
            const char *slash = strrchr(z, '/');
            node->ctx->zDbName = sqlite3_mprintf("%s", slash ? slash + 1 : z);
        } else {
            node->ctx->zDbName = sqlite3_mprintf("%s", node->config.dbName);
        }
    }
    if (!node->config.captureText) {
        rc = hpplite_rows_enable(node->ctx);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "hpplite: cannot set up row capture (%s); capture=text captures statements\n", sqlite3_errmsg(node->db));
            return rc;
        }
    }

    if (zFile && zFile[0]) {
        hpplite_internal_open_begin();
        rc = sqlite3_open_v2(zFile, &node->bkDb, SQLITE_OPEN_READWRITE, NULL);
        hpplite_internal_open_end();
        if (rc != SQLITE_OK) {
            if (node->bkDb) sqlite3_close(node->bkDb);
            node->bkDb = NULL;
        } else {
            sqlite3_busy_timeout(node->bkDb, 5000);
            /* No fsync per bookkeeping commit (#142): in WAL mode the file stays
            ** consistent, and a power loss can lose only the last bookkeeping
            ** commits, which the next flush adopts from the chain (#90, #119).
            ** The application's own setting is its own; its next fsync carries
            ** the bookkeeping frames before it. */
            sqlite3_exec(node->bkDb, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
            hpplite_set_bookkeeping_db(node->ctx, node->bkDb);
        }
    }
    return SQLITE_OK;
}

/* capture=auto (the default, #146): a file that carries HPPLite's tables
** keeps row or text capture; any other starts in page mode, and switches
** to rows at the sync if the rollup's history turns out to be statements */
static int node_file_has_log(sqlite3 *db) {
    return sqlite3_table_column_metadata(db, "main", "_hpplite_meta", "next_height", NULL, NULL, NULL, NULL, NULL) == SQLITE_OK;
}

static int node_init_tracking(HppliteNode *node) {
    const char *zFile = sqlite3_db_filename(node->db, "main");
    int rc;

    if (node->config.captureAuto && zFile && zFile[0]) node->config.capturePages = !node_file_has_log(node->db);
    if (zFile && zFile[0] && node->config.capturePages) {
        sqlite3_busy_timeout(node->db, 5000);
        rc = node_init_pages(node, zFile);
        if (rc != SQLITE_OK) return rc;
    } else {
        rc = node_init_rows(node, zFile);
        if (rc != SQLITE_OK) return rc;
    }

    node->lastCheckpointHeight = node->ctx->lastCheckpointHeight;
    memcpy(node->lastCheckpointRoot, node->ctx->lastCheckpointRoot, HPPLITE_HASH_SIZE);
    /* Batches confirmed since the last checkpoint are in _hpplite_meta
    ** (heights), so the cadence survives a restart: a process that opens,
    ** writes a few batches and exits still reaches a checkpoint eventually */
    if (node->ctx->blockHeight > node->lastCheckpointHeight + 1)
        node->batchesSinceCheckpoint = (int)(node->ctx->blockHeight - 1 - node->lastCheckpointHeight);
    node->lastFlushTime = node_current_time_ms();
    node_set_stmt_limit(node);
    node_detect_follower(node);
    if (node->config.capturePages && zFile && zFile[0] && !node->follower) {
        rc = node_start_pages(node, zFile);
        if (rc != SQLITE_OK) return rc;
    }
    return SQLITE_OK;
}

/* capture=auto: the page mode let go, the sidecar removed, row capture set
** up on the same connection (the rollup's history is statements) */
static int node_switch_to_rows(HppliteNode *node) {
    const char *zFile = sqlite3_db_filename(node->db, "main");
    char *zSide;
    int rc;
    if (!zFile || !zFile[0]) return SQLITE_MISUSE;
    if (node->pm) { hpplite_pagemode_stop(node->pm); node->pm = NULL; }
    if (node->pieceDb) { sqlite3_exec(node->pieceDb, "ROLLBACK", NULL, NULL, NULL); sqlite3_close(node->pieceDb); node->pieceDb = NULL; }
    node_ck_close(node);
    node_pins_close(node);
    if (node->ctx) { hpplite_shutdown(node->ctx); node->ctx = NULL; }
    if (node->bkDb) { sqlite3_close(node->bkDb); node->bkDb = NULL; }
    zSide = sqlite3_mprintf("%s-hpplite.db", zFile);
    if (zSide) { unlink(zSide); sqlite3_free(zSide); }
    node->config.capturePages = 0;
    node->pagesSnapshotDecided = 0; node->pagesWalSuspended = 0;
    rc = node_init_rows(node, zFile);
    if (rc != SQLITE_OK) return rc;
    node->lastCheckpointHeight = node->ctx->lastCheckpointHeight;
    memcpy(node->lastCheckpointRoot, node->ctx->lastCheckpointRoot, HPPLITE_HASH_SIZE);
    node->batchesSinceCheckpoint = 0;
    node_set_stmt_limit(node);
    node_check_rollup(node);                        /* the fresh context bound to the rollup */
    fprintf(stderr, "hpplite: the rollup's history is statements: row capture on %s (capture=auto)\n", zFile);
    return SQLITE_OK;
}

static void node_learn_batch_limit(HppliteNode *node);

/*
** Auto-connect to L1 and sync state.
*/
static int node_ensure_lease(HppliteNode *node);
static void node_load_readers(HppliteNode *node);

static HppliteBatch *node_decode_batch(HppliteNode *node, uint64_t height,
                                       const uint8_t *data, size_t dataLen);

/*
** A named database's rollup among its wallet's (#61): the one it is bound to
** already; else the one whose first batch carries its name (a replica, or a
** writer that lost its file); else a new one. 1 with the address, 0 not.
*/
static int node_named_rollup(HppliteNode *node, const HppliteNodeConfig *config,
                             const char *factoryHex, uint8_t rollup[20]) {
    const char *zName = node->ctx->zDbName;
    const char *colon = strchr(node->ctx->zRollup, ':');
    uint8_t (*all)[20] = NULL;
    int n, i;
    if (node->ctx->zRollup[0] && colon && strlen(colon + 1) == 42) {
        for (i = 0; i < 20; i++) {
            unsigned v;
            if (sscanf(colon + 3 + 2 * i, "%2x", &v) != 1) return 0;
            rollup[i] = (uint8_t)v;
        }
        return 1;
    }
    all = sqlite3_malloc(256 * 20);
    if (!all) return 0;
    n = hpplite_l1_factory_list_my_rollups(config->rpcUrl, factoryHex, config->privkey, all, 256);
    char *zOthers = NULL;                 /* the wallet's databases, for the message below (#79) */
    for (i = 0; i < n; i++) {
        char hex[43];
        HppliteL1 *l1;
        snprintf(hex, sizeof(hex), "0x");
        for (int k = 0; k < 20; k++) snprintf(hex + 2 + 2 * k, 3, "%02x", all[i][k]);
        l1 = hpplite_l1_connect(config->rpcUrl, hex);
        if (l1) {
            size_t len = 0;
            uint8_t *data = node_fetch_bytes(node, l1, 1, &len);
            HppliteBatch *b = data ? node_decode_batch(node, 1, data, len) : NULL;
            int match = b && b->zDbName && strcmp(b->zDbName, zName) == 0;
            if (b && b->zDbName && !match) zOthers = sqlite3_mprintf("%z%s\"%s\"", zOthers, zOthers ? ", " : "", b->zDbName);
            hpplite_batch_free(b);
            free(data);
            hpplite_l1_disconnect(l1);
            if (match) {
                memcpy(rollup, all[i], 20);
                sqlite3_free(all);
                sqlite3_free(zOthers);
                return 1;
            }
        }
    }
    sqlite3_free(all);
    if (n < 0) { sqlite3_free(zOthers); return 0; }   /* could not list: do not create a second one */
    /* The factory makes a wallet's first rollup its primary - the one an
    ** unnamed database uses. A named database's must never be it: the
    ** primary comes first (getOrCreateRollup is idempotent, also when two
    ** processes ask at once) */
    if (hpplite_l1_factory_get_my_rollup(config->rpcUrl, factoryHex, config->privkey, rollup) == 0) {
        char *txHash = hpplite_l1_factory_get_or_create_rollup(config->rpcUrl, factoryHex, config->privkey, rollup);
        if (txHash) {
            HppliteL1 *tempL1 = hpplite_l1_connect(config->rpcUrl, factoryHex);
            if (tempL1) {
                hpplite_l1_wait_for_tx(tempL1, txHash, 60);
                hpplite_l1_disconnect(tempL1);
            }
            sqlite3_free(txHash);
        }
        if (hpplite_l1_factory_get_my_rollup(config->rpcUrl, factoryHex, config->privkey, rollup) != 1) return 0;
    }
    if (node->config.verifyOnly) {
        fprintf(stderr, "hpplite: role=verify: no rollup of this wallet is database \"%s\" (its databases: %s); nothing to verify\n",
                zName, zOthers ? zOthers : "none named");
        sqlite3_free(zOthers);
        return 0;
    }
    /* a misspelt name would be a new database: say which exist (#79) */
    fprintf(stderr, "hpplite: no rollup of this wallet is database \"%s\" yet (its databases: %s); creating one\n",
            zName, zOthers ? zOthers : "none named");
    sqlite3_free(zOthers);
    return hpplite_l1_factory_create_rollup(config->rpcUrl, factoryHex, config->privkey,
                                            config->checkpointInterval > 0 ? (uint64_t)config->checkpointInterval : 100,
                                            rollup);
}

static void node_auto_connect_l1(HppliteNode *node, const HppliteNodeConfig *config) {
    if (node->follower) return;
    if (!config->rpcUrl) return;
    if (config->chainId) hpplite_l1_set_default_chain_id(config->chainId);
    if (!config->hasContract && !config->hasFactory) return;
    node->wantsL1 = 1;

    if (config->hasFactory && config->hasPrivkey) {
        /* Use factory to find/create rollup */
        char factoryHex[43];
        snprintf(factoryHex, sizeof(factoryHex), "0x");
        for (int i = 0; i < 20; i++) {
            snprintf(factoryHex + 2 + i*2, 3, "%02x", config->factory[i]);
        }

        /* Check if rollup exists */
        unsigned char rollup[20];
        int named = node->ctx && node->ctx->zDbName;
        int result = named ? node_named_rollup(node, config, factoryHex, rollup)
                           : hpplite_l1_factory_get_my_rollup(config->rpcUrl, factoryHex, config->privkey, rollup);
        if (result == 1 && !named && node->ctx->blockHeight <= 1) {
            /* an empty file, unnamed, on a wallet with named databases: it
            ** gets the primary rollup - a forgotten dbname= would rebuild
            ** nothing, silently (#79) */
            uint8_t (*all)[20] = sqlite3_malloc(256 * 20);
            int n = all ? hpplite_l1_factory_list_my_rollups(config->rpcUrl, factoryHex, config->privkey, all, 256) : 0;
            sqlite3_free(all);
            if (n > 1) fprintf(stderr, "hpplite: this wallet has %d rollups (named databases, dbname=); this open is unnamed"
                               " and uses the wallet's primary rollup\n", n);
        }

        if (result == 0 && config->verifyOnly) {
            fprintf(stderr, "hpplite: role=verify: this wallet has no rollup on this factory; nothing to verify\n");
            return;
        }
        if (result == 0 && !named) {
            /* No rollup exists - create one */
            char *txHash = hpplite_l1_factory_get_or_create_rollup(
                config->rpcUrl, factoryHex, config->privkey, rollup);

            if (txHash) {
                /* Wait for transaction to be mined */
                HppliteL1 *tempL1 = hpplite_l1_connect(config->rpcUrl, factoryHex);
                if (tempL1) {
                    hpplite_l1_wait_for_tx(tempL1, txHash, 60);
                    hpplite_l1_disconnect(tempL1);
                }
                sqlite3_free(txHash);

                /* Try again to get rollup address */
                result = hpplite_l1_factory_get_my_rollup(
                    config->rpcUrl, factoryHex, config->privkey, rollup);
            }
        }

        if (result == 1 && named) {
            /* its own rollup, not the wallet's first */
            char rollupHex[43];
            snprintf(rollupHex, sizeof(rollupHex), "0x");
            for (int i = 0; i < 20; i++) snprintf(rollupHex + 2 + i * 2, 3, "%02x", rollup[i]);
            node->l1 = hpplite_l1_connect(config->rpcUrl, rollupHex);
            if (node->l1) hpplite_l1_set_privkey(node->l1, config->privkey);
            node->ownsL1 = (node->l1 != NULL);
        } else if (result == 1) {
            /* Connect to the rollup */
            node->l1 = hpplite_l1_connect_factory(
                config->rpcUrl, factoryHex, config->privkey);
            node->ownsL1 = (node->l1 != NULL);
        }
    } else if (config->hasFactory && node->hasSigner) {
        /* a signer's wallet: its rollup on the factory, found by address; a signer creates none (#17) */
        char factoryHex[43], rollupHex[43];
        unsigned char rollup[20];
        int i;
        snprintf(factoryHex, sizeof(factoryHex), "0x");
        for (i = 0; i < 20; i++) snprintf(factoryHex + 2 + i * 2, 3, "%02x", config->factory[i]);
        if (hpplite_l1_factory_get_rollup(config->rpcUrl, factoryHex, node->signer.address, rollup) == 1) {
            snprintf(rollupHex, sizeof(rollupHex), "0x");
            for (i = 0; i < 20; i++) snprintf(rollupHex + 2 + i * 2, 3, "%02x", rollup[i]);
            node->l1 = hpplite_l1_connect(config->rpcUrl, rollupHex);
            node->ownsL1 = (node->l1 != NULL);
            if (node->l1) hpplite_l1_set_signer(node->l1, node->signer.sign, node->signer.arg, node->signer.address);
        } else {
            fprintf(stderr, "hpplite: this signer's wallet has no rollup on the factory, and a signer creates none:"
                            " create it once with the key, or name contract=\n");
        }
    } else if (config->hasContract) {
        /* Direct contract connection */
        char contractHex[43];
        snprintf(contractHex, sizeof(contractHex), "0x");
        for (int i = 0; i < 20; i++) {
            snprintf(contractHex + 2 + i*2, 3, "%02x", config->contract[i]);
        }
        node->l1 = hpplite_l1_connect(config->rpcUrl, contractHex);
        node->ownsL1 = (node->l1 != NULL);

        if (node->l1 && config->hasPrivkey) {
            hpplite_l1_set_privkey(node->l1, config->privkey);
        } else if (node->l1 && node->hasSigner) {
            hpplite_l1_set_signer(node->l1, node->signer.sign, node->signer.arg, node->signer.address);
        }
    }

    /* Catch up with whatever is already on L1, then take the lease now
    ** rather than at the first write: a restart has no window in which the
    ** rollup is unclaimed, and lease trouble shows at open */
    if (node->l1) {
        node_learn_batch_limit(node);
        if (hpplite_node_sync_from_l1(node, 1) < 0) {
            node->state = HPPLITE_STATE_ERROR;
        } else if (!node->follower && !node->config.verifyOnly && (node->cipher || node->config.noEncrypt)) {
            node_ensure_lease(node);          /* an audit never takes it (#5) */
        }
    }
}

/*
** Is any other connection on this file left (one that would take over the
** sequencer)? Our shared lock on the users file goes; if an exclusive one is
** then granted, nobody else holds it.
*/
static int node_others_on_file(HppliteNode *node) {
    int others;
    if (node->usersFd < 0) return 0;
    flock(node->usersFd, LOCK_UN);
    others = flock(node->usersFd, LOCK_EX | LOCK_NB) != 0;
    if (!others) flock(node->usersFd, LOCK_UN);
    return others;
}

/*
** A follower whose sequencer is gone (its process exited or crashed, or it
** closed) takes over: the lock, then what the sequencer moved on in the file
** since this connection opened (height, checkpoint, carry: our copy is from
** then), then L1 and the lease - the same instance id, so the lease is ours
** if it is still running. Called under the flush mutex. Returns 1 if this
** node now sequences.
*/
static int node_try_promote(HppliteNode *node) {
    if (!node->follower || !node->canonicalPath || !node->ctx) return 0;
    if (hpplite_find_sequencer(node->canonicalPath)) return 0;
    if (node->lockFd < 0 && !node_take_file_lock(node)) return 0;
    if (node->lockFd < 0) return 0;
    node->follower = 0;
    if (node->config.capturePages && !node->pm) {
        hppliteLoadMeta(node->ctx);                   /* the sidecar as the sequencer left it */
        node_start_pages(node, sqlite3_db_filename(node->db, "main"));
    }
    hpplite_set_bookkeeping_db(node->ctx, node->bkDb);
    node->lastCheckpointHeight = node->ctx->lastCheckpointHeight;
    memcpy(node->lastCheckpointRoot, node->ctx->lastCheckpointRoot, HPPLITE_HASH_SIZE);
    node->batchesSinceCheckpoint = node->ctx->blockHeight > node->lastCheckpointHeight + 1
        ? (int)(node->ctx->blockHeight - 1 - node->lastCheckpointHeight) : 0;
    if (!node->l1) {
        node_auto_connect_l1(node, &node->config);
    } else if (hpplite_node_sync_from_l1(node, 1) < 0) {
        node->state = HPPLITE_STATE_ERROR;
    } else {
        node->leaseRecheckAt = 0;
        node_ensure_lease(node);
    }
    return 1;
}

/*
** Initialize a node with configuration.
*/
HppliteNode *hpplite_node_create(const HppliteNodeConfig *config) {
    HppliteNode *node;
    int rc;

    if (!config) return NULL;

    node = sqlite3_malloc(sizeof(HppliteNode));
    if (!node) return NULL;
    memset(node, 0, sizeof(HppliteNode));

    /* Initialize common fields */
    if (node_init_common(node, config) != 0) {
        hpplite_node_destroy(node);
        return NULL;
    }

    /* Open database */
    hpplite_internal_open_begin();          /* this node is the HPPLite instance, not an auto-enabled one */
    rc = sqlite3_open(config->dbPath, &node->db);
    hpplite_internal_open_end();
    if (rc != SQLITE_OK) {
        hpplite_node_destroy(node);
        return NULL;
    }
    node->ownsDb = 1;

    if (node_init_tracking(node) != SQLITE_OK) {
        hpplite_node_destroy(node);
        return NULL;
    }

    /* Auto-connect to L1 */
    node_auto_connect_l1(node, config);

    /* Register node with global registry */
    hpplite_register_node(node->db, node);

    return node;
}

/*
** Initialize a node using an existing database connection.
*/
HppliteNode *hpplite_node_create_with_db(sqlite3 *db, const HppliteNodeConfig *config) {
    HppliteNode *node;

    if (!db || !config) return NULL;

    node = sqlite3_malloc(sizeof(HppliteNode));
    if (!node) return NULL;
    memset(node, 0, sizeof(HppliteNode));

    /* Use existing db, don't own it */
    node->db = db;
    node->ownsDb = 0;

    /* Initialize common fields */
    if (node_init_common(node, config) != 0) {
        hpplite_node_destroy(node);
        return NULL;
    }

    if (node_init_tracking(node) != SQLITE_OK) {
        hpplite_node_destroy(node);
        return NULL;
    }

    /* Auto-connect to L1 */
    node_auto_connect_l1(node, config);

    /* Register node with global registry */
    hpplite_register_node(node->db, node);

    return node;
}

/*
** Free a node and all resources.
*/
void hpplite_node_destroy(HppliteNode *node) {
    if (!node) return;

    /* Stop timer thread first */
    hpplite_node_stop_timer(node);

    /* A follower whose sequencer is gone takes over to post what is left:
    ** the last connection out posts */
    /* (a page-mode follower keeps no queue of its own: it takes over whenever
    ** the sequencer is gone, and its page mode then finds what the file holds
    ** beyond the chain) */
    if (node->follower && node->ctx && !node->ctx->bDbClosed
        && (hpplite_node_pending_count(node) > 0 || (node->config.capturePages && !node->pm)) && node->flushMutex) {
        pthread_mutex_lock((pthread_mutex_t *)node->flushMutex);
        node_try_promote(node);
        pthread_mutex_unlock((pthread_mutex_t *)node->flushMutex);
    }

    /* Flush any pending data before cleanup (not when bookkeeping shared a
    ** connection that is already closed: nothing can run there any more) */
    if (node->ctx && !(node->ctx->bDbClosed && node->ctx->bkDb == node->ctx->db)
        && hpplite_node_pending_count(node) > 0) {
        hpplite_node_flush_batch(node);
    }

    /* Stop if running */
    if (node->state == HPPLITE_STATE_RUNNING) {
        hpplite_node_stop(node);
    }

    /* the batches the store holds since the last anchor get theirs (#164) */
    if (node->l1 && node->ctx && !node->ctx->bDbClosed && node->flushMutex && node_ranges(node)) {
        pthread_mutex_lock((pthread_mutex_t *)node->flushMutex);
        node_anchor(node);
        pthread_mutex_unlock((pthread_mutex_t *)node->flushMutex);
    }
    if (node->manifest) { cJSON_Delete(node->manifest); node->manifest = NULL; }
    free(node->zAnchorRef);
    node->zAnchorRef = NULL;

    /* Let go of the sequencer lease so a restart need not wait it out (a
    ** database rebuilt into a fresh file, another host) - unless another
    ** connection to this file is still open: it takes over as the same
    ** sequencer (the id is the file's), and a release would only cost it a claim */
    if (node->l1 && node->hasLease && !(node->lockFd >= 0 && node_others_on_file(node))) {
        hpplite_node_release_lease(node);
    }

    /* Disconnect L1 only if we own it */
    if (node->l1 && node->ownsL1) {
        hpplite_l1_disconnect(node->l1);
    }
    if (node->store) {
        hpplite_store_close(node->store);
        node->store = NULL;
    }

    /* Shutdown HPPLite context */
    if (node->ctx) {
        if (node->pm) { hpplite_pagemode_stop(node->pm); node->pm = NULL; }
        node->ctx->xWalMaint = NULL; node->ctx->pWalMaintArg = NULL;     /* before the context goes */
        hpplite_shutdown(node->ctx);
        node->ctx = NULL;
    }

    if (node->pieceDb) { sqlite3_exec(node->pieceDb, "ROLLBACK", NULL, NULL, NULL); sqlite3_close(node->pieceDb); node->pieceDb = NULL; }
    node_ck_close(node);
    node_pins_close(node);
    if (node->bkDb) {
        sqlite3_close(node->bkDb);
    }

    /* Close database only if we own it. Unregister first so the close
    ** (which disconnects hpplite_status) does not tear this node down again. */
    if (node->db && node->ownsDb) {
        hpplite_unregister_node(node->db);
        sqlite3_close(node->db);
    }

    /* The locks last: everything this sequencer had to do in the file is done */
    if (node->lockFd >= 0) {
        close(node->lockFd);
        node->lockFd = -1;
    }
    if (node->usersFd >= 0) {
        close(node->usersFd);
        node->usersFd = -1;
    }

    if (node->timerWake) {
        pthread_cond_destroy((pthread_cond_t *)node->timerWake);
        pthread_mutex_destroy((pthread_mutex_t *)node->timerWakeMutex);
        sqlite3_free(node->timerWake);
        sqlite3_free(node->timerWakeMutex);
        node->timerWake = node->timerWakeMutex = NULL;
    }

    /* Free crypto */
    if (node->crypto) {
        hpplite_crypto_free(node->crypto);
    }

    /* Free config strings */
    sqlite3_free(node->config.nodeId);
    sqlite3_free(node->config.dataDir);
    sqlite3_free(node->config.dbPath);
    sqlite3_free(node->config.dbName);
    sqlite3_free(node->config.storeUrl);
    free(node->aReaders);
    memset(node->viewKey, 0, sizeof node->viewKey);
    memset(node->seriesDek, 0, sizeof node->seriesDek);
    sqlite3_free(node->config.rpcUrl);
    sqlite3_free(node->canonicalPath);
    if (node->flushMutex) {
        pthread_mutex_destroy((pthread_mutex_t *)node->flushMutex);
        sqlite3_free(node->flushMutex);
        node->flushMutex = NULL;
    }
    if (node->walMaintMutex) {
        pthread_mutex_destroy((pthread_mutex_t *)node->walMaintMutex);
        sqlite3_free(node->walMaintMutex);
        node->walMaintMutex = NULL;
    }
    /* Key material does not outlive the node */
    memset(node->config.privkey, 0, sizeof(node->config.privkey));
    memset(node->encKey, 0, sizeof(node->encKey));

    sqlite3_free(node);
}

/*
** ============================================================================
** Batches
** ============================================================================
*/

/*
** Take the DA size limit from the contract's system config, if it has one.
*/
/* The contract's batch limit is not the only one: the chain's RPC takes so
** much calldata per transaction (HPP Sepolia: ~84.9 KB, below the contract's
** 128 KB). submitBatch adds 4 + 5*32 bytes of ABI and pads to 32. */
#define SUBMIT_ABI_OVERHEAD (4 + 5 * 32 + 31)
static void node_cap_batch_limit(HppliteNode *node) {
    size_t tx = node->config.maxTxBytes ? node->config.maxTxBytes
                                        : hpplite_network_max_tx_bytes(node->config.chainId);
    /* a transaction's calldata limit is the chain's (the mock stands in for it); a directory has none (#35) */
    if (node->l1 && strcmp(hpplite_l1_backend_name(node->l1), "dir") == 0) return;
    if (tx > SUBMIT_ABI_OVERHEAD && node->config.maxBatchBytes > tx - SUBMIT_ABI_OVERHEAD) {
        node->config.maxBatchBytes = tx - SUBMIT_ABI_OVERHEAD;
    }
}

static void node_learn_batch_limit(HppliteNode *node) {
    HppliteSystemConfig *cfg;
    if (!node || !node->l1) return;
    cfg = hpplite_l1_get_system_config(node->l1);
    if (cfg) {
        if (cfg->batchSizeLimit > 0) node->config.maxBatchBytes = (size_t)cfg->batchSizeLimit;
        if (cfg->leaseDuration > 0) node->leaseDuration = cfg->leaseDuration;
        /* The contract's checkpointInterval is the configuration of record
        ** (#44): a node given none takes it; one given its own writes it
        ** back (the owner's transaction, once per node), so that what the
        ** chain says and what the node does agree. */
        if (cfg->checkpointInterval > 0 && !node->checkpointSynced) {
            node->checkpointSynced = 1;
            if (!node->checkpointGiven) {
                node->config.checkpointInterval = (int)cfg->checkpointInterval;
            } else if ((uint64_t)node->config.checkpointInterval != cfg->checkpointInterval && !node->config.verifyOnly) {
                char *tx = hpplite_l1_set_checkpoint_interval(node->l1, (uint64_t)node->config.checkpointInterval);
                if (tx) {
                    hpplite_l1_wait_for_tx(node->l1, tx, 30);
                    sqlite3_free(tx);
                    fprintf(stderr, "hpplite: checkpoint interval on the contract set to %d (was %llu)\n",
                            node->config.checkpointInterval, (unsigned long long)cfg->checkpointInterval);
                } else {
                    fprintf(stderr, "hpplite: the contract's checkpoint interval is %llu, this node's %d; could not set it "
                                    "(the owner's transaction) - the node's value is used\n",
                            (unsigned long long)cfg->checkpointInterval, node->config.checkpointInterval);
                }
            }
        }
        hpplite_l1_system_config_free(cfg);
    }
    node_cap_batch_limit(node);
    node_set_stmt_limit(node);
    hpplite_l1_set_max_tx_gas(node->l1, node->config.maxTxGas ? node->config.maxTxGas
                                         : hpplite_network_max_tx_gas(node->config.chainId));
    node_check_rollup(node);
}

/*
** A database's history is on one rollup. Opened against another - a new
** wallet, another chain, a copied file - it must not post its batches there
** (nor re-post kept ones after "a reorg" that is no reorg), and must not
** replay a stranger's: it stops and says which rollup is its own. Bound the
** first time the rollup is known (a fresh wallet's is created at its first
** post, so this runs again before every submission). 1 ok, 0 not known
** yet, -1 another rollup.
*/
static int node_check_rollup(HppliteNode *node) {
    char c[43], d[43], z[80];
    int rc;
    if (node->wrongRollup) return -1;
    if (!node->l1 || !node->ctx || hpplite_l1_get_addresses(node->l1, c, d) != 0 || !c[0]) return 0;
    if (strcmp(c, "0x0000000000000000000000000000000000000000") == 0) return 0;
    snprintf(z, sizeof(z), "%llu:%s", (unsigned long long)node->config.chainId, c);
    rc = hpplite_bind_rollup(node->ctx, z);
    if (rc == -1) {
        hpplite_error(node->ctx, "this database's history is on rollup %s, not %s (another wallet, chain or "
                "copy?); not posting or replaying - open it with its own rollup, or start a new database",
                node->ctx->zRollup, z);
        node->wrongRollup = 1;
        node->state = HPPLITE_STATE_ERROR;
        return -1;
    }
    return rc == -2 ? 0 : 1;
}

/* the bytes inside an envelope: a page batch by its magic (#146), else JSON */
static HppliteBatch *node_batch_from_plain(uint64_t height, const uint8_t *plain, size_t n) {
    if (hpplite_pagebatch_is(plain, n)) {
        HppliteBatch *b = sqlite3_malloc64(sizeof(*b));
        if (!b) return NULL;
        memset(b, 0, sizeof(*b));
        b->height = height;
        b->bPages = 1;
        b->pPagesRaw = sqlite3_malloc64((sqlite3_uint64)n);
        if (!b->pPagesRaw) { sqlite3_free(b); return NULL; }
        memcpy(b->pPagesRaw, plain, n);
        b->nPagesRaw = n;
        /* every page batch carries its root in the trailer: what reads batches
        ** (the checkpoint flow, verify, the demo) sees it as the batch's (#154) */
        { HpplitePageBatch *pb = hpplite_pagebatch_decode(plain, n);
          if (pb) { memcpy(b->postStateRoot, pb->root, 32); b->hasRoot = 1; if (pb->nName) b->zDbName = sqlite3_mprintf("%.*s", (int)pb->nName, pb->zName); hpplite_pagebatch_free(pb); } }
        return b;
    }
    return hpplite_batch_from_json((const char *)plain);
}

/*
** Decode batch bytes from L1: open the envelope if sealed, else plaintext JSON.
*/
static HppliteBatch *node_decode_batch(HppliteNode *node, uint64_t height,
                                       const uint8_t *data, size_t dataLen) {
    HppliteBatch *batch = NULL;
    node->lastFetchForeign = 0;
    if (!data || dataLen == 0) return NULL;
    if (hpplite_envelope_is_pk(data, dataLen)) {
        /* sealed to readers' public keys (#15): the wallet's key or a viewing key opens it */
        uint8_t *plain = NULL;
        size_t plainLen = 0;
        int foreign = 0, encoding;
        if (node->config.hasPrivkey) plain = hpplite_envelope_open_pk(node->config.privkey, height, data, dataLen, &plainLen, &foreign, NULL);
        if (!plain && node->hasViewKey) plain = hpplite_envelope_open_pk(node->viewKey, height, data, dataLen, &plainLen, &foreign, NULL);
        if (!plain) {
            /* the wallet is always among its own batches' readers, so one it is not among was
            ** posted by another sequencer: foreign, skipped (#39). A viewing key alone proves
            ** nothing of the kind: revoked or never granted, it stops here. */
            node->lastFetchForeign = node->config.hasPrivkey ? foreign : 0;
            hpplite_error(node->ctx, "batch %llu: %s", (unsigned long long)height,
                    (node->config.hasPrivkey || node->hasViewKey)
                        ? "sealed to readers this key is not among (revoked, or posted by another sequencer)"
                        : "sealed to readers' keys and this node has none (privkey= or viewkey=)");
            return NULL;
        }
        encoding = hpplite_envelope_pk_encoding(data, dataLen);
        if (encoding == HPPLITE_ENCODING_DEFLATE) {
            size_t n2 = 0;
            uint8_t *inflated = hpplite_inflate(plain, plainLen, &n2);
            free(plain);
            if (!inflated) { hpplite_error(node->ctx, "batch %llu: sealed and compressed, but the compressed bytes do not inflate", (unsigned long long)height); return NULL; }
            plain = inflated; plainLen = n2;
        } else if (encoding != HPPLITE_ENCODING_NONE) {
            free(plain);
            hpplite_error(node->ctx, "batch %llu uses encoding %d, which this version does not know", (unsigned long long)height, encoding);
            return NULL;
        }
        batch = node_batch_from_plain(height, plain, plainLen);
        free(plain);
    } else if (hpplite_envelope_is_sealed(data, dataLen)) {
        const HppliteCipher *scheme = NULL;
        uint8_t *plain;
        if (!node->hasEncKey) {
            hpplite_error(node->ctx, "batch %llu is encrypted and this node has no key",
                    (unsigned long long)height);
            return NULL;
        }
        size_t plainLen = 0;
        plain = hpplite_envelope_open(node->encKey, height, data, dataLen, &plainLen, &scheme);
        if (!plain) {
            /* A known scheme that fails our tag was sealed with another key:
            ** posted under a lease we did not hold. Provably not ours. */
            node->lastFetchForeign = (scheme != NULL);
            hpplite_error(node->ctx, "batch %llu: %s", (unsigned long long)height,
                    scheme ? "not sealed with our key (posted by another sequencer)" : "unknown encryption scheme");
            return NULL;
        }
        {
            /* what the envelope's encoding byte says is inside (#12) */
            int encoding = hpplite_envelope_encoding(data, dataLen);
            if (encoding == HPPLITE_ENCODING_DEFLATE) {
                size_t n2 = 0;
                uint8_t *inflated = hpplite_inflate(plain, plainLen, &n2);
                free(plain);
                if (!inflated) {
                    hpplite_error(node->ctx, "batch %llu: sealed and compressed, but the compressed bytes do not inflate", (unsigned long long)height);
                    return NULL;
                }
                plain = inflated; plainLen = n2;
            } else if (encoding != HPPLITE_ENCODING_NONE) {
                free(plain);
                hpplite_error(node->ctx, "batch %llu uses encoding %d, which this version does not know", (unsigned long long)height, encoding);
                return NULL;
            }
        }
        batch = node_batch_from_plain(height, plain, plainLen);
        free(plain);
    } else if (node->cipher) {
        /* An encrypting node never posts plaintext, so a plaintext batch at
        ** its height was posted by someone else: unauthenticated SQL that
        ** every replica would run. Foreign, like a batch with a wrong tag.
        ** (encrypt=off nodes still read plaintext history.) */
        node->lastFetchForeign = 1;
        hpplite_error(node->ctx, "batch %llu is plaintext on an encrypting rollup: not ours", (unsigned long long)height);
        return NULL;
    } else if (hpplite_is_plain_compressed(data, dataLen)) {
        size_t n2 = 0;
        uint8_t *inflated = hpplite_inflate(data + HPPLITE_PLAIN_MAGIC_LEN, dataLen - HPPLITE_PLAIN_MAGIC_LEN, &n2);
        if (!inflated) {
            hpplite_error(node->ctx, "batch %llu: compressed plaintext that does not inflate", (unsigned long long)height);
            return NULL;
        }
        batch = node_batch_from_plain(height, inflated, n2);
        free(inflated);
    } else {
        batch = node_batch_from_plain(height, data, dataLen);
    }
    if (batch && batch->height != height) {
        /* the slot is the truth; a batch claiming another height is corrupt */
        hpplite_error(node->ctx, "batch at height %llu claims height %llu; corrupt",
                (unsigned long long)height, (unsigned long long)batch->height);
        hpplite_batch_free(batch);
        return NULL;
    }
    if (!batch) {
        hpplite_error(node->ctx, "batch %llu on L1 is not a batch this version can decode (%zu bytes)",
                (unsigned long long)height, dataLen);
    }
    return batch;
}

/*
** The batch at our next height is not ours (fails our authentication): it was
** posted by whoever held the lease. Nobody with our key can read it, so every
** key holder skips the height the same way and reconstruction goes on. The
** raw bytes are hashed into the chain of block hashes so the skip is part of
** the history, not an omission.
*/
static int node_skip_foreign_height(HppliteNode *node, uint64_t height) {
    size_t len = 0;
    uint8_t *raw;
    unsigned char h[32];
    int rc;
    /* Every batch failing our tag from height 1 on is not an intruder, it is
    ** the wrong key on our side (or someone else's rollup): never skip that. */
    if (height <= 1) {
        hpplite_error(node->ctx, "batch 1 is not sealed with our key: wrong key or wrong rollup");
        return -1;
    }
    raw = node_fetch_bytes(node, node->l1, height, &len);
    if (!raw) return -1;
    hpplite_sha256(raw, len, h);
    {
        unsigned char k[32];
        hpplite_keccak256(raw, len, k);          /* what the contract hashed into its chain (#33) */
        free(raw);
        rc = hpplite_skip_height(node->ctx, height, h, k);
    }
    if (rc == SQLITE_OK) {
        fprintf(stderr, "hpplite: batch %llu was posted by another sequencer; skipped (%llu so far)\n",
                (unsigned long long)height, (unsigned long long)node->ctx->nForeignBatches);
    }
    return rc == SQLITE_OK ? 0 : -1;
}

/*
** The posted bytes of a height: from the chain, or, for a height the chain
** only commits to (#11), from the store by the committed reference, checked
** against the committed hash. malloc'd, terminated; NULL with the reason in
** last_error when the bytes are in a store this node cannot reach.
*/
static uint8_t *node_fetch_bytes(HppliteNode *node, HppliteL1 *l1, uint64_t height, size_t *pLen) {
    uint8_t *data = hpplite_l1_get_batch(l1, height, pLen);
    unsigned char hash[32], got[32];
    char *zRef = NULL;
    uint8_t *receipt = NULL;
    size_t nReceipt = 0;
    if (data) return data;
    if (hpplite_l1_get_commitment(l1, height, hash, &zRef, &receipt, &nReceipt) != 0) return node_fetch_from_range(node, l1, height, pLen);   /* inside an anchored range (#164)? */
    sqlite3_free(receipt);                        /* verified where it is reported (hpplite-verify); the hash is the check here */
    if (!node->store) {
        hpplite_error(node->ctx, "batch %llu is in a store (%s) and no store= names one", (unsigned long long)height, zRef);
    } else {
        data = hpplite_store_get(node->store, zRef, pLen);
        if (!data) {
            hpplite_error(node->ctx, "batch %llu: the store has no %s: %s", (unsigned long long)height, zRef, hpplite_store_error(node->store));
        } else {
            hpplite_keccak256(data, *pLen, got);
            if (memcmp(got, hash, 32) != 0) {
                hpplite_error(node->ctx, "batch %llu: the store's bytes for %s are not what the chain committed to", (unsigned long long)height, zRef);
                free(data);
                data = NULL;
                *pLen = 0;
            }
        }
    }
    sqlite3_free(zRef);
    return data;
}

int hpplite_node_store_balance(HppliteNode *node, uint8_t out[32]) {
    unsigned char asset[20], me[20];
    uint64_t chainId = 0;
    time_t now = time(NULL);
    if (!node || !node->store || !node->l1 || !out) return -1;
    if (hpplite_store_payment(node->store, asset, &chainId, NULL, NULL) != 0) return -1;
    if (chainId == 0 || hpplite_l1_chain_id(node->l1) != chainId) return -1;      /* the asset is on another chain: not from here */
    if (!node->storeBalanceOk || now - node->storeBalanceAt >= 30) {
        if (hpplite_l1_get_my_address(node->l1, me) != 0) return -1;
        if (hpplite_l1_token_balance(node->l1, asset, me, node->storeBalance) != 0) return -1;
        node->storeBalanceOk = 1;
        node->storeBalanceAt = now;
    }
    memcpy(out, node->storeBalance, 32);
    return 0;
}

HppliteBatch *hpplite_node_fetch_batch(HppliteNode *node, uint64_t height) {
    size_t dataLen = 0;
    uint8_t *data;
    HppliteBatch *batch;
    if (!node || !node->l1) return NULL;
    data = node_fetch_bytes(node, node->l1, height, &dataLen);
    batch = node_decode_batch(node, height, data, dataLen);
    if (batch) {
        /* the sealed bytes stay with the batch: adopted, it is kept as
        ** posted, so a reorg that drops it can be repaired (#99) */
        batch->pPosted = data;
        batch->nPosted = dataLen;
        batch->bOwnsPosted = 1;
    } else {
        free(data);
    }
    return batch;
}

static HppliteBatch *node_fetch_batch(HppliteNode *node, uint64_t height) {
    return hpplite_node_fetch_batch(node, height);
}

/*
** If L1 already holds a batch at our next height, it was produced from this
** file's pending rows by an earlier attempt (process died after submitting,
** or the RPC reply was lost). Adopt it so we neither resubmit nor skip it.
** Returns 1 if adopted, 0 if L1 has nothing at that height, -1 on divergence.
*/
/*
** L1 has fewer batches than we finalized: a chain reorganisation dropped
** the last ones (#49). Every later submission would fail as non-sequential.
** Post the lost heights again from the bytes kept at finalize, unchanged
** (same bytes, same block hashes: replicas that saw them before agree). A
** height no longer kept cannot be restored: stop rather than post past a
** gap. Returns 1 when L1 is caught up, 0 to try again later, -1 to stop.
*/
static int node_repost_lost(HppliteNode *node, uint64_t lastBatch) {
    uint64_t h;
    if (node_check_rollup(node) < 0) return -1;    /* not a reorg: another rollup */
    for (h = lastBatch + 1; h < node->ctx->blockHeight; h++) {
        size_t len = 0;
        uint8_t *bytes = hpplite_posted_batch(node->ctx, h, &len);
        char *txHash;
        int ok;
        if (!bytes) {
            hpplite_error(node->ctx, "L1 has %llu batches but this database finalized %llu (a chain "
                    "reorganisation?), and batch %llu is no longer kept to post again; stopping rather "
                    "than post past the gap", (unsigned long long)lastBatch,
                    (unsigned long long)(node->ctx->blockHeight - 1), (unsigned long long)h);
            node->state = HPPLITE_STATE_ERROR;
            return -1;
        }
        if (!node_ensure_lease(node)) { free(bytes); return 0; }
        txHash = hpplite_l1_submit_batch(node->l1, node->instanceId, h, bytes, len);
        free(bytes);
        ok = txHash && hpplite_l1_wait_for_tx(node->l1, txHash, 30) == 1;
        sqlite3_free(txHash);
        if (!ok) return 0;
        fprintf(stderr, "hpplite: L1 lost batch %llu (a chain reorganisation?); posted it again\n",
                (unsigned long long)h);
    }
    return 1;
}

static int node_adopt_existing(HppliteNode *node) {
    uint64_t lastBatch = 0, totalBatches = 0;
    unsigned char latestHash[32];
    HppliteBatch *batch;
    int rc;

    if (hpplite_l1_get_da_state(node->l1, &lastBatch, &totalBatches, latestHash) != 0) return 0;
    {
        /* the batches the store holds above the chain's height are not lost: they wait for the anchor (#164) */
        uint64_t floor = lastBatch + node_unanchored_above(node, lastBatch);
        if (floor + 1 < node->ctx->blockHeight) {
            node_repost_lost(node, lastBatch);
            return 0;                  /* the batch at our height is still to post */
        }
    }
    if (lastBatch < node->ctx->blockHeight) return 0;

    batch = node_fetch_batch(node, node->ctx->blockHeight);
    if (!batch) {
        /* someone else's batch sits at our height: step over it and post after it */
        if (node->lastFetchForeign && node_skip_foreign_height(node, node->ctx->blockHeight) == 0) return 1;
        return 0;
    }
    rc = batch->bPages ? node_adopt_pages(node, batch) : hpplite_adopt_batch(node->ctx, batch);
    hpplite_batch_free(batch);
    if (rc == SQLITE_OK) {
        node->batchesProduced++;
        node->batchesSinceCheckpoint++;
        return 1;
    }
    /* It matched but could not be finalized now (an application statement or
    ** transaction is open: the gate) - a deferral, not a fork: next flush.
    ** Told apart from "nothing at our height": the batch is on L1, and
    ** posting another at this height would only be refused (#90) */
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) return 2;
    /* The batch matched too, but its finalize could not be written (a full
    ** disk, an I/O error): a local failure, retried at the next flush - not
    ** a fork, which only a mismatch is (#119) */
    if (rc != SQLITE_MISMATCH) {
        hpplite_error(node->ctx, "could not finalize L1 batch %llu (%s): retried",
                (unsigned long long)node->ctx->blockHeight, sqlite3_errstr(rc));
        return 2;
    }
    hpplite_error(node->ctx, "L1 batch %llu does not match local pending SQL",
            (unsigned long long)node->ctx->blockHeight);
    return -1;
}

/*
** Build, submit and finalize one batch. *pMore is set when pending SQL
** remains because the batch was cut at the size limit.
** Returns the confirmed height, or 0.
*/
/*
** Make sure this instance holds the sequencer lease before it posts.
**
** The contract lets anyone claim an expired lease, so "a lease is active"
** is not "our lease is active": a batch submitted under a foreign lease
** reverts (and costs gas every interval). The lease is read from L1, renewed
** when ours is past half its life, claimed when free, and left alone when
** someone else holds it. Returns 1 when posting is allowed.
*/
static int node_ensure_lease(HppliteNode *node) {
    int64_t now = node_current_time_ms();
    HppliteL1State *st;
    uint64_t chainNow, remaining;
    int ours;

    if (now < node->leaseRecheckAt) return node->hasLease;

    st = hpplite_l1_get_state(node->l1);
    if (!st) {
        hpplite_error(node->ctx, "cannot read the sequencer lease from L1; not posting");
        node->leaseRecheckAt = now + 5000;
        return 0;
    }
    chainNow = st->currentTimestamp ? st->currentTimestamp : (uint64_t)time(NULL);
    remaining = st->leaseExpiry > chainNow ? st->leaseExpiry - chainNow : 0;
    ours = remaining > 0 && memcmp(st->sequencerInstance, node->instanceId, 32) == 0;
    if (st->leaseDuration > 0) node->leaseDuration = st->leaseDuration;
    {
        /* A stranger's lease on a rollup we own (possible on rollups from the
        ** pre-VERSION-5 factory): the owner expires it and takes it back. A
        ** sibling instance of our own wallet is left alone - no takeover. */
        unsigned char me[20];
        int stranger = remaining > 0 && !ours && hpplite_l1_get_my_address(node->l1, me) == 0
                       && memcmp(st->sequencerWallet, me, 20) != 0;
        int weOwn = stranger && memcmp(st->owner, me, 20) == 0;
        hpplite_l1_state_free(st);
        if (stranger && weOwn) {
            char *tx = hpplite_l1_release_lease(node->l1, NULL);   /* forceExpireLease: owner only */
            hpplite_error(node->ctx, "another wallet holds the lease of our rollup; %s",
                    tx ? "expiring it as owner" : "could not expire it");
            if (tx) {
                hpplite_l1_wait_for_tx(node->l1, tx, 30);
                sqlite3_free(tx);
                remaining = 0;                                     /* free now: claim below */
            }
        } else if (stranger) {
            hpplite_error(node->ctx, "another wallet holds the lease and we are not the owner");
        }
    }

    if (getenv("HPPLITE_TRACE_LEASE"))
        fprintf(stderr, "hpplite: lease check: remaining %llu, ours %d, hasLease %d\n", (unsigned long long)remaining, ours, node->hasLease);
    if (remaining > 0 && !ours) {
        if (!node->warnedForeignLease) {
            hpplite_error(node->ctx, "another sequencer instance holds the lease (%llu s left); "
                            "batches are kept pending until it expires", (unsigned long long)remaining);
            node->warnedForeignLease = 1;
        }
        node->hasLease = 0;
        node->leaseHeldElsewhere = 1;
        node->leaseRecheckAt = now + (int64_t)(remaining < 30 ? remaining : 30) * 1000;
        return 0;
    }
    node->warnedForeignLease = 0;

    if (ours && remaining * 2 < node->leaseDuration) {
        if (hpplite_node_renew_lease(node) == 0) remaining = node->leaseDuration;
    } else if (!ours) {
        if (hpplite_node_claim_lease(node) != 0) {
            if (getenv("HPPLITE_TRACE_LEASE")) fprintf(stderr, "hpplite: lease check: claim failed\n");
            node->leaseRecheckAt = now + 5000;
            return 0;
        }
        if (getenv("HPPLITE_TRACE_LEASE")) fprintf(stderr, "hpplite: lease check: claim ok\n");
        /* a mined claim can still have reverted (this wallet is not the
        ** sequencer the owner named, #17): the chain says whose it is */
        st = hpplite_l1_get_state(node->l1);
        if (st) {
            chainNow = st->currentTimestamp ? st->currentTimestamp : (uint64_t)time(NULL);
            ours = st->leaseExpiry > chainNow && memcmp(st->sequencerInstance, node->instanceId, 32) == 0;
            hpplite_l1_state_free(st);
            if (!ours) {
                hpplite_error(node->ctx, "the lease claim did not take: this wallet is not the rollup's sequencer; batches are kept pending");
                node->hasLease = 0;
                node->leaseRecheckAt = now + 30000;
                return 0;
            }
        }
        remaining = node->leaseDuration;
    }
    node->hasLease = 1;
    node->leaseHeldElsewhere = 0;
    /* Look again well before the renewal point, never later than a minute */
    {
        uint64_t s = remaining / 4;
        if (s < 1) s = 1;
        if (s > 60) s = 60;
        node->leaseRecheckAt = now + (int64_t)s * 1000;
    }
    return 1;
}

/*
** Deterministic crash points for the crash harness (tests/stress/crash_hpplite.c):
** HPPLITE_TEST_CRASH_AT=after_submit|after_finalize|mid_series makes the
** process die (like kill -9) at that point of a flush. Compiled only into
** mock-L1 builds.
*/
#ifdef HPPLITE_TEST_HOOKS
static void test_crash_point(const char *zName) {
    const char *z = getenv("HPPLITE_TEST_CRASH_AT");
    if (z && strcmp(z, zName) == 0) _exit(137);
}
#else
#define test_crash_point(zName) ((void)0)
#endif

/* Page mode (#146): a batch of pages from the queued commits, deflated and
** sealed as any batch, posted, finalized in the sidecar. */
/* Raw page records deflate about this many times as diffs: the build's budget
** of raw bytes for a posted limit, corrected when a batch comes out over it */
#define HPPLITE_PAGE_DEFLATE_GUESS 8

/* A replica takes a page batch into its file (#146): the records applied at
** their offsets (a diff XORed onto the page as it is), the file cut to the
** batch's size, page 1 marked rollback-journal with the change counter moved
** so the connections reading the file notice, all under SQLite's exclusive
** lock taken through a connection of our own; then the root, compared with
** the batch's trailer, and the meta row. */
static int node_apply_pages(HppliteNode *node, const HppliteBatch *batch) {
    HpplitePageBatch *b = hpplite_pagebatch_decode(batch->pPagesRaw, batch->nPagesRaw);
    const char *zFile = sqlite3_db_filename(node->db, "main");
    sqlite3 *adb = NULL;
    sqlite3_file *pf = NULL;
    unsigned char *page = NULL, root[32];
    unsigned i, counter = 0;
    int rc;
    if (!b) { hpplite_error(node->ctx, "batch %llu: page batch that does not decode", (unsigned long long)batch->height); return SQLITE_CORRUPT; }
    if (!node->pm) { hpplite_pagebatch_free(b); hpplite_error(node->ctx, "batch %llu is a page batch and this node is not in page mode (capture=pages)", (unsigned long long)batch->height); return SQLITE_MISMATCH; }
    if (batch->height > 1 && batch->height == node->ctx->blockHeight && b->piece <= 1 && !node->ctx->openTxnId) {
        /* the file must be where the sidecar says the last batch left it: a
        ** file deleted or restored while its sidecar stayed would take diffs
        ** onto the wrong pages (the root check below catches that, this says why).
        ** Not between the pieces of one commit, nor after a series the sequencer
        ** did not finish (open_txn in the sidecar): the sidecar holds the commit's
        ** final root from its first piece on (#152) */
        hpplite_pagemode_root(node->pm, root);
        if (memcmp(root, node->ctx->stateRoot, 32) != 0) {
            hpplite_pagebatch_free(b);
            hpplite_error(node->ctx, "the database file is not at the root its bookkeeping (%s-hpplite.db) recorded at height %llu: remove the sidecar with the file, or restore the file", zFile, (unsigned long long)node->ctx->blockHeight);
            return SQLITE_MISMATCH;
        }
    }
    if (!node->config.verifyOnly) {
        /* a node that may post later keeps WAL for its own writes; pages are
        ** applied in rollback-journal mode (its readers check the change
        ** counter, and no WAL shadows the file), so for the sync it switches:
        ** possible only while its connection is the file's only one, which
        ** the open is */
        char mode[16];
        sqlite3_stmt *q = NULL;
        mode[0] = 0;
        if (sqlite3_prepare_v2(node->db, "PRAGMA journal_mode", -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) snprintf(mode, sizeof mode, "%s", sqlite3_column_text(q, 0));
        sqlite3_finalize(q);
        if (strcasecmp(mode, "wal") == 0) {
            node_ck_close(node);
            node_pins_close(node);
            hpplite_pagemode_pause(node->pm);
            if (hppliteInternalExec(node->ctx, node->db, "PRAGMA journal_mode=DELETE") != SQLITE_OK) {
                hpplite_pagemode_resume(node->pm);
                hpplite_pagebatch_free(b);
                hpplite_error(node->ctx, "batch %llu: pages cannot be applied while other connections hold the file in WAL mode", (unsigned long long)batch->height);
                return SQLITE_BUSY;
            }
            node->pagesWalSuspended = 1;
        }
    }
    if (node->pieceDb) {
        /* a piece after the first (#152): the lock held since */
        adb = node->pieceDb; pf = node->piecePf; rc = SQLITE_OK;
    } else {
    hpplite_internal_open_begin();
    rc = sqlite3_open_v2(zFile, &adb, SQLITE_OPEN_READWRITE, NULL);
    hpplite_internal_open_end();
    if (rc != SQLITE_OK) { if (adb) sqlite3_close(adb); hpplite_pagebatch_free(b); return rc; }
    sqlite3_busy_timeout(adb, 5000);
    rc = sqlite3_exec(adb, "BEGIN EXCLUSIVE", NULL, NULL, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_file_control(adb, "main", SQLITE_FCNTL_FILE_POINTER, &pf);
    }
    page = sqlite3_malloc64((sqlite3_uint64)b->pageSize);
    if (rc == SQLITE_OK && (!pf || !pf->pMethods || !page)) rc = SQLITE_ERROR;
    for (i = 0; rc == SQLITE_OK && i < b->nPages; i++) {
        sqlite3_int64 off = (sqlite3_int64)(b->aPage[i].pgno - 1) * (sqlite3_int64)b->pageSize;
        unsigned k;
        if (b->aPage[i].flags & HPPLITE_PAGE_DIFF) {
            rc = pf->pMethods->xRead(pf, page, (int)b->pageSize, off);
            if (rc == SQLITE_IOERR_SHORT_READ) rc = SQLITE_OK;          /* a diff onto a page the file does not have yet reads as zeros */
            if (rc != SQLITE_OK) break;
            for (k = 0; k < b->pageSize; k++) page[k] ^= b->aPage[i].body[k];
        } else memcpy(page, b->aPage[i].body, b->pageSize);
        if (b->aPage[i].pgno == 1) { counter = ((unsigned)page[24] << 24) | ((unsigned)page[25] << 16) | ((unsigned)page[26] << 8) | page[27]; page[18] = page[19] = 1; }
        rc = pf->pMethods->xWrite(pf, page, (int)b->pageSize, off);
        if (rc == SQLITE_OK) rc = hpplite_pagemode_applied(node->pm, b->aPage[i].pgno, page);
    }
    if (rc == SQLITE_OK) {
        /* page 1 as the file has it: the counters moved, rollback-journal mode */
        rc = pf->pMethods->xRead(pf, page, (int)b->pageSize, 0);
        if (rc == SQLITE_OK) {
            counter = (((unsigned)page[24] << 24) | ((unsigned)page[25] << 16) | ((unsigned)page[26] << 8) | page[27]) + 1;
            page[24] = (unsigned char)(counter >> 24); page[25] = (unsigned char)(counter >> 16); page[26] = (unsigned char)(counter >> 8); page[27] = (unsigned char)counter;
            memcpy(page + 92, page + 24, 4);
            page[18] = page[19] = 1;
            rc = pf->pMethods->xWrite(pf, page, (int)b->pageSize, 0);
            if (rc == SQLITE_OK) rc = hpplite_pagemode_applied(node->pm, 1, page);
        }
    }
    if (rc == SQLITE_OK) rc = pf->pMethods->xTruncate(pf, (sqlite3_int64)b->commitSize * (sqlite3_int64)b->pageSize);
    if (rc == SQLITE_OK) rc = hpplite_pagemode_applied_size(node->pm, b->commitSize);
    if (rc == SQLITE_OK) rc = pf->pMethods->xSync(pf, SQLITE_SYNC_NORMAL);
    if (rc == SQLITE_OK && b->bMore) {
        /* a piece with more after it: the lock stays until the last, so no
        ** reader of this replica sees half a commit */
        node->pieceDb = adb; node->piecePf = pf;
    } else {
        sqlite3_exec(adb, "ROLLBACK", NULL, NULL, NULL);          /* nothing of SQLite's to commit; the lock goes */
        sqlite3_close(adb);
        node->pieceDb = NULL; node->piecePf = NULL;
    }
    sqlite3_free(page);
    if (rc != SQLITE_OK) { hpplite_pagebatch_free(b); hpplite_error(node->ctx, "batch %llu: its pages could not be written (rc=%d)", (unsigned long long)batch->height, rc); return rc; }
    hpplite_pagemode_root(node->pm, root);
    /* a piece with more after it carries the whole commit's root: checked at the last piece (#152) */
    if (!b->bMore) node->ctx->nRootsCompared++;
    if (!b->bMore && memcmp(root, b->root, 32) != 0) {
        node->ctx->nRootMismatches++;
        hpplite_error(node->ctx, "batch %llu: the root after applying it differs from the posted one", (unsigned long long)batch->height);
        hpplite_pagebatch_free(b);
        return SQLITE_MISMATCH;
    }
    node->ctx->rootCmpHeight = batch->height;          /* for the verifier: compared with the on-chain checkpoint (#5) */
    memcpy(node->ctx->rootCmp, b->root, 32);
    rc = hppliteAdvanceMetaPages(node->ctx, batch->height, batch->pPosted, batch->nPosted, b->root, b->bMore);   /* the posted bytes: the chain hash (#33) */
    hpplite_pagebatch_free(b);
    return rc;
}

/* The sequencer finds its own page batch on the chain, not finalized here (a
** commit that failed, a crash): the same commits, still queued, build the
** same pages; the roots must agree, then the chain's bytes are the posted copy */
static int node_adopt_pages(HppliteNode *node, const HppliteBatch *batch) {
    HpplitePageBatch *b = hpplite_pagebatch_decode(batch->pPagesRaw, batch->nPagesRaw);
    unsigned char *bytes = NULL, root[32];
    size_t len = 0;
    int more = 0, rc;
    if (!b) return SQLITE_CORRUPT;
    if (!node->pm || b->lastCommit < b->firstCommit) { hpplite_pagebatch_free(b); return SQLITE_MISMATCH; }
    if (node->ctx->inflightHeight == batch->height && memcmp(node->ctx->inflightRoot, b->root, 32) == 0) {
        /* the sidecar says this node posted it and died before finalizing: the
        ** file is at or past it (it is where the pages came from), so it is taken
        ** as posted; whatever the file holds beyond it, the first flush sees in
        ** the root and ships the file whole again. The queue, if the process is
        ** the same, holds the commits it was built from: settled too. */
        rc = hppliteAdvanceMetaPages(node->ctx, batch->height, batch->pPosted, batch->nPosted, b->root, b->bMore);
        if (rc == SQLITE_OK && hpplite_pagemode_pending(node->pm) > 0) {
            unsigned char r2[32]; int more2 = 0; unsigned char *bytes2 = NULL; size_t len2 = 0;
            if (hpplite_pagemode_build_n(node->pm, (unsigned)(b->lastCommit - b->firstCommit + 1), b->piece ? b->nPages : 0, &bytes2, &len2, r2, &more2) == SQLITE_OK) {
                if (memcmp(r2, b->root, 32) == 0) hpplite_pagemode_finalized(node->pm); else hpplite_pagemode_failed(node->pm);
                sqlite3_free(bytes2);
            }
        }
        hpplite_pagebatch_free(b);
        return rc;
    }
    if (hpplite_pagemode_captured(node->pm) == 0) {
        /* nothing of this node's own was captured: it is behind the chain (a
        ** fresh file, a file resumed elsewhere), whatever the file as opened
        ** holds; the batch is applied, as a replica would, and the file as
        ** opened (queued whole in case it was ahead) is dropped: the chain
        ** rebuilds it */
        hpplite_pagemode_discard(node->pm);
        hpplite_pagebatch_free(b);
        return node_apply_pages(node, batch);
    }
    rc = hpplite_pagemode_build_n(node->pm, (unsigned)(b->lastCommit - b->firstCommit + 1), b->piece ? b->nPages : 0, &bytes, &len, root, &more);
    if (rc != SQLITE_OK) { hpplite_pagebatch_free(b); return rc == SQLITE_DONE ? SQLITE_MISMATCH : rc; }
    if (memcmp(root, b->root, 32) != 0) {
        hpplite_pagemode_failed(node->pm);
        sqlite3_free(bytes); hpplite_pagebatch_free(b);
        hpplite_error(node->ctx, "page batch %llu on the chain is not what this node would post (a fork?)", (unsigned long long)batch->height);
        return SQLITE_MISMATCH;
    }
    rc = hppliteAdvanceMetaPages(node->ctx, batch->height, batch->pPosted, batch->nPosted, b->root, b->bMore);
    if (rc == SQLITE_OK) hpplite_pagemode_finalized(node->pm); else hpplite_pagemode_failed(node->pm);
    sqlite3_free(bytes); hpplite_pagebatch_free(b);
    return rc;
}


/*
** ============================================================================
** Ranges (#164): the store holds the batches, the chain one commitment per
** anchor interval. A sequencer with a store on a chain that takes ranges
** (DA v3) does not commit each batch: the batch is finalized once the store
** has it (a row in _hpplite_unanchored), and the anchor, on its own timer,
** at close, or on hpplite_anchor(), writes a manifest naming every batch
** since the last anchor to the store and commits the range: the chain hash
** after it, the state root (the anchor is the checkpoint), the manifest's
** digest, the store's receipt. A replica reaching a height inside a range
** fetches the manifest by the digest's CID, checks that it folds to the
** anchored chain hash, and the batch by the manifest's reference.
** ============================================================================
*/

/* a sequencer whose batches go to a store on a chain with ranges */
static int node_ranges(HppliteNode *node) {
    return node && node->store && node->l1 && !node->config.verifyOnly && hpplite_l1_has_ranges(node->l1);
}

/* the commitment of the batch being flushed, kept for the finalize's row */
static void node_anchor_row(HppliteNode *node, const unsigned char hash[32], const char *zRef, const uint8_t *receipt, size_t nReceipt) {
    free(node->zAnchorRef);
    node->zAnchorRef = strdup(zRef);
    memcpy(node->anchorRow.hash, hash, 32);
    node->anchorRow.zRef = node->zAnchorRef;
    node->anchorRow.receipt = NULL;
    node->anchorRow.nReceipt = 0;
    if (receipt && nReceipt && nReceipt <= sizeof node->anchorReceipt) {
        memcpy(node->anchorReceipt, receipt, nReceipt);
        node->anchorRow.receipt = node->anchorReceipt;
        node->anchorRow.nReceipt = nReceipt;
    }
}

/* how many batches above the chain's height the store holds (rows of _hpplite_unanchored) */
static uint64_t node_unanchored_above(HppliteNode *node, uint64_t lastBatch) {
    sqlite3_stmt *q = NULL;
    uint64_t n = 0;
    int saved;
    if (!node->ctx || !node->ctx->bkDb) return 0;
    saved = hppliteBkEnter(node->ctx);
    if (sqlite3_prepare_v2(node->ctx->bkDb, "SELECT count(*) FROM _hpplite_unanchored WHERE height > ?1", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, (sqlite3_int64)lastBatch);
        if (sqlite3_step(q) == SQLITE_ROW) n = (uint64_t)sqlite3_column_int64(q, 0);
    }
    sqlite3_finalize(q);
    hppliteBkLeave(node->ctx, saved);
    return n;
}

static void hex32(const unsigned char *b, char out[67]) { out[0] = '0'; out[1] = 'x'; hpplite_bytes_to_hex(b, 32, out + 2); }

/*
** The anchor: every row of _hpplite_unanchored above the chain's height,
** in one manifest at the store and one commitment on the chain. 1 anchored,
** 0 nothing to anchor, -1 the chain or the store refused (the rows stay;
** the next tick tries again).
*/
static int node_anchor(HppliteNode *node) {
    uint64_t lastBatch = 0, total = 0, from = 0, to = 0, expect;
    unsigned char latest[32], chainAfter[32], root[32], digest[32], payTo[20];
    uint8_t compact[HPPLITE_RECEIPT_SIZE];
    cJSON *m = NULL, *batches;
    sqlite3_stmt *q = NULL;
    char *text = NULL, *zRef = NULL, *zReceipt = NULL, *txHash = NULL, cid[64], hx[67];
    HppliteRange r;
    int saved, rc = -1, nCompact = -1, hasRoot = 0, confirmed = 0;
    HppliteCtx *ctx = node->ctx;
    if (!node_ranges(node) || !ctx || !ctx->bkDb) return 0;
    if (hpplite_l1_get_da_state(node->l1, &lastBatch, &total, latest) != 0) { hpplite_error(ctx, "anchor: the chain's state could not be read"); return -1; }
    node->anchoredHeight = lastBatch;
    /* rows the chain already covers (an anchor confirmed after the delete failed, or a restart);
    ** a bookkeeping transaction, so the write is not taken for a foreign one (#37) */
    if (node_unanchored_above(node, 0) > node_unanchored_above(node, lastBatch)) {
        int trc = hppliteBkBegin(ctx, 1);
        if (trc == SQLITE_OK) {
            saved = hppliteBkEnter(ctx);
            if (sqlite3_prepare_v2(ctx->bkDb, "DELETE FROM _hpplite_unanchored WHERE height <= ?1", -1, &q, NULL) == SQLITE_OK) { sqlite3_bind_int64(q, 1, (sqlite3_int64)lastBatch); trc = sqlite3_step(q) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR; }
            sqlite3_finalize(q); q = NULL;
            hppliteBkLeave(ctx, saved);
            hppliteBkEnd(ctx, trc);
        }
    }
    saved = hppliteBkEnter(ctx);
    m = cJSON_CreateObject();
    batches = cJSON_AddArrayToObject(m, "batches");
    cJSON_AddNumberToObject(m, "v", 1);
    hex32(node->instanceId, hx); cJSON_AddStringToObject(m, "instance", hx);
    expect = lastBatch + 1;
    if (sqlite3_prepare_v2(ctx->bkDb, "SELECT height, hash, ref, receipt, chain_after, root, chain_before FROM _hpplite_unanchored ORDER BY height", -1, &q, NULL) == SQLITE_OK) {
        while (sqlite3_step(q) == SQLITE_ROW) {
            uint64_t h = (uint64_t)sqlite3_column_int64(q, 0);
            cJSON *b;
            if (h != expect) {
                hpplite_error(ctx, "anchor: batch %llu is at the store but %llu is not: the batches since the chain's height %llu are not whole",
                              (unsigned long long)h, (unsigned long long)expect, (unsigned long long)lastBatch);
                hppliteBkLeave(ctx, saved); sqlite3_finalize(q); cJSON_Delete(m); return -1;
            }
            if (from == 0) { from = h; if (sqlite3_column_bytes(q, 6) == 32) { hex32(sqlite3_column_blob(q, 6), hx); cJSON_AddStringToObject(m, "chain_before", hx); } }
            to = h; expect = h + 1;
            b = cJSON_CreateObject();
            cJSON_AddNumberToObject(b, "h", (double)h);
            if (sqlite3_column_bytes(q, 1) == 32) { hex32(sqlite3_column_blob(q, 1), hx); cJSON_AddStringToObject(b, "hash", hx); }
            cJSON_AddStringToObject(b, "ref", (const char *)sqlite3_column_text(q, 2));
            if (sqlite3_column_type(q, 3) == SQLITE_BLOB && sqlite3_column_bytes(q, 3) > 0) {
                char *rx = malloc((size_t)sqlite3_column_bytes(q, 3) * 2 + 3);
                if (rx) { rx[0] = '0'; rx[1] = 'x'; hpplite_bytes_to_hex(sqlite3_column_blob(q, 3), (size_t)sqlite3_column_bytes(q, 3), rx + 2); cJSON_AddStringToObject(b, "receipt", rx); free(rx); }
            }
            if (sqlite3_column_bytes(q, 4) == 32) memcpy(chainAfter, sqlite3_column_blob(q, 4), 32);
            if (sqlite3_column_bytes(q, 5) == 32) { memcpy(root, sqlite3_column_blob(q, 5), 32); hasRoot = 1; } else hasRoot = 0;
            cJSON_AddItemToArray(batches, b);
        }
    }
    sqlite3_finalize(q);
    hppliteBkLeave(ctx, saved);
    if (from == 0) { cJSON_Delete(m); return 0; }
    if (!hasRoot) hpplite_get_state_root(ctx, root);     /* row capture: the trees give the root after the last finalized batch */
    cJSON_AddNumberToObject(m, "from", (double)from);
    cJSON_AddNumberToObject(m, "to", (double)to);
    hex32(chainAfter, hx); cJSON_AddStringToObject(m, "chain_after", hx);
    hex32(root, hx); cJSON_AddStringToObject(m, "root", hx);
    text = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    if (!text) return -1;
    hpplite_store_sha256((const uint8_t *)text, strlen(text), digest);
    hpplite_store_cid_from_sha256(digest, cid);
    zRef = hpplite_store_put(node->store, (const uint8_t *)text, strlen(text), &zReceipt);
    node->storeBalanceOk = 0;
    if (!zRef) {
        hpplite_error(ctx, "anchor: the store did not take the manifest of batches %llu-%llu: %s", (unsigned long long)from, (unsigned long long)to, hpplite_store_error(node->store));
        free(text); sqlite3_free(zReceipt); return -1;
    }
    if (strcmp(zRef, cid) != 0) {
        /* a replica finds the manifest by the digest the chain carries: the store must name it so */
        hpplite_error(ctx, "anchor: the store names the manifest %s, not its digest's CID %s: replicas could not find it; not anchored", zRef, cid);
        free(text); sqlite3_free(zRef); sqlite3_free(zReceipt); return -1;
    }
    nCompact = (zReceipt && hpplite_store_domain(node->store, payTo, NULL) == 0) ? hpplite_store_receipt_compact(zReceipt, payTo, compact) : -1;
    memset(&r, 0, sizeof r);
    r.fromHeight = from; r.toHeight = to;
    memcpy(r.chainHashAfter, chainAfter, 32);
    memcpy(r.stateRoot, root, 32);
    memcpy(r.manifestDigest, digest, 32);
    if (nCompact > 0) { r.receipt = compact; r.nReceipt = (size_t)nCompact; }
    if (!node_ensure_lease(node)) { free(text); sqlite3_free(zRef); sqlite3_free(zReceipt); return -1; }
    txHash = hpplite_l1_commit_range(node->l1, node->instanceId, &r);
    if (txHash) {
        confirmed = hpplite_l1_wait_for_tx(node->l1, txHash, 30) == 1;
        if (confirmed) snprintf(node->lastAnchorTx, sizeof node->lastAnchorTx, "%s", txHash);
        sqlite3_free(txHash);
    }
    if (!confirmed) {
        /* the transaction may have landed despite the lost answer: the chain's height says */
        uint64_t nowLast = 0;
        if (hpplite_l1_get_da_state(node->l1, &nowLast, &total, latest) == 0 && nowLast >= to) confirmed = 1;
    }
    if (confirmed) {
        int trc = hppliteBkBegin(ctx, 1);
        if (trc == SQLITE_OK) {
            saved = hppliteBkEnter(ctx);
            if (sqlite3_prepare_v2(ctx->bkDb, "DELETE FROM _hpplite_unanchored WHERE height <= ?1", -1, &q, NULL) == SQLITE_OK) { sqlite3_bind_int64(q, 1, (sqlite3_int64)to); trc = sqlite3_step(q) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR; }
            sqlite3_finalize(q); q = NULL;
            hppliteBkLeave(ctx, saved);
            trc = hppliteBkEnd(ctx, trc);
        }
        if (trc != SQLITE_OK) hpplite_error(ctx, "anchor: batches %llu-%llu are anchored but their rows could not be cleared (rc=%d); cleared at the next anchor", (unsigned long long)from, (unsigned long long)to, trc);
        node->anchoredHeight = to;
        /* the anchor is the checkpoint */
        hpplite_set_checkpoint(ctx, to, root);
        node->lastCheckpointHeight = to;
        memcpy(node->lastCheckpointRoot, root, HPPLITE_HASH_SIZE);
        node->batchesSinceCheckpoint = 0;
        if (node->onCheckpointSubmitted) node->onCheckpointSubmitted(node->callbackArg, from, to, node->lastAnchorTx);
        hpplite_error_clear(ctx);
        rc = 1;
    } else {
        hpplite_error(ctx, "anchor: the chain did not take the commitment of batches %llu-%llu; tried again at the next anchor", (unsigned long long)from, (unsigned long long)to);
        rc = -1;
    }
    free(text); sqlite3_free(zRef); sqlite3_free(zReceipt);
    return rc;
}

uint64_t hpplite_node_anchor(HppliteNode *node) {
    pthread_mutex_t *mutex;
    int rc;
    uint64_t h = 0;
    if (!node) return 0;
    mutex = (pthread_mutex_t *)node->flushMutex;
    if (mutex) pthread_mutex_lock(mutex);
    if (node->state == HPPLITE_STATE_INIT) hpplite_node_start(node);
    rc = node_anchor(node);
    if (rc == 1) h = node->anchoredHeight;
    if (node->config.anchorIntervalMs > 0) node->anchorAt = node_current_time_ms() + node->config.anchorIntervalMs;
    if (mutex) pthread_mutex_unlock(mutex);
    return h;
}

/* a replica: the bytes of a height inside an anchored range, by the manifest the chain's digest names */
static uint8_t *node_fetch_from_range(HppliteNode *node, HppliteL1 *l1, uint64_t height, size_t *pLen) {
    HppliteRange r;
    uint8_t *data = NULL;
    cJSON *b = NULL, *e;
    unsigned char want[32], got[32];
    const char *zRef = NULL;
    if (hpplite_l1_get_range(l1, height, &r) != 0) return NULL;
    if (!node->store) {
        hpplite_error(node->ctx, "batch %llu is in a store (anchored in range %llu-%llu) and no store= names one",
                      (unsigned long long)height, (unsigned long long)r.fromHeight, (unsigned long long)r.toHeight);
        hpplite_l1_range_clear(&r);
        return NULL;
    }
    if (!node->manifest || node->manifestFrom != r.fromHeight || node->manifestTo != r.toHeight) {
        /* the manifest, by its digest's CID; it must be the bytes the chain named, and its batches must fold to the anchored chain hash */
        char cid[64], hx[67];
        size_t n = 0;
        uint8_t *text, digest[32];
        unsigned char chain[32], hash[32];
        cJSON *m, *arr;
        const cJSON *cb;
        uint64_t h = r.fromHeight;
        hpplite_store_cid_from_sha256(r.manifestDigest, cid);
        text = hpplite_store_get(node->store, cid, &n);
        if (!text) { hpplite_error(node->ctx, "range %llu-%llu: the store has no manifest %s: %s", (unsigned long long)r.fromHeight, (unsigned long long)r.toHeight, cid, hpplite_store_error(node->store)); hpplite_l1_range_clear(&r); return NULL; }
        hpplite_store_sha256(text, n, digest);
        if (memcmp(digest, r.manifestDigest, 32) != 0) { hpplite_error(node->ctx, "range %llu-%llu: the store's manifest is not the one the chain anchored", (unsigned long long)r.fromHeight, (unsigned long long)r.toHeight); free(text); hpplite_l1_range_clear(&r); return NULL; }
        m = cJSON_ParseWithLength((const char *)text, n);
        free(text);
        arr = m ? cJSON_GetObjectItemCaseSensitive(m, "batches") : NULL;
        cb = m ? cJSON_GetObjectItemCaseSensitive(m, "chain_before") : NULL;
        if (!arr || !cJSON_IsArray(arr) || !cJSON_IsString(cb) || hpplite_hex_to_bytes(cb->valuestring + 2, chain, 32) != 32) {
            hpplite_error(node->ctx, "range %llu-%llu: the manifest is malformed", (unsigned long long)r.fromHeight, (unsigned long long)r.toHeight);
            cJSON_Delete(m); hpplite_l1_range_clear(&r); return NULL;
        }
        if (h == node->ctx->blockHeight && memcmp(chain, node->ctx->chainHash, 32) != 0) {
            unsigned char zero[32] = {0};
            if (memcmp(node->ctx->chainHash, zero, 32) != 0) {
                hpplite_error(node->ctx, "range %llu-%llu: the manifest starts from a chain hash that is not this history's at %llu", (unsigned long long)r.fromHeight, (unsigned long long)r.toHeight, (unsigned long long)(h - 1));
                cJSON_Delete(m); hpplite_l1_range_clear(&r); return NULL;
            }
        }
        cJSON_ArrayForEach(e, arr) {
            const cJSON *hh = cJSON_GetObjectItemCaseSensitive(e, "h"), *hs = cJSON_GetObjectItemCaseSensitive(e, "hash");
            if (!cJSON_IsNumber(hh) || (uint64_t)hh->valuedouble != h || !cJSON_IsString(hs) || hpplite_hex_to_bytes(hs->valuestring + 2, hash, 32) != 32) {
                hpplite_error(node->ctx, "range %llu-%llu: the manifest does not name batch %llu in order", (unsigned long long)r.fromHeight, (unsigned long long)r.toHeight, (unsigned long long)h);
                cJSON_Delete(m); hpplite_l1_range_clear(&r); return NULL;
            }
            hpplite_chain_hash_next(chain, h, hash, got);
            memcpy(chain, got, 32);
            h++;
        }
        if (h != r.toHeight + 1 || memcmp(chain, r.chainHashAfter, 32) != 0) {
            hex32(r.chainHashAfter, hx);
            hpplite_error(node->ctx, "range %llu-%llu: the manifest's batches do not fold to the anchored chain hash %s", (unsigned long long)r.fromHeight, (unsigned long long)r.toHeight, hx);
            cJSON_Delete(m); hpplite_l1_range_clear(&r); return NULL;
        }
        if (node->manifest) cJSON_Delete(node->manifest);
        node->manifest = m;
        node->manifestFrom = r.fromHeight;
        node->manifestTo = r.toHeight;
    }
    node->anchoredHeight = r.toHeight > node->anchoredHeight ? r.toHeight : node->anchoredHeight;
    b = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(node->manifest, "batches"), (int)(height - r.fromHeight));
    e = b ? cJSON_GetObjectItemCaseSensitive(b, "ref") : NULL;
    zRef = cJSON_IsString(e) ? e->valuestring : NULL;
    e = b ? cJSON_GetObjectItemCaseSensitive(b, "hash") : NULL;
    if (!zRef || !cJSON_IsString(e) || hpplite_hex_to_bytes(e->valuestring + 2, want, 32) != 32) { hpplite_l1_range_clear(&r); return NULL; }
    data = hpplite_store_get(node->store, zRef, pLen);
    if (!data) {
        hpplite_error(node->ctx, "batch %llu: the store has no %s: %s", (unsigned long long)height, zRef, hpplite_store_error(node->store));
    } else {
        hpplite_keccak256(data, *pLen, got);
        if (memcmp(got, want, 32) != 0) {
            hpplite_error(node->ctx, "batch %llu: the store's bytes for %s are not what the manifest names", (unsigned long long)height, zRef);
            free(data); data = NULL; *pLen = 0;
        }
    }
    hpplite_l1_range_clear(&r);
    return data;
}

static uint64_t node_flush_pages(HppliteNode *node, int *pMore, size_t maxBytes) {
    HppliteCtx *ctx = node->ctx;
    unsigned char *bytes = NULL, root[32];
    size_t len = 0, budget, overhead = node->cipher ? node_envelope_overhead(node) : HPPLITE_PLAIN_MAGIC_LEN;
    if (maxBytes > 0) maxBytes = maxBytes > overhead ? maxBytes - overhead : 1;      /* what the envelope adds is posted too (#150) */
    budget = maxBytes * (node->pageDeflateRatio ? node->pageDeflateRatio : HPPLITE_PAGE_DEFLATE_GUESS);
    uint64_t height = ctx->blockHeight;
    const uint8_t *payload = NULL; size_t payloadLen = 0;
    uint8_t *sealed = NULL, *packed = NULL, *framed = NULL;
    uint8_t encoding = HPPLITE_ENCODING_NONE;
    int rc, confirmed = 0, tries;
    char zStoreWhy[240];
    int storedOnly = 0;
    zStoreWhy[0] = 0;
    node_wal_maintenance(node);                         /* frames another connection or process committed since (#153); the pins moved */
    node_pages_decide_snapshot(node);                   /* if nothing decided it yet */
    if (!node->l1 && node->wantsL1) {
        /* l1=/contract=/factory= was given but no connection came of it (no key,
        ** bad RPC): finalizing locally would move the height past batches the
        ** chain never saw; everything stays pending, as the row flush keeps it (#156) */
        if (!node->warnedNoL1) {
            hpplite_error(ctx, "L1 was configured but is not connected; batches are kept pending (check the key and the RPC endpoint)");
            node->warnedNoL1 = 1;
        }
        *pMore = 0; return 0;
    }
    if (node->l1) {
        if (!node->cipher && !node->config.noEncrypt) {
            if (!node->warnedNoKey) { hpplite_error(ctx, "no private key: batches are kept pending and not posted (set privkey=/keyfile=/HPPLITE_PRIVATE_KEY, or encrypt=off to post plaintext)"); node->warnedNoKey = 1; }
            *pMore = 0; return 0;
        }
        if (!node_ensure_lease(node)) { *pMore = 0; return 0; }          /* only the lease holder posts */
        if (node_check_rollup(node) < 0) { *pMore = 0; return 0; }
    }
    hpplite_pagemode_set_name(node->pm, height == 1 ? ctx->zDbName : NULL);     /* batch 1 names the database (#160) */
    for (tries = 0; ; tries++) {
        rc = hpplite_pagemode_build(node->pm, budget, &bytes, &len, root, pMore);
        if (rc != SQLITE_OK) { if (rc != SQLITE_DONE) hpplite_error(ctx, "page batch %llu could not be built (rc=%d)", (unsigned long long)height, rc); return 0; }
        payload = bytes; payloadLen = len; encoding = HPPLITE_ENCODING_NONE; packed = NULL;
        if (!node->config.noCompress) {
            size_t n = 0;
            uint8_t *c = hpplite_deflate_level(payload, payloadLen, 1, &n);      /* diffs are mostly zeros: level 1 is nearly as small and several times faster */
            if (c && n + HPPLITE_PLAIN_MAGIC_LEN < payloadLen) { packed = c; payload = c; payloadLen = n; encoding = HPPLITE_ENCODING_DEFLATE; } else free(c);
        }
        if (payloadLen > 0) { unsigned r = (unsigned)(len / payloadLen); node->pageDeflateRatio = r < 1 ? 1 : r > 64 ? 64 : r; }   /* what these pages deflate to: the next budget */
        /* over the posted limit: the guess was optimistic for these pages; half the
        ** raw budget and build again, down to a single commit (which posts whatever its size) */
        /* only a chain taking the bytes itself has a hard limit; a store takes any size */
        if (maxBytes > 0 && payloadLen > maxBytes && node->l1 && !node->store && budget > (size_t)hpplite_pagemode_page_size(node->pm) + 96 && tries < 12) {
            hpplite_pagemode_failed(node->pm);
            sqlite3_free(bytes); free(packed); bytes = NULL; packed = NULL;
            budget /= 2;
            continue;
        }
        break;
    }
    if (node->cipher && node->nReaders > 0 && node->config.hasPrivkey) {
        /* readers besides the wallet (#15): sealed to every one of them and to
        ** the wallet, under the series' data key, as the row flush does (#150) */
        uint8_t *aTo = malloc((size_t)(node->nReaders + 1) * 33);
        if (aTo) {
            memcpy(aTo, node->keypair.pubkey, 33);
            memcpy(aTo + 33, node->aReaders, (size_t)node->nReaders * 33);
            if (!node->seriesDekValid) { hpplite_random_bytes(node->seriesDek, 32); node->seriesDekValid = 1; }
            sealed = hpplite_envelope_seal_pk(node->cipher, node->seriesDek, aTo, node->nReaders + 1, height, encoding, payload, payloadLen, &payloadLen);
            free(aTo);
            if (!*pMore) node->seriesDekValid = 0;       /* the series ends with this batch: a new key next */
        }
        payload = sealed;
    } else if (node->cipher) {
        sealed = hpplite_envelope_seal(node->cipher, node->encKey, height, encoding, payload, payloadLen, &payloadLen);
        payload = sealed;
    } else if (encoding == HPPLITE_ENCODING_DEFLATE) {
        framed = malloc(HPPLITE_PLAIN_MAGIC_LEN + payloadLen);
        if (framed) { memcpy(framed, HPPLITE_PLAIN_MAGIC, HPPLITE_PLAIN_MAGIC_LEN); memcpy(framed + HPPLITE_PLAIN_MAGIC_LEN, payload, payloadLen); payload = framed; payloadLen += HPPLITE_PLAIN_MAGIC_LEN; }
    }
    if (!payload) { hpplite_pagemode_failed(node->pm); sqlite3_free(bytes); free(packed); return 0; }
    if (node->l1) {
        char *txHash;
        /* the sidecar knows the batch before the chain does: a process that dies
        ** between the submit and the finalize finds it its own on reopen */
        if (hppliteMarkInflightPages(ctx, height, root) != SQLITE_OK) { hpplite_pagemode_failed(node->pm); sqlite3_free(bytes); free(packed); free(sealed); free(framed); *pMore = 0; return 0; }
        if (node->store) {
            /* the bytes to the store, the commitment to the chain (#11), as the row flush does */
            char *zReceipt = NULL;
            char *zRef = hpplite_store_put(node->store, payload, payloadLen, &zReceipt);
            node->storeBalanceOk = 0;
            if (!zRef) {
                if (node->config.storeFallback && hpplite_store_last_failure(node->store) == HPPLITE_STORE_FAIL_UNREACHABLE
                    && !(node_ranges(node) && node_unanchored_above(node, node->anchoredHeight))) {      /* not past batches waiting for their anchor (#164) */
                    txHash = hpplite_l1_submit_batch(node->l1, node->instanceId, height, payload, payloadLen);
                    if (!node->warnedStoreDown) {
                        fprintf(stderr, "hpplite: store %s does not answer (%s): page batch %llu %s on the chain instead\n",
                                hpplite_store_url(node->store), hpplite_store_error(node->store), (unsigned long long)height, txHash ? "posted" : "could not be posted");
                        node->warnedStoreDown = 1;
                    }
                } else {
                    snprintf(zStoreWhy, sizeof zStoreWhy, "the store did not take it: %s", hpplite_store_error(node->store));
                    txHash = NULL;
                }
            } else {
                unsigned char h[32], payTo[20];
                uint8_t compact[HPPLITE_RECEIPT_SIZE];
                int nCompact = (zReceipt && hpplite_store_domain(node->store, payTo, NULL) == 0) ? hpplite_store_receipt_compact(zReceipt, payTo, compact) : -1;
                node->warnedStoreDown = 0;
                hpplite_keccak256(payload, payloadLen, h);
                if (node_ranges(node)) {
                    node_anchor_row(node, h, zRef, nCompact > 0 ? compact : NULL, nCompact > 0 ? (size_t)nCompact : 0);   /* the chain's commitment comes with the anchor (#164) */
                    txHash = NULL;
                    storedOnly = 1;
                } else {
                    txHash = hpplite_l1_commit_batch(node->l1, node->instanceId, height, h, zRef, nCompact > 0 ? compact : NULL, nCompact > 0 ? (size_t)nCompact : 0);
                    if (!txHash) hpplite_error(ctx, "the chain did not take the commitment of page batch %llu (ref %s)", (unsigned long long)height, zRef);
                }
            }
            sqlite3_free(zRef);
            sqlite3_free(zReceipt);
        } else {
            txHash = hpplite_l1_submit_batch(node->l1, node->instanceId, height, payload, payloadLen);
        }
        if (txHash) { confirmed = (hpplite_l1_wait_for_tx(node->l1, txHash, 30) == 1); if (confirmed) snprintf(node->lastBatchTx, sizeof(node->lastBatchTx), "%s", txHash); if (confirmed && node->onBatchPosted) node->onBatchPosted(node->callbackArg, height, txHash); sqlite3_free(txHash); }
        if (storedOnly) confirmed = 1;
        if (confirmed) test_crash_point("after_submit");
    } else {
        confirmed = 1;                          /* l1=local: finalized here, kept nowhere */
    }
    if (!confirmed) {
        /* the transaction may have landed despite the lost answer (#49): the
        ** commits back in the queue, then what the chain holds at our height,
        ** which the adoption rebuilds from them and compares */
        int adopted;
        hpplite_pagemode_failed(node->pm);
        adopted = node_adopt_existing(node);
        if (adopted == 1) { sqlite3_free(bytes); free(packed); free(sealed); free(framed); return height; }
        if (node->failStreak < 6) node->failStreak++;
        node->retryAt = node_current_time_ms() + (node->config.batchIntervalMs > 0 ? node->config.batchIntervalMs : 5000);
        if (zStoreWhy[0]) hpplite_error(ctx, "page batch %llu kept pending: %s", (unsigned long long)height, zStoreWhy);
        else hpplite_error(ctx, "page batch %llu not confirmed on L1, kept pending", (unsigned long long)height);
        sqlite3_free(bytes); free(packed); free(sealed); free(framed);
        *pMore = 0;
        return 0;
    }
    ctx->pAnchorRow = storedOnly ? &node->anchorRow : NULL;
    rc = hppliteAdvanceMetaPages(ctx, height, node->l1 ? payload : NULL, node->l1 ? payloadLen : 0, root, hpplite_pagemode_piece_more(node->pm));
    ctx->pAnchorRow = NULL;
    if (rc == SQLITE_OK) {
        hpplite_pagemode_finalized(node->pm);
        node->batchesProduced++;
        node->batchesSinceCheckpoint++;
        node->failStreak = 0;
        node->retryAt = 0;
        hpplite_error_clear(ctx);
        test_crash_point(*pMore ? "mid_series" : "after_finalize");
    } else {
        /* on the chain, not finalized here: the next flush adopts it (#90) */
        hpplite_pagemode_failed(node->pm);
        node->retryAt = 0;
        hpplite_error(ctx, "page batch %llu could not be finalized (rc=%d); adopted at the next flush", (unsigned long long)height, rc);
        height = 0;
        *pMore = 0;
    }
    sqlite3_free(bytes); free(packed); free(sealed); free(framed);
    return height;
}

static uint64_t node_flush_one(HppliteNode *node, int *pMore) {
    HppliteCtx *ctx = node->ctx;
    int frc;
    HppliteBatch *batch;
    sqlite3_int64 maxId = 0;
    uint64_t height;
    int confirmed = 0;
    char zStoreWhy[240];               /* why the store did not take the batch (#11): the reason the error keeps */
    int storedOnly = 0;                /* the store has it and the chain's commitment comes with the anchor (#164) */

    *pMore = 0;
    zStoreWhy[0] = 0;
    /* The root is O(database size): compute it only on the batch that
    ** closes a checkpoint interval (the checkpoint is submitted from it) */
    int withRoot = node->batchesSinceCheckpoint + 1 >= node->config.checkpointInterval;
    /* The DA limit applies to the posted bytes: leave room for the envelope */
    size_t maxBytes = node->config.maxBatchBytes;
    if (node->cipher && maxBytes > 0) {
        size_t overhead;
        if (!node->readersLoaded) node_load_readers(node);         /* the readers decide the envelope, the envelope the budget (#15) */
        overhead = node_envelope_overhead(node);
        maxBytes = maxBytes > overhead ? maxBytes - overhead : 1;
    }
    if (!node->l1 && node->wantsL1) {
        /* l1=/contract=/factory= was given but no connection came of it
        ** (no key, bad RPC, ...): finalizing locally would advance the
        ** height past batches L1 never saw. Keep everything pending. */
        if (!node->warnedNoL1) {
            hpplite_error(node->ctx, "L1 was configured but is not connected; batches are kept pending "
                            "(check the key and the RPC endpoint)");
            node->warnedNoL1 = 1;
        }
        return 0;
    }

    if (!node->l1 && !node->config.localOnly) {
        /* No chain at all. Finalizing here would delete the log - the one
        ** thing this is for - with nothing anywhere else. Keep it until a
        ** chain is configured; l1=local is the explicit throw-away mode (#38). */
        if (!node->warnedNoChain) {
            fprintf(stderr, "hpplite: no chain configured: the log is kept pending (%lld rows) until l1= names one; "
                            "l1=local finalizes batches locally and keeps no history\n",
                    (long long)hpplite_pending_count(ctx));
            node->warnedNoChain = 1;
        }
        *pMore = 0;
        return 0;
    }

    if (node->pm) return node_flush_pages(node, pMore, maxBytes);

    batch = hpplite_build_batch_limited(ctx, maxBytes, withRoot, &maxId);
    if (!batch) return 0;
    height = batch->height;
    *pMore = batch->partial;

    if (node->l1) {
        /* No key means no encryption key: never post plaintext by accident.
        ** encrypt=off is the only way to post in the clear, and it is explicit. */
        if (!node->cipher && !node->config.noEncrypt) {
            if (!node->warnedNoKey) {
                hpplite_error(node->ctx, "no private key: batches are kept pending and not posted "
                                "(set privkey=/keyfile=/HPPLITE_PRIVATE_KEY, or encrypt=off to post plaintext)");
                node->warnedNoKey = 1;
            }
            hpplite_batch_free(batch);
            *pMore = 0;
            return 0;
        }

        /* Only the lease holder may post; never submit into someone else's lease */
        if (!node_ensure_lease(node)) {
            hpplite_batch_free(batch);
            *pMore = 0;
            return 0;
        }
        /* the rollup may only now be known (created at the first post) */
        if (node_check_rollup(node) < 0) {
            hpplite_batch_free(batch);
            *pMore = 0;
            return 0;
        }

        char *json = hpplite_batch_to_json(batch);
        uint8_t *postedCopy = NULL;
        size_t postedLen = 0;
        if (json) {
            const uint8_t *payload = (const uint8_t *)json;
            size_t payloadLen = strlen(json);
            uint8_t *sealed = NULL, *packed = NULL;
            uint8_t encoding = HPPLITE_ENCODING_NONE;
            if (!node->config.noCompress) {
                /* deflate the JSON before it is sealed (#12): ciphertext does not
                ** compress. Only when it shrinks; a tiny batch stays as it was. */
                size_t n = 0;
                uint8_t *c = hpplite_deflate(payload, payloadLen, &n);
                if (c && n + HPPLITE_PLAIN_MAGIC_LEN < payloadLen) {
                    packed = c;
                    payload = c;
                    payloadLen = n;
                    encoding = HPPLITE_ENCODING_DEFLATE;
                } else {
                    free(c);
                }
            }
            if (node->cipher && node->nReaders > 0 && node->config.hasPrivkey) {
                /* readers besides the wallet (#15): sealed to every one of them and to
                ** the wallet, under the series' data key */
                uint8_t *aTo = malloc((size_t)(node->nReaders + 1) * 33);
                if (aTo) {
                    memcpy(aTo, node->keypair.pubkey, 33);
                    memcpy(aTo + 33, node->aReaders, (size_t)node->nReaders * 33);
                    if (!node->seriesDekValid) { hpplite_random_bytes(node->seriesDek, 32); node->seriesDekValid = 1; }
                    sealed = hpplite_envelope_seal_pk(node->cipher, node->seriesDek, aTo, node->nReaders + 1, height,
                                                      encoding, payload, payloadLen, &payloadLen);
                    free(aTo);
                    if (!batch->partial) node->seriesDekValid = 0;       /* the series ends with this batch: a new key next */
                }
                payload = sealed;
            } else if (node->cipher) {
                sealed = hpplite_envelope_seal(node->cipher, node->encKey, height,
                                               encoding, payload, payloadLen, &payloadLen);
                payload = sealed;
            } else if (encoding == HPPLITE_ENCODING_DEFLATE) {
                /* plaintext: the HPZ1 frame says what follows */
                uint8_t *framed = malloc(HPPLITE_PLAIN_MAGIC_LEN + payloadLen);
                if (framed) {
                    memcpy(framed, HPPLITE_PLAIN_MAGIC, HPPLITE_PLAIN_MAGIC_LEN);
                    memcpy(framed + HPPLITE_PLAIN_MAGIC_LEN, payload, payloadLen);
                    free(packed);
                    packed = framed;
                    payload = framed;
                    payloadLen += HPPLITE_PLAIN_MAGIC_LEN;
                }
            }
            if (payload) {
                char *txHash;
                if (node->store) {
                    /* the bytes to the store, the commitment to the chain (#11) */
                    char *zReceipt = NULL;
                    char *zRef = hpplite_store_put(node->store, payload, payloadLen, &zReceipt);
                    node->storeBalanceOk = 0;                 /* a put may have paid: the status row reads the balance again */
                    if (!zRef) {
                        if (node->config.storeFallback && hpplite_store_last_failure(node->store) == HPPLITE_STORE_FAIL_UNREACHABLE
                            && !(node_ranges(node) && node_unanchored_above(node, node->anchoredHeight))) {
                            /* the store does not answer: the chain carries the bytes meanwhile, as
                            ** before the store (#11); availability never waits on a provider.
                            ** Not past batches waiting for their anchor (#164): the chain takes
                            ** only the next height, and the anchor needs the store */
                            txHash = hpplite_l1_submit_batch(node->l1, node->instanceId, height, payload, payloadLen);
                            if (!node->warnedStoreDown) {
                                fprintf(stderr, "hpplite: store %s does not answer (%s): batch %llu %s on the chain instead\n",
                                        hpplite_store_url(node->store), hpplite_store_error(node->store), (unsigned long long)height,
                                        txHash ? "posted" : "could not be posted");
                                node->warnedStoreDown = 1;
                            }
                        } else {
                            snprintf(zStoreWhy, sizeof zStoreWhy, "the store did not take it: %s", hpplite_store_error(node->store));
                            txHash = NULL;
                        }
                    } else {
                        unsigned char h[32], payTo[20];
                        uint8_t compact[HPPLITE_RECEIPT_SIZE];
                        int nCompact = (zReceipt && hpplite_store_domain(node->store, payTo, NULL) == 0)
                                       ? hpplite_store_receipt_compact(zReceipt, payTo, compact) : -1;
                        node->warnedStoreDown = 0;
                        hpplite_keccak256(payload, payloadLen, h);
                        if (node_ranges(node)) {
                            /* the store has it: the chain gets one commitment per anchor (#164) */
                            node_anchor_row(node, h, zRef, nCompact > 0 ? compact : NULL, nCompact > 0 ? (size_t)nCompact : 0);
                            txHash = NULL;
                            storedOnly = 1;
                        } else {
                            txHash = hpplite_l1_commit_batch(node->l1, node->instanceId, height, h, zRef,
                                                             nCompact > 0 ? compact : NULL, nCompact > 0 ? (size_t)nCompact : 0);
                            if (!txHash) hpplite_error(node->ctx, "the chain did not take the commitment of batch %llu (ref %s)",
                                                       (unsigned long long)height, zRef);
                        }
                    }
                    sqlite3_free(zRef);
                    sqlite3_free(zReceipt);
                } else {
                    txHash = hpplite_l1_submit_batch(node->l1, node->instanceId, height, payload, payloadLen);
                }
                if (!txHash && hpplite_l1_submit_was_oversized(node->l1) && payloadLen > 1024) {
                    /* The RPC takes less per transaction than the contract per
                    ** batch (an unknown chain's limit): post smaller batches,
                    ** right away - nothing was sent */
                    size_t smaller = payloadLen / 4 * 3;
                    if (smaller < node->config.maxBatchBytes) node->config.maxBatchBytes = smaller;
                    node_set_stmt_limit(node);
                    fprintf(stderr, "hpplite: the RPC refused a %zu-byte batch as oversized; posting batches of "
                            "at most %zu bytes (set maxtx= to the chain's limit)\n",
                            payloadLen, node->config.maxBatchBytes);
                    free(sealed);
                    sqlite3_free(json);
                    hpplite_batch_free(batch);
                    *pMore = 1;
                    return 0;
                }
                if (txHash) {
                    confirmed = (hpplite_l1_wait_for_tx(node->l1, txHash, 30) == 1);
                    if (confirmed) snprintf(node->lastBatchTx, sizeof(node->lastBatchTx), "%s", txHash);
                    if (confirmed && (postedCopy = malloc(payloadLen)) != NULL) {
                        memcpy(postedCopy, payload, payloadLen);   /* kept with the batch (#49) */
                        postedLen = payloadLen;
                    }
                    if (confirmed && node->onBatchPosted) {
                        node->onBatchPosted(node->callbackArg, height, txHash);
                    }
                    sqlite3_free(txHash);
                }
                if (storedOnly) {
                    confirmed = 1;
                    if ((postedCopy = malloc(payloadLen)) != NULL) { memcpy(postedCopy, payload, payloadLen); postedLen = payloadLen; }
                    if (node->onBatchPosted) node->onBatchPosted(node->callbackArg, height, "stored");
                }
            }
            free(sealed);
            free(packed);
            sqlite3_free(json);
        }
        batch->pPosted = postedCopy;
        batch->nPosted = postedLen;
        node->postedCopy = postedCopy;     /* freed after finalize */
        if (confirmed) test_crash_point("after_submit");
        if (!confirmed) {
            /* The transaction may have landed despite the error */
            int adopted = node_adopt_existing(node);
            confirmed = adopted == 1;
            if (adopted == 2) {
                node->retryAt = 0;         /* on L1, finalized at the next flush: not a failure (#90) */
                *pMore = 0;
            } else if (!confirmed) {
                int64_t wait = (int64_t)node->config.batchIntervalMs;
                if (wait <= 0) wait = 5000;
                if (node->failStreak < 6) node->failStreak++;
                wait <<= node->failStreak;                 /* 2x per failure */
                if (wait > 300000) wait = 300000;          /* ... up to the lease length */
                node->retryAt = node_current_time_ms() + wait;
                if (zStoreWhy[0]) {
                    hpplite_error(node->ctx, "batch %llu kept pending: %s (retry in %llds)",
                            (unsigned long long)height, zStoreWhy, (long long)(wait / 1000));
                } else {
                    hpplite_error(node->ctx, "batch %llu not confirmed on L1, kept pending (retry in %llds)",
                            (unsigned long long)height, (long long)(wait / 1000));
                }
                *pMore = 0;
            }
            hpplite_batch_free(batch);
            return confirmed ? height : 0;
        }
    }

    ctx->pAnchorRow = storedOnly ? &node->anchorRow : NULL;
    frc = hpplite_finalize_batch(ctx, batch, maxId);
    ctx->pAnchorRow = NULL;
    if (frc != SQLITE_OK) {
        /* Typically: an application statement is mid-step, so bookkeeping
        ** must not commit now (see hppliteBkBegin). The batch is on L1 (when there
        ** is one) and the next flush adopts it; locally nothing changed. */
        node->retryAt = 0;        /* not an L1 failure: try again next tick */
        if (ctx->nAppStmtActive == 0 && !ctx->bAppTxnOpen && ctx->txnGate != 1) {
            /* not a deferral for an application statement or transaction
            ** (the gate): something is actually wrong */
            hpplite_error(node->ctx, "could not finalize batch %llu locally",
                    (unsigned long long)height);
        }
        height = 0;
        *pMore = 0;
    } else {
        node->batchesProduced++;
        node->batchesSinceCheckpoint++;
        node->failStreak = 0;
        node->retryAt = 0;
        hpplite_error_clear(ctx);          /* posted and finalized: whatever was wrong is not now (#32) */
        test_crash_point(*pMore ? "mid_series" : "after_finalize");
    }

    hpplite_batch_free(batch);
    free(node->postedCopy);
    node->postedCopy = NULL;
    return height;
}

static uint64_t node_flush_unlocked(HppliteNode *node) {
    uint64_t height = 0, h;
    int more;

    if (!node->ctx) return 0;
    /* a node the open hook made and no timer started (interval=0: the
    ** application flushes) starts at its first flush (#118) */
    if (node->state == HPPLITE_STATE_INIT) hpplite_node_start(node);
    if (node->config.verifyOnly) return 0;      /* a verifier rebuilds and compares; it posts nothing (#5) */
    /* the sequencer connection posts - unless it is gone: then this one does */
    if (node->follower && !node_try_promote(node)) return 0;
    if (node->l1 && node_check_rollup(node) < 0) return 0;
    node->lastFlushTime = node_current_time_ms();

    /* Finish anything a previous attempt left on L1. With pending SQL, a
    ** batch at our height must be ours (adopt) or the chain has forked
    ** (refuse). With nothing pending it is simply history we have not seen
    ** yet - another process of this wallet posted while we were idle (a
    ** rebuild running beside the live sequencer, a resumed machine): replay
    ** it, as open does. */
    if (node->l1) {
        int rc;
        if (hpplite_node_pending_count(node) == 0) {
            if (hpplite_node_sync_from_l1(node, 1) < 0) {
                node->state = HPPLITE_STATE_ERROR;
                return 0;
            }
        }
        while ((rc = node_adopt_existing(node)) == 1) { }
        if (rc < 0) {
            node->state = HPPLITE_STATE_ERROR;
            return 0;
        }
        if (rc == 2) return 0;           /* ours is on L1, waiting for the gate: nothing to post (#90) */
    }
    hpplite_rows_seq_flush(node->ctx);   /* busy now: the next flush */

    /* One batch per call normally; several when the size limit splits them */
    do {
        h = node_flush_one(node, &more);
        if (h == 0) break;
        height = h;
    } while (more);

    /* Anchor the state root on L1 once an interval of batches is complete */
    if (height && node->l1 && !node_ranges(node) && hpplite_node_should_checkpoint(node)) {
        char *txHash = hpplite_node_submit_checkpoint(node);
        if (txHash) {
            sqlite3_free(txHash);
        } else {
            hpplite_error(node->ctx, "checkpoint after batch %llu not submitted; will retry",
                    (unsigned long long)height);
        }
    }
    return height;
}

/*
** Flush pending SQL to create a batch and post to L1.
*/
uint64_t hpplite_node_flush_batch(HppliteNode *node) {
    pthread_mutex_t *mutex;
    uint64_t height;

    if (!node) return 0;
    mutex = (pthread_mutex_t *)node->flushMutex;
    if (mutex) pthread_mutex_lock(mutex);
    /* An explicit flush reads the lease afresh; only the timer uses the cache */
    node->leaseRecheckAt = 0;
    node->retryAt = 0;
    height = node_flush_unlocked(node);
    if (mutex) pthread_mutex_unlock(mutex);
    return height;
}

/*
** Bring the local database up to date with L1.
*/
int hpplite_node_sync_from_l1(HppliteNode *node, int useTxn) {
    uint64_t lastBatch = 0, totalBatches = 0;
    unsigned char latestHash[32];
    int synced = 0;

    if (!node || !node->l1 || !node->ctx) return 0;
    if (node_check_rollup(node) < 0) return -1;
    if (hpplite_l1_get_da_state(node->l1, &lastBatch, &totalBatches, latestHash) != 0) return 0;
    node->anchoredHeight = lastBatch;

    while (node->ctx->blockHeight <= lastBatch) {
        HppliteBatch *batch = node_fetch_batch(node, node->ctx->blockHeight);
        int rc;
        if (!batch) {
            if (node->lastFetchForeign && node_skip_foreign_height(node, node->ctx->blockHeight) == 0) {
                synced++;
                continue;
            }
            if (node->ctx->zLastError[0]) {   /* the fetch said what is wrong with the batch; keep that */
                fprintf(stderr, "hpplite: sync stopped at height %llu of %llu on L1\n",
                        (unsigned long long)node->ctx->blockHeight, (unsigned long long)lastBatch);
            } else {
                hpplite_error(node->ctx, "sync stopped at height %llu of %llu on L1",
                        (unsigned long long)node->ctx->blockHeight, (unsigned long long)lastBatch);
            }
            node->state = HPPLITE_STATE_ERROR;
            return synced ? synced : -1;
        }
        if (!batch->bPages && node->pm && node->config.captureAuto && batch->height == 1 && hpplite_pagemode_captured(node->pm) == 0) {
            /* capture=auto on a file that started in page mode, and the rollup's
            ** history is statements: the node switches to rows here, before
            ** anything of its own was captured, and goes on replaying */
            if (node_switch_to_rows(node) != SQLITE_OK) { hpplite_batch_free(batch); node->state = HPPLITE_STATE_ERROR; return -1; }
        }
        if (batch->bPages) {
            /* a page batch: applied to the file (a replica), or adopted (the sequencer's own) */
            /* a sequencer (its file is where the pages come from) adopts; a verifier applies */
            rc = (node->pm && !node->config.verifyOnly && !node->follower && node->lockFd >= 0) ? node_adopt_pages(node, batch) : node_apply_pages(node, batch);
            if (rc != SQLITE_OK) { hpplite_batch_free(batch); node->state = HPPLITE_STATE_ERROR; return -1; }
            node->lastCheckpointHeight = node->ctx->lastCheckpointHeight;
            hpplite_batch_free(batch);
            synced++;
            continue;
        }
        if (hpplite_node_pending_count(node) > 0) {
            /* Local writes exist: the L1 batch must be ours */
            rc = hpplite_adopt_batch(node->ctx, batch);
            if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
                hpplite_batch_free(batch);   /* matched; finalized by a later flush */
                return synced;
            }
            if (rc != SQLITE_OK) {
                hpplite_error(node->ctx, "local pending SQL diverges from L1 batch %llu",
                        (unsigned long long)batch->height);
                hpplite_batch_free(batch);
                return -1;
            }
        } else {
            rc = hpplite_replay_batch(node->ctx, batch, useTxn);
            if (rc != SQLITE_OK && rc != SQLITE_DONE) {
                hpplite_batch_free(batch);
                return -1;
            }
        }
        node->lastCheckpointHeight = node->ctx->lastCheckpointHeight;
        hpplite_batch_free(batch);
        synced++;
    }
    /* Caught up: what we rebuilt must be what the contract committed to.
    ** The DA contract keeps a rolling hash over every batch (#33); ours
    ** advanced with every batch finalized, replayed or skipped. A file from
    ** before the hash was kept has none (zero) and is not compared. */
    if (lastBatch > 0 && node->ctx->blockHeight == lastBatch + 1) {
        static const unsigned char zero[32];
        unsigned char chain[32];
        uint64_t covered = 0;
        if (memcmp(node->ctx->chainHash, zero, 32) != 0
            && hpplite_l1_get_chain_hash(node->l1, &covered, chain) == 0 && covered == lastBatch
            && memcmp(chain, node->ctx->chainHash, 32) != 0) {
            hpplite_error(node->ctx, "chain hash mismatch after batch %llu: the batches read are not the ones the contract committed to",
                          (unsigned long long)lastBatch);
            node->state = HPPLITE_STATE_ERROR;
            return -1;
        }
    }
    if (synced) hpplite_error_clear(node->ctx);   /* up to date with L1 (#32) */
    node_pages_decide_snapshot(node);               /* the chain seen: is the file ahead of it? */
    if (node->pagesWalSuspended && !node->pieceDb) {
        /* the pages applied: back to WAL for this node's own writes, the file as
        ** it is now the base the page mode diffs against (a commit's pieces still
        ** to come keep the lock, and the mode, until the last) */
        hppliteInternalExec(node->ctx, node->db, "PRAGMA journal_mode=WAL");
        hpplite_pagemode_resume(node->pm);
        node->pagesWalSuspended = 0;
    }
    return synced;
}

/* The WAL's checkpoint runs on the timer thread (#144). SQLite's automatic
** one runs on whichever connection's commit finds a thousand frames in the
** WAL, with an fsync of the database: on the application's thread, once
** per thousand pages, and the bookkeeping traffic (the log rows, their
** pruning, the posted copies) fills the WAL several times faster than the
** application's own writes. It was the writer's p99. Here it is passive:
** it never waits for the application and the application never waits for
** it. The bookkeeping connection's wal hook says how big the WAL is after
** each batch; between batches the timer looks once a second. */
static int node_wal_hook(void *pArg, sqlite3 *db, const char *zDb, int nFrames) {
    HppliteNode *node = (HppliteNode *)pArg;
    (void)db; (void)zDb;
    node->walPages = nFrames;
    return SQLITE_OK;
}

/* The connection the timer checkpoints the file's WAL on: the bookkeeping
** one, a second connection to the same file; in page mode that one is the
** sidecar, so a plain connection of its own is opened (#151). None while the
** file is being applied to in rollback-journal mode: the switch out of WAL
** needs the file to itself. */
static void node_ck_open(HppliteNode *node) {
    const char *zFile;
    if (node->ckDb || !node->pm || node->config.verifyOnly) return;
    zFile = sqlite3_db_filename(node->db, "main");
    if (!zFile || !zFile[0]) return;
    hpplite_internal_open_begin();
    if (sqlite3_open_v2(zFile, &node->ckDb, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) { if (node->ckDb) sqlite3_close(node->ckDb); node->ckDb = NULL; }
    hpplite_internal_open_end();
    if (node->ckDb) {
        sqlite3_busy_timeout(node->ckDb, 5000);
        /* a connection learns the file is in WAL mode on its first read; until
        ** then a checkpoint on it does nothing (and says -1 frames) */
        sqlite3_exec(node->ckDb, "PRAGMA journal_mode", NULL, NULL, NULL);
    }
}
static void node_ck_close(HppliteNode *node) {
    if (!node->ckDb) return;
    sqlite3_wal_hook(node->ckDb, NULL, NULL);
    sqlite3_close(node->ckDb);
    node->ckDb = NULL;
}
static sqlite3 *node_ck_conn(HppliteNode *node) { return node->pm ? node->ckDb : node->bkDb; }

/* The pins (#155): a read transaction held on one of two plain connections,
** moved forward right after each poll: SQLite backfills only up to the
** oldest reader's mark and restarts the WAL only when all of it is
** backfilled, so a frame the reader has not read is never restarted over.
** The new pin is taken before the poll and the old one let go after it. */
static sqlite3 *node_pin_open(HppliteNode *node, int i) {
    const char *zFile;
    if (node->pinDb[i]) return node->pinDb[i];
    zFile = sqlite3_db_filename(node->db, "main");
    if (!zFile || !zFile[0]) return NULL;
    hpplite_internal_open_begin();
    if (sqlite3_open_v2(zFile, &node->pinDb[i], SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) { if (node->pinDb[i]) sqlite3_close(node->pinDb[i]); node->pinDb[i] = NULL; }
    hpplite_internal_open_end();
    if (node->pinDb[i]) sqlite3_busy_timeout(node->pinDb[i], 5000);
    return node->pinDb[i];
}
static void node_pins_close(HppliteNode *node) {
    int i;
    for (i = 0; i < 2; i++) if (node->pinDb[i]) { sqlite3_exec(node->pinDb[i], "COMMIT", NULL, NULL, NULL); sqlite3_close(node->pinDb[i]); node->pinDb[i] = NULL; }
}
/* the WAL's upkeep for a page-mode sequencer: the new pin, the poll (frames
** another connection or process committed, #153), the old pin let go, the
** checkpoint when due. From the timer each tick, and from a commit hook at
** 1000 frames when no timer runs */
static void node_wal_maintenance(void *pArg) {
    HppliteNode *node = pArg;
    sqlite3 *fresh;
    if (!node || !node->pm || node->config.verifyOnly || !node->ctx || !node->ctx->zWalKey || !node->walMaintMutex) return;
    if (pthread_mutex_trylock((pthread_mutex_t *)node->walMaintMutex) != 0) return;   /* one at a time; the other's poll covers this */
    fresh = node_pin_open(node, !node->pinActive);
    if (fresh) sqlite3_exec(fresh, "BEGIN; SELECT count(*) FROM sqlite_schema", NULL, NULL, NULL);   /* its snapshot: the WAL as it is now */
    hpplite_walread_poll(node->ctx->zWalKey, 0);
    if (node->pinDb[node->pinActive]) sqlite3_exec(node->pinDb[node->pinActive], "COMMIT", NULL, NULL, NULL);
    if (fresh) node->pinActive = !node->pinActive;
    node_ck_open(node);
    node_checkpoint_wal(node, node_current_time_ms());
    pthread_mutex_unlock((pthread_mutex_t *)node->walMaintMutex);
}

static void node_checkpoint_wal(HppliteNode *node, int64_t now) {
    int nLog = 0, nCkpt = 0;
    sqlite3 *ck = node_ck_conn(node);
    if (!ck) return;
    if (node->walPages < 1000 && now < node->walCheckAt) return;
    node->walCheckAt = now + 1000;
    if (sqlite3_wal_checkpoint_v2(ck, "main", SQLITE_CHECKPOINT_PASSIVE, &nLog, &nCkpt) == SQLITE_OK) {
        node->walPages = nCkpt >= nLog ? 0 : nLog;       /* all of it in the database: the next writer restarts the WAL */
    }
}

/*
** Timer thread function - auto-flush batches on interval.
*/
static void *node_timer_thread_func(void *arg) {
    HppliteNode *node = (HppliteNode *)arg;

    if (!node) return NULL;

    int intervalMs = node->config.batchIntervalMs;
    if (intervalMs <= 0) return NULL;

    /* Wake every 100 ms at most; a stop wakes it at once (a close must not
    ** wait out the sleep: with one connection per request - PHP - that was
    ** most of what HPPLite added to a request) */
    int sleepMs = (intervalMs < 100) ? intervalMs : 100;
    pthread_cond_t *wake = (pthread_cond_t *)node->timerWake;
    pthread_mutex_t *wakeMutex = (pthread_mutex_t *)node->timerWakeMutex;

    while (node->timerRunning) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += (long)sleepMs * 1000000L;
        until.tv_sec += until.tv_nsec / 1000000000L;
        until.tv_nsec %= 1000000000L;
        pthread_mutex_lock(wakeMutex);
        if (node->timerRunning) pthread_cond_timedwait(wake, wakeMutex, &until);
        pthread_mutex_unlock(wakeMutex);

        if (!node->timerRunning) break;

        /* A node the open hook made starts here, on the first tick, not in
        ** the hook: opening a connection must not wait on the chain (the
        ** lease claim). hpplite_open() starts its node itself. Until then the
        ** state said "init" for the connection's whole life (#118) */
        if (node->state == HPPLITE_STATE_INIT) {
            pthread_mutex_t *mutex = (pthread_mutex_t *)node->flushMutex;
            if (mutex) {
                pthread_mutex_lock(mutex);
                if (node->timerRunning) hpplite_node_start(node);
                pthread_mutex_unlock(mutex);
            }
        }

        /* Flush on the interval, or early once pending SQL nears the DA limit */
        int64_t now = node_current_time_ms();
        int64_t elapsed = now - node->lastFlushTime;
        int sizeTrigger = node->config.maxBatchBytes > 0 &&
            (node->pm ? hpplite_pagemode_pending_bytes(node->pm) >= node->config.maxBatchBytes * HPPLITE_PAGE_DEFLATE_GUESS / 2
                      : hpplite_pending_bytes(node->ctx) >= (sqlite3_int64)(node->config.maxBatchBytes / 2));
        if (node->pm) node_wal_maintenance(node); else node_checkpoint_wal(node, now);

        if (node->follower) {
            /* capture only; every second, see whether the sequencer is gone */
            if (now < node->promoteAt) continue;
            node->promoteAt = now + 1000;
            pthread_mutex_t *mutex = (pthread_mutex_t *)node->flushMutex;
            pthread_mutex_lock(mutex);
            if (node->timerRunning) node_try_promote(node);
            pthread_mutex_unlock(mutex);
            continue;
        }
        if (now < node->retryAt) continue;           /* backing off after L1 failures */
        if (elapsed >= intervalMs || sizeTrigger) {
            pthread_mutex_t *mutex = (pthread_mutex_t *)node->flushMutex;
            if (mutex) {
                pthread_mutex_lock(mutex);
                if (node->timerRunning) {
                    node_flush_unlocked(node);
                }
                pthread_mutex_unlock(mutex);
            }
        }
        /* the anchor, on the chain's cadence (#164); a refused one is tried again within a minute */
        if (node->config.anchorIntervalMs > 0 && node->store && node->l1 && now >= node->anchorAt) {
            pthread_mutex_t *mutex = (pthread_mutex_t *)node->flushMutex;
            if (node->anchorAt == 0) {
                /* decided at the first tick: what a previous run left at the store is anchored soon, the rest on the cadence */
                node->anchorAt = now + (node_unanchored_above(node, node->anchoredHeight) ? 2000 : node->config.anchorIntervalMs);
            } else if (mutex) {
                int rc = 0;
                pthread_mutex_lock(mutex);
                if (node->timerRunning && node_ranges(node)) rc = node_anchor(node);
                pthread_mutex_unlock(mutex);
                node->anchorAt = now + (rc < 0 && node->config.anchorIntervalMs > 60000 ? 60000 : node->config.anchorIntervalMs);
            }
        }
    }

    return NULL;
}

/*
** Start auto-flush timer thread.
*/
void hpplite_node_start_timer(HppliteNode *node) {
    if (!node) return;
    if (node->config.batchIntervalMs <= 0) return;
    if (node->timerRunning) return;
    if (!node->bkDb) {
        /* In-memory databases have no bookkeeping connection: a timer would
        ** run BEGIN/COMMIT on the application's connection from another
        ** thread, inside its transactions. Flush on close or hpplite_flush(). */
        return;
    }
    if (!node->flushMutex) return;

    /* Start thread */
    pthread_t *thread = sqlite3_malloc(sizeof(pthread_t));
    if (!thread) return;
    if (!node->timerWake) {
        pthread_cond_t *c = sqlite3_malloc(sizeof(pthread_cond_t));
        pthread_mutex_t *m = sqlite3_malloc(sizeof(pthread_mutex_t));
        if (!c || !m) { sqlite3_free(c); sqlite3_free(m); sqlite3_free(thread); return; }
        pthread_cond_init(c, NULL);
        pthread_mutex_init(m, NULL);
        node->timerWake = c;
        node->timerWakeMutex = m;
    }

    node->timerRunning = 1;
    pthread_create(thread, NULL, node_timer_thread_func, node);
    node->timerThread = thread;
    /* the WAL's checkpoint is this thread's now (#144); the application's
    ** commits stop doing it. interval=0 (no timer) leaves SQLite's own. */
    if (!node->pm) sqlite3_wal_hook(node->bkDb, node_wal_hook, node);
    /* page mode: the connection's wal hook is the capture's (#155) and already
    ** stands in for SQLite's checkpoint; the pragma would replace it */
    if (node->ctx && !node->pm) hppliteInternalExec(node->ctx, node->db, "PRAGMA wal_autocheckpoint=0");
}

/*
** Stop auto-flush timer thread.
*/
void hpplite_node_stop_timer(HppliteNode *node) {
    if (!node || !node->timerRunning) return;

    /* Signal thread to stop, and wake it */
    pthread_mutex_lock((pthread_mutex_t *)node->timerWakeMutex);
    node->timerRunning = 0;
    if (!node->pm && node->bkDb) sqlite3_wal_hook(node->bkDb, NULL, NULL);
    if (node->ctx && node->db && !node->pm) hppliteInternalExec(node->ctx, node->db, "PRAGMA wal_autocheckpoint=1000");   /* SQLite's own again */
    pthread_cond_signal((pthread_cond_t *)node->timerWake);
    pthread_mutex_unlock((pthread_mutex_t *)node->timerWakeMutex);

    /* Wait for thread to finish */
    pthread_t *thread = (pthread_t *)node->timerThread;
    if (thread) {
        pthread_join(*thread, NULL);
        sqlite3_free(thread);
        node->timerThread = NULL;
    }

}

/*
** ============================================================================
** Lease
** ============================================================================
*/

/*
** Claim sequencer lease on L1.
** Returns 0 on success, -1 if lease unavailable.
*/
/* Does the chain say the lease is held by this instance right now? */
static int node_lease_is_ours(HppliteNode *node) {
    HppliteL1State *st = hpplite_l1_get_state(node->l1);
    int ours;
    if (!st) return -1;
    {
        uint64_t now = st->currentTimestamp ? st->currentTimestamp : (uint64_t)time(NULL);
        ours = st->leaseExpiry > now && memcmp(st->sequencerInstance, node->instanceId, 32) == 0;
    }
    hpplite_l1_state_free(st);
    return ours;
}

int hpplite_node_claim_lease(HppliteNode *node) {
    if (!node || !node->l1) return -1;

    /* A claim is a transaction: it can be mined and still have reverted
    ** (another instance holds the lease). Believing the hash alone made a
    ** second instance of the same wallet think it held the lease and then
    ** expire the live sequencer's on close (issue #40). Wait for it and
    ** ask the chain who holds the lease. */
    char *txHash = hpplite_l1_claim_sequencer(node->l1, node->instanceId);
    if (!txHash) {
        return -1;  /* Lease not available */
    }
    hpplite_l1_wait_for_tx(node->l1, txHash, 30);
    sqlite3_free(txHash);
    if (node_lease_is_ours(node) != 1) {
        node->hasLease = 0;
        return -1;
    }
    node->hasLease = 1;
    node->leaseRecheckAt = 0;
    return 0;
}

/*
** Release our sequencer lease on L1.
*/
int hpplite_node_release_lease(HppliteNode *node) {
    if (!node || !node->l1 || !node->hasLease) return -1;

    /* Never expire a lease that is not ours: forceExpireLease is owner-only
    ** and the owner wallet may be running the real sequencer elsewhere */
    {
        int ours = node_lease_is_ours(node);
        if (ours != 1) {
            fprintf(stderr, "hpplite: not releasing the lease: %s\n",
                    ours < 0 ? "cannot read the lease from L1" : "it is held by another instance");
            node->hasLease = 0;
            return -1;
        }
    }
    char *txHash = hpplite_l1_release_lease(node->l1, node->instanceId);
    if (!txHash) {
        fprintf(stderr, "hpplite: lease release transaction could not be sent\n");
        return -1;
    }
    /* Wait so the next instance (possibly this process restarting) can claim */
    if (hpplite_l1_wait_for_tx(node->l1, txHash, 30) != 1) {
        fprintf(stderr, "hpplite: lease release %s not confirmed\n", txHash);
    }
    sqlite3_free(txHash);
    node->hasLease = 0;
    node->leaseRecheckAt = 0;
    return 0;
}

/*
** Renew sequencer lease on L1.
*/
int hpplite_node_renew_lease(HppliteNode *node) {
    if (!node || !node->l1) return -1;

    /* Renew lease with our instance ID */
    char *txHash = hpplite_l1_renew_lease(node->l1, node->instanceId);
    if (!txHash) {
        return -1;  /* Renewal failed */
    }
    sqlite3_free(txHash);
    return 0;
}

/*
** Check if our lease is still active.
*/
int hpplite_node_has_lease(HppliteNode *node) {
    if (!node || !node->l1) return 0;

    /* Check if any lease is active on L1 */
    return hpplite_l1_is_lease_active(node->l1);
}

/*
** Start the node.
*/
int hpplite_node_start(HppliteNode *node) {
    if (!node) return -1;
    if (node->state == HPPLITE_STATE_RUNNING) return 0;
    if (node->state == HPPLITE_STATE_ERROR) return -1;    /* sync refused the L1 history: stay in error */

    /* Take the lease if it is free or ours; a foreign holder (another
    ** instance of this wallet, or a stranger) is left alone - we capture
    ** and keep pending until it expires. Starting is not a failure then. */
    if (node->l1 && !node->follower && !node->config.verifyOnly) {
        node->leaseRecheckAt = 0;
        node_ensure_lease(node);
    }

    node->state = HPPLITE_STATE_RUNNING;
    return 0;
}

/*
** Stop the node.
*/
void hpplite_node_stop(HppliteNode *node) {
    if (!node) return;
    if (node->state != HPPLITE_STATE_RUNNING) return;

    node->state = HPPLITE_STATE_STOPPED;
}

/*
** Execute SQL (queued for next batch).
*/
int hpplite_node_exec(HppliteNode *node, const char *sql, char **errMsg) {
    if (!node || !sql) return -1;
    return hpplite_exec(node->ctx, sql, errMsg);
}

/*
** Get number of pending SQL statements.
*/
int hpplite_node_pending_count(HppliteNode *node) {
    if (!node || !node->ctx) return 0;
    if (node->pm) {
        /* what another connection or process committed since the last look
        ** is pending too (#153): a look at the WAL first, cheap when nothing
        ** was added */
        if (node->ctx->zWalKey && !node->config.verifyOnly) hpplite_walread_poll(node->ctx->zWalKey, 0);
        return (int)hpplite_pagemode_pending(node->pm);
    }
    return hpplite_pending_count(node->ctx);
}

/*
** ============================================================================
** Checkpoints
** ============================================================================
*/

/*
** Check if node should create a checkpoint.
*/
int hpplite_node_should_checkpoint(HppliteNode *node) {
    if (!node) return 0;
    if (node->pm && hpplite_pagemode_piece_open(node->pm)) return 0;   /* a commit's pieces still to post: its intermediate roots are no checkpoint (#152) */
    return (node->batchesSinceCheckpoint >= node->config.checkpointInterval);
}

/*
** Set the L1 connection for this node.
*/
void hpplite_node_set_l1(HppliteNode *node, HppliteL1 *l1) {
    if (!node) return;
    node->l1 = l1;
    node->ownsL1 = 0;  /* Externally provided - we don't own it */

    /* Pass our private key, or our signer, to L1 for signing transactions */
    if (node->config.hasPrivkey) {
        hpplite_l1_set_privkey(l1, node->config.privkey);
    } else if (node->hasSigner) {
        hpplite_l1_set_signer(l1, node->signer.sign, node->signer.arg, node->signer.address);
    }
    node_learn_batch_limit(node);
}

int hpplite_node_set_view_key(HppliteNode *node, const unsigned char privkey[32]) {
    uint8_t pub[33];
    if (!node || !privkey || hpplite_pk_pubkey(privkey, pub) != 0) return -1;
    memcpy(node->viewKey, privkey, 32);
    node->hasViewKey = 1;
    return 0;
}

void hpplite_node_readers_changed(HppliteNode *node) {
    if (node) node->readersLoaded = 0;
}

/* The readers granted, from _hpplite_readers (#15); the wallet itself comes first */
static void node_load_readers(HppliteNode *node) {
    sqlite3 *db = node->ctx ? (node->ctx->bkDb ? node->ctx->bkDb : node->db) : NULL;
    sqlite3_stmt *q = NULL;
    int saved;
    if (!db) return;
    free(node->aReaders);
    node->aReaders = NULL;
    node->nReaders = 0;
    saved = hppliteBkEnter(node->ctx);
    if (sqlite3_prepare_v2(db, "SELECT pubkey FROM _hpplite_readers ORDER BY pubkey", -1, &q, NULL) == SQLITE_OK) {
        while (sqlite3_step(q) == SQLITE_ROW) {
            if (sqlite3_column_bytes(q, 0) != 33 || node->nReaders >= HPPLITE_PK_MAX_RECIPIENTS - 1) continue;
            uint8_t *grown = realloc(node->aReaders, (size_t)(node->nReaders + 1) * 33);
            if (!grown) break;
            node->aReaders = grown;
            memcpy(node->aReaders + 33 * node->nReaders, sqlite3_column_blob(q, 0), 33);
            node->nReaders++;
        }
    }
    sqlite3_finalize(q);
    hppliteBkLeave(node->ctx, saved);
    node->readersLoaded = 1;
    node_set_stmt_limit(node);                     /* the envelope grew or shrank with them */
}

int hpplite_node_set_batch_key(HppliteNode *node, const unsigned char key[32]) {
    if (!node || !key) return -1;
    if (node->flushMutex) pthread_mutex_lock((pthread_mutex_t *)node->flushMutex);
    memcpy(node->encKey, key, HPPLITE_CIPHER_KEY_SIZE);
    node->hasEncKey = 1;
    if (!node->config.noEncrypt) node->cipher = hpplite_cipher_default();
    node->warnedNoKey = 0;
    if (node->l1 && node->hasSigner && !node->follower && !node->config.verifyOnly) { node->leaseRecheckAt = 0; node_ensure_lease(node); }
    if (node->flushMutex) pthread_mutex_unlock((pthread_mutex_t *)node->flushMutex);
    return 0;
}

int hpplite_node_set_signer(HppliteNode *node, const HppliteSigner *signer) {
    int rc = 0;
    if (!node || !signer || !signer->sign) return -1;
    if (node->config.hasPrivkey || node->hasSigner) return -2;
    if (node->flushMutex) pthread_mutex_lock((pthread_mutex_t *)node->flushMutex);
    node->signer = *signer;
    node->hasSigner = 1;
    memcpy(node->keypair.address, signer->address, 20);
    if (node->ctx) hpplite_error_clear(node->ctx);
    if (node->l1) {
        hpplite_l1_set_signer(node->l1, node->signer.sign, node->signer.arg, node->signer.address);
        if (!node->follower && !node->config.verifyOnly && (node->cipher || node->config.noEncrypt)) { node->leaseRecheckAt = 0; node_ensure_lease(node); }
    } else if (!node->follower) {
        node_auto_connect_l1(node, &node->config);
        if (node->wantsL1 && !node->l1) rc = -1;
    }
    if (node->flushMutex) pthread_mutex_unlock((pthread_mutex_t *)node->flushMutex);
    return rc;
}

int hpplite_node_set_key(HppliteNode *node, const unsigned char privkey[32]) {
    HppliteKeypair kp;
    int rc = 0;
    if (!node || !privkey || !node->crypto) return -1;
    if (node->hasSigner) return -2;
    if (node->config.hasPrivkey) return memcmp(node->config.privkey, privkey, 32) == 0 ? 0 : -2;
    if (hpplite_crypto_keypair_from_privkey(node->crypto, &kp, privkey) != 0) return -1;
    if (node->flushMutex) pthread_mutex_lock((pthread_mutex_t *)node->flushMutex);   /* not under a timer flush */
    node->keypair = kp;
    memcpy(node->config.privkey, privkey, 32);
    node->config.hasPrivkey = 1;
    hpplite_cipher_derive_key(privkey, node->encKey);          /* the batch key: whoever can write the rollup can read it */
    node->hasEncKey = 1;
    if (!node->config.noEncrypt) node->cipher = hpplite_cipher_default();
    if (node->store) hpplite_store_set_key(node->store, privkey);
    node->warnedNoKey = 0;
    if (node->ctx) hpplite_error_clear(node->ctx);
    if (node->l1) {
        hpplite_l1_set_privkey(node->l1, privkey);
        if (!node->follower && !node->config.verifyOnly) { node->leaseRecheckAt = 0; node_ensure_lease(node); }
    } else if (!node->follower) {
        node_auto_connect_l1(node, &node->config);              /* named at open, unreachable without a key: now */
        if (node->wantsL1 && !node->l1) rc = -1;
    }
    if (node->flushMutex) pthread_mutex_unlock((pthread_mutex_t *)node->flushMutex);
    return rc;
}

/*
** Create and submit checkpoint to L1.
*/
char *hpplite_node_submit_checkpoint(HppliteNode *node) {
    if (!node || !node->l1) return NULL;
    if (node->batchesSinceCheckpoint == 0) return NULL;

    if (!node_ensure_lease(node)) return NULL;

    /* Current state: root after the last confirmed batch */
    uint64_t currentHeight = hpplite_get_block_height(node->ctx) - 1;

    /* Create checkpoint */
    HppliteCheckpoint *cp = hpplite_checkpoint_new(
        node->lastCheckpointHeight + 1,
        currentHeight
    );
    if (!cp) return NULL;

    memcpy(cp->preStateRoot, node->lastCheckpointRoot, HPPLITE_HASH_SIZE);
    memcpy(cp->postStateRoot, node->ctx->stateRoot, HPPLITE_HASH_SIZE);
    cp->timestamp = (uint64_t)time(NULL);

    /* Submit to L1 with our instance ID */
    char *txHash = hpplite_l1_submit_checkpoint(node->l1, cp, node->instanceId);

    if (txHash) {
        /* Update checkpoint tracking (persisted in _hpplite_meta) */
        hpplite_set_checkpoint(node->ctx, cp->toHeight, cp->postStateRoot);
        node->lastCheckpointHeight = cp->toHeight;
        memcpy(node->lastCheckpointRoot, cp->postStateRoot, HPPLITE_HASH_SIZE);
        node->batchesSinceCheckpoint = 0;

        /* Notify callback */
        if (node->onCheckpointSubmitted) {
            node->onCheckpointSubmitted(node->callbackArg, cp->fromHeight, cp->toHeight, txHash);
        }
    }

    hpplite_checkpoint_free(cp);
    return txHash;
}

/*
** ============================================================================
** Accessors
** ============================================================================
*/

/*
** Get current block height.
*/
uint64_t hpplite_node_get_height(HppliteNode *node) {
    if (!node || !node->ctx) return 0;
    if (node->follower) hpplite_refresh_meta(node->ctx);   /* the sequencer moved the file (#100) */
    return hpplite_get_block_height(node->ctx);
}

/*
** Get current state root.
*/
void hpplite_node_get_state_root(HppliteNode *node, unsigned char *out) {
    if (!node || !node->ctx || !out) return;
    if (node->pm) { hpplite_pagemode_root(node->pm, out); return; }
    hpplite_get_state_root(node->ctx, out);
}

/*
** Get node's public key.
*/
void hpplite_node_get_pubkey(HppliteNode *node, unsigned char *out) {
    if (!node || !out) return;
    memcpy(out, node->keypair.pubkey, HPPLITE_PUBKEY_SIZE);
}

/*
** Get node's Ethereum address.
*/
void hpplite_node_get_address(HppliteNode *node, unsigned char *out) {
    if (!node || !out) return;
    memcpy(out, node->keypair.address, HPPLITE_ADDRESS_SIZE);
}

/*
** Get node's instance ID.
*/
void hpplite_node_get_instance_id(HppliteNode *node, unsigned char *out) {
    if (!node || !out) return;
    memcpy(out, node->instanceId, 32);
}

/*
** Get node statistics as JSON string.
*/
char *hpplite_node_get_stats(HppliteNode *node) {
    char pubkeyHex[HPPLITE_PUBKEY_SIZE * 2 + 1];
    char addressHex[HPPLITE_ADDRESS_SIZE * 2 + 1];
    char stateRootHex[HPPLITE_HASH_SIZE * 2 + 1];
    char instanceIdHex[32 * 2 + 1];
    unsigned char stateRoot[HPPLITE_HASH_SIZE];
    const char *stateStr;

    if (!node) return NULL;

    hpplite_bytes_to_hex(node->keypair.pubkey, HPPLITE_PUBKEY_SIZE, pubkeyHex);
    hpplite_bytes_to_hex(node->keypair.address, HPPLITE_ADDRESS_SIZE, addressHex);
    hpplite_bytes_to_hex(node->instanceId, 32, instanceIdHex);

    hpplite_node_get_state_root(node, stateRoot);
    hpplite_bytes_to_hex(stateRoot, HPPLITE_HASH_SIZE, stateRootHex);

    switch (node->state) {
        case HPPLITE_STATE_INIT: stateStr = "init"; break;
        case HPPLITE_STATE_RUNNING: stateStr = "running"; break;
        case HPPLITE_STATE_SYNCING: stateStr = "syncing"; break;
        case HPPLITE_STATE_STOPPED: stateStr = "stopped"; break;
        case HPPLITE_STATE_ERROR: stateStr = "error"; break;
        default: stateStr = "unknown"; break;
    }

    return sqlite3_mprintf(
        "{"
        "\"nodeId\":\"%s\","
        "\"state\":\"%s\","
        "\"instanceId\":\"0x%s\","
        "\"pubkey\":\"0x%s\","
        "\"address\":\"0x%s\","
        "\"height\":%llu,"
        "\"stateRoot\":\"0x%s\","
        "\"pending\":%d,"
        "\"logErrors\":%llu,"
        "\"batchesProduced\":%llu,"
        "\"batchesSinceCheckpoint\":%d,"
        "\"lastCheckpointHeight\":%llu,"
        "\"encryption\":\"%s\""
        "}",
        node->config.nodeId ? node->config.nodeId : "",
        stateStr,
        instanceIdHex,
        pubkeyHex,
        addressHex,
        (unsigned long long)hpplite_node_get_height(node),
        stateRootHex,
        hpplite_node_pending_count(node),
        (unsigned long long)(node->ctx ? node->ctx->nLogErrors : 0),
        (unsigned long long)node->batchesProduced,
        node->batchesSinceCheckpoint,
        (unsigned long long)node->lastCheckpointHeight,
        node->cipher ? node->cipher->name : "none"
    );
}
