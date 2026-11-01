/*
** HPPLite against the real contracts on a local anvil chain (issue #47).
** Driven by tests/l1/anvil_test.sh, which provides HPPLITE_ANVIL_RPC,
** HPPLITE_ANVIL_FACTORY, HPPLITE_ANVIL_KEY0 (our wallet) and
** HPPLITE_ANVIL_KEY1 (a stranger). Time travel through `cast rpc`.
**
** What the mock cannot show: real claim/renew/release transactions and
** receipts, lease expiry by chain time, a second instance of our own
** wallet, a stranger's claim after expiry, sleeping for days, and (#51)
** that what capture accepts survives the real path: sealed, posted to
** contract storage at the size limit, replayed by a fresh replica.
*/
#include "test_lib.h"
#include "l1_interface.h"
#include "eth/eth_client.h"
#include "batch.h"
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#define DIR "/tmp/hpplite_anvil"

static const char *RPC, *FACTORY, *KEY0, *KEY1, *KEY2, *KEY3, *KEY4, *KEY5, *ANVIL_PID;

static void chain_sleep(int seconds) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "cast rpc evm_increaseTime %d --rpc-url %s >/dev/null && cast rpc evm_mine --rpc-url %s >/dev/null", seconds, RPC, RPC);
    (void)!system(cmd);
}

