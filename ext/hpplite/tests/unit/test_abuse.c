/*
** HPPLite abuse tests
**
** Inputs chosen to break the guards rather than to exercise them: every
** spelling of a refused statement, bytes on L1 that no HPPLite ever posted,
** statements sized to the limit, and random mutations of valid batches.
** The invariants are small and the same everywhere:
**   - a refused statement leaves no row behind and no pending log entry
**   - a batch that is not ours never changes the local database
**   - nothing we read from L1 can crash the process
**   - a replica never applies part of a transaction (one larger than a
**     batch spans several: tests/unit/test_big_txn.c)
**   - what capture lets through fits in one batch as posted
*/

#include "test_lib.h"
#include "hpplite_int.h"
#include "cipher.h"
#include "batch.h"

static const unsigned char PRIVKEY[32] = {
    0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,0x30,
    0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,0x40
};

#define DIR "/tmp/hpplite_test_abuse"

static HppliteNode *open_node_ex(const char *dbPath, HppliteL1 *l1, size_t maxBatchBytes, int noEncrypt) {
    TestNodeOpts o = { .maxBatchBytes = maxBatchBytes, .noEncrypt = noEncrypt, .checkpointInterval = TEST_INTERVAL_1000 };
    return test_open_node(dbPath, l1, &o);
}
static HppliteNode *open_node(const char *dbPath, HppliteL1 *l1) { return open_node_ex(dbPath, l1, 0, 0); }

/* Post arbitrary bytes at the next height under the node's own instance id,
** as a sequencer holding the lease (or an attacker who stole it) could. */
static int post_raw(HppliteNode *n, HppliteL1 *l1, uint64_t height, const uint8_t *data, size_t len) {
    char *tx = hpplite_l1_submit_batch(l1, n->instanceId, height, data, len);
    if (!tx) return -1;
    sqlite3_free(tx);
    return 0;
}

/* A valid batch at the given height, sealed with the node's key */
static uint8_t *sealed_batch(HppliteNode *n, uint64_t height, uint64_t claimedHeight, const char *sql, size_t *pLen) {
    HppliteBatch *b = hpplite_batch_new(claimedHeight);
    char *json;
    uint8_t *sealed;
    hpplite_batch_add_stmt(b, sql, -1, 0);
    json = hpplite_batch_to_json(b);
    sealed = hpplite_envelope_seal(n->cipher, n->encKey, height, HPPLITE_ENCODING_NONE,
                                   (const uint8_t *)json, strlen(json), pLen);
    sqlite3_free(json);
    hpplite_batch_free(b);
    return sealed;
}

