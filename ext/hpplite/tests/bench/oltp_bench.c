/*
** HPPLite OLTP benchmark in the shape of sysbench's oltp_* scripts (#140).
**
** A plain SQLite connection (this binary's own amalgamation), optionally
** with a loadable extension (--ext path/hpplite.dylib) loaded first, so one
** harness measures any version of HPPLite through its public surface.
**
**   sbtest1..N(id INTEGER PRIMARY KEY, k INTEGER, c TEXT(120), pad TEXT(60)), index on k
**   read_write  per transaction: 10 point selects, 1 simple range, 1 sum range,
**               1 order range, 1 distinct range, 1 index update, 1 non-index
**               update, 1 delete + 1 insert               (18 statements)
**   write_only  index update, non-index update, delete + insert   (4)
**   read_only   the 14 reads
**
** Usage: oltp_bench [--ext FILE] [--mode read_write|write_only|read_only]
**                   [--skip-trx] [--tables N] [--rows M] [--seconds S]
**                   [--dir DIR] [--seed S] [--label TEXT] [--json FILE]
**                   [--uri "&param=value..."] [--hist]
** Not part of ctest; tools/bench_versions.sh runs it per tag.
*/
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#define RANGE 100
#define PREPARE_CHUNK 500   /* rows per transaction while loading: below one DA batch on every version (#57) */
#define PRIVKEY_HEX "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"

typedef enum { MODE_RW, MODE_WO, MODE_RO } Mode;

static struct {
    const char *ext, *dir, *label, *json, *uriExtra;
    Mode mode;
    int skipTrx, tables, rows, seconds, hist;
    uint64_t seed;
} opt = { NULL, "/tmp/hpplite_oltp", "", NULL, "", MODE_RW, 0, 4, 10000, 10, 0, 42 };

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {          /* xorshift64* */
    rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
    return rng * 2685821657736338717ull;
}
static int rnd_id(void) { return 1 + (int)(rnd() % (uint64_t)opt.rows); }
/* sysbench's "###########-###########-..." strings */
static void rnd_str(char *out, int groups) {
    int i, g, n = 0;
    for (g = 0; g < groups; g++) {
        if (g) out[n++] = '-';
        for (i = 0; i < 11; i++) out[n++] = '0' + (char)(rnd() % 10);
    }
    out[n] = 0;
}

static void die(const char *what, sqlite3 *db) {
    fprintf(stderr, "oltp_bench: %s: %s\n", what, db ? sqlite3_errmsg(db) : "?");
    exit(1);
}
static void run(sqlite3 *db, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "oltp_bench: %s: %s\n", sql, err ? err : "?");
        exit(1);
    }
}
static sqlite3_stmt *prep(sqlite3 *db, const char *fmt, int t) {
    char sql[256];
    sqlite3_stmt *s;
    snprintf(sql, sizeof(sql), fmt, t);
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) die(sql, db);
    return s;
}
/* step to completion; reads drain their rows */
static void exec_stmt(sqlite3 *db, sqlite3_stmt *s) {
    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {}
    if (rc != SQLITE_DONE) die(sqlite3_sql(s), db);
    sqlite3_reset(s);
}

static sqlite3_int64 status_int(sqlite3 *db, const char *col) {
    char sql[128];
    sqlite3_stmt *s;
    sqlite3_int64 v = -1;
    snprintf(sql, sizeof(sql), "SELECT %s FROM hpplite_status", col);
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return v;
}
static long long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : 0;
}
static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
static double pct(double *a, size_t n, double p) {
    size_t i = (size_t)(p * (double)(n - 1) + 0.5);
    return n ? a[i < n ? i : n - 1] : 0;
}

typedef struct {
    sqlite3_stmt *point, *range, *sum, *order, *distinct, *idxUpd, *nonIdxUpd, *del, *ins;
} TableStmts;

