/*
** The page root and the page batch (#146): a root kept one path at a time
** equals a scan; batches of diffs taken from the VFS's commits rebuild the
** file and its root.
*/
#include "test_lib.h"
#include "pages.h"
#include "walread.h"
#include <sys/stat.h>

#define DIR "/tmp/hpplite_test_pages"

static int exec_ok(sqlite3 *db, const char *sql) { return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK; }
static unsigned char *read_file(const char *path, size_t *pLen) {
    FILE *f = fopen(path, "rb"); unsigned char *buf; long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1); if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; } fclose(f);
    *pLen = (size_t)n; return buf;
}

static void test_root_incremental_equals_scan(void) {
    TEST("a root kept page by page, through growth and truncation, equals the root of a scan");
    HpplitePageRoot *inc = hpplite_pageroot_new(512);
    unsigned char page[512], a[32], b[32];
    unsigned i;
    for (i = 1; i <= 100; i++) { memset(page, (int)i, sizeof page); hpplite_pageroot_set(inc, i, page); }   /* past the first capacity of 16 */
    memset(page, 7, sizeof page); hpplite_pageroot_set(inc, 37, page);
    hpplite_pageroot_truncate(inc, 60);
    HpplitePageRoot *scan = hpplite_pageroot_new(512);
    for (i = 1; i <= 60; i++) { memset(page, i == 37 ? 7 : (int)i, sizeof page); hpplite_pageroot_set(scan, i, page); }
    hpplite_pageroot_get(inc, a); hpplite_pageroot_get(scan, b);
    int same = memcmp(a, b, 32) == 0 && hpplite_pageroot_crc(inc) == hpplite_pageroot_crc(scan) && hpplite_pageroot_pages(inc) == 60;
    /* one more page after the truncation: a different root */
    memset(page, 9, sizeof page); hpplite_pageroot_set(inc, 61, page); hpplite_pageroot_get(inc, a);
    int moved = memcmp(a, b, 32) != 0;
    hpplite_pageroot_free(inc); hpplite_pageroot_free(scan);
    if (!same) FAIL("incremental root or checksum differs from the scan's");
    if (!moved) FAIL("a new page left the root unchanged");
    PASS();
}

/* ---- commits from the WAL reader, batched as diffs, applied onto a fresh file ---- */
typedef struct { unsigned nPages; unsigned *aPgno; unsigned char **aImg; unsigned nTruncate; } Commit;
typedef struct { Commit *a; int n, alloc; int pageSize; } Spool;
static void sink(void *pArg, const HppliteWalCommit *c) {
    Spool *s = pArg; Commit *k; unsigned i;
    if (s->n == s->alloc) { s->alloc = s->alloc ? s->alloc * 2 : 64; s->a = realloc(s->a, (size_t)s->alloc * sizeof(Commit)); }
    k = &s->a[s->n++]; k->nPages = c->nPages; k->nTruncate = c->nTruncate;
    k->aPgno = malloc(c->nPages * sizeof(unsigned)); k->aImg = malloc(c->nPages * sizeof(unsigned char *));
    for (i = 0; i < c->nPages; i++) { k->aPgno[i] = c->aPgno[i]; k->aImg[i] = malloc((size_t)c->pageSize); memcpy(k->aImg[i], c->aImg[i], (size_t)c->pageSize); }
    s->pageSize = c->pageSize;
}

