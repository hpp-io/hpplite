/*
** Premise test for the soft replica of #166 (entry 12 of
** doc/sqlite-dependencies.md), stock SQLite only: a read transaction pinned
** at a WAL frame bounds every checkpoint there, the frames past the pin can
** be cut off the WAL file and the database reopens at the pin's state, and
** a pin held under a writer costs WAL growth and nothing else.
*/
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define DB "/tmp/hpplite_test_wal_pin.db"
static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("  Testing: %s... ", name); fflush(stdout); } while (0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while (0)
#define FAILF(...) do { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); return; } while (0)

static int exec(sqlite3 *db, const char *sql) { return sqlite3_exec(db, sql, 0, 0, 0) == SQLITE_OK; }
static sqlite3_int64 scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s; sqlite3_int64 v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, 0) != SQLITE_OK) return -1;
    if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s); return v;
}
static int integrity_ok(sqlite3 *db) {
    sqlite3_stmt *s; int ok = 0;
    sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &s, 0);
    if (sqlite3_step(s) == SQLITE_ROW) ok = strcmp((const char *)sqlite3_column_text(s, 0), "ok") == 0;
    sqlite3_finalize(s); return ok;
}
static void fresh(void) { unlink(DB); unlink(DB "-wal"); unlink(DB "-shm"); }
/* every connection here closes without a checkpoint: the WAL and the -shm stay as they are */
static sqlite3 *open_db(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(DB, &db) != SQLITE_OK) return NULL;
    sqlite3_db_config(db, SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, 0);
    exec(db, "PRAGMA journal_mode=WAL"); exec(db, "PRAGMA wal_autocheckpoint=0");
    return db;
}
static long wal_frames(void) { struct stat st; if (stat(DB "-wal", &st) != 0 || st.st_size <= 32) return 0; return (st.st_size - 32) / (24 + 4096); }
static int write_rows(sqlite3 *db, int n) { int i; for (i = 0; i < n; i++) if (!exec(db, "INSERT INTO t(v) VALUES(zeroblob(2000))")) return 0; return 1; }
/* a pin: a read transaction holding the snapshot of this moment */
static sqlite3 *pin_open(void) { sqlite3 *p = open_db(); if (!p || !exec(p, "BEGIN") || scalar(p, "SELECT count(*) FROM t") < 0) return NULL; return p; }

static void test_pin_bounds_checkpoint(void) {
    TEST("a reader pinned at a frame bounds every checkpoint there: PASSIVE stops, TRUNCATE is BUSY, the pin still reads its snapshot");
    fresh();
    sqlite3 *w = open_db(), *pin;
    int nLog = -1, nCkpt = -1, rc;
    long hardFrames;
    if (!w || !exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)") || !write_rows(w, 3)) FAIL("setup");
    exec(w, "PRAGMA wal_checkpoint(TRUNCATE)");
    if (!write_rows(w, 2)) FAIL("hard rows");                     /* hard, and still in the WAL when the pin is taken */
    hardFrames = wal_frames();
    pin = pin_open();
    if (!pin) FAIL("pin");
    if (!write_rows(w, 5)) FAIL("soft rows");                     /* soft: after the pin */
    rc = sqlite3_wal_checkpoint_v2(w, NULL, SQLITE_CHECKPOINT_PASSIVE, &nLog, &nCkpt);
    if (rc != SQLITE_OK || nCkpt != hardFrames) FAILF("PASSIVE checkpointed %d of %d frames, the pin is at %ld (rc %d)", nCkpt, nLog, hardFrames, rc);
    rc = sqlite3_wal_checkpoint_v2(w, NULL, SQLITE_CHECKPOINT_TRUNCATE, &nLog, &nCkpt);
    if (rc != SQLITE_BUSY) FAILF("TRUNCATE past the pin returned %d, want SQLITE_BUSY", rc);
    if (scalar(pin, "SELECT count(*) FROM t") != 5 || scalar(w, "SELECT count(*) FROM t") != 10) FAIL("the pin reads 5, the writer 10");
    sqlite3_close(pin); sqlite3_close(w);
    PASS();
}

