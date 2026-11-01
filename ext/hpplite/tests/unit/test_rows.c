/*
** HPPLite row capture (capture engine v2, #19)
**
** A writer with row capture runs a workload; its log is applied to an empty
** database - schema text executed, row records applied with triggers and
** foreign keys off - and the two must be equal: every table, every row, every
** rowid, sqlite_sequence included.
*/

#include "test_lib.h"
#include "capture_rows.h"

#define DIR "/tmp/hpplite_test_rows"

static int exec(sqlite3 *db, const char *sql) { return exec_sql(db, sql) == SQLITE_OK; }   /* ROWS_VERBOSE=1 shows what failed */

typedef struct { sqlite3 *db; HppliteCtx *ctx; } W;
static int open_writer_ex(const char *path, W *w, int fnArgLimit) {
    if (sqlite3_open(path, &w->db) != SQLITE_OK) return 0;
    exec(w->db, "PRAGMA journal_mode=WAL");
    if (fnArgLimit > 0) sqlite3_limit(w->db, SQLITE_LIMIT_FUNCTION_ARG, fnArgLimit);
    if (hpplite_init(w->db, &w->ctx) != SQLITE_OK) return 0;
    return hpplite_rows_enable(w->ctx) == SQLITE_OK;
}
static int open_writer(const char *path, W *w) { return open_writer_ex(path, w, 0); }
static void close_writer(W *w) { hpplite_shutdown(w->ctx); sqlite3_close(w->db); }

/* Apply the writer's whole log, in order, to a plain database */
static const char *apply_log(sqlite3 *writer, sqlite3 *replica, int *pRecords, int *pTexts) {
    static char why[512];
    sqlite3_stmt *st;
    int on;
    *pRecords = *pTexts = 0;
    sqlite3_db_config(replica, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, &on);
    exec(replica, "PRAGMA foreign_keys=OFF");
    /* entry by entry, in autocommit: a text write's foreign keys are the
    ** writer's (its mode), and the pragma only switches outside a transaction */
    if (sqlite3_prepare_v2(writer, "SELECT sql, tbl, op, rec, mode FROM _hpplite_pending ORDER BY id", -1, &st, NULL) != SQLITE_OK) return "read log";
    while (sqlite3_step(st) == SQLITE_ROW) {
        int op = sqlite3_column_int(st, 2);
        if (op > 0) {
            char *err = NULL;
            if (hpplite_rows_apply(replica, (const char *)sqlite3_column_text(st, 1), op,
                                   sqlite3_column_blob(st, 3), sqlite3_column_bytes(st, 3), &err) != SQLITE_OK) {
                snprintf(why, sizeof why, "apply: %s", err ? err : "?");
                sqlite3_free(err); sqlite3_finalize(st); return why;
            }
            (*pRecords)++;
        } else {
            /* statement text: the schema, or a write logged the v1 way */
            const char *sql = (const char *)sqlite3_column_text(st, 0);
            int on2;
            sqlite3_db_config(replica, SQLITE_DBCONFIG_ENABLE_TRIGGER, 1, &on2);   /* a text write's triggers are its effects */
            if (sqlite3_column_int(st, 4) & 1) exec(replica, "PRAGMA foreign_keys=ON");
            if (!exec(replica, sql)) { snprintf(why, sizeof why, "text: %.200s: %s", sql, sqlite3_errmsg(replica)); sqlite3_finalize(st); return why; }
            exec(replica, "PRAGMA foreign_keys=OFF");
            sqlite3_db_config(replica, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, &on2);
            (*pTexts)++;
        }
    }
    sqlite3_finalize(st);
    return NULL;
}

/* Every table, every row (rowid tables with their rowid), typed */
#define dump(db) test_dump(db, 0)

/* The check every test ends with */
static const char *replica_differs(W *w, const char *replicaPath, int *pRecords, int *pTexts) {
    static char why[400];
    sqlite3 *r;
    if (sqlite3_open(replicaPath, &r) != SQLITE_OK) return "open replica";
    const char *err = apply_log(w->db, r, pRecords, pTexts);
    if (err) { sqlite3_close(r); return err; }
    char *a = dump(w->db), *b = dump(r);
    int same = strcmp(a, b) == 0;
    if (!same) {
        size_t i = 0; while (a[i] && a[i] == b[i]) i++;
        size_t s = i > 60 ? i - 60 : 0;
        snprintf(why, sizeof why, "replica differs near\n      writer  ...%.140s\n      replica ...%.140s", a + s, b + s);
    }
    sqlite3_free(a); sqlite3_free(b);
    sqlite3_close(r);
    return same ? NULL : why;
}