static sqlite3 *open_db(const char *file, const char *key, const char *extra) {
    char uri[1024];
    sqlite3 *db = NULL;
    snprintf(uri, sizeof(uri), "file:" DIR "/%s?hpplite=on&rpc=%s&factory=%s&chainid=31337&privkey=%s&datadir=" DIR "&interval=0%s",
             file, RPC, FACTORY, key, extra ? extra : "");
    if (sqlite3_open_v2(uri, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL) != SQLITE_OK) {
        fprintf(stderr, "open: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

static void dump_state(HppliteL1 *l1, const unsigned char *mine, const char *why) {
    HppliteL1State *st = hpplite_l1_get_state(l1);
    if (!st) { printf("[%s: no state] ", why); return; }
    printf("\n    [%s] holder=", why); for (int i = 0; i < 12; i++) printf("%02x", st->sequencerInstance[i]);
    printf(" mine="); for (int i = 0; i < 12; i++) printf("%02x", mine[i]);
    printf(" expiry=%llu chainNow=%llu wall=%llu\n",
           (unsigned long long)st->leaseExpiry, (unsigned long long)st->currentTimestamp, (unsigned long long)time(NULL));
    hpplite_l1_state_free(st);
}

static int lease_is(HppliteL1 *l1, const unsigned char *instanceId) {
    HppliteL1State *st = hpplite_l1_get_state(l1);
    int ours;
    if (!st) return -1;
    ours = st->leaseExpiry > st->currentTimestamp && memcmp(st->sequencerInstance, instanceId, 32) == 0;
    hpplite_l1_state_free(st);
    return ours;
}

/* 1. The documented quick start, end to end on a real chain */
static void test_open_write_post_checkpoint(void) {
    TEST("open via factory, write, batch on chain, checkpoint anchored");
    sqlite3 *db = open_db("t1.db", KEY0, "&checkpoint=1");
    if (!db) FAIL("open");
    HppliteNode *n = hpplite_get_node(db);
    if (!n || !n->l1) FAIL("no L1 after open with factory");
    sqlite3_exec(db, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL, NULL, NULL);
    if (scalar(db, "SELECT hpplite_flush()") != 1) FAIL("flush should post batch 1");
    if (scalar(db, "SELECT height FROM hpplite_status") != 2) FAIL("height 2");
    if (scalar(db, "SELECT last_checkpoint_height FROM hpplite_status") != 1) FAIL("checkpoint 1 on chain");
    if (scalar(db, "SELECT lease FROM hpplite_status") != 1) FAIL("lease held");
    char c[43], d[43]; hpplite_l1_get_addresses(n->l1, c, d);
    unsigned char inst[32]; memcpy(inst, n->instanceId, 32);
    sqlite3_close(db);
    HppliteL1 *peek = hpplite_l1_connect(RPC, c);
    {
        /* the contract's configuration as the backend reads it (#44): the
        ** interval this node wrote back, and the lease length */
        HppliteSystemConfig *sc = hpplite_l1_get_system_config(peek);
        uint64_t ci = sc ? sc->checkpointInterval : 0, ld = sc ? sc->leaseDuration : 0;
        hpplite_l1_system_config_free(sc);
        if (ci != 1 || ld == 0) { hpplite_l1_disconnect(peek); FAILF("getSystemConfig as read: checkpointInterval %llu (want 1), leaseDuration %llu (want > 0)", (unsigned long long)ci, (unsigned long long)ld); }
    }
    if (lease_is(peek, inst) != 0) { dump_state(peek, inst, "after close"); hpplite_l1_disconnect(peek); FAIL("close must release the lease"); }
    hpplite_l1_disconnect(peek);
    PASS();
}

/* 1b. The verifier (#5): the tool rebuilds KEY0's rollup from the chain
** alone, matches the checkpoint the contract anchors, and says whether the
** live file equals it - before and after a write past the log */
static int run_verify(const char *extra, char *out, size_t n) {
    const char *bin = getenv("HPPLITE_VERIFY_BIN");
    char cmd[1600];
    FILE *p;
    size_t got = 0;
    int status;
    if (!bin) return -2;
    snprintf(cmd, sizeof cmd, "HPPLITE_PARAMS='rpc=%s&factory=%s&chainid=31337&privkey=%s' '%s' %s 2>&1",
             RPC, FACTORY, KEY0, bin, extra ? extra : "");
    p = popen(cmd, "r");
    if (!p) return -2;
    while (got + 1 < n && fgets(out + got, (int)(n - got), p)) got = strlen(out);
    status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
static void test_verify_tool(void) {
    char out[4096] = "", cmd[600];
    int rc;
    TEST("hpplite-verify: rebuilds from the chain, matches the anchored checkpoint, leaves the live sequencer's lease alone; --db sees a write past the log");
    if (!getenv("HPPLITE_VERIFY_BIN")) FAIL("HPPLITE_VERIFY_BIN not set by anvil_test.sh");
    /* the live sequencer holds the lease while the audit runs */
    sqlite3 *w = open_db("t1.db", KEY0, "&checkpoint=1");
    if (!w) FAIL("open writer");
    if (scalar(w, "SELECT lease FROM hpplite_status") != 1) { sqlite3_close(w); FAIL("writer should hold the lease"); }
    snprintf(cmd, sizeof cmd, "--db " DIR "/t1.db");
    rc = run_verify(cmd, out, sizeof out);
    if (rc != 0 || !strstr(out, "matches the rebuilt content") || !strstr(out, "equals the chain") || !strstr(out, "\nverified"))
        { sqlite3_close(w); FAILF("exit %d, output:\n%s", rc, out); }
    if (strstr(out, "holds the lease")) { sqlite3_close(w); FAILF("a verifier went for the lease:\n%s", out); }
    if (scalar(w, "SELECT lease FROM hpplite_status") != 1) { sqlite3_close(w); FAIL("the writer's lease was disturbed by the audit"); }
    sqlite3_close(w);
    /* a write by a plain connection: the live file no longer equals the chain */
    {
        sqlite3 *plain = NULL;
        if (sqlite3_open(DIR "/t1.db", &plain) != SQLITE_OK) FAIL("plain open");
        if (sqlite3_exec(plain, "INSERT INTO t VALUES(2)", NULL, NULL, NULL) != SQLITE_OK) { sqlite3_close(plain); FAIL("plain write"); }
        sqlite3_close(plain);
    }
    out[0] = 0;
    rc = run_verify(cmd, out, sizeof out);
    if (rc != 1 || !strstr(out, "DIFFERS from the chain") || !strstr(out, "table t: differs (2 rows live, 1 rebuilt)") || !strstr(out, "NOT verified"))
        FAILF("after a write past the log: exit %d, output:\n%s", rc, out);
    PASS();
}

/* 2. A second instance of our own wallet must not disturb the running sequencer (issue #40) */
static void test_second_instance_same_wallet(void) {
    TEST("second instance, same wallet: claim reverts, close does not expire the live lease");
    sqlite3 *a = open_db("t2a.db", KEY0, NULL);
    if (!a) FAIL("open a");
    HppliteNode *na = hpplite_get_node(a);
    sqlite3_exec(a, "INSERT INTO t VALUES(2)", NULL, NULL, NULL);
    sqlite3_int64 h0 = scalar(a, "SELECT hpplite_flush()");
    if (h0 == 0) { dump_state(na->l1, na->instanceId, "a's flush"); sqlite3_close(a); FAIL("a posts"); }
    unsigned char instA[32]; memcpy(instA, na->instanceId, 32);
    if (lease_is(na->l1, instA) != 1) FAIL("a holds the lease");

    /* the operator mistake: the same key opened again elsewhere (the C API claims at open) */
    char uri[1024];
    snprintf(uri, sizeof(uri), "file:" DIR "/t2b.db?hpplite=on&rpc=%s&factory=%s&chainid=31337&privkey=%s&datadir=" DIR "&interval=0", RPC, FACTORY, KEY0);
    sqlite3 *b = hpplite_open(uri);
    if (!b) FAIL("open b");
    HppliteNode *nb = hpplite_get_node(b);
    int bad = nb->hasLease;
    sqlite3_exec(b, "INSERT INTO t VALUES(99)", NULL, NULL, NULL);
    sqlite3_int64 hb = scalar(b, "SELECT hpplite_flush()");
    sqlite3_close(b);                                   /* must not forceExpireLease */
    int stillA = lease_is(na->l1, instA);
    sqlite3_exec(a, "INSERT INTO t VALUES(3)", NULL, NULL, NULL);
    sqlite3_int64 ha = scalar(a, "SELECT hpplite_flush()");
    sqlite3_close(a);
    if (bad) FAIL("b believes it holds a lease that a holds (blind claim, #40)");
    if (hb != 0) FAIL("b must not post into a's lease");
    if (stillA != 1) FAIL("a's lease was cut by b's close (#40)");
    if (ha == 0) FAIL("a keeps posting");
    PASS();
}

/* 3. Sleep for days, wake, write: one claim, then business as usual */
/* 5. The application calls into L1 (hpplite_get_l1) while the timer thread flushes: one curl handle */
static void *app_thread_rpcs(void *arg) {
    HppliteL1 *l1 = arg;
    for (int i = 0; i < 40; i++) {
        HppliteL1State *st = hpplite_l1_get_state(l1);
        if (st) hpplite_l1_state_free(st);
        HppliteSystemConfig *c = hpplite_l1_get_system_config(l1);
        if (c) hpplite_l1_system_config_free(c);
    }
    return NULL;
}
static void test_concurrent_rpc(void) {
    TEST("application RPCs through hpplite_get_l1() while the timer flushes (shared curl handle)");
    sqlite3 *a = open_db("t5.db", KEY0, "&interval=200ms");
    if (!a) FAIL("open");
    HppliteNode *na = hpplite_get_node(a);
    sqlite3_exec(a, "CREATE TABLE IF NOT EXISTS t(x)", NULL, NULL, NULL);
    pthread_t th;
    pthread_create(&th, NULL, app_thread_rpcs, na->l1);
    for (int i = 0; i < 20; i++) {
        sqlite3_exec(a, "INSERT INTO t VALUES(7)", NULL, NULL, NULL);
        usleep(100000);
    }
    pthread_join(th, NULL);
    if (na->state == HPPLITE_STATE_ERROR) FAIL("node in error");
    sqlite3_close(a);
    PASS();
}

static void test_sleep_and_wake(void) {
    TEST("lease expired by chain time after a long sleep: wake, claim, post");
    sqlite3 *a = open_db("t3.db", KEY0, NULL);
    if (!a) FAIL("open");
    chain_sleep(3 * 24 * 3600);
    HppliteNode *na = hpplite_get_node(a);
    if (hpplite_l1_is_lease_active(na->l1)) FAIL("lease should have expired");
    sqlite3_exec(a, "INSERT INTO t VALUES(4)", NULL, NULL, NULL);
    if (scalar(a, "SELECT hpplite_flush()") == 0) FAIL("flush after sleep");
    if (lease_is(na->l1, na->instanceId) != 1) FAIL("lease re-claimed");
    sqlite3_close(a);
    PASS();
}

/* 4. A stranger after expiry: what the contract allows today (#48 will make this revert) */
static void test_stranger_after_expiry(void) {
    TEST("stranger claims after expiry: node must not fork and must say so");
    sqlite3 *a = open_db("t4.db", KEY0, NULL);
    if (!a) FAIL("open");
    HppliteNode *na = hpplite_get_node(a);
    char c[43], d[43];
    hpplite_l1_get_addresses(na->l1, c, d);
    chain_sleep(301);
    HppliteL1 *stranger = hpplite_l1_connect(RPC, c);
    unsigned char k1[32]; eth_hex_decode(KEY1, k1, 32);
    hpplite_l1_set_privkey(stranger, k1);
    unsigned char inst[32]; memset(inst, 0xEE, 32);
    char *tx = hpplite_l1_claim_sequencer(stranger, inst);
    int claimed = tx && hpplite_l1_wait_for_tx(stranger, tx, 30) == 1;
    sqlite3_free(tx);
    printf("(stranger claim %s) ", claimed ? "ACCEPTED (old contract)" : "rejected");
    if (claimed) FAIL("the contract let a stranger claim the lease (#48)");
    sqlite3_exec(a, "INSERT INTO t VALUES(5)", NULL, NULL, NULL);
    uint64_t h = (uint64_t)scalar(a, "SELECT hpplite_flush()");
    if (claimed) {
        /* with the old contract: our node must keep the row pending, not post, not error */
        if (h != 0) FAIL("posted into a stranger's lease");
        if (scalar(a, "SELECT pending FROM hpplite_status") != 1) FAIL("row must stay pending");
        chain_sleep(301);
        if (scalar(a, "SELECT hpplite_flush()") == 0) FAIL("after the stranger's lease expires, we post again");
    } else if (h == 0) {
        FAIL("flush");
    }
    hpplite_l1_disconnect(stranger);
    sqlite3_close(a);
    PASS();
}


/* 6. Edge-case SQL and full-size batches end to end (#51). KEY2's own,
** fresh rollup: writer posts, a replica on another file rebuilds from the
** chain, roots must match, every posted batch within the contract's limit. */
static const char *aAllowed[] = {
    "CREATE TABLE e(id INTEGER PRIMARY KEY, v)",
    "INSERT INTO e(v) VALUES('random()')",
    "INSERT INTO e(v) VALUES('now')",
    "INSERT INTO e(v) VALUES('datetime(''now'')')",
    "INSERT INTO e(v) VALUES('CURRENT_TIMESTAMP')",
    "INSERT INTO e(v) VALUES('DELETE FROM _hpplite_pending')",
    "INSERT INTO e(v) VALUES('it''s now or never')",
    "INSERT INTO e(v) VALUES(upper('now'))",
    "INSERT INTO e(v) VALUES(datetime('2024-01-01 00:00:00'))",
    "INSERT INTO e(v) VALUES(date('2024-01-01', '+1 day'))",
    "INSERT INTO e(v) VALUES(strftime('%Y-%m-%d %H', '2024-02-29 13:00'))",
    "INSERT INTO e(v) VALUES(unixepoch('2024-01-01'))",
    "INSERT INTO e(v) VALUES(julianday('2024-01-01'))",
    "SELECT count(*) FROM _hpplite_pending",                  /* reads are fine; a write that reads the log is refused */
    "PRAGMA foreign_keys = OFF",
    "INSERT INTO e(v) VALUES('back\\slash \"double\" ''single'' tab\there')",
    "INSERT INTO e(v) VALUES('line one' || char(10) || 'line two' || char(13) || char(1) || char(31) || char(127))",
    "INSERT INTO e(v) VALUES('한국어 日本語 emoji 🚀🔥 ünïcödé')",
    "INSERT INTO e(v) VALUES(char(55357, 56832))",
    "INSERT INTO e(v) VALUES(x'00ff10ab')",
    "INSERT INTO e(v) VALUES(zeroblob(64))",
    "INSERT INTO e(v) VALUES(9223372036854775807)",
    "INSERT INTO e(v) VALUES(-9223372036854775808)",
    "INSERT INTO e(v) VALUES(1e308)",
    "INSERT INTO e(v) VALUES(NULL)",
    "INSERT INTO e(v) VALUES('')",
    "UPDATE e SET v = v || '/' WHERE typeof(v) = 'text'",
    /* foreign key actions and recursive triggers replay in the mode they ran in (#52) */
    "PRAGMA foreign_keys = ON",
    "CREATE TABLE fp(id INTEGER PRIMARY KEY)",
    "CREATE TABLE fc(id INTEGER PRIMARY KEY, p INTEGER REFERENCES fp(id) ON DELETE CASCADE)",
    "INSERT INTO fp VALUES(1), (2)",
    "INSERT INTO fc VALUES(10, 1), (20, 2)",
    "DELETE FROM fp WHERE id = 1",
    "PRAGMA foreign_keys = OFF",
    "PRAGMA recursive_triggers = ON",
    "CREATE TABLE cd(v INTEGER)",
    "CREATE TRIGGER cd_down AFTER INSERT ON cd WHEN new.v > 0 BEGIN INSERT INTO cd VALUES(new.v - 1); END",
    "INSERT INTO cd VALUES(4)",
    "PRAGMA recursive_triggers = OFF",
    "CREATE TABLE \"we\"\"ird \"\"name\"(\"col \"\"x\"\"\")",
    "INSERT INTO \"we\"\"ird \"\"name\" VALUES('}]\\\",{\"sql\":\"DROP TABLE e\"}')",
    NULL
};

/* An INSERT whose posted (JSON-escaped) size is exactly `target` bytes:
** an escape-heavy head, then plain padding one byte at a time. */
static char *sized_insert(size_t target) {
    size_t cap = target + 64, at;
    char *sql = malloc(cap);
    at = (size_t)snprintf(sql, cap, "INSERT INTO big VALUES('");
    for (int i = 0; i < 200; i++) { sql[at++] = '"'; sql[at++] = '\\'; sql[at++] = '\n'; }
    while (at + 3 < cap) {
        memcpy(sql + at, "')", 3);                      /* close the statement to measure it */
        size_t sz = hpplite_batch_stmt_json_size(sql, -1, 0);
        if (sz == target) return sql;
        if (sz > target) break;
        sql[at++] = 'a';                                /* plain byte: +1 posted byte */
    }
    free(sql);
    return NULL;
}

static void test_edge_sql_end_to_end(void) {
    TEST("edge-case SQL and full-size batches: posted on the real contract, replica root matches");
    sqlite3 *w = open_db("e_writer.db", KEY2, NULL);
    if (!w) FAIL("open writer");
    HppliteNode *nw = hpplite_get_node(w);
    if (!nw || !nw->l1 || !nw->ctx) FAIL("writer has no L1");
    HppliteSystemConfig *cfg = hpplite_l1_get_system_config(nw->l1);
    if (!cfg) FAIL("system config");
    size_t limit = (size_t)cfg->batchSizeLimit;
    hpplite_l1_system_config_free(cfg);
    size_t stmtLimit = nw->ctx->maxStmtBytes;
    if (limit == 0 || stmtLimit == 0 || stmtLimit >= limit) FAIL("no batch limit learned from the contract");

    for (int i = 0; aAllowed[i]; i++) {
        char *err = NULL;
        if (sqlite3_exec(w, aAllowed[i], NULL, NULL, &err) != SQLITE_OK) {
            printf("[%s: %s] ", aAllowed[i], err ? err : "?"); sqlite3_free(err); sqlite3_close(w); FAIL("an allowed statement was refused");
        }
    }
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); FAIL("corpus batch not posted"); }

    /* one statement at exactly the per-statement limit, alone in its batch:
    ** the largest batch the node will ever post, on the real contract; and
    ** one a byte over it, posted in pieces (#59) */
    sqlite3_exec(w, "CREATE TABLE big(v)", NULL, NULL, NULL);
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); FAIL("flush before the big statement"); }
    uint64_t hBig0 = (uint64_t)scalar(w, "SELECT height FROM hpplite_status");
    char *atLimit = sized_insert(stmtLimit);
    char *overLimit = sized_insert(stmtLimit + 1);
    if (!atLimit || !overLimit) { sqlite3_close(w); FAIL("could not build statements of the target size"); }
    int rcAt = sqlite3_exec(w, atLimit, NULL, NULL, NULL);
    uint64_t hAt = rcAt == SQLITE_OK ? (uint64_t)scalar(w, "SELECT hpplite_flush()") : 0;
    int rcOver = sqlite3_exec(w, overLimit, NULL, NULL, NULL);
    free(atLimit); free(overLimit);
    if (rcAt != SQLITE_OK) { sqlite3_close(w); FAIL("a statement exactly at the limit was refused"); }
    if (rcOver != SQLITE_OK) { sqlite3_close(w); FAIL("a statement one byte over the limit was refused (#59: pieces)"); }
    uint64_t hBig = (uint64_t)scalar(w, "SELECT hpplite_flush()");
    if (hAt == 0 || hBig == 0) { dump_state(nw->l1, nw->instanceId, "big flush"); sqlite3_close(w); FAIL("the contract did not take the large batches (gas or size)"); }
    if (hBig < hAt + 2) { sqlite3_close(w); FAIL("the statement over the limit did not span batches"); }
    size_t bigLen = 0, atLen = 0;
    for (uint64_t h = hBig0; h <= hBig; h++) {
        size_t len = 0;
        uint8_t *raw = hpplite_l1_get_batch(nw->l1, h, &len);
        free(raw);
        if (len > bigLen) bigLen = len;
        if (h == hAt) atLen = len;
    }
    printf("(limit %zu, at-limit batch %zu, largest %zu) ", limit, atLen, bigLen);
    if (bigLen > limit) { sqlite3_close(w); FAIL("posted batch over the contract's limit"); }
    if (bigLen < limit - limit / 20) { sqlite3_close(w); FAIL("no batch near the limit: the test is not exercising it"); }

    /* a backlog of escape-heavy rows that fills several batches */
    uint64_t hBefore = (uint64_t)scalar(w, "SELECT height FROM hpplite_status");
    sqlite3_exec(w, "CREATE TABLE fill(i, v)", NULL, NULL, NULL);
    char row[1200];
    for (int i = 0; i < 300; i++) {
        int at = snprintf(row, sizeof(row), "INSERT INTO fill VALUES(%d, '", i);
        for (int k = 0; k < 250; k++) { row[at++] = (k % 3) ? 'q' : '"'; row[at++] = (k % 5) ? 'z' : '\\'; }
        snprintf(row + at, sizeof(row) - (size_t)at, " %d🚀')", i);
        if (sqlite3_exec(w, row, NULL, NULL, NULL) != SQLITE_OK) { sqlite3_close(w); FAIL("fill insert"); }
    }
    uint64_t hLast = (uint64_t)scalar(w, "SELECT hpplite_flush()");
    /* hBefore is the next height to post, so the backlog is hBefore..hLast */
    if (hLast < hBefore) { sqlite3_close(w); FAIL("backlog not posted"); }
    if (hLast == hBefore) { sqlite3_close(w); FAIL("the backlog fit in one batch: the test is not exercising the cut"); }
    for (uint64_t h = hBefore; h <= hLast; h++) {
        size_t len = 0;
        uint8_t *b = hpplite_l1_get_batch(nw->l1, h, &len);
        free(b);
        if (len > limit) { sqlite3_close(w); FAIL("a backlog batch is over the contract's limit"); }
    }
    if (scalar(w, "SELECT pending FROM hpplite_status") != 0) { sqlite3_close(w); FAIL("pending left after flush"); }

    unsigned char rootW[32], rootR[32];
    hpplite_state_root(w, rootW);
    sqlite3_int64 rowsW = scalar(w, "SELECT (SELECT count(*) FROM e) + (SELECT count(*) FROM big) + (SELECT count(*) FROM fill)");
    if (scalar(w, "SELECT count(*) FROM fc") != 1 || scalar(w, "SELECT count(*) FROM cd") != 5) { sqlite3_close(w); FAIL("test premise: cascade or recursion did not run on the writer"); }
    sqlite3_int64 heightW = scalar(w, "SELECT height FROM hpplite_status");
    sqlite3_close(w);                       /* releases the lease */

    sqlite3 *r = open_db("e_replica.db", KEY2, NULL);
    if (!r) FAIL("open replica");
    if (scalar(r, "SELECT height FROM hpplite_status") != heightW) { sqlite3_close(r); FAIL("replica did not sync to the writer's height"); }
    hpplite_state_root(r, rootR);
    sqlite3_int64 rowsR = scalar(r, "SELECT (SELECT count(*) FROM e) + (SELECT count(*) FROM big) + (SELECT count(*) FROM fill)");
    sqlite3_int64 bigR = scalar(r, "SELECT length(v) FROM big");
    sqlite3_int64 fcR = scalar(r, "SELECT count(*) FROM fc"), cdR = scalar(r, "SELECT count(*) FROM cd");
    sqlite3_close(r);
    if (rowsR != rowsW) FAIL("replica row count differs");
    if (bigR <= 0) FAIL("the at-limit row is missing on the replica");
    if (fcR != 1) FAIL("the cascade did not replay on the replica (#52)");
    if (cdR != 5) FAIL("the recursive trigger did not replay on the replica (#52)");
    if (memcmp(rootW, rootR, 32) != 0) FAIL("replica state root differs from the writer's");
    PASS();
}

