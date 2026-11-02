/*
** HPPLite as a loadable extension on stock SQLite.
**
** This program links only the SQLite amalgamation - no HPPLite code - and
** loads hpplite.so/.dylib at runtime. It proves that HPPLite needs no change
** to SQLite core and no HPPLite-specific compile options.
*/

#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef HPPLITE_EXT_PATH
#error "HPPLITE_EXT_PATH must point at the built extension"
#endif

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { printf("  Testing: %s... ", name); tests_run++; } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

#define DB_URI "file:/tmp/hpplite_test_loadable.db?hpplite=on&l1=local&datadir=/tmp/hpplite_test_loadable"

static sqlite3_int64 scalar(sqlite3 *db, const char *sql, int *ok) {
    sqlite3_stmt *stmt;
    sqlite3_int64 v = -1;
    *ok = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) { v = sqlite3_column_int64(stmt, 0); *ok = 1; }
    sqlite3_finalize(stmt);
    return v;
}

static void test_load_and_use(void) {
    sqlite3 *db;
    int ok, rc;
    char *err = NULL;
    TEST("stock SQLite: no HPPLite before .load");
    unlink("/tmp/hpplite_test_loadable.db");
    unlink("/tmp/hpplite_test_loadable.db-wal");
    unlink("/tmp/hpplite_test_loadable.db-shm");
    unlink("/tmp/hpplite_test_loadable.db-hpplite.db");   /* the page-mode sidecar (#146) */
    (void)!system("mkdir -p /tmp/hpplite_test_loadable");

    rc = sqlite3_open_v2(DB_URI, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    if (rc != SQLITE_OK) FAIL("open");
    scalar(db, "SELECT height FROM hpplite_status", &ok);
    if (ok) FAIL("hpplite_status should not exist before loading the extension");
    PASS();

    TEST("sqlite3_load_extension activates HPPLite on the loading connection");
    sqlite3_enable_load_extension(db, 1);
    rc = sqlite3_load_extension(db, HPPLITE_EXT_PATH, NULL, &err);
    if (rc != SQLITE_OK) { printf("(%s) ", err ? err : "?"); sqlite3_free(err); FAIL("load_extension"); }
    if (scalar(db, "SELECT height FROM hpplite_status", &ok) != 1 || !ok) FAIL("height should be 1 after load");
    PASS();

    TEST("loading the module again into a connection it is on does nothing (Rails loads extensions per connection)");
    rc = sqlite3_load_extension(db, HPPLITE_EXT_PATH, NULL, &err);
    if (rc != SQLITE_OK) { printf("(%s) ", err ? err : "?"); sqlite3_free(err); FAIL("second load of the same module should succeed"); }
    sqlite3_free(err); err = NULL;
    if (scalar(db, "SELECT height FROM hpplite_status", &ok) != 1 || !ok) FAIL("first instance must stay intact");
    if (scalar(db, "SELECT hooks_replaced FROM hpplite_status", &ok) != 0 || !ok) FAIL("its hooks must stay its own");
    PASS();

    TEST("a connection opened after the first load has HPPLite already; loading it there is a no-op too");
    {
        sqlite3 *db2 = NULL;
        int ok2 = 0;
        rc = sqlite3_open_v2(DB_URI, &db2, SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, NULL);
        if (rc != SQLITE_OK) FAIL("open second connection");
        sqlite3_enable_load_extension(db2, 1);
        rc = sqlite3_load_extension(db2, HPPLITE_EXT_PATH, NULL, &err);
        if (rc != SQLITE_OK) { printf("(%s) ", err ? err : "?"); sqlite3_free(err); sqlite3_close(db2); FAIL("load on a connection the auto-extension set up"); }
        sqlite3_free(err); err = NULL;
        scalar(db2, "SELECT height FROM hpplite_status", &ok2);
        sqlite3_close(db2);
        if (!ok2) FAIL("the second connection has HPPLite");
    }
    PASS();

    TEST("writes are captured and flushed through the extension");
    {
    rc = sqlite3_exec(db,
        "CREATE TABLE t(id INTEGER PRIMARY KEY, v);"
        "INSERT INTO t VALUES(1,'a');"
        "BEGIN; INSERT INTO t VALUES(2,'b'); INSERT INTO t VALUES(3,'c'); COMMIT;",
        NULL, NULL, &err);
    if (rc != SQLITE_OK) { printf("(%s) ", err); sqlite3_free(err); FAIL("exec"); }
    { long long p = scalar(db, "SELECT pending FROM hpplite_status", &ok), pages = getenv("HPPLITE_CAPTURE") && strcasecmp(getenv("HPPLITE_CAPTURE"), "pages") == 0;
      if (p != 4 && !(pages && p == 5)) { printf("(pending %lld) ", p); FAIL("expected 4 pending"); } }   /* page capture: the file as opened counts too */
    if (scalar(db, "SELECT hpplite_flush()", &ok) != 1) FAIL("flush should produce height 1");
    if (scalar(db, "SELECT pending FROM hpplite_status", &ok) != 0) FAIL("pending should be 0");
    if (scalar(db, "SELECT height FROM hpplite_status", &ok) != 2) FAIL("height should be 2");
    PASS();
    }

    TEST("sqlite3_close() flushes pending via xDisconnect (no core close hook)");
    sqlite3_exec(db, "INSERT INTO t VALUES(4,'d')", NULL, NULL, NULL);
    if (sqlite3_close(db) != SQLITE_OK) FAIL("close");
    PASS();

    TEST("later connections get HPPLite automatically (auto-extension)");
    rc = sqlite3_open_v2(DB_URI, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    if (rc != SQLITE_OK) FAIL("reopen");
    if (scalar(db, "SELECT height FROM hpplite_status", &ok) != 3 || !ok) FAIL("height should be 3 (close flushed batch 2)");
    if (scalar(db, "SELECT pending FROM hpplite_status", &ok) != 0) FAIL("nothing pending after reopen");
    if (scalar(db, "SELECT count(*) FROM t", &ok) != 4) FAIL("rows");
    sqlite3_close(db);
    PASS();

    TEST("a driver clearing the connection's hooks as it closes it is not a takeover (Python at exit)");
    {
        char path[] = "/tmp/hpplite_test_loadable_err.XXXXXX";
        int fd = mkstemp(path), saved = dup(2), found;
        char buf[4096] = "";
        FILE *f;
        rc = sqlite3_open_v2(DB_URI, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, NULL);
        if (rc != SQLITE_OK || fd < 0) FAIL("open");
        sqlite3_exec(db, "INSERT INTO t VALUES(5,'e')", NULL, NULL, NULL);
        fflush(stderr); dup2(fd, 2);
        sqlite3_commit_hook(db, NULL, NULL);          /* what the driver does as it closes */
        sqlite3_rollback_hook(db, NULL, NULL);
        sqlite3_trace_v2(db, 0, NULL, NULL);
        rc = sqlite3_close(db);
        fflush(stderr); dup2(saved, 2); close(saved);
        f = fopen(path, "r");
        if (f) { size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f); }
        unlink(path); close(fd);
        found = strstr(buf, "replaced the connection") != NULL;
        if (rc != SQLITE_OK) FAIL("close");
        if (found) { printf("(%s) ", buf); FAIL("reported as another component's takeover"); }
    }
    PASS();

    TEST("a plain connection (no hpplite=on) is untouched");
    rc = sqlite3_open("/tmp/hpplite_test_loadable.db", &db);
    if (rc != SQLITE_OK) FAIL("plain open");
    scalar(db, "SELECT height FROM hpplite_status", &ok);
    if (ok) FAIL("hpplite_status should not exist on a plain connection");
    if (scalar(db, "SELECT count(*) FROM t", &ok) != 5) FAIL("plain read");
    sqlite3_close(db);
    PASS();
}

int main(void) {
    printf("=== HPPLite Loadable Extension Test ===\n");
    printf("  extension: %s\n", HPPLITE_EXT_PATH);
    test_load_and_use();
    unlink("/tmp/hpplite_test_loadable.db");
    unlink("/tmp/hpplite_test_loadable.db-wal");
    unlink("/tmp/hpplite_test_loadable.db-shm");
    unlink("/tmp/hpplite_test_loadable.db-hpplite.db");   /* the page-mode sidecar (#146) */
    (void)!system("rm -rf /tmp/hpplite_test_loadable");
    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
