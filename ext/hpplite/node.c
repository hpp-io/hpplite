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
static void node_checkpoint_wal(HppliteNode *node, int64_t now);
static void node_ck_close(HppliteNode *node);
static void node_pins_close(HppliteNode *node);
static void node_wal_maintenance(void *pArg);
static int node_ranges(HppliteNode *node);
static int node_anchor(HppliteNode *node);
static uint64_t node_unanchored_above(HppliteNode *node, uint64_t lastBatch);
static uint8_t *node_fetch_from_range(HppliteNode *node, HppliteL1 *l1, uint64_t height, size_t *pLen);
static int node_check_rollup(HppliteNode *node);
static char *node_snapshot_name(HppliteNode *node, HppliteL1 *l1);
static uint8_t *node_store_get_checked(HppliteNode *node, const char *zRef, const unsigned char want[32], size_t *pLen);
static struct cJSON *node_snapshot_index(HppliteNode *node, const char *zIndex, const unsigned char sha[32]);
#include "da_uri.h"
#include "cJSON.h"
static void node_cap_batch_limit(HppliteNode *node);
static uint8_t *node_fetch_bytes(HppliteNode *node, HppliteL1 *l1, uint64_t height, size_t *pLen);
static int node_check_rollup(HppliteNode *node);
#include <sys/stat.h>
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
#include "eth/eth_client.h"
#include "spend.h"

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
    memcpy(node->config.ownerPub, config->ownerPub, 33);
    node->config.hasOwnerPub = config->hasOwnerPub;
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
    node->config.storePermit = config->storePermit;
    memcpy(node->config.funder, config->funder, 20);
    node->config.hasFunder = config->hasFunder;
    node->syncTo = config->syncTo;
    node->syncAtMs = config->syncAtMs;
    node->config.spendCap = config->spendCap;
    node->config.spendWindow = config->spendWindow;
    node->config.lowBalanceHours = config->lowBalanceHours;
    node->config.storeFallback = config->storeFallback;
    node->config.anchorIntervalMs = config->anchorIntervalMs;
    node->config.keepLease = config->keepLease;
    node->config.noAnchorAtClose = config->noAnchorAtClose;
    node->config.snapshotIntervalMs = config->snapshotIntervalMs;
    if (config->hasFleet) {
        /* fleet= (#316): one database in a shared rollup - the fleet's anchor anchors, there is no release */
        node->config.hasFleet = 1;
        memcpy(node->config.fleet, config->fleet, 20);
        memcpy(node->config.dbId, config->dbId, 32);
        memcpy(node->config.fleetOwner, config->fleetOwner, 20);
        memcpy(node->config.nameHash, config->nameHash, 32);
        node->config.admitIssuedAt = config->admitIssuedAt;
        memcpy(node->config.admitSig, config->admitSig, 65);
        node->config.hasAdmit = config->hasAdmit;
        node->config.onForeign = config->onForeign;
        node->config.anchorIntervalMs = 0;
        /* a snapshot every HPPLITE_FLEET_SNAPSHOT_BATCHES batches, or after snapshot= (default 24 h) with any batch since;
        ** snapshot=0: none (#314) */
        node->config.snapshotIntervalMs = config->snapshotGiven ? config->snapshotIntervalMs : 24 * 3600 * 1000;
        node->config.keepLease = 1;
        node->config.noAnchorAtClose = 1;
    }
    node->config.fromSnapshot = config->fromSnapshot;
    node->config.keepSeconds = config->keepSeconds;
    node->config.keepAll = config->keepAll;
    node->config.relayUrl = node_strdup(config->relayUrl);
    node->config.relayCap = config->relayCap;
    if (node->config.relayUrl) {
        /* the relay (#126): an x402-paid service like the store, its /commit takes the signed forms */
        if (strncmp(node->config.relayUrl, "http://", 7) != 0 && strncmp(node->config.relayUrl, "https://", 8) != 0) node->relay = NULL;
        else node->relay = hpplite_store_open(node->config.relayUrl, config->hasPrivkey ? config->privkey : NULL, config->relayCap);
        if (node->relay) hpplite_store_set_relay(node->relay);
        if (!node->relay) {
            fprintf(stderr, "hpplite: relay=%s: not a relay URL (http://host:port of operators/relay or a service like it)\n", node->config.relayUrl);
            return -1;
        }
    }
    if (node->config.storeUrl) {
        /* the batch bytes' store (#11): the chain will hold commitments only */
        node->store = hpplite_store_open(node->config.storeUrl, config->hasPrivkey ? config->privkey : NULL, config->storeCap);
        if (node->store && config->hasFunder) hpplite_store_set_funder(node->store, config->funder);   /* an external account (#272) */
        if (!node->store) {
            fprintf(stderr, "hpplite: store=%s: not a store HPPLite knows: store=on for the network's canonical store, or http(s)://host[:port] of an hpplite-store/2 store (operators/store/hpplite_store.py in front of your own disk; kubo:// and s3:// were removed in v0.8, #250)\n", node->config.storeUrl);
            return -1;
        }
    }
    if (node->store || node->relay) {
        /* what they are paid, over time (#173): a cap over a window, the rate a warning reads */
        /* the wallet's money: one ledger per wallet in the data directory, whatever rollups it has there;
        ** a node opened without its key (hpplite_key later, #17) keeps it in memory */
        char *zLedger = NULL;
        unsigned char me[20];
        if (node->config.dataDir && config->hasPrivkey && eth_address_from_privkey(config->privkey, me) == 0) {
            char hex[41];
            hpplite_bytes_to_hex(me, 20, hex);
            zLedger = sqlite3_mprintf("%s/spend-%s.json", node->config.dataDir, hex);
        }
        node->spend = hpplite_spend_open(zLedger, node->config.spendCap, node->config.spendWindow);
        sqlite3_free(zLedger);
        hpplite_store_set_spend(node->store, node->spend);
        hpplite_store_set_spend(node->relay, node->spend);
    }
    node_cap_batch_limit(node);
    node->config.noEncrypt = config->noEncrypt;
    node->config.noCompress = config->noCompress;
    node->config.localOnly = config->localOnly;
    node->config.verifyOnly = config->verifyOnly;

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
/* Who a batch is sealed to with the public-key envelope (#15): the wallet first, the owner when it is another
** key (ownerkey=, #209: a recovery key no node holds), then the readers granted (the owner once). The count, and
** the keys (malloc'd, 33 bytes each) when aTo is given; 1 means the wallet alone: the symmetric envelope */
static int node_recipients(HppliteNode *node, uint8_t **paTo) {
    int n = 1, owner = node->config.hasOwnerPub && memcmp(node->config.ownerPub, node->keypair.pubkey, 33) != 0, i;
    uint8_t *a;
    if (owner) n++;
    for (i = 0; i < node->nReaders; i++) if (!owner || memcmp(node->aReaders + 33 * i, node->config.ownerPub, 33) != 0) n++;
    if (!paTo) return n;
    *paTo = a = malloc((size_t)n * 33);
    if (!a) return 0;
    memcpy(a, node->keypair.pubkey, 33);
    n = 1;
    if (owner) memcpy(a + 33 * n++, node->config.ownerPub, 33);
    for (i = 0; i < node->nReaders; i++)
        if (!owner || memcmp(node->aReaders + 33 * i, node->config.ownerPub, 33) != 0) memcpy(a + 33 * n++, node->aReaders + 33 * i, 33);
    return n;
}

/* What the envelope adds: the symmetric one, or the public-key one when the batch has readers besides the wallet */
static size_t node_envelope_overhead(HppliteNode *node) {
    if (!node->cipher) return 0;
    if (node->config.hasPrivkey && node_recipients(node, NULL) > 1) return hpplite_envelope_pk_overhead(node->cipher, node_recipients(node, NULL));
    return hpplite_envelope_overhead(node->cipher);
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
    const char *zHost = getenv("HPPLITE_HOST_NAME");          /* a host name the platform keeps across machines (somaro): it wins */
    if (zHost && zHost[0]) snprintf((char *)buf + 32, 255, "%s", zHost);
    else if (gethostname((char *)buf + 32, 255) != 0) buf[32] = 0;
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
    if (node->ctx->blockHeight <= 1 || memcmp(have, node->ctx->stateRoot, 32) != 0) {
        /* somaro#93, case 1: a fleet database's file that is not what its bookkeeping last sent goes whole, said once */
        if (node->config.hasFleet && node->ctx->blockHeight > 1 && !node->fleetForeignFile)
            fprintf(stderr, "hpplite: %s is not the state of height %llu it last sent: the file goes whole as height %llu\n",
                    sqlite3_db_filename(node->db, "main"), (unsigned long long)(node->ctx->blockHeight - 1), (unsigned long long)node->ctx->blockHeight);
        hpplite_pagemode_snapshot(node->pm);
    }
    node->pagesSnapshotDecided = 1;
}