static void test_basic_types(void) {
    TEST("inserts, updates, deletes of every type replay exactly; nothing is logged as text");
    reset_dir();
    W w;
    if (!open_writer(DIR "/basic.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE t(id INTEGER PRIMARY KEY, i INTEGER, r REAL, s TEXT, b BLOB, n)");
    exec(w.db, "CREATE TABLE plain(a, b)");          /* rowid table without an INTEGER PRIMARY KEY */
    exec(w.db, "INSERT INTO t VALUES(1, 9007199254740993, 0.1, 'héllo', X'00ff10', NULL)");
    exec(w.db, "INSERT INTO t(i, r, s) VALUES(-9223372036854775808, -0.0, '')");
    exec(w.db, "INSERT INTO t(i, r, s, b) VALUES(9223372036854775807, 1e308, 'x', zeroblob(3))");
    exec(w.db, "UPDATE t SET s = 'changed', r = 2.5 WHERE id = 1");
    exec(w.db, "UPDATE t SET i = i + 1");            /* every row */
    exec(w.db, "DELETE FROM t WHERE id = 2");
    exec(w.db, "INSERT INTO plain VALUES(1, 'a'), (2, 'b'), (3, 'c')");
    exec(w.db, "DELETE FROM plain WHERE a = 2");
    exec(w.db, "UPDATE plain SET rowid = 100 WHERE a = 3");   /* the rowid itself moves */
    exec(w.db, "UPDATE t SET id = 50 WHERE id = 3");          /* an INTEGER PRIMARY KEY moves */
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/basic_r.db", &nr, &nt);
    int texts = (int)scalar(w.db, "SELECT count(*) FROM _hpplite_pending WHERE op = 0 AND sql LIKE 'INSERT%' OR sql LIKE 'UPDATE%' OR sql LIKE 'DELETE%'");
    close_writer(&w);
    printf("(%d records, %d schema statements) ", nr, nt);
    if (why) FAIL(why);
    if (texts) FAIL("a write was logged as text");
    PASS();
}

static void test_effects_not_text(void) {
    TEST("random(), the clock and an application-defined function in writes: accepted, replica equal");
    reset_dir();
    W w;
    if (!open_writer(DIR "/nd.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v, at DEFAULT CURRENT_TIMESTAMP, tok)");
    /* what WordPress's SQLite plugin does: REGEXP as a function only the application has */
    sqlite3_create_function(w.db, "regexp", 2, SQLITE_UTF8, NULL, NULL, NULL, NULL);
    int ok1 = exec(w.db, "INSERT INTO t(v, tok) VALUES(random(), hex(randomblob(8)))");
    int ok2 = exec(w.db, "INSERT INTO t(v) VALUES(datetime('now'))");
    int ok3 = exec(w.db, "UPDATE t SET tok = lower(hex(randomblob(4))) WHERE id = 1");
    exec(w.db, "INSERT INTO t(v) VALUES('rss_0123456789abcdef0123456789abcdef'), ('keep')");
    int ok4 = exec(w.db, "DELETE FROM t WHERE v GLOB 'rss_*'");
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/nd_r.db", &nr, &nt);
    close_writer(&w);
    if (!ok1 || !ok2 || !ok3 || !ok4) FAIL("a write was refused");
    if (why) FAIL(why);
    PASS();
}

static void test_cascades_and_app_triggers(void) {
    TEST("foreign-key cascades and the application's triggers: captured as rows, not replayed twice");
    reset_dir();
    W w;
    if (!open_writer(DIR "/fk.db", &w)) FAIL("open");
    exec(w.db, "PRAGMA foreign_keys=ON");
    exec(w.db, "CREATE TABLE parent(id INTEGER PRIMARY KEY, name);"
               "CREATE TABLE child(id INTEGER PRIMARY KEY, pid REFERENCES parent(id) ON DELETE CASCADE ON UPDATE CASCADE, v);"
               "CREATE TABLE audit(id INTEGER PRIMARY KEY, what);"
               "CREATE TRIGGER child_audit AFTER INSERT ON child BEGIN INSERT INTO audit(what) VALUES('child ' || new.id); END;"
               "CREATE TRIGGER parent_count AFTER DELETE ON parent BEGIN INSERT INTO audit(what) VALUES('parent gone ' || old.id); END;");
    exec(w.db, "INSERT INTO parent VALUES(1, 'a'), (2, 'b')");
    exec(w.db, "INSERT INTO child(pid, v) VALUES(1, 'x'), (1, 'y'), (2, 'z')");
    exec(w.db, "UPDATE parent SET id = 10 WHERE id = 2");        /* cascades to child.pid */
    exec(w.db, "DELETE FROM parent WHERE id = 1");               /* cascades: deletes two children */
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/fk_r.db", &nr, &nt);
    sqlite3_int64 audit = scalar(w.db, "SELECT count(*) FROM audit");
    close_writer(&w);
    if (audit != 4) FAILF("writer audit rows %lld, want 4", (long long)audit);
    if (why) FAIL(why);
    PASS();
}

static void test_conflicts_upserts_bulk(void) {
    TEST("REPLACE conflicts, upserts, INSERT ... SELECT, bulk updates and deletes");
    reset_dir();
    W w;
    if (!open_writer(DIR "/bulk.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE u(id INTEGER PRIMARY KEY, email TEXT UNIQUE, n INTEGER DEFAULT 0)");
    exec(w.db, "CREATE TABLE src(x)");
    exec(w.db, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 500) INSERT INTO src SELECT x FROM c");
    exec(w.db, "INSERT INTO u(email) SELECT 'user' || x || '@x' FROM src");
    exec(w.db, "INSERT OR REPLACE INTO u(id, email) VALUES(9999, 'user7@x')");   /* replaces row 7 by its UNIQUE email */
    exec(w.db, "INSERT INTO u(email, n) VALUES('user8@x', 1) ON CONFLICT(email) DO UPDATE SET n = n + 1");
    exec(w.db, "INSERT INTO u(email) VALUES('user9@x') ON CONFLICT DO NOTHING");
    exec(w.db, "UPDATE OR REPLACE u SET email = 'user3@x' WHERE id = 4");        /* row 3 goes, silently (#64) */
    exec(w.db, "UPDATE u SET n = n + id % 3");
    exec(w.db, "DELETE FROM u WHERE id % 5 = 0");
    exec(w.db, "UPDATE u SET n = n WHERE id < 0");                               /* no-op */
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/bulk_r.db", &nr, &nt);
    close_writer(&w);
    printf("(%d records) ", nr);
    if (why) FAIL(why);
    PASS();
}

static void test_without_rowid_and_generated(void) {
    TEST("WITHOUT ROWID tables (composite key, key changes) and generated columns");
    reset_dir();
    W w;
    if (!open_writer(DIR "/wr.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE kv(a TEXT, b INTEGER, v, PRIMARY KEY(b, a)) WITHOUT ROWID");
    exec(w.db, "CREATE TABLE g(id INTEGER PRIMARY KEY, x, y AS (x * 2), z AS (x || '!') STORED)");
    exec(w.db, "INSERT INTO kv VALUES('k1', 1, 'one'), ('k2', 1, 'two'), ('k1', 2, 'three')");
    exec(w.db, "UPDATE kv SET v = 'uno' WHERE a = 'k1' AND b = 1");
    exec(w.db, "UPDATE kv SET b = 5 WHERE a = 'k2'");          /* key moves */
    exec(w.db, "DELETE FROM kv WHERE b = 2");
    exec(w.db, "INSERT INTO g(x) VALUES(1), (2), ('a')");
    exec(w.db, "UPDATE g SET x = 10 WHERE id = 1");
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/wr_r.db", &nr, &nt);
    close_writer(&w);
    if (why) FAIL(why);
    PASS();
}

static void test_table_rebuild_migration(void) {
    TEST("a migration that rebuilds a table in a transaction: the copied rows are captured");
    reset_dir();
    W w;
    if (!open_writer(DIR "/mig.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE item(id INTEGER PRIMARY KEY, name, price)");
    exec(w.db, "INSERT INTO item(name, price) VALUES('a', 1), ('b', 2), ('c', 3)");
    /* what Django, Alembic batch mode and drizzle-kit do for most ALTERs */
    int ok = exec(w.db, "BEGIN;"
                        "CREATE TABLE new_item(id INTEGER PRIMARY KEY, name TEXT NOT NULL, price REAL, sku TEXT DEFAULT 'none');"
                        "INSERT INTO new_item(id, name, price) SELECT id, name, price * 1.5 FROM item;"
                        "DROP TABLE item;"
                        "ALTER TABLE new_item RENAME TO item;"
                        "COMMIT;");
    exec(w.db, "INSERT INTO item(name, price, sku) VALUES('d', 4, 'D-4')");
    exec(w.db, "UPDATE item SET sku = 'A-1' WHERE name = 'a'");
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/mig_r.db", &nr, &nt);
    close_writer(&w);
    printf("(%d records, %d schema statements) ", nr, nt);
    if (!ok) FAIL("migration failed");
    if (why) FAIL(why);
    PASS();
}

static void test_table_from_another_connection(void) {
    TEST("a table created by another connection: the first write to it is logged as text, then rows");
    reset_dir();
    W w, w2;
    if (!open_writer(DIR "/other.db", &w) || !open_writer(DIR "/other.db", &w2)) FAIL("open");
    exec(w.db, "CREATE TABLE a(x)");
    exec(w.db, "INSERT INTO a VALUES(1)");
    exec(w2.db, "CREATE TABLE b(id INTEGER PRIMARY KEY, y)");        /* w's triggers do not know b yet */
    int ok1 = exec(w.db, "INSERT INTO b(y) VALUES('first')");
    int ok2 = exec(w.db, "INSERT INTO b(y) VALUES('second')");
    if (getenv("ROWS_VERBOSE")) {
        sqlite3_stmt *q;
        sqlite3_prepare_v2(w.db, "SELECT id, txn, op, tbl, substr(sql, 1, 60) FROM _hpplite_pending ORDER BY id", -1, &q, NULL);
        while (sqlite3_step(q) == SQLITE_ROW) fprintf(stderr, "    log %lld txn %lld op %d %s %s\n", (long long)sqlite3_column_int64(q, 0),
            (long long)sqlite3_column_int64(q, 1), sqlite3_column_int(q, 2), sqlite3_column_text(q, 3), sqlite3_column_text(q, 4));
        sqlite3_finalize(q);
    }
    sqlite3_int64 texts = scalar(w.db, "SELECT count(*) FROM _hpplite_pending WHERE op = 0 AND sql LIKE 'INSERT INTO b%'");
    sqlite3_int64 recs = scalar(w.db, "SELECT count(*) FROM _hpplite_pending WHERE op = 1 AND tbl = 'b'");
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/other_r.db", &nr, &nt);
    close_writer(&w2);
    close_writer(&w);
    if (!ok1 || !ok2) FAIL("writes failed");
    if (texts != 1 || recs != 1) FAILF("want 1 text + 1 record for b, got %lld + %lld", (long long)texts, (long long)recs);
    if (why) FAIL(why);
    PASS();
}

static void test_autoincrement_and_fts5(void) {
    TEST("AUTOINCREMENT (sqlite_sequence) and an FTS5 table (its internal tables): replica equal and searchable");
    reset_dir();
    W w;
    if (!open_writer(DIR "/fts.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE s(id INTEGER PRIMARY KEY AUTOINCREMENT, v)");
    exec(w.db, "INSERT INTO s(v) VALUES('a'), ('b'), ('c')");
    exec(w.db, "DELETE FROM s WHERE id = 3");                         /* sqlite_sequence stays at 3 */
    exec(w.db, "INSERT INTO s(v) VALUES('d')");
    int fts = exec(w.db, "CREATE VIRTUAL TABLE docs USING fts5(title, body)");
    if (fts) {
        exec(w.db, "INSERT INTO docs VALUES('one', 'row based capture'), ('two', 'statement based replication')");
        exec(w.db, "UPDATE docs SET body = 'row capture again' WHERE title = 'two'");
        exec(w.db, "DELETE FROM docs WHERE title = 'one'");
        /* external content, kept in step by the application's triggers */
        exec(w.db, "CREATE TABLE notes(id INTEGER PRIMARY KEY, body TEXT);"
                   "CREATE VIRTUAL TABLE notes_fts USING fts5(body, content='notes', content_rowid='id');"
                   "CREATE TRIGGER notes_ai AFTER INSERT ON notes BEGIN"
                   "  INSERT INTO notes_fts(rowid, body) VALUES(new.id, new.body); END;"
                   "CREATE TRIGGER notes_ad AFTER DELETE ON notes BEGIN"
                   "  INSERT INTO notes_fts(notes_fts, rowid, body) VALUES('delete', old.id, old.body); END;"
                   "CREATE TRIGGER notes_au AFTER UPDATE ON notes BEGIN"
                   "  INSERT INTO notes_fts(notes_fts, rowid, body) VALUES('delete', old.id, old.body);"
                   "  INSERT INTO notes_fts(rowid, body) VALUES(new.id, new.body); END;");
        exec(w.db, "INSERT INTO notes(body) VALUES('capture rows'), ('replicate text'), ('capture all')");
        exec(w.db, "UPDATE notes SET body = 'nothing here' WHERE id = 3");
        exec(w.db, "DELETE FROM notes WHERE id = 2");
        exec(w.db, "CREATE TABLE plain(x); INSERT INTO plain VALUES(1)");   /* still rows */
        /* what decides is the table written, however it is spelled */
        exec(w.db, "INSERT INTO plain SELECT body FROM notes");                          /* reads notes: rows */
        exec(w.db, "WITH c(v) AS (SELECT 'x') INSERT OR REPLACE INTO main.\"notes\"(id, body) SELECT 9, v FROM c");  /* text */
        exec(w.db, "/* orm */ UPDATE OR IGNORE [notes] SET body = 'capture late' WHERE id = 9");          /* text */
        exec(w.db, "DELETE FROM `notes` WHERE id = 9");                                  /* text */
        /* a parent whose cascade deletes notes reaches the index too */
        exec(w.db, "PRAGMA foreign_keys = ON;"
                   "CREATE TABLE author(id INTEGER PRIMARY KEY);"
                   "CREATE TABLE notes2(id INTEGER PRIMARY KEY, author INTEGER REFERENCES author(id) ON DELETE CASCADE, body);"
                   "CREATE VIRTUAL TABLE notes2_fts USING fts5(body, content='notes2', content_rowid='id');"
                   "CREATE TRIGGER notes2_ad AFTER DELETE ON notes2 BEGIN"
                   "  INSERT INTO notes2_fts(notes2_fts, rowid, body) VALUES('delete', old.id, old.body); END;"
                   "INSERT INTO author VALUES(1); INSERT INTO notes2 VALUES(1, 1, 'gone soon');"
                   "INSERT INTO notes2_fts(rowid, body) VALUES(1, 'gone soon');"
                   "DELETE FROM author WHERE id = 1");                                   /* text: cascade */
    }
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/fts_r.db", &nr, &nt);
    sqlite3 *r; sqlite3_open(DIR "/fts_r.db", &r);
    sqlite3_int64 hits = fts ? scalar(r, "SELECT count(*) FROM docs WHERE docs MATCH 'capture'") : -1;
    sqlite3_int64 nhits = fts ? scalar(r, "SELECT count(*) FROM notes_fts WHERE notes_fts MATCH 'capture'") : -1;
    sqlite3_int64 plainRows = fts ? scalar(w.db, "SELECT count(*) FROM _hpplite_pending WHERE tbl = 'plain'") : -1;
    sqlite3_int64 noteText = fts ? scalar(w.db, "SELECT count(*) FROM _hpplite_pending WHERE op = 0 AND (sql LIKE '%notes\"(id%' "
                                              "OR sql LIKE '%[notes]%' OR sql LIKE '%`notes`%')") : -1;
    sqlite3_int64 authorText = fts ? scalar(w.db, "SELECT count(*) FROM _hpplite_pending WHERE op = 0 AND sql LIKE 'DELETE FROM author%'") : -1;
    sqlite3_close(r);
    close_writer(&w);
    if (why) FAIL(why);
    if (fts && hits != 1) FAILF("replica search: %lld hits, want 1", (long long)hits);
    if (fts && nhits != 1) FAILF("replica search, external content: %lld hits, want 1", (long long)nhits);
    if (fts && plainRows != 3) FAILF("%lld row records for an ordinary table, want 3 (one row, then two read from notes)", (long long)plainRows);
    if (fts && noteText != 3) FAILF("%lld of the 3 writes to notes, however spelled, logged as text", (long long)noteText);
    if (fts && authorText != 1) FAIL("a delete whose cascade reaches an FTS-synced table must be text");
    PASS();
}

static void test_failures_and_rollbacks(void) {
    TEST("failed statements and rolled-back transactions leave no records; changes() and last_insert_rowid() as plain SQLite");
    reset_dir();
    W w;
    if (!open_writer(DIR "/fail.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v UNIQUE)");
    exec(w.db, "INSERT INTO t VALUES(1, 'a')");
    sqlite3_int64 before = scalar(w.db, "SELECT count(*) FROM _hpplite_pending WHERE op > 0");
    exec(w.db, "INSERT INTO t VALUES(2, 'a')");                       /* UNIQUE fails */
    exec(w.db, "BEGIN; INSERT INTO t VALUES(3, 'c'); ROLLBACK");
    exec(w.db, "BEGIN; INSERT INTO t VALUES(4, 'd'); INSERT INTO t VALUES(5, 'a'); COMMIT");   /* the second fails, the txn commits */
    exec(w.db, "INSERT INTO t VALUES(6, 'f')");
    sqlite3_int64 lir = sqlite3_last_insert_rowid(w.db);
    exec(w.db, "UPDATE t SET v = v WHERE id = 6");
    int ch = sqlite3_changes(w.db);
    sqlite3_int64 after = scalar(w.db, "SELECT count(*) FROM _hpplite_pending WHERE op > 0");
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/fail_r.db", &nr, &nt);
    close_writer(&w);
    if (lir != 6) FAILF("last_insert_rowid %lld, want 6", (long long)lir);
    if (ch != 1) FAILF("changes() %d, want 1", ch);
    if (after - before != 3) FAILF("%lld records, want 3 (rows 4, 6, the update of 6)", (long long)(after - before));
    if (why) FAIL(why);
    PASS();
}

/* A text-logged statement held open (its RETURNING rows not read to the
** end) must not silence a row-captured write made meanwhile (#63) */
static void test_write_beside_open_text_statement(void) {
    TEST("a row write while a text-logged statement is still open is recorded");
    reset_dir();
    W w;
    if (!open_writer(DIR "/open.db", &w)) FAIL("open");
    exec(w.db, "CREATE VIRTUAL TABLE ft USING fts5(body)");
    exec(w.db, "CREATE TABLE c(id INTEGER PRIMARY KEY, body)");
    exec(w.db, "CREATE TRIGGER c_ai AFTER INSERT ON c BEGIN INSERT INTO ft(rowid, body) VALUES(new.id, new.body); END");
    exec(w.db, "CREATE TABLE other(x)");
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(w.db, "INSERT INTO c(body) VALUES('hello') RETURNING id", -1, &st, NULL) != SQLITE_OK) FAIL("prepare");
    if (sqlite3_step(st) != SQLITE_ROW) FAIL("step");            /* held open: not reset */
    exec(w.db, "INSERT INTO other VALUES(1)");
    exec(w.db, "INSERT INTO other VALUES(2)");
    sqlite3_finalize(st);
    exec(w.db, "INSERT INTO other VALUES(3)");
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/open_r.db", &nr, &nt);
    close_writer(&w);
    if (why) FAIL(why);
    if (nr != 3) FAILF("%d records, want 3", nr);
    PASS();
}

/* A TEMP table named like a main table: main's triggers are shaped by
** main's columns, and writes to main's table go through (#67) */
static void test_temp_table_shadow(void) {
    TEST("a TEMP table with a main table's name does not shape that table's capture");
    reset_dir();
    W w;
    if (!open_writer(DIR "/shadow.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE t(a, b)");
    exec(w.db, "CREATE TEMP TABLE t(b, a, c)");
    exec(w.db, "CREATE TABLE u(x)");                                  /* the triggers are rebuilt here */
    if (!exec(w.db, "INSERT INTO main.t VALUES(1, 2)")) FAIL("the write to main.t failed");
    exec(w.db, "INSERT INTO t VALUES(7, 8, 9)");                       /* temp.t: not captured, not main's */
    exec(w.db, "UPDATE main.t SET b = 3");
    exec(w.db, "CREATE TABLE v(y)");                                  /* rebuilt again, temp.t still there */
    exec(w.db, "INSERT INTO main.t VALUES(4, 5)");
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/shadow_r.db", &nr, &nt);
    close_writer(&w);
    if (why) FAIL(why);
    if (nr != 3) FAILF("%d records, want 3", nr);
    PASS();
}

/* INTEGER PRIMARY KEY DESC: a key column with an index of its own, not the
** rowid - rowids must travel with the records (#73) */
static void test_integer_primary_key_desc(void) {
    TEST("INTEGER PRIMARY KEY DESC is not the rowid: rowids replay as the writer's");
    reset_dir();
    W w;
    if (!open_writer(DIR "/desc.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE t(id INTEGER PRIMARY KEY DESC, x)");
    exec(w.db, "INSERT INTO t(rowid, id, x) VALUES(100, 5, 1)");
    exec(w.db, "INSERT INTO t(id, x) VALUES(3, 2)");
    exec(w.db, "UPDATE t SET rowid = 7 WHERE id = 3");
    exec(w.db, "UPDATE t SET id = 9 WHERE rowid = 7");
    exec(w.db, "DELETE FROM t WHERE rowid = 100");
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/desc_r.db", &nr, &nt);
    close_writer(&w);
    if (why) FAIL(why);
    PASS();
}

/* A table too wide for the SQLite's function argument limit (127 on host
** SQLites before 3.48): its writes are text, every other table's stay
** records, and nothing is rebuilt per statement (#65) */
static void test_wide_table(void) {
    TEST("a table past the function argument limit is logged as text; the rest stays rows");
    reset_dir();
    W w;
    if (!open_writer_ex(DIR "/wide.db", &w, 127)) FAIL("open");
    char *sql = sqlite3_mprintf("CREATE TABLE wide(id INTEGER PRIMARY KEY");
    for (int i = 0; i < 70; i++) sql = sqlite3_mprintf("%z, c%d", sql, i);
    sql = sqlite3_mprintf("%z)", sql);
    exec(w.db, sql);
    sqlite3_free(sql);
    exec(w.db, "CREATE TABLE narrow(x)");
    if (w.ctx->rowsBuiltFor < 0) FAIL("the triggers were not built");
    exec(w.db, "INSERT INTO wide(c0, c69) VALUES('a', 'z')");
    exec(w.db, "UPDATE wide SET c1 = 'b'");
    exec(w.db, "INSERT INTO narrow VALUES(1), (2)");
    exec(w.db, "DELETE FROM narrow WHERE x = 1");
    if (w.ctx->rowsBuiltFor < 0) FAIL("the triggers were torn down by a write");
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/wide_r.db", &nr, &nt);
    close_writer(&w);
    if (why) FAIL(why);
    if (nr != 3) FAILF("%d records, want 3 (narrow's)", nr);
    if (nt != 4) FAILF("%d text statements, want 4 (two CREATEs, wide's two writes)", nt);
    PASS();
}

/* Columns named rowid, oid or _rowid_ shadow the rowid in trigger and
** replay SQL; a WITHOUT ROWID table with one looks like a rowid table to a
** "SELECT rowid" probe (#72) */
static void test_rowid_named_columns(void) {
    TEST("columns named rowid / oid / _rowid_ do not stand in for the rowid");
    reset_dir();
    W w;
    if (!open_writer(DIR "/rid.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE t(rowid TEXT, x)");
    exec(w.db, "CREATE TABLE wr(a, rowid TEXT, PRIMARY KEY(a)) WITHOUT ROWID");
    exec(w.db, "CREATE TABLE o(oid, y)");
    exec(w.db, "CREATE TABLE all3(rowid, oid, _rowid_)");                 /* no name left: text */
    exec(w.db, "INSERT INTO t VALUES('a', 1), ('a', 2), ('b', 3)");
    exec(w.db, "DELETE FROM t WHERE x = 2");                              /* one row, by its real rowid */
    exec(w.db, "UPDATE t SET rowid = 'c' WHERE x = 3");                   /* the column, not the rowid */
    exec(w.db, "INSERT INTO wr VALUES(1, 'r1'), (2, 'r2')");
    exec(w.db, "UPDATE wr SET rowid = 'R2' WHERE a = 2");
    exec(w.db, "DELETE FROM wr WHERE a = 1");
    exec(w.db, "INSERT INTO o VALUES('p', 1), ('p', 2)");
    exec(w.db, "DELETE FROM o WHERE y = 1");
    exec(w.db, "INSERT INTO all3 VALUES(1, 2, 3), (1, 2, 4)");
    exec(w.db, "DELETE FROM all3 WHERE _rowid_ = 3");
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/rid_r.db", &nr, &nt);
    close_writer(&w);
    if (why) FAIL(why);
    if (nr != 12) FAILF("%d records, want 12", nr);
    PASS();
}

/* Writes to SQLite's own tables (writable_schema, sqlite_stat1) have no
** triggers to record them: they are text, never silence (#74) */
static void test_sqlite_tables_are_text(void) {
    TEST("a write to sqlite_schema or sqlite_stat1 is logged as text");
    reset_dir();
    W w;
    if (!open_writer(DIR "/sys.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE t(a)");
    exec(w.db, "CREATE INDEX ti ON t(a)");
    exec(w.db, "ANALYZE");                                                 /* makes sqlite_stat1; not tracked */
    if (!exec(w.db, "INSERT INTO sqlite_stat1(tbl, idx, stat) VALUES('t', 'ti', '100 1')")) FAIL("sqlite_stat1 write");
    exec(w.db, "PRAGMA writable_schema=ON");
    if (!exec(w.db, "UPDATE sqlite_schema SET sql = 'CREATE TABLE t(a, b)' WHERE name = 't'")) FAIL("schema write");
    exec(w.db, "PRAGMA writable_schema=OFF");
    sqlite3_int64 texts = scalar(w.db, "SELECT count(*) FROM _hpplite_pending WHERE op = 0 AND (sql LIKE '%sqlite_stat1%' OR sql LIKE '%sqlite_schema%')");
    sqlite3_int64 recs = scalar(w.db, "SELECT count(*) FROM _hpplite_pending WHERE op > 0");
    close_writer(&w);
    if (texts != 2) FAILF("%lld text statements on SQLite's tables, want 2", (long long)texts);
    if (recs != 0) FAILF("%lld records, want 0", (long long)recs);
    PASS();
}

/* hpplite_shutdown() on a connection that lives on: the triggers and
** functions that held the context go with it (#66) */
static void test_shutdown_leaves_connection_usable(void) {
    TEST("after hpplite_shutdown() the connection writes on, with no capture trigger or function left");
    reset_dir();
    W w;
    if (!open_writer(DIR "/shut.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE t(x)");
    exec(w.db, "INSERT INTO t VALUES(1)");
    hpplite_shutdown(w.ctx);
    if (!exec(w.db, "INSERT INTO t VALUES(2)")) FAIL("a write after shutdown failed");
    sqlite3_int64 trig = scalar(w.db, "SELECT count(*) FROM temp.sqlite_schema WHERE name LIKE 'hpplite_cap_%'");
    int fn = exec(w.db, "SELECT hpplite_cap()");
    sqlite3_int64 rows = scalar(w.db, "SELECT count(*) FROM t");
    sqlite3_close(w.db);
    if (trig != 0) FAILF("%lld capture triggers left", (long long)trig);
    if (fn) FAIL("hpplite_cap() still registered");
    if (rows != 2) FAILF("%lld rows, want 2", (long long)rows);
    PASS();
}

/* A record is bytes from the chain: malformed ones are refused, never
** read past (#69) */
static void test_malformed_records(void) {
    TEST("a malformed record is refused, not read past; the capture functions refuse bad calls");
    reset_dir();
    sqlite3 *db;
    if (sqlite3_open(DIR "/mal.db", &db) != SQLITE_OK) FAIL("open");
    exec(db, "CREATE TABLE t(a)");
    /* an update record: key of 1 value, a text whose length claims 2^31 bytes */
    static const unsigned char rec[] = { 0x01, 0x03, 0x80, 0x80, 0x80, 0x80, 0x08, 0x00, 0x01, 0x00 };
    char *err = NULL;
    int rc = hpplite_rows_apply(db, "t", HPPLITE_ROW_UPDATE, rec, (int)sizeof(rec), &err);
    sqlite3_free(err);
    if (rc == SQLITE_OK) FAIL("a record with a length past its end was applied");
    static const unsigned char rec2[] = { 0x01, 0x04, 0x7f };                 /* a delete key: blob of 127 bytes, 0 given */
    rc = hpplite_rows_apply(db, "t", HPPLITE_ROW_DELETE, rec2, (int)sizeof(rec2), &err);
    sqlite3_free(err);
    if (rc == SQLITE_OK) FAIL("a record with a short value was applied");
    sqlite3_close(db);
    /* the capture functions are callable from any SQL: no argument count is trusted (#70) */
    W w;
    if (!open_writer(DIR "/mal2.db", &w)) FAIL("open writer");
    if (exec(w.db, "SELECT hpplite_upd()")) FAIL("hpplite_upd() with no arguments succeeded");
    if (exec(w.db, "SELECT hpplite_upd(2, 1)")) FAIL("hpplite_upd() with too few arguments succeeded");
    if (exec(w.db, "SELECT hpplite_upd(-1)")) FAIL("hpplite_upd() with a negative key count succeeded");
    if (!exec(w.db, "SELECT hpplite_rec()")) FAIL("hpplite_rec() with no arguments failed (an empty record is one)");
    close_writer(&w);
    PASS();
}

/* The triggers rebuilt inside an application transaction go with its
** ROLLBACK: the old ones are back, and must be known to be (#88) */
static void test_rebuild_rolled_back(void) {
    TEST("a trigger rebuild inside a transaction that rolls back is noticed: the next write rebuilds again");
    reset_dir();
    W a, b;
    if (!open_writer(DIR "/rb.db", &a) || !open_writer(DIR "/rb.db", &b)) FAIL("open");
    exec(a.db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)");
    exec(b.db, "INSERT INTO t(v) VALUES('b0')");                      /* b's triggers know t(id, v) */
    exec(a.db, "ALTER TABLE t ADD COLUMN w");
    exec(b.db, "BEGIN");
    exec(b.db, "INSERT INTO t(v) VALUES('text')");                    /* text (not covered), rebuilt after it, in the transaction */
    exec(b.db, "ROLLBACK");                                           /* the old triggers are back */
    if (!exec(b.db, "INSERT INTO t(v, w) VALUES('after', 1)")) FAIL("the write after the rollback failed");
    exec(b.db, "SAVEPOINT s");
    exec(a.db, "ALTER TABLE t ADD COLUMN x");
    exec(b.db, "INSERT INTO t(v) VALUES('text2')");                   /* rebuilt inside the savepoint */
    exec(b.db, "ROLLBACK TO s");
    exec(b.db, "RELEASE s");
    if (!exec(b.db, "INSERT INTO t(v, x) VALUES('after2', 2)")) FAIL("the write after ROLLBACK TO failed");
    int nr, nt;
    const char *why = replica_differs(&b, DIR "/rb_r.db", &nr, &nt);
    close_writer(&b);
    close_writer(&a);
    if (why) FAIL(why);
    PASS();
}

/* A virtual table whose name is longer than the target parser's buffer:
** compared cut it would pass for a plain table, and its write would record
** nothing (#78) */
static void test_long_virtual_table_name(void) {
    TEST("a write to a virtual table with a 300-byte name is logged as text");
    reset_dir();
    W w;
    if (!open_writer(DIR "/longname.db", &w)) FAIL("open");
    char name[301];
    memset(name, 'v', 300); name[300] = 0;
    char *sql = sqlite3_mprintf("CREATE VIRTUAL TABLE \"%w\" USING fts5(b)", name);
    exec(w.db, sql); sqlite3_free(sql);
    sql = sqlite3_mprintf("INSERT INTO \"%w\"(b) VALUES('x')", name);
    if (!exec(w.db, sql)) { sqlite3_free(sql); FAIL("the write failed"); }
    sqlite3_free(sql);
    sqlite3_int64 texts = scalar(w.db, "SELECT count(*) FROM _hpplite_pending WHERE op = 0 AND sql LIKE 'INSERT INTO%'");
    close_writer(&w);
    if (texts != 1) FAILF("%lld text statements for the write, want 1", (long long)texts);
    PASS();
}

/* A refused statement prepared on a stale schema: SQLite reruns it with
** the interrupt cleared and no trace; it must still not run (#89) */
static void test_refused_stale_statement(void) {
    TEST("a refused statement that is also stale does not run on its rerun");
    reset_dir();
    W a, b;
    if (!open_writer(DIR "/stale.db", &a) || !open_writer(DIR "/stale.db", &b)) FAIL("open");
    int ok1 = exec(a.db, "CREATE VIRTUAL TABLE ft USING fts5(body)");
    int ok2 = exec(a.db, "CREATE TABLE c(id INTEGER PRIMARY KEY, v)");
    int ok3 = exec(a.db, "CREATE TRIGGER c_ai AFTER INSERT ON c BEGIN INSERT INTO ft(rowid, body) VALUES(new.id, new.v); END");
    int aAuto = sqlite3_get_autocommit(a.db), aStage = a.ctx->rerunStage, aOwn = a.ctx->ownTxn;
    int ok4 = exec(b.db, "SELECT count(*) FROM c");                   /* b sees the schema */
    const char *e4 = sqlite3_errmsg(b.db);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(b.db, "INSERT INTO c(v) VALUES(random())", -1, &st, NULL) != SQLITE_OK)
        FAILF("prepare: %s (%d); a: ddl %d %d %d autocommit %d rerunStage %d own %d; b's select %d (%s) txn %d",
              sqlite3_errmsg(b.db), sqlite3_extended_errcode(b.db), ok1, ok2, ok3, aAuto, aStage, aOwn, ok4, e4,
              sqlite3_txn_state(b.db, "main"));
    exec(a.db, "CREATE TABLE z(x)");                                  /* b's statement is stale now */
    int rc = sqlite3_step(st);                                        /* text-logged (reaches ft), random(): refused */
    sqlite3_finalize(st);
    /* the refusal says why this write was text, not that a version lacks something (#85) */
    int saysText = strstr(b.ctx->zLastRefusal, "logged as text") != NULL, saysV01 = strstr(b.ctx->zLastRefusal, "v0.1") != NULL;
    sqlite3_int64 rows = scalar(b.db, "SELECT count(*) FROM c");
    int again = exec(b.db, "INSERT INTO c(v) VALUES('fine')");        /* the connection goes on */
    close_writer(&b);
    close_writer(&a);
    if (rc == SQLITE_DONE) FAIL("the refused statement's rerun ran (SQLITE_DONE)");
    if (rows != 0) FAILF("%lld rows in c, want 0: the refused write happened", (long long)rows);
    if (!again) FAIL("the next statement on the connection failed");
    if (!saysText || saysV01) FAIL("the refusal does not say the statement is logged as text, or blames v0.1");
    PASS();
}

/* A refused statement is remembered until its end so that a stale rerun of
** it (no trace, interrupt cleared) is denied at its reprepare (#89). One
** expired for another reason - its own rollback undid a schema change of
** the transaction, which expires every statement - never reruns, and must
** not leave the connection's next prepare denied */
static void test_refused_then_expired(void) {
    TEST("a refused statement expired by its rollback (a schema change in the transaction) leaves the next statement allowed");
    reset_dir();
    W a;
    char err[200];
    if (!open_writer(DIR "/expired.db", &a)) FAIL("open");
    exec(a.db, "CREATE VIRTUAL TABLE ft USING fts5(body)");
    exec(a.db, "BEGIN");
    exec(a.db, "CREATE TABLE c(id INTEGER PRIMARY KEY, v)");
    int rc = exec(a.db, "INSERT INTO ft(body) VALUES(random())");   /* text (a virtual table), random(): refused */
    int autoc = sqlite3_get_autocommit(a.db);                       /* SQLITE_INTERRUPT rolled the transaction back */
    int again = exec(a.db, "CREATE TABLE d(x)");
    snprintf(err, sizeof(err), "%s", sqlite3_errmsg(a.db));
    close_writer(&a);
    if (rc) FAIL("premise: the write was not refused");
    if (!autoc) FAIL("premise: the transaction was not rolled back");
    if (!again) FAILF("the next statement on the connection failed: %s", err);
    PASS();
}
/* The same in autocommit: refused as text because another connection moved
** the schema (this connection's triggers are stale), on a statement prepared
** on the new schema - no rerun follows either */
static void test_refused_after_foreign_alter(void) {
    TEST("a statement refused after another connection's ALTER (stale triggers, text rules) leaves the next statement allowed");
    reset_dir();
    W a, b;
    char err[200];
    if (!open_writer(DIR "/expired2.db", &a) || !open_writer(DIR "/expired2.db", &b)) FAIL("open");
    exec(a.db, "CREATE TABLE t(v)");
    exec(b.db, "ALTER TABLE t ADD COLUMN c DEFAULT 'x'");
    int rc = exec(a.db, "INSERT INTO t(v) VALUES(random())");        /* a's triggers are stale: text, random(): refused */
    int autoc = sqlite3_get_autocommit(a.db);
    int again = exec(a.db, "CREATE TABLE e(x)");
    snprintf(err, sizeof(err), "%s", sqlite3_errmsg(a.db));
    close_writer(&b);
    close_writer(&a);
    if (rc) FAIL("premise: the write after the other connection's ALTER was not refused");
    if (!autoc) FAIL("premise: a transaction is open");
    if (!again) FAILF("the next statement failed: %s", err);
    PASS();
}

/* A statement HPPLite tracks for a rerun (prepared on a schema another
** connection changed) runs in a transaction of HPPLite's. One that changes
** the schema itself (a CREATE VIRTUAL TABLE: FTS5's nested DDL expires it)
** must still commit at its end, not stay open with the next statements
** inside, unseen by other connections */
static void test_tracked_schema_change_commits(void) {
    TEST("a tracked statement that changes the schema itself commits at its end; nothing stays open");
    reset_dir();
    W a, b;
    if (!open_writer(DIR "/tracked.db", &a) || !open_writer(DIR "/tracked.db", &b)) FAIL("open");
    exec(a.db, "CREATE TABLE t(x)");
    exec(b.db, "CREATE TABLE z(x)");                                  /* a's next statement is stale */
    exec(a.db, "CREATE VIRTUAL TABLE ft USING fts5(b)");
    int auto1 = sqlite3_get_autocommit(a.db);
    exec(a.db, "INSERT INTO t VALUES(1)");
    int auto2 = sqlite3_get_autocommit(a.db);
    sqlite3_int64 seen = scalar(b.db, "SELECT count(*) FROM t");
    int nr, nt;
    const char *why = replica_differs(&a, DIR "/tracked_r.db", &nr, &nt);
    close_writer(&b);
    close_writer(&a);
    if (!auto1) FAIL("a transaction of HPPLite's is open after the tracked CREATE VIRTUAL TABLE");
    if (!auto2) FAIL("a transaction of HPPLite's is open after the next statement");
    if (seen != 1) FAILF("the other connection sees %lld rows in t, want 1", (long long)seen);
    if (why) FAIL(why);
    PASS();
}

static void test_groups(void) {
    TEST("records of one transaction share its group, as v1 names them");
    reset_dir();
    W w;
    if (!open_writer(DIR "/grp.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE t(v)");
    exec(w.db, "INSERT INTO t VALUES(1)");
    exec(w.db, "BEGIN; INSERT INTO t VALUES(2); UPDATE t SET v = v + 10; DELETE FROM t WHERE v = 11; COMMIT");
    exec(w.db, "INSERT INTO t VALUES(3)");
    sqlite3_int64 groups = scalar(w.db, "SELECT count(DISTINCT CASE WHEN txn = 0 THEN id ELSE txn END) FROM _hpplite_pending WHERE op > 0");
    sqlite3_int64 biggest = scalar(w.db, "SELECT max(n) FROM (SELECT count(*) n FROM _hpplite_pending WHERE op > 0 GROUP BY CASE WHEN txn = 0 THEN id ELSE txn END)");
    close_writer(&w);
    if (groups != 3 || biggest != 4) FAILF("groups %lld (want 3), largest %lld records (want 4: the insert, the update of 2 rows, the delete)", (long long)groups, (long long)biggest);
    PASS();
}

int main(void) {
    printf("HPPLite row capture\n");
    test_suite_init("rows", NULL, DIR);
    test_verbose = getenv("ROWS_VERBOSE") != NULL;
    test_basic_types();
    test_effects_not_text();
    test_cascades_and_app_triggers();
    test_conflicts_upserts_bulk();
    test_without_rowid_and_generated();
    test_table_rebuild_migration();
    test_table_from_another_connection();
    test_autoincrement_and_fts5();
    test_failures_and_rollbacks();
    test_write_beside_open_text_statement();
    test_temp_table_shadow();
    test_integer_primary_key_desc();
    test_wide_table();
    test_rowid_named_columns();
    test_sqlite_tables_are_text();
    test_shutdown_leaves_connection_usable();
    test_malformed_records();
    test_rebuild_rolled_back();
    test_long_virtual_table_name();
    test_refused_stale_statement();
    test_refused_then_expired();
    test_refused_after_foreign_alter();
    test_tracked_schema_change_commits();
    test_groups();
    return test_report();
}