/* 7. A transaction several batches long on the real contract (#57). KEY3's
** own rollup. Every posted batch is within the contract's limit; a replica
** rebuilt from the chain has all of it and the writer's root. */
static void test_big_transaction_end_to_end(void) {
    TEST("a transaction larger than a batch spans batches on the real contract; replica root matches");
    sqlite3 *w = open_db("b_writer.db", KEY3, NULL);
    if (!w) FAIL("open writer");
    HppliteNode *nw = hpplite_get_node(w);
    if (!nw || !nw->l1) FAIL("writer has no L1");
    if (sqlite3_exec(w, "CREATE TABLE seed(id INTEGER PRIMARY KEY, v TEXT)", NULL, NULL, NULL) != SQLITE_OK) FAIL("create");
    if (scalar(w, "SELECT hpplite_flush()") <= 0) FAIL("schema batch");
    sqlite3_int64 h0 = scalar(w, "SELECT height FROM hpplite_status");
    /* ~400 KB as posted: three batches or more at the 128 KB limit */
    if (sqlite3_exec(w, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) FAIL("begin");
    for (int i = 0; i < 3000; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO seed VALUES(%d, '%0120d')", i, i);
        if (sqlite3_exec(w, sql, NULL, NULL, NULL) != SQLITE_OK) { sqlite3_close(w); FAIL("a statement of the large transaction was refused"); }
    }
    if (sqlite3_exec(w, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) { sqlite3_close(w); FAIL("commit"); }
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); FAIL("not posted"); }
    sqlite3_int64 h1 = scalar(w, "SELECT height FROM hpplite_status");
    if (h1 - h0 < 3) { sqlite3_close(w); FAIL("the transaction did not span batches"); }
    int nOpen = 0;
    for (sqlite3_int64 h = h0; h < h1; h++) {
        HppliteBatch *b = hpplite_node_fetch_batch(nw, (uint64_t)h);
        if (!b) { sqlite3_close(w); FAIL("fetch"); }
        nOpen += b->openTxn;
        hpplite_batch_free(b);
    }
    if (nOpen < 2) { sqlite3_close(w); FAIL("batches not marked open"); }
    unsigned char rootW[32], rootR[32];
    hpplite_state_root(w, rootW);
    sqlite3_close(w);
    sqlite3 *r = open_db("b_replica.db", KEY3, NULL);
    if (!r) FAIL("open replica");
    sqlite3_int64 rows = scalar(r, "SELECT count(*) FROM seed");
    sqlite3_int64 dropped = scalar(r, "SELECT txns_dropped FROM hpplite_status");
    hpplite_state_root(r, rootR);
    sqlite3_close(r);
    if (rows != 3000) FAIL("replica does not have the whole transaction");
    if (dropped != 0) FAIL("replica dropped a transaction");
    if (memcmp(rootW, rootR, 32) != 0) FAIL("replica state root differs from the writer's");
    PASS();
}

