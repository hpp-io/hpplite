/*
** HPPLite row capture end to end (#19)
**
** capture=rows: a writer's records travel the whole way - log, batch, mock
** L1, sync - and a replica rebuilt from L1 alone must reach the writer's
** rows and state root. The cases are the ones statement capture had to
** refuse (non-deterministic values, application functions, the machine's
** time zone) and the ones records change the shape of (plaintext batches,
** size cuts, statements larger than a batch, hooks replaced by another
** component). What stays refused stays refused: ATTACH.
**
** test_rows.c checks capture and apply on one connection; this file checks
** the pipeline.
*/

#include "test_lib.h"
#include "batch.h"
#include "cipher.h"
#include "capture_rows.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

static const unsigned char PRIVKEY[32] = {
    0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,0x70,
    0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x7b,0x7c,0x7d,0x7e,0x7f,0x80
};

#define DIR "/tmp/hpplite_test_rows_e2e"

static HppliteNode *open_node_ex(const char *dbPath, HppliteL1 *l1, size_t maxBatch, int noEncrypt) {
    TestNodeOpts o = { .maxBatchBytes = maxBatch, .noEncrypt = noEncrypt };
    return test_open_node(dbPath, l1, &o);
}
static int g_plain;                     /* replicas of an encrypt=off history read plaintext */
static HppliteNode *open_node(const char *dbPath, HppliteL1 *l1) { return open_node_ex(dbPath, l1, 0, g_plain); }

/* Every row of every table (sqlite_sequence included), the FTS5 shadow tables left out */
#define DUMP_FLAGS TEST_DUMP_SKIP_FTS_SHADOW

/* Flush everything, rebuild a replica from L1, compare all rows and the
** root. Destroys the writer. */
static const char *replica_matches(HppliteNode *w, HppliteL1 *l1, const char *replicaFile) {
    TestNodeOpts o = { .noEncrypt = g_plain };
    return test_replica_check(w, l1, tpath(replicaFile), &o, DUMP_FLAGS, 50, 1);
}

static void app_fn(sqlite3_context *c, int n, sqlite3_value **v) {
    (void)n;
    sqlite3_result_int64(c, sqlite3_value_int64(v[0]) * 1000 + (rand() % 1000));   /* not on any replica */
}

