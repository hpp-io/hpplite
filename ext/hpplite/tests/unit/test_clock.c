/*
** HPPLite clock SQL replays (#56)
**
** CURRENT_TIMESTAMP, datetime('now'), unixepoch() and DEFAULT CURRENT_TIMESTAMP
** columns - the defaults every ORM template uses - are accepted: the writer
** records the time each such statement saw, replay uses it. A replica that
** replays later, or in another process, computes the same values. Time
** zones still differ between machines, so 'localtime' and 'utc' modifiers
** in writes stay refused.
*/

#include "test_lib.h"
#include "batch.h"
#include "clock.h"
#include <unistd.h>

static const unsigned char PRIVKEY[32] = {
    0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,0x70,
    0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x7b,0x7c,0x7d,0x7e,0x7f,0x80
};

#define DIR "/tmp/hpplite_test_clock"

static HppliteNode *open_node_lim(const char *dbPath, HppliteL1 *l1, size_t limit) {
    TestNodeOpts o = { .maxBatchBytes = limit, .checkpointInterval = TEST_INTERVAL_1000 };
    return test_open_node(dbPath, l1, &o);
}
static HppliteNode *open_node(const char *dbPath, HppliteL1 *l1) { return open_node_lim(dbPath, l1, 0); }

/* Every row of a table, as one string (column values joined) */
#define dump(db, sql) query_text(db, sql)

static const char *SCHEMA =
    "CREATE TABLE ev(id INTEGER PRIMARY KEY, name TEXT,"
    " at TEXT DEFAULT CURRENT_TIMESTAMP, day TEXT DEFAULT (date('now')),"
    " epoch INTEGER DEFAULT (unixepoch()), ms TEXT DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')))";
#define EV_ALL "SELECT id, name, at, day, epoch, ms FROM ev ORDER BY id"

/* ------------------------------------------------------------------------ */

static void test_defaults_replay(void) {
    TEST("DEFAULT CURRENT_TIMESTAMP / date('now') / unixepoch() columns replay to the writer's values");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/w.db", l1);
    if (!w) FAIL("open");
    if (!w->ctx->bClockPin) FAIL("the writer's connection is not on the clock VFS");
    if (hpplite_node_exec(w, SCHEMA, NULL) != SQLITE_OK) FAIL("a table with time defaults was refused");
    for (int i = 0; i < 3; i++) {
        char sql[96];
        snprintf(sql, sizeof(sql), "INSERT INTO ev(name) VALUES('e%d')", i);
        if (hpplite_node_exec(w, sql, NULL) != SQLITE_OK) FAIL("insert into a table with time defaults refused");
        usleep(30000);                                     /* milliseconds differ per row */
    }
    hpplite_node_flush_batch(w);
    char *a = dump(w->db, EV_ALL);
    usleep(1100000);                                       /* replay a second later */
    HppliteNode *r = open_node(DIR "/r.db", l1);
    if (!r || r->state == HPPLITE_STATE_ERROR) { sqlite3_free(a); FAIL("replica"); }
    char *b = dump(r->db, EV_ALL);
    int same = strcmp(a, b) == 0, roots = same_root(w, r);
    int distinct = scalar(w->db, "SELECT count(DISTINCT ms) FROM ev") == 3;
    if (!same) printf("\n    writer:\n%s    replica:\n%s", a, b);
    sqlite3_free(a); sqlite3_free(b);
    hpplite_node_destroy(r);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    if (!distinct) FAIL("test premise: rows should have distinct times");
    if (!same) FAIL("replica values differ");
    if (!roots) FAIL("replica root differs");
    PASS();
}

