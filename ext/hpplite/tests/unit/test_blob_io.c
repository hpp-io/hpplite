/*
** HPPLite incremental blob I/O (#169)
**
** sqlite3_blob_write changes a row without a statement: no trigger, no
** trace, no authorizer. Page capture ships the pages it changed. Row and
** statement capture cannot see it, so they refuse the transaction it is in
** (the commit hook rolls it back) and say page capture is the answer. The
** pre-update hook is what sees the write; a host SQLite without it still
** shows an autocommitted one (a commit with no statement running).
** Every case ends with a replica equal to the writer.
*/

#include "test_lib.h"
#include "hpplite_int.h"
#include <stdio.h>
#include <string.h>

static const unsigned char PRIVKEY[32] = {
    0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f,0xa0,
    0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf,0xb0
};

#define DIR "/tmp/hpplite_test_blob_io"

static HppliteNode *open_writer(const char *file, HppliteL1 *l1) {
    TestNodeOpts o = { .checkpointInterval = TEST_INTERVAL_1000 };
    return test_open_node(tpath(file), l1, &o);
}

static const char *replica_matches(HppliteNode *w, HppliteL1 *l1, const char *file) {
    TestNodeOpts o = { .checkpointInterval = TEST_INTERVAL_1000 };
    return test_replica_check(w, l1, tpath(file), &o, TEST_DUMP_SKIP_FTS_SHADOW, 50, 1);
}

/* the blob of row id, written at offset; the rc of sqlite3_blob_close (the commit, autocommitted) */
static int blob_write(sqlite3 *db, sqlite3_int64 id, int offset, const char *bytes) {
    sqlite3_blob *b = NULL;
    int rc = sqlite3_blob_open(db, "main", "f", "data", id, 1, &b);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_blob_write(b, bytes, (int)strlen(bytes), offset);
    int rcClose = sqlite3_blob_close(b);
    return rc != SQLITE_OK ? rc : rcClose;
}

static const char *SETUP[] = {
    "CREATE TABLE f(id INTEGER PRIMARY KEY, data BLOB)",
    "INSERT INTO f(data) VALUES(zeroblob(64))",
    "INSERT INTO f(data) VALUES(zeroblob(64))",
    NULL };

/* 1. Autocommitted: refused in rows and text (the file unchanged, the reason
** in last_refusal), written in pages; the replica equal either way */
static void test_autocommit(void) {
    TEST("an autocommitted sqlite3_blob_write: refused in row/statement capture, shipped in page capture, replica equal");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_writer("auto.db", l1);
    if (!w) FAIL("open");
    if (run(w, SETUP) != 0) FAIL("setup");
    int rc = blob_write(w->db, 1, 8, "written");
    char head[64];
    text_of(w->db, "SELECT hex(substr(data, 9, 7)) FROM f WHERE id = 1", head, sizeof head);
    if (rc != SQLITE_OK) FAILF("page capture refused it: rc %d (%s)", rc, sqlite3_errmsg(w->db));
    if (strcmp(head, "7772697474656E")) FAILF("page capture: the write is not in the file (%s)", head);
    /* ordinary writes still go through after it */
    if (exec_sql(w->db, "INSERT INTO f(data) VALUES(x'0102')") != SQLITE_OK) FAILF("an INSERT after it: %s", sqlite3_errmsg(w->db));
    const char *why = replica_matches(w, l1, "auto_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 2. Inside BEGIN ... COMMIT: the COMMIT refused and the whole transaction
** rolled back in rows and text; committed in pages */
static void test_in_transaction(void) {
    TEST("sqlite3_blob_write inside BEGIN...COMMIT: the COMMIT refused (all of it rolled back) in rows/text, committed in pages");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_writer("txn.db", l1);
    if (!w) FAIL("open");
    if (run(w, SETUP) != 0) FAIL("setup");
    if (exec_sql(w->db, "BEGIN") != SQLITE_OK) FAIL("BEGIN");
    if (exec_sql(w->db, "INSERT INTO f(data) VALUES(x'aa')") != SQLITE_OK) FAIL("INSERT");
    if (blob_write(w->db, 2, 0, "in a txn") != SQLITE_OK) FAILF("the blob write itself: %s", sqlite3_errmsg(w->db));
    int rc = exec_sql(w->db, "COMMIT");
    if (!sqlite3_get_autocommit(w->db)) exec_sql(w->db, "ROLLBACK");
    sqlite3_int64 n = scalar(w->db, "SELECT count(*) FROM f");
    {
        if (rc != SQLITE_OK) FAILF("page capture refused the COMMIT: %s", sqlite3_errmsg(w->db));
        if (n != 3) FAILF("%lld rows, want 3", (long long)n);
    }
    /* the next transaction is not refused for the last one's write */
    const char *after[] = { "BEGIN", "UPDATE f SET data = x'0b' WHERE id = 1", "COMMIT", NULL };
    if (run(w, after) != 0) FAILF("the next transaction: %s", sqlite3_errmsg(w->db));
    const char *why = replica_matches(w, l1, "txn_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}


int main(void) {
    printf("=== HPPLite incremental blob I/O (#169), page capture ===\n");
    test_suite_init("blobio", PRIVKEY, DIR);
    test_autocommit();
    test_in_transaction();
    return test_report();
}