static uint32_t rng_state = 0x9e3779b9u;
static uint32_t rng(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

/* ------------------------------------------------------------------------
** 1. Guard spelling corpus: every way of writing a refused statement
** ---------------------------------------------------------------------- */
static const char *aRefused[] = {
    /* our tables, through the parser (quoting, case, comments, schema prefix) */
    "INSERT INTO _hpplite_pending(txn, sql) VALUES(0, 'x')",
    "INSERT INTO \"_hpplite_pending\"(txn, sql) VALUES(0, 'x')",
    "INSERT INTO [_hpplite_pending](txn, sql) VALUES(0, 'x')",
    "INSERT INTO `_hpplite_pending`(txn, sql) VALUES(0, 'x')",
    "insert into _HPPLITE_PENDING(txn, sql) values(0, 'x')",
    "/* hi */ INSERT /* there */ INTO\n_hpplite_pending(txn, sql) VALUES(0, 'x')",
    "INSERT INTO main._hpplite_pending(txn, sql) VALUES(0, 'x')",
    "DELETE FROM _hpplite_pending",
    "DELETE FROM main.\"_hpplite_pending\" WHERE 1",
    "UPDATE _hpplite_meta SET value = 0",
    "UPDATE \"_HPPLITE_META\" SET value = 0 WHERE key = 'block_height'",
    "DROP TABLE _hpplite_pending",
    "DROP TABLE IF EXISTS [_hpplite_meta]",
    "ALTER TABLE _hpplite_pending RENAME TO t2",
    "ALTER TABLE _hpplite_pending ADD COLUMN z",
    "ALTER TABLE t RENAME TO _hpplite_t",
    "CREATE TABLE _hpplite_evil(x)",
    "CREATE TEMP TABLE _hpplite_evil(x)",
    "CREATE VIEW _hpplite_v AS SELECT 1",
    "CREATE INDEX ix ON _hpplite_pending(txn)",
    "CREATE TRIGGER tr AFTER INSERT ON t BEGIN DELETE FROM _hpplite_pending; END",
    "CREATE TRIGGER tr AFTER INSERT ON _hpplite_pending BEGIN INSERT INTO t VALUES(99); END",
    "CREATE TEMP TRIGGER tr AFTER INSERT ON _hpplite_pending BEGIN INSERT INTO t VALUES(99); END",
    "INSERT INTO t SELECT 1 FROM _hpplite_pending",
    "INSERT INTO t VALUES((SELECT count(*) FROM _hpplite_pending))",
    "INSERT INTO t SELECT sql FROM _hpplite_pending",
    "INSERT INTO t SELECT value FROM _hpplite_meta",
    "UPDATE t SET x = (SELECT value FROM _hpplite_meta WHERE key = 'block_height')",
    /* writes to another file are not captured */
    "ATTACH ':memory:' AS other",
    "ATTACH DATABASE '" DIR "/other.db' AS other",
    /* values replay cannot reproduce */
    "INSERT INTO t VALUES(random())",
    "INSERT INTO t VALUES(RANDOM())",
    "INSERT INTO t VALUES(random ( ))",
    "INSERT INTO t VALUES(\"random\"())",
    "INSERT INTO t VALUES(randomblob(4))",
    "INSERT INTO t VALUES(abs(random()))",
    "INSERT INTO t SELECT random()",
    "UPDATE t SET x = random()",
    "UPDATE t SET x = x WHERE random() > 0",
    "DELETE FROM t WHERE x = random()",
    "INSERT INTO t VALUES(last_insert_rowid())",
    "INSERT INTO t VALUES(changes())",
    "INSERT INTO t VALUES(total_changes())",
    "INSERT INTO t VALUES(sqlite_version())",
    "INSERT INTO t VALUES(sqlite_source_id())",
    "INSERT INTO t VALUES(sqlite_compileoption_used('THREADSAFE'))",
    "INSERT INTO t VALUES(datetime('now', 'localtime'))",
    "INSERT INTO t VALUES(datetime ( 'now' , 'localtime' ))",
    "INSERT INTO t VALUES(strftime('%s', 'now', 'utc'))",
    "INSERT INTO t VALUES(CASE WHEN 1 THEN random() ELSE 0 END)",
    "INSERT INTO t VALUES(1) RETURNING random()",
    "CREATE TABLE d(x DEFAULT (datetime('now', 'localtime')))",
    "CREATE TABLE d(x DEFAULT (random()))",
    "CREATE TABLE d(x, y AS (random()))",
    NULL
};

/* Allowed: function names in data, time functions with a fixed argument,
** reads of our tables, pragmas that do not change replay. */
static const char *aAllowed[] = {
    "INSERT INTO t VALUES('random()')",
    "INSERT INTO t VALUES('now')",
    "INSERT INTO t VALUES('datetime(''now'')')",
    "INSERT INTO t VALUES('CURRENT_TIMESTAMP')",
    "INSERT INTO t VALUES('_hpplite_pending')",
    "INSERT INTO t VALUES('DELETE FROM _hpplite_pending')",
    "INSERT INTO t VALUES('it''s now or never')",
    "INSERT INTO t VALUES(upper('now'))",
    "INSERT INTO t SELECT 1 WHERE 'x' IN ('now', 'later')",
    "INSERT INTO t VALUES(length(('now')))",
    "INSERT INTO t VALUES(datetime('2024-01-01', '+1 day'))",
    "INSERT INTO t VALUES(datetime('2024-01-01 00:00:00'))",
    "INSERT INTO t VALUES(date('2024-01-01', '+1 day'))",
    "INSERT INTO t VALUES(strftime('%Y', '2024-01-01'))",
    "INSERT INTO t VALUES(unixepoch('2024-01-01'))",
    "INSERT INTO t VALUES(julianday('2024-01-01'))",
    "INSERT INTO t VALUES(abs(-1))",
    "INSERT INTO t VALUES(length('now'))",
    "CREATE TABLE nowadays(x)",
    "INSERT INTO nowadays VALUES(1)",
    "CREATE TABLE random_things(date_col, time_col)",
    "INSERT INTO random_things VALUES(1, 2)",
    "CREATE TABLE \"date\"(x)",
    "INSERT INTO \"date\" VALUES(1)",
    "UPDATE t SET x = x WHERE x = 'now'",
    /* foreign keys and recursive triggers replay in the writer's mode (#52) */
    "PRAGMA foreign_keys",
    "PRAGMA foreign_keys = ON",
    "PRAGMA foreign_keys=1",
    "PRAGMA foreign_keys(1)",
    "pragma FOREIGN_KEYS = true",
    "PRAGMA main.foreign_keys = yes",
    "PRAGMA foreign_keys = OFF",
    "PRAGMA recursive_triggers = ON",
    "PRAGMA recursive_triggers(1)",
    "PRAGMA recursive_triggers = 0",
    "PRAGMA journal_mode",
    "PRAGMA user_version = 3",
    /* ALTER runs SQLite's internal functions (sqlite_rename_table ...),
    ** which are not in pragma_function_list: still SQLite's, not the app's */
    "CREATE TABLE alt(a, b, c)",
    "ALTER TABLE alt RENAME COLUMN b TO bb",
    "ALTER TABLE alt RENAME TO alt2",
    "ALTER TABLE alt2 DROP COLUMN c",
    "ALTER TABLE alt2 ADD COLUMN d DEFAULT 4",
    /* the clock replays: each statement's time is recorded and pinned on
    ** replay (#56); the replica check below compares the values */
    "INSERT INTO t VALUES(datetime('now'))",
    "INSERT INTO t VALUES(DATETIME ( 'now' ))",
    "INSERT INTO t VALUES(datetime('NOW'))",
    "INSERT INTO t VALUES(datetime(\n'now'))",
    "INSERT INTO t VALUES(timediff('now', '2024-01-01'))",
    "INSERT INTO t VALUES(upper(datetime('now')))",
    "INSERT INTO t VALUES(date('now'))",
    "INSERT INTO t VALUES(time('now'))",
    "INSERT INTO t VALUES(julianday('now'))",
    "INSERT INTO t VALUES(unixepoch('now'))",
    "INSERT INTO t VALUES(datetime())",
    "INSERT INTO t VALUES(date())",
    "INSERT INTO t VALUES(date( ))",
    "INSERT INTO t VALUES(time())",
    "INSERT INTO t VALUES(julianday())",
    "INSERT INTO t VALUES(unixepoch())",
    "INSERT INTO t VALUES(strftime('%s'))",
    "INSERT INTO t VALUES(strftime('%Y-%m-%d'))",
    "INSERT INTO t VALUES(strftime('%s', 'now'))",
    "INSERT INTO t VALUES(CURRENT_TIMESTAMP)",
    "INSERT INTO t VALUES(current_timestamp)",
    "INSERT INTO t VALUES(CURRENT_TIME)",
    "INSERT INTO t VALUES(CURRENT_DATE)",
    "INSERT INTO t VALUES(1 + (SELECT CURRENT_DATE))",
    "UPDATE t SET x = CURRENT_TIMESTAMP",
    "INSERT INTO t VALUES('x' || datetime('now'))",
    "INSERT INTO t VALUES(coalesce(NULL, datetime('now')))",
    "INSERT INTO t VALUES((SELECT datetime('now')))",
    "CREATE TABLE dnow(x DEFAULT CURRENT_TIMESTAMP, y DEFAULT (unixepoch()))",
    "INSERT INTO dnow DEFAULT VALUES",
    /* reads never reach the log: nothing to replay, nothing to refuse (#54) */
    "SELECT random()",
    "SELECT randomblob(4)",
    "SELECT x FROM t ORDER BY random()",
    "SELECT last_insert_rowid()",
    "SELECT changes(), total_changes()",
    "SELECT sqlite_version()",
    "WITH r(v) AS (SELECT random()) SELECT v FROM r",
    "SELECT * FROM t WHERE x > abs(random()) % 2",
    /* a cascade with foreign keys on is replayed in that mode (#52) */
    "PRAGMA foreign_keys = ON",
    "CREATE TABLE ap(id INTEGER PRIMARY KEY)",
    "CREATE TABLE ac(id INTEGER PRIMARY KEY, p INTEGER REFERENCES ap(id) ON DELETE CASCADE)",
    "INSERT INTO ap VALUES(1)",
    "INSERT INTO ac VALUES(10, 1)",
    "DELETE FROM ap WHERE id = 1",
    "PRAGMA foreign_keys = OFF",
    NULL
};

static void test_guard_spellings(void) {
    TEST("every spelling of a refused statement is refused; look-alikes pass");
    SKIP_IF_ROWS("a write reading HPPLite's tables records its values");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/guards.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(0)", NULL);
    int pendingBefore = hpplite_node_pending_count(n);
    int rowsBefore = count_rows(n->db, "t");
    uint64_t heightBefore = hpplite_node_get_height(n);

    for (int i = 0; aRefused[i]; i++) {
        char *err = NULL;
        int rc = sqlite3_exec(n->db, aRefused[i], NULL, NULL, &err);
        if (rc == SQLITE_OK) FAILF("accepted: %s", aRefused[i]);
        sqlite3_free(err);
        if (hpplite_node_pending_count(n) != pendingBefore) FAILF("pending log changed by: %s", aRefused[i]);
        if (count_rows(n->db, "t") != rowsBefore) FAILF("rows changed by: %s", aRefused[i]);
        if (count_rows(n->db, "_hpplite_pending") < 0) FAILF("log table gone after: %s", aRefused[i]);
        if (hpplite_node_get_height(n) != heightBefore) FAILF("height changed by: %s", aRefused[i]);
        if (sqlite3_get_autocommit(n->db) == 0) FAILF("left a transaction open: %s", aRefused[i]);
    }
    /* none of the DDL took effect either */
    if (count_rows(n->db, "_hpplite_evil") != -1) FAIL("_hpplite_evil was created");
    if (count_rows(n->db, "d") != -1) FAIL("table with a localtime default was created");
    if (sqlite3_db_filename(n->db, "other") != NULL) FAIL("ATTACH took effect");

    for (int i = 0; aAllowed[i]; i++) {
        char *err = NULL;
        int rc = sqlite3_exec(n->db, aAllowed[i], NULL, NULL, &err);
        if (rc != SQLITE_OK) FAILF("refused: %s (%s)", aAllowed[i], err ? err : "?");
        sqlite3_free(err);
    }
    /* what was allowed replays to the same root on a fresh replica */
    if (hpplite_node_flush_batch(n) <= 0) FAIL("flush");
    unsigned char rootA[32], rootB[32];
    hpplite_node_get_state_root(n, rootA);
    HppliteNode *r = open_node(DIR "/guards_replica.db", l1);
    if (!r) FAIL("open replica");
    if (r->state == HPPLITE_STATE_ERROR) FAIL("replica sync failed");
    hpplite_node_get_state_root(r, rootB);
    if (memcmp(rootA, rootB, 32) != 0) FAIL("allowed statements did not replay to the same root");
    if (count_rows(r->db, "t") != count_rows(n->db, "t")) FAIL("replica row count differs");
    hpplite_node_destroy(r);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* An application-defined function (Django registers ~40 on every
** connection) is not on the replica: a write that calls it, directly or
** through a view or trigger, cannot replay and must be refused at the
** write; so must a generated column or index that would call it. Reads that call it are fine (#53). */
static void appFn(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    sqlite3_result_int64(ctx, sqlite3_value_int64(argv[0]) * 7);
}
static const char *aAppRefused[] = {
    "INSERT INTO t VALUES(app_fn(1))",
    "INSERT INTO t VALUES(APP_FN(1))",
    "INSERT INTO t SELECT app_fn(x) FROM t",
    "UPDATE t SET x = app_fn(x)",
    "UPDATE t SET x = x + 1 WHERE app_fn(x) = 7",
    "DELETE FROM t WHERE app_fn(x) = 14",
    "INSERT INTO t VALUES(1) RETURNING app_fn(x)",
    "INSERT INTO t SELECT y FROM v_app",                     /* through a view */
    "INSERT INTO trig_src VALUES(1)",                        /* through a trigger body */
    /* a generated column or expression index calls it on every later write,
    ** where the authorizer does not look: so the CREATE itself is refused */
    "CREATE TABLE d(x, y AS (app_fn(x)))",
    "CREATE INDEX ix_app ON t(app_fn(x))",
    NULL
};
static void test_app_defined_functions(void) {
    TEST("writes that call an application-defined function are refused; reads are not");
    SKIP_IF_ROWS("accepted, replicas equal (test_rows_e2e)");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/appfn.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1)", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(2)", NULL);
    /* the objects that hide a call; created before the function exists, as a
    ** replica would see them, so their creation itself is a legal write */
    hpplite_node_exec(n, "CREATE VIEW v_app AS SELECT app_fn(x) AS y FROM t", NULL);
    hpplite_node_exec(n, "CREATE TABLE trig_src(a)", NULL);
    hpplite_node_exec(n, "CREATE TRIGGER tr_app AFTER INSERT ON trig_src BEGIN INSERT INTO t VALUES(app_fn(new.a)); END", NULL);
    if (sqlite3_create_function(n->db, "app_fn", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, appFn, NULL, NULL) != SQLITE_OK)
        FAIL("register app_fn");
    int pendingBefore = hpplite_node_pending_count(n);
    int rowsBefore = count_rows(n->db, "t");
    for (int i = 0; aAppRefused[i]; i++) {
        char *err = NULL;
        int rc = sqlite3_exec(n->db, aAppRefused[i], NULL, NULL, &err);
        if (rc == SQLITE_OK) FAILF("accepted: %s", aAppRefused[i]);
        sqlite3_free(err);
        if (hpplite_node_pending_count(n) != pendingBefore) FAILF("pending log changed by: %s", aAppRefused[i]);
        if (count_rows(n->db, "t") != rowsBefore) FAILF("rows changed by: %s", aAppRefused[i]);
        if (sqlite3_get_autocommit(n->db) == 0) FAILF("left a transaction open: %s", aAppRefused[i]);
    }
    /* reads may call it */
    static const char *aRead[] = { "SELECT app_fn(x) FROM t", "SELECT * FROM v_app",
                                   "SELECT x FROM t WHERE app_fn(x) = 7", NULL };
    for (int i = 0; aRead[i]; i++) {
        char *err = NULL;
        if (sqlite3_exec(n->db, aRead[i], NULL, NULL, &err) != SQLITE_OK) FAILF("read refused: %s (%s)", aRead[i], err ? err : "?");
        sqlite3_free(err);
    }
    /* a prepared write is checked on every run, not only the first: the
    ** same text is fine until a trigger that calls the function appears */
    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(n->db, "INSERT INTO t VALUES(?1)", -1, &ins, NULL) != SQLITE_OK) FAIL("prepare");
    sqlite3_bind_int(ins, 1, 5);
    if (sqlite3_step(ins) != SQLITE_DONE) FAIL("plain prepared insert refused");
    sqlite3_reset(ins);
    hpplite_node_exec(n, "CREATE TRIGGER tr_t AFTER INSERT ON t WHEN new.x = 6 BEGIN INSERT INTO trig_src VALUES(0); END", NULL);
    sqlite3_finalize(ins);
    ins = NULL;
    if (sqlite3_prepare_v2(n->db, "INSERT INTO t VALUES(?1)", -1, &ins, NULL) != SQLITE_OK) FAIL("prepare 2");
    sqlite3_bind_int(ins, 1, 6);
    if (sqlite3_step(ins) == SQLITE_DONE) FAIL("insert that fires a trigger calling app_fn was accepted");
    sqlite3_finalize(ins);
    hpplite_node_exec(n, "DROP TRIGGER tr_t", NULL);
    /* everything accepted replays to the writer's root */
    if (hpplite_node_flush_batch(n) <= 0) FAIL("flush");
    unsigned char rootA[32], rootB[32];
    hpplite_node_get_state_root(n, rootA);
    HppliteNode *r = open_node(DIR "/appfn_replica.db", l1);
    if (!r) FAIL("open replica");
    if (r->state == HPPLITE_STATE_ERROR) FAIL("replica sync failed");
    if (r->ctx->nReplaySkipped != 0) FAIL("replica skipped a statement");
    hpplite_node_get_state_root(r, rootB);
    int rowsR = count_rows(r->db, "t"), rowsW = count_rows(n->db, "t");
    hpplite_node_destroy(r);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    if (memcmp(rootA, rootB, 32) != 0) FAIL("replica root differs");
    if (rowsR != rowsW) FAIL("replica row count differs");
    PASS();
}