/* The file against what this sequencer last sent, while it runs (after #327): the check its start makes, so a
** divergence the WAL never showed (frames missed, a write past SQLite) goes whole as the next height without waiting
** for a restart, weeks away for an always-on application. From the timer every HPPLITE_FILE_CHECK_SECONDS (60), and
** sound only when the node's state is the file's: nothing pending, its own passive checkpoint put the whole WAL in
** the file, and no commit arrived while the file was read (the WAL polled again after). A file whose stat is the one
** last found matching is not read again: an idle database costs a stat */
static void node_stat_key(const char *zFile, uint64_t key[4], int *pOk) {
    struct stat st;
    *pOk = stat(zFile, &st) == 0;
    if (!*pOk) return;
    key[0] = (uint64_t)st.st_dev; key[1] = (uint64_t)st.st_ino; key[2] = (uint64_t)st.st_size;
#ifdef __APPLE__
    key[3] = (uint64_t)st.st_mtimespec.tv_sec * 1000000000ull + (uint64_t)st.st_mtimespec.tv_nsec;
#else
    key[3] = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
#endif
}
static void node_check_file(HppliteNode *node) {
    const char *zFile;
    uint64_t key[4];
    unsigned char have[32], file[32];
    HpplitePageRoot *pr;
    int nLog = -1, nCkpt = -1, ok, rc;
    if (!node->pm || node->follower || node->config.verifyOnly || !node->hasLease || !node->pagesSnapshotDecided || !node->ckDb
        || node->state != HPPLITE_STATE_RUNNING || !node->ctx || !node->ctx->zWalKey || node->ctx->openTxnId) return;
    zFile = node->ctx->zWalKey;
    node_stat_key(zFile, key, &ok);
    if (!ok || (node->fileMatched && memcmp(key, node->fileKey, sizeof key) == 0)) return;
    hpplite_walread_poll(zFile, 0);
    if (hpplite_pagemode_pending(node->pm) || hpplite_pagemode_piece_open(node->pm)) return;     /* the next flush first */
    if (sqlite3_wal_checkpoint_v2(node->ckDb, "main", SQLITE_CHECKPOINT_PASSIVE, &nLog, &nCkpt) != SQLITE_OK || nLog < 0 || nCkpt != nLog) return;
    node_stat_key(zFile, key, &ok);
    if (!ok) return;
    pr = key[2] ? hpplite_pageroot_from_file(zFile, NULL) : NULL;
    if (key[2] && !pr) return;
    if (node->ctx->blockHeight > 1 && (!pr || hpplite_pageroot_pages(pr) <= 1)) {
        /* #330: emptied under the node (no table needs fewer than two pages) - never the database's newest state */
        hpplite_pageroot_free(pr);
        memcpy(node->fileKey, key, sizeof key);
        node->fileMatched = 1;                                   /* said once for this file; a file changed again is looked at again */
        hpplite_error(node->ctx, "%s is empty and its bookkeeping says the history has %llu heights: not sent; %s",
                      sqlite3_db_filename(node->db, "main"), (unsigned long long)(node->ctx->blockHeight - 1),
                      node->config.hasFleet ? "the next start rebuilds it from the store" : "restore the file");
        return;
    }
    hpplite_pageroot_get(pr, file);
    hpplite_pageroot_free(pr);
    hpplite_walread_poll(zFile, 0);
    if (hpplite_pagemode_pending(node->pm)) return;                                              /* a commit meanwhile: the next check */
    hpplite_pagemode_root(node->pm, have);
    if (memcmp(have, file, 32) == 0) { memcpy(node->fileKey, key, sizeof key); node->fileMatched = 1; return; }
    node->fileMatched = 0;
    fprintf(stderr, "hpplite: %s is not the state of height %llu it last sent: the file goes whole as height %llu\n",
            sqlite3_db_filename(node->db, "main"), (unsigned long long)(node->ctx->blockHeight - 1), (unsigned long long)node->ctx->blockHeight);
    if ((rc = hpplite_pagemode_resync(node->pm)) != SQLITE_OK)
        hpplite_error(node->ctx, "the file could not be read again to go whole (rc=%d): it goes whole at the next start", rc);
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

/* A file from v0.7 and earlier carries HPPLite's tables in the file itself: its history is statements or rows,
** which this release does not read (#253) */
static int node_file_has_log(sqlite3 *db) {
    return sqlite3_table_column_metadata(db, "main", "_hpplite_meta", "next_height", NULL, NULL, NULL, NULL, NULL) == SQLITE_OK;
}

/* somaro#93: the file holds a database - a schema, or more than the one page an empty file switched to WAL has; a
** fresh file (absent, or opened and empty) is not one */
static int node_file_has_pages(sqlite3 *db) {
    sqlite3_stmt *q = NULL;
    int has = 0;
    if (sqlite3_prepare_v2(db, "SELECT (SELECT count(*) FROM sqlite_master) > 0 OR (SELECT page_count FROM pragma_page_count()) > 1", -1, &q, NULL) == SQLITE_OK
        && sqlite3_step(q) == SQLITE_ROW) has = sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    return has;
}

/* Page capture is the one capture (#253): the pages each commit wrote, read from the WAL, the bookkeeping in a
** sidecar. It needs a file: an in-memory database has no WAL and no path */
static int node_init_tracking(HppliteNode *node) {
    const char *zFile = sqlite3_db_filename(node->db, "main");
    int rc;

    if (!zFile || !zFile[0]) {
        hpplite_set_init_error("an in-memory database has no WAL to capture: HPPLite needs a file (page capture, v0.8, #253)");
        fprintf(stderr, "hpplite: %s\n", hpplite_init_error());
        return SQLITE_MISUSE;
    }
    if (node_file_has_log(node->db)) {
        hpplite_set_init_error("this file's history is statements or rows (v0.7 and earlier), which this release does not read (#253): "
                               "v0.7 is the last release that does; a page-capture database starts from a fresh file");
        fprintf(stderr, "hpplite: %s\n", hpplite_init_error());
        return SQLITE_ERROR;
    }
    sqlite3_busy_timeout(node->db, 5000);
    if (node->config.hasFleet && node->store && !node->config.verifyOnly && !node->config.onForeign) {
        /* somaro#93, case 2 with no onforeign=: a file with pages and no bookkeeping beside it, against a history at the
        ** store - refused before the sidecar or the page capture touch anything (the switch to WAL alone rewrites the
        ** header), and before any claim */
        char *zSide = sqlite3_mprintf("%s-hpplite.db", zFile);
        struct stat sst;
        HppliteFleetView v;
        int pages = node_file_has_pages(node->db), side = zSide && stat(zSide, &sst) == 0;
        sqlite3_free(zSide);
        if (pages && !side && hpplite_store_fleet_view(node->store, node->config.fleet, hpplite_l1_default_chain_id(), node->config.dbId, &v) == 0
            && v.hasHead && v.toHeight > 0) {
            static char why[PATH_MAX + 300];                  /* the init error is a static string */
            snprintf(why, sizeof why, "%s has pages and no bookkeeping, and the fleet's history of this database is at height %llu: refused; "
                     "onforeign=history rebuilds the file from the history, onforeign=file sends the file as height %llu",
                     zFile, (unsigned long long)v.toHeight, (unsigned long long)v.toHeight + 1);
            hpplite_set_init_error(why);
            fprintf(stderr, "hpplite: %s\n", hpplite_init_error());
            return SQLITE_ERROR;
        }
    }
    rc = node_init_pages(node, zFile);
    if (rc != SQLITE_OK) return rc;

    node->lastCheckpointHeight = node->ctx->lastCheckpointHeight;
    memcpy(node->lastCheckpointRoot, node->ctx->lastCheckpointRoot, HPPLITE_HASH_SIZE);
    /* Batches confirmed since the last checkpoint are in _hpplite_meta
    ** (heights), so the cadence survives a restart: a process that opens,
    ** writes a few batches and exits still reaches a checkpoint eventually */
    if (node->ctx->blockHeight > node->lastCheckpointHeight + 1)
        node->batchesSinceCheckpoint = (int)(node->ctx->blockHeight - 1 - node->lastCheckpointHeight);
    node->lastFlushTime = node_current_time_ms();
    node_detect_follower(node);
    if (!node->follower) {
        rc = node_start_pages(node, zFile);
        if (rc != SQLITE_OK) return rc;
    }
    return SQLITE_OK;
}

static void node_learn_batch_limit(HppliteNode *node);

/*
** Auto-connect to L1 and sync state.
*/
static int node_ensure_lease(HppliteNode *node);
static uint64_t node_fleet_chain_id(HppliteNode *node);
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
    int nUnread = 0;                      /* ...and how many could not be read (#228) */
    int nNoAnswer = 0;                    /* ...of those, because the store did not answer (#229): no new rollup then */
    int nEmpty = 0;                       /* ...and how many have no batches yet (the wallet's unused primary): not databases (#241) */
    char *zTail = NULL;                   /* the message's end: what could not be read, and why */
    if (node->store) hpplite_store_take_status(node->store);
    node->ctx->zQuietError[0] = 0;
    for (i = 0; i < n; i++) {
        char hex[43];
        HppliteL1 *l1;
        snprintf(hex, sizeof(hex), "0x");
        for (int k = 0; k < 20; k++) snprintf(hex + 2 + 2 * k, 3, "%02x", all[i][k]);
        l1 = hpplite_l1_connect(config->rpcUrl, hex);
        if (l1) {
            size_t len = 0;
            uint8_t *data;
            HppliteBatch *b;
            char *zSnap;
            uint64_t last = 0, total = 0;
            unsigned char latest[32];
            if (hpplite_l1_get_da_state(l1, &last, &total, latest) == 0 && last == 0) {
                nEmpty++;                            /* nothing to read: no batch names it (#241) */
                hpplite_l1_disconnect(l1);
                continue;
            }
            node->ctx->quiet = 1;                    /* another rollup's bytes, or ours pruned: not this open's failure (#228) */
            data = node_fetch_bytes(node, l1, 1, &len);
            b = data ? node_decode_batch(node, 1, data, len) : NULL;
            zSnap = (!b && hpplite_l1_has_ranges(l1)) ? node_snapshot_name(node, l1) : NULL;   /* batch 1 pruned: the snapshot names it (#226) */
            node->ctx->quiet = 0;
            if (!b && !zSnap) {
                long st = node->store ? hpplite_store_take_status(node->store) : -1;
                nUnread++;
                if (st == 0 || st >= 500) nNoAnswer++;   /* no answer, or the store failing: its bytes may well be there */
            }
            const char *zHas = b && b->zDbName ? b->zDbName : zSnap;
            int match = zHas && strcmp(zHas, zName) == 0;
            if (zHas && !match) zOthers = sqlite3_mprintf("%z%s\"%s\"", zOthers, zOthers ? ", " : "", zHas);
            sqlite3_free(zSnap);
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
    zTail = sqlite3_mprintf("%s%s", nUnread ? "; some could not be read: " : "",
                            nUnread ? (node->ctx->zQuietError[0] ? node->ctx->zQuietError : "(no reason recorded)") : "");
    if (nEmpty) zTail = sqlite3_mprintf("%z; %d rollup%s with no batches", zTail, nEmpty, nEmpty == 1 ? "" : "s");
    /* The factory makes a wallet's first rollup its primary - the one an
    ** unnamed database uses. A named database's must never be it: the
    ** primary comes first (getOrCreateRollup is idempotent, also when two
    ** processes ask at once) */
    if (hpplite_l1_factory_get_my_rollup(config->rpcUrl, factoryHex, config->privkey, rollup) == 0 && node->relay
        && hpplite_l1_factory_create_rollup_for(config->rpcUrl, factoryHex, config->privkey, node->relay,
                                                config->checkpointInterval > 0 ? (uint64_t)config->checkpointInterval : 100, rollup) == 0) {
        /* the primary, through the relay (#126) */
    }
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
        if (hpplite_l1_factory_get_my_rollup(config->rpcUrl, factoryHex, config->privkey, rollup) != 1) { sqlite3_free(zOthers); sqlite3_free(zTail); return 0; }
    }
    if (node->config.verifyOnly) {
        hpplite_error(node->ctx, "role=verify: no rollup of this wallet is database \"%s\" (its databases: %s%s); nothing to verify",
                      zName, zOthers ? zOthers : "none named", zTail);
        sqlite3_free(zOthers); sqlite3_free(zTail);
        return 0;
    }
    if (nNoAnswer) {
        /* the store did not answer for some: one of them may be this database, and a second rollup under the
        ** name would be a second history (#229). Writes stay local; the next flush looks again */
        hpplite_error(node->ctx, "no rollup of this wallet is database \"%s\" among those readable (its databases: %s), and %d could not be read"
                      " because the store did not answer (%s): not creating one until it does",
                      zName, zOthers ? zOthers : "none named", nNoAnswer, node->ctx->zQuietError);
        sqlite3_free(zOthers); sqlite3_free(zTail);
        return 0;
    }
    /* a misspelt name would be a new database: say which exist (#79) */
    fprintf(stderr, "hpplite: no rollup of this wallet is database \"%s\" yet (its databases: %s%s); creating one\n",
            zName, zOthers ? zOthers : "none named", zTail);
    sqlite3_free(zOthers); sqlite3_free(zTail);
    if (node->relay) {
        int rc = hpplite_l1_factory_create_rollup_for(config->rpcUrl, factoryHex, config->privkey, node->relay,
                                                      config->checkpointInterval > 0 ? (uint64_t)config->checkpointInterval : 100, rollup);
        if (rc == 0) return 1;
        if (rc == -1) return 0;
    }
    return hpplite_l1_factory_create_rollup(config->rpcUrl, factoryHex, config->privkey,
                                            config->checkpointInterval > 0 ? (uint64_t)config->checkpointInterval : 100,
                                            rollup);
}

static void node_auto_connect_l1(HppliteNode *node, const HppliteNodeConfig *config) {
    if (node->follower) return;
    if (!config->rpcUrl) return;
    if (config->chainId) hpplite_l1_set_default_chain_id(config->chainId);
    if (!config->hasContract && !config->hasFactory && !config->hasFleet) return;
    node->wantsL1 = 1;

    /* contract= names the rollup and wins over any factory beside it - the alias's (l1=hpp-sepolia), HPPLITE_FACTORY,
    ** factory= (#271): the factory only finds or creates a rollup when none is named */
    if (config->hasFleet) {
        /* fleet= (#316): the chain client's fleet form, for this database; its lease is the fleet's record. First:
        ** a chain alias's factory never makes it a rollup instead */
        node->l1 = hpplite_l1_connect_fleet(config->rpcUrl, config->fleet, config->dbId, config->fleetOwner, config->nameHash,
                                            config->hasAdmit ? config->admitIssuedAt : 0, config->hasAdmit ? config->admitSig : NULL);
        node->ownsL1 = (node->l1 != NULL);
        if (node->l1 && config->hasPrivkey) hpplite_l1_set_privkey(node->l1, config->privkey);
        else if (node->l1 && node->hasSigner) hpplite_l1_set_signer(node->l1, node->signer.sign, node->signer.arg, node->signer.address);
    } else if (config->hasFactory && config->hasPrivkey && !config->hasContract) {
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
        if (result == 0 && !named && node->relay) {
            /* through the relay (#126): createRollupFor, the wallet's signature, the relay's gas */
            int rc = hpplite_l1_factory_create_rollup_for(config->rpcUrl, factoryHex, config->privkey, node->relay,
                                                          config->checkpointInterval > 0 ? (uint64_t)config->checkpointInterval : 100, rollup);
            if (rc == 0) result = hpplite_l1_factory_get_my_rollup(config->rpcUrl, factoryHex, config->privkey, rollup);
            /* -2: created from the wallet below (a factory before VERSION 10, or the relay failed and the wallet can pay) */
            else if (rc == -1) result = -1;
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
    } else if (config->hasFactory && node->hasSigner && !config->hasContract) {
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
    if (node->l1 && node->config.hasFleet && node->store && !node->config.verifyOnly) {
        /* fleet= (#316 d): a file behind the history the store holds (a fresh or emptied file, a new volume) is rebuilt
        ** from it - the lease first, then the store at this run's epoch (the fence: from then the store refuses the old
        ** holder, so nothing it writes after is missed), then the batches by the sync below. Not this run's lease: the
        ** file is not opened as the database's sequencer */
        HppliteFleetView v;
        /* the store not answering (a deploy, a herd of opens, #329): the open waits for it - with a backoff from 1 s to
        ** 30 s, a 429's Retry-After, a jitter of up to a quarter from the instance - and says so once; after
        ** HPPLITE_FLEET_OPEN_WAIT (120 s) it fails, loud: "synced height N" is not said of a file the store's history
        ** may be ahead of, nor is the database left open and unsequenced */
        const char *zWait = getenv("HPPLITE_FLEET_OPEN_WAIT");
        int64_t waitMs = (zWait && atol(zWait) > 0 ? atol(zWait) : 120) * 1000, t0 = node_current_time_ms(), backoff = 1000;
        int attempt = 0;
        while (hpplite_store_fleet_view(node->store, node->config.fleet, node_fleet_chain_id(node), node->config.dbId, &v) != 0) {
            int64_t now = node_current_time_ms(), pause = hpplite_store_retry_after(node->store) > 0 ? hpplite_store_retry_after(node->store) * 1000LL : backoff;
            pause += (int64_t)(node->instanceId[4 + attempt % 8] * (uint64_t)(pause / 4) / 256);
            if (now - t0 + pause > waitMs) {
                hpplite_error(node->ctx, "the store's view of this fleet database could not be read after %lld s: %s; not opened as its sequencer",
                              (long long)((now - t0) / 1000), hpplite_store_error(node->store));
                node->state = HPPLITE_STATE_ERROR;
                node->fleetOpenRefused = 2;
                return;
            }
            if (attempt++ == 0)
                fprintf(stderr, "hpplite: waiting for the store to read this fleet database's view: %s; tried again for up to %lld s (HPPLITE_FLEET_OPEN_WAIT)\n",
                        hpplite_store_error(node->store), (long long)(waitMs / 1000));
            usleep((useconds_t)(pause * 1000));
            backoff = backoff * 2 < 30000 ? backoff * 2 : 30000;
        }
        /* #330: the file emptied beside its bookkeeping at N (a restore that brought the sidecars back and not the file)
        ** is a lost file, never the database's newest state: rebuilt from the store's history, as a new volume is */
        if (node->ctx->blockHeight > 1 && !node_file_has_pages(node->db)) {
            const char *zLost = sqlite3_db_filename(node->db, "main");
            unsigned long long n = (unsigned long long)(node->ctx->blockHeight - 1);
            if (!v.hasHead) {
                hpplite_error(node->ctx, "%s is empty and its bookkeeping says the history has %llu heights, and the store holds none of it: not sent; "
                              "restore the file, or retire the database", zLost, n);
                node->state = HPPLITE_STATE_ERROR;
                node->fleetOpenRefused = 2;
                return;
            }
            if (hppliteResetMeta(node->ctx) != SQLITE_OK) {
                hpplite_error(node->ctx, "%s is empty and its bookkeeping says the history has %llu heights: the bookkeeping could not be reset for the rebuild", zLost, n);
                node->state = HPPLITE_STATE_ERROR;
                node->fleetOpenRefused = 2;
                return;
            }
            node->lastCheckpointHeight = 0;
            node->batchesSinceCheckpoint = 0;
            fprintf(stderr, "hpplite: %s is empty and its bookkeeping says the history has %llu heights: rebuilt from the store\n", zLost, n);
        }
        if (v.hasHead && v.toHeight + 1 > node->ctx->blockHeight) {
            int i, atEpoch = 0;
            const char *zFile = sqlite3_db_filename(node->db, "main");
            /* somaro#93, case 2: pages and no bookkeeping against a history at the store - which is true is not known
            ** here, so onforeign= decides; unset, nothing is touched, no claim (an empty or absent file is case 3) */
            int foreign = node->ctx->blockHeight <= 1 && zFile && zFile[0] && node_file_has_pages(node->db);
            if (foreign && !node->config.onForeign) {
                hpplite_error(node->ctx, "%s has pages and no bookkeeping, and the fleet's history of this database is at height %llu: refused; "
                              "onforeign=history rebuilds the file from the history, onforeign=file sends the file as height %llu",
                              zFile, (unsigned long long)v.toHeight, (unsigned long long)v.toHeight + 1);
                node->state = HPPLITE_STATE_ERROR;
                return;
            }
            node_learn_batch_limit(node);                     /* the relay, before the claim */
            node->leaseRecheckAt = 0;
            if (!node_ensure_lease(node)) {
                hpplite_error(node->ctx, "the store holds this database's history to height %llu and the file is at %llu: it is rebuilt by the lease's "
                              "holder, and the lease is not this run's (%s); not opened as its sequencer", (unsigned long long)v.toHeight,
                              (unsigned long long)(node->ctx->blockHeight - 1), node->ctx->zLastError[0] ? node->ctx->zLastError : "held elsewhere");
                node->state = HPPLITE_STATE_ERROR;
                return;
            }
            for (i = 0; i < 60 && !atEpoch; i++) {
                if (i) usleep(500 * 1000);
                atEpoch = hpplite_store_fleet_view(node->store, node->config.fleet, node_fleet_chain_id(node), node->config.dbId, &v) == 0
                          && v.epoch == hpplite_l1_fleet_epoch(node->l1) && memcmp(v.instance, node->instanceId, 32) == 0
                          && v.admittedAt == hpplite_l1_fleet_admitted_at(node->l1);      /* and its admission (#318) */
            }
            if (!atEpoch) {
                hpplite_error(node->ctx, "the store does not show this run's lease (epoch %llu) after 30 s: the rebuild waits for it (the fence); not opened as its sequencer",
                              (unsigned long long)hpplite_l1_fleet_epoch(node->l1));
                node->state = HPPLITE_STATE_ERROR;
                return;
            }
            if (foreign && node->config.onForeign == 2) {
                /* onforeign=file: the bookkeeping as of the history's height N - batch N's hash, the signed head's
                ** rolling hash and root - and the file, not that root, goes whole as N+1 (case 1's path) */
                size_t n = 0;
                unsigned char blk[32];
                uint8_t *raw = hpplite_store_get_fleet_name(node->store, node->config.fleet, node->config.dbId, v.admittedAt, v.toHeight, &n);
                unsigned char me[20];
                if (!raw || (hpplite_l1_get_my_address(node->l1, me) == 0 && memcmp(me, v.headSigner, 20) != 0)) {
                    free(raw);
                    hpplite_error(node->ctx, "onforeign=file: the store's height %llu could not be read or is not this database's key's: %s",
                                  (unsigned long long)v.toHeight, hpplite_store_error(node->store));
                    node->state = HPPLITE_STATE_ERROR;
                    return;
                }
                hpplite_sha256(raw, n, blk);
                free(raw);
                if (hppliteSetMetaAt(node->ctx, v.toHeight, blk, v.chainAfter, v.stateRoot) != SQLITE_OK) {
                    hpplite_error(node->ctx, "onforeign=file: the bookkeeping could not be set to height %llu", (unsigned long long)v.toHeight);
                    node->state = HPPLITE_STATE_ERROR;
                    return;
                }
                node->fleetForeignFile = 1;
                fprintf(stderr, "hpplite: %s sent whole as height %llu over the fleet's history (onforeign=file)\n", zFile, (unsigned long long)v.toHeight + 1);
            } else {
                if (foreign) {
                    /* onforeign=history: the batches are page diffs from an empty file, so the file is emptied first - only
                    ** now, the lease this run's and the store at its epoch, so a refused claim never costs the file */
                    char *zWal = sqlite3_mprintf("%s-wal", zFile);
                    if (truncate(zFile, 0) != 0 || (zWal && access(zWal, F_OK) == 0 && truncate(zWal, 0) != 0)) {
                        sqlite3_free(zWal);
                        hpplite_error(node->ctx, "onforeign=history: %s could not be emptied for the rebuild", zFile);
                        node->state = HPPLITE_STATE_ERROR;
                        return;
                    }
                    sqlite3_free(zWal);
                    node->fleetForeignHistory = v.toHeight;
                }
                fprintf(stderr, "hpplite: fleet database behind the store: rebuilding heights %llu to %llu from it\n",
                        (unsigned long long)node->ctx->blockHeight, (unsigned long long)v.toHeight);
            }
        }
    }
    if (node->l1) {
        node_learn_batch_limit(node);
        if (hpplite_node_sync_from_l1(node, 1) < 0) {
            node->state = HPPLITE_STATE_ERROR;
            /* fleet= (somaro): a history that does not rebuild - a root that differs, a rolling hash or signer that
            ** does not match - fails the open: no write, no lease used, and nothing a platform could take as ready */
            if (node->config.hasFleet) node->fleetOpenRefused = 1;
        } else if (node->fleetForeignHistory && node->ctx->blockHeight == node->fleetForeignHistory + 1 && (node->fleetForeignHistory = 0, 1)) {
            /* somaro#93: onforeign=history - the file is now the history's state, as a rebuild of an empty file */
            fprintf(stderr, "hpplite: %s replaced by the fleet's history at height %llu (onforeign=history)\n",
                    sqlite3_db_filename(node->db, "main"), (unsigned long long)(node->ctx->blockHeight - 1));
            if (!node->follower && !node->config.verifyOnly && (node->cipher || node->config.noEncrypt)) node_ensure_lease(node);
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
    if (!node->pm) {
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

/* fleet= (somaro): the open fails when the store's history did not rebuild - the reason as the open's error and on
** stderr, once */
static int node_fleet_refuse_open(HppliteNode *node) {
    static char why[700];
    if (!node->fleetOpenRefused) return 0;
    if (node->fleetOpenRefused == 2) snprintf(why, sizeof why, "%s", node->ctx ? node->ctx->zLastError : "the store's view could not be read");   /* #329 */
    else snprintf(why, sizeof why, "the fleet database could not be rebuilt from the store's history: %s; nothing was written and it is not "
             "opened as its sequencer", node->ctx && node->ctx->zLastError[0] ? node->ctx->zLastError : "the sync stopped");
    fprintf(stderr, "hpplite: %s\n", why);
    hpplite_set_init_error(why);
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
    if (node_fleet_refuse_open(node)) { hpplite_node_destroy(node); return NULL; }

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
    if (node_fleet_refuse_open(node)) { hpplite_node_destroy(node); return NULL; }

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
        && (hpplite_node_pending_count(node) > 0 || !node->pm) && node->flushMutex) {
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

    /* the batches the store holds since the last anchor get theirs (#164) -
    ** unless anchorclose=off (#266): then the timer anchors on its cadence and
    ** the close leaves the rows waiting, for an app that sleeps and wakes to
    ** pay one anchor a day rather than one per cycle */
    if (node->l1 && node->ctx && !node->ctx->bDbClosed && node->flushMutex && node_ranges(node) && !node->config.noAnchorAtClose) {
        pthread_mutex_lock((pthread_mutex_t *)node->flushMutex);
        node_anchor(node);
        pthread_mutex_unlock((pthread_mutex_t *)node->flushMutex);
    }
    if (node->manifest) { cJSON_Delete(node->manifest); node->manifest = NULL; }
    free(node->softChain);
    free(node->aEarlier);
    free(node->zAnchorRef);
    node->zAnchorRef = NULL;

    /* Let go of the sequencer lease so a restart need not wait it out (a
    ** database rebuilt into a fresh file, another host) - unless another
    ** connection to this file is still open: it takes over as the same
    ** sequencer (the id is the file's), and a release would only cost it a
    ** claim. keeplease=on (#265) keeps it in every case: a sidecar whose app
    ** sleeps and wakes is the same instance at the next open and finds the
    ** lease its own, so the cycle sends no claim and no release */
    if (node->l1 && node->hasLease && !node->config.keepLease && !(node->lockFd >= 0 && node_others_on_file(node))) {
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
    if (node->relay) {
        hpplite_store_close(node->relay);
        node->relay = NULL;
    }
    hpplite_spend_close(node->spend);           /* after the handles that wrote to it */
    node->spend = NULL;

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
    sqlite3_free(node->config.relayUrl);
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
    if (node->relay) hpplite_l1_set_relay(node->l1, node->relay);     /* before the interval write-back below (#126) */
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
            if (node_ranges(node)) {
                /* with a store on a chain with ranges the anchor carries the root (#164) and no checkpoint transaction
                ** is sent: checkpoint= would buy one contract write and nothing else, so it is ignored, said once (#256) */
                if (node->checkpointGiven && (uint64_t)node->config.checkpointInterval != cfg->checkpointInterval)
                    fprintf(stderr, "hpplite: checkpoint=%d ignored: with a store the anchor carries the root (every anchor=) and no checkpoint "
                                    "transaction is sent; the contract's interval (%llu) is left as it is\n",
                            node->config.checkpointInterval, (unsigned long long)cfg->checkpointInterval);
                node->config.checkpointInterval = (int)cfg->checkpointInterval;
            } else if (!node->checkpointGiven) {
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
    return NULL;                       /* a statements or rows batch (v0.7 and earlier): not read (#253) */
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
        hpplite_error(node->ctx, "batch %llu is not a page batch (%zu bytes): a statements or rows history from v0.7 or earlier, which this release does not read (#253)",
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
    uint8_t *data;
    if (node->config.hasFleet) {
        /* fleet= (#316 d): a batch is a name at the store; the seal is the database's key, the rolling hash is checked
        ** against the signed latest head when the sync is done */
        data = node->store ? hpplite_store_get_fleet_name(node->store, node->config.fleet, node->config.dbId, hpplite_l1_fleet_admitted_at(node->l1), height, pLen) : NULL;
        if (!data) hpplite_error(node->ctx, "batch %llu: %s", (unsigned long long)height, node->store ? hpplite_store_error(node->store) : "no store= names one");
        return data;
    }
    data = hpplite_l1_get_batch(l1, height, pLen);
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
        if (hpplite_store_funder(node->store, me) != 0 && hpplite_l1_get_my_address(node->l1, me) != 0) return -1;
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

HppliteBatch *hpplite_node_decode(HppliteNode *node, uint64_t height, const uint8_t *data, size_t len) {
    return node && data ? node_decode_batch(node, height, data, len) : NULL;
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
    rc = node_adopt_pages(node, batch);
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
/* the chain a fleet's heads are signed on (#316) */
static uint64_t node_fleet_chain_id(HppliteNode *node) {
    uint64_t id = node->l1 ? hpplite_l1_chain_id(node->l1) : 0;
    return id ? id : hpplite_l1_default_chain_id();
}

/* fleet= (#316 d): the fence's line, once - an operator sees the epoch held and the record's */
static void node_say_fenced(HppliteNode *node) {
    if (node->warnedFenced) return;
    node->warnedFenced = 1;
    hpplite_error(node->ctx, "%s", hpplite_l1_relay_error(node->l1) && strncmp(hpplite_l1_relay_error(node->l1), "fenced:", 7) == 0
                                   ? hpplite_l1_relay_error(node->l1) : "fenced: the fleet's record of this database moved past this run's epoch; this run writes no more - reopen to take the lease again");
}

static int node_ensure_lease(HppliteNode *node) {
    int64_t now = node_current_time_ms();
    HppliteL1State *st;
    uint64_t chainNow, remaining;
    int ours;

    if (now < node->leaseRecheckAt) return node->hasLease;

    if (node->config.hasFleet) {
        /* fleet= (#316 d): the record moved past this run's epoch - it writes no more and reads nothing, until reopened;
        ** it stays up (a restart is a new run that may claim) */
        hpplite_l1_fleet_set_instance(node->l1, node->instanceId);
        if (hpplite_l1_fleet_fenced(node->l1)) { node->hasLease = 0; node->leaseRecheckAt = INT64_MAX; return 0; }
    }
    st = hpplite_l1_get_state(node->l1);
    if (!st) {
        hpplite_error(node->ctx, "cannot read the sequencer lease from L1; not posting");
        node->leaseRecheckAt = now + 5000;
        return 0;
    }
    if (node->config.hasFleet && hpplite_l1_fleet_fenced(node->l1)) {
        hpplite_l1_state_free(st);
        node_say_fenced(node);
        node->hasLease = 0;
        node->leaseRecheckAt = INT64_MAX;
        return 0;
    }
    chainNow = st->currentTimestamp ? st->currentTimestamp : (uint64_t)time(NULL);
    remaining = st->leaseExpiry > chainNow ? st->leaseExpiry - chainNow : 0;
    ours = remaining > 0 && memcmp(st->sequencerInstance, node->instanceId, 32) == 0;
    if (st->leaseDuration > 0) node->leaseDuration = st->leaseDuration;
    {
        /* Another wallet's lease: the chain names the sequencer (only that wallet claims, VERSION 5 and later), so
        ** it is the sequencer the owner set - after a rotation (#208) this node is the old one. Nothing is evicted
        ** (#252: the pre-VERSION-5 eviction went with those factories); a sibling instance of our own wallet is
        ** left alone too - no takeover */
        unsigned char me[20];
        int stranger = remaining > 0 && !ours && hpplite_l1_get_my_address(node->l1, me) == 0
                       && memcmp(st->sequencerWallet, me, 20) != 0;
        hpplite_l1_state_free(st);
        if (stranger) hpplite_error(node->ctx, "another wallet holds the lease: the chain names it as the sequencer (a rotation, #208?); not posting");
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
            if (node->config.hasFleet) {
                /* fleet= (#316 d): fenced, or refused (the relay's 402, a retired record...): rows stay pending; the claim
                ** is tried again every 30 s, the reason said once per episode */
                const char *why = hpplite_l1_relay_error(node->l1);
                int ra = node->relay ? hpplite_store_retry_after(node->relay) : 0;
                int limited = ra > 0 || (why && strstr(why, "answered 429")), queued = why && strstr(why, "answered 503");
                int64_t wait = ra > 0 ? ra * 1000LL : 30000, jitter;
                if (hpplite_l1_fleet_fenced(node->l1)) { node_say_fenced(node); node->leaseRecheckAt = INT64_MAX; return 0; }
                /* the relay's 429 says when (somaro#48); a jitter of up to a quarter of the wait on top, drawn from the
                ** instance, so a host's refused claims come back spread over the relay's next window, not all at its edge */
                jitter = wait / 4;
                wait += (int64_t)((((uint64_t)node->instanceId[2] << 8) | node->instanceId[3]) % (uint64_t)jitter);
                if (!node->warnedClaim) {
                    if (limited)
                        hpplite_error(node->ctx, "the fleet's lease waits on the relay's %s (%s): batches are kept pending, the claim tried again in %lld s%s",
                                      queued ? "send queue" : "rate limit", why && why[0] ? why : "429", (long long)(wait / 1000), ra > 0 ? " (its Retry-After, and a jitter)" : "");
                    else
                        hpplite_error(node->ctx, "the fleet's lease could not be taken: %s; batches are kept pending, tried again every 30 s",
                                      why && why[0] ? why : "no reason given");
                    node->warnedClaim = 1;
                }
                node->leaseRecheckAt = now + (limited ? wait : 30000);
            }
            return 0;
        }
        node->warnedClaim = 0;
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
#define HPPLITE_IDLE_SYNC_MS (10 * 60 * 1000)   /* an idle writer holding the lease looks at the chain this often (#292) */

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
        {
            char hr[65], hb[65];
            hpplite_bytes_to_hex(root, 32, hr); hpplite_bytes_to_hex(b->root, 32, hb);
            hpplite_error(node->ctx, "batch %llu: the root after applying it differs from the posted one (applied 0x%.16s.., posted 0x%.16s..)",
                          (unsigned long long)batch->height, hr, hb);
        }
        hpplite_pagebatch_free(b);
        return SQLITE_MISMATCH;
    }
    node->ctx->rootCmpHeight = batch->height;          /* for the verifier: compared with the on-chain checkpoint (#5) */
    memcpy(node->ctx->rootCmp, b->root, 32);
    if (node->snapApply) rc = hppliteSetMetaAt(node->ctx, batch->height, node->snapBlock, node->snapChain, b->root);   /* a snapshot: the bookkeeping at its height (#222) */
    else rc = hppliteAdvanceMetaPages(node->ctx, batch->height, batch->pPosted, batch->nPosted, b->root, b->bMore);   /* the posted bytes: the chain hash (#33) */
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

/* The store's bucket is the rollup's (hpplite-store/2, #214): named by the DA address, bound and
** given the funding wallet's permit (HPPLITE_STORE_PERMIT) at the first write, once the addresses are known. */
static void node_store_bucket(HppliteNode *node) {
    char c[43], d[43];
    const char *zDa;
    unsigned char da[20];
    /* whatever the store speaks (#244): whether it keeps buckets is learnt from its /status, which a store down
    ** at open has not answered yet; the address is only recorded, a store that keeps no buckets ignores it */
    if (node->storeBucketSet || !node->store) return;
    if (!node->l1 || hpplite_l1_get_addresses(node->l1, c, d) != 0) return;
    zDa = d[0] ? d : c;
    if (strlen(zDa) != 42 || hpplite_hex_to_bytes(zDa + 2, da, 20) != 20) return;
    hpplite_store_set_bucket(node->store, da, node->config.storePermit);
    node->storeBucketSet = 1;
}

/* The batch under its rollup's name at the store (#165): the bytes with the sequencer's
** signature, so a reader finds it by height before the anchor. Best effort: a store that
** takes no names, or refuses, is said once; the hard tier does not depend on it. */
/* The batch under its name: 1 written, 0 not attempted (no addresses, no key to sign with), -1 the store refused it */
static int node_name_batch(HppliteNode *node, uint64_t height, const uint8_t *payload, size_t payloadLen) {
    char c[43], d[43], name[80];
    unsigned char da[20];
    uint8_t sig[65];
    const char *zDa;
    uint64_t chainId;
    if (!node->store || !node->l1 || hpplite_l1_get_addresses(node->l1, c, d) != 0) return 0;
    zDa = d[0] ? d : c;
    if (strlen(zDa) != 42 || hpplite_hex_to_bytes(zDa + 2, da, 20) != 20) return 0;
    chainId = hpplite_l1_chain_id(node->l1);
    if (!chainId) chainId = hpplite_l1_default_chain_id();          /* a backend on no chain (the mock): the signing domain still needs one */
    if (hpplite_store_sign_batch(node->store, da, chainId, node->instanceId, height, payload, payloadLen, sig) != 0) return 0;   /* no raw key here: the content put stands alone */
    snprintf(name, sizeof name, "%.40s-%llu", zDa + 2, (unsigned long long)height);
    { char *p; for (p = name; *p && *p != '-'; p++) if (*p >= 'A' && *p <= 'F') *p = (char)(*p - 'A' + 'a'); }
    return hpplite_store_put_named(node->store, name, node->instanceId, payload, payloadLen, sig) == 0 ? 1 : -1;
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

/* The manifest the range commitment ending at `height` names, fetched by its digest's CID and checked against it; NULL: none */
static cJSON *node_range_manifest_on(HppliteNode *node, HppliteL1 *l1, uint64_t height) {
    HppliteRange r;
    char cid[64];
    uint8_t *text;
    size_t n = 0;
    cJSON *m = NULL;
    memset(&r, 0, sizeof r);
    if (!node->store || !l1 || height == 0 || hpplite_l1_get_range(l1, height, &r) != 0) return NULL;
    hpplite_store_cid_from_sha256(r.manifestDigest, cid);
    text = node_store_get_checked(node, cid, r.manifestDigest, &n);
    if (text) m = cJSON_ParseWithLength((const char *)text, n);
    free(text);
    hpplite_l1_range_clear(&r);
    return m;
}

/* a snapshot's index (#221) by its CID, checked against the sha-256 the manifest names; parsed, or NULL */
static cJSON *node_snapshot_index(HppliteNode *node, const char *zIndex, const unsigned char sha[32]) {
    size_t n = 0;
    uint8_t *text = node_store_get_checked(node, zIndex, sha, &n);
    cJSON *ix = text ? cJSON_ParseWithLength((const char *)text, n) : NULL;
    free(text);
    return ix;
}

static cJSON *node_range_manifest(HppliteNode *node, uint64_t height) { return node_range_manifest_on(node, node->l1, height); }

/* a 32-byte hex field of a manifest or index (#240): a string, 0x and 64 hex digits (or the 64 alone); never read
** past a short one */
static int json_hex32(const cJSON *j, unsigned char out[32]) {
    return cJSON_IsString(j) && j->valuestring && hpplite_hex_to_bytes(j->valuestring, out, 32) == 32;
}

/* a snapshot a manifest names - "snapshot" the latest, "earliest" the oldest still kept (#257): 0 and its height, -1 none */
static int node_manifest_snapshot_key(const cJSON *m, const char *zKey, uint64_t *pHeight, char zIndex[64], int64_t *pAt, unsigned char sha[32]) {
    const cJSON *sn = m ? cJSON_GetObjectItemCaseSensitive(m, zKey) : NULL;
    const cJSON *h = sn ? cJSON_GetObjectItemCaseSensitive(sn, "height") : NULL;
    const cJSON *ix = sn ? cJSON_GetObjectItemCaseSensitive(sn, "index") : NULL;
    const cJSON *at = sn ? cJSON_GetObjectItemCaseSensitive(sn, "at") : NULL;
    const cJSON *sh = sn ? cJSON_GetObjectItemCaseSensitive(sn, "sha256") : NULL;
    if (!cJSON_IsNumber(h) || h->valuedouble < 1 || !cJSON_IsString(ix) || strlen(ix->valuestring) >= 64
        || !json_hex32(sh, sha)) return -1;
    *pHeight = (uint64_t)h->valuedouble;
    snprintf(zIndex, 64, "%s", ix->valuestring);
    if (pAt) *pAt = cJSON_IsNumber(at) ? (int64_t)at->valuedouble : 0;
    return 0;
}
static int node_manifest_snapshot(const cJSON *m, uint64_t *pHeight, char zIndex[64], int64_t *pAt, unsigned char sha[32]) {
    return node_manifest_snapshot_key(m, "snapshot", pHeight, zIndex, pAt, sha);
}

/* The older snapshots still kept (#223): the sequencer's own list, oldest first, in the sidecar's _hpplite_snapshots
** (#257) - a restart on this file has it; the manifest names only the latest and the oldest. */
static int node_earlier_push(HppliteNode *node, uint64_t height, const char *zIndex, const unsigned char sha[32], int64_t at) {
    HppliteSnapRef *e;
    if (node->nEarlier == node->nEarlierAlloc) {
        int n = node->nEarlierAlloc ? node->nEarlierAlloc * 2 : 8;
        HppliteSnapRef *a = realloc(node->aEarlier, (size_t)n * sizeof *a);
        if (!a) return -1;
        node->aEarlier = a;
        node->nEarlierAlloc = n;
    }
    e = &node->aEarlier[node->nEarlier++];
    e->height = height;
    snprintf(e->index, sizeof e->index, "%s", zIndex);
    memcpy(e->sha, sha, 32);
    e->at = at;
    return 0;
}
/* the list from the sidecar, oldest first; entries below `floor` are gone from the store (a prune this file did not
** record: the chain's manifest, which names the oldest kept, is right about it) */
static void node_snapshots_load(HppliteNode *node, uint64_t floor) {
    sqlite3_stmt *q = NULL;
    node->nEarlier = 0;
    if (!node->ctx || !node->ctx->bkDb) return;
    if (sqlite3_prepare_v2(node->ctx->bkDb, "SELECT height, idx, sha, at FROM _hpplite_snapshots WHERE height >= ?1 ORDER BY height", -1, &q, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)floor);
    while (sqlite3_step(q) == SQLITE_ROW) {
        const char *ix = (const char *)sqlite3_column_text(q, 1);
        if (ix && sqlite3_column_bytes(q, 2) == 32 && strlen(ix) < 64)
            node_earlier_push(node, (uint64_t)sqlite3_column_int64(q, 0), ix, sqlite3_column_blob(q, 2), sqlite3_column_int64(q, 3));
    }
    sqlite3_finalize(q);
}
static void node_snapshots_save(HppliteNode *node) {
    sqlite3_stmt *q = NULL;
    int i;
    if (!node->ctx || !node->ctx->bkDb) return;
    sqlite3_exec(node->ctx->bkDb, "DELETE FROM _hpplite_snapshots", NULL, NULL, NULL);
    if (sqlite3_prepare_v2(node->ctx->bkDb, "INSERT INTO _hpplite_snapshots(height, idx, sha, at) VALUES(?1, ?2, ?3, ?4)", -1, &q, NULL) != SQLITE_OK) return;
    for (i = 0; i < node->nEarlier; i++) {
        sqlite3_bind_int64(q, 1, (sqlite3_int64)node->aEarlier[i].height);
        sqlite3_bind_text(q, 2, node->aEarlier[i].index, -1, SQLITE_STATIC);
        sqlite3_bind_blob(q, 3, node->aEarlier[i].sha, 32, SQLITE_STATIC);
        sqlite3_bind_int64(q, 4, (sqlite3_int64)node->aEarlier[i].at);
        sqlite3_step(q);
        sqlite3_reset(q);
    }
    sqlite3_finalize(q);
}

#define HPPLITE_SNAPSHOT_CHUNK (8u << 20)     /* well under a store's size limit (16 MiB on Sepolia) */

/* bytes compressed (at this deflate level) and sealed as a batch at `height` is - the same recipients, its own
** data key - or framed plain without a key (#221); malloc'd, NULL on failure */
static uint8_t *node_seal_at(HppliteNode *node, uint64_t height, const uint8_t *in, size_t len, int level, size_t *pLen) {
    const uint8_t *payload = in;
    size_t payloadLen = len;
    uint8_t *packed = NULL, *out = NULL;
    uint8_t encoding = HPPLITE_ENCODING_NONE;
    if (!node->config.noCompress) {
        size_t n = 0;
        uint8_t *c = hpplite_deflate_level(payload, payloadLen, level, &n);
        if (c && n + HPPLITE_PLAIN_MAGIC_LEN < payloadLen) { packed = c; payload = c; payloadLen = n; encoding = HPPLITE_ENCODING_DEFLATE; } else free(c);
    }
    if (node->cipher && node->config.hasPrivkey && node_recipients(node, NULL) > 1) {
        uint8_t *aTo = NULL, dek[32];
        int nTo = node_recipients(node, &aTo);
        hpplite_random_bytes(dek, 32);                  /* its own data key: a batch series' is not disturbed */
        if (aTo && nTo > 1) out = hpplite_envelope_seal_pk(node->cipher, dek, aTo, nTo, height, encoding, payload, payloadLen, &payloadLen);
        free(aTo);
    } else if (node->cipher) {
        out = hpplite_envelope_seal(node->cipher, node->encKey, height, encoding, payload, payloadLen, &payloadLen);
    } else {
        out = malloc((encoding == HPPLITE_ENCODING_DEFLATE ? HPPLITE_PLAIN_MAGIC_LEN : 0) + payloadLen);
        if (out && encoding == HPPLITE_ENCODING_DEFLATE) { memcpy(out, HPPLITE_PLAIN_MAGIC, HPPLITE_PLAIN_MAGIC_LEN); memcpy(out + HPPLITE_PLAIN_MAGIC_LEN, payload, payloadLen); payloadLen += HPPLITE_PLAIN_MAGIC_LEN; }
        else if (out) memcpy(out, payload, payloadLen);
    }
    free(packed);
    *pLen = out ? payloadLen : 0;
    return out;
}

/* A rollup's name from its latest snapshot (#226): batch 1, which names it (#160), may be pruned (keep=, #223).
** The manifest the last anchor names, its snapshot's index, the sealed name object - each checked by its hash. */
static char *node_snapshot_name(HppliteNode *node, HppliteL1 *l1) {
    uint64_t last = 0, total = 0, h = 0;
    unsigned char latest[32], ixSha[32];
    char zIndex[64], *zName = NULL;
    int64_t at = 0;
    cJSON *m = NULL, *ix = NULL;
    uint8_t *bytes = NULL;
    size_t n = 0;
    const cJSON *nm, *ref, *sha;
    if (!node->store || !l1 || hpplite_l1_get_da_state(l1, &last, &total, latest) != 0 || last == 0) return NULL;
    m = node_range_manifest_on(node, l1, last);
    if (!m || node_manifest_snapshot(m, &h, zIndex, &at, ixSha) != 0) goto done;
    ix = node_snapshot_index(node, zIndex, ixSha);
    nm = ix ? cJSON_GetObjectItemCaseSensitive(ix, "name") : NULL;
    ref = nm ? cJSON_GetObjectItemCaseSensitive(nm, "ref") : NULL;
    sha = nm ? cJSON_GetObjectItemCaseSensitive(nm, "sha256") : NULL;
    if (!cJSON_IsString(ref) || !json_hex32(sha, latest)) goto done;
    bytes = node_store_get_checked(node, ref->valuestring, latest, &n);
    if (bytes) {
        HppliteBatch *b = node_decode_batch(node, h, bytes, n);
        if (b && b->zDbName) zName = sqlite3_mprintf("%s", b->zDbName);
        hpplite_batch_free(b);
    }
done:
    free(bytes);
    cJSON_Delete(ix); cJSON_Delete(m);
    return zName;
}

/*
** A snapshot (#221): the database as of `height` - the anchored height, the
** last batch finalized - from the page mode's images (exact by construction,
** no read of the live file), a page batch with every page full, compressed
** and sealed as a batch at that height is, to the same recipients. Its bytes
** go to the store in chunks (content puts at the height), then an index
** naming them with the height, the root, the chain hash and the block hash
** there; the anchor's manifest names the index. 0 made, -1 not (said once).
*/
static int node_make_snapshot(HppliteNode *node, uint64_t height, const unsigned char root[32]) {
    HppliteCtx *ctx = node->ctx;
    unsigned char *image = NULL, imgRoot[32];
    uint8_t *sealed = NULL;
    const uint8_t *payload;
    size_t len = 0, payloadLen, off;
    cJSON *ix = NULL, *chunks;
    char *text = NULL, *zRef = NULL, hx[67];
    const char *why = NULL;
    int rc = -1, prc;
    if (!node->pm) why = "the rollup is not in page mode (capture=pages)";
    else if (ctx->blockHeight != height + 1 || ctx->openTxnId) why = "a commit is cut over batches at the anchored height";
    if (!why && (prc = hpplite_pagemode_image(node->pm, &image, &len, imgRoot)) != SQLITE_OK) why = prc == SQLITE_BUSY ? "the whole file is still queued" : "the image could not be built";
    if (!why && memcmp(imgRoot, root, 32) != 0) why = "the image's root is not the anchored one";
    if (why) {
        if (!node->warnedSnapshot) { fprintf(stderr, "hpplite: no snapshot at height %llu: %s\n", (unsigned long long)height, why); node->warnedSnapshot = 1; }
        sqlite3_free(image);
        return -1;
    }
    payload = sealed = node_seal_at(node, height, image, len, 6, &payloadLen);
    if (!payload) goto done;
    ix = cJSON_CreateObject();
    cJSON_AddNumberToObject(ix, "v", 1);
    cJSON_AddNumberToObject(ix, "height", (double)height);
    hex32(root, hx); cJSON_AddStringToObject(ix, "root", hx);
    hex32(ctx->chainHash, hx); cJSON_AddStringToObject(ix, "chain", hx);
    hex32(ctx->prevBlockHash, hx); cJSON_AddStringToObject(ix, "block", hx);
    cJSON_AddNumberToObject(ix, "at", (double)time(NULL));
    cJSON_AddNumberToObject(ix, "bytes", (double)payloadLen);
    chunks = cJSON_AddArrayToObject(ix, "chunks");
    node_store_bucket(node);
    for (off = 0; off < payloadLen; off += HPPLITE_SNAPSHOT_CHUNK) {
        size_t n = payloadLen - off < HPPLITE_SNAPSHOT_CHUNK ? payloadLen - off : HPPLITE_SNAPSHOT_CHUNK;
        zRef = hpplite_store_put_at(node->store, height, payload + off, n);
        if (!zRef) { hpplite_error(ctx, "snapshot at %llu: the store did not take a chunk: %s", (unsigned long long)height, hpplite_store_error(node->store)); goto done; }
        {
            /* each chunk with its sha-256: a reader checks what any store serves (a Kubo CID of a large file is not one) */
            cJSON *ch = cJSON_CreateObject();
            unsigned char d[32];
            hpplite_store_sha256(payload + off, n, d);
            cJSON_AddStringToObject(ch, "ref", zRef);
            hex32(d, hx); cJSON_AddStringToObject(ch, "sha256", hx);
            cJSON_AddItemToArray(chunks, ch);
        }
        sqlite3_free(zRef); zRef = NULL;
    }
    if (ctx->zDbName && ctx->zDbName[0]) {
        /* the database's name (#160) as batch 1 carries it, sealed: a named rollup is found by it once
        ** batch 1 is pruned (#226) - a page batch with no pages, small to read for a lookup */
        HpplitePageBatch nb;
        unsigned char *nbytes;
        size_t nlen = 0, slen = 0;
        uint8_t *nsealed;
        memset(&nb, 0, sizeof nb);
        nb.pageSize = (unsigned)hpplite_pagemode_page_size(node->pm);
        memcpy(nb.root, root, 32);
        nb.zName = ctx->zDbName; nb.nName = (unsigned)strlen(ctx->zDbName);
        const char *zNameWhy = "the name could not be sealed";
        nbytes = hpplite_pagebatch_encode(&nb, &nlen);
        nsealed = nbytes ? node_seal_at(node, height, nbytes, nlen, 1, &slen) : NULL;
        sqlite3_free(nbytes);
        if (nsealed) {
            zNameWhy = hpplite_store_error(node->store);
            zRef = hpplite_store_put_at(node->store, height, nsealed, slen);
            if (zRef) {
                cJSON *nm = cJSON_AddObjectToObject(ix, "name");
                unsigned char d[32];
                hpplite_store_sha256(nsealed, slen, d);
                cJSON_AddStringToObject(nm, "ref", zRef);
                hex32(d, hx); cJSON_AddStringToObject(nm, "sha256", hx);
                sqlite3_free(zRef); zRef = NULL;
            }
            free(nsealed);
        }
        if (!cJSON_GetObjectItemCaseSensitive(ix, "name")) {
            /* without the name a pruned rollup could not be found by dbname= (#226), silently (#235): no snapshot
            ** this anchor; the next one tries again */
            hpplite_error(ctx, "snapshot at %llu: the store did not take its name: %s", (unsigned long long)height,
                          zNameWhy);
            goto done;
        }
    }
    text = cJSON_PrintUnformatted(ix);
    if (!text) goto done;
    zRef = hpplite_store_put_at(node->store, height, (const uint8_t *)text, strlen(text));
    if (!zRef || strlen(zRef) >= sizeof node->snapIndex) { hpplite_error(ctx, "snapshot at %llu: the store did not take its index: %s", (unsigned long long)height, hpplite_store_error(node->store)); goto done; }
    node->snapHeight = height;
    snprintf(node->snapIndex, sizeof node->snapIndex, "%s", zRef);
    hpplite_store_sha256((const uint8_t *)text, strlen(text), node->snapIndexSha);
    memcpy(node->snapRoot, root, 32);
    node->snapAt = (int64_t)time(NULL);
    node->snapLooked = 1;
    node->warnedSnapshot = 0;
    rc = 0;
done:
    node->storeBalanceOk = 0;
    sqlite3_free(zRef);
    free(text);
    cJSON_Delete(ix);
    sqlite3_free(image); free(sealed);
    return rc;
}

static void node_prune_plan(HppliteNode *node);

#define HPPLITE_FLEET_SNAPSHOT_BATCHES 100

/*
** A fleet database's snapshot (#314, doc/fleet-protocol.md "Snapshots and pruning"): after a flush, while the lease is
** this run's, when HPPLITE_FLEET_SNAPSHOT_BATCHES batches have gone since the last one or snapshot= has passed with any
** batch since. The page mode's image at the height just finalized, its root the head's; sealed as a batch at that
** height is, in chunks, then the index naming them - each piece signed by this database's key. The store checks the
** index against the head it holds there and lets the history below go once a root holds a later head (unless
** keep=all). Not taken: said once, tried at the next flush; the batches go on regardless.
*/
static void node_fleet_snapshot(HppliteNode *node, uint64_t height, const unsigned char root[32]) {
    HppliteCtx *ctx = node->ctx;
    unsigned char *image = NULL, imgRoot[32], sha[32];
    uint8_t *sealed = NULL;
    size_t len = 0, sealedLen = 0, off, nChunks = 0;
    const char *why = NULL;
    char fl[43], db[67], hx[67], *zChunks = NULL, *zIndex = NULL;
    uint64_t chainId, admitted, part;
    int64_t now = (int64_t)time(NULL);
    int prc, i;
    if (node->config.snapshotIntervalMs <= 0 || !node->pm || !node->store || !node->hasLease || height < 2) return;
    if (!node->snapAt) node->snapAt = now;          /* none known: the interval counts from this run */
    if (height < node->snapHeight + HPPLITE_FLEET_SNAPSHOT_BATCHES
        && !(height > node->snapHeight && (now - node->snapAt) * 1000 >= (int64_t)node->config.snapshotIntervalMs)) return;
    if (ctx->blockHeight != height + 1 || ctx->openTxnId) return;          /* a commit cut over batches: the next flush */
    prc = hpplite_pagemode_image(node->pm, &image, &len, imgRoot);
    if (prc == SQLITE_BUSY) { sqlite3_free(image); return; }               /* commits still queued: the next flush */
    if (prc != SQLITE_OK) why = "the image could not be built";
    else if (memcmp(imgRoot, root, 32) != 0) why = "the image's root is not the head's";
    if (!why && !(sealed = node_seal_at(node, height, image, len, 6, &sealedLen))) why = "the image could not be sealed";
    sqlite3_free(image);
    chainId = hpplite_l1_chain_id(node->l1);
    if (!chainId) chainId = hpplite_l1_default_chain_id();
    admitted = hpplite_l1_fleet_admitted_at(node->l1);
    zChunks = sqlite3_mprintf("");
    for (off = 0, part = 1; !why && off < sealedLen; off += HPPLITE_SNAPSHOT_CHUNK, part++) {
        size_t n = sealedLen - off < HPPLITE_SNAPSHOT_CHUNK ? sealedLen - off : HPPLITE_SNAPSHOT_CHUNK;
        char *z;
        if (hpplite_store_put_fleet_snapshot(node->store, node->config.fleet, chainId, node->config.dbId, admitted, height, part, sealed + off, n) != 0) {
            why = hpplite_store_error(node->store);
            break;
        }
        hpplite_store_sha256(sealed + off, n, sha);
        hpplite_bytes_to_hex(sha, 32, hx);
        z = sqlite3_mprintf("%s%s\"0x%s\"", zChunks, nChunks ? "," : "", hx);
        sqlite3_free(zChunks); zChunks = z;
        nChunks++;
    }
    if (!why && zChunks) {
        /* fleet.snapshot_index: canonical JSON, keys sorted */
        char blk[67], ch[67], rt[67];
        for (i = 0; i < 20; i++) snprintf(fl + 2 * i, 3, "%02x", node->config.fleet[i]);
        for (i = 0; i < 32; i++) snprintf(db + 2 * i, 3, "%02x", node->config.dbId[i]);
        hpplite_bytes_to_hex(ctx->prevBlockHash, 32, blk); hpplite_bytes_to_hex(ctx->chainHash, 32, ch); hpplite_bytes_to_hex(root, 32, rt);
        zIndex = sqlite3_mprintf("{\"admittedAt\":%llu,\"at\":%lld,\"block\":\"0x%s\",\"bytes\":%llu,\"chain\":\"0x%s\",\"chunks\":[%s],"
                                 "\"dbId\":\"0x%s\",\"height\":%llu,\"protocol\":\"hpplite-fleet/1\",\"prune\":%s,\"rollup\":\"0x%s\",\"root\":\"0x%s\"}",
                                 (unsigned long long)admitted, (long long)now, blk, (unsigned long long)sealedLen, ch, zChunks, db,
                                 (unsigned long long)height, node->config.keepAll ? "false" : "true", fl, rt);
        if (!zIndex || hpplite_store_put_fleet_snapshot(node->store, node->config.fleet, chainId, node->config.dbId, admitted, height, 0,
                                                        (const uint8_t *)zIndex, strlen(zIndex)) != 0)
            why = zIndex ? hpplite_store_error(node->store) : "out of memory";
    }
    free(sealed);
    sqlite3_free(zChunks);
    sqlite3_free(zIndex);
    node->storeBalanceOk = 0;
    if (why) {
        if (!node->warnedSnapshot) fprintf(stderr, "hpplite: no snapshot at height %llu: %s; tried at the next flush\n", (unsigned long long)height, why);
        node->warnedSnapshot = 1;
        return;
    }
    node->warnedSnapshot = 0;
    node->snapHeight = height;
    node->snapAt = now;
    fprintf(stderr, "hpplite: snapshot at height %llu put at the store (%llu bytes in %llu chunk%s)%s\n", (unsigned long long)height,
            (unsigned long long)sealedLen, (unsigned long long)nChunks, nChunks == 1 ? "" : "s",
            node->config.keepAll ? ", the history kept (keep=all)" : "; the history below it goes once a root holds a later head");
}

/*
** A fleet database rebuilt from its latest snapshot (#314): the file behind it (empty, a lost volume) or below the
** history the store pruned. The index by its CID, its Snapshot signature this database's key's; each chunk by its
** sha-256; the image opened with this database's keys, every page, its root the index's - all before the file is
** touched. Then written as the file, the bookkeeping set at its height (block, chain, root), and the batches after it
** follow; the rolling hash after the last must be the latest head's, signed by this database's key (the caller's
** check), which ties the snapshot's chain hash to the history. 1 started there; 0 not (why said); -1 the file could
** not be written.
*/
static int node_fleet_start_from_snapshot(HppliteNode *node, const HppliteFleetView *fv) {
    HppliteCtx *ctx = node->ctx;
    uint8_t *text = NULL, *all = NULL;
    size_t n = 0, total = 0, want = 0;
    unsigned char sha[32], digest[32], signer[20], me[20], root[32], chain[32], block[32];
    char cid[64];
    cJSON *ix = NULL, *chunks = NULL, *c;
    HppliteBatch *batch = NULL;
    const char *why = NULL;
    uint64_t h = fv->snapHeight, chainId = hpplite_l1_chain_id(node->l1);
    if (!chainId) chainId = hpplite_l1_default_chain_id();
    text = hpplite_store_get(node->store, fv->snapIndex, &n);
    if (text) { hpplite_store_sha256(text, n, sha); hpplite_store_cid_from_sha256(sha, cid); }
    if (!text || strcmp(cid, fv->snapIndex) != 0) why = "its index is not at the store under its CID";
    if (!why) {
        hpplite_store_fleet_snapshot_digest(node->config.fleet, chainId, node->config.dbId, fv->admittedAt, h, 0, sha, digest);
        if (hpplite_store_recover(digest, fv->snapSig, signer) != 0 || hpplite_l1_get_my_address(node->l1, me) != 0 || memcmp(signer, me, 20) != 0)
            why = "its index is not signed by this database's key";
    }
    if (!why && !(ix = cJSON_ParseWithLength((const char *)text, n))) why = "its index is not JSON";
    if (!why) {
        const cJSON *jh = cJSON_GetObjectItemCaseSensitive(ix, "height"), *ja = cJSON_GetObjectItemCaseSensitive(ix, "admittedAt"),
                    *jn = cJSON_GetObjectItemCaseSensitive(ix, "bytes");
        unsigned char jdb[32];
        chunks = cJSON_GetObjectItemCaseSensitive(ix, "chunks");
        if (!cJSON_IsNumber(jh) || (uint64_t)jh->valuedouble != h || !cJSON_IsNumber(ja) || (uint64_t)ja->valuedouble != fv->admittedAt
            || !cJSON_IsNumber(jn) || !cJSON_IsArray(chunks) || !json_hex32(cJSON_GetObjectItemCaseSensitive(ix, "dbId"), jdb) || memcmp(jdb, node->config.dbId, 32) != 0
            || !json_hex32(cJSON_GetObjectItemCaseSensitive(ix, "root"), root) || !json_hex32(cJSON_GetObjectItemCaseSensitive(ix, "chain"), chain)
            || !json_hex32(cJSON_GetObjectItemCaseSensitive(ix, "block"), block))
            why = "its index is malformed or names another database, admission or height";
        else want = (size_t)jn->valuedouble;
    }
    free(text);
    if (!why) {
        cJSON_ArrayForEach(c, chunks) {
            unsigned char d[32];
            uint8_t *piece = NULL, *grown;
            if (json_hex32(c, d)) { hpplite_store_cid_from_sha256(d, cid); piece = node_store_get_checked(node, cid, d, &n); }
            if (!piece) { why = "a chunk is not at the store, or not the one the index names"; break; }
            grown = realloc(all, total + n);
            if (!grown) { free(piece); why = "out of memory"; break; }
            all = grown; memcpy(all + total, piece, n); total += n; free(piece);
        }
        if (!why && total != want) why = "its chunks do not add up to its size";
    }
    cJSON_Delete(ix);
    if (!why && !(batch = node_decode_batch(node, h, all, total))) why = "it does not open with this database's keys";
    free(all);
    if (!why && !batch->bPages) why = "it is not a page image";
    if (!why) {
        HpplitePageBatch *pb = hpplite_pagebatch_decode(batch->pPagesRaw, batch->nPagesRaw);
        if (!pb || pb->nPages != pb->commitSize || pb->bMore || memcmp(pb->root, root, 32) != 0) why = "its image is not the state its index names";
        if (pb) hpplite_pagebatch_free(pb);
    }
    if (why) {
        if (batch) hpplite_batch_free(batch);
        fprintf(stderr, "hpplite: the fleet's snapshot at height %llu is not used (%s)\n", (unsigned long long)h, why);
        return 0;
    }
    batch->height = h;
    node->snapApply = 1; memcpy(node->snapChain, chain, 32); memcpy(node->snapBlock, block, 32);
    if (node_apply_pages(node, batch) != SQLITE_OK) {
        node->snapApply = 0;
        hpplite_batch_free(batch);
        return -1;
    }
    node->snapApply = 0;
    hpplite_batch_free(batch);
    node->snapHeight = h;
    node->snapAt = fv->snapAt;
    fprintf(stderr, "hpplite: started from the fleet's snapshot at height %llu (signed by this database's key, its root matched); replaying from %llu\n",
            (unsigned long long)h, (unsigned long long)(h + 1));
    (void)ctx;
    return 1;
}

/* The snapshot this anchor's manifest names (#221): a new one when snapshot= says the last is old
** enough, else the latest an earlier anchor named (read once from that anchor's manifest) */
static void node_anchor_snapshot(HppliteNode *node, cJSON *m, uint64_t lastBatch, uint64_t to, const unsigned char root[32]) {
    cJSON *sn;
    if (!node->snapLooked) {
        cJSON *prev = NULL;
        if (lastBatch) {
            long st;
            if (node->store) hpplite_store_take_status(node->store);
            prev = node_range_manifest(node, lastBatch);
            st = node->store ? hpplite_store_take_status(node->store) : -1;
            if (!prev && (st == 0 || st >= 500)) {
                /* the store did not answer (#236): nothing is decided - taking "no snapshot known" would make a
                ** fresh one at the next anchor and drop the list; the next anchor reads again, this one names none */
                fprintf(stderr, "hpplite: snapshot: the previous anchor's manifest could not be read (%s); nothing decided until it can\n", hpplite_store_error(node->store));
                return;
            }
        }
        if (prev && node_manifest_snapshot(prev, &node->snapHeight, node->snapIndex, &node->snapAt, node->snapIndexSha) != 0) node->snapHeight = 0;
        if (prev && node->snapHeight) {
            /* keep= (#223): the older snapshots still kept - this file's list (#257), below the oldest the chain's
            ** manifest names nothing is kept; a fresh file (a sequencer rebuilt, #167) knows the oldest from the manifest
            ** and nothing between it and the latest: those go when the latest is pruned */
            uint64_t eh = 0; int64_t eat = 0; char eIndex[64]; unsigned char eSha[32];
            int named = node_manifest_snapshot_key(prev, "earliest", &eh, eIndex, &eat, eSha) == 0 && eh < node->snapHeight;
            node_snapshots_load(node, named ? eh : node->snapHeight);
            if (named && node->nEarlier == 0) {
                node_earlier_push(node, eh, eIndex, eSha, eat);
                fprintf(stderr, "hpplite: snapshots: this file's bookkeeping has none; the anchor's manifest names the oldest kept (%llu) and the latest (%llu) - "
                                "any between them are kept at the store until the latest is pruned\n", (unsigned long long)eh, (unsigned long long)node->snapHeight);
            }
        }
        cJSON_Delete(prev);
        node->snapLooked = 1;
    }
    if (node->config.snapshotIntervalMs > 0
        && (node->snapHeight == 0 || ((int64_t)time(NULL) - node->snapAt) * 1000 >= (int64_t)node->config.snapshotIntervalMs)) {
        uint64_t ph = node->snapHeight;
        char pIndex[64];
        unsigned char pSha[32];
        int64_t pAt = node->snapAt;
        int made;
        snprintf(pIndex, sizeof pIndex, "%s", node->snapIndex);
        memcpy(pSha, node->snapIndexSha, 32);
        if (node->spare.height == to && memcmp(node->spareRoot, root, 32) == 0) {
            /* the one made for the anchor the chain refused (#237): the same height and root, its bytes at the store */
            node->snapHeight = node->spare.height;
            snprintf(node->snapIndex, sizeof node->snapIndex, "%s", node->spare.index);
            memcpy(node->snapIndexSha, node->spare.sha, 32);
            memcpy(node->snapRoot, root, 32);
            node->snapAt = node->spare.at;
            made = 0;
        } else made = node_make_snapshot(node, to, root);
        node->spare.height = 0;
        if (made == 0 && ph && node->config.keepSeconds > 0)
            node_earlier_push(node, ph, pIndex, pSha, pAt);    /* keep= (#223): the one it replaces stays kept until a prune passes it */
    }
    if (node->snapHeight == 0) return;
    node_prune_plan(node);                          /* keep= (#223): decided before the manifest, so it names only what is kept; sent once the chain accepts (#238) */
    sn = cJSON_AddObjectToObject(m, "snapshot");
    cJSON_AddNumberToObject(sn, "height", (double)node->snapHeight);
    cJSON_AddStringToObject(sn, "index", node->snapIndex);
    cJSON_AddNumberToObject(sn, "at", (double)node->snapAt);
    { char hx[67]; hex32(node->snapIndexSha, hx); cJSON_AddStringToObject(sn, "sha256", hx); }
    if (node->config.keepSeconds > 0 && node->nEarlier > 0) {
        /* the oldest snapshot still kept (#257): where a verifier starts when batch 1 is pruned (#224); the ones
        ** between it and the latest are the sequencer's own bookkeeping */
        cJSON *e = cJSON_AddObjectToObject(m, "earliest");
        char hx[67];
        cJSON_AddNumberToObject(e, "height", (double)node->aEarlier[0].height);
        cJSON_AddStringToObject(e, "index", node->aEarlier[0].index);
        cJSON_AddNumberToObject(e, "at", (double)node->aEarlier[0].at);
        hex32(node->aEarlier[0].sha, hx); cJSON_AddStringToObject(e, "sha256", hx);
    }
}

/*
** keep= (#223): at an anchor, the history behind the newest snapshot made at
** least `keep` ago is let go - the sequencer signs a Prune at its height, the
** store deletes the bucket's batches and older snapshots below it. Decided
** before the manifest is written, so it names only the snapshots kept; sent
** once the chain has accepted the anchor (#238: a refused anchor leaves the
** store as the manifest on chain describes it, and node_anchor restores the
** list). Only a snapshot an earlier anchor named qualifies (one made now is
** not that old). The state stays provable from that snapshot on; the
** commitments stay on chain. A Prune the store did not take is planned again
** at the next anchor (the list is already without what it lets go).
*/
static void node_prune_plan(HppliteNode *node) {
    int64_t cutoff;
    uint64_t p = 0;
    int i, k;
    node->prunePlanned = 0;
    if (node->config.keepSeconds <= 0 || !node->store || !node->snapHeight) return;
    cutoff = (int64_t)time(NULL) - node->config.keepSeconds;
    for (i = 0; i < node->nEarlier; i++) if (node->aEarlier[i].at <= cutoff && node->aEarlier[i].height > p) p = node->aEarlier[i].height;
    if (node->snapAt <= cutoff && node->snapHeight > p) p = node->snapHeight;
    if (p <= node->prunedHeight) return;
    node->prunePlanned = p;
    for (i = 0, k = 0; i < node->nEarlier; i++) if (node->aEarlier[i].height >= p) node->aEarlier[k++] = node->aEarlier[i];
    node->nEarlier = k;
}

static void node_prune_send(HppliteNode *node) {
    uint64_t p = node->prunePlanned;
    node->prunePlanned = 0;
    if (!p || !node->store) return;
    if (hpplite_store_prune(node->store, p) != 0) {
        fprintf(stderr, "hpplite: keep=: the store did not prune below the snapshot at %llu: %s\n", (unsigned long long)p, hpplite_store_error(node->store));
        return;
    }
    node->prunedHeight = p;
}

/* bytes the store holds under zRef, checked against a sha-256 from an authenticated source (#222) */
static uint8_t *node_store_get_checked(HppliteNode *node, const char *zRef, const unsigned char want[32], size_t *pLen) {
    unsigned char got[32];
    uint8_t *data = hpplite_store_get(node->store, zRef, pLen);
    if (!data) return NULL;
    hpplite_store_sha256(data, *pLen, got);
    if (memcmp(got, want, 32) != 0) { free(data); *pLen = 0; return NULL; }
    return data;
}

/*
** A new replica starts from the latest snapshot (#222): the one the last
** anchor's manifest names, its index and chunks checked against the hashes
** that manifest and index carry, its root and chain hash against the range
** commitment ending at its height on the chain, its image's root against
** that root before anything is written; then written as the file, the
** bookkeeping at that height, and the sync goes on from the next batch.
** 1 started there; 0 not (from batch 1; why said when a snapshot was
** refused); -1 the file could not be written.
*/
static int node_start_from_snapshot_at(HppliteNode *node, uint64_t lastBatch, int earliest) {
    HppliteCtx *ctx = node->ctx;
    cJSON *m, *ix = NULL, *chunks = NULL, *c;
    uint64_t h = 0;
    int64_t at = 0;
    char zIndex[64];
    uint8_t *text = NULL, *all = NULL;
    size_t n = 0, total = 0, want = 0;
    unsigned char root[32], chain[32], block[32], ixSha[32];
    HppliteRange r;
    HppliteBatch *batch = NULL;
    const char *why = NULL;
    node->snapshotPastStop = 0;
    m = node_range_manifest(node, lastBatch);
    if (!m || node_manifest_snapshot(m, &h, zIndex, &at, ixSha) != 0 || h < 2) { cJSON_Delete(m); return 0; }
    if (earliest) {
        /* the oldest snapshot still kept (keep=, #223; #224, "earliest" #257) - the most history verified */
        uint64_t eh = 0; int64_t eat = 0; char eIndex[64]; unsigned char eSha[32];
        if (node_manifest_snapshot_key(m, "earliest", &eh, eIndex, &eat, eSha) == 0 && eh >= 2 && eh < h) {
            h = eh; at = eat; snprintf(zIndex, sizeof zIndex, "%s", eIndex); memcpy(ixSha, eSha, 32);
        }
    }
    cJSON_Delete(m);
    if (node->syncTo && h > node->syncTo) { node->snapshotPastStop = h; return 0; }                         /* a stop before it (#206): from batch 1 */
    if (node->syncAtMs && (uint64_t)at * 1000 > node->syncAtMs) { node->snapshotPastStop = h; return 0; }  /* ...or a time before it was made (#205) */
    memset(&r, 0, sizeof r);
    ix = node_snapshot_index(node, zIndex, ixSha);
    if (!ix) why = "its index is not at the store, or not the one the anchor named";
    else {
        const cJSON *jh = cJSON_GetObjectItemCaseSensitive(ix, "height"), *jr = cJSON_GetObjectItemCaseSensitive(ix, "root"),
                    *jc = cJSON_GetObjectItemCaseSensitive(ix, "chain"), *jb = cJSON_GetObjectItemCaseSensitive(ix, "block"),
                    *jn = cJSON_GetObjectItemCaseSensitive(ix, "bytes");
        chunks = cJSON_GetObjectItemCaseSensitive(ix, "chunks");
        if (!cJSON_IsNumber(jh) || (uint64_t)jh->valuedouble != h || !cJSON_IsString(jr) || !cJSON_IsString(jc) || !cJSON_IsString(jb) || !cJSON_IsNumber(jn) || !cJSON_IsArray(chunks)
            || !json_hex32(jr, root) || !json_hex32(jc, chain) || !json_hex32(jb, block))
            why = "its index is malformed";
        else if (hpplite_l1_get_range(node->l1, h, &r) != 0 || r.toHeight != h) why = "no range commitment ends at its height";
        else if (memcmp(r.stateRoot, root, 32) != 0) why = "its root is not the one the chain anchored there";
        else if (memcmp(r.chainHashAfter, chain, 32) != 0) why = "its chain hash is not the one the chain anchored there";
        if (cJSON_IsNumber(jn)) want = (size_t)jn->valuedouble;
    }
    hpplite_l1_range_clear(&r);
    if (!why) {
        cJSON_ArrayForEach(c, chunks) {
            const cJSON *ref = cJSON_GetObjectItemCaseSensitive(c, "ref"), *sha = cJSON_GetObjectItemCaseSensitive(c, "sha256");
            unsigned char d[32];
            uint8_t *part = NULL, *grown;
            if (cJSON_IsString(ref) && json_hex32(sha, d))
                part = node_store_get_checked(node, ref->valuestring, d, &n);
            if (!part) { why = "a chunk is not at the store, or not the one the index names"; break; }
            grown = realloc(all, total + n);
            if (!grown) { free(part); why = "out of memory"; break; }
            all = grown; memcpy(all + total, part, n); total += n; free(part);
        }
        if (!why && total != want) why = "its chunks do not add up to its size";
    }
    if (!why && !(batch = node_decode_batch(node, h, all, total))) why = "it does not open with this node's keys";
    if (!why && !batch->bPages) why = "it is not a page image";
    if (!why) {
        HpplitePageBatch *pb = hpplite_pagebatch_decode(batch->pPagesRaw, batch->nPagesRaw);
        if (!pb || pb->nPages != pb->commitSize || pb->bMore || memcmp(pb->root, root, 32) != 0) why = "its image is not the anchored state";
        if (pb) hpplite_pagebatch_free(pb);
    }
    free(all);
    cJSON_Delete(ix);
    if (why) {
        if (batch) hpplite_batch_free(batch);
        fprintf(stderr, "hpplite: the snapshot at height %llu is not used (%s): replaying from batch 1\n", (unsigned long long)h, why);
        return 0;
    }
    batch->height = h;
    node->snapApply = 1; memcpy(node->snapChain, chain, 32); memcpy(node->snapBlock, block, 32);
    if (node_apply_pages(node, batch) != SQLITE_OK) {
        node->snapApply = 0;
        hpplite_batch_free(batch);
        return -1;
    }
    node->snapApply = 0;
    hpplite_batch_free(batch);
    hpplite_set_checkpoint(ctx, h, root);           /* the root the chain anchored there: as a checkpoint */
    node->lastCheckpointHeight = h;
    node->snapHeight = h;
    fprintf(stderr, "hpplite: started from the snapshot at height %llu (anchored root matched); replaying from %llu\n", (unsigned long long)h, (unsigned long long)(h + 1));
    return 1;
}

static int node_start_from_snapshot(HppliteNode *node, uint64_t lastBatch, int earliest) {
    int rc = node_start_from_snapshot_at(node, lastBatch, earliest);
    if (rc == 0 && earliest) rc = node_start_from_snapshot_at(node, lastBatch, 0);   /* the oldest was not usable: the latest (#224) */
    return rc;
}

/*
** The anchor: every row of _hpplite_unanchored above the chain's height,
** in one manifest at the store and one commitment on the chain. 1 anchored,
** 0 nothing to anchor, -1 the chain or the store refused (the rows stay;
** the next tick tries again).
*/
static int node_anchor_once(HppliteNode *node) {
    uint64_t lastBatch = 0, total = 0, from = 0, to = 0, expect;
    unsigned char latest[32], chainAfter[32], root[32], digest[32], payTo[20];
    uint8_t compact[HPPLITE_STATEMENT_SIZE];   /* the /2 bucket statement */
    cJSON *m = NULL, *batches;
    sqlite3_stmt *q = NULL;
    char *text = NULL, *zRef = NULL, *txHash = NULL, cid[64], hx[67];
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
    if (!hasRoot) hpplite_get_state_root(ctx, root);     /* the root after the last finalized batch */
    cJSON_AddNumberToObject(m, "from", (double)from);
    cJSON_AddNumberToObject(m, "to", (double)to);
    hex32(chainAfter, hx); cJSON_AddStringToObject(m, "chain_after", hx);
    hex32(root, hx); cJSON_AddStringToObject(m, "root", hx);
    node_anchor_snapshot(node, m, lastBatch, to, root);
    text = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    if (!text) return -1;
    hpplite_store_sha256((const uint8_t *)text, strlen(text), digest);
    hpplite_store_cid_from_sha256(digest, cid);
    node_store_bucket(node);
    zRef = hpplite_store_put_at(node->store, to, (const uint8_t *)text, strlen(text));
    node->storeBalanceOk = 0;
    if (!zRef) {
        hpplite_error(ctx, "anchor: the store did not take the manifest of batches %llu-%llu: %s", (unsigned long long)from, (unsigned long long)to, hpplite_store_error(node->store));
        free(text); return -1;
    }
    if (strcmp(zRef, cid) != 0) {
        /* a replica finds the manifest by the digest the chain carries: the store must name it so */
        hpplite_error(ctx, "anchor: the store names the manifest %s, not its digest's CID %s: replicas could not find it; not anchored", zRef, cid);
        free(text); sqlite3_free(zRef); return -1;
    }
    nCompact = hpplite_store_is_bucket(node->store) ? hpplite_store_statement(node->store, compact) : -1;   /* /2: the bucket statement (#214); a store without buckets commits none */
    memset(&r, 0, sizeof r);
    r.fromHeight = from; r.toHeight = to;
    memcpy(r.chainHashAfter, chainAfter, 32);
    memcpy(r.stateRoot, root, 32);
    memcpy(r.manifestDigest, digest, 32);
    if (nCompact > 0) { r.receipt = compact; r.nReceipt = (size_t)nCompact; }
    if (!node_ensure_lease(node)) { free(text); sqlite3_free(zRef); return -1; }
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
        const char *zRelay = hpplite_l1_relay_error(node->l1);    /* why the relay did not take it (#126) */
        hpplite_error(ctx, "anchor: the chain did not take the commitment of batches %llu-%llu%s%s%s; tried again at the next anchor",
                      (unsigned long long)from, (unsigned long long)to, zRelay ? " (relay: " : "", zRelay ? zRelay : "", zRelay ? ")" : "");
        rc = -1;
    }
    free(text); sqlite3_free(zRef);
    return rc;
}

static int node_anchor(HppliteNode *node) {
    uint64_t was = node->snapHeight;
    char wasIndex[64];
    unsigned char wasSha[32];
    int64_t wasAt = node->snapAt;
    int rc, wasN = node->nEarlier, wasLooked = node->snapLooked;
    HppliteSnapRef *wasEarlier = wasN ? malloc((size_t)wasN * sizeof *wasEarlier) : NULL;
    snprintf(wasIndex, sizeof wasIndex, "%s", node->snapIndex);
    memcpy(wasSha, node->snapIndexSha, 32);
    if (wasEarlier) memcpy(wasEarlier, node->aEarlier, (size_t)wasN * sizeof *wasEarlier);
    rc = node_anchor_once(node);
    if (rc == 1) {
        node_snapshots_save(node);                  /* the list the manifest on chain describes (#257) */
        node_prune_send(node);                      /* keep= (#223, #238): the chain has the anchor; now the store lets go */
        free(wasEarlier);
    } else {
        /* the list as it was (a planned prune filtered it, #238; a snapshot made now was pushed to it) */
        node->prunePlanned = 0;
        if (wasN) { memcpy(node->aEarlier, wasEarlier, (size_t)wasN * sizeof *wasEarlier); }
        node->nEarlier = wasN;
        free(wasEarlier);
        if (node->snapHeight != was) {
            /* a snapshot made for a range the chain did not take: no commitment ends at its height for a
            ** replica to check it against; the one before stands. Kept aside (#237): the retry, anchoring the
            ** same height and root, names it instead of uploading the image again */
            node->spare.height = node->snapHeight;
            snprintf(node->spare.index, sizeof node->spare.index, "%s", node->snapIndex);
            memcpy(node->spare.sha, node->snapIndexSha, 32);
            node->spare.at = node->snapAt;
            memcpy(node->spareRoot, node->snapRoot, 32);
            node->snapHeight = was; snprintf(node->snapIndex, sizeof node->snapIndex, "%s", wasIndex); node->snapAt = wasAt; memcpy(node->snapIndexSha, wasSha, 32);
        }
        /* the first anchor of a process reads the previous manifest inside node_anchor_once: what was saved above is
        ** "nothing known", and a restart whose first anchor the chain refused forgot the snapshot kept (#259) - the
        ** retry reads the manifest again */
        if (!wasLooked) node->snapLooked = 0;
    }
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
    char of[43] = "", da[43];
    hpplite_l1_get_addresses(l1, of, da);
    if (!node->manifest || node->manifestFrom != r.fromHeight || node->manifestTo != r.toHeight || strcmp(node->manifestOf, of) != 0) {
        /* the manifest, by its digest's CID; it must be the bytes the chain named, and its batches must fold to the anchored chain hash.
        ** Keyed by the rollup too (#234): the lookup by name reads batch 1 of each of the wallet's rollups through here, and two
        ** rollups' first ranges have the same bounds more often than not */
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
        if (!arr || !cJSON_IsArray(arr) || !json_hex32(cb, chain)) {
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
            if (!cJSON_IsNumber(hh) || (uint64_t)hh->valuedouble != h || !json_hex32(hs, hash)) {
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
        snprintf(node->manifestOf, sizeof node->manifestOf, "%s", of);
    }
    node->anchoredHeight = r.toHeight > node->anchoredHeight ? r.toHeight : node->anchoredHeight;
    b = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(node->manifest, "batches"), (int)(height - r.fromHeight));
    e = b ? cJSON_GetObjectItemCaseSensitive(b, "ref") : NULL;
    zRef = cJSON_IsString(e) ? e->valuestring : NULL;
    e = b ? cJSON_GetObjectItemCaseSensitive(b, "hash") : NULL;
    if (!zRef || !json_hex32(e, want)) { hpplite_l1_range_clear(&r); return NULL; }
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

/* A connection that did not come at open - the lookup by name could not tell this database's rollup from
** the others' while the store did not answer (#229), or the RPC was down - is tried again at a flush, at most
** every ten seconds, as a key given after open tries it (hpplite_node_set_key). Nothing is finalized meanwhile */
static void node_retry_l1(HppliteNode *node) {
    int64_t now = node_current_time_ms();
    if (node->l1 || !node->wantsL1 || node->follower || !(node->config.hasPrivkey || node->hasSigner)) return;
    if (node->l1RetryAt && now < node->l1RetryAt) return;
    node->l1RetryAt = now + 10000;
    node_auto_connect_l1(node, &node->config);
    if (node->l1) node->warnedNoL1 = 0;
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
    node_retry_l1(node);
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
    if (node->cipher && node->config.hasPrivkey && node_recipients(node, NULL) > 1) {
        /* readers besides the wallet (#15) - the owner (#209), the keys granted: sealed to every one of them and
        ** to the wallet, under the series' data key, as the row flush does (#150) */
        uint8_t *aTo = NULL;
        int nTo = node_recipients(node, &aTo);
        if (aTo && nTo > 1) {
            if (!node->seriesDekValid) { hpplite_random_bytes(node->seriesDek, 32); node->seriesDekValid = 1; }
            sealed = hpplite_envelope_seal_pk(node->cipher, node->seriesDek, aTo, nTo, height, encoding, payload, payloadLen, &payloadLen);
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
        if (node->store && node->config.hasFleet) {
            /* fleet= (#316 c): the batch and its head, signed by this database's key, in one write - the rolling hash
            ** after it, the page root, the bytes' sha-256 and the lease's epoch; the fleet's anchor commits to it */
            unsigned char k[32], after[32];
            uint64_t chainId = hpplite_l1_chain_id(node->l1);
            if (!chainId) chainId = hpplite_l1_default_chain_id();
            hpplite_keccak256(payload, payloadLen, k);
            hpplite_chain_hash_next(ctx->chainHash, height, k, after);
            if (hpplite_store_put_fleet_name(node->store, node->config.fleet, chainId, node->config.dbId, hpplite_l1_fleet_admitted_at(node->l1), height, node->instanceId, after, root,
                                             hpplite_l1_fleet_epoch(node->l1), payload, payloadLen) == 0) {
                node->warnedStoreDown = 0;
                node->warnedFunder = 0;
                storedOnly = 1;
            } else {
                snprintf(zStoreWhy, sizeof zStoreWhy, "the store did not take it: %s", hpplite_store_error(node->store));
                /* not the lease's epoch (#316 d): the record is read again at the next look - moved past this run's epoch,
                ** the run is fenced; still ours (the store behind), the batch goes again at the next flush */
                if (strstr(hpplite_store_error(node->store), "not the lease's epoch")) { node->hasLease = 0; node->leaseRecheckAt = 0; }
            }
            txHash = NULL;
        } else if (node->store) {
            /* the bytes to the store, the commitment to the chain (#11), as the row flush does */
            char *zRef = NULL;
            int named = 0;
            node_store_bucket(node);
            /* one write per batch (#296): a bucket store under ranges takes the batch by its name, which carries the
            ** bytes, and holds them under their CID - the ref, computed here. Without a key to sign the name, the
            ** content put and a name after it, as before */
            if (node_ranges(node) && hpplite_store_is_bucket(node->store)) named = node_name_batch(node, height, payload, payloadLen);
            if (named == 1) {
                uint8_t sha[32];
                char cid[64];
                hpplite_store_sha256(payload, payloadLen, sha);
                hpplite_store_cid_from_sha256(sha, cid);
                zRef = sqlite3_mprintf("%s", cid);
            } else if (named == 0) zRef = hpplite_store_put_at(node->store, height, payload, payloadLen);
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
                unsigned char h[32];
                node->warnedStoreDown = 0;
                node->warnedFunder = 0;
                hpplite_keccak256(payload, payloadLen, h);
                if (node_ranges(node)) {
                    node_anchor_row(node, h, zRef, NULL, 0);   /* the chain's commitment comes with the anchor (#164); the store's word is its bucket statement there */
                    if (named == 0 && node_name_batch(node, height, payload, payloadLen) < 0 && !node->warnedNames) {
                        fprintf(stderr, "hpplite: the store did not take batch %llu under its name (%s): readers find it only through the anchor\n",
                                (unsigned long long)height, hpplite_store_error(node->store));
                        node->warnedNames = 1;
                    }
                    txHash = NULL;
                    storedOnly = 1;
                } else {
                    txHash = hpplite_l1_commit_batch(node->l1, node->instanceId, height, h, zRef, NULL, 0);
                    if (!txHash) hpplite_error(ctx, "the chain did not take the commitment of page batch %llu (ref %s)", (unsigned long long)height, zRef);
                }
            }
            sqlite3_free(zRef);
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
        if (zStoreWhy[0] && node->store && hpplite_store_retry_after(node->store) * 1000LL > node->retryAt - node_current_time_ms())
            node->retryAt = node_current_time_ms() + hpplite_store_retry_after(node->store) * 1000LL;     /* a 429's Retry-After (somaro#48) */
        if (zStoreWhy[0]) {
            /* an external funder's refusal (#272) and a bucket's daily cap (#288) are the funder's to lift: said once per episode, not at every flush */
            int funder = node->store && hpplite_store_waiting_on_funder(node->store);
            if (!(funder && node->warnedFunder)) hpplite_error(ctx, "page batch %llu kept pending: %s", (unsigned long long)height, zStoreWhy);
            node->warnedFunder = funder;
        } else hpplite_error(ctx, "page batch %llu not confirmed on L1, kept pending", (unsigned long long)height);
        sqlite3_free(bytes); free(packed); free(sealed); free(framed);
        *pMore = 0;
        return 0;
    }
    ctx->pAnchorRow = storedOnly && !node->config.hasFleet ? &node->anchorRow : NULL;   /* a fleet's anchor is the fleet's */
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
        if (node->config.hasFleet && storedOnly && !*pMore) node_fleet_snapshot(node, height, root);   /* #314 */
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
    *pMore = 0;
    /* The DA limit applies to the posted bytes: leave room for the envelope */
    size_t maxBytes = node->config.maxBatchBytes;
    if (node->cipher && maxBytes > 0) {
        size_t overhead;
        if (!node->readersLoaded) node_load_readers(node);         /* the readers decide the envelope, the envelope the budget (#15) */
        overhead = node_envelope_overhead(node);
        maxBytes = maxBytes > overhead ? maxBytes - overhead : 1;
    }
    node_retry_l1(node);
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
                    (long long)hpplite_node_pending_count(node));
            node->warnedNoChain = 1;
        }
        *pMore = 0;
        return 0;
    }

    return node_flush_pages(node, pMore, maxBytes);
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
    hpplite_node_spend_check(node);
    if (mutex) pthread_mutex_unlock(mutex);
    return height;
}

void hpplite_node_spend_check(HppliteNode *node) {
    time_t now = time(NULL);
    char w[200] = "";
    int hours = node && node->config.lowBalanceHours ? node->config.lowBalanceHours : 72;
    double wei = -1;
    uint64_t span = 0;
    /* a rate from five minutes of history at least (a burst at start is not a rate), checked every 30 s;
    ** HPPLITE_SPEND_CHECK_SECONDS sets both (tests) */
    const char *zTest = getenv("HPPLITE_SPEND_CHECK_SECONDS");
    long every = zTest && atol(zTest) > 0 ? atol(zTest) : 30;
    uint64_t minSpan = zTest && atol(zTest) > 0 ? (uint64_t)atol(zTest) : 300;
    if (!node || !node->l1 || !node->spend || now - node->spendCheckedAt < every) return;
    node->spendCheckedAt = now;
    if (hpplite_l1_eth_balance(node->l1, &wei) == 0) hpplite_spend_eth_sample(node->spend, wei);
    else wei = -1;
    if (hours > 0) {
        uint64_t spent = hpplite_spend_since(node->spend, 86400, &span);
        uint8_t b[32];
        static const uint8_t zero24[24];
        if (spent && span >= minSpan && hpplite_node_store_balance(node, b) == 0 && memcmp(b, zero24, 24) == 0) {
            uint64_t bal = 0;
            for (int i = 24; i < 32; i++) bal = (bal << 8) | b[i];
            double perHour = (double)spent * 3600.0 / (double)span, left = (double)bal / perHour;
            if (left < hours) snprintf(w, sizeof w, "USDC.e for about %.0f h at the recent rate (%llu left, %.0f an hour)", left, (unsigned long long)bal, perHour);
        }
        double ethSpent = hpplite_spend_eth_spent(node->spend, &span);
        if (ethSpent > 0 && span >= minSpan && wei >= 0) {
            double perHour = ethSpent * 3600.0 / (double)span, left = wei / perHour;
            size_t n = strlen(w);
            if (left < hours) snprintf(w + n, sizeof w - n, "%sETH for about %.0f h at the recent rate (%.6f ETH left)", n ? "; " : "", left, wei / 1e18);
        }
    }
    if (w[0] && strcmp(w, node->spendWarning) != 0 && !node->spendWarning[0]) fprintf(stderr, "hpplite: warning: %s (HPPLITE_LOW_BALANCE=%d)\n", w, hours);
    else if (!w[0] && node->spendWarning[0]) fprintf(stderr, "hpplite: the balance warning cleared\n");
    snprintf(node->spendWarning, sizeof node->spendWarning, "%s", w);
}

/*
** Bring the local database up to date with L1.
*/
/*
** Recovery (#167): the batches the store holds above the chain's height,
** read by their names, applied, and taken as this node's own, so a sequencer
** that starts where the last one died (another machine, a fresh file) loses
** nothing that reached the store. A name is taken only when its signature
** recovers to this node's wallet (the one sequencer key) and the batch
** applies on top of what is already here; the walk stops at the first height
** without one. Each recovered batch gets its _hpplite_unanchored row (the
** content reference, the chain hash before and after), so the next anchor
** names it. Once per node: a restart on the same file starts past them.
*/
static int node_soft_walk(HppliteNode *node) {
    char c[43], d[43], name[80], *p, cid[64];
    const char *zDa;
    unsigned char da[20], me[20], who[20], inst[32], k[32], before[32], after[32], digest[32];
    uint8_t sig[65];
    uint64_t first = 0, last = 0;
    if (node->softWalked || !node_ranges(node) || node->follower || !node->ctx) return 0;
    node->softWalked = 1;
    if (hpplite_l1_get_addresses(node->l1, c, d) != 0 || hpplite_l1_get_my_address(node->l1, me) != 0) return 0;
    zDa = d[0] ? d : c;
    if (strlen(zDa) != 42 || hpplite_hex_to_bytes(zDa + 2, da, 20) != 20) return 0;
    for (;;) {
        uint64_t h = node->ctx->blockHeight, chainId;
        size_t n = 0;
        uint8_t *data;
        HppliteBatch *batch;
        int rc, saved;
        sqlite3_stmt *q = NULL;
        snprintf(name, sizeof name, "%.40s-%llu", zDa + 2, (unsigned long long)h);
        for (p = name; *p && *p != '-'; p++) if (*p >= 'A' && *p <= 'F') *p = (char)(*p - 'A' + 'a');
        data = hpplite_store_get_named(node->store, name, &n, sig, inst);
        if (!data) break;                                            /* the first height the store has no name for: the end */
        chainId = hpplite_l1_chain_id(node->l1);
        if (!chainId) chainId = hpplite_l1_default_chain_id();
        if (hpplite_store_batch_signer(da, chainId, inst, h, data, n, sig, who) != 0 || memcmp(who, me, 20) != 0) {
            char zw[41];
            hpplite_bytes_to_hex(who, 20, zw);
            hpplite_error(node->ctx, "batch %llu at the store under its name is signed by 0x%s, not this wallet: not recovered, nothing past it either", (unsigned long long)h, zw);
            free(data);
            break;
        }
        batch = node_decode_batch(node, h, data, n);
        if (!batch) { hpplite_error(node->ctx, "batch %llu at the store could not be read; recovery stops before it", (unsigned long long)h); free(data); break; }
        batch->pPosted = data; batch->nPosted = n; batch->bOwnsPosted = 1;
        memcpy(before, node->ctx->chainHash, 32);
        rc = (node->pm && !node->config.verifyOnly && node->lockFd >= 0) ? node_adopt_pages(node, batch) : node_apply_pages(node, batch);
        if (rc != SQLITE_OK || node->ctx->blockHeight != h + 1) {
            hpplite_error(node->ctx, "batch %llu at the store does not apply on this history (rc=%d); recovery stops before it", (unsigned long long)h, rc);
            hpplite_batch_free(batch);
            break;
        }
        hpplite_keccak256(data, n, k);
        hpplite_chain_hash_next(before, h, k, after);
        hpplite_store_sha256(data, n, digest);
        hpplite_store_cid_from_sha256(digest, cid);
        rc = hppliteBkBegin(node->ctx, 1);
        if (rc == SQLITE_OK) {
            saved = hppliteBkEnter(node->ctx);
            rc = sqlite3_prepare_v2(node->ctx->bkDb, "INSERT OR REPLACE INTO _hpplite_unanchored(height, hash, ref, receipt, chain_before, chain_after, root) VALUES(?1, ?2, ?3, NULL, ?4, ?5, NULL)", -1, &q, NULL);
            if (rc == SQLITE_OK) {
                sqlite3_bind_int64(q, 1, (sqlite3_int64)h);
                sqlite3_bind_blob(q, 2, k, 32, SQLITE_STATIC);
                sqlite3_bind_text(q, 3, cid, -1, SQLITE_STATIC);
                sqlite3_bind_blob(q, 4, before, 32, SQLITE_STATIC);
                sqlite3_bind_blob(q, 5, after, 32, SQLITE_STATIC);
                rc = sqlite3_step(q) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
            }
            sqlite3_finalize(q);
            hppliteBkLeave(node->ctx, saved);
            rc = hppliteBkEnd(node->ctx, rc);
        }
        hpplite_batch_free(batch);
        if (rc != SQLITE_OK) { hpplite_error(node->ctx, "batch %llu recovered but its row for the anchor could not be written (rc=%d)", (unsigned long long)h, rc); break; }
        if (!first) first = h;
        last = h;
        node->batchesProduced++;
        node->batchesSinceCheckpoint++;
    }
    if (first) {
        fprintf(stderr, "hpplite: recovered batches %llu-%llu from the store (signed by this wallet, not yet anchored): the next anchor takes them\n",
                (unsigned long long)first, (unsigned long long)last);
        node->anchorAt = 0;                                          /* the timer anchors them soon */
    }
    return first ? (int)(last - first + 1) : 0;
}

/*
** The soft tier for a verifier (#166): the batches the store holds under the
** rollup's names past the anchored height, applied as they arrive, so a
** replica follows within a flush interval instead of an anchor interval.
** Each one must be signed by the coordinator's sequencerWallet() and apply on
** this history; the chain hash after each is kept, and the next range
** commitment that lands inside them must name the same one - else the store
** served batches the chain did not commit (equivocation, a stale store) and
** this replica's soft state is not the rollup's.
*/
static int node_soft_follows(HppliteNode *node) {
    /* not with a stop (#206, #205): --to and --at ask for the hard history as of a point; the soft tier is past the anchor */
    return node && node->config.verifyOnly && !node->syncTo && !node->syncAtMs && node->store && node->l1 && !node->softStopped && hpplite_l1_has_ranges(node->l1);
}

static int node_soft_push(HppliteNode *node, uint64_t h, const unsigned char before[32], const unsigned char after[32]) {
    if (!node->softFirst) { node->softFirst = h; node->nSoft = 0; memcpy(node->softBase, before, 32); }
    if (node->nSoft == node->nSoftAlloc) {
        size_t n = node->nSoftAlloc ? node->nSoftAlloc * 2 : 64;
        unsigned char (*a)[32] = realloc(node->softChain, n * 32);
        if (!a) return -1;
        node->softChain = a;
        node->nSoftAlloc = n;
    }
    memcpy(node->softChain[node->nSoft++], after, 32);
    return 0;
}

static void node_soft_clear(HppliteNode *node) {
    node->softFirst = 0;
    node->nSoft = 0;
}

/* the name of height h at the store: "<da hex, lower case>-<h>" */
static int node_store_name(HppliteNode *node, uint64_t h, char name[80], unsigned char da[20]) {
    char c[43], d[43], *p;
    const char *zDa;
    if (hpplite_l1_get_addresses(node->l1, c, d) != 0) return -1;
    zDa = d[0] ? d : c;
    if (strlen(zDa) != 42 || hpplite_hex_to_bytes(zDa + 2, da, 20) != 20) return -1;
    snprintf(name, 80, "%.40s-%llu", zDa + 2, (unsigned long long)h);
    for (p = name; *p && *p != '-'; p++) if (*p >= 'A' && *p <= 'F') *p = (char)(*p - 'A' + 'a');
    return 0;
}

/* The chain hash the range commitment ending at `height` names (0 when none ends there; height 0: zero) */
static int node_range_chain(HppliteNode *node, uint64_t height, unsigned char out[32]) {
    HppliteRange r;
    int ok;
    if (height == 0) { memset(out, 0, 32); return 0; }
    memset(&r, 0, sizeof r);
    if (hpplite_l1_get_range(node->l1, height, &r) != 0) return -1;
    ok = r.toHeight == height;
    if (ok) memcpy(out, r.chainHashAfter, 32);
    hpplite_l1_range_clear(&r);
    return ok ? 0 : -1;
}

/* A soft state from before this process (the file is past the anchored height): its chain hashes again,
** from the store's names folded on the chain's hash at the anchored height; they must end at the file's */
static int node_soft_recount(HppliteNode *node, uint64_t anchored) {
    unsigned char chain[32], after[32], k[32], da[20], inst[32];
    uint8_t sig[65];
    char name[80];
    uint64_t h;
    if (node_range_chain(node, anchored, chain) != 0) return -1;
    node_soft_clear(node);
    for (h = anchored + 1; h < node->ctx->blockHeight; h++) {
        size_t n = 0;
        uint8_t *data;
        if (node_store_name(node, h, name, da) != 0 || !(data = hpplite_store_get_named(node->store, name, &n, sig, inst))) return -1;
        hpplite_keccak256(data, n, k);
        free(data);
        hpplite_chain_hash_next(chain, h, k, after);
        if (node_soft_push(node, h, chain, after) != 0) return -1;
        memcpy(chain, after, 32);
    }
    return memcmp(chain, node->ctx->chainHash, 32) == 0 ? 0 : -1;
}

/* A page-mode replica's file from another one, page by page (#166): the sequencer's bytes as they are -
** sqlite3_backup would move the schema cookie, and the next batch's diffs on page 1 would land on another
** page than the sequencer's. Page 1's change counter moves past this file's, so its readers notice. */
static int node_copy_pages_raw(HppliteNode *node, HppliteNode *from) {
    const char *zFile = sqlite3_db_filename(node->db, "main");
    sqlite3 *adb = NULL;
    sqlite3_file *pf = NULL, *pf2 = NULL;
    sqlite3_int64 size = 0, off;
    unsigned char *page = NULL, hdr[100];
    unsigned counter = 0;
    int ps, rc;
    if (!node->pm || !from->pm || (ps = hpplite_pagemode_page_size(from->pm)) <= 0) return SQLITE_MISUSE;
    hpplite_internal_open_begin();
    rc = sqlite3_open_v2(zFile, &adb, SQLITE_OPEN_READWRITE, NULL);
    hpplite_internal_open_end();
    if (rc != SQLITE_OK) { if (adb) sqlite3_close(adb); return rc; }
    sqlite3_busy_timeout(adb, 5000);
    rc = sqlite3_exec(adb, "BEGIN EXCLUSIVE", NULL, NULL, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_file_control(adb, "main", SQLITE_FCNTL_FILE_POINTER, &pf);
    if (rc == SQLITE_OK) rc = sqlite3_file_control(from->db, "main", SQLITE_FCNTL_FILE_POINTER, &pf2);
    if (rc == SQLITE_OK && (!pf || !pf->pMethods || !pf2 || !pf2->pMethods || !(page = sqlite3_malloc(ps)))) rc = SQLITE_ERROR;
    if (rc == SQLITE_OK) rc = pf2->pMethods->xFileSize(pf2, &size);
    if (rc == SQLITE_OK) {
        int r1 = pf->pMethods->xRead(pf, hdr, 100, 0);
        if (r1 == SQLITE_OK) counter = ((unsigned)hdr[24] << 24) | ((unsigned)hdr[25] << 16) | ((unsigned)hdr[26] << 8) | hdr[27];
    }
    for (off = 0; rc == SQLITE_OK && off < size; off += ps) {
        rc = pf2->pMethods->xRead(pf2, page, ps, off);
        if (rc == SQLITE_OK && off == 0) {
            counter++;
            page[24] = (unsigned char)(counter >> 24); page[25] = (unsigned char)(counter >> 16); page[26] = (unsigned char)(counter >> 8); page[27] = (unsigned char)counter;
            memcpy(page + 92, page + 24, 4);
            page[18] = page[19] = 1;
        }
        if (rc == SQLITE_OK) rc = pf->pMethods->xWrite(pf, page, ps, off);
    }
    if (rc == SQLITE_OK) rc = pf->pMethods->xTruncate(pf, size);
    if (rc == SQLITE_OK) rc = pf->pMethods->xSync(pf, SQLITE_SYNC_NORMAL);
    sqlite3_exec(adb, "ROLLBACK", NULL, NULL, NULL);             /* nothing of SQLite's to commit; the lock goes */
    sqlite3_close(adb);
    sqlite3_free(page);
    return rc;
}

/* After a disagreement (#166 step 3): this replica again, from the chain and the store. A second
** verifier builds it in <db>-rebuild.db with this node's key and chain connection; sqlite3_backup
** copies it into this connection's database - atomic for readers, the connections stay open - or, a
** page-mode file, its pages are copied as they are and the sidecar by backup; the bookkeeping is
** reloaded. One edge: a page-mode copy keeps the sequencer's schema cookie, so another connection
** holding a schema parsed at that same cookie number from the other history would not reparse it. 0, or -1 with this node as it was. */
static int node_soft_rebuild(HppliteNode *node) {
    HppliteNodeConfig cfg;
    HppliteNode *n2;
    char *zTmp;
    static const char *aSuffix[] = { "", "-wal", "-shm", "-hpplite.db", "-hpplite.db-wal", "-hpplite.db-shm", NULL };
    int rc = -1, saved, i;
    if (!node->config.verifyOnly || !node->config.dbPath || !node->db) return -1;
    zTmp = sqlite3_mprintf("%s-rebuild.db", node->config.dbPath);
    if (!zTmp) return -1;
    for (i = 0; aSuffix[i]; i++) { char *z = sqlite3_mprintf("%s%s", zTmp, aSuffix[i]); if (z) unlink(z); sqlite3_free(z); }
    cfg = node->config;
    cfg.dbPath = zTmp;
    cfg.verifyOnly = 1;
    n2 = hpplite_node_create(&cfg);
    if (n2) {
        if (node->hasViewKey) hpplite_node_set_view_key(n2, node->viewKey);
        else if (node->hasEncKey && !node->config.hasPrivkey) hpplite_node_set_batch_key(n2, node->encKey);
        hpplite_node_set_l1(n2, node->l1);
        n2->ownsL1 = 0;
        if (hpplite_node_sync_from_l1(n2, 1) >= 0 && n2->state != HPPLITE_STATE_ERROR) {
            sqlite3_backup *b;
            saved = hppliteInternalBegin(node->ctx);          /* HPPLite's own write: no capture, no commit-hook refusal */
            if (node->pm && n2->pm) rc = node_copy_pages_raw(node, n2) == SQLITE_OK ? 0 : -1;
            else {
                b = sqlite3_backup_init(node->db, "main", n2->db, "main");
                rc = b && sqlite3_backup_step(b, -1) == SQLITE_DONE ? 0 : -1;
                if (b) sqlite3_backup_finish(b);
            }
            if (rc == 0 && node->ctx->bkDb && node->ctx->bkDb != node->db && n2->ctx->bkDb && n2->ctx->bkDb != n2->db) {
                b = sqlite3_backup_init(node->ctx->bkDb, "main", n2->ctx->bkDb, "main");
                rc = b && sqlite3_backup_step(b, -1) == SQLITE_DONE ? 0 : -1;
                if (b) sqlite3_backup_finish(b);
            }
            hppliteInternalEnd(node->ctx, saved);
            if (rc == 0) {
                hppliteLoadMeta(node->ctx);
                if (node->pm && n2->pm) hpplite_pagemode_take_root(node->pm, n2->pm);   /* the file's page tree is the copy's */
                node_soft_clear(node);
                for (size_t k = 0; k < n2->nSoft; k++) node_soft_push(node, n2->softFirst + k, k ? n2->softChain[k - 1] : n2->softBase, n2->softChain[k]);
                node->softStopped = 0;
                node->state = HPPLITE_STATE_RUNNING;
                hpplite_error_clear(node->ctx);
                fprintf(stderr, "hpplite: rebuilt from the chain and the store: height %llu, soft to %llu\n",
                        (unsigned long long)(node->softFirst ? node->softFirst : node->ctx->blockHeight), (unsigned long long)node->ctx->blockHeight);
            }
        }
        n2->l1 = NULL;
        hpplite_node_destroy(n2);
    }
    for (i = 0; aSuffix[i]; i++) { char *z = sqlite3_mprintf("%s%s", zTmp, aSuffix[i]); if (z) unlink(z); sqlite3_free(z); }
    sqlite3_free(zTmp);
    return rc;
}

/* The anchor moved to `anchored`: the soft batches up to it are hard if the chain names what was applied */
static int node_soft_settle(HppliteNode *node, uint64_t anchored) {
    uint64_t end = node->softFirst + node->nSoft - 1;
    unsigned char chain[32];
    if (!node->softFirst || anchored < node->softFirst) return 0;
    if (anchored > end) { node_soft_clear(node); return 0; }      /* past them: the replay from end+1 and the final check compare */
    if (node_range_chain(node, anchored, chain) != 0) return 0;   /* asked again at the next sync */
    if (memcmp(chain, node->softChain[anchored - node->softFirst], 32) != 0) {
        node->softStopped = 1;
        node->state = HPPLITE_STATE_ERROR;
        hpplite_error(node->ctx, "the chain's commitment at height %llu names another chain hash than the batches the store served past height %llu: "
                      "this replica's soft state from %llu on is not the rollup's (equivocation, or a stale store); rebuilding it",
                      (unsigned long long)anchored, (unsigned long long)(node->softFirst - 1), (unsigned long long)node->softFirst);
        node->softDisagreements++;
        return node_soft_rebuild(node) == 0 ? 1 : -1;
    }
    if (anchored == end) node_soft_clear(node);
    else {
        size_t drop = (size_t)(anchored - node->softFirst + 1);
        memcpy(node->softBase, node->softChain[drop - 1], 32);
        memmove(node->softChain, node->softChain + drop, (node->nSoft - drop) * 32);
        node->nSoft -= drop;
        node->softFirst = anchored + 1;
    }
    return 0;
}

/* Past the anchored height: the store's next names, checked, applied */
static int node_soft_follow(HppliteNode *node) {
    HppliteL1State *st;
    unsigned char seq[20], da[20], who[20], inst[32], k[32], before[32], after[32];
    uint8_t sig[65];
    char name[80];
    int applied = 0;
    if (!node_soft_follows(node) || !(st = hpplite_l1_get_state(node->l1))) return 0;
    memcpy(seq, st->sequencerWallet, 20);
    hpplite_l1_state_free(st);
    for (;;) {
        uint64_t h = node->ctx->blockHeight, chainId = hpplite_l1_chain_id(node->l1);
        size_t n = 0;
        uint8_t *data;
        HppliteBatch *batch;
        int rc;
        if (!chainId) chainId = hpplite_l1_default_chain_id();
        if (node_store_name(node, h, name, da) != 0 || !(data = hpplite_store_get_named(node->store, name, &n, sig, inst))) break;
        if (hpplite_store_batch_signer(da, chainId, inst, h, data, n, sig, who) != 0 || memcmp(who, seq, 20) != 0) {
            fprintf(stderr, "hpplite: batch %llu at the store is not signed by the rollup's sequencer: the soft tier stops before it\n", (unsigned long long)h);
            free(data);
            break;
        }
        batch = node_decode_batch(node, h, data, n);
        if (!batch) { free(data); break; }
        batch->pPosted = data; batch->nPosted = n; batch->bOwnsPosted = 1;
        hpplite_keccak256(data, n, k);
        memcpy(before, node->ctx->chainHash, 32);
        hpplite_chain_hash_next(before, h, k, after);
        rc = node_apply_pages(node, batch);
        hpplite_batch_free(batch);
        if (rc != SQLITE_OK || node->ctx->blockHeight != h + 1) {
            fprintf(stderr, "hpplite: batch %llu at the store does not apply on this history (rc=%d): the soft tier stops before it\n", (unsigned long long)h, rc);
            break;
        }
        if (node_soft_push(node, h, before, after) != 0) break;
        applied++;
    }
    return applied;
}

int hpplite_node_sync_from_l1(HppliteNode *node, int useTxn) {
    uint64_t lastBatch = 0, totalBatches = 0;
    unsigned char latestHash[32];
    int synced = 0;

    HppliteFleetView fv;
    if (!node || !node->l1 || !node->ctx) return 0;
    if (node_check_rollup(node) < 0) return -1;
    memset(&fv, 0, sizeof fv);
    if (node->config.hasFleet) {
        /* fleet= (#316 d): the history is the store's heads; the fleet's anchor, not the node, posts roots */
        if (!node->store || hpplite_store_fleet_view(node->store, node->config.fleet, node_fleet_chain_id(node), node->config.dbId, &fv) != 0) return 0;
        lastBatch = fv.hasHead ? fv.toHeight : 0;
    } else if (hpplite_l1_get_da_state(node->l1, &lastBatch, &totalBatches, latestHash) != 0) return 0;
    else node->anchoredHeight = lastBatch;
    if (!node->config.hasFleet && node_soft_follows(node)) {
        /* the soft tier (#166): a state from before this process counted again, then what the anchor settled */
        if (!node->softFirst && node->ctx->blockHeight > lastBatch + 1 && node_soft_recount(node, lastBatch) != 0) {
            node->softStopped = 1;
            node->state = HPPLITE_STATE_ERROR;
            hpplite_error(node->ctx, "this replica is past the anchored height %llu and the store no longer serves the batches it applied there: rebuilding it",
                          (unsigned long long)lastBatch);
            node->softDisagreements++;
            if (node_soft_rebuild(node) != 0) return -1;
            return 1;
        }
        {
            int settled = node_soft_settle(node, lastBatch);
            if (settled < 0) return -1;
            if (settled > 0) return 1;                  /* rebuilt: the copy is up to date */
        }
    }

    if (node->config.hasFleet && fv.snapHeight > node->snapHeight) { node->snapHeight = fv.snapHeight; node->snapAt = fv.snapAt; }   /* #314 */
    if (node->config.hasFleet && fv.snapHeight >= 2 && fv.snapHeight <= lastBatch && node->ctx->blockHeight <= fv.snapHeight
        && (node->ctx->blockHeight == 1 || fv.prunedBelow > node->ctx->blockHeight) && node->pm && !node->config.verifyOnly
        && hpplite_pagemode_captured(node->pm) == 0 && hpplite_pagemode_pending(node->pm) == 0) {
        /* a fleet database behind its latest snapshot (#314): from it rather than batch 1 - and below the store's prune,
        ** the only way back */
        int started = node_fleet_start_from_snapshot(node, &fv);
        if (started < 0) { node->state = HPPLITE_STATE_ERROR; return -1; }
        if (started > 0) synced++;
    }
    if (!node->config.hasFleet && node->config.fromSnapshot && node->pm && node->store && node->ctx->blockHeight == 1 && lastBatch >= 2
        && hpplite_l1_has_ranges(node->l1) && hpplite_pagemode_captured(node->pm) == 0 && hpplite_pagemode_pending(node->pm) == 0) {
        /* a new replica: from the latest snapshot rather than batch 1 (#222) */
        int started = node_start_from_snapshot(node, lastBatch, 0);
        if (started < 0) { node->state = HPPLITE_STATE_ERROR; return -1; }
        if (started > 0) synced++;
    }
    while (node->ctx->blockHeight <= lastBatch && !(node->syncTo && node->ctx->blockHeight > node->syncTo) && !node->syncAtReached) {
        /* a verifier reads batch 1 first - every step since genesis is the stronger check - and a history pruned at
        ** the store (keep=, #223) has none to read: then the oldest snapshot the anchors still name (#224), its root
        ** checked against the one the chain anchored there, and the batches after it. Was hpplite-verify's retry with
        ** from=earliest (#255); now any verifier's. The read of batch 1 is quiet (#228): a prune is not this open's failure */
        int mayFallBack = node->ctx->blockHeight == 1 && node->config.verifyOnly && !node->triedOldestSnapshot && node->pm && node->store && lastBatch >= 2
                          && hpplite_l1_has_ranges(node->l1) && hpplite_pagemode_captured(node->pm) == 0 && hpplite_pagemode_pending(node->pm) == 0;
        HppliteBatch *batch;
        int rc;
        if (mayFallBack) { node->ctx->quiet = 1; node->ctx->zQuietError[0] = 0; }
        batch = node_fetch_batch(node, node->ctx->blockHeight);
        node->ctx->quiet = 0;
        if (batch && node->syncAtMs) {
            /* a stop by time (#205): no batch written after it begins; one that continues a transaction in flight
            ** at that time is applied, so the database is never left halfway through one */
            uint64_t ms = 0;
            int more = 0;
            hpplite_pagebatch_meta(batch->pPagesRaw, batch->nPagesRaw, &ms, &more);
            if (ms > node->syncAtMs && !node->syncAtInFlight) { hpplite_batch_free(batch); node->syncAtReached = 1; break; }
            node->syncAtInFlight = more;
            node->lastAppliedMs = ms;
        }
        if (!batch) {
            if (node->lastFetchForeign && node_skip_foreign_height(node, node->ctx->blockHeight) == 0) {
                synced++;
                continue;
            }
            if (mayFallBack) {
                char why[256];
                int started;
                snprintf(why, sizeof why, "%s", node->ctx->zQuietError[0] ? node->ctx->zQuietError : "not readable");
                node->triedOldestSnapshot = 1;
                fprintf(stderr, "hpplite: batch 1 cannot be read: trying the oldest snapshot the anchors name (a history pruned at the store, keep=)\n");
                started = node_start_from_snapshot(node, lastBatch, 1);
                if (started < 0) { node->state = HPPLITE_STATE_ERROR; return -1; }
                if (started > 0) { synced++; continue; }
                if (node->snapshotPastStop)
                    hpplite_error(node->ctx, "batch 1 cannot be read (%s) and the oldest snapshot the store keeps is at %llu, past the stop: the batches before "
                                  "it are pruned (keep=), so the database as of an earlier height or time cannot be rebuilt", why, (unsigned long long)node->snapshotPastStop);
                else hpplite_error(node->ctx, "%s", why);        /* the quiet read's reason, said now */
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
        /* a page batch: a sequencer (its file is where the pages come from) adopts its own; a replica or a
        ** verifier applies it to the file */
        rc = (node->pm && !node->config.verifyOnly && !node->follower && node->lockFd >= 0) ? node_adopt_pages(node, batch) : node_apply_pages(node, batch);
        if (rc != SQLITE_OK) { hpplite_batch_free(batch); node->state = HPPLITE_STATE_ERROR; return -1; }
        node->lastCheckpointHeight = node->ctx->lastCheckpointHeight;
        hpplite_batch_free(batch);
        synced++;
    }
    (void)useTxn;
    /* Caught up: what we rebuilt must be what the contract committed to.
    ** The DA contract keeps a rolling hash over every batch (#33); ours
    ** advanced with every batch finalized, replayed or skipped. A file from
    ** before the hash was kept has none (zero) and is not compared. */
    if (node->config.hasFleet && lastBatch > 0 && node->ctx->blockHeight == lastBatch + 1) {
        /* the rolling hash over what was read is the latest head's, and the head is this database's key's (#316 d) */
        unsigned char me[20];
        if (memcmp(node->ctx->chainHash, fv.chainAfter, 32) != 0
            || (hpplite_l1_get_my_address(node->l1, me) == 0 && memcmp(me, fv.headSigner, 20) != 0)) {
            hpplite_error(node->ctx, "the batches read from the store to height %llu are not the history its latest head signs (the rolling hash or the "
                          "signer differs)", (unsigned long long)lastBatch);
            node->state = HPPLITE_STATE_ERROR;
            return -1;
        }
    } else if (lastBatch > 0 && node->ctx->blockHeight == lastBatch + 1) {
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
    if (!node->config.hasFleet && !node->config.verifyOnly && node->ctx->blockHeight == lastBatch + 1 && hpplite_node_pending_count(node) == 0)
        synced += node_soft_walk(node);             /* what the store holds past the chain (#167), before anything of ours */
    if (!node->config.hasFleet && node->ctx->blockHeight >= lastBatch + 1) synced += node_soft_follow(node);   /* a verifier's soft tier (#166) */
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
static sqlite3 *node_ck_conn(HppliteNode *node) { return node->ckDb; }

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

/* The file's WAL into the database (#144, #155): a passive checkpoint on a plain connection of the node's own, once a
** second from the WAL upkeep; the application connection's own checkpoint is off (its wal hook is the capture's),
** so the application never pays for one, and the pins (#155) keep the frames a batch still needs */
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


/* The first anchor of a run (#308): anchor= after the last anchor on chain (its lastCheckpointTime), not after this
** run's start - a sequencer restarted more often than anchor= (a deploy, a reboot, a new build) must still anchor.
** Past it already: soon, after a jitter of up to a hundredth of anchor= (at most 30 s) drawn from the instance, so a
** host's sequencers restarted together do not anchor in the same second. One read of the chain */
static int64_t node_anchor_deadline(HppliteNode *node, int64_t now) {
    int64_t interval = node->config.anchorIntervalMs, due, jitter;
    HppliteL1State *st = hpplite_l1_get_state(node->l1);
    if (!st) return now + interval;
    if (!st->lastCheckpointTime) { hpplite_l1_state_free(st); return now + interval; }
    due = ((int64_t)st->lastCheckpointTime - (int64_t)st->currentTimestamp) * 1000 + interval;   /* in the chain's clock */
    hpplite_l1_state_free(st);
    if (due > interval) due = interval;
    if (due > 0) return now + due;
    jitter = interval / 100 < 28000 ? interval / 100 : 28000;
    return now + 2000 + (jitter > 0 ? (int64_t)(((uint64_t)node->instanceId[0] << 8 | node->instanceId[1]) % (uint64_t)jitter) : 0);
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
        /* the wait on the monotonic clock: a wall clock stepped back (a VM's guest agent, NTP) must not hold the
        ** timer for the step, nor a step forward cut its waits short (somaro's v0.1 soak) */
        pthread_mutex_lock(wakeMutex);
#ifdef __APPLE__
        if (node->timerRunning) {
            struct timespec rel = { sleepMs / 1000, (long)(sleepMs % 1000) * 1000000L };
            pthread_cond_timedwait_relative_np(wake, wakeMutex, &rel);
        }
#else
        if (node->timerRunning) {
            struct timespec until;
            clock_gettime(CLOCK_MONOTONIC, &until);
            until.tv_nsec += (long)sleepMs * 1000000L;
            until.tv_sec += until.tv_nsec / 1000000000L;
            until.tv_nsec %= 1000000000L;
            pthread_cond_timedwait(wake, wakeMutex, &until);
        }
#endif
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
        int sizeTrigger = node->config.maxBatchBytes > 0 && node->pm
            && hpplite_pagemode_pending_bytes(node->pm) >= node->config.maxBatchBytes * HPPLITE_PAGE_DEFLATE_GUESS / 2;
        if (node->pm) node_wal_maintenance(node);

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
        /* the anchor's first deadline, decided before this tick's flush (#270): what a previous run left at the
        ** store is anchored soon - unless anchorclose=off, where every run leaves rows by design and they wait
        ** for the cadence like the rest; a batch this tick posts is this run's and waits for the cadence too */
        if (node->config.anchorIntervalMs > 0 && node->store && node->l1 && node->anchorAt == 0) {
            node->anchorAt = !node->config.noAnchorAtClose && node_unanchored_above(node, node->anchoredHeight) ? now + 2000 : node_anchor_deadline(node, now);
        }
        /* An idle writer holding the lease asks the chain nothing at its tick (#292): nobody else posts while the
        ** lease holds, and a lease lost meanwhile (expired, the owner's forceExpire) is met at the next post, which
        ** reads the lease and adopts or refuses. It still looks every HPPLITE_IDLE_SYNC_MS; a waiting writer, a
        ** follower and a verifier keep looking at every tick, and hpplite_flush() always does */
        if ((elapsed >= intervalMs || sizeTrigger) && !sizeTrigger && node->state == HPPLITE_STATE_RUNNING && node->hasLease
            && !node->leaseHeldElsewhere && !node->config.verifyOnly && now < node->idleSyncAt && hpplite_node_pending_count(node) == 0) {
            node->lastFlushTime = now;
        } else if (elapsed >= intervalMs || sizeTrigger) {
            pthread_mutex_t *mutex = (pthread_mutex_t *)node->flushMutex;
            if (mutex) {
                pthread_mutex_lock(mutex);
                if (node->timerRunning) {
                    node_flush_unlocked(node);
                    node->idleSyncAt = now + HPPLITE_IDLE_SYNC_MS;
                }
                pthread_mutex_unlock(mutex);
            }
        }
        /* the file against what was last sent (after #327) */
        if (node->pm && now >= node->fileCheckAt) {
            const char *zEvery = getenv("HPPLITE_FILE_CHECK_SECONDS");
            pthread_mutex_t *mutex = (pthread_mutex_t *)node->flushMutex;
            if (mutex) {
                pthread_mutex_lock(mutex);
                if (node->timerRunning) node_check_file(node);
                pthread_mutex_unlock(mutex);
            }
            node->fileCheckAt = now + (zEvery && atol(zEvery) > 0 ? atol(zEvery) : 60) * 1000;
        }
        /* the anchor, on the chain's cadence (#164); a refused one is tried again within a minute. Nothing at the store
        ** above the anchored height: no attempt and no chain read, and the deadline stays armed - the next batch is
        ** anchored at the next tick, not a whole anchor= later (#308) */
        if (node->config.anchorIntervalMs > 0 && node->store && node->l1 && now >= node->anchorAt
            && node_unanchored_above(node, node->anchoredHeight) > 0) {
            pthread_mutex_t *mutex = (pthread_mutex_t *)node->flushMutex;
            if (mutex) {
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
#ifdef __APPLE__
        pthread_cond_init(c, NULL);                   /* the timer waits with the relative wait, on no wall clock */
#else
        {   pthread_condattr_t at;                    /* the timer's waits on the monotonic clock */
            pthread_condattr_init(&at);
            pthread_condattr_setclock(&at, CLOCK_MONOTONIC);
            pthread_cond_init(c, &at);
            pthread_condattr_destroy(&at); }
#endif
        pthread_mutex_init(m, NULL);
        node->timerWake = c;
        node->timerWakeMutex = m;
    }

    node->timerRunning = 1;
    pthread_create(thread, NULL, node_timer_thread_func, node);
    node->timerThread = thread;
    /* the WAL's checkpoint is this thread's now (#144); the application's
    ** commits stop doing it. interval=0 (no timer) leaves SQLite's own. */
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
    if (!node || !sql || !node->ctx) return -1;
    return sqlite3_exec(node->ctx->db, sql, NULL, NULL, errMsg);     /* the VFS sees the commits: nothing else to do */
}

/*
** Get number of pending SQL statements.
*/
int hpplite_node_pending_count(HppliteNode *node) {
    if (!node || !node->ctx) return 0;
    if (!node->pm) return 0;       /* a follower or a verifier runs no page machinery of its own: nothing of theirs waits */
    /* what another connection or process committed since the last look
    ** is pending too (#153): a look at the WAL first, cheap when nothing
    ** was added */
    if (node->ctx->zWalKey && !node->config.verifyOnly) hpplite_walread_poll(node->ctx->zWalKey, 0);
    return (int)hpplite_pagemode_pending(node->pm);
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
}

int hpplite_node_set_batch_key(HppliteNode *node, const unsigned char key[32]) {
    if (!node || !key) return -1;
    if (node->flushMutex) pthread_mutex_lock((pthread_mutex_t *)node->flushMutex);
    memcpy(node->encKey, key, HPPLITE_CIPHER_KEY_SIZE);
    node->hasEncKey = 1;
    if (!node->config.noEncrypt) node->cipher = hpplite_cipher_default();
    node->warnedNoKey = 0;
    if (node->l1 && !node->follower) {
        /* the history could not be read without the key: it is read now, before anything of this node's own is
        ** captured - a fresh sequencer's file must be the chain's before it posts (#253; a signer's node, anvil 23) */
        int wasError = node->state == HPPLITE_STATE_ERROR;
        if (hpplite_node_sync_from_l1(node, 1) >= 0 && wasError && node->state == HPPLITE_STATE_ERROR) {
            node->state = HPPLITE_STATE_RUNNING;             /* the error was the sealed history: read now */
            hpplite_error_clear(node->ctx);
        }
    }
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