/* 1. What statement capture refused: the values exist only on the writer */
static void test_nondeterministic_values(void) {
    TEST("random(), randomblob(), the clock, 'localtime', CURRENT_TIMESTAMP defaults, last_insert_rowid(), an app function: replica equal");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/nd.db", l1);
    if (!w) FAIL("open");
    sqlite3_create_function(w->db, "app_fn", 1, SQLITE_UTF8, NULL, app_fn, NULL, NULL);
    const char *sql[] = {
        "CREATE TABLE t(id INTEGER PRIMARY KEY, r, b, at, local, made DEFAULT CURRENT_TIMESTAMP, prev, f)",
        "INSERT INTO t(r, b, at, local) VALUES(random(), randomblob(16), datetime('now'), datetime('now', 'localtime'))",
        "INSERT INTO t(r, prev) VALUES(random(), last_insert_rowid())",
        "INSERT INTO t(f) VALUES(app_fn(7))",
        "UPDATE t SET r = abs(random()) % 1000, at = strftime('%s', 'now') WHERE id <= 2",
        "INSERT INTO t(r, b) SELECT random(), randomblob(4) FROM t",
        "DELETE FROM t WHERE id = (SELECT min(id) FROM t WHERE r % 2 = 0)",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT count(*) FROM t") < 5) FAIL("writer premise");
    const char *why = replica_matches(w, l1, "nd_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 2. sqlite_sequence: a zero-row INSERT creates an entry no record carries;
** a dropped and recreated table restarts it */
static void test_sequence(void) {
    TEST("sqlite_sequence: zero-row INSERT into a new table, DROP and recreate, deletes past the top: replica equal");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/seq.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "CREATE TABLE src(v)",
        "CREATE TABLE a(id INTEGER PRIMARY KEY AUTOINCREMENT, v)",
        "INSERT INTO a(v) SELECT v FROM src",                           /* (a, 0) */
        "CREATE TABLE b(id INTEGER PRIMARY KEY AUTOINCREMENT, v)",
        "INSERT INTO b(v) VALUES(1), (2), (3)",
        "DELETE FROM b WHERE id = 3",                                   /* seq stays 3 */
        "BEGIN", "CREATE TABLE c(id INTEGER PRIMARY KEY AUTOINCREMENT, v)",
        "INSERT INTO c(v) SELECT v FROM src", "COMMIT",                 /* a migration's copy of nothing */
        "DROP TABLE b",
        "CREATE TABLE b(id INTEGER PRIMARY KEY AUTOINCREMENT, v)",
        "INSERT INTO b(v) SELECT v FROM src",                           /* (b, 0) again */
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT capture = 'rows' FROM hpplite_status") != 1) FAIL("hpplite_status.capture is not 'rows' (#83)");
    if (scalar(w->db, "SELECT count(*) FROM sqlite_sequence") != 3) FAIL("writer premise: three entries");
    const char *why = replica_matches(w, l1, "seq_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 3. Mixed: schema changes and virtual tables are text in the same
** transactions as records, foreign keys on */
static void test_mixed_text_and_records(void) {
    TEST("one transaction mixing DDL (text), FTS5 writes (text) and records, foreign keys on: replica equal and searchable");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/mixed.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "PRAGMA foreign_keys = ON",
        "BEGIN",
        "CREATE TABLE post(id INTEGER PRIMARY KEY, body TEXT, at DEFAULT CURRENT_TIMESTAMP)",
        "CREATE TABLE tag(id INTEGER PRIMARY KEY, post INTEGER REFERENCES post(id) ON DELETE CASCADE, name)",
        "CREATE VIRTUAL TABLE post_fts USING fts5(body, content='post', content_rowid='id')",
        "CREATE TRIGGER post_ai AFTER INSERT ON post BEGIN INSERT INTO post_fts(rowid, body) VALUES(new.id, new.body); END",
        "CREATE TRIGGER post_ad AFTER DELETE ON post BEGIN INSERT INTO post_fts(post_fts, rowid, body) VALUES('delete', old.id, old.body); END",
        "INSERT INTO post(body) VALUES('rows are captured'), ('text is replayed'), ('rows again')",
        "INSERT INTO tag(post, name) SELECT id, 'n' || random() FROM post",
        "COMMIT",
        "DELETE FROM post WHERE id = 2",                                /* cascade to tag, FTS delete */
        "UPDATE tag SET name = upper(name) WHERE post = 1",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT count(*) FROM tag") != 2) FAIL("writer premise: cascade");
    const char *why = replica_matches(w, l1, "mixed_r.db");
    if (!why) {
        HppliteNode *r = open_node(DIR "/mixed_r.db", l1);
        sqlite3_int64 hits = r ? scalar(r->db, "SELECT count(*) FROM post_fts WHERE post_fts MATCH 'rows'") : -1;
        if (r) hpplite_node_destroy(r);
        if (hits != 2) { hpplite_l1_disconnect(l1); FAILF("replica search: %lld hits, want 2", (long long)hits); }
    }
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 4. Records larger than a batch: pieces (#59); transactions larger than a
** batch: open/cont (#57) */
static void test_records_over_batch_size(void) {
    TEST("a 300 KB blob and a 2000-row transaction with 16 KB batches: cut into pieces, replica equal");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node_ex(DIR "/big.db", l1, 16 * 1024, 0);
    if (!w) FAIL("open");
    const char *sql[] = {
        "CREATE TABLE blobs(id INTEGER PRIMARY KEY, data BLOB)",
        "INSERT INTO blobs(data) VALUES(randomblob(300000))",
        "CREATE TABLE many(id INTEGER PRIMARY KEY, v TEXT)",
        "BEGIN",
        "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 2000) "
        "INSERT INTO many(v) SELECT hex(randomblob(20)) FROM n",
        "UPDATE many SET v = lower(v) WHERE id % 3 = 0",
        "COMMIT",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    int last = hpplite_node_flush_batch(w);
    if (last < 20) FAILF("last height %d: the records were not cut into 16 KB batches", last);
    /* the dump compares a blob's length and first byte; the root, all of it */
    const char *why = replica_matches(w, l1, "big_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 5. Size cut: a transaction of records is never split across batches
** unless it is larger than a batch */
static void test_size_cut_keeps_transactions(void) {
    TEST("the size cut never splits a transaction of records across batches");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node_ex(DIR "/cut.db", l1, 3000, 0);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(tag, pad)", NULL);
    if (hpplite_node_flush_batch(w) != 1) FAIL("batch 1");
    for (int k = 0; k < 12; k++) {
        int nStmt = 1 + (k * 7) % 9;
        hpplite_node_exec(w, "BEGIN", NULL);
        for (int s = 0; s < nStmt; s++) {
            char q[256];
            snprintf(q, sizeof(q), "INSERT INTO t VALUES(%d, '%080d')", k, s);
            if (sqlite3_exec(w->db, q, NULL, NULL, NULL) != SQLITE_OK) FAIL("insert");
        }
        hpplite_node_exec(w, "COMMIT", NULL);
    }
    int last = hpplite_node_flush_batch(w);
    if (last < 3) FAILF("flush: last height %d, want several batches", last);
    /* every entry posted is a record that decodes; that no group is split
    ** is what replay checks (a split group is held and dropped whole) */
    for (int h = 2; h <= last; h++) {
        HppliteBatch *b = hpplite_node_fetch_batch(w, (uint64_t)h);
        if (!b) FAIL("fetch posted batch");
        for (int i = 0; i < b->nTxns; i++) {
            int op = 0; char *zTab = NULL;
            if (!(b->aTxns[i].mode & HPPLITE_MODE_ROW)) continue;
            if (hpplite_rows_entry_info(b->aTxns[i].zSql, &op, &zTab) != SQLITE_OK) { hpplite_batch_free(b); FAIL("entry undecodable"); }
            sqlite3_free(zTab);
        }
        hpplite_batch_free(b);
    }
    const char *why = replica_matches(w, l1, "cut_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 6. Plaintext batches carry records, marked so a version without row
** capture refuses them */
static void test_plaintext_entries(void) {
    TEST("encrypt=off: the batch on L1 is plaintext JSON whose entries are records (mode 4), decodable");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    g_plain = 1;
    HppliteNode *w = open_node_ex(DIR "/plain.db", l1, 0, 1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(w, "INSERT INTO t VALUES(random())", NULL);
    if (hpplite_node_flush_batch(w) != 1) FAIL("flush");
    size_t len = 0;
    uint8_t *raw = hpplite_l1_get_batch(l1, 1, &len);
    if (!raw || hpplite_envelope_is_sealed(raw, len)) { free(raw); FAIL("expected plaintext on L1"); }
    char *json = malloc(len + 1);
    memcpy(json, raw, len); json[len] = 0;
    free(raw);
    HppliteBatch *b = hpplite_batch_from_json(strchr(json, '{'));
    free(json);
    if (!b || b->nTxns != 2) { if (b) hpplite_batch_free(b); FAIL("two entries: the CREATE TABLE and a record"); }
    int op = 0; char *zTab = NULL;
    int ok = !(b->aTxns[0].mode & HPPLITE_MODE_ROW) && strncmp(b->aTxns[0].zSql, "CREATE TABLE", 12) == 0
          && (b->aTxns[1].mode & HPPLITE_MODE_ROW)
          && hpplite_rows_entry_info(b->aTxns[1].zSql, &op, &zTab) == SQLITE_OK
          && op == HPPLITE_ROW_INSERT && strcmp(zTab, "t") == 0;
    sqlite3_free(zTab);
    hpplite_batch_free(b);
    if (!ok) FAIL("entries are not what was written");
    const char *why = replica_matches(w, l1, "plain_r.db");
    g_plain = 0;
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 7. Still refused with row capture: ATTACH (writes to another file) */
static void test_attach_refused(void) {
    TEST("ATTACH is still refused with row capture on");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/att.db", l1);
    if (!w) FAIL("open");
    int rc = sqlite3_exec(w->db, "ATTACH '" DIR "/other.db' AS other", NULL, NULL, NULL);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    if (rc == SQLITE_OK) FAIL("ATTACH accepted");
    PASS();
}

static int foreign_commit_hook(void *p) { (void)p; return 0; }
static int foreign_trace(unsigned m, void *c, void *p, void *x) { (void)m; (void)c; (void)p; (void)x; return 0; }

/* 8. Another component replaces the trace hook: the triggers still record
** writes to the tables they cover; the replacement is still reported */
static void test_hooks_replaced(void) {
    TEST("another component replaces the hooks: writes are still recorded, the replacement still reported, replica equal");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/hooks.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL);
    sqlite3_commit_hook(w->db, foreign_commit_hook, NULL);
    sqlite3_trace_v2(w->db, SQLITE_TRACE_STMT, foreign_trace, NULL);
    sqlite3_exec(w->db, "INSERT INTO t VALUES(random())", NULL, NULL, NULL);
    if (hpplite_node_pending_count(w) != 3) { printf("(pending %d) ", hpplite_node_pending_count(w)); FAIL("the write must be recorded by the triggers"); }
    if (scalar(w->db, "SELECT hooks_replaced FROM hpplite_status") != 1) FAIL("replacement should be counted");
    sqlite3_exec(w->db, "INSERT INTO t VALUES(3)", NULL, NULL, NULL);
    const char *why = replica_matches(w, l1, "hooks_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 9. Pending records survive a reopen and are posted by the next writer */
static void test_reopen(void) {
    TEST("records pending at close are posted after a reopen; a text-mode reopen posts them too");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/re.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    hpplite_node_flush_batch(w);
    hpplite_node_exec(w, "INSERT INTO t(v) VALUES(random()), (random())", NULL);
    hpplite_node_exec(w, "UPDATE t SET v = randomblob(3) WHERE id = 1", NULL);
    /* close without the final flush posting: the mock refuses */
    hpplite_l1_mock_set_fail_submit(l1, 1);
    hpplite_node_destroy(w);
    hpplite_l1_mock_set_fail_submit(l1, 0);
    /* reopened without row capture: the records already logged still post */
    w = open_node(DIR "/re.db", l1);
    if (!w) FAIL("reopen");
    hpplite_node_exec(w, "INSERT INTO t(v) VALUES('text')", NULL);
    const char *why = replica_matches(w, l1, "re_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 10. VACUUM may renumber the rowids of tables without an INTEGER PRIMARY
** KEY, and records address their rows by rowid: the replica's VACUUM must
** renumber the same way for the records after it to hit the same rows */
static void test_vacuum(void) {
    TEST("VACUUM on tables without INTEGER PRIMARY KEY (gaps, a WITHOUT ROWID table), then updates and deletes by rowid: replica equal");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/vac.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "CREATE TABLE plain(name TEXT, v)",
        "CREATE TABLE keyed(k TEXT PRIMARY KEY, v)",
        "CREATE TABLE wr(k TEXT PRIMARY KEY, v) WITHOUT ROWID",
        "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 300) "
        "INSERT INTO plain SELECT 'p' || i, i FROM n",
        "INSERT INTO keyed SELECT name, v FROM plain",
        "INSERT INTO wr SELECT name, v FROM plain",
        "DELETE FROM plain WHERE v % 3 = 0",
        "DELETE FROM keyed WHERE v % 4 = 0",
        "DELETE FROM wr WHERE v % 5 = 0",
        "VACUUM",
        "UPDATE plain SET v = -v WHERE rowid IN (1, 2, 50, 150)",
        "DELETE FROM plain WHERE rowid = 10",
        "UPDATE keyed SET v = random() WHERE rowid < 20",
        "INSERT INTO plain VALUES('after', 0)",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT max(rowid) FROM plain") != 201) FAILF("writer premise: VACUUM renumbered plain to %lld rows", scalar(w->db, "SELECT max(rowid) FROM plain"));
    const char *why = replica_matches(w, l1, "vac_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 11. The capture triggers are HPPLite's: the application cannot drop or
** shadow them; a table's own drop takes its triggers, and they come back
** with the table */
static void test_trigger_guard(void) {
    TEST("the application cannot drop or create hpplite_cap_ triggers; DROP TABLE still works and capture resumes on a new table");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/guard.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(x)", NULL);
    char name[64] = "";
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(w->db, "SELECT name FROM temp.sqlite_schema WHERE type = 'trigger' AND name LIKE 'hpplite_cap_i%'", -1, &q, NULL) == SQLITE_OK) {
        if (sqlite3_step(q) == SQLITE_ROW) snprintf(name, sizeof(name), "%s", sqlite3_column_text(q, 0));
        sqlite3_finalize(q);
    }
    if (!name[0]) FAIL("no capture trigger to test with");
    char sql[160];
    snprintf(sql, sizeof(sql), "DROP TRIGGER temp.\"%s\"", name);
    int dropRc = sqlite3_exec(w->db, sql, NULL, NULL, NULL);
    int createRc = sqlite3_exec(w->db, "CREATE TEMP TRIGGER hpplite_cap_x AFTER INSERT ON main.t BEGIN SELECT 1; END", NULL, NULL, NULL);
    /* a DROP TABLE prepared and never run must not open a window for a
    ** DROP TRIGGER prepared after it (#71) */
    sqlite3_stmt *unrun = NULL;
    if (sqlite3_prepare_v2(w->db, "DROP TABLE t", -1, &unrun, NULL) != SQLITE_OK) FAIL("prepare DROP TABLE");
    int windowRc = sqlite3_exec(w->db, sql, NULL, NULL, NULL);
    sqlite3_finalize(unrun);
    hpplite_node_exec(w, "INSERT INTO t VALUES(random())", NULL);
    int dropTableRc = sqlite3_exec(w->db, "DROP TABLE t", NULL, NULL, NULL);
    hpplite_node_exec(w, "CREATE TABLE t(y); INSERT INTO t VALUES(randomblob(4))", NULL);
    sqlite3_int64 recs = scalar(w->db, "SELECT count(*) FROM _hpplite_pending WHERE op > 0");
    const char *why = replica_matches(w, l1, "guard_r.db");
    hpplite_l1_disconnect(l1);
    if (dropRc == SQLITE_OK) FAIL("DROP TRIGGER of a capture trigger accepted");
    if (windowRc == SQLITE_OK) FAIL("DROP TRIGGER of a capture trigger accepted after an unrun DROP TABLE was prepared");
    if (createRc == SQLITE_OK) FAIL("CREATE TRIGGER with the capture prefix accepted");
    if (dropTableRc != SQLITE_OK) FAIL("DROP TABLE refused");
    if (recs != 2) FAILF("%lld records, want 2 (before and after the drop)", (long long)recs);
    if (why) FAIL(why);
    PASS();
}

/* 12. A v0.1 database (statement history, posted and pending) opened by
** v0.2: the rest of its life is records, and a replica rebuilt from the
** whole history - statements, then records - equals it */
static void test_text_history_then_rows(void) {
    TEST("a database with statement history (posted and pending) continues with records: replica equal");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts text = { .captureText = 1 };
    HppliteNode *w = test_open_node(DIR "/hist.db", l1, &text);
    if (!w) FAIL("open text");
    hpplite_node_exec(w, "CREATE TABLE e(id INTEGER PRIMARY KEY, body, at)", NULL);
    hpplite_node_exec(w, "INSERT INTO e(body, at) VALUES('one', '2026-01-01'), ('two', '2026-01-02')", NULL);
    hpplite_node_flush_batch(w);
    hpplite_node_exec(w, "UPDATE e SET body = 'two!' WHERE id = 2", NULL);    /* pending at close */
    hpplite_l1_mock_set_fail_submit(l1, 1);
    hpplite_node_destroy(w);
    hpplite_l1_mock_set_fail_submit(l1, 0);
    w = open_node(DIR "/hist.db", l1);                                     /* the default: rows */
    if (!w) FAIL("reopen with rows");
    const char *sql[] = {
        "INSERT INTO e(body, at) VALUES('three', datetime('now'))",
        "UPDATE e SET at = datetime('now', 'localtime') WHERE id = 1",
        "DELETE FROM e WHERE id = 2",
        "ALTER TABLE e ADD COLUMN n INTEGER DEFAULT 7",
        "UPDATE e SET n = abs(random()) % 100",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT count(*) FROM _hpplite_pending WHERE op > 0") < 3) FAIL("the reopened writer must record rows");
    const char *why = replica_matches(w, l1, "hist_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 13. ALTER TABLE DROP COLUMN: SQLite checks every trigger on the table and
** ours name every column - the ALTER failed (Django's migrations). They are
** dropped first, in a transaction of HPPLite's in autocommit, and rebuilt */
static void test_drop_column(void) {
    TEST("ALTER TABLE DROP COLUMN in autocommit and in a transaction, writes before and after: replica equal");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/dropcol.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "CREATE TABLE p(id INTEGER PRIMARY KEY, name TEXT, legacy TEXT, n INTEGER)",
        "INSERT INTO p(name, legacy, n) VALUES('a', 'x', 1), ('b', 'y', 2)",
        "ALTER TABLE p DROP COLUMN legacy",
        "INSERT INTO p(name, n) VALUES('c', 3)",
        "UPDATE p SET n = n * 10 WHERE id = 1",
        "BEGIN",
        "INSERT INTO p(name, n) VALUES('d', 4)",
        "ALTER TABLE \"p\" DROP COLUMN \"name\"",
        "UPDATE p SET n = -n WHERE id = 2",
        "COMMIT",
        "DELETE FROM p WHERE id = 3",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT count(*) FROM pragma_table_info('p')") != 2) FAIL("writer premise: two columns left");
    if (scalar(w->db, "SELECT count(*) FROM temp.sqlite_schema WHERE type = 'trigger' AND tbl_name = 'p'") != 3)
        FAIL("the table's capture triggers were not rebuilt");
    const char *why = replica_matches(w, l1, "dropcol_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 14. Alembic's table rebuild, foreign keys on: DROP TABLE of a table that
** another one references with ON DELETE CASCADE deletes there too (the
** rebuilt copy of a self-referencing table). The DROP is text, and its
** replay cascades the same way: its triggers must record nothing */
static void test_drop_table_cascade(void) {
    TEST("DROP TABLE cascading through foreign keys (Alembic batch rebuild, FKs on): replica equal");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/alembic.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "PRAGMA foreign_keys = ON",
        "CREATE TABLE comments(id INTEGER PRIMARY KEY, parent_id INTEGER REFERENCES comments(id) ON DELETE CASCADE, body TEXT)",
        "INSERT INTO comments VALUES(1, NULL, 'root'), (2, 1, 'reply'), (3, 2, 'reply to reply')",
        "BEGIN",
        "CREATE TABLE _alembic_tmp_comments(id INTEGER PRIMARY KEY, parent_id INTEGER REFERENCES comments(id) ON DELETE CASCADE, "
        "body VARCHAR(500), edited INTEGER NOT NULL DEFAULT 0)",
        "INSERT INTO _alembic_tmp_comments(id, parent_id, body) SELECT id, parent_id, body FROM comments",
        "DROP TABLE comments",
        "ALTER TABLE _alembic_tmp_comments RENAME TO comments",
        "COMMIT",
        "INSERT INTO comments(parent_id, body) VALUES(NULL, 'after')",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    const char *why = replica_matches(w, l1, "alembic_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 15. A batch on L1 whose finalize the gate deferred (a statement of the
** application mid-step) is adopted at the next flush, not posted again as
** another batch at the same height, counted as an L1 failure (#90) */
static void test_deferred_finalize_not_reposted(void) {
    TEST("a posted batch the gate defers is not re-posted at the next flush, nor counted as a failure");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/defer.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(x); INSERT INTO t VALUES(1), (2)", NULL);
    sqlite3_stmt *held = NULL;
    if (sqlite3_prepare_v2(w->db, "SELECT x FROM t", -1, &held, NULL) != SQLITE_OK) FAIL("prepare");
    if (sqlite3_step(held) != SQLITE_ROW) FAIL("step");            /* mid-step: the gate is the application's */
    uint64_t h1 = hpplite_node_flush_batch(w);                     /* posted, finalize deferred */
    uint64_t h2 = hpplite_node_flush_batch(w);                     /* still deferred: nothing to post */
    uint64_t last = 0, total = 0;
    unsigned char hash[32];
    hpplite_l1_get_da_state(l1, &last, &total, hash);
    int streak = w->failStreak;
    sqlite3_finalize(held);
    hpplite_node_flush_batch(w);                                   /* adopted (an adoption confirms no new height) */
    sqlite3_int64 height = scalar(w->db, "SELECT height FROM hpplite_status");
    const char *why = replica_matches(w, l1, "defer_r.db");
    hpplite_l1_disconnect(l1);
    if (h1 != 0 || h2 != 0) FAILF("flushes under the gate confirmed %llu, %llu; want 0, 0", (unsigned long long)h1, (unsigned long long)h2);
    if (total != 1) FAILF("%llu batches on L1 after two flushes under the gate, want 1", (unsigned long long)total);
    if (streak != 0) FAILF("failStreak %d after a deferral, want 0", streak);
    if (height != 2) FAILF("height %lld after the statement ended and a flush, want 2 (one batch confirmed)", (long long)height);
    if (why) FAIL(why);
    PASS();
}

/* 16. The application's TEMP triggers: replayed as text they live on the
** replica's connection, where SQLITE_DBCONFIG_ENABLE_TRIGGER leaves them
** on; they must not fire on records (their effects are records too), and
** must fire for the text that needs them (one reaching a virtual table) (#76) */
static void test_temp_triggers(void) {
    TEST("an application TEMP trigger: silent under records, live for text; replica equal");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/temptrig.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "CREATE TABLE t(x)", "CREATE TABLE u(y)",
        "CREATE TEMP TRIGGER tt AFTER INSERT ON main.t BEGIN INSERT INTO u VALUES(new.x * 10); END",
        "INSERT INTO t VALUES(1), (2)",                                  /* records for t and, through tt, for u */
        "CREATE TABLE p(b)", "CREATE VIRTUAL TABLE pf USING fts5(b)",
        "CREATE TEMP TRIGGER tp AFTER INSERT ON main.p BEGIN INSERT INTO pf(rowid, b) VALUES(new.rowid, new.b); END",
        "INSERT INTO p VALUES('searchable')",                            /* text: reaches pf through tp */
        "INSERT INTO t VALUES(3)",                                       /* records again, after text */
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT count(*) FROM u") != 3) FAIL("writer premise: the TEMP trigger wrote u");
    const char *why = replica_matches(w, l1, "temptrig_r.db");
    if (!why) {
        HppliteNode *r = open_node(DIR "/temptrig_r.db", l1);
        sqlite3_int64 hits = r ? scalar(r->db, "SELECT count(*) FROM pf WHERE pf MATCH 'searchable'") : -1;
        if (r) hpplite_node_destroy(r);
        if (hits != 1) { hpplite_l1_disconnect(l1); FAILF("replica search: %lld hits, want 1", (long long)hits); }
    }
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* Connection pragmas that change what a statement does without appearing
** in its text travel with it, as foreign keys do (#28, #52) */
static void test_connection_pragmas(void) {
    TEST("legacy_alter_table, ignore_check_constraints, writable_schema (records) and case_sensitive_like (text) replay as the writer had them");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/prag.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "CREATE TABLE t(a INTEGER CHECK(a > 0))",
        "CREATE VIEW v AS SELECT a FROM t",
        "PRAGMA legacy_alter_table = ON",
        "ALTER TABLE t RENAME TO u",                 /* the view keeps saying t */
        "PRAGMA legacy_alter_table = OFF",
        "INSERT INTO u VALUES(1)",
        "PRAGMA ignore_check_constraints = ON",
        "INSERT INTO u VALUES(-1)",                  /* a row the CHECK would refuse */
        "PRAGMA ignore_check_constraints = OFF",
        "PRAGMA writable_schema = ON",
        "UPDATE sqlite_schema SET sql = 'CREATE TABLE u(a INTEGER)' WHERE name = 'u'",
        "PRAGMA writable_schema = RESET",
        "INSERT INTO u VALUES(-2)",                  /* allowed now: the CHECK is gone */
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT count(*) FROM u") != 3) FAIL("writer premise: 3 rows");
    if (scalar(w->db, "SELECT count(*) FROM sqlite_schema WHERE name = 'v' AND sql LIKE '%FROM t'") != 1) FAIL("writer premise: the view still names t");
    const char *why = replica_matches(w, l1, "prag_r.db");
    if (!why) {
        HppliteNode *r = open_node(DIR "/prag_r.db", l1);
        sqlite3_int64 view = r ? scalar(r->db, "SELECT count(*) FROM sqlite_schema WHERE name = 'v' AND sql LIKE '%FROM t'") : -1;
        sqlite3_int64 check = r ? scalar(r->db, "SELECT count(*) FROM sqlite_schema WHERE name = 'u' AND sql LIKE '%CHECK%'") : -1;
        if (r) hpplite_node_destroy(r);
        if (view != 1) why = "replica: the view names u, the writer's names t (legacy_alter_table)";
        else if (check != 0) why = "replica: table u still has its CHECK (writable_schema)";
    }
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);

    /* text capture: LIKE's case sensitivity decides which rows a write touches */
    l1 = hpplite_l1_create_mock();
    TestNodeOpts text = { .captureText = 1 };
    w = test_open_node(DIR "/like.db", l1, &text);
    if (!w) FAIL("open text");
    const char *sql2[] = {
        "CREATE TABLE s(x)",
        "INSERT INTO s VALUES('Abc'), ('abc')",
        "PRAGMA case_sensitive_like = ON",
        "UPDATE s SET x = x || '!' WHERE x LIKE 'a%'",   /* one row on the writer */
        NULL };
    if (run(w, sql2) != 0) FAIL("writer SQL (text)");
    if (scalar(w->db, "SELECT count(*) FROM s WHERE x = 'abc!'") != 1 || scalar(w->db, "SELECT count(*) FROM s WHERE x = 'Abc'") != 1) FAIL("writer premise: one row touched");
    why = replica_matches(w, l1, "like_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* A connection opened without HPPLite writes past the log; the sequencer's
** roots still match (it hashes the live file), the divergence shows only on
** a rebuild. Detected: the file changed while no HPPLite connection wrote
** (#37) */
static void test_foreign_writes_detected(void) {
    TEST("a write by a connection without HPPLite is counted in hpplite_status.foreign_writes; HPPLite's own traffic is not");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/foreign.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = { "CREATE TABLE t(a)", "INSERT INTO t VALUES(1)", NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    hpplite_node_flush_batch(w);
    hpplite_node_exec(w, "INSERT INTO t VALUES(2)", NULL);
    hpplite_node_flush_batch(w);
    hpplite_node_flush_batch(w);                       /* nothing to post: a bookkeeping look, no change */
    hpplite_node_exec(w, "BEGIN; UPDATE t SET a = a WHERE 0; COMMIT", NULL);   /* a no-op transaction of the writer's */
    hpplite_node_flush_batch(w);
    sqlite3_int64 before = scalar(w->db, "SELECT foreign_writes FROM hpplite_status");
    if (before != 0) FAILF("foreign_writes %lld after the writer's own traffic, want 0", (long long)before);

    sqlite3 *plain;
    if (sqlite3_open(DIR "/foreign.db", &plain) != SQLITE_OK) FAIL("plain open");
    if (sqlite3_exec(plain, "INSERT INTO t VALUES(99)", NULL, NULL, NULL) != SQLITE_OK) { sqlite3_close(plain); FAIL("plain write"); }
    sqlite3_close(plain);
    hpplite_node_flush_batch(w);                       /* the next bookkeeping look sees the file changed */
    sqlite3_int64 after = scalar(w->db, "SELECT foreign_writes FROM hpplite_status");
    hpplite_node_exec(w, "INSERT INTO t VALUES(3)", NULL);
    hpplite_node_flush_batch(w);
    sqlite3_int64 later = scalar(w->db, "SELECT foreign_writes FROM hpplite_status");
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    if (after != 1) FAILF("foreign_writes %lld after a write past the log, want 1", (long long)after);
    if (later != 1) FAILF("foreign_writes %lld after the writer's next own write, want still 1", (long long)later);
    PASS();
}

/* Why height stopped advancing, from SQL (#32): hpplite_status.state and
** last_error, the latter cleared by the next success */
static void test_state_and_last_error(void) {
    TEST("hpplite_status.state and last_error: a failed post is named and cleared by the next success; a lease held elsewhere; an undecodable batch puts the replica in error");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/st.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = { "CREATE TABLE t(a)", "INSERT INTO t VALUES(1)", NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    WANT_STATUS(w->db, "state", "sequencing", "at open");
    WANT_STATUS(w->db, "last_error", "", "at open");
    hpplite_l1_mock_set_fail_submit(l1, 1);
    if (hpplite_node_flush_batch(w) != 0) FAIL("premise: the post should fail");
    WANT_STATUS(w->db, "state", "retrying", "after a failed post");
    WANT_STATUS_PREFIX(w->db, "last_error", "batch 1 not confirmed on L1", "after a failed post");
    hpplite_l1_mock_set_fail_submit(l1, 0);
    if (hpplite_node_flush_batch(w) != 1) FAIL("premise: the post should succeed now");
    WANT_STATUS(w->db, "state", "sequencing", "after the next success");
    WANT_STATUS(w->db, "last_error", "", "after the next success");

    /* a second node on another file, with its own rows to post, while the first holds the lease */
    HppliteNode *r = open_node(DIR "/st_r.db", l1);
    if (!r) FAIL("replica open");
    if (scalar(r->db, "SELECT count(*) FROM t") != 1) FAIL("premise: replica synced");
    hpplite_node_exec(r, "INSERT INTO t VALUES(2)", NULL);
    hpplite_node_flush_batch(r);
    WANT_STATUS(r->db, "state", "waiting", "with the lease held elsewhere");
    WANT_STATUS_PREFIX(r->db, "last_error", "another sequencer instance holds the lease", "with the lease held elsewhere");
    hpplite_node_exec(r, "DELETE FROM t WHERE a = 2", NULL);   /* its rows are not going anywhere */
    hpplite_node_destroy(r);

    /* a sealed batch at height 2 that claims height 9: corrupt, a fresh replica stops there, in error */
    {
        HppliteBatch *b = hpplite_batch_new(9);
        size_t len = 0;
        hpplite_batch_add_stmt(b, "INSERT INTO t VALUES(9)", -1, 0);
        char *json = hpplite_batch_to_json(b);
        uint8_t *sealed = hpplite_envelope_seal(w->cipher, w->encKey, 2, HPPLITE_ENCODING_NONE,
                                                (const uint8_t *)json, strlen(json), &len);
        char *tx = sealed ? hpplite_l1_submit_batch(l1, w->instanceId, 2, sealed, len) : NULL;
        sqlite3_free(json); hpplite_batch_free(b); free(sealed);
        if (!tx) FAIL("premise: corrupt batch submitted");
        sqlite3_free(tx);
    }
    hpplite_node_destroy(w);
    HppliteNode *r2 = open_node(DIR "/st_r2.db", l1);
    if (!r2) FAIL("second replica open");
    WANT_STATUS(r2->db, "state", "error", "with a corrupt batch on L1");
    WANT_STATUS_PREFIX(r2->db, "last_error", "batch at height 2 claims height 9; corrupt", "with a corrupt batch on L1");
    hpplite_node_destroy(r2);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* No chain configured (#38): the log is the product, so it is kept pending
** until one is; l1=local is the explicit throw-away mode */
static HppliteNode *open_chainless(const char *dbPath, int localOnly) {
    TestNodeOpts o = { .localOnly = localOnly ? 1 : -1, .start = 1 };
    return test_open_node(dbPath, NULL, &o);
}
static void test_no_chain_keeps_the_log(void) {
    TEST("hpplite=on with no chain keeps the log pending (state 'unposted'); l1=local finalizes batches locally");
    reset_dir();
    HppliteL1 *l1 = NULL;
    HppliteNode *n = open_chainless(DIR "/nochain.db", 0);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(a)", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1)", NULL);
    uint64_t h = hpplite_node_flush_batch(n);
    if (h != 0) { hpplite_node_destroy(n); FAILF("finalized batch %llu with no chain: the history is gone", (unsigned long long)h); }
    if (hpplite_node_pending_count(n) != 2) { hpplite_node_destroy(n); FAIL("the rows should stay pending"); }
    WANT_STATUS(n->db, "state", "unposted", "with no chain");
    hpplite_node_destroy(n);

    n = open_chainless(DIR "/local.db", 1);
    if (!n) FAIL("open local");
    hpplite_node_exec(n, "CREATE TABLE t(a)", NULL);
    if (hpplite_node_flush_batch(n) != 1) { hpplite_node_destroy(n); FAIL("l1=local should finalize locally"); }
    if (hpplite_node_pending_count(n) != 0) { hpplite_node_destroy(n); FAIL("l1=local: pending should be empty"); }
    WANT_STATUS(n->db, "state", "local", "with l1=local");
    hpplite_node_destroy(n);
    PASS();
}

/* role=verify (#5): rebuild from the chain, compare every posted root and
** the on-chain checkpoint, and never post, claim the lease or create */
static void test_verify_role(void) {
    TEST("role=verify rebuilds, compares roots and the on-chain checkpoint, and never posts or takes the lease");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNodeConfig config;
    memset(&config, 0, sizeof(config));
    config.nodeId = "rows";
    memcpy(config.privkey, PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = DIR;
    config.dbPath = DIR "/vw.db";
    config.checkpointInterval = 2;
    config.noEncrypt = g_plain;
    HppliteNode *w = hpplite_node_create(&config);
    if (!w) FAIL("open writer");
    hpplite_node_set_l1(w, l1);
    hpplite_node_start(w);
    hpplite_node_exec(w, "CREATE TABLE t(a)", NULL);
    if (hpplite_node_flush_batch(w) != 1) FAIL("batch 1");
    hpplite_node_exec(w, "INSERT INTO t VALUES(1), (2)", NULL);
    if (hpplite_node_flush_batch(w) != 2) FAIL("batch 2 (carries the root, checkpoint)");
    hpplite_node_exec(w, "INSERT INTO t VALUES(3)", NULL);
    if (hpplite_node_flush_batch(w) != 3) FAIL("batch 3");
    hpplite_node_destroy(w);                                  /* releases the lease */

    config.dbPath = DIR "/vr.db";
    config.checkpointInterval = 0;                            /* the contract's */
    config.verifyOnly = 1;
    HppliteNode *v = hpplite_node_create(&config);
    if (!v) FAIL("open verifier");
    hpplite_node_set_l1(v, l1);
    if (hpplite_node_sync_from_l1(v, 1) < 0) { hpplite_node_destroy(v); FAIL("sync"); }
    hpplite_node_start(v);
    if (scalar(v->db, "SELECT count(*) FROM t") != 3) { hpplite_node_destroy(v); FAIL("rebuilt content"); }
    if (v->ctx->nRootsCompared < 1 || v->ctx->nRootMismatches != 0) { hpplite_node_destroy(v); FAILF("roots compared %llu, mismatches %llu", (unsigned long long)v->ctx->nRootsCompared, (unsigned long long)v->ctx->nRootMismatches); }
    {
        HppliteL1State *st = hpplite_l1_get_state(l1);
        int ok = st && st->lastCheckpointHeight == 2 && v->ctx->rootCmpHeight == 2
                 && memcmp(st->lastStateRoot, v->ctx->rootCmp, 32) == 0;
        hpplite_l1_state_free(st);
        if (!ok) { hpplite_node_destroy(v); FAIL("the on-chain checkpoint root at height 2 should equal the rebuilt root there"); }
    }
    WANT_STATUS(v->db, "state", "verify", "as a verifier");
    if (v->hasLease) { hpplite_node_destroy(v); FAIL("a verifier took the lease"); }
    hpplite_node_exec(v, "INSERT INTO t VALUES(99)", NULL);  /* a verifier's own write: never posted */
    uint64_t h = hpplite_node_flush_batch(v);
    uint64_t last = 0, total = 0;
    hpplite_l1_get_da_state(l1, &last, &total, NULL);
    int lease = v->hasLease, pending = hpplite_node_pending_count(v);
    hpplite_node_destroy(v);
    hpplite_l1_disconnect(l1);
    if (h != 0 || last != 3) FAILF("a verifier posted: flush %llu, chain height %llu", (unsigned long long)h, (unsigned long long)last);
    if (lease) FAIL("a verifier claimed the lease to post");
    if (pending != 1) FAIL("the verifier's own row should stay pending (and unposted)");
    PASS();
}

/* Another connection on the file holds the write lock until released (or
** for ms when set); an ALTER first when asked, committed before the hold:
** the writer's triggers are then stale for the schema its statement is
** prepared on */
struct holder { HppliteNode *f; int alter, ms; volatile int held, release; };
static void *hold_lock(void *a) {
    struct holder *h = a;
    if (h->alter) sqlite3_exec(h->f->db, "BEGIN IMMEDIATE; ALTER TABLE t ADD COLUMN c DEFAULT 'x'; COMMIT", NULL, NULL, NULL);
    sqlite3_exec(h->f->db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    h->held = 1;
    if (h->ms) usleep((useconds_t)h->ms * 1000);
    else while (!h->release) usleep(1000);
    sqlite3_exec(h->f->db, "COMMIT", NULL, NULL, NULL);
    return NULL;
}
static double now_ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0; }
/* the writer's busy handler: a wait of ten 50 ms sleeps, as busy_timeout 500
** would do, counting the waits it starts (SQLite passes count 0 to open one).
** "One wait, not two" is proven by that count, not by the clock: on a loaded
** CI runner every sleep overshoots and one wait has taken 1.3 s */
static int count_waits(void *p, int count) {
    if (count == 0) ++*(int *)p;
    if (count >= 10) return 0;
    usleep(50000);
    return 1;
}
/* one step of sql on db while h holds the lock: the step's result and its
** time; the hold is released after the step and joined */
static int step_under_hold(sqlite3 *db, const char *sql, struct holder *h, double *pMs) {
    sqlite3_stmt *s = NULL;
    pthread_t t;
    int rc;
    double t0;
    h->held = 0; h->release = 0;
    pthread_create(&t, NULL, hold_lock, h);
    while (!h->held) usleep(1000);
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return SQLITE_ERROR;
    t0 = now_ms();
    rc = sqlite3_step(s);
    *pMs = now_ms() - t0;
    sqlite3_finalize(s);
    h->release = 1;
    pthread_join(t, NULL);
    return rc;
}

/* 21. The write lock held elsewhere past busy_timeout (#92): one wait, then
** SQLite's own SQLITE_BUSY (retryable), not a refusal; the retry is rows */
static void test_lock_contention(void) {
    TEST("write lock held elsewhere past busy_timeout: one wait and SQLITE_BUSY (in a transaction too, after a schema change too, never a refusal); retried, random() and the write under the new schema are rows");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/lk.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = { "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", "INSERT INTO t(v) VALUES(0)", NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    HppliteNode *f = open_node(DIR "/lk.db", l1);     /* second connection, same file: it captures its writes too */
    if (!f) FAIL("second connection open");
    int waits = 0;
    sqlite3_busy_handler(w->db, count_waits, &waits);
    struct holder h = { f, 0, 0, 0, 0 };
    double ms = 0;
    int rc;

    /* a value row capture takes and text refuses, under a lock held throughout */
    waits = 0;
    rc = step_under_hold(w->db, "INSERT INTO t(v) VALUES(random())", &h, &ms);
    if (rc != SQLITE_BUSY) FAILF("random() under a held lock: rc %d (%s) after %.0f ms, want SQLITE_BUSY (%d)", rc, sqlite3_errmsg(w->db), ms, SQLITE_BUSY);
    if (waits != 1) FAILF("random() under a held lock: SQLITE_BUSY after %d busy waits in %.0f ms (one wait, not two)", waits, ms);
    if (sqlite3_exec(w->db, "INSERT INTO t(v) VALUES(random())", NULL, NULL, NULL) != SQLITE_OK) FAILF("retry after the lock was released: %s", sqlite3_errmsg(w->db));

    /* the lock released between the wait and a second one: SQLITE_BUSY or
    ** success, never a refusal (which the driver reports as "interrupted") */
    h.ms = 750;
    waits = 0;
    rc = step_under_hold(w->db, "INSERT INTO t(v) VALUES(random())", &h, &ms);
    h.ms = 0;
    if (rc != SQLITE_BUSY && rc != SQLITE_DONE) FAILF("random() with the lock released after 750 ms: rc %d (%s) after %.0f ms, want SQLITE_BUSY or SQLITE_DONE", rc, sqlite3_errmsg(w->db), ms);
    if (waits != 1) FAILF("random() with the lock released after 750 ms: %d busy waits (one, whatever the outcome)", waits);
    if (rc == SQLITE_BUSY && sqlite3_exec(w->db, "INSERT INTO t(v) VALUES(random())", NULL, NULL, NULL) != SQLITE_OK) FAILF("retry: %s", sqlite3_errmsg(w->db));

    /* the same inside the application's transaction: BUSY, and the transaction is still the application's */
    if (sqlite3_exec(w->db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) FAIL("BEGIN");
    waits = 0;
    rc = step_under_hold(w->db, "INSERT INTO t(v) VALUES(2)", &h, &ms);
    if (rc != SQLITE_BUSY) FAILF("in a transaction under a held lock: rc %d (%s) after %.0f ms, want SQLITE_BUSY", rc, sqlite3_errmsg(w->db), ms);
    if (waits != 1) FAILF("in a transaction under a held lock: SQLITE_BUSY after %d busy waits in %.0f ms (one wait, not two)", waits, ms);
    if (sqlite3_get_autocommit(w->db)) FAIL("in a transaction under a held lock: the transaction was rolled back (SQLITE_BUSY leaves it to the application)");
    if (sqlite3_exec(w->db, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK) FAILF("ROLLBACK: %s", sqlite3_errmsg(w->db));
    if (sqlite3_exec(w->db, "BEGIN; INSERT INTO t(v) VALUES(2); COMMIT", NULL, NULL, NULL) != SQLITE_OK) FAILF("retry in a transaction: %s", sqlite3_errmsg(w->db));

    /* the other connection moves the schema, then holds the lock: the writer's statement is prepared on
    ** the new schema while its triggers are for the old one */
    h.alter = 1;
    waits = 0;
    rc = step_under_hold(w->db, "INSERT INTO t(v) VALUES(random())", &h, &ms);
    h.alter = 0;
    if (rc != SQLITE_BUSY) FAILF("schema moved, lock held: rc %d (%s) after %.0f ms, want SQLITE_BUSY", rc, sqlite3_errmsg(w->db), ms);
    if (waits != 1) FAILF("schema moved, lock held: SQLITE_BUSY after %d busy waits in %.0f ms (one wait, not two)", waits, ms);
    if (sqlite3_exec(w->db, "INSERT INTO t(v) VALUES(random())", NULL, NULL, NULL) != SQLITE_OK) FAILF("retry after the schema moved: %s", sqlite3_errmsg(w->db));
    if (scalar(w->db, "SELECT count(*) FROM t WHERE c = 'x'") != 5) FAILF("writer premise: five rows with the new column, %lld", scalar(w->db, "SELECT count(*) FROM t WHERE c = 'x'"));
    if (scalar(w->db, "SELECT count(*) FROM _hpplite_pending WHERE rec IS NULL AND sql LIKE 'INSERT%'") != 0)
        FAILF("every INSERT should be a record, none text: %lld text INSERTs",
              scalar(w->db, "SELECT count(*) FROM _hpplite_pending WHERE rec IS NULL AND sql LIKE 'INSERT%'"));
    if (scalar(w->db, "SELECT count(*) FROM _hpplite_pending WHERE rec IS NOT NULL") < 5)
        FAILF("five inserted rows should have records: %lld", scalar(w->db, "SELECT count(*) FROM _hpplite_pending WHERE rec IS NOT NULL"));
    sqlite3_busy_timeout(w->db, 5000);                 /* the node's own, before waits goes out of scope */
    hpplite_node_destroy(f);
    const char *why = replica_matches(w, l1, "lk_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

int main(void) {
    printf("=== HPPLite row capture end to end (#19) ===\n");
    test_suite_init("rows", PRIVKEY, DIR);
    srand(1);
    test_nondeterministic_values();
    test_sequence();
    test_mixed_text_and_records();
    test_records_over_batch_size();
    test_size_cut_keeps_transactions();
    test_plaintext_entries();
    test_attach_refused();
    test_hooks_replaced();
    test_reopen();
    test_vacuum();
    test_trigger_guard();
    test_text_history_then_rows();
    test_drop_column();
    test_drop_table_cascade();
    test_deferred_finalize_not_reposted();
    test_temp_triggers();
    test_connection_pragmas();
    test_foreign_writes_detected();
    test_state_and_last_error();
    test_no_chain_keeps_the_log();
    test_verify_role();
    test_lock_contention();
    return test_report();
}