/* "interrupted" / "not authorized" is all a driver reports for a refusal:
** the reason must be one query away for the application (or its agent) */
static void test_last_refusal(void) {
    TEST("the reason of the last refusal is in hpplite_status.last_refusal");
    SKIP_IF_ROWS("random() is accepted");
    reset_dir();
    HppliteNode *n = open_node(DIR "/why.db", NULL);
    if (!n) FAIL("open");
    char why[512];
    text_of(n->db, "SELECT last_refusal FROM hpplite_status", why, sizeof(why));
    if (why[0]) FAIL("a refusal before any statement");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    if (sqlite3_exec(n->db, "INSERT INTO t VALUES(random())", NULL, NULL, NULL) == SQLITE_OK) FAIL("random() accepted");
    text_of(n->db, "SELECT last_refusal FROM hpplite_status", why, sizeof(why));
    if (!strstr(why, "random()") || !strstr(why, "INSERT INTO t VALUES(random())")) FAILF("reason/statement missing: '%s'", why);
    if (sqlite3_exec(n->db, "DELETE FROM _hpplite_pending", NULL, NULL, NULL) == SQLITE_OK) FAIL("log write accepted");
    text_of(n->db, "SELECT last_refusal FROM hpplite_status", why, sizeof(why));
    if (!strstr(why, "_hpplite_")) FAILF("authorizer reason missing: '%s'", why);
    hpplite_node_destroy(n);
    PASS();
}

/* An application function on a connection: its name reaches a replica's
** function list nowhere */
static void fn_app_double(sqlite3_context *c, int argc, sqlite3_value **argv) {
    (void)argc;
    sqlite3_result_int64(c, 2 * sqlite3_value_int64(argv[0]));
}

/* The text rules, one place (textVerdict, #104): for a fixed set of
** statements, the rule each one falls under, as hpplite_status.last_refusal
** says it; and which of them determinism=warn lets through */
static void test_text_verdicts(void) {
    TEST("why a statement logged as text is refused, rule by rule (#104); warn mode lets a value through, never a function");
    SKIP_IF_ROWS("values are rows");
    reset_dir();
    static const struct { const char *sql, *why, *advice; } aCase[] = {
        { "INSERT INTO t VALUES(random())", "non-deterministic function random()", "compute the value in the application and bind it" },
        { "INSERT INTO t VALUES(datetime('now', 'localtime'))", "machine's time zone", "bind the value instead" },
        { "UPDATE t SET x = last_insert_rowid()", "non-deterministic function last_insert_rowid()", "compute the value in the application and bind it" },
        { "INSERT INTO t VALUES(app_double(2))", "app_double() is defined by the application", "compute the value in the application and bind it" },
        { "INSERT INTO t SELECT count(*) FROM _hpplite_pending", "may not reference _hpplite_ tables", NULL },
        { NULL, NULL, NULL } };
    HppliteNode *n = open_node(DIR "/verdict.db", NULL);
    if (!n) FAIL("open");
    sqlite3_create_function(n->db, "app_double", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, fn_app_double, NULL, NULL);
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    for (int i = 0; aCase[i].sql; i++) {
        char why[512];
        int rc = sqlite3_exec(n->db, aCase[i].sql, NULL, NULL, NULL);
        if (rc != SQLITE_INTERRUPT) FAILF("%s: rc %d, want SQLITE_INTERRUPT (refused)", aCase[i].sql, rc);
        text_of(n->db, "SELECT last_refusal FROM hpplite_status", why, sizeof(why));
        if (!strstr(why, aCase[i].why)) FAILF("%s: refused for '%s', want the rule '%s'", aCase[i].sql, why, aCase[i].why);
        if (aCase[i].advice && !strstr(why, aCase[i].advice)) FAILF("%s: no advice '%s' in '%s'", aCase[i].sql, aCase[i].advice, why);
        if (!aCase[i].advice && strstr(why, "bind")) FAILF("%s: advice where there is nothing to advise: '%s'", aCase[i].sql, why);
    }
    if (count_rows(n->db, "t") != 0) FAIL("a refused statement ran");
    if (n->ctx->nNondeterministic != 4) FAILF("nondeterministic counted %llu, want 4 (the _hpplite_ reference is not a determinism matter)",
                                              (unsigned long long)n->ctx->nNondeterministic);
    hpplite_node_destroy(n);

    /* determinism=warn: a value replay cannot reproduce is let through and
    ** counted; a function the replica does not have is still refused */
    TestNodeOpts o = { .determinismWarn = 1, .checkpointInterval = TEST_INTERVAL_1000 };
    HppliteNode *w = test_open_node(DIR "/verdict_warn.db", NULL, &o);
    if (!w) FAIL("open warn");
    sqlite3_create_function(w->db, "app_double", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, fn_app_double, NULL, NULL);
    hpplite_node_exec(w, "CREATE TABLE t(x)", NULL);
    if (sqlite3_exec(w->db, "INSERT INTO t VALUES(random())", NULL, NULL, NULL) != SQLITE_OK) FAIL("warn: random() refused");
    if (sqlite3_exec(w->db, "UPDATE t SET x = last_insert_rowid()", NULL, NULL, NULL) != SQLITE_OK) FAIL("warn: last_insert_rowid() refused");
    if (sqlite3_exec(w->db, "INSERT INTO t VALUES(app_double(2))", NULL, NULL, NULL) != SQLITE_INTERRUPT) FAIL("warn: an application function let through");
    if (sqlite3_exec(w->db, "INSERT INTO t SELECT count(*) FROM _hpplite_pending", NULL, NULL, NULL) != SQLITE_INTERRUPT) FAIL("warn: a _hpplite_ reference let through");
    if (count_rows(w->db, "t") != 1 || w->ctx->nNondeterministic != 3) FAILF("warn: %d rows, counted %llu; want 1 and 3", count_rows(w->db, "t"), (unsigned long long)w->ctx->nNondeterministic);
    /* a value rule hit first does not hide a later rule (review of #104): every rule is applied */
    if (sqlite3_exec(w->db, "INSERT INTO t VALUES(datetime('now', 'localtime') || app_double(1))", NULL, NULL, NULL) != SQLITE_INTERRUPT)
        FAIL("warn: a time zone value let an application function through");
    if (sqlite3_exec(w->db, "INSERT INTO t SELECT datetime('now', 'localtime') FROM _hpplite_pending", NULL, NULL, NULL) != SQLITE_INTERRUPT)
        FAIL("warn: a time zone value let a _hpplite_ reference through");
    if (count_rows(w->db, "t") != 1 || w->ctx->nNondeterministic != 6) FAILF("warn: %d rows, counted %llu; want 1 and 6 (every determinism rule hit counts)", count_rows(w->db, "t"), (unsigned long long)w->ctx->nNondeterministic);
    /* a non-deterministic builtin before an application function in the same statement does not hide it (#115) */
    if (sqlite3_exec(w->db, "INSERT INTO t VALUES(random() + app_double(1))", NULL, NULL, NULL) != SQLITE_INTERRUPT)
        FAIL("warn: random() before app_double() let the application function through");
    if (count_rows(w->db, "t") != 1) FAIL("warn: the hidden application function ran");
    hpplite_node_destroy(w);
    PASS();
}

