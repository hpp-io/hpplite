/*
** HPPLite test library (#106): see test_lib.h
*/
#include "test_lib.h"
#include "l1_interface.h"
#include "compress.h"
#include <strings.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>

int tests_run = 0, tests_passed = 0;
int test_verbose = 0;

int test_rows_mode(void) {
    const char *v = getenv("HPPLITE_CAPTURE");
    return !(v && strcasecmp(v, "text") == 0);
}
int test_pages_mode(void) {
    const char *v = getenv("HPPLITE_CAPTURE");
    return v && strcasecmp(v, "pages") == 0;
}
int test_pending_is(long long pending, int n) {
    return pending == n || (test_pages_mode() && pending == n + 1);
}

int test_report(void) {
    printf("\n%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}

/* ---- the suite ---- */

static const unsigned char DEFAULT_PRIVKEY[32] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20
};
static const char *g_nodeId = "test";
static const unsigned char *g_privkey = DEFAULT_PRIVKEY;
static char g_dir[256] = "/tmp/hpplite_test";

uint8_t *test_readable_bytes(const uint8_t *raw, size_t len, size_t *pLen) {
    uint8_t *out;
    if (hpplite_is_plain_compressed(raw, len)) return hpplite_inflate(raw + HPPLITE_PLAIN_MAGIC_LEN, len - HPPLITE_PLAIN_MAGIC_LEN, pLen);
    out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, raw, len);
    out[len] = 0;
    if (pLen) *pLen = len;
    return out;
}

void test_suite_init(const char *nodeId, const unsigned char privkey[32], const char *dir) {
    if (nodeId) g_nodeId = nodeId;
    if (privkey) g_privkey = privkey;
    if (dir) snprintf(g_dir, sizeof g_dir, "%s", dir);
    else snprintf(g_dir, sizeof g_dir, "/tmp/hpplite_test_%s.%d", g_nodeId, (int)getpid());
}

const char *test_dir(void) { return g_dir; }

const char *tpath(const char *file) {
    static char buf[4][512];
    static int which;
    char *out = buf[which = (which + 1) & 3];
    snprintf(out, sizeof buf[0], "%s/%s", g_dir, file);
    return out;
}

void reset_dir(void) {
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", g_dir, g_dir);
    (void)!system(cmd);
}

void remove_dir(void) {
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    (void)!system(cmd);
}

/* ---- SQL ---- */

sqlite3_int64 scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s;
    sqlite3_int64 v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return v;
}

void text_of(sqlite3 *db, const char *sql, char *out, size_t n) {
    sqlite3_stmt *s;
    out[0] = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return;
    if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_text(s, 0)) snprintf(out, n, "%s", (const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
}

int count_rows(sqlite3 *db, const char *table) {
    char *sql = sqlite3_mprintf("SELECT count(*) FROM \"%w\"", table);
    int n = sql ? (int)scalar(db, sql) : -1;
    sqlite3_free(sql);
    return n;
}

int exec_sql(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK && test_verbose) fprintf(stderr, "  [%s]: %s\n", sql, err ? err : "?");
    sqlite3_free(err);
    return rc;
}

int run(HppliteNode *n, const char **aSql) {
    for (int i = 0; aSql[i]; i++) {
        char *err = NULL;
        if (sqlite3_exec(n->db, aSql[i], NULL, NULL, &err) != SQLITE_OK) {
            printf("[%s: %s] ", aSql[i], err ? err : "?");
            sqlite3_free(err);
            return -1;
        }
    }
    return 0;
}

char *query_text(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st;
    sqlite3_str *s = sqlite3_str_new(NULL);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            for (int i = 0; i < sqlite3_column_count(st); i++) {
                const char *v = (const char *)sqlite3_column_text(st, i);
                sqlite3_str_appendf(s, "%s|", v ? v : "NULL");
            }
            sqlite3_str_appendf(s, "\n");
        }
        sqlite3_finalize(st);
    }
    return sqlite3_str_finish(s);
}

/* ---- nodes ---- */