/* 8. The chain stops answering mid-session (#49): anvil is frozen (SIGSTOP),
** so connections hang rather than fail. Writes are local and must stay
** fast; a flush and a close must return in bounded time, keeping the
** batches pending; once the chain answers again everything posts and a
** replica matches. KEY4's own rollup. */
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void anvil_signal(const char *sig) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "kill -%s %s", sig, ANVIL_PID);
    (void)!system(cmd);
}
static void test_l1_outage(void) {
    TEST("L1 frozen mid-session: writes stay fast, flush and close return, all posts after recovery");
    sqlite3 *w = open_db("o_writer.db", KEY4, NULL);
    if (!w) FAIL("open writer");
    sqlite3_exec(w, "CREATE TABLE o(id INTEGER PRIMARY KEY, v TEXT)", NULL, NULL, NULL);
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); FAIL("first batch"); }
    anvil_signal("STOP");
    double slowest = 0, t0, t;
    for (int i = 0; i < 50; i++) {
        char sql[96];
        snprintf(sql, sizeof(sql), "INSERT INTO o VALUES(%d, 'during the outage')", i);
        t0 = now_s();
        int rc = sqlite3_exec(w, sql, NULL, NULL, NULL);
        t = now_s() - t0;
        if (t > slowest) slowest = t;
        if (rc != SQLITE_OK) { anvil_signal("CONT"); sqlite3_close(w); FAIL("a write failed during the outage"); }
    }
    t0 = now_s();
    sqlite3_int64 flushed = scalar(w, "SELECT hpplite_flush()");
    double flushTime = now_s() - t0;
    t0 = now_s();
    sqlite3_close(w);                                      /* close tries to post: must give up in bounded time */
    double closeTime = now_s() - t0;
    printf("(write max %.3fs, flush %.1fs -> %lld, close %.1fs) ", slowest, flushTime, (long long)flushed, closeTime);
    anvil_signal("CONT");
    if (slowest > 0.5) FAIL("a write waited on the chain");
    if (flushTime > 20) FAIL("flush hung on the frozen chain");
    if (closeTime > 20) FAIL("close hung on the frozen chain");
    /* the close could not release the lease: the next process posts once
    ** it expires (a sibling instance of our own wallet is never taken over) */
    chain_sleep(400);
    w = open_db("o_writer.db", KEY4, NULL);                /* the rows waited in the log */
    if (!w) FAIL("reopen writer");
    sqlite3_int64 pend = scalar(w, "SELECT pending FROM hpplite_status");
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); FAIL("nothing posted after recovery"); }
    unsigned char rootW[32], rootR[32];
    hpplite_state_root(w, rootW);
    sqlite3_int64 left = scalar(w, "SELECT pending FROM hpplite_status");
    sqlite3_close(w);
    if (pend != 50) { printf("(pending %lld) ", (long long)pend); FAIL("the outage's writes were not kept pending"); }
    if (left != 0) FAIL("pending left after recovery");
    sqlite3 *r = open_db("o_replica.db", KEY4, NULL);
    if (!r) FAIL("replica");
    sqlite3_int64 rows = scalar(r, "SELECT count(*) FROM o");
    hpplite_state_root(r, rootR);
    sqlite3_close(r);
    if (rows != 50) FAIL("replica rows");
    if (memcmp(rootW, rootR, 32) != 0) FAIL("replica root differs");
    PASS();
}