/* Migration tools keep their version in the header (PRAGMA user_version):
** a replica that becomes the writer reads it to decide what to migrate */
static void test_header_pragmas_replicate(void) {
    TEST("PRAGMA user_version / application_id writes reach a replica");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/uv.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    if (sqlite3_exec(n->db, "PRAGMA user_version = 7", NULL, NULL, NULL) != SQLITE_OK) FAIL("user_version");
    if (sqlite3_exec(n->db, "pragma main.APPLICATION_ID=1095779411", NULL, NULL, NULL) != SQLITE_OK) FAIL("application_id");
    hpplite_node_exec(n, "BEGIN", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1)", NULL);
    hpplite_node_exec(n, "PRAGMA user_version = 8", NULL);           /* in a migration's transaction */
    hpplite_node_exec(n, "COMMIT", NULL);
    hpplite_node_exec(n, "BEGIN", NULL);
    hpplite_node_exec(n, "PRAGMA user_version = 99", NULL);
    hpplite_node_exec(n, "ROLLBACK", NULL);                          /* rolled back: not replayed */
    sqlite3_exec(n->db, "PRAGMA user_version", NULL, NULL, NULL);    /* a read: not captured */
    if (hpplite_node_flush_batch(n) <= 0) FAIL("flush");
    unsigned char ra[32], rb[32];
    hpplite_node_get_state_root(n, ra);
    HppliteNode *r = open_node(DIR "/uv_replica.db", l1);
    if (!r) FAIL("replica");
    sqlite3_stmt *st;
    int uv = -1, aid = -1;
    if (sqlite3_prepare_v2(r->db, "SELECT user_version FROM pragma_user_version", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) uv = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(r->db, "SELECT application_id FROM pragma_application_id", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) aid = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    hpplite_node_get_state_root(r, rb);
    hpplite_node_destroy(r);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    if (uv != 8) FAILF("replica user_version %d, expected 8", uv);
    if (aid != 1095779411) FAILF("replica application_id %d", aid);
    if (memcmp(ra, rb, 32) != 0) FAIL("roots differ");
    PASS();
}

/* What the application reads back about its own writes must be what plain
** SQLite says: compare-and-set and optimistic locking depend on changes()
** (found by an agent-built inventory app: an order for 9 units with 3 in
** stock was accepted because a 0-row UPDATE reported 1) */
static sqlite3_int64 changes_after(sqlite3 *db, const char *sql) {
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) return -100;
    return sqlite3_changes64(db);
}
static void test_changes_are_the_applications(void) {
    TEST("sqlite3_changes() after a write is the write's own count, whatever HPPLite logged");
    reset_dir();
    HppliteNode *n = open_node(DIR "/chg.db", NULL);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE p(id INTEGER PRIMARY KEY, stock INTEGER, at TEXT)", NULL);
    hpplite_node_exec(n, "INSERT INTO p VALUES(1, 3, NULL), (2, 5, NULL), (3, 7, NULL)", NULL);
    struct { const char *sql; sqlite3_int64 want; } a[] = {
        { "UPDATE p SET stock = stock - 9 WHERE id = 1 AND stock >= 9", 0 },
        { "DELETE FROM p WHERE id = 42", 0 },
        { "UPDATE p SET stock = stock - 1 WHERE id = 1 AND stock >= 1", 1 },
        { "UPDATE p SET at = datetime('now')", 3 },                  /* reads the clock (#56) */
        { "UPDATE p SET at = CURRENT_TIMESTAMP WHERE id > 1", 2 },
        { "UPDATE p SET at = datetime('now') WHERE id = 99", 0 },
        { "INSERT INTO p VALUES(4, 1, CURRENT_TIMESTAMP)", 1 },
        { NULL, 0 } };
    for (int i = 0; a[i].sql; i++) {
        sqlite3_int64 got = changes_after(n->db, a[i].sql);
        if (got != a[i].want) { hpplite_node_destroy(n); FAILF("%s: changes() = %lld, expected %lld", a[i].sql, (long long)got, (long long)a[i].want); }
        /* the explicit read sees the same */
        sqlite3_stmt *st;
        sqlite3_int64 viaSql = -1;
        if (sqlite3_prepare_v2(n->db, "SELECT changes()", -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) viaSql = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
        if (viaSql != a[i].want) { hpplite_node_destroy(n); FAILF("%s: SELECT changes() = %lld", a[i].sql, (long long)viaSql); }
    }
    hpplite_node_destroy(n);
    PASS();
}

/* node:sqlite's StatementSync.get() steps a statement once and resets it:
** for INSERT ... RETURNING the write is complete after that one step, and
** SQLite reports no end of statement to the trace (found by an agent-built
** habit tracker: rows written that way never reached the chain) */
static void test_returning_stepped_once(void) {
    TEST("INSERT/UPDATE ... RETURNING stepped once and reset, then run again: every run captured");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/ret.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE r(id INTEGER PRIMARY KEY, v TEXT)", NULL);
    sqlite3_stmt *ins = NULL, *upd = NULL;
    if (sqlite3_prepare_v2(n->db, "INSERT INTO r(v) VALUES(?1) RETURNING id", -1, &ins, NULL) != SQLITE_OK) FAIL("prepare");
    if (sqlite3_prepare_v2(n->db, "UPDATE r SET v = v || '!' WHERE id = ?1 RETURNING v", -1, &upd, NULL) != SQLITE_OK) FAIL("prepare upd");
    for (int i = 0; i < 5; i++) {
        char v[16];
        snprintf(v, sizeof(v), "row%d", i);
        sqlite3_bind_text(ins, 1, v, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(ins) != SQLITE_ROW) FAIL("insert step");
        sqlite3_reset(ins);                                   /* .get(): one step, then reset */
        sqlite3_bind_int(upd, 1, i + 1);
        if (sqlite3_step(upd) != SQLITE_ROW) FAIL("update step");
        sqlite3_reset(upd);
    }
    sqlite3_finalize(ins);
    sqlite3_finalize(upd);
    if (!test_pending_is(hpplite_node_pending_count(n), 11)) { printf("(pending %d) ", hpplite_node_pending_count(n)); FAIL("not every run was captured"); }
    if (n->ctx->nAppStmtActive != 0) FAIL("statements left counted as running");
    if (hpplite_node_flush_batch(n) <= 0) FAIL("flush");
    unsigned char ra[32], rb[32];
    hpplite_node_get_state_root(n, ra);
    HppliteNode *r = open_node(DIR "/ret_replica.db", l1);
    if (!r) FAIL("replica");
    int rows = count_rows(r->db, "r");
    hpplite_node_get_state_root(r, rb);
    hpplite_node_destroy(r);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    if (rows != 5) FAIL("replica rows");
    if (memcmp(ra, rb, 32) != 0) FAIL("replica root differs (the updates)");
    PASS();
}

static void test_last_insert_rowid_is_the_applications(void) {
    TEST("last_insert_rowid() after a write that inserts nothing is the application's, not the log's");
    reset_dir();
    HppliteNode *n = open_node(DIR "/rowid.db", NULL);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE u(id INTEGER PRIMARY KEY, k TEXT UNIQUE)", NULL);
    hpplite_node_exec(n, "CREATE TABLE w(k TEXT PRIMARY KEY, v) WITHOUT ROWID", NULL);
    hpplite_node_exec(n, "INSERT INTO u(id, k) VALUES(41, 'a')", NULL);
    sqlite3_int64 a = sqlite3_last_insert_rowid(n->db);
    hpplite_node_exec(n, "INSERT OR IGNORE INTO u(k) VALUES('a')", NULL);   /* ignored */
    sqlite3_int64 b = sqlite3_last_insert_rowid(n->db);
    hpplite_node_exec(n, "INSERT INTO w VALUES('x', 1)", NULL);             /* no rowid */
    sqlite3_int64 c = sqlite3_last_insert_rowid(n->db);
    hpplite_node_exec(n, "UPDATE u SET k = 'b'", NULL);
    sqlite3_int64 d = sqlite3_last_insert_rowid(n->db);
    hpplite_node_destroy(n);
    if (a != 41 || b != 41 || c != 41 || d != 41) FAILF("last_insert_rowid %lld %lld %lld %lld, expected 41 each", (long long)a, (long long)b, (long long)c, (long long)d);
    PASS();
}

/* "Changed no rows" is not "had no effect": an INSERT of zero rows into an
** AUTOINCREMENT table creates its sqlite_sequence entry (Django's table
** rebuild of an empty table does exactly this) */
static void test_zero_row_insert_into_autoincrement(void) {
    TEST("a zero-row INSERT that creates a sqlite_sequence entry reaches the replica");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/seq.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE src(id INTEGER PRIMARY KEY, v)", NULL);
    hpplite_node_exec(n, "CREATE TABLE a(id INTEGER PRIMARY KEY AUTOINCREMENT, v)", NULL);
    hpplite_node_exec(n, "INSERT INTO a(id, v) SELECT id, v FROM src", NULL);   /* 0 rows */
    if (hpplite_node_flush_batch(n) <= 0) FAIL("flush");
    int seqW = count_rows(n->db, "sqlite_sequence");
    HppliteNode *r = open_node(DIR "/seq_replica.db", l1);
    if (!r) FAIL("replica");
    int seqR = count_rows(r->db, "sqlite_sequence");
    hpplite_node_destroy(r);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    if (seqW != 1) FAIL("test premise: the writer has the sequence entry");
    if (seqR != seqW) FAILF("replica sqlite_sequence has %d rows, writer %d", seqR, seqW);
    PASS();
}

/* VACUUM was refused (its internal ATTACH) and, had it run, was not logged:
** it may renumber the rowids of tables without an INTEGER PRIMARY KEY, and
** a statement after it that names a rowid must hit the same row on a
** replica. Logged once it has run; replayed outside a transaction. */
static void test_vacuum_logged(void) {
    TEST("VACUUM runs, is logged once it has run, and rowid-addressed writes after it replay onto the same rows");
    SKIP_IF_PAGES("VACUUM lands in the file as pages like any other write; nothing is logged");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/vac.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE plain(name, v)", NULL);
    hpplite_node_exec(n, "WITH RECURSIVE k(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM k WHERE i < 200) "
                         "INSERT INTO plain SELECT 'p' || i, i FROM k", NULL);
    hpplite_node_exec(n, "DELETE FROM plain WHERE v % 2 = 0", NULL);
    if (sqlite3_exec(n->db, "VACUUM", NULL, NULL, NULL) != SQLITE_OK) FAILF("VACUUM: %s", sqlite3_errmsg(n->db));
    if (sqlite3_exec(n->db, "VACUUM INTO '" DIR "/copy.db'", NULL, NULL, NULL) != SQLITE_OK) FAILF("VACUUM INTO: %s", sqlite3_errmsg(n->db));
    hpplite_node_exec(n, "UPDATE plain SET v = -1 WHERE rowid = 7", NULL);
    hpplite_node_exec(n, "DELETE FROM plain WHERE rowid = 8", NULL);
    int logged = -1;
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(n->db, "SELECT count(*) FROM _hpplite_pending WHERE sql = 'VACUUM'", -1, &q, NULL) == SQLITE_OK) {
        if (sqlite3_step(q) == SQLITE_ROW) logged = sqlite3_column_int(q, 0);
        sqlite3_finalize(q);
    }
    while (hpplite_node_pending_count(n) > 0 && hpplite_node_flush_batch(n) > 0) { }
    unsigned char ra[32], rb[32];
    hpplite_node_get_state_root(n, ra);
    hpplite_node_destroy(n);
    HppliteNode *r = open_node(DIR "/vac_replica.db", l1);
    if (!r) FAIL("replica");
    hpplite_node_get_state_root(r, rb);
    hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    if (logged != 1) FAILF("%d VACUUM entries logged, want 1 (VACUUM INTO changes nothing here)", logged);
    if (memcmp(ra, rb, 32) != 0) FAIL("replica state root differs");
    PASS();
}

