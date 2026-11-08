/*
** HPPLite turned on from the environment (#55)
**
** Drivers that cannot open URI filenames (better-sqlite3, the default of
** Drizzle and Knex) open a plain path. HPPLITE_FILES names the files HPPLite
** handles - glob patterns, ':'-separated, matched against the full path and
** the file name - and HPPLITE_PARAMS carries what the URI would have
** (l1=hpp-sepolia&interval=5s ...). Everything else opens as plain SQLite.
*/

#include "test_lib.h"
#include <unistd.h>

#define DIR "/tmp/hpplite_test_env"
#define KEY "0x8182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0"


static void test_plain_path_enabled(void) {
    TEST("a plain-path open of a file named in HPPLITE_FILES is an HPPLite connection");
    reset_dir();
    setenv("HPPLITE_FILES", "*/app*.db:" DIR "/exact.db", 1);
    setenv("HPPLITE_PARAMS", "privkey=" KEY "&datadir=" DIR "&interval=0", 1);
    sqlite3 *db = NULL;
    if (sqlite3_open(DIR "/app.db", &db) != SQLITE_OK) FAIL("open");
    HppliteNode *n = hpplite_get_node(db);
    if (!n) { sqlite3_close(db); FAIL("HPPLite did not start on a matching file"); }
    if (n->db != db) { sqlite3_close(db); FAIL("node is not on the application connection"); }
    if (n->bkDb && hpplite_get_node(n->bkDb)) { sqlite3_close(db); FAIL("the bookkeeping connection enabled HPPLite on itself"); }
    sqlite3_exec(db, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL, NULL, NULL);
    sqlite3_int64 pending = scalar(db, "SELECT pending FROM hpplite_status");
    sqlite3_close(db);
    if (!test_pending_is(pending, 2)) FAILF("writes not captured (pending %lld)", (long long)pending);

    if (sqlite3_open(DIR "/exact.db", &db) != SQLITE_OK) FAIL("open exact");
    int exact = hpplite_get_node(db) != NULL;
    sqlite3_close(db);
    if (!exact) FAIL("a full-path pattern did not match");
    PASS();
}

static void test_others_untouched(void) {
    TEST("files not named, :memory: and an unset HPPLITE_FILES stay plain SQLite");
    reset_dir();
    setenv("HPPLITE_FILES", "*/app*.db", 1);
    setenv("HPPLITE_PARAMS", "privkey=" KEY "&datadir=" DIR "&interval=0", 1);
    sqlite3 *db = NULL;
    sqlite3_open(DIR "/cache.db", &db);
    int other = hpplite_get_node(db) != NULL;
    sqlite3_close(db);
    sqlite3_open(":memory:", &db);
    int mem = hpplite_get_node(db) != NULL;
    sqlite3_close(db);
    unsetenv("HPPLITE_FILES");
    sqlite3_open(DIR "/app.db", &db);
    int unset = hpplite_get_node(db) != NULL;
    sqlite3_close(db);
    if (other) FAIL("a file not named was enabled");
    if (mem) FAIL(":memory: was enabled");
    if (unset) FAIL("enabled without HPPLITE_FILES");
    PASS();
}

