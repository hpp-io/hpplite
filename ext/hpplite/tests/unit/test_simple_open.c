/*
** Test for hpplite_open() simple API
**
** Tests:
**   1. Basic open/close with hpplite=on
**   2. State tracking works through standard sqlite3_exec
**   3. Config loading from URI params
**   4. Manual flush creates batch
**   5. State root changes after modifications
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include "hpplite.h"
#include "node.h"
#include "config.h"
#include "da_uri.h"
#include "batch.h"

#define TEST(name) printf("  %-50s ", name)
#define PASS() printf("✓\n")
#define FAIL(msg) do { printf("✗ %s\n", msg); return 1; } while(0)

static void printHex(const unsigned char *data, int len) {
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
}

int main(void) {
    printf("=== HPPLite Simple Open API Test ===\n\n");
    int tests = 0, passed = 0;

    /* Clean up any previous test data */
    unlink("/tmp/test_simple_open.db");
    (void)!system("rm -rf /tmp/hpplite_test_simple");
    mkdir("/tmp/hpplite_test_simple", 0755);

    /* Test 1: Basic open/close */
    tests++;
    TEST("Basic open/close");
    {
        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&l1=local&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open returned NULL");

        int rc = hpplite_close(db);
        if (rc != SQLITE_OK) FAIL("hpplite_close failed");

        PASS();
        passed++;
    }

    /* Test 2: Context retrieval */
    tests++;
    TEST("Context retrieval");
    {
        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&l1=local&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");

        HppliteCtx *ctx = hpplite_context(db);
        if (!ctx) FAIL("hpplite_context returned NULL");

        hpplite_close(db);
        PASS();
        passed++;
    }

    /* Test 3: Config loading */
    tests++;
    TEST("Config loading from URI; role= takes verify only (#248)");
    {
        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&chainid=12345&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");

        HppliteConfig *cfg = hpplite_get_config(db);
        if (!cfg) FAIL("hpplite_get_config returned NULL");

        if (cfg->chainId != 12345) FAIL("chainId not 12345");
        hpplite_close(db);

        /* role=sequencer and role=observer were parsed and read nowhere; now anything but verify refuses the open */
        db = hpplite_open("file:/tmp/test_simple_open.db?hpplite=on&role=observer&chainid=12345&datadir=/tmp/hpplite_test_simple");
        if (db) { hpplite_close(db); FAIL("role=observer should refuse the open"); }
        if (!hpplite_config_error() || !strstr(hpplite_config_error(), "role= takes verify only")) FAIL("the refusal should say role= takes verify only");
        db = hpplite_open("file:/tmp/test_simple_open.db?hpplite=on&role=verify&chainid=12345&datadir=/tmp/hpplite_test_simple");
        if (!db) FAIL("role=verify should open");
        { sqlite3_stmt *st = NULL; int isVerify = 0;
          if (sqlite3_prepare_v2(db, "SELECT state FROM hpplite_status", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
              isVerify = sqlite3_column_text(st, 0) && strcmp((const char *)sqlite3_column_text(st, 0), "verify") == 0;
          sqlite3_finalize(st);
          if (!isVerify) { hpplite_close(db); FAIL("role=verify should open in the verify state"); } }
        hpplite_close(db);
        PASS();
        passed++;
    }

    /* Test 4: State root starts at zero */
    tests++;
    TEST("Initial state root is zero");
    {
        unlink("/tmp/test_simple_open.db");
        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&l1=local&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");

        unsigned char root[HPPLITE_HASH_SIZE];
        hpplite_state_root(db, root);

        /* Check it's all zeros (page capture: the root is the file's, and a fresh file has its first page) */
        int allZero = 1;
        for (int i = 0; i < HPPLITE_HASH_SIZE; i++) {
            if (root[i] != 0) { allZero = 0; break; }
        }
        { sqlite3_stmt *q = NULL; const char *cap = NULL;       /* page capture (the default on a new rollup): the root is the file's, a fresh file has its first page */
          if (sqlite3_prepare_v2(db, "SELECT capture FROM hpplite_status", -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) cap = (const char *)sqlite3_column_text(q, 0);
          if (cap && strcmp(cap, "pages") == 0) allZero = 1;
          sqlite3_finalize(q); }
        if (!allZero) FAIL("initial state root not zero");

        hpplite_close(db);
        PASS();
        passed++;
    }

    /* Test 6: Manual flush creates batch */
    tests++;
    TEST("Manual flush creates batch");
    {
        unlink("/tmp/test_simple_open.db");
        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&l1=local&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");

        /* Insert some data */
        sqlite3_exec(db,
            "CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT);"
            "INSERT INTO items(id, value) VALUES(1, 'test');",
            NULL, NULL, NULL);

        /* Flush to create batch */
        uint64_t height = hpplite_flush(db);
        if (height == 0) FAIL("hpplite_flush returned 0");

        printf("(block %llu) ", (unsigned long long)height);

        hpplite_close(db);
        PASS();
        passed++;
    }

    /* Test 7: Multiple operations accumulate */
    tests++;
    TEST("Multiple operations accumulate");
    {
        unlink("/tmp/test_simple_open.db");
        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&l1=local&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");

        /* Multiple commits */
        sqlite3_exec(db, "CREATE TABLE t1(x);", NULL, NULL, NULL);
        sqlite3_exec(db, "INSERT INTO t1 VALUES(1);", NULL, NULL, NULL);
        sqlite3_exec(db, "INSERT INTO t1 VALUES(2);", NULL, NULL, NULL);
        sqlite3_exec(db, "INSERT INTO t1 VALUES(3);", NULL, NULL, NULL);

        /* the pending count: the commits waiting */
        int pending = hpplite_node_pending_count(hpplite_get_node(db));
        printf("(%d pending) ", pending);

        /* Flush all at once */
        uint64_t height = hpplite_flush(db);
        if (height == 0) FAIL("hpplite_flush returned 0");

        hpplite_close(db);
        PASS();
        passed++;
    }

    /* Test 8: L1 alias resolves to chain ID */
    tests++;
    TEST("L1 alias 'hpp-sepolia' resolves");
    {
        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&l1=hpp-sepolia&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");

        HppliteConfig *cfg = hpplite_get_config(db);
        if (!cfg) FAIL("hpplite_get_config returned NULL");

        if (cfg->chainId != 181228) {
            printf("(chainId=%llu) ", (unsigned long long)cfg->chainId);
            FAIL("hpp-sepolia should resolve to 181228");
        }

        hpplite_close(db);
        PASS();
        passed++;
    }

    /* Test 9: Automatic flush after interval (background thread) */
    tests++;
    TEST("Auto-flush via background thread");
    {
        unlink("/tmp/test_simple_open.db");
        /* Use 200ms interval for testing */
        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&l1=local&interval=200ms&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");

        /* Check config has correct interval */
        HppliteConfig *cfg = hpplite_get_config(db);
        if (cfg->batchIntervalMs != 200) FAIL("interval not set correctly");
        {   /* interval=0 means no timer at all; it used to fall back to the 5 s default */
            unlink("/tmp/test_simple_open_zero.db");
            sqlite3 *z = hpplite_open("file:/tmp/test_simple_open_zero.db?hpplite=on&l1=local&interval=0&datadir=/tmp/hpplite_test_simple");
            if (!z) FAIL("open interval=0");
            if (hpplite_get_config(z)->batchIntervalMs != 0) FAIL("interval=0 should parse as 0");
            if (hpplite_get_node(z)->timerRunning) FAIL("interval=0 must not start the timer");
            sqlite3_close(z);
            unlink("/tmp/test_simple_open_zero.db");
        }

        /* Get height BEFORE inserting data */
        HppliteCtx *ctx = hpplite_context(db);
        uint64_t h1 = hpplite_get_block_height(ctx);

        /* Insert data */
        sqlite3_exec(db, "CREATE TABLE auto(x);", NULL, NULL, NULL);
        sqlite3_exec(db, "INSERT INTO auto VALUES(1);", NULL, NULL, NULL);

        /* Wait for the background thread to auto-flush: the interval is 200 ms,
        ** a loaded CI runner can take seconds to schedule the thread */
        for (int i = 0; i < 100 && hpplite_get_block_height(ctx) <= h1; i++) usleep(50000);

        /* Check that block height increased - NO manual flush call needed! */
        uint64_t h2 = hpplite_get_block_height(ctx);
        printf("(h1=%llu h2=%llu) ", (unsigned long long)h1, (unsigned long long)h2);

        if (h2 <= h1) {
            FAIL("background thread did not auto-flush");
        }

        hpplite_close(db);
        PASS();
        passed++;
    }

    /* Test 10: Auto-extension with standard sqlite3_open */
    tests++;
    TEST("Auto-extension with sqlite3_open_v2");
    {
        unlink("/tmp/test_simple_open.db");

        /* Open with standard SQLite API - HPPLite auto-initializes!
         * No registration needed - HPPLite is built into SQLite. */
        sqlite3 *db;
        int rc = sqlite3_open_v2(
            "file:/tmp/test_simple_open.db?hpplite=on&l1=local&datadir=/tmp/hpplite_test_simple",
            &db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI,
            NULL
        );
        if (rc != SQLITE_OK) FAIL("sqlite3_open_v2 failed");

        /* HPPLite should be active */
        HppliteCtx *ctx = hpplite_context(db);
        if (!ctx) FAIL("HPPLite not auto-initialized");

        /* Do some SQL */
        sqlite3_exec(db, "CREATE TABLE auto_test(id INT);", NULL, NULL, NULL);
        sqlite3_exec(db, "INSERT INTO auto_test VALUES(42);", NULL, NULL, NULL);

        /* Verify state root changed */
        unsigned char root[32];
        hpplite_state_root(db, root);
        int nonzero = 0;
        for (int i = 0; i < 32; i++) if (root[i]) nonzero = 1;
        if (!nonzero) FAIL("state root is zero after INSERT");

        /* Close with standard sqlite3_close - should auto-flush via close hook */
        sqlite3_close(db);

        PASS();
        passed++;
    }

    /* Test 11: Standard sqlite3_close triggers flush via close hook */
    tests++;
    TEST("sqlite3_close triggers close hook flush");
    {
        unlink("/tmp/test_simple_open.db");

        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&l1=local&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");

        sqlite3_exec(db, "CREATE TABLE hook_test(x);", NULL, NULL, NULL);
        sqlite3_exec(db, "INSERT INTO hook_test VALUES(1);", NULL, NULL, NULL);

        /* Use sqlite3_close directly (not hpplite_close) */
        int rc = sqlite3_close(db);
        if (rc != SQLITE_OK) FAIL("sqlite3_close failed");

        /* If we got here without crash, close hook worked */
        PASS();
        passed++;
    }

    /* Test 12: l1= alias supplies the factory on both open paths */
    tests++;
    TEST("l1 alias sets the factory via hpplite_open and sqlite3_open_v2");
    {
        unsigned char expect[20];
        const HppliteNetworkAlias *net = hpplite_da_lookup_network("hpp-sepolia");
        if (!net || !net->factory) FAIL("alias has no factory");
        if (hpplite_from_hex(net->factory + 2, expect, 20) != 0) FAIL("alias factory hex");

        unlink("/tmp/test_simple_open.db");
        sqlite3 *db = hpplite_open("file:/tmp/test_simple_open.db?hpplite=on&l1=hpp-sepolia&datadir=/tmp/hpplite_test_simple");
        if (!db) FAIL("hpplite_open failed");
        HppliteConfig *cfg = hpplite_get_config(db);
        if (!cfg || !cfg->hasFactory || memcmp(cfg->factory, expect, 20) != 0) FAIL("hpplite_open: factory not set from alias");
        hpplite_close(db);

        unlink("/tmp/test_simple_open.db");
        int rc = sqlite3_open_v2(
            "file:/tmp/test_simple_open.db?hpplite=on&l1=hpp-sepolia&datadir=/tmp/hpplite_test_simple",
            &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
        if (rc != SQLITE_OK) FAIL("sqlite3_open_v2 failed");
        cfg = hpplite_get_config(db);
        if (!cfg || !cfg->hasFactory || memcmp(cfg->factory, expect, 20) != 0) FAIL("sqlite3_open_v2: factory not set from alias");
        if (!cfg->rpcUrl || strcmp(cfg->rpcUrl, net->rpcUrl) != 0) FAIL("rpc not set from alias");
        sqlite3_close(db);

        /* explicit factory= wins over the alias */
        unlink("/tmp/test_simple_open.db");
        rc = sqlite3_open_v2(
            "file:/tmp/test_simple_open.db?hpplite=on&l1=hpp-sepolia&factory=0x1111111111111111111111111111111111111111&datadir=/tmp/hpplite_test_simple",
            &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
        if (rc != SQLITE_OK) FAIL("sqlite3_open_v2 failed");
        cfg = hpplite_get_config(db);
        if (!cfg || !cfg->hasFactory || cfg->factory[0] != 0x11) FAIL("explicit factory should override the alias");
        sqlite3_close(db);
        PASS();
        passed++;
    }

    /* Summary */
    /* #36: hpplite_open() reads parameters as sqlite3_uri_parameter() does:
    ** %HH decoded, nothing cut */
    tests++;
    TEST("hpplite_open: parameters URL-decoded, long ones whole");
    {
        char longRpc[400], uri[1200];
        memset(longRpc, 0, sizeof(longRpc));
        strcpy(longRpc, "http://example.invalid:8545/");
        for (size_t i = strlen(longRpc); i < 300; i++) longRpc[i] = 'p';
        unlink("/tmp/test_simple_open.db");
        snprintf(uri, sizeof(uri), "file:/tmp/test_simple_open.db?hpplite=on&l1=local&interval=0&datadir=/tmp/hpplite_test_simple"
                 "&dbname=%%7Bfile%%7D%%20x&nodeid=a%%20node&rpc=%s", longRpc);
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");
        HppliteConfig *cfg = hpplite_get_config(db);
        int okName = cfg && cfg->dbName && strcmp(cfg->dbName, "{file} x") == 0;
        int noNode = cfg && cfg->nodeId == NULL;          /* nodeid= is no parameter (#248): a label the C API may set */
        int okRpc = cfg && cfg->rpcUrl && strcmp(cfg->rpcUrl, longRpc) == 0;
        hpplite_close(db);
        if (!okName) FAIL("%7Bfile%7D and %20 not decoded");
        if (!noNode) FAIL("nodeid= should not be read from the URI");
        if (!okRpc) FAIL("a 300-byte value was cut");
        PASS();
        passed++;
    }

    /* #78: a %00 would end the value where it stands; such a parameter is ignored, not cut */
    tests++;
    TEST("hpplite_open: a parameter with %00 is ignored, not cut");
    {
        unlink("/tmp/test_simple_open.db");
        sqlite3 *db = hpplite_open("file:/tmp/test_simple_open.db?hpplite=on&l1=local&interval=0&datadir=/tmp/hpplite_test_simple"
                                   "&dbname=cut%00here");
        if (!db) FAIL("hpplite_open failed");
        HppliteConfig *cfg = hpplite_get_config(db);
        int cut = cfg && cfg->dbName && strcmp(cfg->dbName, "cut") == 0;
        hpplite_close(db);
        if (cut) FAIL("the value was cut at the %00");
        PASS();
        passed++;
    }

    /* #123: a connection of a set-up file opens with HPPLite while another
    ** connection is inside a write transaction (PHP's one-per-request) */
    tests++;
    TEST("hpplite_open: starts beside another connection's write transaction");
    {
        unlink("/tmp/test_simple_open.db"); unlink("/tmp/test_simple_open.db-wal"); unlink("/tmp/test_simple_open.db-shm");
        sqlite3 *first = hpplite_open("file:/tmp/test_simple_open.db?hpplite=on&l1=local&interval=0&datadir=/tmp/hpplite_test_simple");
        if (!first) FAIL("first open");
        sqlite3_exec(first, "CREATE TABLE t(x)", NULL, NULL, NULL);
        hpplite_close(first);
        sqlite3 *other = NULL;
        if (sqlite3_open("/tmp/test_simple_open.db", &other) != SQLITE_OK) FAIL("plain connection");
        if (sqlite3_exec(other, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) FAIL("the other connection's write transaction");
        sqlite3 *db = hpplite_open("file:/tmp/test_simple_open.db?hpplite=on&l1=local&interval=0&datadir=/tmp/hpplite_test_simple");
        HppliteCtx *ctx = db ? hpplite_context(db) : NULL;
        sqlite3_exec(other, "COMMIT", NULL, NULL, NULL);
        sqlite3_close(other);
        if (!db) FAIL("hpplite_open failed while another connection held the write lock");
        if (!ctx) { hpplite_close(db); FAIL("opened without HPPLite"); }
        sqlite3_exec(db, "INSERT INTO t VALUES(1)", NULL, NULL, NULL);
        sqlite3_stmt *q = NULL; int pending = -1;
        if (sqlite3_prepare_v2(db, "SELECT pending FROM hpplite_status", -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW)
            pending = sqlite3_column_int(q, 0);
        sqlite3_finalize(q);
        hpplite_close(db);
        if (pending < 1) FAIL("the write after the open was not captured");   /* page capture: the file as opened counts too */
        PASS();
        passed++;
    }

    printf("\n=== Results: %d/%d tests passed ===\n", passed, tests);

    /* Cleanup */
    unlink("/tmp/test_simple_open.db");
    (void)!system("rm -rf /tmp/hpplite_test_simple");

    return (passed == tests) ? 0 : 1;
}