/* PRAGMA count_changes=1 (deprecated, supported): every INSERT/UPDATE/DELETE
** prepared on the connection returns its change count as a row - HPPLite's
** own writes there too, which then looked failed: every write was refused.
** Found by SQLite's fkey2.test run with HPPLite on every connection. */
static void test_count_changes(void) {
    TEST("PRAGMA count_changes=1: writes run, are captured, and replay");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/cc.db", l1);
    if (!n) FAIL("open");
    int ok = sqlite3_exec(n->db, "PRAGMA count_changes = 1; CREATE TABLE t(id INTEGER PRIMARY KEY, v);"
                                 "INSERT INTO t(v) VALUES('a'), ('b'); UPDATE t SET v = v || '!'; DELETE FROM t WHERE id = 1;"
                                 "BEGIN; INSERT INTO t(v) VALUES('c'); COMMIT; VACUUM", NULL, NULL, NULL) == SQLITE_OK;
    while (hpplite_node_pending_count(n) > 0 && hpplite_node_flush_batch(n) > 0) { }
    unsigned char ra[32], rb[32];
    hpplite_node_get_state_root(n, ra);
    hpplite_node_destroy(n);
    HppliteNode *r = open_node(DIR "/cc_replica.db", l1);
    if (!r) FAIL("replica");
    hpplite_node_get_state_root(r, rb);
    int rows = count_rows(r->db, "t");
    hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    if (!ok) FAIL("a write was refused");
    if (rows != 2 || memcmp(ra, rb, 32) != 0) FAIL("replica differs");
    PASS();
}

/* The file stays in WAL: leaving it failed as "database is locked" (the
** bookkeeping connection holds it), found by SQLite's changes.test */
static void test_journal_mode_kept(void) {
    TEST("leaving WAL is refused with the reason; reading the mode and WAL itself are fine");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/jm.db", l1);
    if (!n) FAIL("open");
    int readOk = sqlite3_exec(n->db, "PRAGMA journal_mode", NULL, NULL, NULL) == SQLITE_OK;
    int walOk = sqlite3_exec(n->db, "PRAGMA journal_mode = WAL", NULL, NULL, NULL) == SQLITE_OK;
    int offRc = sqlite3_exec(n->db, "PRAGMA journal_mode = DELETE", NULL, NULL, NULL);
    char why[256] = "";
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(n->db, "SELECT last_refusal FROM hpplite_status", -1, &q, NULL) == SQLITE_OK) {
        if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_text(q, 0)) snprintf(why, sizeof(why), "%s", sqlite3_column_text(q, 0));
        sqlite3_finalize(q);
    }
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    if (!readOk || !walOk) FAIL("reading the mode or setting WAL failed");
    if (offRc != SQLITE_AUTH) FAILF("leaving WAL: rc %d, want SQLITE_AUTH", offRc);
    if (!strstr(why, "WAL")) FAILF("the refusal does not say why: %s", why);
    PASS();
}