/* 9. A chain reorganisation drops two batches the writer finalized (#49):
** evm_snapshot before them, evm_revert after. The writer must post them
** again and continue; a replica rebuilt from the chain matches. KEY5. */
static void cast_rpc(const char *method, const char *arg, char *out, size_t n) {
    char cmd[256];
    FILE *f;
    snprintf(cmd, sizeof(cmd), "cast rpc %s %s --rpc-url %s 2>/dev/null", method, arg ? arg : "", RPC);
    out[0] = 0;
    if ((f = popen(cmd, "r")) != NULL) {
        if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = 0;
        pclose(f);
    }
}
static void test_reorg(void) {
    TEST("a reorg drops two finalized batches: the writer posts them again and continues; replica matches");
    sqlite3 *w = open_db("g_writer.db", KEY5, NULL);
    if (!w) FAIL("open writer");
    sqlite3_exec(w, "CREATE TABLE g(id INTEGER PRIMARY KEY, v TEXT)", NULL, NULL, NULL);
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); FAIL("batch 1"); }
    sqlite3_exec(w, "INSERT INTO g VALUES(1, 'before')", NULL, NULL, NULL);
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); FAIL("batch 2"); }
    char snap[80];
    cast_rpc("evm_snapshot", NULL, snap, sizeof(snap));
    if (!snap[0]) { sqlite3_close(w); FAIL("evm_snapshot"); }
    sqlite3_exec(w, "INSERT INTO g VALUES(2, 'lost in the reorg')", NULL, NULL, NULL);
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); FAIL("batch 3"); }
    sqlite3_exec(w, "INSERT INTO g VALUES(3, 'also lost')", NULL, NULL, NULL);
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); FAIL("batch 4"); }
    char rev[80];
    cast_rpc("evm_revert", snap, rev, sizeof(rev));
    if (strcmp(rev, "true") != 0) { sqlite3_close(w); FAIL("evm_revert"); }
    sqlite3_exec(w, "INSERT INTO g VALUES(4, 'after')", NULL, NULL, NULL);
    for (int k = 0; k < 4 && scalar(w, "SELECT pending FROM hpplite_status") > 0; k++) scalar(w, "SELECT hpplite_flush()");
    sqlite3_int64 left = scalar(w, "SELECT pending FROM hpplite_status");
    unsigned char rootW[32], rootR[32];
    hpplite_state_root(w, rootW);
    sqlite3_close(w);
    if (left != 0) FAIL("the writer did not get past the reorg");
    sqlite3 *r = open_db("g_replica.db", KEY5, NULL);
    if (!r) FAIL("replica");
    sqlite3_int64 rows = scalar(r, "SELECT count(*) FROM g");
    hpplite_state_root(r, rootR);
    sqlite3_close(r);
    if (rows != 4) { printf("(replica rows %lld) ", (long long)rows); FAIL("the lost batches are not on the chain"); }
    if (memcmp(rootW, rootR, 32) != 0) FAIL("replica root differs");
    PASS();
}