static void test_uri_still_wins(void) {
    TEST("a URI with hpplite= decides for itself; HPPLITE_PARAMS do not override it");
    reset_dir();
    setenv("HPPLITE_FILES", "*", 1);
    setenv("HPPLITE_PARAMS", "privkey=" KEY "&datadir=" DIR "&interval=0", 1);
    sqlite3 *db = NULL;
    sqlite3_open_v2("file:" DIR "/off.db?hpplite=off", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    int off = hpplite_get_node(db) == NULL;
    sqlite3_close(db);
    unsetenv("HPPLITE_FILES");
    unsetenv("HPPLITE_PARAMS");
    if (!off) FAIL("hpplite=off in the URI was overridden by the environment");
    PASS();
}



/* A read-only connection writes nothing to capture: it opens as it would
** without HPPLite (it failed: "cannot start", found by SQLite's delete.test) */
static void test_read_only_open(void) {
    TEST("a read-only connection to a captured file opens and reads (HPPLite stands aside)");
    reset_dir();
    sqlite3 *w = NULL, *r = NULL;
    int rc = sqlite3_open_v2("file:" DIR "/ro.db?hpplite=on&privkey=" KEY "&datadir=" DIR, &w,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    if (rc != SQLITE_OK) FAIL("writer open");
    sqlite3_exec(w, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL, NULL, NULL);
    rc = sqlite3_open_v2("file:" DIR "/ro.db?hpplite=on&privkey=" KEY "&datadir=" DIR, &r,
                         SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL);
    int n = -1;
    sqlite3_stmt *q;
    if (rc == SQLITE_OK && sqlite3_prepare_v2(r, "SELECT count(*) FROM t", -1, &q, NULL) == SQLITE_OK) {
        if (sqlite3_step(q) == SQLITE_ROW) n = sqlite3_column_int(q, 0);
        sqlite3_finalize(q);
    }
    sqlite3_close(r);
    sqlite3_close(w);
    if (rc != SQLITE_OK) FAIL("read-only open refused");
    if (n != 1) FAIL("read-only connection cannot read");
    PASS();
}

/* capture= and determinism= are gone (#253): a URI or an environment from before refuses the open and says so */
static void test_capture_values(void) {
    TEST("capture= and determinism= refuse the open (gone in v0.8); HPPLITE_CAPTURE too");
    reset_dir();
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2("file:" DIR "/cap.db?hpplite=on&capture=rows&privkey=" KEY "&datadir=" DIR, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    int opened = rc == SQLITE_OK && hpplite_get_node(db) != NULL;
    const char *msg = db ? sqlite3_errmsg(db) : "";
    int said = strstr(msg, "capture= is gone") != NULL;
    sqlite3_close(db);
    if (opened) FAIL("capture=rows opened");
    if (!said) FAIL("the refusal should say capture= is gone");
    db = NULL;
    rc = sqlite3_open_v2("file:" DIR "/det.db?hpplite=on&determinism=warn&privkey=" KEY "&datadir=" DIR, &db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    opened = rc == SQLITE_OK && hpplite_get_node(db) != NULL;
    sqlite3_close(db);
    if (opened) FAIL("determinism=warn opened");
    setenv("HPPLITE_CAPTURE", "pages", 1);
    db = NULL;
    rc = sqlite3_open_v2("file:" DIR "/env.db?hpplite=on&privkey=" KEY "&datadir=" DIR, &db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    opened = rc == SQLITE_OK && hpplite_get_node(db) != NULL;
    sqlite3_close(db);
    unsetenv("HPPLITE_CAPTURE");
    if (opened) FAIL("HPPLITE_CAPTURE=pages opened: the variable is gone, even for the value that stayed");
    PASS();
}

/* HPPLITE_PARAMS applies to a URI open too, under the URI's own parameters (#86) */
static void test_params_under_uri(void) {
    TEST("HPPLITE_PARAMS applies to a hpplite=on URI; the URI's own parameters win");
    reset_dir();
    setenv("HPPLITE_PARAMS", "dbname=fromparams&interval=0", 1);
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2("file:" DIR "/p1.db?hpplite=on&privkey=" KEY "&datadir=" DIR, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    HppliteNode *n = rc == SQLITE_OK ? hpplite_get_node(db) : NULL;
    char name1[32] = "";
    if (n && n->ctx->zDbName) snprintf(name1, sizeof(name1), "%s", n->ctx->zDbName);
    sqlite3_close(db);
    db = NULL;
    rc = sqlite3_open_v2("file:" DIR "/p2.db?hpplite=on&privkey=" KEY "&datadir=" DIR "&dbname=fromuri", &db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    n = rc == SQLITE_OK ? hpplite_get_node(db) : NULL;
    char name2[32] = "";
    if (n && n->ctx->zDbName) snprintf(name2, sizeof(name2), "%s", n->ctx->zDbName);
    sqlite3_close(db);
    unsetenv("HPPLITE_PARAMS");
    if (strcmp(name1, "fromparams") != 0) FAILF("dbname from HPPLITE_PARAMS: '%s', want fromparams", name1);
    if (strcmp(name2, "fromuri") != 0) FAILF("dbname with the URI's own: '%s', want fromuri", name2);
    PASS();
}

int main(void) {
    printf("=== HPPLite enabled from the environment (#55) ===\n");
    test_suite_init("env", NULL, DIR);
    test_plain_path_enabled();
    test_others_untouched();
    test_uri_still_wins();
    test_read_only_open();
    test_capture_values();
    test_params_under_uri();
    return test_report();
}
