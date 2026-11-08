/*
** HPPLite durability tests
**
** The local database file is the only durable state. These tests cover:
**   - reopening an existing file continues height, no replay over local data
**   - a process that dies with unflushed commits loses nothing
**   - a failed L1 submission keeps the batch pending and retries at the same height
**   - batches replayed from L1 are not logged again as new pending SQL
**   - bound parameters are expanded so batches are replayable
**   - the pending log is atomic with the user's transaction
**   - the state root is a function of content, not of history
*/

#include "test_lib.h"
#include "cipher.h"
#include <unistd.h>
#include <sys/wait.h>

#define PRIVKEY_HEX "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"

static const unsigned char PRIVKEY[32] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20
};

#define DIR "/tmp/hpplite_test_durability"

/* no checkpoint interval given: the node's own default */
static HppliteNode *open_node(const char *dbPath, HppliteL1 *l1) {
    TestNodeOpts o = { 0 };           /* the node's own checkpoint interval */
    return test_open_node(dbPath, l1, &o);
}

/* 1. Reopening an existing file continues where it left off */
static void test_reopen_continues(void) {
    TEST("reopen continues height, no duplicate rows");
    reset_dir();
    const char *path = DIR "/reopen.db";

    HppliteNode *n = open_node(path, NULL);
    if (!n) FAIL("open 1");
    hpplite_node_exec(n, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1,'a')", NULL);
    if (hpplite_node_flush_batch(n) != 1) FAIL("first flush should be height 1");
    hpplite_node_destroy(n);

    n = open_node(path, NULL);
    if (!n) FAIL("open 2");
    if (hpplite_node_get_height(n) != 2) FAIL("height should be 2 after reopen");
    if (hpplite_node_pending_count(n) != 0) FAIL("nothing should be pending after reopen");
    if (count_rows(n->db, "t") != 1) FAIL("row count changed by reopen");
    hpplite_node_exec(n, "INSERT INTO t VALUES(2,'b')", NULL);
    if (hpplite_node_flush_batch(n) != 2) FAIL("second flush should be height 2");
    hpplite_node_destroy(n);

    n = open_node(path, NULL);
    if (!n) FAIL("open 3");
    if (hpplite_node_get_height(n) != 3) FAIL("height should be 3");
    if (count_rows(n->db, "t") != 2) FAIL("expected 2 rows");
    hpplite_node_destroy(n);
    PASS();
}







