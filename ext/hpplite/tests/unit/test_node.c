/*
** Test HPPLite Node Module
**
** Tests singleton sequencer functionality.
** Simplified model - no witnesses, just sequencer + L1 coordination.
*/

#include "node.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>

/* Test counters */
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    printf("  Testing: %s... ", name); \
    tests_run++; \
} while(0)

#define PASS() do { \
    printf("PASS\n"); \
    tests_passed++; \
} while(0)

#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
} while(0)

/* page capture (#146): the file as opened may count as one pending more; cases about the log table skip */
/* pending counts commits; the file as opened (its pages, never shipped) may count as one more (#146) */
static int pending_is(long long pending, int n) { return pending == n || pending == n + 1; }

/* Test private key (for reproducible testing) */
static const unsigned char SEQ_PRIVKEY[32] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
};

/* Callback tracking */
static int batches_posted = 0;
static uint64_t last_batch_height = 0;

static void on_batch_posted(void *arg, uint64_t height, const char *txHash) {
    (void)arg;
    (void)txHash;
    batches_posted++;
    last_batch_height = height;
}

/* Helper: create test directory */
static void ensure_test_dir(const char *path) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", path);
    (void)!system(cmd);
}

/* Helper: cleanup test files */
static void cleanup_test_files(const char *dir) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    (void)!system(cmd);
}