/* 10. One wallet, several databases (#61): each named database gets its own
** rollup beside the wallet's first (which already has history here), and a
** replica that only knows the name finds it; {file} names a database by its
** file, so one setting serves all of an application's */
static void test_named_databases(void) {
    TEST("one key, named databases: a rollup each beside the wallet's first; replicas find theirs by name");
    char ca[64], cb[64], cc[64], cu[64], ra[64], rb[64], rc_[64];
    unsigned char rootA[32], rootB[32], rootC[32], r1[32], r2[32], r3[32];
    sqlite3 *a = open_db("named_a.db", KEY1, "&dbname=alpha");
    sqlite3 *b = open_db("named_b.db", KEY1, "&dbname=beta");
    sqlite3 *c = open_db("orders.db", KEY1, "&dbname={file}");
    sqlite3 *u = open_db("named_u.db", KEY1, NULL);
    if (!a || !b || !c || !u) FAIL("open");
    sqlite3_exec(a, "CREATE TABLE t(v); INSERT INTO t VALUES('alpha'), (random())", NULL, NULL, NULL);
    sqlite3_exec(b, "CREATE TABLE t(v); INSERT INTO t VALUES('beta')", NULL, NULL, NULL);
    sqlite3_exec(c, "CREATE TABLE o(id INTEGER PRIMARY KEY, n); INSERT INTO o(n) VALUES(1), (2), (3)", NULL, NULL, NULL);
    scalar(a, "SELECT hpplite_flush()"); scalar(b, "SELECT hpplite_flush()"); scalar(c, "SELECT hpplite_flush()");
    text_of(a, "SELECT contract FROM hpplite_status", ca, sizeof(ca));
    text_of(b, "SELECT contract FROM hpplite_status", cb, sizeof(cb));
    text_of(c, "SELECT contract FROM hpplite_status", cc, sizeof(cc));
    text_of(u, "SELECT contract FROM hpplite_status", cu, sizeof(cu));
    hpplite_state_root(a, rootA); hpplite_state_root(b, rootB); hpplite_state_root(c, rootC);
    sqlite3_close(a); sqlite3_close(b); sqlite3_close(c); sqlite3_close(u);
    if (!ca[0] || !cb[0] || !cc[0] || !cu[0]) FAIL("a database without a contract");
    if (!strcmp(ca, cb) || !strcmp(ca, cc) || !strcmp(cb, cc)) { printf("(%s %s %s) ", ca, cb, cc); FAIL("named databases share a rollup"); }
    if (!strcmp(ca, cu) || !strcmp(cb, cu) || !strcmp(cc, cu)) { printf("(a %s b %s c %s u %s) ", ca, cb, cc, cu); FAIL("a named database took the wallet's first rollup"); }
    /* replicas: new files that know only the key and the name */
    a = open_db("named_a_replica.db", KEY1, "&dbname=alpha");
    b = open_db("named_b_replica.db", KEY1, "&dbname=beta");
    c = open_db("orders_replica.db", KEY1, "&dbname=orders.db");
    if (!a || !b || !c) FAIL("open replicas");
    text_of(a, "SELECT contract FROM hpplite_status", ra, sizeof(ra));
    text_of(b, "SELECT contract FROM hpplite_status", rb, sizeof(rb));
    text_of(c, "SELECT contract FROM hpplite_status", rc_, sizeof(rc_));
    hpplite_state_root(a, r1); hpplite_state_root(b, r2); hpplite_state_root(c, r3);
    sqlite3_int64 na = scalar(a, "SELECT count(*) FROM t"), nc = scalar(c, "SELECT count(*) FROM o");
    sqlite3_close(a); sqlite3_close(b); sqlite3_close(c);
    if (strcmp(ra, ca) || strcmp(rb, cb) || strcmp(rc_, cc)) FAIL("a replica found another rollup than its writer's");
    if (na != 2 || nc != 3) FAIL("replica rows");
    if (memcmp(r1, rootA, 32) || memcmp(r2, rootB, 32) || memcmp(r3, rootC, 32)) FAIL("replica root differs");
    PASS();
}