/* 8. After a crash the old lease blocks submission only until it expires */
static void test_stale_lease_after_crash(void) {
    TEST("stale lease after crash: pending kept, submits once lease expires");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/lease_a.db", l1);
    if (!a) FAIL("open a");
    hpplite_node_exec(a, "CREATE TABLE t(x)", NULL);
    if (hpplite_node_flush_batch(a) != 1) FAIL("batch 1");
    a->hasLease = 0;              /* pretend a crashed: lease is never released */
    hpplite_node_destroy(a);

    HppliteNode *b = open_node(DIR "/lease_b.db", l1);
    if (!b) FAIL("open b");
    if (count_rows(b->db, "t") != 0) FAIL("reconstruction");
    hpplite_node_exec(b, "INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(b) != 0) FAIL("submit should fail while a's lease is active");
    if (!test_pending_is(hpplite_node_pending_count(b), 1)) FAIL("pending must be kept");

    hpplite_l1_mock_advance_time(l1, 10 * 60);   /* lease expires */
    if (hpplite_node_flush_batch(b) != 2) FAIL("submit should succeed after expiry");
    if (hpplite_node_pending_count(b) != 0) FAIL("pending should be empty");
    if (!b->hasLease) FAIL("b should hold the lease now");

    /* issue #21: the holder renews once the lease is past half its life,
    ** so a busy sequencer never lets its own lease lapse */
    hpplite_l1_mock_advance_time(l1, 200);
    hpplite_node_exec(b, "INSERT INTO t VALUES(2)", NULL);
    if (hpplite_node_flush_batch(b) != 3) FAIL("submit near expiry");
    if (hpplite_l1_get_lease_time_remaining(l1) != 300) { printf("(left=%llu) ", (unsigned long long)hpplite_l1_get_lease_time_remaining(l1)); FAIL("lease should have been renewed"); }

    /* and a failing L1 makes the timer back off instead of retrying every interval */
    hpplite_l1_mock_set_fail_submit(l1, 1);
    hpplite_node_exec(b, "INSERT INTO t VALUES(3)", NULL);
    if (hpplite_node_flush_batch(b) != 0) FAIL("submit should fail");
    if (b->failStreak != 1 || b->retryAt == 0) FAIL("failure should schedule a delayed retry");
    hpplite_node_exec(b, "INSERT INTO t VALUES(4)", NULL);
    hpplite_node_flush_batch(b);
    if (b->failStreak != 2) FAIL("second failure doubles the backoff");
    hpplite_l1_mock_set_fail_submit(l1, 0);
    if (hpplite_node_flush_batch(b) != 4) FAIL("explicit flush retries at once");
    if (b->failStreak != 0 || b->retryAt != 0) FAIL("success clears the backoff");
    hpplite_node_destroy(b);
    hpplite_l1_disconnect(l1);
    PASS();
}




/* 12. Background flush while the application holds an open transaction */
static void test_timer_vs_open_transaction(void) {
    TEST("background flush never sees or disturbs an open transaction");
    reset_dir();
    sqlite3 *db = hpplite_open("file:" DIR "/timer.db?hpplite=on&l1=local&interval=100ms&datadir=" DIR);
    if (!db) FAIL("open");
    HppliteCtx *ctx = hpplite_context(db);
    char *err = NULL;
    if (sqlite3_exec(db, "CREATE TABLE t(x)", NULL, NULL, &err) != SQLITE_OK) FAIL("create");
    usleep(300000);                                  /* timer flushes the CREATE */
    uint64_t h1 = hpplite_get_block_height(ctx);
    if (h1 != 2) { printf("(h=%llu) ", (unsigned long long)h1); FAIL("CREATE should have been flushed by the timer"); }

    if (sqlite3_exec(db, "BEGIN; INSERT INTO t VALUES(1); INSERT INTO t VALUES(2);", NULL, NULL, &err) != SQLITE_OK) FAIL("begin");
    usleep(400000);                                  /* several timer ticks inside the txn */
    if (hpplite_get_block_height(ctx) != 2) FAIL("timer must not flush uncommitted rows");
    if (sqlite3_exec(db, "INSERT INTO t VALUES(3); COMMIT;", NULL, NULL, &err) != SQLITE_OK) {
        printf("(%s) ", err ? err : "?"); sqlite3_free(err);
        FAIL("COMMIT failed while the timer thread was active (SQLITE_BUSY?)");
    }
    usleep(300000);
    if (hpplite_get_block_height(ctx) != 3) FAIL("committed transaction should be flushed as one batch");
    if (hpplite_node_pending_count(hpplite_get_node(db)) != 0) FAIL("pending should be empty");
    sqlite3_close(db);
    PASS();
}

/* 13. Checkpoint progress survives reopen */
static void test_checkpoint_persists(void) {
    TEST("checkpoint progress persists across reopen");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNodeConfig config;
    memset(&config, 0, sizeof(config));
    config.nodeId = "cp"; memcpy(config.privkey, PRIVKEY, 32); config.hasPrivkey = 1;
    config.dataDir = DIR; config.dbPath = DIR "/cp.db"; config.checkpointInterval = 2;
    config.localOnly = 1;   /* no chain here: the local throw-away mode (#38) */
    HppliteNode *n = hpplite_node_create(&config);
    if (!n) FAIL("open");
    hpplite_node_set_l1(n, l1); hpplite_node_start(n);
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL); hpplite_node_flush_batch(n);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1)", NULL); hpplite_node_flush_batch(n);
    if (hpplite_l1_mock_get_checkpoint_count(l1) != 1) FAIL("flush should have submitted the checkpoint");
    hpplite_node_destroy(n);

    config.localOnly = 1;   /* no chain here: the local throw-away mode (#38) */
    n = hpplite_node_create(&config);
    if (!n) FAIL("reopen");
    hpplite_node_set_l1(n, l1);
    if (n->lastCheckpointHeight != 2) { printf("(cp=%llu) ", (unsigned long long)n->lastCheckpointHeight); FAIL("last checkpoint height not restored"); }
    if (n->ctx->lastCheckpointHeight != 2) FAIL("ctx checkpoint height not restored");
    const HppliteCheckpoint *cp = hpplite_l1_mock_get_last_checkpoint(l1);
    if (!cp || memcmp(cp->postStateRoot, n->lastCheckpointRoot, 32) != 0) FAIL("checkpoint root not restored");
    /* issue #22: a process that writes one batch and exits, repeatedly, must
    ** still reach the next checkpoint - the count is derived from heights */
    hpplite_node_exec(n, "INSERT INTO t VALUES(2)", NULL); hpplite_node_flush_batch(n);
    hpplite_node_destroy(n);
    config.localOnly = 1;   /* no chain here: the local throw-away mode (#38) */
    n = hpplite_node_create(&config);
    if (!n) FAIL("reopen 2");
    hpplite_node_set_l1(n, l1);
    if (n->batchesSinceCheckpoint != 1) { printf("(since=%d) ", n->batchesSinceCheckpoint); FAIL("batches since checkpoint not derived on reopen"); }
    hpplite_node_exec(n, "INSERT INTO t VALUES(3)", NULL); hpplite_node_flush_batch(n);
    if (hpplite_l1_mock_get_checkpoint_count(l1) != 2) FAIL("second checkpoint should follow across restarts");
    if (n->lastCheckpointHeight != 4) FAIL("second checkpoint height");
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 14. :memory: has no WAL to capture: refused at open with the reason (#253) */
static void test_memory_database(void) {
    TEST(":memory: is refused: page capture needs a file");
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(":memory:", l1);
    if (n) { hpplite_node_destroy(n); FAIL("an in-memory database opened"); }
    if (!hpplite_init_error() || !strstr(hpplite_init_error(), "in-memory")) FAIL("the refusal should say why");
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 15. hpplite_sync() SQL function pulls new batches from L1 */
static void test_sync_function(void) {
    TEST("SELECT hpplite_sync() replays new L1 batches into a reader");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *reader = open_node(DIR "/reader.db", l1);   /* opened against an empty L1 */
    if (!reader) FAIL("open reader");
    hpplite_node_release_lease(reader);                       /* a reader does not sequence */
    HppliteNode *writer = open_node(DIR "/writer.db", l1);
    if (!writer) FAIL("open writer");
    hpplite_node_exec(writer, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(writer, "INSERT INTO t VALUES(1)", NULL);
    hpplite_node_exec(writer, "INSERT INTO t VALUES(2)", NULL);
    if (hpplite_node_flush_batch(writer) != 1) FAIL("writer batch");

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(reader->db, "SELECT hpplite_sync()", -1, &s, NULL) != SQLITE_OK) FAIL("prepare sync");
    int synced = (sqlite3_step(s) == SQLITE_ROW) ? sqlite3_column_int(s, 0) : -1;
    sqlite3_finalize(s);
    if (synced != 1) { printf("(synced=%d) ", synced); FAIL("expected 1 batch synced"); }
    if (count_rows(reader->db, "t") != 2) FAIL("rows not replayed");
    if (hpplite_node_pending_count(reader) != 0) FAIL("replayed SQL must not be pending");
    if (hpplite_node_get_height(reader) != 2) FAIL("reader height");
    reader->l1 = NULL;   /* reader never held the lease; nothing to release */
    hpplite_node_destroy(reader);
    hpplite_node_destroy(writer);
    hpplite_l1_disconnect(l1);
    PASS();
}


/* 17. Transaction stays pending past the wait: adopted once it lands */
static void test_slow_confirmation(void) {
    TEST("submit whose transaction is still pending after the wait is adopted later");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/slow.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);

    /* the mock stores the batch at submit time, so "pending" here means the
    ** reply timed out but the transaction did land: adoption finds it */
    hpplite_l1_mock_set_tx_pending(l1, 1);
    if (hpplite_node_flush_batch(n) != 1) FAIL("landed-but-unconfirmed batch should be adopted");
    hpplite_l1_mock_set_tx_pending(l1, 0);
    if (hpplite_node_pending_count(n) != 0) FAIL("pending");
    if (hpplite_node_get_height(n) != 2) FAIL("height");
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}


/* 19. Batches are sealed by default; only the wallet key opens them */
static void test_encryption_default(void) {
    TEST("batches are sealed with the wallet-derived key; wrong key cannot reconstruct");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/enc_a.db", l1);
    if (!a) FAIL("open a");
    if (!a->cipher) FAIL("encryption should be on by default when a key exists");
    hpplite_node_exec(a, "CREATE TABLE secrets(id INTEGER PRIMARY KEY, v TEXT)", NULL);
    hpplite_node_exec(a, "INSERT INTO secrets VALUES(1,'the-plaintext-marker')", NULL);
    if (hpplite_node_flush_batch(a) != 1) FAIL("flush");

    size_t len = 0;
    uint8_t *raw = hpplite_l1_get_batch(l1, 1, &len);
    if (!raw) FAIL("batch missing");
    if (!hpplite_envelope_is_sealed(raw, len)) FAIL("posted bytes are not an envelope");
    if (memmem(raw, len, "the-plaintext-marker", 20)) FAIL("plaintext leaked to L1");
    free(raw);
    HppliteBatch *b = hpplite_node_fetch_batch(a, 1);
    if (!b || !b->bPages) FAIL("owner cannot read its own batch");
    hpplite_batch_free(b);
    unsigned char ra[32]; hpplite_node_get_state_root(a, ra);
    hpplite_node_destroy(a);

    /* same wallet, fresh file: reconstructs */
    HppliteNode *r = open_node(DIR "/enc_r.db", l1);
    if (!r) FAIL("open reader");
    if (count_rows(r->db, "secrets") != 1) FAIL("owner reconstruction");
    unsigned char rr[32]; hpplite_node_get_state_root(r, rr);
    if (memcmp(ra, rr, 32) != 0) FAIL("root after reconstruction");
    r->l1 = NULL; hpplite_node_destroy(r);

    /* different wallet: sees ciphertext only, replays nothing, does not fork */
    HppliteNodeConfig config;
    memset(&config, 0, sizeof(config));
    config.nodeId = "stranger"; memcpy(config.privkey, PRIVKEY, 32); config.privkey[31] ^= 1;
    config.hasPrivkey = 1; config.dataDir = DIR; config.dbPath = DIR "/enc_x.db";
    config.localOnly = 1;   /* no chain here: the local throw-away mode (#38) */
    HppliteNode *x = hpplite_node_create(&config);
    if (!x) FAIL("open stranger");
    hpplite_node_set_l1(x, l1);
    if (hpplite_node_sync_from_l1(x, 1) >= 0) FAIL("stranger should sync nothing and be told (issue #29)");
    if (x->state != HPPLITE_STATE_ERROR) FAIL("a node that cannot read L1 history is in error state");
    if (hpplite_node_get_height(x) != 1) FAIL("stranger height must not advance");
    if (hpplite_node_fetch_batch(x, 1) != NULL) FAIL("stranger opened the batch");
    x->l1 = NULL; hpplite_node_destroy(x);
    hpplite_l1_disconnect(l1);
    PASS();
}


/* 21. A background flush must never make the application lose a capture */
#include <pthread.h>
typedef struct { HppliteNode *n; int rows; volatile int done; } WriterArgs;
static void *writer_thread(void *arg) {
    WriterArgs *w = arg;
    char sql[128];
    for (int i = 1; i <= w->rows; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES(%d,'r%d')", i, i);
        sqlite3_exec(w->n->db, sql, NULL, NULL, NULL);          /* autocommit, app thread */
    }
    w->done = 1;
    return NULL;
}


/* 23. hpplite=on that cannot be honoured fails the open instead of capturing nothing */
static void test_open_failure_is_loud(void) {
    TEST("sqlite3_open_v2 with hpplite=on and a bad key fails instead of silently skipping HPPLite");
    reset_dir();
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2("file:" DIR "/loud.db?hpplite=on&privkey=0x0000000000000000000000000000000000000000000000000000000000000000",
                             &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    if (rc == SQLITE_OK) { sqlite3_close(db); FAIL("open should fail"); }
    if (!strstr(sqlite3_errmsg(db), "hpplite")) { printf("(%s) ", sqlite3_errmsg(db)); FAIL("error should name hpplite"); }
    sqlite3_close(db);
    /* and a good key opens */
    rc = sqlite3_open_v2("file:" DIR "/loud.db?hpplite=on&privkey=0x" PRIVKEY_HEX,
                         &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    if (rc != SQLITE_OK) FAIL("open with a valid key");
    if (!hpplite_context(db)) FAIL("hpplite active");
    sqlite3_close(db);
    PASS();
}


/* 25. A close that fails with SQLITE_BUSY must not end capture (issue #31) */
static void test_close_busy_keeps_capturing(void) {
    TEST("sqlite3_close() -> SQLITE_BUSY: capture continues, the real close flushes");
    reset_dir();
    sqlite3 *db = NULL;
    sqlite3_stmt *leak = NULL;
    const char *uri = "file:" DIR "/busy.db?hpplite=on&l1=local&privkey=0x" PRIVKEY_HEX "&datadir=" DIR;
    if (sqlite3_open_v2(uri, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL) != SQLITE_OK) FAIL("open");
    sqlite3_exec(db, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL, NULL, NULL);
    sqlite3_prepare_v2(db, "SELECT x FROM t", -1, &leak, NULL);
    sqlite3_step(leak);
    if (sqlite3_close(db) != SQLITE_BUSY) FAIL("close with a live statement should be BUSY");
    if (!hpplite_context(db)) FAIL("HPPLite must survive a failed close");
    /* the statement is still unfinalized (close would fail again) but its
    ** read transaction is over, so the connection can write */
    sqlite3_reset(leak);
    int before = hpplite_node_pending_count(hpplite_get_node(db));
    if (sqlite3_exec(db, "INSERT INTO t VALUES(2)", NULL, NULL, NULL) != SQLITE_OK) FAIL("write after failed close");
    if (hpplite_node_pending_count(hpplite_get_node(db)) != before + 1) { printf("(before %d, after %d, node %p pm %p) ", before, hpplite_node_pending_count(hpplite_get_node(db)), (void *)hpplite_get_node(db), hpplite_get_node(db) ? (void *)hpplite_get_node(db)->pm : NULL); FAIL("write after the failed close must be captured"); }
    sqlite3_finalize(leak);
    if (sqlite3_close(db) != SQLITE_OK) FAIL("close after finalize");

    if (sqlite3_open_v2(uri, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, NULL) != SQLITE_OK) FAIL("reopen");
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT height, pending, (SELECT count(*) FROM t) FROM hpplite_status", -1, &s, NULL);
    if (sqlite3_step(s) != SQLITE_ROW) FAIL("status");
    int height = sqlite3_column_int(s, 0), pending = sqlite3_column_int(s, 1), rows = sqlite3_column_int(s, 2);
    sqlite3_finalize(s);
    if (rows != 2 || pending != 0 || height < 2) {
        printf("(height=%d pending=%d rows=%d) ", height, pending, rows);
        sqlite3_close(db);
        FAIL("everything should have been flushed by the time the close succeeded");
    }
    int heightBefore = height;
    /* variant: nothing runs between the failed close and the real one; the
    ** sentinel destructor tears the node down on the real close */
    sqlite3_exec(db, "INSERT INTO t VALUES(3)", NULL, NULL, NULL);
    sqlite3_prepare_v2(db, "SELECT x FROM t", -1, &leak, NULL);
    if (sqlite3_close(db) != SQLITE_BUSY) FAIL("second BUSY close");
    sqlite3_finalize(leak);
    if (sqlite3_close(db) != SQLITE_OK) FAIL("final close");
    if (sqlite3_open_v2(uri, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, NULL) != SQLITE_OK) FAIL("reopen 2");
    sqlite3_prepare_v2(db, "SELECT height, pending FROM hpplite_status", -1, &s, NULL);
    sqlite3_step(s);
    height = sqlite3_column_int(s, 0); pending = sqlite3_column_int(s, 1);
    sqlite3_finalize(s);
    sqlite3_close(db);
    if (height != heightBefore + 1 || pending != 0) { printf("(height=%d pending=%d) ", height, pending); FAIL("row 3 flushed by one of the two closes; nothing left over"); }
    PASS();
}


/* 27. Two hpplite=on connections to one file: one sequencer, one follower, one log */
static void test_second_connection_is_follower(void) {
    TEST("second connection to the same file captures into the shared log; sequencer posts all");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/pool.db", l1);
    if (!a) FAIL("open a");
    HppliteNodeConfig config;
    memset(&config, 0, sizeof(config));
    config.nodeId = "b"; memcpy(config.privkey, PRIVKEY, 32); config.hasPrivkey = 1;
    config.dataDir = DIR; config.dbPath = DIR "/pool.db";
    config.localOnly = 1;   /* no chain here: the local throw-away mode (#38) */
    HppliteNode *b = hpplite_node_create(&config);
    if (!b) FAIL("open b");
    if (!b->follower || a->follower) FAIL("b should be the follower, a the sequencer");
    hpplite_node_set_l1(b, l1);
    if (hpplite_node_start(b) != 0 || b->hasLease) FAIL("follower never claims the lease");

    hpplite_node_exec(a, "CREATE TABLE t(id INTEGER PRIMARY KEY, who)", NULL);
    hpplite_node_exec(a, "INSERT INTO t VALUES(1,'a')", NULL);
    hpplite_node_exec(b, "INSERT INTO t VALUES(2,'b')", NULL);
    hpplite_node_exec(b, "BEGIN; INSERT INTO t VALUES(3,'b'); INSERT INTO t VALUES(4,'b'); COMMIT", NULL);
    hpplite_node_exec(a, "BEGIN; INSERT INTO t VALUES(5,'a'); INSERT INTO t VALUES(6,'a'); COMMIT", NULL);
    hpplite_node_exec(b, "INSERT INTO t VALUES(7,'b')", NULL);
    if (!test_pending_is(hpplite_node_pending_count(a), 6)) FAIL("both see the shared log");   /* page mode: six commits in the sequencer's queue, the follower keeps none */
    if (hpplite_node_flush_batch(b) != 0) FAIL("follower must not post");
    if (hpplite_node_flush_batch(a) != 1) FAIL("sequencer posts");
    HppliteBatch *batch = hpplite_node_fetch_batch(a, 1);
    if (!batch || !batch->bPages) FAIL("one batch with every commit from both connections");
    hpplite_batch_free(batch);

    /* the sequencer closes first; the follower keeps capturing into the file,
    ** and - the last connection out - posts what is left when it closes */
    hpplite_node_destroy(a);
    hpplite_node_exec(b, "INSERT INTO t VALUES(8,'b-after')", NULL);
    if (!b->follower || hpplite_node_pending_count(b) != 0) FAIL("b is still a follower until it acts");   /* a follower keeps no queue; it ships the file whole when it takes over */
    hpplite_node_destroy(b);
    HppliteNode *c = open_node(DIR "/pool.db", l1);
    if (!c) FAIL("reopen");
    if (c->follower) FAIL("with a and b gone, c is the sequencer");
    if (hpplite_node_pending_count(c) != 0 || hpplite_node_get_height(c) != 3) FAIL("b posted its leftover as it closed");
    unsigned char rootC[32], rootR[32];
    hpplite_node_get_state_root(c, rootC);
    hpplite_node_destroy(c);
    HppliteNode *r = open_node(DIR "/pool_r.db", l1);
    if (!r) FAIL("reader");
    hpplite_node_get_state_root(r, rootR);
    if (count_rows(r->db, "t") != 8 || memcmp(rootC, rootR, 32) != 0) FAIL("rebuild from the merged log");
    r->l1 = NULL; hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 28. sqlite3_close() while the timer thread is inside a flush must not deadlock */
static void test_close_during_timer_flush(void) {
    TEST("sqlite3_close() while a timer flush is in flight returns (no deadlock)");
    reset_dir();
    const char *uri = "file:" DIR "/tclose.db?hpplite=on&privkey=0x" PRIVKEY_HEX "&datadir=" DIR
                      "&interval=1ms&checkpoint=1";
    {
        /* a hang fails the whole suite loudly; a watchdog, not a performance bound:
        ** the macOS CI runner took 62 s for an honest run (#129). HPPLITE_TEST_ALARM
        ** sets it; 0 disables it (a debugger session) */
        const char *z = getenv("HPPLITE_TEST_ALARM");
        alarm(z ? (unsigned)atoi(z) : 300);
    }
    for (int cycle = 0; cycle < 12; cycle++) {
        sqlite3 *db = NULL;
        if (sqlite3_open_v2(uri, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL) != SQLITE_OK) FAIL("open");
        HppliteNode *n = hpplite_get_node(db);
        if (!n) FAIL("node");
        /* the timer's flush parks in wait_for_tx; close lands while it is in flight */
        HppliteL1 *l1 = hpplite_l1_create_mock();
        hpplite_l1_mock_set_submit_delay_ms(l1, 40);
        hpplite_node_set_l1(n, l1);
        sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS t(x)", NULL, NULL, NULL);
        sqlite3_exec(db, "WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i<2000)"
                         " INSERT INTO t SELECT i FROM s", NULL, NULL, NULL);
        usleep((useconds_t)(5000 + (cycle * 7919) % 60000));
        if (sqlite3_close(db) != SQLITE_OK) FAIL("close");
        hpplite_l1_disconnect(l1);
    }
    alarm(0);
    PASS();
}

/* 29. A batch posted by someone else at our height is skipped, by writer and reader alike (issue #39) */
static void test_foreign_batch_is_skipped(void) {
    TEST("foreign batch at our height: sequencer steps over it, rebuild skips it the same way");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/fb.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(w) != 1) FAIL("batch 1");
    /* an intruder held the lease for a while and left junk at height 2 */
    static const uint8_t junk[] = "HPE2\x01\x00junk-sealed-with-another-key-0123456789abcdef0123456789abcdef";
    hpplite_l1_mock_inject_batch(l1, junk, sizeof(junk) - 1);
    hpplite_node_exec(w, "INSERT INTO t VALUES(2)", NULL);
    uint64_t h = hpplite_node_flush_batch(w);
    if (h != 3) { printf("(h=%llu) ", (unsigned long long)h); FAIL("our next batch should land at height 3, after the foreign one"); }
    if (w->ctx->nForeignBatches != 1 || w->state == HPPLITE_STATE_ERROR) FAIL("writer counts the skipped height and stays healthy");
    if (hpplite_node_pending_count(w) != 0) FAIL("pending drained");
    unsigned char rootW[32]; hpplite_node_get_state_root(w, rootW);
    hpplite_node_destroy(w);

    HppliteNode *r = open_node(DIR "/fb_r.db", l1);
    if (!r) FAIL("reader open");
    if (r->state == HPPLITE_STATE_ERROR) FAIL("reader must not stop at the foreign batch");
    if (r->ctx->nForeignBatches != 1) FAIL("reader counts the skipped height");
    if (count_rows(r->db, "t") != 2) FAIL("reader rows");
    unsigned char rootR[32]; hpplite_node_get_state_root(r, rootR);
    if (memcmp(rootW, rootR, 32) != 0) FAIL("roots differ");
    if (memcmp(r->ctx->prevBlockHash, w->ctx ? rootR : rootR, 0) != 0) { } /* prev hashes compared below */
    r->l1 = NULL; hpplite_node_destroy(r);
    /* both followed the same rule: a fresh reader's chain of block hashes equals the writer's */
    HppliteNode *w2 = open_node(DIR "/fb.db", l1);
    HppliteNode *r2 = open_node(DIR "/fb_r.db", l1);
    if (!w2 || !r2) FAIL("reopen");
    if (memcmp(w2->ctx->prevBlockHash, r2->ctx->prevBlockHash, 32) != 0) FAIL("block hash chain differs after the skip");
    r2->l1 = NULL; hpplite_node_destroy(r2);
    hpplite_node_destroy(w2);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 30. Another wallet's lease on our rollup is not evicted (#252: the chain names the sequencer since VERSION 5, so after
** a rotation, #208, that wallet is the new sequencer and this node the old one); a sibling instance is not touched either */
static void test_stranger_lease_evicted_sibling_not(void) {
    TEST("another wallet holding our rollup's lease is not evicted - the batch waits and the node says so; a sibling of our wallet is left alone");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/ev.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    if (hpplite_node_flush_batch(n) != 1) FAIL("batch 1");
    unsigned char other[32]; memset(other, 0xAB, 32);
    /* another wallet holds the lease: on the chain that is the sequencer the owner set */
    hpplite_l1_mock_claim_foreign(l1, NULL, other, 300);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(n) != 0) FAIL("the old node must not expire the named sequencer's lease and post (#252)");
    if (n->hasLease) FAIL("the lease is not ours");
    if (!strstr(n->ctx->zLastError, "holds the lease")) FAILF("the node should say why: '%s'", n->ctx->zLastError);
    if (hpplite_node_pending_count(n) != 1) FAIL("kept pending");
    /* the lease given back (the chain's state after the other instance released it): we post again */
    hpplite_l1_mock_claim_foreign(l1, NULL, other, 0);
    n->leaseRecheckAt = 0;
    if (hpplite_node_flush_batch(n) != 2) FAIL("with the lease free again, the node reclaims and posts");
    if (!n->hasLease) FAIL("lease back with us");
    /* a sibling: our own wallet, another instance - we must not take over */
    unsigned char me[20]; hpplite_l1_get_my_address(l1, me);
    hpplite_l1_mock_claim_foreign(l1, me, other, 300);
    n->leaseRecheckAt = 0;
    hpplite_node_exec(n, "INSERT INTO t VALUES(2)", NULL);
    if (hpplite_node_flush_batch(n) != 0) FAIL("must not post into a sibling's lease");
    if (hpplite_node_pending_count(n) != 1) FAIL("kept pending");
    if (hpplite_l1_get_lease_time_remaining(l1) == 0) FAIL("sibling's lease must be untouched");
    hpplite_l1_mock_advance_time(l1, 301);
    if (hpplite_node_flush_batch(n) != 3) FAIL("posts once the sibling's lease lapses");
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 31. A foreign trace/commit hook ends capture; HPPLite notices and re-arms (issue #41) */
static int foreign_commit_hook(void *p) { (void)p; return 0; }
static int foreign_trace(unsigned m, void *p, void *s, void *x) { (void)m; (void)p; (void)s; (void)x; return 0; }


/* 33. A node with nothing pending that flushes while another instance posted: replay, not ERROR */
static void test_idle_reader_flush_replays(void) {
    TEST("flush on an idle node finds new L1 batches: replays them instead of failing to adopt");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/live.db", l1);
    if (!a) FAIL("open a");
    hpplite_node_exec(a, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(a) != 1) FAIL("a batch 1");
    /* a rebuild running beside the live sequencer (verify_rebuild.py), its timer ticking */
    HppliteNode *r = open_node(DIR "/rebuild.db", l1);
    if (!r || count_rows(r->db, "t") != 1) FAIL("reader synced batch 1");
    hpplite_node_exec(a, "INSERT INTO t VALUES(2)", NULL);
    if (hpplite_node_flush_batch(a) != 2) FAIL("a batch 2");
    /* the reader's timer flushes with nothing pending: must catch up, not error */
    hpplite_node_flush_batch(r);
    if (r->state == HPPLITE_STATE_ERROR) FAIL("idle reader went to ERROR on a batch it had not seen");
    if (count_rows(r->db, "t") != 2 || hpplite_node_get_height(r) != 3) FAIL("reader replayed batch 2");
    /* and with pending SQL of its own, a foreign batch at its height is still refused */
    hpplite_node_exec(r, "INSERT INTO t VALUES(99)", NULL);
    hpplite_node_exec(a, "INSERT INTO t VALUES(3)", NULL);
    if (hpplite_node_flush_batch(a) != 3) FAIL("a batch 3");
    hpplite_node_flush_batch(r);
    if (r->state != HPPLITE_STATE_ERROR) FAIL("a writer whose pending SQL diverges from L1 must refuse, not fork");
    hpplite_node_destroy(r);
    hpplite_node_destroy(a);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* A claim mined but reverted (#34, the shape of issue #40 on the real
** chain): the node asks the chain who holds the lease and does not believe
** the hash; nothing is posted, nothing is lost, the next claim works */
static void test_claim_reverted_on_chain(void) {
    TEST("a claim that reverts after its hash leaves the node without the lease; it posts once a claim lands");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    hpplite_l1_mock_set_claim_reverts(l1, 1);
    HppliteNode *n = open_node(DIR "/claimrev.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    if (hpplite_node_flush_batch(n) != 0) FAIL("posted without the lease");
    if (n->hasLease) FAIL("the node believed a reverted claim");
    if (!test_pending_is(hpplite_node_pending_count(n), 1)) FAIL("pending must be kept");
    {
        uint64_t last = 0, total = 0;
        hpplite_l1_get_da_state(l1, &last, &total, NULL);
        if (last != 0) FAIL("a batch reached L1 without the lease");
    }
    hpplite_l1_mock_set_claim_reverts(l1, 0);
    if (hpplite_node_flush_batch(n) != 1) FAIL("the next claim should land and post");
    if (!n->hasLease || hpplite_node_pending_count(n) != 0) FAIL("lease held, pending posted");
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* A finalize whose bookkeeping COMMIT fails (a full disk): the file still
** says the old height and the rows are still pending, so memory must too -
** or the next flush posts the same rows again at the next height (#97) */
static void test_finalize_commit_fails(void) {
    TEST("a finalize whose COMMIT fails leaves the height where the file has it; the batch is adopted, not posted again");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/full.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1)", NULL);
    hpplite_test_fail_commits = 1;                       /* the finalize of batch 1 */
    uint64_t h = hpplite_node_flush_batch(n);
    hpplite_test_fail_commits = 0;
    uint64_t last = 0, total = 0;
    hpplite_l1_get_da_state(l1, &last, &total, NULL);
    if (h != 0 || last != 1) FAILF("flush %llu, chain %llu: batch 1 should be posted and not finalized", (unsigned long long)h, (unsigned long long)last);
    if (hpplite_node_get_height(n) != 1) FAILF("height in memory %llu after the failed finalize, the file says 1", (unsigned long long)hpplite_node_get_height(n));
    if (!test_pending_is(hpplite_node_pending_count(n), 2)) FAIL("the rows should still be pending");
    h = hpplite_node_flush_batch(n);                     /* adopts batch 1 from L1 and finalizes it */
    hpplite_l1_get_da_state(l1, &last, &total, NULL);
    if (last != 1) FAILF("the chain holds %llu batches: the same rows were posted again", (unsigned long long)last);
    if (hpplite_node_get_height(n) != 2 || hpplite_node_pending_count(n) != 0) FAIL("batch 1 should be adopted and finalized now");
    hpplite_node_exec(n, "INSERT INTO t VALUES(2)", NULL);
    if (hpplite_node_flush_batch(n) != 2) FAIL("batch 2");
    unsigned char rootW[32], rootR[32];
    hpplite_state_root(n->db, rootW);
    hpplite_node_destroy(n);
    HppliteNode *r = open_node(DIR "/full_r.db", l1);
    if (!r) { hpplite_l1_disconnect(l1); FAIL("replica open"); }
    int rows = count_rows(r->db, "t");
    hpplite_state_root(r->db, rootR);
    hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    if (rows != 2) FAILF("replica has %d rows, the writer 2 (a row posted twice)", rows);
    if (memcmp(rootW, rootR, 32) != 0) FAIL("replica root differs");
    PASS();
}

/* A full disk in the middle of a series of batches (#49): a backlog that
** splits into several batches posts its first, whose finalize cannot
** commit; the next flush finds it on L1 and adopts it, and that commit
** fails too (the disk is still full); then there is room again. The node
** must carry on where the chain is - nothing posted twice, no gap - and
** not stay in error over a disk that was full for a while. */
static void test_full_disk_mid_series(void) {
    TEST("full disk mid-series: the finalize and the adopt after it both fail to commit; with room again the series completes, nothing posted twice, no stuck error");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .checkpointInterval = TEST_INTERVAL_1000, .maxBatchBytes = 2048 };
    HppliteNode *n = test_open_node(DIR "/full_series.db", l1, &o);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT)", NULL);
    if (hpplite_node_flush_batch(n) != 1) FAIL("batch 1");
    for (int i = 0; i < 60; i++) {
        char sql[200];
        snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%d, '%0100d')", i, i);
        hpplite_node_exec(n, sql, NULL);
    }
    uint64_t last = 0, total = 0;
    hpplite_test_fail_commits = 2;                       /* batch 2's finalize, then its adopt */
    uint64_t h1 = hpplite_node_flush_batch(n);
    uint64_t h2 = hpplite_node_flush_batch(n);
    hpplite_test_fail_commits = 0;
    int errAfterFull = n->state == HPPLITE_STATE_ERROR;
    char *afterFull = sqlite3_mprintf("%s", n->ctx->zLastError);
    hpplite_l1_get_da_state(l1, &last, &total, NULL);
    if (h1 != 0 || h2 != 0) FAILF("flushes %llu, %llu with the disk full: want 0, 0", (unsigned long long)h1, (unsigned long long)h2);
    if (last != 2) FAILF("the chain holds %llu batches after two failed commits: want 2 (batch 2 posted once)", (unsigned long long)last);
    if (hpplite_node_get_height(n) != 2) FAILF("height %llu in memory, the file says 2", (unsigned long long)hpplite_node_get_height(n));
    /* room again: the series completes from batch 2 on */
    uint64_t h3 = 0;
    for (int k = 0; k < 10 && hpplite_node_pending_count(n) > 0; k++) h3 = hpplite_node_flush_batch(n);
    hpplite_l1_get_da_state(l1, &last, &total, NULL);
    int pending = hpplite_node_pending_count(n);
    uint64_t height = hpplite_node_get_height(n);
    printf("(after the full disk: state %s, last_error \"%s\"; then %llu batches on chain, height %llu) ",
           errAfterFull ? "error" : "ok", afterFull, (unsigned long long)last, (unsigned long long)height);
    sqlite3_free(afterFull);
    if (pending != 0) FAILF("the series did not complete: %d pending, last flush %llu", pending, (unsigned long long)h3);   /* page mode: the rows sit in one page, the series is shorter */
    if (last + 1 != height) FAILF("the chain holds %llu batches, the file finalized %llu", (unsigned long long)last, (unsigned long long)(height - 1));
    for (uint64_t h = 1; h <= last; h++) {
        HppliteBatch *b = hpplite_node_fetch_batch(n, h);
        if (!b) FAILF("batch %llu is missing on the chain", (unsigned long long)h);
        hpplite_batch_free(b);
    }
    hpplite_node_exec(n, "INSERT INTO t VALUES(100, 'after')", NULL);
    if (hpplite_node_flush_batch(n) != height) FAIL("the node does not go on");
    if (n->state == HPPLITE_STATE_ERROR) FAIL("the node stays in error after the disk had room again (a full disk is not a fork)");
    const char *why = test_replica_check(n, l1, DIR "/full_series_r.db", &o, 0, 1, 1);
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* A batch the node adopted from L1 (posted, confirmation lost, found there
** at the next flush) must be kept like one it finalized itself, or a reorg
** that drops it cannot be repaired: "no longer kept to post again" (#99,
** found by the fault harness, #98) */
static void test_adopted_batch_kept_for_reorg(void) {
    TEST("an adopted batch is kept for a reorg: dropped by the chain, it is posted again, not a dead end");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/adopt_keep.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1)", NULL);
    hpplite_l1_mock_set_lose_confirmation(l1, 1);        /* posted, the caller never hears: found on L1 and adopted */
    if (hpplite_node_flush_batch(n) != 1 || hpplite_node_pending_count(n) != 0) FAIL("premise: batch 1 posted and adopted from L1");
    hpplite_l1_mock_set_lose_confirmation(l1, 0);
    hpplite_l1_mock_truncate(l1, 0);                     /* a reorg drops batch 1 */
    hpplite_node_exec(n, "INSERT INTO t VALUES(2)", NULL);
    uint64_t h = hpplite_node_flush_batch(n);
    uint64_t last = 0, total = 0;
    hpplite_l1_get_da_state(l1, &last, &total, NULL);
    int err = n->state == HPPLITE_STATE_ERROR;
    int pending = hpplite_node_pending_count(n);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    if (err) FAIL("the node stopped: the adopted batch was not kept to post again");
    if (last != 2 || h != 2 || pending != 0) FAILF("chain %llu batches, flush %llu, pending %d: want 2, 2, 0", (unsigned long long)last, (unsigned long long)h, pending);
    PASS();
}


int main(void) {
    printf("=== HPPLite Durability Tests ===\n");
    test_suite_init("durability", PRIVKEY, DIR);
    test_reopen_continues();
    test_stale_lease_after_crash();
    test_timer_vs_open_transaction();
    test_checkpoint_persists();
    test_memory_database();
    test_sync_function();
    test_slow_confirmation();
    test_encryption_default();
    test_open_failure_is_loud();
    test_close_busy_keeps_capturing();
    test_second_connection_is_follower();
    test_close_during_timer_flush();
    test_foreign_batch_is_skipped();
    test_stranger_lease_evicted_sibling_not();
    test_idle_reader_flush_replays();
    test_claim_reverted_on_chain();
    test_finalize_commit_fails();
    test_full_disk_mid_series();
    test_adopted_batch_kept_for_reorg();
    remove_dir();
    return test_report();
}