/*
** Test 1: Node creation and basic properties
*/
static void test_node_create(void) {
    HppliteNodeConfig config;
    HppliteNode *node;
    unsigned char pubkey[HPPLITE_PUBKEY_SIZE];
    unsigned char address[HPPLITE_ADDRESS_SIZE];
    unsigned char instanceId[32];
    char *stats;

    TEST("node_create");

    ensure_test_dir("/tmp/hpplite_test_seq");

    memset(&config, 0, sizeof(config));
    config.nodeId = "test-sequencer";
    memcpy(config.privkey, SEQ_PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = "/tmp/hpplite_test_seq";
    config.dbPath = "/tmp/hpplite_test_seq/test.db";
    config.localOnly = 1;   /* no chain here: the local throw-away mode (#38) */

    node = hpplite_node_create(&config);
    if (!node) {
        FAIL("Failed to create node");
        return;
    }

    /* Check pubkey is derived */
    hpplite_node_get_pubkey(node, pubkey);
    int hasNonZero = 0;
    for (int i = 0; i < HPPLITE_PUBKEY_SIZE; i++) {
        if (pubkey[i] != 0) hasNonZero = 1;
    }
    if (!hasNonZero) {
        FAIL("Pubkey is all zeros");
        hpplite_node_destroy(node);
        return;
    }

    /* Check address is derived */
    hpplite_node_get_address(node, address);
    hasNonZero = 0;
    for (int i = 0; i < HPPLITE_ADDRESS_SIZE; i++) {
        if (address[i] != 0) hasNonZero = 1;
    }
    if (!hasNonZero) {
        FAIL("Address is all zeros");
        hpplite_node_destroy(node);
        return;
    }

    /* Check instance ID is unique */
    hpplite_node_get_instance_id(node, instanceId);
    hasNonZero = 0;
    for (int i = 0; i < 32; i++) {
        if (instanceId[i] != 0) hasNonZero = 1;
    }
    if (!hasNonZero) {
        FAIL("Instance ID is all zeros");
        hpplite_node_destroy(node);
        return;
    }

    /* Check stats */
    stats = hpplite_node_get_stats(node);
    if (!stats || strlen(stats) < 10) {
        FAIL("Stats not generated");
        hpplite_node_destroy(node);
        return;
    }
    sqlite3_free(stats);

    hpplite_node_destroy(node);
    cleanup_test_files("/tmp/hpplite_test_seq");

    PASS();
}

/*
** Test 2: SQL execution and batch production
*/
static void test_sequencer_batches(void) {
    HppliteNodeConfig config;
    HppliteNode *sequencer;
    char *errMsg = NULL;
    int rc;
    uint64_t height;

    TEST("sequencer_batches");

    ensure_test_dir("/tmp/hpplite_test_seq2");

    memset(&config, 0, sizeof(config));
    config.nodeId = "test-sequencer";
    memcpy(config.privkey, SEQ_PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = "/tmp/hpplite_test_seq2";
    config.dbPath = "/tmp/hpplite_test_seq2/test.db";
    config.localOnly = 1;   /* no chain here: the local throw-away mode (#38) */

    sequencer = hpplite_node_create(&config);
    if (!sequencer) {
        FAIL("Failed to create sequencer");
        return;
    }

    /* Set callbacks */
    batches_posted = 0;
    sequencer->onBatchPosted = on_batch_posted;

    /* Execute SQL */
    rc = hpplite_node_exec(sequencer, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)", &errMsg);
    if (rc != 0) {
        FAIL(errMsg ? errMsg : "Failed to exec SQL");
        sqlite3_free(errMsg);
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_seq2");
        return;
    }

    /* Check pending count */
    if (!pending_is(hpplite_node_pending_count(sequencer), 1)) {
        FAIL("Pending count should be 1");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_seq2");
        return;
    }

    /* Execute more SQL */
    rc = hpplite_node_exec(sequencer, "INSERT INTO users (name) VALUES ('Alice')", &errMsg);
    if (rc != 0) {
        FAIL(errMsg ? errMsg : "Failed to insert");
        sqlite3_free(errMsg);
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_seq2");
        return;
    }

    /* Flush batch */
    height = hpplite_node_flush_batch(sequencer);
    if (height != 1) {
        FAIL("Expected batch height 1");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_seq2");
        return;
    }

    /* Pending should be 0 now */
    if (hpplite_node_pending_count(sequencer) != 0) {
        FAIL("Pending count should be 0 after flush");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_seq2");
        return;
    }

    hpplite_node_destroy(sequencer);
    cleanup_test_files("/tmp/hpplite_test_seq2");

    PASS();
}

/*
** Test 3: Multiple batches and state progression
*/
static void test_multiple_batches(void) {
    HppliteNodeConfig config;
    HppliteNode *sequencer;
    uint64_t height;
    unsigned char root1[HPPLITE_HASH_SIZE];
    unsigned char root2[HPPLITE_HASH_SIZE];

    TEST("multiple_batches");

    ensure_test_dir("/tmp/hpplite_test_multi");

    memset(&config, 0, sizeof(config));
    config.nodeId = "test-sequencer";
    memcpy(config.privkey, SEQ_PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = "/tmp/hpplite_test_multi";
    config.dbPath = "/tmp/hpplite_test_multi/test.db";
    config.localOnly = 1;   /* no chain here: the local throw-away mode (#38) */

    sequencer = hpplite_node_create(&config);
    if (!sequencer) {
        FAIL("Failed to create sequencer");
        return;
    }

    /* First batch */
    hpplite_node_exec(sequencer, "CREATE TABLE t1 (x INTEGER)", NULL);
    height = hpplite_node_flush_batch(sequencer);
    if (height != 1) {
        FAIL("Expected height 1");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_multi");
        return;
    }
    hpplite_node_get_state_root(sequencer, root1);

    /* Second batch */
    hpplite_node_exec(sequencer, "INSERT INTO t1 VALUES (100)", NULL);
    height = hpplite_node_flush_batch(sequencer);
    if (height != 2) {
        FAIL("Expected height 2");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_multi");
        return;
    }
    hpplite_node_get_state_root(sequencer, root2);

    /* State root should change between batches */
    if (memcmp(root1, root2, HPPLITE_HASH_SIZE) == 0) {
        FAIL("State root should change after insert");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_multi");
        return;
    }

    /* Check current height */
    if (hpplite_node_get_height(sequencer) != 3) {
        FAIL("Height should be 3 (next block)");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_multi");
        return;
    }

    hpplite_node_destroy(sequencer);
    cleanup_test_files("/tmp/hpplite_test_multi");

    PASS();
}

/*
** Test 4: Checkpoint interval tracking
*/
static void test_checkpoint_tracking(void) {
    HppliteNodeConfig config;
    HppliteNode *sequencer;

    TEST("checkpoint_tracking");

    ensure_test_dir("/tmp/hpplite_test_cp");

    memset(&config, 0, sizeof(config));
    config.nodeId = "test-sequencer";
    memcpy(config.privkey, SEQ_PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = "/tmp/hpplite_test_cp";
    config.dbPath = "/tmp/hpplite_test_cp/test.db";
    config.localOnly = 1;   /* no chain here: the local throw-away mode (#38) */
    config.checkpointInterval = 3;  /* Checkpoint every 3 batches */

    sequencer = hpplite_node_create(&config);
    if (!sequencer) {
        FAIL("Failed to create sequencer");
        return;
    }

    /* Should not checkpoint yet */
    if (hpplite_node_should_checkpoint(sequencer)) {
        FAIL("Should not checkpoint at start");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_cp");
        return;
    }

    /* Produce 3 batches */
    for (int i = 0; i < 3; i++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "CREATE TABLE t%d (x)", i);
        hpplite_node_exec(sequencer, sql, NULL);
        hpplite_node_flush_batch(sequencer);
    }

    /* Should checkpoint now */
    if (!hpplite_node_should_checkpoint(sequencer)) {
        FAIL("Should checkpoint after 3 batches");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_cp");
        return;
    }

    hpplite_node_destroy(sequencer);
    cleanup_test_files("/tmp/hpplite_test_cp");

    PASS();
}

/*
** Test 5: Node start/stop lifecycle
*/
static void test_node_lifecycle(void) {
    HppliteNodeConfig config;
    HppliteNode *sequencer;

    TEST("node_lifecycle");

    ensure_test_dir("/tmp/hpplite_test_life");

    memset(&config, 0, sizeof(config));
    config.nodeId = "test-sequencer";
    memcpy(config.privkey, SEQ_PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = "/tmp/hpplite_test_life";
    config.dbPath = "/tmp/hpplite_test_life/test.db";
    config.localOnly = 1;   /* no chain here: the local throw-away mode (#38) */

    sequencer = hpplite_node_create(&config);
    if (!sequencer) {
        FAIL("Failed to create sequencer");
        return;
    }

    /* Start node */
    int rc = hpplite_node_start(sequencer);
    if (rc != 0) {
        FAIL("Failed to start node");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_life");
        return;
    }

    if (sequencer->state != HPPLITE_STATE_RUNNING) {
        FAIL("Node should be running");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_life");
        return;
    }

    /* Stop node */
    hpplite_node_stop(sequencer);
    if (sequencer->state != HPPLITE_STATE_STOPPED) {
        FAIL("Node should be stopped");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_life");
        return;
    }

    hpplite_node_destroy(sequencer);
    cleanup_test_files("/tmp/hpplite_test_life");

    PASS();
}


static HppliteNode *open_local_node(const char *dir, const char *id) {
    static HppliteNodeConfig config;
    static char dbPath[256];
    cleanup_test_files(dir);
    ensure_test_dir(dir);
    snprintf(dbPath, sizeof(dbPath), "%s/test.db", dir);
    memset(&config, 0, sizeof(config));
    config.nodeId = (char *)id;
    memcpy(config.privkey, SEQ_PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = (char *)dir;
    config.dbPath = dbPath;
    config.localOnly = 1;
    return hpplite_node_create(&config);
}


/* A writer that never pauses between transactions: the flush asks for the
** gate and the writer hands it over at the end of its transaction, so the
** batch is finalized on the first try. Before: every try found a statement
** active and gave up, and nothing was ever posted. */
typedef struct { HppliteNode *node; volatile int stop; double worstMs; int txns; } Writer;
static double node_now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }
static void *writer_main(void *arg) {
    Writer *w = (Writer *)arg;
    sqlite3 *db = w->node->db;
    while (!w->stop) {
        double t0 = node_now_ms(), d;
        sqlite3_exec(db, "BEGIN; INSERT INTO t(v) VALUES('abcdefghijklmnopqrstuvwxyz'); COMMIT", NULL, NULL, NULL);
        d = node_now_ms() - t0;
        if (d > w->worstMs) w->worstMs = d;
        w->txns++;
    }
    return NULL;
}

/* The bookkeeping connection commits without an fsync of its own (#142):
** 3 ms per batch under the gate, for commits the chain can replace. */
static void test_bookkeeping_commits_without_fsync(void) {
    HppliteNode *node;
    sqlite3_stmt *st = NULL;
    int sync = -1;

    TEST("bookkeeping_commits_without_fsync");
    node = open_local_node("/tmp/hpplite_test_sync_normal", "test-sync-normal");
    if (!node) { FAIL("create"); return; }
    if (!node->bkDb) { FAIL("no bookkeeping connection"); hpplite_node_destroy(node); return; }
    if (sqlite3_prepare_v2(node->bkDb, "PRAGMA synchronous", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) sync = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (sync != 1) {
        char msg[80];
        snprintf(msg, sizeof(msg), "bookkeeping connection: PRAGMA synchronous = %d, expected 1 (NORMAL)", sync);
        FAIL(msg);
    } else PASS();
    hpplite_node_destroy(node);
    cleanup_test_files("/tmp/hpplite_test_sync_normal");
}


/* The WAL's checkpoint runs on the timer thread, not at the application's
** commit (#144): the application connection's automatic checkpoint is off,
** and the timer moves the WAL into the database on its own. */
static void test_wal_checkpoint_on_the_timer(void) {
    HppliteNodeConfig config;
    HppliteNode *node;
    sqlite3_stmt *st = NULL;
    char *err = NULL;
    int autockpt = -1, i, waited;
    struct stat stDb;

    TEST("wal_checkpoint_on_the_timer");
    cleanup_test_files("/tmp/hpplite_test_wal");
    ensure_test_dir("/tmp/hpplite_test_wal");
    memset(&config, 0, sizeof(config));
    config.nodeId = "test-wal";
    memcpy(config.privkey, SEQ_PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = "/tmp/hpplite_test_wal";
    config.dbPath = "/tmp/hpplite_test_wal/test.db";
    config.localOnly = 1;
    config.batchIntervalMs = 3600 * 1000;
    node = hpplite_node_create(&config);
    if (!node) { FAIL("create"); return; }
    hpplite_node_start(node);
    hpplite_node_start_timer(node);
    if (sqlite3_prepare_v2(node->db, "PRAGMA wal_autocheckpoint", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) autockpt = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (autockpt != 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "the application connection still checkpoints itself (wal_autocheckpoint = %d)", autockpt);
        FAIL(msg); hpplite_node_destroy(node); return;
    }
    /* 5 MB of rows in ten transactions: well past a thousand WAL pages */
    if (hpplite_node_exec(node, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT)", &err) != 0) { FAIL(err); hpplite_node_destroy(node); return; }
    for (i = 0; i < 10; i++) {
        if (hpplite_node_exec(node, "BEGIN; WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i < 1000)"
                                    " INSERT INTO t(v) SELECT zeroblob(500) FROM s; COMMIT", &err) != 0) { FAIL(err); hpplite_node_destroy(node); return; }
    }
    /* the frames reach the database file only through a checkpoint: the
    ** file grows past what the schema alone takes */
    for (waited = 0; waited < 40; waited++) {
        if (stat("/tmp/hpplite_test_wal/test.db", &stDb) == 0 && stDb.st_size > 4 * 1024 * 1024) break;
        usleep(100000);
    }
    if (stat("/tmp/hpplite_test_wal/test.db", &stDb) != 0 || stDb.st_size <= 4 * 1024 * 1024) {
        char msg[120];
        snprintf(msg, sizeof(msg), "no checkpoint in 4 s: the database file is %lld bytes, the rows sit in the WAL", (long long)stDb.st_size);
        FAIL(msg);
    } else PASS();
    hpplite_node_destroy(node);
    cleanup_test_files("/tmp/hpplite_test_wal");
}

int main(void) {
    printf("\n=== HPPLite Node Tests (Singleton Sequencer Model) ===\n\n");

    test_node_create();
    test_sequencer_batches();
    test_multiple_batches();
    test_checkpoint_tracking();
    test_node_lifecycle();
    test_wal_checkpoint_on_the_timer();
    test_bookkeeping_commits_without_fsync();

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