static void test_batches_of_diffs_rebuild_file_and_root(void) {
    TEST("page batches of diffs, 5 commits each, rebuild the file byte for byte and carry the roots a replica recomputes");
    reset_dir();
    sqlite3 *db = NULL;
    Spool s; memset(&s, 0, sizeof s);
    if (sqlite3_open(DIR "/w.db", &db) != SQLITE_OK) FAIL("open");
    if (!exec_ok(db, "PRAGMA journal_mode=WAL")) FAIL("WAL");
    hpplite_walread_watch(sqlite3_db_filename(db, "main"), sink, &s, 0);
    if (!exec_ok(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, k INTEGER, v TEXT); CREATE INDEX tk ON t(k)")) FAIL("schema");
    for (int i = 0; i < 30; i++) {
        char sql[200];
        snprintf(sql, sizeof sql, "BEGIN; WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i < 60)"
                                  " INSERT INTO t(k, v) SELECT i * %d, 'row ' || i || ' batch %d' FROM s; COMMIT", i + 1, i);
        if (!exec_ok(db, sql)) FAIL("insert");
        if (i % 5 == 2 && !exec_ok(db, "DELETE FROM t WHERE id % 9 = 0")) FAIL("delete");
        if (i == 20) { hpplite_walread_poll(sqlite3_db_filename(db, "main"), 0); if (!exec_ok(db, "PRAGMA wal_checkpoint(TRUNCATE)")) FAIL("checkpoint"); }
    }
    hpplite_walread_poll(sqlite3_db_filename(db, "main"), 0);
    if (!exec_ok(db, "PRAGMA wal_checkpoint(TRUNCATE)")) FAIL("final checkpoint");
    hpplite_walread_poll(sqlite3_db_filename(db, "main"), 0); hpplite_walread_unwatch(sqlite3_db_filename(db, "main"));
    sqlite3_close(db);

    /* the sequencer's side: its root and the previous images (by page), batches of 5 commits */
    int ps = s.pageSize;
    HpplitePageRoot *seq = hpplite_pageroot_new(ps);
    unsigned maxPg = 0; int i; unsigned j;
    for (i = 0; i < s.n; i++) for (j = 0; j < s.a[i].nPages; j++) if (s.a[i].aPgno[j] > maxPg) maxPg = s.a[i].aPgno[j];
    unsigned char **prev = calloc(maxPg + 1, sizeof(*prev)), **last = calloc(maxPg + 1, sizeof(*last));
    unsigned char *dirty = calloc(maxPg + 1, 1);
    /* the replica's side: a file in memory, its root */
    size_t repLen = 0, repCap = (size_t)(maxPg + 1) * (size_t)ps;
    unsigned char *rep = calloc(repCap, 1);
    HpplitePageRoot *repRoot = hpplite_pageroot_new(ps);
    int nBatches = 0, rootsOk = 1, decodeOk = 1;
    size_t bytes = 0;
    for (i = 0; i < s.n; i++) {
        for (j = 0; j < s.a[i].nPages; j++) { unsigned pg = s.a[i].aPgno[j]; last[pg] = s.a[i].aImg[j]; dirty[pg] = 1; }
        unsigned commitSize = s.a[i].nTruncate;
        if (i % 5 == 4 || i == s.n - 1) {
            /* build: the dirty set, diffed against the previous batch's images; the root after */
            HpplitePageBatch b; memset(&b, 0, sizeof b);
            b.pageSize = (unsigned)ps; b.commitSize = commitSize; b.firstCommit = (uint64_t)(i - i % 5 + 1); b.lastCommit = (uint64_t)(i + 1);
            HpplitePageRec *recs = calloc(maxPg + 1, sizeof(HpplitePageRec));
            unsigned char *bodies = malloc((size_t)(maxPg + 1) * (size_t)ps);
            unsigned pg, n = 0;
            for (pg = 1; pg <= maxPg; pg++) if (dirty[pg]) {
                unsigned char *body = bodies + (size_t)n * (size_t)ps;
                if (pg <= commitSize) {
                    hpplite_pageroot_set(seq, pg, last[pg]);
                    if (prev[pg]) { for (int k = 0; k < ps; k++) body[k] = last[pg][k] ^ prev[pg][k]; recs[n].flags = HPPLITE_PAGE_DIFF; }
                    else { memcpy(body, last[pg], (size_t)ps); recs[n].flags = HPPLITE_PAGE_FULL; }
                    recs[n].pgno = pg; recs[n].body = body; n++;
                    if (!prev[pg]) prev[pg] = malloc((size_t)ps);
                    memcpy(prev[pg], last[pg], (size_t)ps);
                }
                dirty[pg] = 0;
            }
            hpplite_pageroot_truncate(seq, commitSize);
            for (pg = commitSize + 1; pg <= maxPg; pg++) if (prev[pg]) { free(prev[pg]); prev[pg] = NULL; }
            b.nPages = n; b.aPage = recs;
            hpplite_pageroot_get(seq, b.root); b.crc = hpplite_pageroot_crc(seq);
            size_t len = 0; unsigned char *enc = hpplite_pagebatch_encode(&b, &len);
            bytes += len;
            /* the replica: decode, apply diffs onto its file, truncate, recompute, compare roots */
            HpplitePageBatch *d = hpplite_pagebatch_decode(enc, len);
            if (!d || d->nPages != n || d->commitSize != commitSize) decodeOk = 0;
            else {
                for (j = 0; j < d->nPages; j++) {
                    size_t off = (size_t)(d->aPage[j].pgno - 1) * (size_t)ps;
                    if (d->aPage[j].flags & HPPLITE_PAGE_DIFF) for (int k = 0; k < ps; k++) rep[off + k] ^= d->aPage[j].body[k];
                    else memcpy(rep + off, d->aPage[j].body, (size_t)ps);
                    hpplite_pageroot_set(repRoot, d->aPage[j].pgno, rep + off);
                }
                repLen = (size_t)d->commitSize * (size_t)ps;
                hpplite_pageroot_truncate(repRoot, d->commitSize);
                unsigned char r[32]; hpplite_pageroot_get(repRoot, r);
                if (memcmp(r, d->root, 32) != 0 || hpplite_pageroot_crc(repRoot) != d->crc) rootsOk = 0;
            }
            hpplite_pagebatch_free(d); sqlite3_free(enc); free(recs); free(bodies);
            nBatches++;
        }
    }
    size_t fileLen = 0; unsigned char *file = read_file(DIR "/w.db", &fileLen);
    int same = file && fileLen == repLen && memcmp(file, rep, fileLen) == 0;
    printf("(%d commits in %d batches, %zu bytes of batches, file %zu bytes) ", s.n, nBatches, bytes, fileLen);
    free(file); free(rep); free(dirty); free(last);
    for (j = 0; j <= maxPg; j++) free(prev[j]);
    free(prev); hpplite_pageroot_free(seq); hpplite_pageroot_free(repRoot);
    for (i = 0; i < s.n; i++) { for (j = 0; j < s.a[i].nPages; j++) free(s.a[i].aImg[j]); free(s.a[i].aImg); free(s.a[i].aPgno); }
    free(s.a);
    if (!decodeOk) FAIL("a batch did not decode as encoded");
    if (!rootsOk) FAIL("a replica's root after a batch differs from the trailer's");
    if (!same) FAIL("the replica's file differs from the sequencer's");
    PASS();
}

static void test_root_from_file(void) {
    TEST("the root from a scan of the file equals the root kept while writing it");
    reset_dir();
    sqlite3 *db = NULL;
    Spool s; memset(&s, 0, sizeof s);
    if (sqlite3_open(DIR "/f.db", &db) != SQLITE_OK) FAIL("open");
    if (!exec_ok(db, "PRAGMA journal_mode=WAL")) FAIL("WAL");
    hpplite_walread_watch(sqlite3_db_filename(db, "main"), sink, &s, 0);
    if (!exec_ok(db, "CREATE TABLE t(x); INSERT INTO t VALUES(zeroblob(5000)); INSERT INTO t VALUES(zeroblob(9000)); DELETE FROM t WHERE rowid = 1; INSERT INTO t VALUES(1)")) FAIL("writes");
    hpplite_walread_poll(sqlite3_db_filename(db, "main"), 0);
    if (!exec_ok(db, "PRAGMA wal_checkpoint(TRUNCATE)")) FAIL("checkpoint");
    hpplite_walread_poll(sqlite3_db_filename(db, "main"), 0); hpplite_walread_unwatch(sqlite3_db_filename(db, "main"));
    sqlite3_close(db);
    HpplitePageRoot *kept = hpplite_pageroot_new(s.pageSize);
    int i; unsigned j;
    for (i = 0; i < s.n; i++) { for (j = 0; j < s.a[i].nPages; j++) if (s.a[i].aPgno[j] <= s.a[i].nTruncate) hpplite_pageroot_set(kept, s.a[i].aPgno[j], s.a[i].aImg[j]); hpplite_pageroot_truncate(kept, s.a[i].nTruncate); }
    int ps = 0; HpplitePageRoot *scanned = hpplite_pageroot_from_file(DIR "/f.db", &ps);
    unsigned char a[32], b[32]; hpplite_pageroot_get(kept, a); hpplite_pageroot_get(scanned, b);
    int same = scanned && ps == s.pageSize && memcmp(a, b, 32) == 0 && hpplite_pageroot_pages(kept) == hpplite_pageroot_pages(scanned);
    hpplite_pageroot_free(kept); hpplite_pageroot_free(scanned);
    for (i = 0; i < s.n; i++) { for (j = 0; j < s.a[i].nPages; j++) free(s.a[i].aImg[j]); free(s.a[i].aImg); free(s.a[i].aPgno); }
    free(s.a);
    if (!same) FAIL("the scanned root differs from the kept one");
    PASS();
}

int main(void) {
    printf("=== HPPLite page root and page batch ===\n");
    test_suite_init("pages", NULL, DIR);
    test_root_incremental_equals_scan();
    test_batches_of_diffs_rebuild_file_and_root();
    test_root_from_file();
    remove_dir();
    return test_report();
}