/* The timer's bookkeeping commit can land between an application
** transaction's read and its write however the flags are checked at its
** start: stress it with the timer thread flushing continuously */
#include <pthread.h>
typedef struct { HppliteNode *n; volatile int stop; int flushes; } Flusher;
static void *flusher(void *arg) {
    Flusher *f = arg;
    while (!f->stop) { hpplite_node_flush_batch(f->n); f->flushes++; }
    return NULL;
}
static void test_read_then_write_under_timer(void) {
    TEST("read-then-write transactions never hit 'database is locked' while flushes run");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    hpplite_l1_mock_set_batch_size_limit(l1, 64 * 1024 * 1024);
    HppliteNode *n = open_node(DIR "/race2.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE s(id INTEGER PRIMARY KEY, v INTEGER)", NULL);
    hpplite_node_exec(n, "INSERT INTO s VALUES(1, 0)", NULL);
    Flusher f = { n, 0, 0 };
    pthread_t th;
    pthread_create(&th, NULL, flusher, &f);
    int locked = 0, done = 0;
    for (int i = 0; i < 1500; i++) {
        sqlite3_stmt *st;
        if (sqlite3_exec(n->db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) { locked++; continue; }
        if (sqlite3_prepare_v2(n->db, "SELECT v FROM s WHERE id = 1", -1, &st, NULL) == SQLITE_OK) { sqlite3_step(st); sqlite3_finalize(st); }
        int rc = sqlite3_exec(n->db, "UPDATE s SET v = v + 1 WHERE id = 1", NULL, NULL, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_exec(n->db, "INSERT INTO s(v) VALUES(1)", NULL, NULL, NULL);
        if (rc == SQLITE_OK && sqlite3_exec(n->db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) done++;
        else { locked++; sqlite3_exec(n->db, "ROLLBACK", NULL, NULL, NULL); }
    }
    f.stop = 1;
    pthread_join(th, NULL);
    hpplite_node_flush_batch(n);
    sqlite3_int64 v = -1;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(n->db, "SELECT v FROM s WHERE id = 1", -1, &st, NULL) == SQLITE_OK) { if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0); sqlite3_finalize(st); }
    unsigned char ra[32], rb[32];
    hpplite_node_get_state_root(n, ra);
    HppliteNode *r = open_node(DIR "/race2_replica.db", l1);
    if (r) hpplite_node_get_state_root(r, rb);
    if (r) hpplite_node_destroy(r);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    printf("(%d flushes, %d done, %d locked) ", f.flushes, done, locked);
    if (locked) FAIL("application transactions were locked out by bookkeeping");
    if (v != done) FAIL("counter");
    if (!r || memcmp(ra, rb, 32) != 0) FAIL("replica root differs");
    PASS();
}

/* An autocommit statement that changes nothing commits with its log row,
** and the row is removed just after: a batch built in between carried a row
** the log no longer had, and adopting it (confirmation lost) failed. Found
** booting Ghost (#58): "L1 batch 2 does not match local pending SQL". */
typedef struct { HppliteNode *n; volatile int stop; int flushes; } Flusher2;
static void *flusher2(void *arg) {
    Flusher2 *f = arg;
    while (!f->stop) { hpplite_node_flush_batch(f->n); f->flushes++; }
    return NULL;
}
static void test_no_op_rows_vs_adoption(void) {
    TEST("batches built while no-op statements commit and unlog: adoption never mismatches");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    hpplite_l1_mock_set_batch_size_limit(l1, 64 * 1024 * 1024);
    HppliteNode *n = open_node(DIR "/noop.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(id INTEGER PRIMARY KEY, x)", NULL);
    hpplite_l1_mock_set_lose_confirmation(l1, 1);          /* every batch is finished by adoption */
    Flusher2 f = { n, 0, 0 };
    pthread_t th;
    pthread_create(&th, NULL, flusher2, &f);
    for (int i = 0; i < 3000 && n->state != HPPLITE_STATE_ERROR; i++) {
        char sql[64];
        sqlite3_exec(n->db, "UPDATE t SET x = x WHERE id < 0", NULL, NULL, NULL);   /* logs, then unlogs */
        snprintf(sql, sizeof(sql), "INSERT INTO t(x) VALUES(%d)", i);
        sqlite3_exec(n->db, sql, NULL, NULL, NULL);
    }
    f.stop = 1;
    pthread_join(th, NULL);
    hpplite_l1_mock_set_lose_confirmation(l1, 0);
    int state = n->state;
    hpplite_node_flush_batch(n);
    int left = hpplite_node_pending_count(n);
    unsigned char ra[32], rb[32];
    hpplite_node_get_state_root(n, ra);
    HppliteNode *r = open_node(DIR "/noop_replica.db", l1);
    if (r) hpplite_node_get_state_root(r, rb);
    int rows = r ? count_rows(r->db, "t") : -1;
    if (r) hpplite_node_destroy(r);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    printf("(%d flushes) ", f.flushes);
    if (state == HPPLITE_STATE_ERROR) FAIL("an adoption mismatched: a batch carried a row the log removed");
    if (left != 0) FAIL("pending left");
    if (rows != 3000 || memcmp(ra, rb, 32) != 0) FAIL("replica differs");
    PASS();
}

/* A transaction that reads, then writes (Knex's deferred BEGIN, Django's
** atomic(): the usual shape) must not be broken by a bookkeeping commit in
** between: in WAL mode the read snapshot could no longer become a write */
/* #133: a bookkeeping begin in read mode that fails inside a write
** transaction of bookkeeping's (the application thread asks for the state
** root while the timer thread finalizes) must not erase the write's hold on
** the transaction gate: the gate then stayed taken for the connection's
** life and every later application statement spun on it (the suite hang). */
static void test_gate_survives_nested_begin(void) {
    TEST("the transaction gate is released by the write that took it even after a failed read-mode begin inside it (#133)");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/gate.db", l1);
    if (!n) FAIL("open");
    HppliteCtx *c = n->ctx;
    if (c->bkDb == c->db) { hpplite_node_destroy(n); FAIL("premise: bookkeeping has its own connection"); }
    if (hppliteBkBegin(c, 1) != SQLITE_OK) { hpplite_node_destroy(n); FAIL("the write begin"); }
    if (c->txnGate != 2) { hpplite_node_destroy(n); FAIL("the write holds the gate"); }
    if (hppliteBkBegin(c, 0) == SQLITE_OK) { hpplite_node_destroy(n); FAIL("premise: a begin inside a transaction fails"); }
    hppliteBkEnd(c, SQLITE_OK);
    int gate = c->txnGate;
    hpplite_node_destroy(n);
    if (gate != 0) FAILF("the gate is still %d after the write ended: the failed read begin erased its hold", gate);
    PASS();
}

static void test_bookkeeping_waits_for_app_transaction(void) {
    TEST("a flush during an application's read-then-write transaction does not lock it out");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/rw.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE s(id INTEGER PRIMARY KEY, v INTEGER)", NULL);
    hpplite_node_exec(n, "INSERT INTO s VALUES(1, 10)", NULL);
    if (hpplite_node_flush_batch(n) != 1) FAIL("batch 1");
    hpplite_node_exec(n, "INSERT INTO s VALUES(2, 20)", NULL);            /* pending: the flush has work */
    if (sqlite3_exec(n->db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) FAIL("begin");
    sqlite3_int64 v = -1;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(n->db, "SELECT v FROM s WHERE id = 1", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);                                         /* the read transaction stays open */
    }
    hpplite_node_flush_batch(n);                                      /* as the timer would, mid-transaction */
    char *err = NULL;
    int rc = sqlite3_exec(n->db, "UPDATE s SET v = v + 1 WHERE id = 1", NULL, NULL, &err);
    if (rc != SQLITE_OK) { printf("[%s] ", err ? err : "?"); sqlite3_free(err); sqlite3_exec(n->db, "ROLLBACK", NULL, NULL, NULL); hpplite_node_destroy(n); FAIL("the write after the read failed"); }
    if (sqlite3_exec(n->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) FAIL("commit");
    hpplite_node_flush_batch(n);
    if (hpplite_node_pending_count(n) != 0) FAIL("pending left");
    unsigned char ra[32], rb[32];
    hpplite_node_get_state_root(n, ra);
    HppliteNode *r = open_node(DIR "/rw_replica.db", l1);
    if (!r) FAIL("replica");
    hpplite_node_get_state_root(r, rb);
    hpplite_node_destroy(r);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    if (v != 10) FAIL("read");
    if (memcmp(ra, rb, 32) != 0) FAIL("replica root differs");
    PASS();
}

/* ------------------------------------------------------------------------
** 2. Adversarial bytes on L1
** ---------------------------------------------------------------------- */

/* Sequencer A posts a real batch 1, then <bytes> land at height 2 (whoever
** posted them). A fresh replica syncs. Returns the replica; the caller
** checks state, height and rows. */
static HppliteNode *sync_after(HppliteL1 **pL1, const uint8_t *bytes, size_t len, const char *tag) {
    HppliteL1 *l1 = hpplite_l1_create_mock();
    char path[256];
    snprintf(path, sizeof(path), DIR "/adv_%s.db", tag);
    HppliteNode *a = open_node(path, l1);
    if (!a) return NULL;
    hpplite_node_exec(a, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(a, "INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(a) != 1) { hpplite_node_destroy(a); return NULL; }
    if (post_raw(a, l1, 2, bytes, len) != 0) { hpplite_node_destroy(a); return NULL; }
    hpplite_node_destroy(a);
    snprintf(path, sizeof(path), DIR "/adv_%s_replica.db", tag);
    *pL1 = l1;
    return open_node(path, l1);
}

static void test_wrong_height_batch(void) {
    TEST("a sealed batch claiming another height is corrupt, not applied");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/wh.db", l1);
    if (!a) FAIL("open");
    hpplite_node_exec(a, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(a, "INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(a) != 1) FAIL("batch 1");
    size_t len = 0;
    /* sealed for slot 2 (the AEAD passes) but the JSON says height 9 */
    uint8_t *bytes = sealed_batch(a, 2, 9, "DELETE FROM t", &len);
    if (!bytes || post_raw(a, l1, 2, bytes, len) != 0) FAIL("post");
    free(bytes);
    hpplite_node_destroy(a);

    HppliteNode *r = open_node(DIR "/wh_replica.db", l1);
    if (!r) FAIL("open replica");
    if (count_rows(r->db, "t") != 1) FAIL("the DELETE was applied");
    if (hpplite_node_get_height(r) != 2) FAIL("height moved past the corrupt batch");
    if (r->state != HPPLITE_STATE_ERROR) FAIL("replica should stop in ERROR");
    if (r->ctx->nForeignBatches != 0) FAIL("corrupt is not foreign: it must not be skipped");
    hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    PASS();
}

static void test_plaintext_on_encrypting_rollup(void) {
    TEST("plaintext SQL at height 2 of an encrypting rollup is skipped as foreign");
    reset_dir();
    HppliteL1 *l1 = NULL;
    const char *evil = "{\"height\":2,\"timestamp\":0,\"txns\":[{\"sql\":\"DELETE FROM t\",\"txn\":0}]}";
    HppliteNode *r = sync_after(&l1, (const uint8_t *)evil, strlen(evil), "plain");
    if (!r) FAIL("setup");
    if (count_rows(r->db, "t") != 1) FAIL("plaintext SQL was applied");
    if (r->ctx->nForeignBatches != 1) FAIL("should be skipped as a foreign batch");
    if (hpplite_node_get_height(r) != 3) FAIL("height should step over the foreign batch");
    if (r->state == HPPLITE_STATE_ERROR) FAIL("a skipped batch is not an error");
    hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    PASS();
}

static void test_plaintext_at_height_1(void) {
    TEST("plaintext batch 1 on an encrypting rollup: refuse, never replay");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/p1.db", l1);
    if (!a) FAIL("open");
    const char *evil = "{\"height\":1,\"timestamp\":0,\"txns\":[{\"sql\":\"CREATE TABLE t(x)\",\"txn\":0},{\"sql\":\"INSERT INTO t VALUES(1)\",\"txn\":0}]}";
    if (post_raw(a, l1, 1, (const uint8_t *)evil, strlen(evil)) != 0) FAIL("post");
    hpplite_node_destroy(a);
    HppliteNode *r = open_node(DIR "/p1_replica.db", l1);
    if (!r) FAIL("open replica");
    if (count_rows(r->db, "t") != -1) FAIL("plaintext batch 1 was replayed");
    if (r->state != HPPLITE_STATE_ERROR) FAIL("replica should be in ERROR");
    if (hpplite_node_get_height(r) != 1) FAIL("height must not move");
    /* and it must not post over it either */
    hpplite_node_exec(r, "CREATE TABLE u(x)", NULL);
    if (hpplite_node_flush_batch(r) > 0) FAIL("an ERROR node must not post");
    hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    PASS();
}

static void test_garbage_on_l1(void) {
    TEST("garbage, truncated and oversized envelopes at height 2: never applied, never crash");
    reset_dir();
    static const char *aText[] = {
        "", "{", "}", "[]", "null", "{\"height\":2}", "{\"height\":2,\"txns\":null}",
        "{\"height\":2,\"txns\":[{\"sql\":null}]}", "{\"height\":2,\"txns\":[{\"sql\":\"DELETE FROM t\",\"txn\":-1}]}",
        "{\"height\":\"2\",\"txns\":[]}", "{\"height\":18446744073709551616,\"txns\":[]}",
        "{\"height\":2,\"txns\":[{\"sql\":\"\\u0000DELETE FROM t\"}]}",
        "{\"height\":2,\"txns\":[{\"sql\":\"DELETE FROM t\"}],\"height\":3}",
        "HPE2", "HPE2\x00\x00\x00\x00", "HPE1", "HPE9garbage", "\xff\xfe\xfd", "\x00\x00\x00\x00\x00\x00\x00\x00",
        NULL
    };
    char tag[32];
    for (int i = 0; aText[i]; i++) {
        HppliteL1 *l1 = NULL;
        size_t len = (i == 0) ? 0 : strlen(aText[i]);
        if (i == 14) len = 8;          /* the one with embedded NULs */
        if (i == 18) len = 8;
        snprintf(tag, sizeof(tag), "g%d", i);
        HppliteNode *r = sync_after(&l1, (const uint8_t *)aText[i], len, tag);
        if (!r) { if (len == 0) continue; FAILF("setup %d", i); }   /* the mock refuses empty data: fine */
        if (count_rows(r->db, "t") != 1) FAILF("case %d changed the table", i);
        if (hpplite_node_get_height(r) < 2) FAILF("case %d lost batch 1", i);
        hpplite_node_destroy(r);
        hpplite_l1_disconnect(l1);
    }
    /* a valid envelope, truncated at every length, and extended */
    HppliteL1 *l0 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/trunc_src.db", l0);
    if (!a) FAIL("open");
    size_t fullLen = 0;
    uint8_t *full = sealed_batch(a, 2, 2, "DELETE FROM t", &fullLen);
    if (!full) FAIL("seal");
    hpplite_node_destroy(a);
    hpplite_l1_disconnect(l0);
    for (size_t cut = 1; cut < fullLen; cut += (fullLen > 64 ? fullLen / 32 : 1)) {
        HppliteL1 *l1 = NULL;
        snprintf(tag, sizeof(tag), "t%zu", cut);
        HppliteNode *r = sync_after(&l1, full, cut, tag);
        if (!r) FAILF("setup cut %zu", cut);
        if (count_rows(r->db, "t") != 1) FAILF("truncated at %zu bytes was applied", cut);
        hpplite_node_destroy(r);
        hpplite_l1_disconnect(l1);
    }
    {
        uint8_t *ext = malloc(fullLen + 4096);
        memcpy(ext, full, fullLen);
        memset(ext + fullLen, 'A', 4096);
        HppliteL1 *l1 = NULL;
        HppliteNode *r = sync_after(&l1, ext, fullLen + 4096, "ext");
        if (!r) FAIL("setup ext");
        if (count_rows(r->db, "t") != 1) FAIL("extended envelope was applied");
        hpplite_node_destroy(r);
        hpplite_l1_disconnect(l1);
        free(ext);
    }
    free(full);
    PASS();
}

static void test_mutation_fuzz(void) {
    TEST("random mutations of a valid sealed batch: open() rejects every one, replica untouched");
    reset_dir();
    HppliteL1 *l0 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/fuzz_src.db", l0);
    if (!a) FAIL("open");
    size_t fullLen = 0;
    uint8_t *full = sealed_batch(a, 2, 2, "DELETE FROM t", &fullLen);
    if (!full) FAIL("seal");
    uint8_t key[32];
    memcpy(key, a->encKey, 32);
    hpplite_node_destroy(a);
    hpplite_l1_disconnect(l0);

    /* the sanity half: the unmutated envelope opens */
    {
        const HppliteCipher *scheme = NULL;
        uint8_t *p = hpplite_envelope_open(key, 2, full, fullLen, NULL, &scheme);
        if (!p) FAIL("the valid envelope does not open");
        free(p);
        p = hpplite_envelope_open(key, 3, full, fullLen, NULL, &scheme);
        if (p) { free(p); FAIL("the envelope opens at the wrong height"); }
    }
    /* 2000 direct mutations: bit flips, byte sets, truncations, insertions */
    uint8_t *m = malloc(fullLen + 64);
    int rejected = 0;
    for (int i = 0; i < 2000; i++) {
        size_t len = fullLen;
        memcpy(m, full, fullLen);
        switch (rng() % 4) {
            case 0: m[rng() % len] ^= (uint8_t)(1u << (rng() % 8)); break;
            case 1: m[rng() % len] = (uint8_t)rng(); break;
            case 2: len = 1 + rng() % (len - 1); break;
            case 3: { size_t at = rng() % len; memmove(m + at + 1, m + at, len - at); m[at] = (uint8_t)rng(); len++; break; }
        }
        if (len == fullLen && memcmp(m, full, fullLen) == 0) { rejected++; continue; }   /* a no-op mutation */
        const HppliteCipher *scheme = NULL;
        uint8_t *p = hpplite_envelope_open(key, 2, m, len, NULL, &scheme);
        if (p) { free(p); FAILF("mutation %d opened", i); }
        rejected++;
    }
    /* and through the node, a sample of them */
    char tag[32];
    for (int i = 0; i < 24; i++) {
        size_t len = fullLen;
        memcpy(m, full, fullLen);
        if (i % 3 == 0) m[rng() % len] ^= (uint8_t)(1u << (rng() % 8));
        else if (i % 3 == 1) len = 1 + rng() % (len - 1);
        else m[rng() % len] ^= 0xff;
        HppliteL1 *l1 = NULL;
        snprintf(tag, sizeof(tag), "m%d", i);
        HppliteNode *r = sync_after(&l1, m, len, tag);
        if (!r) FAILF("setup %d", i);
        if (count_rows(r->db, "t") != 1) FAILF("mutation %d through the node was applied", i);
        hpplite_node_destroy(r);
        hpplite_l1_disconnect(l1);
    }
    free(m);
    free(full);
    if (rejected != 2000) FAIL("count");
    PASS();
}

static void test_json_fuzz(void) {
    TEST("batch JSON parser survives mutations of a valid batch");
    HppliteBatch *b = hpplite_batch_new(7);
    hpplite_batch_add_stmt(b, "INSERT INTO t VALUES('a\"b\\c\\n')", 3, 0);
    hpplite_batch_add_stmt(b, "UPDATE t SET x = 'é'", -1, 1);
    char *json = hpplite_batch_to_json(b);
    hpplite_batch_free(b);
    if (!json) FAIL("to_json");
    size_t n = strlen(json);
    char *m = malloc(n + 64);
    for (int i = 0; i < 3000; i++) {
        size_t len = n;
        memcpy(m, json, n + 1);
        switch (rng() % 4) {
            case 0: m[rng() % len] = (char)("{}[]\",:\\0n9-tfu "[rng() % 17]); break;
            case 1: m[rng() % len] = (char)rng(); break;
            case 2: len = rng() % len; m[len] = 0; break;
            case 3: { size_t at = rng() % len; memmove(m + at + 1, m + at, len - at + 1); m[at] = (char)("{}[]\",:\\"[rng() % 8]); len++; break; }
        }
        for (size_t k = 0; k < len; k++) if (m[k] == 0) m[k] = ' ';   /* keep it a C string */
        HppliteBatch *p = hpplite_batch_from_json(m);
        if (p) {
            /* whatever parsed must be internally consistent */
            for (int t = 0; t < p->nTxns; t++) if (!p->aTxns[t].zSql) { free(m); sqlite3_free(json); FAILF("mutation %d: NULL sql in parsed batch", i); }
            hpplite_batch_free(p);
        }
    }
    free(m);
    sqlite3_free(json);
    PASS();
}

/* ------------------------------------------------------------------------
** 3. Size limits
** ---------------------------------------------------------------------- */
static void test_escaped_size_limit(void) {
    TEST("quote-heavy SQL larger than a batch as posted: accepted, posted in pieces within the limit (#59)");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    const size_t LIMIT = 4096;
    HppliteNode *n = open_node_ex(DIR "/size.db", l1, LIMIT, 0);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    if (hpplite_node_flush_batch(n) != 1) FAIL("batch 1");

    /* raw length well under the limit, escaped length over it: every
    ** character is a quote or a control byte that JSON doubles or sextuples */
    char *sql = malloc(LIMIT);
    size_t at = 0;
    at += (size_t)snprintf(sql, LIMIT, "INSERT INTO t VALUES('");
    while (at < 1600) { sql[at++] = '"'; sql[at++] = '\\'; sql[at++] = '\x01'; sql[at++] = '\n'; }
    at += (size_t)snprintf(sql + at, LIMIT - at, "')");
    if (at >= LIMIT - 200) FAIL("test statement too long for the limit");
    int rc = sqlite3_exec(n->db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) FAIL("a statement whose escaped form exceeds the batch was refused");

    /* same raw length, plain characters: accepted, and the batch as posted fits */
    at = (size_t)snprintf(sql, LIMIT, "INSERT INTO t VALUES('");
    while (at < 1600) sql[at++] = 'a';
    at += (size_t)snprintf(sql + at, LIMIT - at, "')");
    rc = sqlite3_exec(n->db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) FAIL("a plain statement of the same length was refused");
    /* near the limit, still plain, still must fit when posted */
    at = (size_t)snprintf(sql, LIMIT, "INSERT INTO t VALUES('");
    while (at < LIMIT - 400) sql[at++] = 'b';
    at += (size_t)snprintf(sql + at, LIMIT - at, "')");
    rc = sqlite3_exec(n->db, sql, NULL, NULL, NULL);
    int secondAccepted = (rc == SQLITE_OK);
    free(sql);
    int h;
    while ((h = hpplite_node_flush_batch(n)) > 0) {
        size_t len = 0;
        uint8_t *raw = hpplite_l1_get_batch(l1, (uint64_t)h, &len);
        if (!raw) FAIL("batch missing on L1");
        free(raw);
        if (len > LIMIT) FAILF("batch %d is %zu bytes as posted, limit %zu", h, len, LIMIT);
    }
    if (hpplite_node_pending_count(n) != 0) FAIL("pending rows left after flushing");
    if (count_rows(n->db, "t") != 2 + secondAccepted) FAIL("row count");
    unsigned char ra[32], rb[32];
    hpplite_node_get_state_root(n, ra);
    HppliteNode *r = open_node_ex(DIR "/size_replica.db", l1, LIMIT, 0);
    if (!r) FAIL("replica");
    hpplite_node_get_state_root(r, rb);
    int rrows = count_rows(r->db, "t");
    hpplite_node_destroy(r);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    if (rrows != 2 + secondAccepted || memcmp(ra, rb, 32) != 0) FAIL("replica differs (escapes cut wrong?)");
    PASS();
}

/* Each transaction inserts rows tagged with its number. Decode every batch
** posted and check that no tag appears in two batches. */
static void test_transaction_boundary(void) {
    TEST("the size cut never splits a transaction across batches");
    SKIP_IF_ROWS("reads statement text; records: test_rows_e2e");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    const size_t LIMIT = 3000;
    HppliteNode *n = open_node_ex(DIR "/txn.db", l1, LIMIT, 0);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(tag, pad)", NULL);
    if (hpplite_node_flush_batch(n) != 1) FAIL("batch 1");
    /* 12 transactions of varying size: 1..9 statements of ~120 bytes */
    const int NTXN = 12;
    for (int k = 0; k < NTXN; k++) {
        int nStmt = 1 + (k * 7) % 9;
        hpplite_node_exec(n, "BEGIN", NULL);
        for (int s = 0; s < nStmt; s++) {
            char sql[256];
            snprintf(sql, sizeof(sql), "INSERT INTO t VALUES('txn%02d', '%080d')", k, s);
            if (sqlite3_exec(n->db, sql, NULL, NULL, NULL) != SQLITE_OK) FAIL("insert");
        }
        hpplite_node_exec(n, "COMMIT", NULL);
    }
    int seenIn[12];
    for (int k = 0; k < NTXN; k++) seenIn[k] = -1;
    int last = hpplite_node_flush_batch(n);      /* posts the whole backlog, batch after batch */
    if (last < 2) FAIL("flush");
    int nBatches = last - 1;
    for (int h = 2; h <= last; h++) {
        HppliteBatch *b = hpplite_node_fetch_batch(n, (uint64_t)h);
        if (!b) FAIL("fetch posted batch");
        for (int i = 0; i < b->nTxns; i++) {
            const char *p = strstr(b->aTxns[i].zSql, "'txn");
            if (!p) continue;
            int k = atoi(p + 4);
            if (seenIn[k] == -1) seenIn[k] = h;
            else if (seenIn[k] != h) { hpplite_batch_free(b); FAILF("transaction %d appears in batches %d and %d", k, seenIn[k], h); }
            /* group ids within a batch are contiguous and start at 0 */
            if (i > 0 && b->aTxns[i].txn != b->aTxns[i - 1].txn && b->aTxns[i].txn != b->aTxns[i - 1].txn + 1) {
                hpplite_batch_free(b); FAIL("transaction groups not contiguous");
            }
        }
        size_t len = 0;
        uint8_t *raw = hpplite_l1_get_batch(l1, (uint64_t)h, &len);
        if (raw) free(raw);
        if (len > LIMIT) { hpplite_batch_free(b); FAILF("batch %d is %zu bytes, limit %zu", h, len, LIMIT); }
        hpplite_batch_free(b);
    }
    if (nBatches < 2) FAIL("the limit did not split the backlog (test is not exercising the cut)");
    for (int k = 0; k < NTXN; k++) if (seenIn[k] == -1) FAILF("transaction %d was never posted", k);
    if (hpplite_node_pending_count(n) != 0) FAIL("pending left");
    /* replica agrees */
    unsigned char rootA[32], rootB[32];
    hpplite_node_get_state_root(n, rootA);
    HppliteNode *r = open_node_ex(DIR "/txn_replica.db", l1, LIMIT, 0);
    if (!r) FAIL("replica");
    hpplite_node_get_state_root(r, rootB);
    if (memcmp(rootA, rootB, 32) != 0) FAIL("replica root differs");
    hpplite_node_destroy(r);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

static void test_oversized_transaction(void) {
    TEST("a transaction that outgrows a batch is posted across batches within the limit (#57)");
    SKIP_IF_PAGES("these rows fit one deflated page batch under this limit; a commit cut into pieces is test_pagemode's (#152)");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    const size_t LIMIT = 2000;
    HppliteNode *n = open_node_ex(DIR "/big.db", l1, LIMIT, 0);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    if (hpplite_node_flush_batch(n) != 1) FAIL("batch 1");
    hpplite_node_exec(n, "BEGIN", NULL);
    for (int s = 0; s < 40; s++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES('%060d')", s);
        if (sqlite3_exec(n->db, sql, NULL, NULL, NULL) != SQLITE_OK) FAILF("statement %d of a large transaction refused", s);
    }
    if (hpplite_node_exec(n, "COMMIT", NULL) != SQLITE_OK) FAIL("commit");
    hpplite_node_exec(n, "INSERT INTO t VALUES('after')", NULL);
    int h = hpplite_node_flush_batch(n);
    if (h < 3) FAIL("the transaction did not span batches");
    if (hpplite_node_pending_count(n) != 0) FAIL("pending rows left: the sequencer cannot drain");
    for (int k = 2; k <= h; k++) {
        size_t len = 0;
        uint8_t *raw = hpplite_l1_get_batch(l1, (uint64_t)k, &len);
        if (raw) free(raw);
        if (len > LIMIT) FAILF("batch %d is %zu bytes, limit %zu", k, len, LIMIT);
    }
    /* a transaction that fits is still fine, in one batch */
    hpplite_node_exec(n, "BEGIN", NULL);
    for (int s = 0; s < 8; s++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES('%060d')", 100 + s);
        if (sqlite3_exec(n->db, sql, NULL, NULL, NULL) != SQLITE_OK) FAIL("a transaction that fits was refused");
    }
    if (hpplite_node_exec(n, "COMMIT", NULL) != SQLITE_OK) FAIL("commit");
    int h2 = hpplite_node_flush_batch(n);
    if (h2 != h + 1) FAIL("a fitting transaction should be one batch");
    unsigned char rootA[32], rootB[32];
    hpplite_node_get_state_root(n, rootA);
    HppliteNode *r = open_node_ex(DIR "/big_replica.db", l1, LIMIT, 0);
    if (!r) FAIL("replica");
    hpplite_node_get_state_root(r, rootB);
    if (memcmp(rootA, rootB, 32) != 0) FAIL("replica root differs");
    if (count_rows(r->db, "t") != count_rows(n->db, "t")) FAIL("replica rows");
    hpplite_node_destroy(r);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

int main(void) {
    printf("=== HPPLite abuse tests ===\n");
    test_suite_init("abuse", PRIVKEY, DIR);
    test_guard_spellings();
    test_app_defined_functions();
    test_last_refusal();
    test_text_verdicts();
    test_header_pragmas_replicate();
    test_changes_are_the_applications();
    test_returning_stepped_once();
    test_last_insert_rowid_is_the_applications();
    test_bookkeeping_waits_for_app_transaction();
    test_gate_survives_nested_begin();
    test_zero_row_insert_into_autoincrement();
    test_vacuum_logged();
    test_count_changes();
    test_journal_mode_kept();
    test_read_then_write_under_timer();
    test_no_op_rows_vs_adoption();
    test_wrong_height_batch();
    test_plaintext_on_encrypting_rollup();
    test_plaintext_at_height_1();
    test_garbage_on_l1();
    test_mutation_fuzz();
    test_json_fuzz();
    test_escaped_size_limit();
    test_transaction_boundary();
    test_oversized_transaction();
    return test_report();
}