HppliteNode *test_open_node(const char *dbPath, HppliteL1 *l1, const TestNodeOpts *o) {
    static const TestNodeOpts none;
    HppliteNodeConfig config;
    HppliteNode *node;
    if (!o) o = &none;
    memset(&config, 0, sizeof(config));
    config.nodeId = (char *)(o->nodeId ? o->nodeId : g_nodeId);
    memcpy(config.privkey, o->privkey ? o->privkey : g_privkey, 32);
    config.hasPrivkey = 1;
    config.dataDir = g_dir;
    config.dbPath = (char *)dbPath;
    config.checkpointInterval = o->checkpointInterval;                   /* 0: the node's default, as in the config (#114) */
    config.maxBatchBytes = o->maxBatchBytes;
    config.noEncrypt = o->noEncrypt;
    config.noCompress = o->noCompress;
    config.verifyOnly = o->verifyOnly;
    config.determinismWarn = o->determinismWarn;
    config.captureText = o->captureText;
    config.capturePages = o->capturePages || (!o->captureText && test_pages_mode());   /* HPPLITE_CAPTURE=pages runs a suite in page mode (#146) */
    config.storeUrl = (char *)o->storeUrl;
    config.storeCap = o->storeCap;
    config.storeFallback = !o->storeFallbackOff;
    config.localOnly = o->localOnly > 0 || (o->localOnly == 0 && l1 == NULL);   /* no chain: the local throw-away mode (#38) */
    node = hpplite_node_create(&config);
    if (!node) return NULL;
    if (l1) {
#ifdef HPPLITE_TEST_HOOKS
        /* the DA limit is the contract's; the mock stands in for it */
        size_t limit = o->daLimit ? o->daLimit : o->maxBatchBytes;
        if (limit) hpplite_l1_mock_set_batch_size_limit(l1, limit);
#endif
        hpplite_node_set_l1(node, l1);
        if (!o->noSync) hpplite_node_sync_from_l1(node, 1);
        hpplite_node_start(node);
    } else if (o->start) {
        hpplite_node_start(node);
    }
    return node;
}

int same_root(HppliteNode *a, HppliteNode *b) {
    unsigned char ra[32], rb[32];
    hpplite_node_get_state_root(a, ra);
    hpplite_node_get_state_root(b, rb);
    return memcmp(ra, rb, 32) == 0;
}

/* ---- the dump ---- */

static void dump_value(sqlite3_str *out, sqlite3_stmt *st, int i) {
    switch (sqlite3_column_type(st, i)) {
        case SQLITE_INTEGER: sqlite3_str_appendf(out, "i%lld,", (long long)sqlite3_column_int64(st, i)); break;
        case SQLITE_FLOAT: {
            double d = sqlite3_column_double(st, i);
            unsigned long long u;
            memcpy(&u, &d, 8);
            sqlite3_str_appendf(out, "f%llx,", u);
            break;
        }
        case SQLITE_NULL: sqlite3_str_appendall(out, "n,"); break;
        default: {
            int n = sqlite3_column_bytes(st, i);
            const unsigned char *b = sqlite3_column_blob(st, i);
            sqlite3_str_appendchar(out, 1, sqlite3_column_type(st, i) == SQLITE_TEXT ? 't' : 'b');
            for (int k = 0; k < n; k++) sqlite3_str_appendf(out, "%02x", b[k]);
            sqlite3_str_appendchar(out, 1, ',');
        }
    }
}

char *test_dump(sqlite3 *db, unsigned flags) {
    sqlite3_str *out = sqlite3_str_new(NULL);
    sqlite3_stmt *tabs;
    char *sel = sqlite3_mprintf(
        "SELECT name FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE '\\_hpplite\\_%%' ESCAPE '\\'"
        " AND sql NOT LIKE 'CREATE VIRTUAL%%'%s%s ORDER BY name",
        flags & TEST_DUMP_SKIP_SQLITE ? " AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\'" : "",
        flags & TEST_DUMP_SKIP_FTS_SHADOW
            ? " AND name NOT LIKE '%\\_data' ESCAPE '\\' AND name NOT LIKE '%\\_idx' ESCAPE '\\'"
              " AND name NOT LIKE '%\\_docsize' ESCAPE '\\' AND name NOT LIKE '%\\_config' ESCAPE '\\'"
              " AND name NOT LIKE '%\\_content' ESCAPE '\\'"
            : "");
    if (sel && sqlite3_prepare_v2(db, sel, -1, &tabs, NULL) == SQLITE_OK) {
        while (sqlite3_step(tabs) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(tabs, 0);
            sqlite3_stmt *st = NULL;
            char *q = NULL;
            sqlite3_str_appendf(out, "[%s]", t);
            /* a rowid table with its rowids (records address rows by them), in rowid order;
            ** not SQLite's own tables (sqlite_sequence), whose rows SQLite rewrites as it likes */
            if (!(flags & TEST_DUMP_NO_ROWID) && strncmp(t, "sqlite_", 7) != 0) {
                q = sqlite3_mprintf("SELECT rowid, * FROM main.\"%w\" ORDER BY rowid", t);
                if (!q || sqlite3_prepare_v2(db, q, -1, &st, NULL) != SQLITE_OK) { sqlite3_free(q); q = NULL; st = NULL; }
            }
            /* else (WITHOUT ROWID) in the order of all its columns */
            if (!st) {
                sqlite3_str *o = sqlite3_str_new(NULL);
                sqlite3_stmt *probe;
                int nCol = 0;
                q = sqlite3_mprintf("SELECT * FROM main.\"%w\"", t);
                if (q && sqlite3_prepare_v2(db, q, -1, &probe, NULL) == SQLITE_OK) { nCol = sqlite3_column_count(probe); sqlite3_finalize(probe); }
                sqlite3_free(q);
                sqlite3_str_appendf(o, "SELECT * FROM main.\"%w\"", t);
                for (int i = 1; i <= nCol; i++) sqlite3_str_appendf(o, "%s%d", i == 1 ? " ORDER BY " : ", ", i);
                q = sqlite3_str_finish(o);
                if (!q || sqlite3_prepare_v2(db, q, -1, &st, NULL) != SQLITE_OK) st = NULL;
            }
            if (st) {
                while (sqlite3_step(st) == SQLITE_ROW) {
                    for (int i = 0; i < sqlite3_column_count(st); i++) dump_value(out, st, i);
                    sqlite3_str_appendchar(out, 1, ';');
                }
                sqlite3_finalize(st);
            }
            sqlite3_free(q);
        }
        sqlite3_finalize(tabs);
    }
    sqlite3_free(sel);
    return sqlite3_str_finish(out);
}

