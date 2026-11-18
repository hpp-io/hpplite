/*
** The fuzz targets' starting inputs (#343): valid page batches made by the encoder (a whole batch with a name, a
** piece with a diff record) and -wal files SQLite wrote (one commit, several). fuzz_seeds <dir> writes them as
** <dir>/pagebatch/*.bin and <dir>/walread/*.wal.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pages.h"
#include "hpplite_sqlite.h"

static void put(const char *dir, const char *sub, const char *name, const unsigned char *p, size_t n) {
    char path[600];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", dir, sub); mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/%s/%s", dir, sub, name);
    if ((f = fopen(path, "wb"))) { fwrite(p, 1, n, f); fclose(f); }
}

static void batch(const char *dir, const char *name, unsigned pageSize, unsigned nPages, int diff, unsigned piece, const char *zName) {
    HpplitePageBatch b;
    HpplitePageRec recs[4];
    unsigned char *bodies = calloc(nPages, pageSize), *bytes;
    size_t len = 0;
    memset(&b, 0, sizeof b);
    for (unsigned i = 0; i < nPages; i++) {
        memset(bodies + i * pageSize, 0x11 * (i + 1), pageSize);
        recs[i].pgno = i + 1; recs[i].flags = diff && i ? HPPLITE_PAGE_DIFF : HPPLITE_PAGE_FULL; recs[i].body = bodies + i * pageSize;
    }
    b.pageSize = pageSize; b.commitSize = nPages; b.firstCommit = 1; b.lastCommit = 2; b.salt1 = 7; b.salt2 = 9;
    b.timestampMs = 1800000000000ull; b.nPages = nPages; b.aPage = recs; memset(b.root, 0xab, 32); b.crc = 42;
    b.zName = zName; b.nName = zName ? (unsigned)strlen(zName) : 0; b.piece = piece; b.bMore = piece ? 1 : 0;
    if ((bytes = hpplite_pagebatch_encode(&b, &len))) { put(dir, "pagebatch", name, bytes, len); sqlite3_free(bytes); }
    free(bodies);
}

static void wal(const char *dir, const char *name, int commits) {
    char db[300], walPath[310], sql[200];
    sqlite3 *h;
    FILE *f;
    unsigned char *p;
    long n;
    snprintf(db, sizeof db, "/tmp/hpplite_fuzz_seed_%d.db", (int)getpid());
    snprintf(walPath, sizeof walPath, "%s-wal", db);
    unlink(db); unlink(walPath);
    if (sqlite3_open(db, &h) != SQLITE_OK) return;
    sqlite3_exec(h, "PRAGMA page_size=1024; PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0; CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT)", NULL, NULL, NULL);
    for (int i = 0; i < commits; i++) {
        snprintf(sql, sizeof sql, "INSERT INTO t(v) VALUES(printf('%%.*c', %d, 'x'))", 50 + i * 300);
        sqlite3_exec(h, sql, NULL, NULL, NULL);
    }
    if ((f = fopen(walPath, "rb"))) {                    /* copied while the connection holds it: a close would checkpoint it away */
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        if ((p = malloc((size_t)n)) && fread(p, 1, (size_t)n, f) == (size_t)n) put(dir, "walread", name, p, (size_t)n);
        free(p); fclose(f);
    }
    sqlite3_close(h);
    unlink(db); unlink(walPath);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: fuzz_seeds <dir>\n"); return 2; }
    mkdir(argv[1], 0755);
    batch(argv[1], "whole_named.bin", 512, 2, 0, 0, "seed");
    batch(argv[1], "piece_diff.bin", 1024, 3, 1, 2, NULL);
    wal(argv[1], "one_commit.wal", 1);
    wal(argv[1], "five_commits.wal", 5);
    printf("fuzz_seeds: wrote %s/pagebatch and %s/walread\n", argv[1], argv[1]);
    return 0;
}