static void test_clock_in_statements(void) {
    TEST("CURRENT_TIMESTAMP, datetime('now'), julianday('now'), strftime('%f') in writes replay exactly");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/w.db", l1);
    if (!w) FAIL("open");
    static const char *aSql[] = {
        "CREATE TABLE t(id INTEGER PRIMARY KEY, a, b, c, d, e)",
        "INSERT INTO t VALUES(1, CURRENT_TIMESTAMP, datetime('now'), julianday('now'), strftime('%s', 'now'), unixepoch('now', 'subsec'))",
        "INSERT INTO t SELECT 2, CURRENT_DATE, CURRENT_TIME, time('now'), date(), strftime('%Y-%m-%d %H:%M:%f', 'now')",
        "UPDATE t SET e = strftime('%Y-%m-%d %H:%M:%f', 'now') WHERE id = 1",
        "INSERT INTO t VALUES(3, datetime('now', '+1 day'), datetime('now', 'start of month'), timediff('now', '2026-01-01'), 0, 0)",
        "BEGIN", "INSERT INTO t VALUES(4, datetime('now'), 0, 0, 0, 0)", "UPDATE t SET b = julianday() WHERE id = 4", "COMMIT",
        NULL
    };
    for (int i = 0; aSql[i]; i++) {
        char *err = NULL;
        if (sqlite3_exec(w->db, aSql[i], NULL, NULL, &err) != SQLITE_OK) { printf("[%s: %s] ", aSql[i], err ? err : "?"); sqlite3_free(err); FAIL("refused"); }
        usleep(5000);
    }
    hpplite_node_flush_batch(w);
    char *a = dump(w->db, "SELECT * FROM t ORDER BY id");
    usleep(50000);
    HppliteNode *r = open_node(DIR "/r.db", l1);
    if (!r) { sqlite3_free(a); FAIL("replica"); }
    char *b = dump(r->db, "SELECT * FROM t ORDER BY id");
    int same = strcmp(a, b) == 0, roots = same_root(w, r);
    if (!same) printf("\n    writer:\n%s    replica:\n%s", a, b);
    sqlite3_free(a); sqlite3_free(b);
    hpplite_node_destroy(r);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    if (!same) FAIL("replica values differ");
    if (!roots) FAIL("replica root differs");
    PASS();
}

static void test_reads_are_live(void) {
    TEST("reads of 'now' see the real clock, also right after a write that used it");
    reset_dir();
    HppliteNode *w = open_node(DIR "/w.db", NULL);
    if (!w) FAIL("open");
    hpplite_node_exec(w, SCHEMA, NULL);
    hpplite_node_exec(w, "INSERT INTO ev(name) VALUES('x')", NULL);
    sqlite3_int64 at = scalar(w->db, "SELECT epoch FROM ev");
    usleep(2100000);
    sqlite3_int64 now = scalar(w->db, "SELECT unixepoch('now')");
    hpplite_node_destroy(w);
    if (now - at < 2) FAILF("read saw %lld, written %lld: the pinned time leaked into a read", (long long)now, (long long)at);
    if (hpplite_clock_pinned() != 0) FAIL("clock still pinned after the statement");
    PASS();
}

static void test_only_clock_statements_carry_it(void) {
    TEST("only statements that read the clock carry its value in the batch");
    SKIP_IF_ROWS("records carry values, not the time");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/w.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE p(x)", NULL);
    hpplite_node_exec(w, "INSERT INTO p VALUES(1)", NULL);
    hpplite_node_exec(w, "INSERT INTO p VALUES(datetime('now'))", NULL);
    uint64_t h = hpplite_node_flush_batch(w);
    HppliteBatch *b = hpplite_node_fetch_batch(w, h);
    if (!b || b->nTxns != 3) { hpplite_batch_free(b); FAIL("batch"); }
    int ok = b->aTxns[0].now == 0 && b->aTxns[1].now == 0 && b->aTxns[2].now != 0;
    if (!ok) printf("[now: %lld %lld %lld] ", (long long)b->aTxns[0].now, (long long)b->aTxns[1].now, (long long)b->aTxns[2].now);
    hpplite_batch_free(b);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    if (!ok) FAIL("now recorded on the wrong statements");
    PASS();
}

static void test_time_zones_refused(void) {
    TEST("'localtime' and 'utc' modifiers in writes are refused (machine time zone); reads are fine");
    SKIP_IF_ROWS("accepted, replicas equal (test_rows_e2e)");
    reset_dir();
    HppliteNode *w = open_node(DIR "/w.db", NULL);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE z(x)", NULL);
    static const char *aRefused[] = {
        "INSERT INTO z VALUES(datetime('now', 'localtime'))",
        "INSERT INTO z VALUES(datetime('2026-01-01 00:00', 'localtime'))",
        "INSERT INTO z VALUES(datetime('2026-01-01 00:00', 'utc'))",
        "UPDATE z SET x = strftime('%H', x, 'LOCALTIME')",
        "CREATE TABLE z2(x DEFAULT (datetime('now', 'localtime')))",
        NULL
    };
    for (int i = 0; aRefused[i]; i++) {
        if (sqlite3_exec(w->db, aRefused[i], NULL, NULL, NULL) == SQLITE_OK) { hpplite_node_destroy(w); FAILF("accepted: %s", aRefused[i]); }
    }
    if (sqlite3_exec(w->db, "SELECT datetime('now', 'localtime')", NULL, NULL, NULL) != SQLITE_OK) { hpplite_node_destroy(w); FAIL("read refused"); }
    if (sqlite3_exec(w->db, "INSERT INTO z VALUES('localtime')", NULL, NULL, NULL) != SQLITE_OK) { hpplite_node_destroy(w); FAIL("'localtime' as data refused"); }
    hpplite_node_destroy(w);
    PASS();
}