static void test_cut_at_pin(void) {
    TEST("the frames past the pin cut off the WAL file: the database reopens at the pin's state, intact, writable");
    fresh();
    sqlite3 *w = open_db(), *pin, *r;
    long hardFrames;
    sqlite3_int64 hardRows, got;
    int nLog, nCkpt;
    if (!w || !exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)") || !write_rows(w, 3)) FAIL("setup");
    exec(w, "PRAGMA wal_checkpoint(TRUNCATE)");
    if (!write_rows(w, 2)) FAIL("hard rows");
    hardFrames = wal_frames(); hardRows = scalar(w, "SELECT count(*) FROM t");
    pin = pin_open();
    if (!pin || !write_rows(w, 4)) FAIL("pin and soft rows");
    /* a partial checkpoint under the pin: pages the soft commits rewrote are not copied even at their hard version, the
    ** -shm's nBackfill moves to the pin; both must be harmless to the cut */
    sqlite3_wal_checkpoint_v2(w, NULL, SQLITE_CHECKPOINT_PASSIVE, &nLog, &nCkpt);
    sqlite3_close(pin); sqlite3_close(w);                         /* NO_CKPT_ON_CLOSE: the WAL keeps every frame, the -shm stays */
    if (wal_frames() != hardFrames + 4 * 2 && wal_frames() <= hardFrames) FAIL("the WAL did not survive the closes");
    if (truncate(DB "-wal", 32 + hardFrames * (24 + 4096)) != 0) FAIL("truncate");
    r = open_db();                                                /* the first opener rebuilds the wal-index from the file */
    if (!r) FAIL("reopen");
    got = scalar(r, "SELECT count(*) FROM t");
    if (!integrity_ok(r)) FAIL("integrity_check after the cut");
    if (got != hardRows) FAILF("%lld rows after the cut, want the pin's %lld", (long long)got, (long long)hardRows);
    if (!exec(r, "INSERT INTO t(v) VALUES('after')") || scalar(r, "SELECT count(*) FROM t") != hardRows + 1) FAIL("a write after the cut");
    sqlite3_close(r);
    PASS();
}

static void test_long_pin(void) {
    TEST("a pin held under 3000 commits: the WAL grows, the autocheckpoint waits, the release lets a TRUNCATE checkpoint take it all");
    fresh();
    sqlite3 *w = open_db(), *pin;
    int nLog = -1, nCkpt = -1, rc;
    if (!w || !exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)") || !write_rows(w, 1)) FAIL("setup");
    exec(w, "PRAGMA wal_autocheckpoint=1000");
    exec(w, "PRAGMA wal_checkpoint(TRUNCATE)");
    pin = pin_open();
    if (!pin || !write_rows(w, 3000)) FAIL("pin and rows");
    if (wal_frames() < 3000) FAILF("the WAL has %ld frames after 3000 commits under the pin", wal_frames());
    if (scalar(pin, "SELECT count(*) FROM t") != 1) FAIL("the pin still reads its snapshot");
    exec(pin, "COMMIT"); sqlite3_close(pin);
    rc = sqlite3_wal_checkpoint_v2(w, NULL, SQLITE_CHECKPOINT_TRUNCATE, &nLog, &nCkpt);
    if (rc != SQLITE_OK || wal_frames() != 0) FAILF("after the release: rc %d, %ld frames left", rc, wal_frames());
    sqlite3_close(w);
    PASS();
}

int main(void) {
    printf("test_wal_pin: a pinned reader bounds checkpoints; the WAL past it can be cut (SQLite %s)\n", sqlite3_libversion());
    test_pin_bounds_checkpoint();
    test_cut_at_pin();
    test_long_pin();
    fresh();
    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
