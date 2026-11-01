/*
** HPPLite core tests (HppliteCtx on :memory:, no node, no L1)
**
**   - content-hash state root: zero at genesis, changes with content, equal
**     for equal content, unchanged by rollback, independent of history,
**     covers WITHOUT ROWID / floats / blobs / views / indexes / triggers
**   - statement capture: what is logged, what is skipped, bound parameters
**   - commit / rollback callbacks
*/

#include "test_lib.h"
#include "batch.h"

static int commits = 0, rollbacks = 0;
static void on_commit(void *arg, unsigned char *root, int n) { (void)arg; (void)root; (void)n; commits++; }
static void on_rollback(void *arg) { (void)arg; rollbacks++; }

static int open_ctx(sqlite3 **pdb, HppliteCtx **pctx) {
    if (sqlite3_open(":memory:", pdb) != SQLITE_OK) return 0;
    if (hpplite_init(*pdb, pctx) != SQLITE_OK) return 0;
    return 1;
}

static void close_ctx(sqlite3 *db, HppliteCtx *ctx) {
    hpplite_shutdown(ctx);
    sqlite3_close(db);
}

static int exec(sqlite3 *db, const char *sql) { return exec_sql(db, sql) == SQLITE_OK; }

static int root_is_zero(const unsigned char *r) {
    unsigned char zero[HPPLITE_HASH_SIZE] = {0};
    return memcmp(r, zero, HPPLITE_HASH_SIZE) == 0;
}

static void test_root_tracks_content(void) {
    sqlite3 *db; HppliteCtx *ctx;
    unsigned char r0[32], r1[32], r2[32], r3[32], r4[32];
    TEST("state root tracks content through DDL/DML and rollback");
    if (!open_ctx(&db, &ctx)) FAIL("open");
    hpplite_get_state_root(ctx, r0);
    if (!root_is_zero(r0)) FAIL("genesis root should be zero");

    exec(db, "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT, age INT)");
    hpplite_get_state_root(ctx, r1);
    if (root_is_zero(r1)) FAIL("schema alone should change the root");

    exec(db, "INSERT INTO users(name, age) VALUES('Alice', 30)");
    hpplite_get_state_root(ctx, r2);
    if (memcmp(r1, r2, 32) == 0) FAIL("insert should change the root");

    exec(db, "UPDATE users SET age = 31 WHERE name = 'Alice'");
    hpplite_get_state_root(ctx, r3);
    if (memcmp(r2, r3, 32) == 0) FAIL("update should change the root");

    exec(db, "BEGIN; INSERT INTO users(name, age) VALUES('David', 40); ROLLBACK;");
    hpplite_get_state_root(ctx, r4);
    if (memcmp(r3, r4, 32) != 0) FAIL("rollback must leave the root unchanged");

    exec(db, "UPDATE users SET age = 30 WHERE name = 'Alice'");
    hpplite_get_state_root(ctx, r4);
    if (memcmp(r2, r4, 32) != 0) FAIL("reverting the content must revert the root");

    exec(db, "DELETE FROM users");
    exec(db, "DROP TABLE users");
    hpplite_get_state_root(ctx, r4);
    if (!root_is_zero(r4)) FAIL("dropping all user schema should return to the zero root");
    close_ctx(db, ctx);
    PASS();
}

static void test_root_independent_of_history(void) {
    sqlite3 *a, *b; HppliteCtx *ca, *cb;
    unsigned char ra[32], rb[32];
    TEST("equal content gives equal root regardless of history");
    if (!open_ctx(&a, &ca) || !open_ctx(&b, &cb)) FAIL("open");
    exec(a, "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, age INT)");
    exec(a, "INSERT INTO t VALUES(1,'Alice',30)");
    exec(a, "BEGIN; INSERT INTO t VALUES(2,'Bob',25); INSERT INTO t VALUES(3,'Charlie',35); COMMIT;");
    exec(a, "UPDATE t SET age=31 WHERE id=1");
    exec(a, "DELETE FROM t WHERE id=2");

    exec(b, "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, age INT)");
    exec(b, "INSERT INTO t VALUES(3,'Charlie',35)");
    exec(b, "INSERT INTO t VALUES(1,'Alice',31)");

    hpplite_get_state_root(ca, ra);
    hpplite_get_state_root(cb, rb);
    if (memcmp(ra, rb, 32) != 0) FAIL("roots differ for identical content");

    exec(b, "UPDATE t SET name='alice' WHERE id=1");
    hpplite_get_state_root(cb, rb);
    if (memcmp(ra, rb, 32) == 0) FAIL("a one-character difference must change the root");
    close_ctx(a, ca); close_ctx(b, cb);
    PASS();
}

