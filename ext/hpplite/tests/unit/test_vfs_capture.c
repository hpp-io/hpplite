/*
** The VFS's page capture (#146): a watched database's commits reach the sink
** as pages, and those pages rebuild the file byte for byte.
*/
#include "test_lib.h"
#include "vfs.h"
#include "clock.h"
#include <sys/stat.h>

#define DIR "/tmp/hpplite_test_vfs_capture"

typedef struct { unsigned nPages; unsigned *aPgno; unsigned char **aImg; unsigned nTruncate; } Commit;
typedef struct { Commit *a; int n, alloc; int pageSize; long long bytes; } Spool;

static void sink(void *pArg, const HppliteVfsCommit *c) {
    Spool *s = pArg;
    Commit *k;
    unsigned i;
    if (s->n == s->alloc) { s->alloc = s->alloc ? s->alloc * 2 : 64; s->a = realloc(s->a, (size_t)s->alloc * sizeof(Commit)); }
    k = &s->a[s->n++];
    k->nPages = c->nPages; k->nTruncate = c->nTruncate;
    k->aPgno = malloc(c->nPages * sizeof(unsigned)); k->aImg = malloc(c->nPages * sizeof(unsigned char *));
    for (i = 0; i < c->nPages; i++) {
        k->aPgno[i] = c->aPgno[i];
        k->aImg[i] = malloc((size_t)c->pageSize); memcpy(k->aImg[i], c->aImg[i], (size_t)c->pageSize);
        s->bytes += c->pageSize;
    }
    s->pageSize = c->pageSize;
}
static void spool_free(Spool *s) {
    int i; unsigned j;
    for (i = 0; i < s->n; i++) { for (j = 0; j < s->a[i].nPages; j++) free(s->a[i].aImg[j]); free(s->a[i].aImg); free(s->a[i].aPgno); }
    free(s->a); memset(s, 0, sizeof(*s));
}
/* apply the spool into a buffer: pages at their offsets, the file cut to each commit's size */
static unsigned char *rebuild(Spool *s, size_t *pLen) {
    unsigned char *buf = NULL; size_t cap = 0, len = 0; int i; unsigned j;
    for (i = 0; i < s->n; i++) {
        size_t need = (size_t)s->a[i].nTruncate * (size_t)s->pageSize;
        if (need > cap) { buf = realloc(buf, need); memset(buf + cap, 0, need - cap); cap = need; }
        for (j = 0; j < s->a[i].nPages; j++) {
            size_t off = (size_t)(s->a[i].aPgno[j] - 1) * (size_t)s->pageSize;
            if (off + (size_t)s->pageSize > cap) { buf = realloc(buf, off + (size_t)s->pageSize); memset(buf + cap, 0, off + (size_t)s->pageSize - cap); cap = off + (size_t)s->pageSize; }
            memcpy(buf + off, s->a[i].aImg[j], (size_t)s->pageSize);
        }
        len = need;
    }
    *pLen = len;
    return buf;
}
static unsigned char *read_file(const char *path, size_t *pLen) {
    FILE *f = fopen(path, "rb"); unsigned char *buf; long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n); if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; } fclose(f);
    *pLen = (size_t)n; return buf;
}
static int exec_ok(sqlite3 *db, const char *sql) { return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK; }

static void test_commits_rebuild_the_file(void) {
    TEST("a watched database's commits, with checkpoints and a WAL restart between, rebuild the file byte for byte");
    reset_dir();
    sqlite3 *db = NULL;
    Spool s; memset(&s, 0, sizeof s);
    if (sqlite3_open(DIR "/w.db", &db) != SQLITE_OK) FAIL("open");
    if (!hpplite_clock_on(db)) FAIL("the connection is not on the hpplite VFS");
    if (!exec_ok(db, "PRAGMA journal_mode=WAL")) FAIL("WAL");
    if (hpplite_vfs_watch(sqlite3_db_filename(db, "main"), sink, &s) != SQLITE_OK) FAIL("watch");
    if (!exec_ok(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, k INTEGER, v TEXT); CREATE INDEX tk ON t(k)")) FAIL("schema");
    for (int i = 0; i < 40; i++) {
        char sql[200];
        snprintf(sql, sizeof sql, "BEGIN; WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i < 50)"
                                  " INSERT INTO t(k, v) SELECT i * %d, 'row ' || i || ' of batch %d' FROM s; COMMIT", i + 1, i);
        if (!exec_ok(db, sql)) FAIL("insert");
        if (i == 15) { if (!exec_ok(db, "PRAGMA wal_checkpoint(TRUNCATE)")) FAIL("checkpoint"); }   /* the WAL starts over */
        if (i % 7 == 3 && !exec_ok(db, "UPDATE t SET v = v || '!' WHERE id % 13 = 0")) FAIL("update");
        if (i % 11 == 5 && !exec_ok(db, "DELETE FROM t WHERE id % 17 = 0")) FAIL("delete");
    }
    if (!exec_ok(db, "PRAGMA wal_checkpoint(TRUNCATE)")) FAIL("final checkpoint");
    int commits = s.n;
    hpplite_vfs_unwatch(sqlite3_db_filename(db, "main"));
    sqlite3_close(db);
    size_t fileLen = 0, builtLen = 0;
    unsigned char *file = read_file(DIR "/w.db", &fileLen), *built = rebuild(&s, &builtLen);
    printf("(%d commits, %lld bytes of pages, file %zu bytes) ", commits, s.bytes, fileLen);
    int same = file && built && fileLen == builtLen && memcmp(file, built, fileLen) == 0;
    free(file); free(built); spool_free(&s);
    if (commits < 40) FAILF("only %d commits reached the sink", commits);
    if (!same) FAIL("the rebuilt file differs from the live one");
    PASS();
}

static void test_unwatched_and_late_watch(void) {
    TEST("an unwatched database reaches no sink; a watch set after the WAL is open captures what follows");
    reset_dir();
    sqlite3 *db = NULL;
    Spool s; memset(&s, 0, sizeof s);
    if (sqlite3_open(DIR "/u.db", &db) != SQLITE_OK) FAIL("open");
    if (!exec_ok(db, "PRAGMA journal_mode=WAL; CREATE TABLE t(x); INSERT INTO t VALUES(1)")) FAIL("write");
    if (s.n != 0) FAIL("a sink was called for an unwatched database");
    if (hpplite_vfs_watch(sqlite3_db_filename(db, "main"), sink, &s) != SQLITE_OK) FAIL("watch");
    if (!exec_ok(db, "INSERT INTO t VALUES(2); INSERT INTO t VALUES(3)")) FAIL("write 2");
    hpplite_vfs_unwatch(sqlite3_db_filename(db, "main"));
    if (!exec_ok(db, "INSERT INTO t VALUES(4)")) FAIL("write 3");
    sqlite3_close(db);
    int n = s.n; spool_free(&s);
    if (n != 2) FAILF("expected the 2 commits made while watched, got %d", n);
    PASS();
}

int main(void) {
    printf("=== HPPLite VFS page capture ===\n");
    test_suite_init("vfscap", NULL, DIR);
    test_commits_rebuild_the_file();
    test_unwatched_and_late_watch();
    remove_dir();
    return test_report();
}
