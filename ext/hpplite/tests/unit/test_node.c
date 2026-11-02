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
    if (hpplite_node_pending_count(sequencer) != 1) {
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

/* The timer flushes early once the pending log nears the DA limit (#143).
** Under row capture a record's bytes are in the rec column, not in sql:
** the size check must see them, or a busy writer posts only on the
** interval and the log grows without bound between flushes. */
static void test_size_trigger_with_row_capture(void) {
    HppliteNodeConfig config;
    HppliteNode *node;
    char *errMsg = NULL;
    uint64_t before;
    int i, waited;

    TEST("size_trigger_with_row_capture");
    cleanup_test_files("/tmp/hpplite_test_size");
    ensure_test_dir("/tmp/hpplite_test_size");

    memset(&config, 0, sizeof(config));
    config.nodeId = "test-size";
    memcpy(config.privkey, SEQ_PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = "/tmp/hpplite_test_size";
    config.dbPath = "/tmp/hpplite_test_size/test.db";
    config.localOnly = 1;
    config.batchIntervalMs = 3600 * 1000;      /* the interval never comes; only the size can flush */
    config.maxBatchBytes = 16 * 1024;          /* half of it, 8 KB, is the trigger */

    node = hpplite_node_create(&config);
    if (!node) { FAIL("Failed to create node"); return; }
    hpplite_node_start(node);
    hpplite_node_start_timer(node);            /* the open hook does this; the C API caller must */
    before = hpplite_node_get_height(node);

    /* 400 rows of ~100 bytes: 40 KB of row records, five times the trigger */
    if (hpplite_node_exec(node, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT); BEGIN", &errMsg) != 0) {
        FAIL(errMsg ? errMsg : "exec"); sqlite3_free(errMsg); hpplite_node_destroy(node); return;
    }
    for (i = 0; i < 400; i++) {
        if (hpplite_node_exec(node, "INSERT INTO t(v) VALUES('0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef')", &errMsg) != 0) {
            FAIL(errMsg ? errMsg : "insert"); sqlite3_free(errMsg); hpplite_node_destroy(node); return;
        }
    }
    if (hpplite_node_exec(node, "COMMIT", &errMsg) != 0) {
        FAIL(errMsg ? errMsg : "commit"); sqlite3_free(errMsg); hpplite_node_destroy(node); return;
    }

    /* the timer ticks every 100 ms; give it three seconds */
    for (waited = 0; waited < 30 && hpplite_node_get_height(node) == before; waited++) usleep(100000);
    if (hpplite_node_get_height(node) == before) {
        char msg[160];
        snprintf(msg, sizeof(msg), "no flush in 3 s with %d pending rows and the interval an hour away: the size trigger did not see the row records",
                 hpplite_node_pending_count(node));
        FAIL(msg);
    } else {
        PASS();
    }
    hpplite_node_destroy(node);
    cleanup_test_files("/tmp/hpplite_test_size");
}

int main(void) {
    printf("\n=== HPPLite Node Tests (Singleton Sequencer Model) ===\n\n");

    test_node_create();
    test_sequencer_batches();
    test_multiple_batches();
    test_checkpoint_tracking();
    test_node_lifecycle();
    test_size_trigger_with_row_capture();

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