static void test_root_covers_schema_kinds_and_types(void) {
    sqlite3 *db; HppliteCtx *ctx;
    unsigned char prev[32], cur[32], direct[32];
    TEST("root covers WITHOUT ROWID, floats, blobs, views, indexes, triggers");
    if (!open_ctx(&db, &ctx)) FAIL("open");
    exec(db, "CREATE TABLE kv(k TEXT PRIMARY KEY, v REAL, b BLOB) WITHOUT ROWID");
    exec(db, "INSERT INTO kv VALUES('b', 2.5, x'0102'), ('a', -0.0, NULL)");
    hpplite_get_state_root(ctx, prev);
    if (root_is_zero(prev)) FAIL("WITHOUT ROWID content not hashed");

    exec(db, "UPDATE kv SET v = 2.5000001 WHERE k='b'");
    hpplite_get_state_root(ctx, cur);
    if (memcmp(prev, cur, 32) == 0) FAIL("float change not reflected");
    memcpy(prev, cur, 32);

    exec(db, "UPDATE kv SET b = x'0103' WHERE k='b'");
    hpplite_get_state_root(ctx, cur);
    if (memcmp(prev, cur, 32) == 0) FAIL("blob change not reflected");
    memcpy(prev, cur, 32);

    exec(db, "CREATE VIEW v AS SELECT k FROM kv");
    hpplite_get_state_root(ctx, cur);
    if (memcmp(prev, cur, 32) == 0) FAIL("view not part of the schema hash");
    memcpy(prev, cur, 32);

    exec(db, "CREATE INDEX kv_v ON kv(v)");
    hpplite_get_state_root(ctx, cur);
    if (memcmp(prev, cur, 32) == 0) FAIL("index not part of the schema hash");
    memcpy(prev, cur, 32);

    exec(db, "CREATE TRIGGER tr AFTER INSERT ON kv BEGIN UPDATE kv SET v = v + 1 WHERE k = NEW.k; END");
    hpplite_get_state_root(ctx, cur);
    if (memcmp(prev, cur, 32) == 0) FAIL("trigger not part of the schema hash");

    hpplite_db_hash(db, direct);
    if (memcmp(cur, direct, 32) != 0) FAIL("hpplite_db_hash disagrees with the context root");
    close_ctx(db, ctx);
    PASS();
}

static void test_root_ignores_hpplite_tables(void) {
    sqlite3 *db; HppliteCtx *ctx;
    unsigned char r1[32], r2[32];
    HppliteBatch *batch;
    TEST("pending log and meta do not influence the root");
    if (!open_ctx(&db, &ctx)) FAIL("open");
    exec(db, "CREATE TABLE t(x)");
    exec(db, "INSERT INTO t VALUES(1)");
    hpplite_get_state_root(ctx, r1);
    if (hpplite_pending_count(ctx) != 2) FAIL("expected 2 pending");
    batch = hpplite_flush_block(ctx);        /* clears pending, advances meta */
    if (!batch) FAIL("flush");
    hpplite_batch_free(batch);
    if (hpplite_pending_count(ctx) != 0) FAIL("pending should be empty");
    hpplite_get_state_root(ctx, r2);
    if (memcmp(r1, r2, 32) != 0) FAIL("flushing changed the root");
    if (hpplite_get_block_height(ctx) != 2) FAIL("height should be 2");
    close_ctx(db, ctx);
    PASS();
}

static int pending_has(HppliteCtx *ctx, const char *needle) {
    sqlite3_stmt *s; int found = 0;
    if (sqlite3_prepare_v2(ctx->bkDb, "SELECT sql FROM _hpplite_pending", -1, &s, NULL) != SQLITE_OK) return 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *z = (const char *)sqlite3_column_text(s, 0);
        if (z && strstr(z, needle)) found++;
    }
    sqlite3_finalize(s);
    return found;
}