size_t test_dump_diff(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] && a[i] == b[i]) i++;
    while (i > 0 && a[i - 1] != ';' && a[i - 1] != ']') i--;
    return i;
}

/* ---- the replica check ---- */

const char *test_replica_check(HppliteNode *w, HppliteL1 *l1, const char *replicaPath, const TestNodeOpts *replicaOpts,
                               unsigned dumpFlags, int flushRounds, int destroyWriter) {
    static char why[400];
    unsigned char rootW[32], rootR[32];
    char *a, *b;
    for (int i = 0; i < flushRounds && hpplite_node_pending_count(w) > 0; i++) hpplite_node_flush_batch(w);
    if (flushRounds > 0 && hpplite_node_pending_count(w) != 0) return "writer still has pending rows after flushing";
    a = test_dump(w->db, dumpFlags);
    hpplite_state_root(w->db, rootW);
    if (destroyWriter) hpplite_node_destroy(w);

    HppliteNode *r = test_open_node(replicaPath, l1, replicaOpts);
    if (!r) { sqlite3_free(a); return "replica open"; }
    if (r->state == HPPLITE_STATE_ERROR) { sqlite3_free(a); hpplite_node_destroy(r); return "replica sync failed"; }
    b = test_dump(r->db, dumpFlags);
    hpplite_state_root(r->db, rootR);
    unsigned long long rHeight = (unsigned long long)hpplite_node_get_height(r);
    char rErr[100]; snprintf(rErr, sizeof rErr, "%s", r->ctx ? r->ctx->zLastError : "");
    hpplite_node_destroy(r);
    int rows = strcmp(a, b) != 0, root = memcmp(rootW, rootR, 32) != 0;
    if (rows) {
        size_t i = test_dump_diff(a, b);
        snprintf(why, sizeof(why), "replica rows differ: writer [%.70s] replica [%.70s] (replica height %llu, last error: %s)", a + i, b + i, rHeight, rErr);
    } else if (root) {
        snprintf(why, sizeof(why), "replica rows equal but its state root differs");
    }
    sqlite3_free(a); sqlite3_free(b);
    return rows || root ? why : NULL;
}

/* ---- hpplite_status ---- */

const char *status_text(sqlite3 *db, const char *col) {
    static char buf[2][512];
    static int which;
    char *out = buf[which ^= 1];
    char sql[128];
    snprintf(sql, sizeof sql, "SELECT %s FROM hpplite_status", col);
    text_of(db, sql, out, sizeof buf[0]);
    return out;
}

sqlite3_int64 status_int(sqlite3 *db, const char *col) {
    char sql[128];
    snprintf(sql, sizeof sql, "SELECT %s FROM hpplite_status", col);
    return scalar(db, sql);
}

/* ---- services a suite starts (#177) ---- */

int test_free_port(void) {
    struct sockaddr_in a;
    socklen_t n = sizeof a;
    int fd = socket(AF_INET, SOCK_STREAM, 0), port = -1;
    if (fd < 0) return -1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0 && getsockname(fd, (struct sockaddr *)&a, &n) == 0)
        port = ntohs(a.sin_port);
    close(fd);
    return port;
}

int test_child_alive(pid_t pid) {
    return pid > 0 && waitpid(pid, NULL, WNOHANG) == 0;
}