int main(int argc, char **argv) {
    int i, t;
    char path[1024], uri[2048];
    sqlite3 *boot = NULL, *db;
    sqlite3_stmt *begin = NULL, *commit = NULL;
    TableStmts *ts;
    double *lat = NULL;
    size_t nLat = 0, capLat = 0;
    double t0, t1, prepMs, prepFlushMs = 0, runMs, flushMs = 0, deadline, sum = 0;
    long long txns = 0, stmts = 0, stmtsPerTxn;
    sqlite3_int64 heightBefore = -1, heightAfter = -1, pendingAfter = -1, pendingLeft = -1;
    long long dbBytes;
    char c120[132], pad60[72];

    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--skip-trx")) { opt.skipTrx = 1; continue; }
        if (!strcmp(a, "--hist")) { opt.hist = 1; continue; }
        if (!v) { fprintf(stderr, "oltp_bench: %s needs a value\n", a); return 2; }
        if (!strcmp(a, "--ext")) opt.ext = v;
        else if (!strcmp(a, "--dir")) opt.dir = v;
        else if (!strcmp(a, "--label")) opt.label = v;
        else if (!strcmp(a, "--json")) opt.json = v;
        else if (!strcmp(a, "--uri")) opt.uriExtra = v;
        else if (!strcmp(a, "--tables")) opt.tables = atoi(v);
        else if (!strcmp(a, "--rows")) opt.rows = atoi(v);
        else if (!strcmp(a, "--seconds")) opt.seconds = atoi(v);
        else if (!strcmp(a, "--seed")) opt.seed = strtoull(v, NULL, 10);
        else if (!strcmp(a, "--mode")) {
            if (!strcmp(v, "read_write")) opt.mode = MODE_RW;
            else if (!strcmp(v, "write_only")) opt.mode = MODE_WO;
            else if (!strcmp(v, "read_only")) opt.mode = MODE_RO;
            else { fprintf(stderr, "oltp_bench: unknown mode %s\n", v); return 2; }
        } else { fprintf(stderr, "oltp_bench: unknown option %s\n", a); return 2; }
        i++;
    }
    if (opt.tables < 1 || opt.rows < RANGE + 1 || opt.seconds < 1) { fprintf(stderr, "oltp_bench: bad sizes\n"); return 2; }
    rng ^= opt.seed * 0x2545F4914F6CDD1Dull;

    {
        char cmd[1200];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", opt.dir, opt.dir);
        if (system(cmd) != 0) { fprintf(stderr, "oltp_bench: cannot reset %s\n", opt.dir); return 1; }
    }
    snprintf(path, sizeof(path), "%s/bench.db", opt.dir);

    if (opt.ext) {
        char *err = NULL;
        /* the bootstrap connection stays open: closing it would unload the
        ** module whose auto-extension every later open relies on */
        if (sqlite3_open(":memory:", &boot) != SQLITE_OK) die("open boot", boot);
        sqlite3_enable_load_extension(boot, 1);
        if (sqlite3_load_extension(boot, opt.ext, NULL, &err) != SQLITE_OK) {
            fprintf(stderr, "oltp_bench: load %s: %s\n", opt.ext, err ? err : "?");
            return 1;
        }
        snprintf(uri, sizeof(uri), "file:%s?hpplite=on&l1=local&privkey=0x" PRIVKEY_HEX "&datadir=%s%s", path, opt.dir, opt.uriExtra);
    } else {
        snprintf(uri, sizeof(uri), "file:%s?%s", path, opt.uriExtra);
    }
    if (sqlite3_open_v2(uri, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL) != SQLITE_OK) die("open", db);
    run(db, "PRAGMA journal_mode=WAL");

    /* ---- prepare: tables, rows, index ---- */
    t0 = now_ms();
    for (t = 1; t <= opt.tables; t++) {
        char sql[256];
        sqlite3_stmt *ins;
        int r;
        snprintf(sql, sizeof(sql), "CREATE TABLE sbtest%d(id INTEGER PRIMARY KEY, k INTEGER NOT NULL DEFAULT 0, c TEXT NOT NULL DEFAULT '', pad TEXT NOT NULL DEFAULT '')", t);
        run(db, sql);
        ins = prep(db, "INSERT INTO sbtest%d(id,k,c,pad) VALUES(?,?,?,?)", t);
        for (r = 1; r <= opt.rows; r++) {
            if ((r - 1) % PREPARE_CHUNK == 0) run(db, "BEGIN");
            rnd_str(c120, 10); rnd_str(pad60, 5);
            sqlite3_bind_int(ins, 1, r);
            sqlite3_bind_int(ins, 2, rnd_id());
            sqlite3_bind_text(ins, 3, c120, -1, SQLITE_STATIC);
            sqlite3_bind_text(ins, 4, pad60, -1, SQLITE_STATIC);
            exec_stmt(db, ins);
            if (r % PREPARE_CHUNK == 0 || r == opt.rows) run(db, "COMMIT");
        }
        sqlite3_finalize(ins);
        snprintf(sql, sizeof(sql), "CREATE INDEX k_%d ON sbtest%d(k)", t, t);
        run(db, sql);
    }
    prepMs = now_ms() - t0;
    if (opt.ext) {
        /* the loaded rows leave the pending table before the run, so the
        ** run's pending count, batches and flush are the run's own */
        t0 = now_ms();
        run(db, "SELECT hpplite_flush()");
        prepFlushMs = now_ms() - t0;
    }

    /* ---- statements ---- */
    ts = calloc((size_t)opt.tables + 1, sizeof(*ts));
    for (t = 1; t <= opt.tables; t++) {
        ts[t].point = prep(db, "SELECT c FROM sbtest%d WHERE id=?", t);
        ts[t].range = prep(db, "SELECT c FROM sbtest%d WHERE id BETWEEN ? AND ?", t);
        ts[t].sum = prep(db, "SELECT SUM(k) FROM sbtest%d WHERE id BETWEEN ? AND ?", t);
        ts[t].order = prep(db, "SELECT c FROM sbtest%d WHERE id BETWEEN ? AND ? ORDER BY c", t);
        ts[t].distinct = prep(db, "SELECT DISTINCT c FROM sbtest%d WHERE id BETWEEN ? AND ? ORDER BY c", t);
        ts[t].idxUpd = prep(db, "UPDATE sbtest%d SET k=k+1 WHERE id=?", t);
        ts[t].nonIdxUpd = prep(db, "UPDATE sbtest%d SET c=? WHERE id=?", t);
        ts[t].del = prep(db, "DELETE FROM sbtest%d WHERE id=?", t);
        ts[t].ins = prep(db, "INSERT INTO sbtest%d(id,k,c,pad) VALUES(?,?,?,?)", t);
    }
    if (!opt.skipTrx) { begin = prep(db, "BEGIN%.0d", 0); commit = prep(db, "COMMIT%.0d", 0); }
    stmtsPerTxn = opt.mode == MODE_RW ? 18 : opt.mode == MODE_WO ? 4 : 14;
    if (opt.ext) heightBefore = status_int(db, "height");

    /* ---- run ---- */
    t0 = now_ms();
    deadline = t0 + opt.seconds * 1000.0;
    for (;;) {
        TableStmts *s = &ts[1 + (int)(rnd() % (uint64_t)opt.tables)];
        double a = now_ms(), d;
        if (a >= deadline) break;
        if (begin) exec_stmt(db, begin);
        if (opt.mode != MODE_WO) {
            int lo;
            for (i = 0; i < 10; i++) { sqlite3_bind_int(s->point, 1, rnd_id()); exec_stmt(db, s->point); }
            lo = rnd_id(); sqlite3_bind_int(s->range, 1, lo); sqlite3_bind_int(s->range, 2, lo + RANGE - 1); exec_stmt(db, s->range);
            lo = rnd_id(); sqlite3_bind_int(s->sum, 1, lo); sqlite3_bind_int(s->sum, 2, lo + RANGE - 1); exec_stmt(db, s->sum);
            lo = rnd_id(); sqlite3_bind_int(s->order, 1, lo); sqlite3_bind_int(s->order, 2, lo + RANGE - 1); exec_stmt(db, s->order);
            lo = rnd_id(); sqlite3_bind_int(s->distinct, 1, lo); sqlite3_bind_int(s->distinct, 2, lo + RANGE - 1); exec_stmt(db, s->distinct);
        }
        if (opt.mode != MODE_RO) {
            int id;
            sqlite3_bind_int(s->idxUpd, 1, rnd_id()); exec_stmt(db, s->idxUpd);
            rnd_str(c120, 10);
            sqlite3_bind_text(s->nonIdxUpd, 1, c120, -1, SQLITE_STATIC); sqlite3_bind_int(s->nonIdxUpd, 2, rnd_id()); exec_stmt(db, s->nonIdxUpd);
            id = rnd_id();
            sqlite3_bind_int(s->del, 1, id); exec_stmt(db, s->del);
            rnd_str(c120, 10); rnd_str(pad60, 5);
            sqlite3_bind_int(s->ins, 1, id); sqlite3_bind_int(s->ins, 2, rnd_id());
            sqlite3_bind_text(s->ins, 3, c120, -1, SQLITE_STATIC); sqlite3_bind_text(s->ins, 4, pad60, -1, SQLITE_STATIC);
            exec_stmt(db, s->ins);
        }
        if (commit) exec_stmt(db, commit);
        d = now_ms() - a;
        if (nLat == capLat) { capLat = capLat ? capLat * 2 : 65536; lat = realloc(lat, capLat * sizeof(double)); }
        lat[nLat++] = d;
        sum += d;
        txns++;
        stmts += stmtsPerTxn;
    }
    t1 = now_ms();
    runMs = t1 - t0;

    if (opt.ext) {
        heightAfter = status_int(db, "height");
        pendingAfter = status_int(db, "pending");
        t0 = now_ms();
        run(db, "SELECT hpplite_flush()");
        flushMs = now_ms() - t0;
        pendingLeft = status_int(db, "pending");
    }
    for (t = 1; t <= opt.tables; t++) {
        sqlite3_finalize(ts[t].point); sqlite3_finalize(ts[t].range); sqlite3_finalize(ts[t].sum);
        sqlite3_finalize(ts[t].order); sqlite3_finalize(ts[t].distinct); sqlite3_finalize(ts[t].idxUpd);
        sqlite3_finalize(ts[t].nonIdxUpd); sqlite3_finalize(ts[t].del); sqlite3_finalize(ts[t].ins);
    }
    sqlite3_finalize(begin); sqlite3_finalize(commit);
    run(db, "PRAGMA wal_checkpoint(TRUNCATE)");
    sqlite3_close(db);
    dbBytes = file_size(path);

    qsort(lat, nLat, sizeof(double), cmp_double);
    {
        const char *mode = opt.mode == MODE_RW ? "read_write" : opt.mode == MODE_WO ? "write_only" : "read_only";
        double tps = txns / (runMs / 1000.0), qps = stmts / (runMs / 1000.0);
        printf("%-12s %-10s %-4s tables=%d rows=%d %ds  tps=%.0f stmt/s=%.0f  lat ms avg=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.2f  prepare=%.0f rows/s",
               opt.label[0] ? opt.label : (opt.ext ? "ext" : "plain"), mode, opt.skipTrx ? "auto" : "trx",
               opt.tables, opt.rows, opt.seconds, tps, qps, nLat ? sum / (double)nLat : 0,
               pct(lat, nLat, 0.50), pct(lat, nLat, 0.95), pct(lat, nLat, 0.99), nLat ? lat[nLat - 1] : 0,
               (double)opt.tables * opt.rows / (prepMs / 1000.0));
        if (opt.ext) printf("  prepare-flush=%.0fms batches=%lld pending=%lld flush=%.0fms left=%lld", prepFlushMs, (long long)(heightAfter - heightBefore), (long long)pendingAfter, flushMs, (long long)pendingLeft);
        printf("  db=%.1fMB\n", dbBytes / 1048576.0);
        if (opt.hist) {
            static const double edge[] = { 0.25, 0.5, 1, 2, 5, 10, 100, 1000 };
            size_t k, b, cnt[9] = { 0 };
            for (k = 0; k < nLat; k++) {
                for (b = 0; b < 8 && lat[k] >= edge[b]; b++) {}
                cnt[b]++;
            }
            printf("  latency histogram (ms):");
            for (b = 0; b < 8; b++) printf(" <%g:%zu", edge[b], cnt[b]);
            printf(" >=1000:%zu\n", cnt[8]);
        }
        if (opt.json) {
            FILE *f = fopen(opt.json, "a");
            if (!f) { perror(opt.json); return 1; }
            fprintf(f, "{\"label\":\"%s\",\"ext\":%s,\"mode\":\"%s\",\"trx\":%s,\"tables\":%d,\"rows\":%d,\"seconds\":%d,"
                       "\"txns\":%lld,\"tps\":%.1f,\"stmts_per_s\":%.1f,\"lat_avg_ms\":%.4f,\"lat_p50_ms\":%.4f,\"lat_p95_ms\":%.4f,"
                       "\"lat_p99_ms\":%.4f,\"lat_max_ms\":%.3f,\"prepare_rows_per_s\":%.0f,\"batches\":%lld,\"pending\":%lld,"
                       "\"flush_ms\":%.1f,\"pending_after_flush\":%lld,\"prepare_flush_ms\":%.1f,\"db_bytes\":%lld}\n",
                    opt.label, opt.ext ? "true" : "false", mode, opt.skipTrx ? "false" : "true", opt.tables, opt.rows, opt.seconds,
                    txns, tps, qps, nLat ? sum / (double)nLat : 0, pct(lat, nLat, 0.50), pct(lat, nLat, 0.95), pct(lat, nLat, 0.99),
                    nLat ? lat[nLat - 1] : 0, (double)opt.tables * opt.rows / (prepMs / 1000.0),
                    opt.ext ? (long long)(heightAfter - heightBefore) : 0, (long long)pendingAfter, flushMs, (long long)pendingLeft, prepFlushMs, dbBytes);
            fclose(f);
        }
    }
    free(lat); free(ts);
    if (boot) sqlite3_close(boot);
    return 0;
}