static void test_statement_classification(void) {
    sqlite3 *db; HppliteCtx *ctx;
    sqlite3_stmt *stmt;
    TEST("only replayable writes are logged");
    if (!open_ctx(&db, &ctx)) FAIL("open");
    exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)");
    if (hpplite_pending_count(ctx) != 1) FAIL("CREATE TABLE should be logged");

    exec(db, "PRAGMA user_version");                     /* a read */
    exec(db, "PRAGMA journal_mode");
    exec(db, "SELECT count(*) FROM t");
    exec(db, "EXPLAIN SELECT 1");
    exec(db, "ANALYZE");
    exec(db, "BEGIN; COMMIT;");
    exec(db, "SAVEPOINT sp; RELEASE sp;");
    if (hpplite_pending_count(ctx) != 1) FAIL("PRAGMA/SELECT/EXPLAIN/ANALYZE/txn control must not be logged");
    /* VACUUM may renumber rowids: logged once it has run */
    exec(db, "VACUUM");
    if (pending_has(ctx, "VACUUM") != 1 || hpplite_pending_count(ctx) != 2) FAIL("VACUUM must be logged once it has run");
    /* the header fields an application writes are (migration versions) */
    exec(db, "PRAGMA user_version = 7");
    if (hpplite_pending_count(ctx) != 3) FAIL("PRAGMA user_version = N must be logged");

    exec(db, "  /* leading comment */ -- and another\n INSERT INTO t VALUES(1,'c')");
    if (pending_has(ctx, "VALUES(1,'c')") != 1) FAIL("comment-prefixed INSERT should be logged");

    exec(db, "WITH src(a) AS (SELECT 2) INSERT INTO t SELECT a, 'w' FROM src");
    if (pending_has(ctx, "WITH src") != 1) FAIL("WITH ... INSERT should be logged");

    exec(db, "CREATE TRIGGER tr AFTER INSERT ON t BEGIN UPDATE t SET v = upper(v) WHERE id = NEW.id; END");
    int before = hpplite_pending_count(ctx);
    exec(db, "INSERT INTO t VALUES(3,'t')");
    if (hpplite_pending_count(ctx) != before + 1) FAIL("trigger body must not be logged separately");
    if (pending_has(ctx, "-- TRIGGER")) FAIL("trigger sub-program leaked into the log");

    exec(db, "SELECT count(*) FROM _hpplite_pending");
    exec(db, "UPDATE _hpplite_meta SET next_height = next_height");   /* user touching our tables */
    if (pending_has(ctx, "_hpplite_")) FAIL("statements naming _hpplite_ tables must not be logged");

    sqlite3_prepare_v2(db, "INSERT INTO t VALUES(?, ?)", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, 10);
    sqlite3_bind_text(stmt, 2, "bound 'q'", -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (pending_has(ctx, "VALUES(10, 'bound ''q''')") != 1) FAIL("bound parameters not expanded");
    if (pending_has(ctx, "?")) FAIL("placeholder leaked into the log");

    exec(db, "INSERT INTO t VALUES(1,'dup')");      /* fails: duplicate key */
    if (pending_has(ctx, "'dup'")) FAIL("failed statement was logged");

    exec(db, "BEGIN; INSERT INTO t VALUES(20,'r'); ROLLBACK;");
    if (pending_has(ctx, "'r'")) FAIL("rolled-back statement was logged");

    /* a statement that fails inside a transaction which then COMMITs */
    int before2 = hpplite_pending_count(ctx);
    exec(db, "BEGIN");
    exec(db, "INSERT INTO t VALUES(30,'ok')");
    exec(db, "INSERT INTO t VALUES(1,'dup-in-txn')");      /* fails, txn continues */
    exec(db, "INSERT INTO t VALUES(31,'ok2')");
    exec(db, "COMMIT");
    if (pending_has(ctx, "'dup-in-txn'")) FAIL("failed statement inside a committed txn was logged");
    if (hpplite_pending_count(ctx) != before2 + 2) FAIL("the two successful statements should be logged");

    /* same through a prepared statement stepped directly */
    sqlite3_prepare_v2(db, "INSERT INTO t VALUES(?, 'p')", -1, &stmt, NULL);
    exec(db, "BEGIN");
    sqlite3_bind_int(stmt, 1, 1); sqlite3_step(stmt); sqlite3_reset(stmt);   /* dup: fails */
    sqlite3_bind_int(stmt, 1, 40); sqlite3_step(stmt); sqlite3_reset(stmt);  /* ok */
    sqlite3_finalize(stmt);
    exec(db, "COMMIT");
    if (pending_has(ctx, "VALUES(1, 'p')")) FAIL("failed prepared statement was logged");
    if (pending_has(ctx, "VALUES(40, 'p')") != 1) FAIL("successful prepared statement not logged");

    /* statements with no effect: inside a transaction their rows go before
    ** the commit; in autocommit the row committed with the statement and may
    ** be in a batch already (another connection's flush), so it stays - a
    ** statement that changed nothing replays as nothing */
    exec(db, "BEGIN; UPDATE t SET v='none' WHERE id=99999; INSERT OR IGNORE INTO t VALUES(1,'ignored'); COMMIT;");
    if (pending_has(ctx, "'none'")) FAIL("no-op UPDATE in a transaction should not be logged");
    if (pending_has(ctx, "'ignored'")) FAIL("ignored INSERT in a transaction should not be logged");
    exec(db, "UPDATE t SET v='none-auto' WHERE id=99999");
    if (pending_has(ctx, "'none-auto'") != 1) FAIL("an autocommit no-op keeps its row");
    if (sqlite3_changes(db) != 0) FAIL("changes() after an autocommit no-op is 0");
    exec(db, "CREATE INDEX t_v ON t(v)");
    if (pending_has(ctx, "CREATE INDEX t_v") != 1) FAIL("DDL must be logged");
    /* a multi-row INSERT aborted mid-way (table without triggers: with a
    ** trigger that changed rows first, SQLite reports changes for the
    ** aborted statement and capture cannot tell; replay skips it instead) */
    exec(db, "CREATE TABLE u(id INTEGER PRIMARY KEY, v); INSERT INTO u VALUES(1,'a');");
    exec(db, "BEGIN; INSERT INTO u VALUES(50,'x'),(1,'dup-multi'); COMMIT;");
    if (pending_has(ctx, "'dup-multi'")) FAIL("aborted multi-row INSERT was logged");
    exec(db, "ROLLBACK");   /* the aborted exec string never reached COMMIT */
    close_ctx(db, ctx);
    PASS();
}