static void test_survives_restart_and_big_txn(void) {
    TEST("recorded times survive a reopen before posting and ride a transaction across batches");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node_lim(DIR "/w.db", l1, 2000);
    if (!w) FAIL("open");
    hpplite_node_exec(w, SCHEMA, NULL);
    hpplite_node_exec(w, "BEGIN", NULL);
    for (int i = 0; i < 40; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO ev(name) VALUES('%040d')", i);
        if (sqlite3_exec(w->db, sql, NULL, NULL, NULL) != SQLITE_OK) { hpplite_node_destroy(w); FAIL("insert"); }
        if (i % 10 == 0) usleep(3000);
    }
    hpplite_node_exec(w, "COMMIT", NULL);
    char *a = dump(w->db, EV_ALL);
    hpplite_l1_mock_set_fail_submit(l1, 1);                /* the close cannot post: the times stay in the log */
    hpplite_node_destroy(w);
    hpplite_l1_mock_set_fail_submit(l1, 0);
    usleep(20000);
    w = open_node_lim(DIR "/w.db", l1, 2000);
    if (!w) { sqlite3_free(a); FAIL("reopen"); }
    if (hpplite_node_pending_count(w) == 0) { sqlite3_free(a); hpplite_node_destroy(w); FAIL("test premise: nothing pending after reopen"); }
    uint64_t h = hpplite_node_flush_batch(w);
    HppliteNode *r = open_node_lim(DIR "/r.db", l1, 2000);
    char *b = r ? dump(r->db, EV_ALL) : NULL;
    int same = b && strcmp(a, b) == 0;
    sqlite3_free(a); sqlite3_free(b);
    if (r) hpplite_node_destroy(r);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    if (h < 3) FAIL("test premise: the transaction did not span batches");
    if (!same) FAIL("replica values differ");
    PASS();
}

static void test_other_vfs_still_refused(void) {
    TEST("a connection on another VFS cannot pin the clock: clock SQL stays refused there");
    SKIP_IF_ROWS("nothing needs the clock pinned");
    reset_dir();
    sqlite3 *db = NULL;
    if (sqlite3_open_v2("file:" DIR "/plain.db?hpplite=on&privkey=0x6162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f80&datadir=" DIR,
                        &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, "unix") != SQLITE_OK) {
        sqlite3_close(db); FAIL("open");
    }
    HppliteNode *n = hpplite_get_node(db);
    if (!n) { sqlite3_close(db); FAIL("hpplite did not start"); }
    if (n->ctx->bClockPin) { sqlite3_close(db); FAIL("the unix VFS was taken for the clock VFS"); }
    sqlite3_exec(db, "CREATE TABLE t(x)", NULL, NULL, NULL);
    int refusedNow = sqlite3_exec(db, "INSERT INTO t VALUES(datetime('now'))", NULL, NULL, NULL) != SQLITE_OK;
    int refusedDefault = sqlite3_exec(db, "CREATE TABLE d(x DEFAULT CURRENT_TIMESTAMP)", NULL, NULL, NULL) != SQLITE_OK;
    sqlite3_close(db);
    if (!refusedNow || !refusedDefault) FAIL("clock SQL accepted where it cannot be pinned");
    PASS();
}

static void test_format(void) {
    TEST("\"now\" in the JSON and the block hash only when set, round-trip");
    HppliteBatch *b = hpplite_batch_new(3);
    hpplite_batch_add_stmt(b, "INSERT INTO t VALUES(1)", -1, 0);
    unsigned char prev[32] = {0}, h0[32], h1[32];
    char *plain = hpplite_batch_to_json(b);
    hpplite_block_hash(b, prev, h0);
    b->aTxns[0].now = 212000000000000LL + 123;
    hpplite_block_hash(b, prev, h1);
    char *json = hpplite_batch_to_json(b);
    HppliteBatch *p = hpplite_batch_from_json(json);
    int ok = p && p->nTxns == 1 && p->aTxns[0].now == 212000000000000LL + 123;
    int noField = strstr(plain, "\"now\"") == NULL;
    sqlite3_free(plain); sqlite3_free(json);
    hpplite_batch_free(b); hpplite_batch_free(p);
    if (!noField) FAIL("now written when unset");
    if (!ok) FAIL("now did not round-trip");
    if (memcmp(h0, h1, 32) == 0) FAIL("now not in the block hash");
    PASS();
}

int main(void) {
    printf("=== HPPLite clock SQL (#56) ===\n");
    test_suite_init("clock", PRIVKEY, DIR);
    test_format();
    test_defaults_replay();
    test_clock_in_statements();
    test_reads_are_live();
    test_only_clock_statements_carry_it();
    test_time_zones_refused();
    test_survives_restart_and_big_txn();
    test_other_vfs_still_refused();
    return test_report();
}