int main(void) {
    RPC = getenv("HPPLITE_ANVIL_RPC"); FACTORY = getenv("HPPLITE_ANVIL_FACTORY");
    KEY0 = getenv("HPPLITE_ANVIL_KEY0"); KEY1 = getenv("HPPLITE_ANVIL_KEY1"); KEY2 = getenv("HPPLITE_ANVIL_KEY2");
    KEY3 = getenv("HPPLITE_ANVIL_KEY3"); KEY4 = getenv("HPPLITE_ANVIL_KEY4"); ANVIL_PID = getenv("HPPLITE_ANVIL_PID");
    KEY5 = getenv("HPPLITE_ANVIL_KEY5");
    if (!RPC || !FACTORY || !KEY0 || !KEY1 || !KEY2 || !KEY3 || !KEY4 || !KEY5 || !ANVIL_PID) { printf("run through tests/l1/anvil_test.sh\n"); return 77; }
    test_suite_init("anvil", NULL, DIR);
    reset_dir();
    printf("=== HPPLite on anvil (%s) ===\n", RPC);
    test_open_write_post_checkpoint();
    test_verify_tool();
    test_second_instance_same_wallet();
    test_concurrent_rpc();
    test_sleep_and_wake();
    test_stranger_after_expiry();
    test_edge_sql_end_to_end();
    test_big_transaction_end_to_end();
    test_l1_outage();
    test_reorg();
    test_named_databases();
    remove_dir();
    return test_report();
}