static void test_nondeterminism_detected(void) {
    sqlite3 *db; HppliteCtx *ctx;
    TEST("non-deterministic statements are counted");
    if (!open_ctx(&db, &ctx)) FAIL("open");
    exec(db, "CREATE TABLE t(x, ts)");
    exec(db, "INSERT INTO t VALUES(1, 'fixed')");
    if (ctx->nNondeterministic != 0) FAIL("deterministic SQL flagged");
    exec(db, "INSERT INTO t VALUES(random(), 'r')");
    exec(db, "INSERT INTO t VALUES(2, datetime('now'))");
    exec(db, "INSERT INTO t VALUES(3, CURRENT_TIMESTAMP)");
    exec(db, "INSERT INTO t VALUES(RANDOMBLOB(4), 'b')");
    /* the clock replays on the clock VFS (#56): only the random values count */
    if (ctx->nNondeterministic != 2) { printf("(count=%llu) ", (unsigned long long)ctx->nNondeterministic); FAIL("expected 2 flagged statements"); }
    exec(db, "UPDATE t SET x = 9 WHERE ts = 'fixed'");
    if (ctx->nNondeterministic != 2) FAIL("deterministic UPDATE flagged");
    close_ctx(db, ctx);
    PASS();
}

static void test_callbacks(void) {
    sqlite3 *db; HppliteCtx *ctx;
    TEST("commit and rollback callbacks fire per transaction");
    if (!open_ctx(&db, &ctx)) FAIL("open");
    hpplite_set_callbacks(ctx, NULL, on_commit, on_rollback);
    commits = rollbacks = 0;
    exec(db, "CREATE TABLE t(x)");                                   /* 1 commit */
    exec(db, "BEGIN; INSERT INTO t VALUES(1); INSERT INTO t VALUES(2); COMMIT;");  /* 1 commit */
    exec(db, "BEGIN; INSERT INTO t VALUES(3); ROLLBACK;");           /* 1 rollback */
    if (commits != 2) { printf("(commits=%d) ", commits); FAIL("expected 2 commits"); }
    if (rollbacks != 1) { printf("(rollbacks=%d) ", rollbacks); FAIL("expected 1 rollback"); }
    close_ctx(db, ctx);
    PASS();
}

static void test_txn_groups(void) {
    sqlite3 *db; HppliteCtx *ctx;
    HppliteBatch *b;
    TEST("batch groups statements by transaction");
    if (!open_ctx(&db, &ctx)) FAIL("open");
    exec(db, "CREATE TABLE t(x)");
    exec(db, "BEGIN; INSERT INTO t VALUES(1); INSERT INTO t VALUES(2); COMMIT;");
    exec(db, "INSERT INTO t VALUES(3)");
    b = hpplite_flush_block(ctx);
    if (!b || b->nTxns != 4) FAIL("expected 4 statements");
    if (b->aTxns[0].txn != 0 || b->aTxns[1].txn != 1 || b->aTxns[2].txn != 1 || b->aTxns[3].txn != 2) {
        printf("(groups %lld %lld %lld %lld) ", (long long)b->aTxns[0].txn, (long long)b->aTxns[1].txn,
               (long long)b->aTxns[2].txn, (long long)b->aTxns[3].txn);
        FAIL("transaction groups wrong");
    }
    hpplite_batch_free(b);
    close_ctx(db, ctx);
    PASS();
}

int main(void) {
    printf("=== HPPLite Core Tests (SQLite %s) ===\n", sqlite3_libversion());
    test_root_tracks_content();
    test_root_independent_of_history();
    test_root_covers_schema_kinds_and_types();
    test_root_ignores_hpplite_tables();
    test_statement_classification();
    test_nondeterminism_detected();
    test_callbacks();
    test_txn_groups();
    return test_report();
}
