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
**
** The singleton challenged (#49, scenarios 12-15): the database resumed on
** another machine while the old instance is still alive; two processes on
** one file killed and restarted in both orders; a misbehaving RPC endpoint
** (tests/l1/rpc_proxy.py in front of anvil); a power loss mid-transaction,
** the file and its -wal reopened as they lie. HPPLITE_ANVIL_TESTS=12,15
** runs a selection. 17 is the DA v2 chain hash (#33). 16 is the soak (#120): hours of a demo-like workload
** with a rebuild and hpplite-verify every N batches, run only when
** HPPLITE_SOAK_SECONDS is set (the nightly soak workflow).
*/
#include "test_lib.h"
#include "envelope_pk.h"
#include "l1_interface.h"
#include "eth/eth_client.h"
#include "batch.h"
#include "pages.h"
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include "store.h"
#include "config.h"
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#define DIR "/tmp/hpplite_anvil"

static const char *FACIL, *FACIL_ADDR;     /* the mock facilitator anvil_test.sh starts (#183) and its address */
static const char *RPC, *FACTORY, *KEY0, *KEY1, *KEY2, *KEY3, *KEY4, *KEY5, *KEY6, *KEY7, *KEY8, *KEY9, *ANVIL_PID;

static void chain_sleep(int seconds) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "cast rpc evm_increaseTime %d --rpc-url %s >/dev/null && cast rpc evm_mine --rpc-url %s >/dev/null", seconds, RPC, RPC);
    (void)!system(cmd);
}

/* the node on `file` with `key`, through the endpoint `rpc` (anvil's, or a
** proxy's). No timer unless `extra` names an interval: a URI parameter's
** first occurrence counts, so the caller's goes first. */
static sqlite3 *open_db_at(const char *rpc, const char *file, const char *key, const char *extra) {
    char uri[1024];
    sqlite3 *db = NULL;
    snprintf(uri, sizeof(uri), "file:" DIR "/%s?hpplite=on&rpc=%s&factory=%s&chainid=31337&privkey=%s&datadir=" DIR "%s&interval=0",
             file, rpc, FACTORY, key, extra ? extra : "");
    if (sqlite3_open_v2(uri, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL) != SQLITE_OK) {
        fprintf(stderr, "open: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    return db;
}
static sqlite3 *open_db(const char *file, const char *key, const char *extra) { return open_db_at(RPC, file, key, extra); }

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
/* hpplite-verify with the given connection parameters (what a user would put in HPPLITE_PARAMS) */
static int run_verify_with(const char *params, const char *extra, char *out, size_t n) {
    const char *bin = getenv("HPPLITE_VERIFY_BIN");
    char cmd[1600];
    FILE *p;
    size_t got = 0;
    int status;
    if (!bin) return -2;
    snprintf(cmd, sizeof cmd, "HPPLITE_PARAMS='%s' '%s' %s 2>&1", params, bin, extra ? extra : "");
    p = popen(cmd, "r");
    if (!p) return -2;
    while (got + 1 < n && fgets(out + got, (int)(n - got), p)) got = strlen(out);
    status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
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
    sqlite3_exec(a, "CREATE TABLE IF NOT EXISTS t(x); INSERT INTO t VALUES(4)", NULL, NULL, NULL);   /* t is scenario 1's; alone, it is made here */
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
    /* ~400 KB of pages that do not deflate (random text, #146): three batches or more at the 128 KB limit */
    if (sqlite3_exec(w, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) FAIL("begin");
    for (int i = 0; i < 3000; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO seed VALUES(%d, hex(randomblob(60)))", i);
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
        { int more = 0; hpplite_pagebatch_meta(b->pPagesRaw, b->nPagesRaw, NULL, &more); nOpen += more; }   /* a commit cut into pieces: every piece but the last says more follows (#152) */
        hpplite_batch_free(b);
    }
    if (nOpen < 2) { sqlite3_close(w); FAIL("the pieces of the commit are not marked as having more to come"); }
    unsigned char rootW[32], rootR[32];
    hpplite_state_root(w, rootW);
    sqlite3_close(w);
    sqlite3 *r = open_db("b_replica.db", KEY3, NULL);
    if (!r) FAIL("open replica");
    sqlite3_int64 rows = scalar(r, "SELECT count(*) FROM seed");
    hpplite_state_root(r, rootR);
    sqlite3_close(r);
    if (rows != 3000) FAIL("replica does not have the whole transaction");
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
    if (pend < 1) { printf("(pending %lld) ", (long long)pend); FAIL("the outage's writes were not kept pending"); }   /* page capture: the file as reopened, one pending image holding them (#146) */
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

/* ---- the singleton challenged (#49): what the chain holds against the file ---- */

/* The chain's batches 1..last are all there and all ours, and the file's
** height is right behind them. NULL, or why. */
static const char *chain_contiguous(HppliteNode *n) {
    static char why[200];
    uint64_t last = 0, total = 0;
    if (hpplite_l1_get_da_state(n->l1, &last, &total, NULL) != 0) return "cannot read the chain's state";
    if (last + 1 != n->ctx->blockHeight) {
        snprintf(why, sizeof why, "the chain holds %llu batches, the file finalized %llu", (unsigned long long)last, (unsigned long long)(n->ctx->blockHeight - 1));
        return why;
    }
    for (uint64_t h = 1; h <= last; h++) {
        HppliteBatch *b = hpplite_node_fetch_batch(n, h);
        if (!b) { snprintf(why, sizeof why, "batch %llu of %llu is missing or not ours", (unsigned long long)h, (unsigned long long)last); return why; }
        hpplite_batch_free(b);
    }
    return NULL;
}

/* A fresh file with the same key rebuilt from the chain alone has every row
** of `w` and its root. NULL, or why. */
static const char *replica_equals_as(sqlite3 *w, const char *replicaFile, const char *key, const char *extra) {
    static char why[400];
    unsigned char rootW[32], rootR[32];
    char *a = test_dump(w, 0), *b;
    hpplite_state_root(w, rootW);
    sqlite3 *r = open_db(replicaFile, key, extra);
    if (!r) { sqlite3_free(a); return "replica open"; }
    b = test_dump(r, 0);
    hpplite_state_root(r, rootR);
    sqlite3_close(r);
    int rows = strcmp(a, b) != 0, root = memcmp(rootW, rootR, 32) != 0;
    if (rows) {
        size_t i = test_dump_diff(a, b);
        snprintf(why, sizeof why, "replica rows differ: writer [%.60s] replica [%.60s]", a + i, b + i);
    } else if (root) {
        snprintf(why, sizeof why, "replica rows equal but its state root differs");
    }
    sqlite3_free(a); sqlite3_free(b);
    return rows || root ? why : NULL;
}
static const char *replica_equals(sqlite3 *w, const char *replicaFile, const char *key) { return replica_equals_as(w, replicaFile, key, NULL); }

/* 12. The database moves to another machine (#49): an empty file with the
** same key rebuilds from the chain and sequences once the old lease has run
** out. The old machine's process is still alive - it fell asleep with a row
** it never posted - and wakes later: it must not post over the new history,
** and must say so. A sibling that slept with nothing pending follows the new
** history and waits for the lease. KEY6's own rollup. */
static void test_resume_elsewhere(void) {
    TEST("resume on another machine: the new file rebuilds and sequences; the stale instance wakes and refuses, never posts over the new history");
    sqlite3 *a = open_db("mig_a.db", KEY6, NULL);
    if (!a) FAIL("open a");
    sqlite3_exec(a, "CREATE TABLE m(id INTEGER PRIMARY KEY, who TEXT)", NULL, NULL, NULL);
    sqlite3_exec(a, "INSERT INTO m VALUES(1, 'old machine')", NULL, NULL, NULL);
    if (scalar(a, "SELECT hpplite_flush()") <= 0) { sqlite3_close(a); FAIL("a posts"); }
    sqlite3_int64 hA = scalar(a, "SELECT height FROM hpplite_status");
    /* a sibling of the same wallet on another directory, idle (a second deployment) */
    sqlite3 *c = open_db("mig_c.db", KEY6, NULL);
    if (!c) FAIL("open c");
    if (scalar(c, "SELECT count(*) FROM m") != 1) FAIL("c did not rebuild from the chain");
    /* the old machine writes once more, then sleeps before its timer posts it */
    sqlite3_exec(a, "INSERT INTO m VALUES(2, 'written on the old machine, never posted')", NULL, NULL, NULL);
    chain_sleep(3700);                                      /* its lease runs out while it sleeps (an hour on VERSION 9, #164) */

    /* the new machine: an empty file, the same key */
    sqlite3 *b = open_db("mig_b.db", KEY6, NULL);
    if (!b) FAIL("open b");
    HppliteNode *nb = hpplite_get_node(b);
    if (scalar(b, "SELECT height FROM hpplite_status") != hA || scalar(b, "SELECT count(*) FROM m") != 1) FAIL("b did not rebuild from the chain");
    if (scalar(b, "SELECT lease FROM hpplite_status") != 1) { dump_state(nb->l1, nb->instanceId, "b's open"); FAIL("b should hold the lease now"); }
    sqlite3_exec(b, "INSERT INTO m VALUES(2, 'new machine')", NULL, NULL, NULL);   /* the same rowid as the row a never posted: the histories part here */
    if (scalar(b, "SELECT hpplite_flush()") <= 0) FAIL("b posts");
    uint64_t chainB = 0, chainAfterA = 0, total = 0;
    hpplite_l1_get_da_state(nb->l1, &chainB, &total, NULL);

    /* the old machine wakes: its process continues, with its unposted row and a new one */
    sqlite3_exec(a, "INSERT INTO m VALUES(3, 'old machine, after waking')", NULL, NULL, NULL);
    sqlite3_int64 postedA = scalar(a, "SELECT hpplite_flush()");
    char stateA[64], errA[512];
    text_of(a, "SELECT state FROM hpplite_status", stateA, sizeof stateA);
    text_of(a, "SELECT last_error FROM hpplite_status", errA, sizeof errA);
    sqlite3_int64 pendA = scalar(a, "SELECT pending FROM hpplite_status");
    sqlite3_close(a);                                       /* tries to post again, and to release "its" lease */
    hpplite_l1_get_da_state(nb->l1, &chainAfterA, &total, NULL);
    int leaseB = lease_is(nb->l1, nb->instanceId);
    printf("(stale: flush %lld, state %s, pending %lld, last_error \"%s\") ", (long long)postedA, stateA, (long long)pendA, errA);
    if (postedA != 0 || chainAfterA != chainB) FAIL("the stale instance posted over the new history");
    if (strcmp(stateA, "error") != 0 || !errA[0]) FAIL("the stale instance did not refuse loudly");
    if (pendA != 2) FAIL("the stale instance's rows must stay in its own file");
    if (leaseB != 1) { dump_state(nb->l1, nb->instanceId, "after a's close"); FAIL("a's close cut b's lease"); }

    /* the idle sibling wakes with nothing pending: it follows the new history and waits */
    if (scalar(c, "SELECT hpplite_flush()") != 0) FAIL("c posted with nothing pending");
    if (scalar(c, "SELECT count(*) FROM m") != 2 || scalar(c, "SELECT height FROM hpplite_status") != scalar(b, "SELECT height FROM hpplite_status")) FAIL("c did not follow the new history");
    sqlite3_exec(c, "INSERT INTO m VALUES(3, 'sibling')", NULL, NULL, NULL);
    sqlite3_int64 postedC = scalar(c, "SELECT hpplite_flush()");
    char stateC[64];
    text_of(c, "SELECT state FROM hpplite_status", stateC, sizeof stateC);
    sqlite3_int64 leaseC = scalar(c, "SELECT lease FROM hpplite_status"), pendC = scalar(c, "SELECT pending FROM hpplite_status");
    sqlite3_close(c);
    hpplite_l1_get_da_state(nb->l1, &chainAfterA, &total, NULL);
    printf("(sibling: flush %lld, lease %lld, pending %lld, state %s) ", (long long)postedC, (long long)leaseC, (long long)pendC, stateC);
    if (postedC != 0 || chainAfterA != chainB || leaseC != 0) FAIL("the sibling posted into b's lease");
    if (lease_is(nb->l1, nb->instanceId) != 1) FAIL("c's close cut b's lease");
    if (pendC != 1) FAIL("the sibling's row must stay in its own file");

    /* the new machine goes on; a replica rebuilt from the chain equals it */
    sqlite3_exec(b, "INSERT INTO m VALUES(3, 'new machine again')", NULL, NULL, NULL);
    if (scalar(b, "SELECT hpplite_flush()") <= 0) FAIL("b keeps posting");
    const char *why = chain_contiguous(nb);
    if (why) { sqlite3_close(b); FAIL(why); }
    sqlite3_int64 rowsB = scalar(b, "SELECT count(*) FROM m");
    sqlite3_close(b);
    sqlite3 *b2 = open_db("mig_b.db", KEY6, NULL);          /* b's file, reopened: the current sequencer */
    if (!b2) FAIL("reopen b");
    if (scalar(b2, "SELECT count(*) FROM m") != rowsB || scalar(b2, "SELECT pending FROM hpplite_status") != 0) FAIL("b's reopen");
    why = replica_equals(b2, "mig_r.db", KEY6);
    sqlite3_close(b2);
    if (why) FAIL(why);
    /* the sibling's status must say what it is doing: waiting for the lease */
    if (strcmp(stateC, "waiting") != 0) FAILF("the sibling's state is '%s', want 'waiting'", stateC);
    PASS();
}

/* 13. Two processes of the application on one file (#49): the first holds
** the file lock and sequences, the second captures into the shared log.
** Each is killed (-9) and started again, the holder first, then the
** follower, with writes before and after every kill. After each restart
** exactly one of them sequences; at the end every write is on the chain
** once and a replica equals the file. KEY7's own rollup. */
typedef struct ProcStatus { long seq, height, pending, busy; int sequencer, lease; char state[32]; } ProcStatus;

static void proc_status_path(int proc, char *out, size_t n) { snprintf(out, n, DIR "/status.%d", proc); }
static void proc_stop_path(int proc, char *out, size_t n) { snprintf(out, n, DIR "/stop.%d", proc); }

static void write_proc_status(int proc, long seq, long busy, sqlite3 *db) {
    char tmp[300], fin[300];
    ProcStatus st;
    proc_status_path(proc, fin, sizeof fin);
    snprintf(tmp, sizeof tmp, "%s.tmp", fin);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    text_of(db, "SELECT state FROM hpplite_status", st.state, sizeof st.state);
    fprintf(f, "%ld %lld %lld %ld %lld %lld %s\n", seq, (long long)scalar(db, "SELECT height FROM hpplite_status"),
            (long long)scalar(db, "SELECT pending FROM hpplite_status"), busy,
            (long long)scalar(db, "SELECT sequencer FROM hpplite_status"), (long long)scalar(db, "SELECT lease FROM hpplite_status"), st.state);
    fclose(f);
    rename(tmp, fin);
}

static int read_proc_status(int proc, ProcStatus *st) {
    char path[300];
    proc_status_path(proc, path, sizeof path);
    FILE *f = fopen(path, "r");
    int ok;
    if (!f) return 0;
    memset(st, 0, sizeof *st);
    ok = fscanf(f, "%ld %ld %ld %ld %d %d %31s", &st->seq, &st->height, &st->pending, &st->busy, &st->sequencer, &st->lease, st->state) == 7;
    fclose(f);
    return ok;
}

/* The application's process: one row every 20 ms until its stop file
** appears, then a clean close. The parent reads its status file. */
static pid_t spawn_writer(int proc, const char *file, const char *key) {
    char path[300];
    proc_stop_path(proc, path, sizeof path); unlink(path);
    proc_status_path(proc, path, sizeof path); unlink(path);
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    if (pid != 0) return pid;
    sqlite3 *db = open_db(file, key, "&interval=200ms");
    if (!db) _exit(2);
    sqlite3_busy_timeout(db, 10000);
    long busy = 0, seq;
    char stop[300];
    proc_stop_path(proc, stop, sizeof stop);
    for (seq = 1; ; seq++) {
        char sql[96];
        snprintf(sql, sizeof sql, "INSERT INTO w VALUES(%d, %ld)", proc, seq);
        if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) { busy++; seq--; }
        if (seq % 10 == 0) write_proc_status(proc, seq, busy, db);
        if (access(stop, F_OK) == 0) break;
        usleep(20000);
    }
    write_proc_status(proc, seq, busy, db);
    sqlite3_close(db);
    _exit(0);
}

/* until pred(status) holds, or `secs` pass: 1 if it did */
static int wait_proc(int proc, int (*pred)(const ProcStatus *, long), long arg, double secs) {
    double t0 = now_s();
    ProcStatus st;
    do {
        if (read_proc_status(proc, &st) && pred(&st, arg)) return 1;
        usleep(100000);
    } while (now_s() - t0 < secs);
    return 0;
}
static int seq_past(const ProcStatus *st, long n) { return st->seq >= n; }
static int height_past(const ProcStatus *st, long n) { return st->height >= n; }
static int sequences(const ProcStatus *st, long want) { return st->sequencer == want; }
static int seq_past_as_sequencer(const ProcStatus *st, long n) { return st->seq >= n && st->sequencer == 1; }
static int seq_past_as_follower(const ProcStatus *st, long n) { return st->seq >= n && st->sequencer == 0; }

static long proc_seq(int proc) { ProcStatus st; return read_proc_status(proc, &st) ? st.seq : 0; }
static long proc_height(int proc) { ProcStatus st; return read_proc_status(proc, &st) ? st.height : 0; }
/* "proc N: seq S height H pending P sequencer Q lease L state Z", for a failure */
static const char *proc_line(int proc) {
    static char line[2][160];
    static int which;
    char *out = line[which ^= 1];
    ProcStatus st;
    if (!read_proc_status(proc, &st)) snprintf(out, sizeof line[0], "proc %d: no status", proc);
    else snprintf(out, sizeof line[0], "proc %d: seq %ld height %ld pending %ld sequencer %d lease %d state %s", proc, st.seq, st.height, st.pending, st.sequencer, st.lease, st.state);
    return out;
}

/* a clean stop: the stop file, then the exit within `secs` (else killed) */
static int stop_writer(int proc, pid_t pid, double secs) {
    char path[300];
    int status = 0;
    double t0 = now_s();
    proc_stop_path(proc, path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f) fclose(f);
    while (waitpid(pid, &status, WNOHANG) == 0) {
        if (now_s() - t0 > secs) { kill(pid, SIGKILL); waitpid(pid, &status, 0); return -1; }
        usleep(100000);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void test_two_processes_one_file(void) {
    TEST("two processes on one file: killed and restarted, holder first then follower; one sequencer after each restart, every write on chain once");
    sqlite3 *w = open_db("two.db", KEY7, NULL);
    if (!w) FAIL("open");
    sqlite3_exec(w, "CREATE TABLE w(proc INTEGER, seq INTEGER)", NULL, NULL, NULL);
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); FAIL("schema batch"); }
    sqlite3_close(w);

    pid_t pid[5] = {0};
    pid[1] = spawn_writer(1, "two.db", KEY7);
    pid[2] = spawn_writer(2, "two.db", KEY7);
    if (!wait_proc(1, seq_past, 30, 15) || !wait_proc(2, seq_past, 30, 15)) { kill(pid[1], SIGKILL); kill(pid[2], SIGKILL); FAIL("the processes did not write"); }
    ProcStatus s1, s2;
    read_proc_status(1, &s1); read_proc_status(2, &s2);
    if (s1.sequencer + s2.sequencer != 1) { kill(pid[1], SIGKILL); kill(pid[2], SIGKILL); FAILF("%d sequencers on one file (1: %s, 2: %s)", s1.sequencer + s2.sequencer, s1.state, s2.state); }
    int holder = s1.sequencer ? 1 : 2, follower = 3 - holder;
    long hBefore = proc_height(holder);
    if (!wait_proc(holder, seq_past_as_sequencer, 60, 20) || !wait_proc(holder, height_past, hBefore + 1, 20)) { kill(pid[1], SIGKILL); kill(pid[2], SIGKILL); FAILF("the holder does not post (%s; %s)", proc_line(holder), proc_line(follower)); }

    /* the holder dies mid-flight: the follower takes over and keeps posting; the holder comes back as a follower */
    kill(pid[holder], SIGKILL); waitpid(pid[holder], NULL, 0);
    long seqAtKill = proc_seq(follower);
    if (!wait_proc(follower, seq_past_as_sequencer, seqAtKill + 40, 30)) { kill(pid[follower], SIGKILL); FAILF("after the holder's death, process %d did not take over and keep writing (%s)", follower, proc_line(follower)); }
    hBefore = proc_height(follower);
    pid[3] = spawn_writer(3, "two.db", KEY7);
    if (!wait_proc(3, seq_past_as_follower, 30, 20)) { kill(pid[follower], SIGKILL); kill(pid[3], SIGKILL); FAILF("the restarted holder did not come back as a follower (%s; %s)", proc_line(3), proc_line(follower)); }
    if (!wait_proc(follower, sequences, 1, 5) || !wait_proc(follower, height_past, hBefore + 1, 20)) { kill(pid[follower], SIGKILL); kill(pid[3], SIGKILL); FAILF("process %d stopped sequencing when 3 restarted (%s; %s)", follower, proc_line(follower), proc_line(3)); }

    /* the follower dies: the holder goes on; the follower comes back */
    kill(pid[3], SIGKILL); waitpid(pid[3], NULL, 0);
    seqAtKill = proc_seq(follower);
    if (!wait_proc(follower, seq_past_as_sequencer, seqAtKill + 40, 30)) { kill(pid[follower], SIGKILL); FAILF("the holder stopped when its follower died (%s)", proc_line(follower)); }
    hBefore = proc_height(follower);
    pid[4] = spawn_writer(4, "two.db", KEY7);
    if (!wait_proc(4, seq_past_as_follower, 30, 20)) { kill(pid[follower], SIGKILL); kill(pid[4], SIGKILL); FAILF("the restarted follower did not come back as a follower (%s; %s)", proc_line(4), proc_line(follower)); }
    if (!wait_proc(follower, sequences, 1, 5) || !wait_proc(follower, height_past, hBefore + 1, 20)) { kill(pid[follower], SIGKILL); kill(pid[4], SIGKILL); FAILF("process %d stopped sequencing when 4 restarted (%s; %s)", follower, proc_line(follower), proc_line(4)); }

    /* both stop cleanly, the holder first (the follower takes over for its last rows) */
    int rcH = stop_writer(follower, pid[follower], 60);
    int rc4 = stop_writer(4, pid[4], 60);
    ProcStatus sH, s4;
    read_proc_status(follower, &sH); read_proc_status(4, &s4);
    printf("(holder %d and follower 3 killed; busy retries %ld+%ld) ", holder, sH.busy, s4.busy);
    if (rcH != 0 || rc4 != 0) FAILF("the processes did not close cleanly (%d, %d)", rcH, rc4);

    w = open_db("two.db", KEY7, NULL);
    if (!w) FAIL("reopen");
    HppliteNode *nw = hpplite_get_node(w);
    sqlite3_int64 pend = scalar(w, "SELECT pending FROM hpplite_status");
    sqlite3_int64 n1 = scalar(w, "SELECT count(*) FROM w WHERE proc = 1"), n2 = scalar(w, "SELECT count(*) FROM w WHERE proc = 2");
    sqlite3_int64 n3 = scalar(w, "SELECT count(*) FROM w WHERE proc = 3"), n4 = scalar(w, "SELECT count(*) FROM w WHERE proc = 4");
    sqlite3_int64 dup = scalar(w, "SELECT count(*) FROM (SELECT proc, seq FROM w GROUP BY proc, seq HAVING count(*) > 1)");
    printf("(rows %lld/%lld/%lld/%lld, height %lld) ", (long long)n1, (long long)n2, (long long)n3, (long long)n4, (long long)scalar(w, "SELECT height FROM hpplite_status"));
    if (pend != 0) { sqlite3_close(w); FAILF("%lld rows left pending: the last process out did not post them", (long long)pend); }
    if (n1 < 30 || n2 < 30 || n3 < 30 || n4 < 30) { sqlite3_close(w); FAIL("a process's writes are missing"); }
    if (dup != 0) { sqlite3_close(w); FAIL("a row was written twice"); }
    const char *why = chain_contiguous(nw);
    if (!why) why = replica_equals(w, "two_r.db", KEY7);
    sqlite3_close(w);
    if (why) FAIL(why);
    PASS();
}

/* 14. The RPC endpoint misbehaves (#49): a proxy (tests/l1/rpc_proxy.py) in
** front of anvil adds latency, loses answers, holds them past the timeout
** and refuses with 503, a phase each and then all at once. The application
** writes throughout, fast; once the endpoint is healthy every write is on
** the chain exactly once (contiguous heights, each row in one batch) and a
** replica equals the file. KEY8's own rollup. */
static pid_t g_proxyPid = 0;
static void kill_proxy(void) {
    if (g_proxyPid > 0) { kill(g_proxyPid, SIGTERM); waitpid(g_proxyPid, NULL, 0); g_proxyPid = 0; }
}
static int start_proxy(int port, const char *modeFile) {
    const char *script = getenv("HPPLITE_RPC_PROXY");
    char portStr[16], upstream[128];
    const char *host = strstr(RPC, "://");
    if (!script || access(script, R_OK) != 0) return -1;
    snprintf(portStr, sizeof portStr, "%d", port);
    snprintf(upstream, sizeof upstream, "%s", host ? host + 3 : RPC);
    fflush(stdout); fflush(stderr);
    g_proxyPid = fork();
    if (g_proxyPid == 0) {
        execlp("python3", "python3", "-I", script, portStr, upstream, modeFile, (char *)NULL);
        _exit(127);
    }
    if (g_proxyPid < 0) return -1;
    atexit(kill_proxy);
    /* up when it answers a request (a bad python3 exits first) */
    for (int i = 0; i < 50; i++) {
        char cmd[200];
        int status;
        if (waitpid(g_proxyPid, &status, WNOHANG) == g_proxyPid) { g_proxyPid = 0; return -1; }
        snprintf(cmd, sizeof cmd, "cast chain-id --rpc-url http://127.0.0.1:%d >/dev/null 2>&1", port);
        if (system(cmd) == 0) return 0;
        usleep(200000);
    }
    kill_proxy();
    return -1;
}
static void set_proxy_mode(const char *modeFile, const char *mode) {
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", modeFile);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fputs(mode, f);
    fclose(f);
    rename(tmp, modeFile);
}
static long proxy_stat(const char *modeFile, const char *key) {
    char path[300], line[80];
    long v = -1;
    snprintf(path, sizeof path, "%s.stats", modeFile);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, strlen(key)) == 0 && line[strlen(key)] == '=') v = atol(line + strlen(key) + 1);
    }
    fclose(f);
    return v;
}
static void test_rpc_faults(void) {
    TEST("RPC faults through a proxy: latency, lost answers, timeouts, 503s; writes stay fast, all on chain once when healthy again");
    static const char *modeFile = DIR "/proxy.mode";
    static const struct { const char *name, *mode; } phase[] = {
        { "latency",  "latency_ms=400\n" },
        { "dropped",  "drop=0.3\n" },
        { "timeouts", "timeout=0.15\nhold_ms=4000\n" },
        { "503s",     "error=0.4\n" },
        { "storm",    "latency_ms=100\ndrop=0.2\ntimeout=0.1\nhold_ms=4000\nerror=0.2\n" },
    };
    int port = test_free_port();   /* never one another program may hold (#177) */
    char proxyRpc[64];
    snprintf(proxyRpc, sizeof proxyRpc, "http://127.0.0.1:%d", port);
    set_proxy_mode(modeFile, "");
    if (start_proxy(port, modeFile) != 0) { printf("SKIP (python3 or tests/l1/rpc_proxy.py not available) "); PASS(); return; }
    setenv("HPPLITE_RPC_TIMEOUT", "2", 1);                   /* read per request: a held answer costs 2 s, not 15 */
    sqlite3 *w = open_db_at(proxyRpc, "rpc_writer.db", KEY8, "&interval=300ms");
    if (!w) { kill_proxy(); unsetenv("HPPLITE_RPC_TIMEOUT"); FAIL("open through the proxy"); }
    HppliteNode *nw = hpplite_get_node(w);
    sqlite3_exec(w, "CREATE TABLE f(id INTEGER PRIMARY KEY, phase TEXT)", NULL, NULL, NULL);
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); kill_proxy(); unsetenv("HPPLITE_RPC_TIMEOUT"); FAIL("schema batch through the healthy proxy"); }
    int written = 0;
    double slowest = 0, t0 = now_s();
    for (size_t p = 0; p < sizeof phase / sizeof phase[0]; p++) {
        set_proxy_mode(modeFile, phase[p].mode);
        /* four rounds a phase: writes, a flush (the application's own, past
        ** the timer's backoff), a second for the chain */
        for (int round = 0; round < 4; round++) {
            for (int i = 0; i < 2; i++) {
                char sql[128];
                snprintf(sql, sizeof sql, "INSERT INTO f VALUES(%d, '%s')", ++written, phase[p].name);
                double t1 = now_s();
                int rc = sqlite3_exec(w, sql, NULL, NULL, NULL);
                if (now_s() - t1 > slowest) slowest = now_s() - t1;
                if (rc != SQLITE_OK) { sqlite3_close(w); kill_proxy(); unsetenv("HPPLITE_RPC_TIMEOUT"); FAILF("a write failed in the %s phase", phase[p].name); }
            }
            scalar(w, "SELECT hpplite_flush()");            /* whatever the endpoint allows; the timer keeps trying too */
            sleep(1);
        }
    }
    double faulty = now_s() - t0;
    long delayed = proxy_stat(modeFile, "delayed"), dropped = proxy_stat(modeFile, "dropped"),
         timedOut = proxy_stat(modeFile, "timed_out"), errored = proxy_stat(modeFile, "errored");
    /* healthy again: everything drains */
    set_proxy_mode(modeFile, "");
    t0 = now_s();
    for (int k = 0; k < 40 && scalar(w, "SELECT pending FROM hpplite_status") > 0; k++) { scalar(w, "SELECT hpplite_flush()"); sleep(1); }
    double drain = now_s() - t0;
    sqlite3_int64 pend = scalar(w, "SELECT pending FROM hpplite_status");
    char state[64];
    text_of(w, "SELECT state FROM hpplite_status", state, sizeof state);
    printf("(write max %.3fs; faults: %ld delayed, %ld dropped, %ld timed out, %ld 503s in %.0fs; drained in %.0fs, state %s) ",
           slowest, delayed, dropped, timedOut, errored, faulty, drain, state);
    const char *why = NULL;
    if (slowest > 0.5) why = "a write waited on the endpoint";
    else if (delayed <= 0 || dropped <= 0 || timedOut <= 0 || errored <= 0) why = "the proxy did not fault every kind: the test is not exercising them";
    else if (pend != 0) why = "rows still pending once the endpoint is healthy";
    else why = chain_contiguous(nw);
    if (!why) {
        /* each row exactly once: a replica rebuilt from the chain holds every row written, no more */
        sqlite3 *r = open_db("rpc_replica.db", KEY8, NULL);
        long long rows = r ? scalar(r, "SELECT count(*) FROM f") : -1;
        if (r) sqlite3_close(r);
        if (rows != written) {
            static char dupWhy[120];
            snprintf(dupWhy, sizeof dupWhy, "a replica holds %lld rows for %d written: a row posted twice or lost", rows, written);
            why = dupWhy;
        }
    }
    if (!why && scalar(w, "SELECT count(*) FROM f") != written) why = "the file lost a row";
    sqlite3_close(w);                                        /* through the healthy proxy */
    kill_proxy();
    unsetenv("HPPLITE_RPC_TIMEOUT");
    if (why) FAIL(why);
    sqlite3 *w2 = open_db("rpc_writer.db", KEY8, NULL);
    if (!w2) FAIL("reopen");
    why = replica_equals(w2, "rpc_replica.db", KEY8);
    sqlite3_close(w2);
    if (why) FAIL(why);
    PASS();
}

/* 15. Power loss (#49): the application's process is killed (-9) in the
** middle of a transaction while its timer posts; the database file and its
** -wal are copied as they lie (what the disk holds after the loss), and
** the copy is opened on another instance: it must open consistent (every
** transaction whole or absent, the height where the chain is), continue,
** and equal a replica. KEY9's own rollup. */
static void test_power_loss(void) {
    TEST("power loss mid-transaction: the file and its -wal copied as they lie and reopened; meta consistent, transactions whole or absent, continues, replica equal");
    sqlite3 *w = open_db("pl.db", KEY9, NULL);
    if (!w) FAIL("open");
    sqlite3_exec(w, "CREATE TABLE p(txn INTEGER, i INTEGER)", NULL, NULL, NULL);
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); FAIL("schema batch"); }
    sqlite3_close(w);
    char stat[300];
    proc_status_path(9, stat, sizeof stat); unlink(stat);
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        sqlite3 *db = open_db("pl.db", KEY9, "&interval=200ms");
        if (!db) _exit(2);
        sqlite3_busy_timeout(db, 10000);
        /* a transaction of three rows, ~10 ms inside it (the flag file is up
        ** then), ~20 ms between transactions (the poster's window) */
        for (long txn = 1; ; txn++) {
            char sql[128];
            FILE *f;
            if ((f = fopen(DIR "/in_txn", "w")) != NULL) fclose(f);
            snprintf(sql, sizeof sql, "BEGIN; INSERT INTO p VALUES(%ld, 1)", txn);
            if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) { sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); continue; }
            usleep(4000);
            snprintf(sql, sizeof sql, "INSERT INTO p VALUES(%ld, 2)", txn);
            sqlite3_exec(db, sql, NULL, NULL, NULL);
            usleep(4000);
            snprintf(sql, sizeof sql, "INSERT INTO p VALUES(%ld, 3); COMMIT", txn);
            if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            unlink(DIR "/in_txn");
            if (txn % 5 == 0 && (f = fopen(stat, "w")) != NULL) {
                fprintf(f, "%ld %lld\n", txn, (long long)scalar(db, "SELECT height FROM hpplite_status"));
                fclose(f);
            }
            usleep(20000);
        }
    }
    /* once a few batches are posted, the power goes - while a transaction is open */
    double t0 = now_s();
    long txn = 0, height = 0;
    while (now_s() - t0 < 60) {
        FILE *f = fopen(stat, "r");
        if (f) { if (fscanf(f, "%ld %ld", &txn, &height) != 2) txn = 0; fclose(f); }
        if (txn >= 40 && height >= 4) break;
        usleep(50000);
    }
    for (int i = 0; i < 5000 && access(DIR "/in_txn", F_OK) != 0; i++) usleep(1000);
    kill(pid, SIGKILL); waitpid(pid, NULL, 0);
    if (txn < 40 || height < 4) FAILF("the writer did not get going (txn %ld, height %ld after 60 s)", txn, height);
    if (access(DIR "/pl.db-wal", F_OK) != 0) FAIL("no -wal beside the file: the test premise (WAL mode, a writer mid-flight) is gone");
    (void)!system("cp " DIR "/pl.db " DIR "/pl_copy.db && cp " DIR "/pl.db-wal " DIR "/pl_copy.db-wal"
                  "; [ -f " DIR "/pl.db-hpplite.db ] && cp " DIR "/pl.db-hpplite.db " DIR "/pl_copy.db-hpplite.db"
                  "; [ -f " DIR "/pl.db-hpplite.db-wal ] && cp " DIR "/pl.db-hpplite.db-wal " DIR "/pl_copy.db-hpplite.db-wal; true");   /* page mode's sidecar as it lies too (#146) */
    chain_sleep(3700);                                      /* the dead process's lease runs out (an hour on VERSION 9, #164) */

    sqlite3 *c = open_db("pl_copy.db", KEY9, NULL);
    if (!c) FAIL("the copy did not open");
    HppliteNode *nc = hpplite_get_node(c);
    char state[64], err[512];
    text_of(c, "SELECT state FROM hpplite_status", state, sizeof state);
    text_of(c, "SELECT last_error FROM hpplite_status", err, sizeof err);
    sqlite3_int64 torn = scalar(c, "SELECT count(*) FROM (SELECT txn FROM p GROUP BY txn HAVING count(*) != 3 OR min(i) != 1 OR max(i) != 3)");
    sqlite3_int64 txns = scalar(c, "SELECT count(DISTINCT txn) FROM p"), pend = scalar(c, "SELECT pending FROM hpplite_status");
    sqlite3_int64 hCopy = scalar(c, "SELECT height FROM hpplite_status");
    uint64_t last = 0, total = 0;
    hpplite_l1_get_da_state(nc->l1, &last, &total, NULL);
    printf("(killed at txn ~%ld; copy: %lld transactions, %lld pending rows, height %lld, chain %llu, state %s) ",
           txn, (long long)txns, (long long)pend, (long long)hCopy, (unsigned long long)last, state);
    if (strcmp(state, "error") == 0) { sqlite3_close(c); FAILF("the copy opened in error: %s", err); }
    if (torn != 0) { sqlite3_close(c); FAIL("a transaction is in the file in part"); }
    if ((uint64_t)hCopy != last + 1) { sqlite3_close(c); FAIL("the copy's height is not right behind the chain after open"); }
    if (scalar(c, "SELECT foreign_writes FROM hpplite_status") != 0) { sqlite3_close(c); FAIL("the copy counts its own rows as foreign writes"); }
    if (pend % 3 != 0 && strcmp(status_text(c, "capture"), "pages") != 0) { sqlite3_close(c); FAIL("the pending log holds part of a transaction"); }   /* page capture counts commits (and the file shipped whole), not rows */
    /* it continues: what was pending and a new transaction post; a replica equals the file */
    if (sqlite3_exec(c, "BEGIN; INSERT INTO p VALUES(100000, 1); INSERT INTO p VALUES(100000, 2); INSERT INTO p VALUES(100000, 3); COMMIT", NULL, NULL, NULL) != SQLITE_OK) { sqlite3_close(c); FAIL("a write on the copy"); }
    if (scalar(c, "SELECT hpplite_flush()") <= 0) { sqlite3_close(c); FAIL("the copy does not post"); }
    if (scalar(c, "SELECT pending FROM hpplite_status") != 0) { sqlite3_close(c); FAIL("pending left after the flush"); }
    const char *why = chain_contiguous(nc);
    if (!why) why = replica_equals(c, "pl_replica.db", KEY9);
    sqlite3_close(c);
    if (why) FAIL(why);
    PASS();
}

/* HPPLITE_ANVIL_TESTS=12,14 runs those cases alone (development) */
/* 16. Soak (#120): a demo-like workload for HPPLITE_SOAK_SECONDS against
** the real contracts, the timer posting every two seconds, a checkpoint
** every five batches. Every HPPLITE_SOAK_EVERY batches the workload pauses,
** the log drains, and three oracles run: a fresh file rebuilt from the chain
** alone equals the writer (rows and root), hpplite-verify says "verified"
** against the live file, and the status counts no refusal and no
** non-deterministic statement. At the end the chain is contiguous. Runs
** only when HPPLITE_SOAK_SECONDS is set (nightly, .github/workflows/soak.yml);
** meant to run alone (HPPLITE_ANVIL_TESTS=16): it writes KEY0's rollup.
** Not a test of SQL coverage (test_edge_sql is): a test of hours. */
static long soak_env(const char *name, long dflt) { const char *z = getenv(name); return z && z[0] ? atol(z) : dflt; }
static void soak_text(char *out, size_t n) {
    static const char *words[] = { "alpha", "it's", "\"quoted\"", "caf\xc3\xa9", "x", "semi;colon", "--dash", "/*c*/", "tab\t", "nl\n", "0.1", "1e308", "", "NULL", "%s", "%%" };
    size_t len = (size_t)(rand() % 100 < 3 ? 2000 + rand() % 20000 : rand() % 200), k = 0;
    while (k + 1 < n && k < len) {
        const char *w = words[rand() % (sizeof words / sizeof words[0])];
        size_t m = strlen(w);
        if (k + m + 1 >= n) break;
        memcpy(out + k, w, m); k += m;
        out[k++] = ' ';
    }
    out[k] = 0;
}
static int soak_step(sqlite3 *w, long *pStmts, long *pTxns, int *pCols) {
    char body[24000];
    sqlite3_stmt *st = NULL;
    int rc = SQLITE_OK, r = rand() % 100;
    sqlite3_int64 id = 1 + rand() % 2000;
    if (r < 45) {
        soak_text(body, sizeof body);
        rc = sqlite3_prepare_v2(w, "INSERT INTO entries(name, body, posted_at, kind) VALUES(?1, ?2, ?3, ?4)", -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(st, 1, rand() % 5 ? "soak" : "", -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, body, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 3, (sqlite3_int64)time(NULL));
            if (rand() % 4) sqlite3_bind_int(st, 4, rand() % 3); else sqlite3_bind_null(st, 4);
            rc = sqlite3_step(st) == SQLITE_DONE ? SQLITE_OK : sqlite3_errcode(w);
        }
        sqlite3_finalize(st);
        (*pStmts)++;
    } else if (r < 60) {
        soak_text(body, sizeof body);
        rc = sqlite3_prepare_v2(w, "UPDATE entries SET body = ?1, kind = coalesce(kind, 0) + 1 WHERE id = ?2", -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(st, 1, body, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 2, id);
            rc = sqlite3_step(st) == SQLITE_DONE ? SQLITE_OK : sqlite3_errcode(w);
        }
        sqlite3_finalize(st);
        (*pStmts)++;
    } else if (r < 68) {
        char sql[200];
        snprintf(sql, sizeof sql, "DELETE FROM entries WHERE id = %lld", (long long)id);
        rc = exec_sql(w, sql);
        (*pStmts)++;
    } else if (r < 80) {
        /* a blob, a float, a WITHOUT ROWID key: the record kinds the demo never writes */
        unsigned char blob[512];
        int n = rand() % (int)sizeof blob, i;
        for (i = 0; i < n; i++) blob[i] = (unsigned char)rand();
        rc = sqlite3_prepare_v2(w, "INSERT OR REPLACE INTO soak_bin(id, b, f) VALUES(?1, ?2, ?3)", -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, id);
            sqlite3_bind_blob(st, 2, blob, n, SQLITE_STATIC);
            sqlite3_bind_double(st, 3, rand() / 7.0);
            rc = sqlite3_step(st) == SQLITE_DONE ? SQLITE_OK : sqlite3_errcode(w);
        }
        sqlite3_finalize(st);
        snprintf(body, sizeof body, "INSERT OR REPLACE INTO soak_kv(k, v) VALUES('k%lld', %d)", (long long)id, rand());
        if (rc == SQLITE_OK) rc = exec_sql(w, body);
        *pStmts += 2;
    } else if (r < 95) {
        /* a transaction of several statements, one in five rolled back */
        int n = 2 + rand() % 6, i, back = rand() % 5 == 0;
        rc = exec_sql(w, "BEGIN");
        for (i = 0; i < n && rc == SQLITE_OK; i++) {
            int inner = rand() % 100;
            if (inner < 50) {
                soak_text(body, sizeof body);
                char sql[200];
                snprintf(sql, sizeof sql, "INSERT INTO entries(name, body, posted_at) VALUES('txn', ?1, %ld)", (long)time(NULL));
                rc = sqlite3_prepare_v2(w, sql, -1, &st, NULL);
                if (rc == SQLITE_OK) { sqlite3_bind_text(st, 1, body, -1, SQLITE_STATIC); rc = sqlite3_step(st) == SQLITE_DONE ? SQLITE_OK : sqlite3_errcode(w); }
                sqlite3_finalize(st);
            } else if (inner < 80) {
                char sql[200];
                snprintf(sql, sizeof sql, "UPDATE entries SET posted_at = posted_at + 1 WHERE id %% 7 = %d", rand() % 7);
                rc = exec_sql(w, sql);
            } else {
                char sql[200];
                snprintf(sql, sizeof sql, "DELETE FROM soak_kv WHERE k = 'k%lld'", (long long)(1 + rand() % 2000));
                rc = exec_sql(w, sql);
            }
            (*pStmts)++;
        }
        if (rc == SQLITE_OK) rc = exec_sql(w, back ? "ROLLBACK" : "COMMIT"); else exec_sql(w, "ROLLBACK");
        (*pTxns)++;
    } else {
        /* schema changes, as a migration would make them */
        char sql[200];
        int which = rand() % 4;
        if (which == 0 && *pCols < 20) { snprintf(sql, sizeof sql, "ALTER TABLE entries ADD COLUMN extra%d TEXT DEFAULT ''", (*pCols)++); }
        else if (which == 1) snprintf(sql, sizeof sql, "CREATE INDEX IF NOT EXISTS entries_kind ON entries(kind, posted_at)");
        else if (which == 2) snprintf(sql, sizeof sql, "DROP INDEX IF EXISTS entries_kind");
        else snprintf(sql, sizeof sql, "CREATE TABLE IF NOT EXISTS soak_side%d(id INTEGER PRIMARY KEY, t TEXT)", rand() % 3);
        rc = exec_sql(w, sql);
        (*pStmts)++;
    }
    return rc;
}
/* the log drained, so the file equals what the chain holds */
static const char *soak_drain(sqlite3 *w) {
    int i;
    for (i = 0; i < 60; i++) {
        if (scalar(w, "SELECT pending FROM hpplite_status") == 0) return NULL;
        if (scalar(w, "SELECT hpplite_flush()") < 0) return "flush failed";
        usleep(200000);
    }
    return "the log did not drain in 60 flushes";
}
/* ...and for the checkpoint a root batch owes: the timer thread may still be
** submitting it when the log reads empty, and hpplite-verify asks the chain */
static const char *soak_settle(sqlite3 *w) {
    int i;
    const char *why = soak_drain(w);
    if (why) return why;
    for (i = 0; i < 50; i++) {
        if (scalar(w, "SELECT height - 1 - last_checkpoint_height FROM hpplite_status") < 5) return NULL;   /* checkpoint=5 */
        usleep(200000);
    }
    return "the owed checkpoint did not land in 10 s";
}
static pid_t start_pin_service_free(const char *svc, int port);
static void soak_stop_store(pid_t pid) {
    if (pid > 0) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); }
    unsetenv("HPPLITE_STORE");
}
static void test_soak(void) {
    long seconds = soak_env("HPPLITE_SOAK_SECONDS", 0), every = soak_env("HPPLITE_SOAK_EVERY", 10);
    const char *storeMode = getenv("HPPLITE_SOAK_STORE");       /* "pin": the bytes at a free pin service, the chain commits (#11) */
    pid_t svcPid = -1;
    char extra[200] = "&interval=2000ms&checkpoint=5", replicaExtra[120] = "&role=verify", storeUrl[64] = "";
    long stmts = 0, txns = 0, checks = 0, failedStmts = 0;
    int cols = 0;
    double start = now_s(), deadline = start + seconds, worstRebuild = 0, worstVerify = 0;
    sqlite3_int64 lastCheckHeight = 1;
    char out[8192], verifyCmd[300];
    const char *why = NULL;
    TEST("soak: a demo-like workload for hours; every N batches a fresh replica equals the writer and hpplite-verify agrees (#120)");
    printf("\n    %ld s, an oracle pass every %ld batches\n", seconds, every);
    if (storeMode && storeMode[0] && strcmp(storeMode, "0") != 0) {
        const char *svc = getenv("HPPLITE_PIN_SERVICE");
        int port = test_free_port();
        if (!svc) FAIL("HPPLITE_SOAK_STORE needs HPPLITE_PIN_SERVICE (anvil_test.sh sets it)");
        svcPid = start_pin_service_free(svc, port);
        if (svcPid < 0) FAIL("the pin service did not start");
        snprintf(storeUrl, sizeof storeUrl, "http://127.0.0.1:%d", port);
        snprintf(extra, sizeof extra, "&interval=2000ms&checkpoint=5&store=%s", storeUrl);
        snprintf(replicaExtra, sizeof replicaExtra, "&role=verify&store=%s", storeUrl);
        setenv("HPPLITE_STORE", storeUrl, 1);                       /* hpplite-verify reads the bytes there too */
        printf("    the bytes at a pin service (%s); the chain holds the commitments\n", storeUrl);
    }
    sqlite3 *w = open_db("soak.db", KEY0, extra);
    if (!w) { soak_stop_store(svcPid); FAIL("open writer"); }
    if (exec_sql(w, "CREATE TABLE IF NOT EXISTS entries(id INTEGER PRIMARY KEY, name TEXT NOT NULL, body TEXT NOT NULL, posted_at INTEGER NOT NULL, kind INTEGER);"
                    "CREATE TABLE IF NOT EXISTS soak_bin(id INTEGER PRIMARY KEY, b BLOB, f REAL);"
                    "CREATE TABLE IF NOT EXISTS soak_kv(k TEXT PRIMARY KEY, v INTEGER) WITHOUT ROWID") != SQLITE_OK) { sqlite3_close(w); soak_stop_store(svcPid); FAIL("schema"); }
    cols = (int)scalar(w, "SELECT count(*) FROM pragma_table_info('entries') WHERE name LIKE 'extra%'");
    snprintf(verifyCmd, sizeof verifyCmd, "--db " DIR "/soak.db");
    while (now_s() < deadline && !why) {
        int rc = soak_step(w, &stmts, &txns, &cols);
        if (rc != SQLITE_OK) {
            failedStmts++;
            if (rc == SQLITE_INTERRUPT) { snprintf(out, sizeof out, "a statement was refused: %s", sqlite3_errmsg(w)); why = out; break; }
        }
        usleep((useconds_t)(20000 + rand() % 180000));
        sqlite3_int64 height = scalar(w, "SELECT height FROM hpplite_status");
        if (height >= lastCheckHeight + every) {
            double t0, tRebuild, tVerify;
            int vrc;
            checks++;
            if ((why = soak_settle(w)) != NULL) break;
            t0 = now_s();
            unlink(DIR "/soak_replica.db"); unlink(DIR "/soak_replica.db-wal"); unlink(DIR "/soak_replica.db-shm");
            why = replica_equals_as(w, "soak_replica.db", KEY0, replicaExtra);
            tRebuild = now_s() - t0;
            if (why) break;
            t0 = now_s();
            out[0] = 0;
            vrc = run_verify(verifyCmd, out, sizeof out);
            tVerify = now_s() - t0;
            if (vrc != 0 || !strstr(out, "\nverified")) { snprintf(out + strlen(out), sizeof out - strlen(out), "\n(hpplite-verify exit %d)", vrc); why = out; break; }
            if (tRebuild > worstRebuild) worstRebuild = tRebuild;
            if (tVerify > worstVerify) worstVerify = tVerify;
            printf("    %6.0f s  height %5lld  %7ld statements  %5ld txns  rebuild %.1f s  verify %.1f s  rows %lld\n",
                   now_s() - start, (long long)height, stmts, txns, tRebuild, tVerify, (long long)scalar(w, "SELECT count(*) FROM entries"));
            fflush(stdout);
            lastCheckHeight = height;
        }
    }
    if (!why) {
        HppliteNode *n = hpplite_get_node(w);
        why = soak_settle(w);
        if (!why) why = chain_contiguous(n);
        if (!why) { unlink(DIR "/soak_replica.db"); unlink(DIR "/soak_replica.db-wal"); unlink(DIR "/soak_replica.db-shm"); why = replica_equals_as(w, "soak_replica.db", KEY0, replicaExtra); }
        if (!why && run_verify(verifyCmd, out, sizeof out) != 0) why = out;
    }
    printf("    end: height %lld, %ld statements (%ld failed, none refused), %ld transactions, %ld oracle passes, worst rebuild %.1f s, worst verify %.1f s\n",
           (long long)scalar(w, "SELECT height FROM hpplite_status"), stmts, failedStmts, txns, checks, worstRebuild, worstVerify);
    if (svcPid > 0) printf("    store: %lld puts, state %s\n", (long long)scalar(w, "SELECT store_calls FROM hpplite_status"), status_text(w, "store_state"));
    sqlite3_close(w);
    soak_stop_store(svcPid);
    if (why) FAIL(why);
    if (checks == 0) FAIL("no oracle pass ran: too short, or the timer never posted");
    PASS();
}

/* the money knobs are environment, not URI (#254): a scenario sets what it needs; wanted() clears them before the next */
static void money_env(const char *cap, const char *permit, const char *spend, const char *low, const char *relayCap, const char *fallback) {
    const char *k[] = { "HPPLITE_STORE_CAP", "HPPLITE_STORE_PERMIT", "HPPLITE_SPEND_CAP", "HPPLITE_LOW_BALANCE", "HPPLITE_RELAY_CAP", "HPPLITE_STORE_FALLBACK" };
    const char *v[] = { cap, permit, spend, low, relayCap, fallback };
    for (int i = 0; i < 6; i++) { if (v[i]) setenv(k[i], v[i], 1); else unsetenv(k[i]); }
}

static int wanted(int number) {
    const char *z = getenv("HPPLITE_ANVIL_TESTS");
    char tag[16];
    money_env(NULL, NULL, NULL, NULL, NULL, NULL);
    if (!z || !z[0]) return 1;
    snprintf(tag, sizeof tag, ",%d,", number);
    char all[256];
    snprintf(all, sizeof all, ",%s,", z);
    return strstr(all, tag) != NULL;
}

/* 17. DA v2 (#33): a batch's bytes are in its log and the contract keeps a
** rolling hash. A batch of about 1 KB costs under 100k gas (storage took
** ~700k); the writer's and a replica's chain hash equal the contract's; a
** commitment altered on the chain (anvil_setStorageAt on chainHash, slot 3)
** makes a fresh rebuild stop in the error state, and the chain restored, a
** rebuild passes again. KEY2, its own named database. */
static long receipt_gas(const char *tx) {
    char cmd[300], out[64] = "";
    FILE *f;
    if (!tx || !tx[0]) return -1;
    snprintf(cmd, sizeof cmd, "cast receipt %s gasUsed --rpc-url %s 2>/dev/null", tx, RPC);
    if ((f = popen(cmd, "r")) != NULL) { if (!fgets(out, sizeof out, f)) out[0] = 0; pclose(f); }
    return atol(out);
}
static const char *strip0x(const char *z) { return (z[0] == '0' && (z[1] == 'x' || z[1] == 'X')) ? z + 2 : z; }
static void test_chain_hash(void) {
    TEST("DA v2 (#33): a 1 KB batch far below storage cost; writer and replica chain hash equal the contract's; a commitment altered on chain is refused");
    sqlite3 *w = open_db("ch_writer.db", KEY2, "&dbname=chainhash");
    if (!w) FAIL("open writer");
    sqlite3_exec(w, "CREATE TABLE c(id INTEGER PRIMARY KEY, v TEXT)", NULL, NULL, NULL);
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); FAIL("batch 1"); }
    char big[1001], sql[1200];
    memset(big, 'x', 1000); big[1000] = 0;
    snprintf(sql, sizeof sql, "INSERT INTO c(v) VALUES('%s')", big);
    sqlite3_exec(w, sql, NULL, NULL, NULL);
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); FAIL("batch 2"); }
    long gas = receipt_gas(status_text(w, "last_batch_tx"));
    printf("(1 KB batch: %ld gas) ", gas);
    /* measured 97k: 21k base, the sealed row's ~1.2 KB as calldata and log data, three slots; storage took ~700k */
    if (gas <= 0 || gas >= 120000) { sqlite3_close(w); FAIL("a 1 KB batch should cost well under the storage DA's ~700k gas"); }
    sqlite3_exec(w, "INSERT INTO c(v) VALUES('three')", NULL, NULL, NULL);
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); FAIL("batch 3"); }

    char contract[80], da[80], mine[80], want[80];
    snprintf(contract, sizeof contract, "%s", status_text(w, "contract"));
    snprintf(da, sizeof da, "%s", status_text(w, "da_contract"));
    snprintf(mine, sizeof mine, "%s", status_text(w, "chain_hash"));
    {
        HppliteL1 *peek = hpplite_l1_connect(RPC, contract);
        uint64_t h = 0;
        uint8_t chain[32];
        if (!peek || hpplite_l1_get_chain_hash(peek, &h, chain) != 0) { hpplite_l1_disconnect(peek); sqlite3_close(w); FAIL("the contract reports no chain hash"); }
        hpplite_l1_disconnect(peek);
        snprintf(want, sizeof want, "0x");
        for (int i = 0; i < 32; i++) snprintf(want + 2 + 2 * i, 3, "%02x", chain[i]);
        if (h != 3 || strcasecmp(strip0x(mine), strip0x(want)) != 0) {
            printf("(contract %s at %llu, writer '%s') ", want, (unsigned long long)h, mine);
            sqlite3_close(w);
            FAIL("the writer's chain hash differs from the contract's");
        }
    }

    sqlite3 *r = open_db("ch_replica.db", KEY2, "&dbname=chainhash&role=verify");
    if (!r) { sqlite3_close(w); FAIL("open replica"); }
    if (strcmp(status_text(r, "state"), "verify") != 0 || scalar(r, "SELECT count(*) FROM c") != 2
        || strcasecmp(strip0x(status_text(r, "chain_hash")), strip0x(want)) != 0) {
        printf("(replica state %s, chain_hash '%s', rows %lld) ", status_text(r, "state"), status_text(r, "chain_hash"), (long long)scalar(r, "SELECT count(*) FROM c"));
        sqlite3_close(r); sqlite3_close(w);
        FAIL("the replica's chain hash differs from the contract's");
    }
    sqlite3_close(r);

    /* the chain's commitment altered: slot 3 of the DA contract is chainHash */
    char arg[300], out[200];
    snprintf(arg, sizeof arg, "%s 0x3 0x1111111111111111111111111111111111111111111111111111111111111111", da);
    cast_rpc("anvil_setStorageAt", arg, out, sizeof out);
    sqlite3 *r2 = open_db("ch_replica2.db", KEY2, "&dbname=chainhash&role=verify");
    const char *st = r2 ? status_text(r2, "state") : "(no open)";
    char err[300];
    snprintf(err, sizeof err, "%s", r2 ? status_text(r2, "last_error") : "");
    int refused = r2 && strcmp(st, "error") == 0 && strstr(err, "chain hash") != NULL;
    printf("(tampered: state %s) ", st);
    snprintf(arg, sizeof arg, "%s 0x3 %s", da, want);
    cast_rpc("anvil_setStorageAt", arg, out, sizeof out);   /* the chain restored */
    if (r2) sqlite3_close(r2);
    if (!refused) { printf("(last_error '%s') ", err); sqlite3_close(w); FAIL("a commitment altered on the chain was not refused"); }

    sqlite3 *r3 = open_db("ch_replica3.db", KEY2, "&dbname=chainhash&role=verify");
    if (!r3) { sqlite3_close(w); FAIL("open replica after restore"); }
    if (strcmp(status_text(r3, "state"), "verify") != 0) { printf("(state %s) ", status_text(r3, "state")); sqlite3_close(r3); sqlite3_close(w); FAIL("the restored chain should rebuild"); }
    sqlite3_close(r3);
    sqlite3_close(w);
    PASS();
}

/* commands for the store scenarios: their first line, and its exit status */
static long first_number(const char *z) { return atol(z); }
static int run_cmd(const char *cmd, char *out, size_t n) {
    FILE *p = popen(cmd, "r");
    size_t got = 0;
    int status;
    out[0] = 0;
    if (!p) return -1;
    while (got + 1 < n && fgets(out + got, (int)(n - got), p)) got = strlen(out);
    status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* 19. A store for the bytes (#11): the reference store (free, hpplite-store/2) as store= on the
** writer: the chain gets commitments (the anchor's range with the bucket statement) and no bytes,
** the store the bytes; a replica with the same store=
** rebuilds equal, one without cannot and says so; the commit transaction
** costs a fraction of posting the bytes. KEY2, its own named database. */
static pid_t start_pin_service_dir(const char *svc, int port, const char *dir);
static pid_t start_pin_service_free(const char *svc, int port) { return start_pin_service_dir(svc, port, DIR "/store_pins"); }
static pid_t start_pin_service_dir(const char *svc, int port, const char *dir) {
    pid_t pid;
    FILE *f = fopen(DIR "/store_signer.key", "w");
    if (!f) return -1;
    fprintf(f, "%s\n", KEY3);
    fclose(f);
    pid = fork();
    if (pid == 0) {
        char portStr[16], script[600];
        snprintf(portStr, sizeof portStr, "%d", port);
        snprintf(script, sizeof script, "%s/hpplite_store.py", svc);
        int fd = open(DIR "/store_service.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execlp("python3", "python3", "-I", script, "--port", portStr, "--dir", dir, "--signer-key", DIR "/store_signer.key",
               "--pay-to", "0x1111111111111111111111111111111111111111", "--network", "eip155:31337",   /* the store's domain: anvil's chain */
               "--rpc", RPC, "--price-per-write", "0", "--price-per-mib-month", "0", (char *)NULL);       /* free; the rollup's sequencer and lease from the chain (#165, #214) */
        _exit(127);
    }
    for (int i = 0; i < 50; i++) {
        char cmd[200], out[400];
        if (!test_child_alive(pid)) return -1;     /* the port was taken: never probe another's server (#177) */
        snprintf(cmd, sizeof cmd, "curl -s -m 2 http://127.0.0.1:%d/status", port);
        if (run_cmd(cmd, out, sizeof out) == 0 && strstr(out, "\"signer\"")) return pid;
        usleep(200 * 1000);
    }
    kill(pid, SIGKILL); waitpid(pid, NULL, 0);
    return -1;
}
static void test_store_commit(void) {
    TEST("a store for the bytes (#11, #164, #214): batches in the rollup's bucket, nothing on the chain until the anchor, then one range with the root and the bucket statement; a replica with the store rebuilds, one without says so; the anchor is cheap");
    const char *svc = getenv("HPPLITE_PIN_SERVICE");
    if (!svc) { printf("SKIP (no pin service in the environment) "); PASS(); return; }
    int port = test_free_port();
    pid_t pid = start_pin_service_free(svc, port);
    if (pid < 0) FAIL("the pin service did not come up");
    char extra[200], big[1001], sql[1200], contract[80], want[80], out[4096] = "";
    snprintf(extra, sizeof extra, "&dbname=stored&store=http://127.0.0.1:%d", port);
    sqlite3 *w = open_db("st_writer.db", KEY2, extra);
    if (!w) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("open writer"); }
    sqlite3_exec(w, "CREATE TABLE s(id INTEGER PRIMARY KEY, v TEXT)", NULL, NULL, NULL);
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { printf("(%s) ", status_text(w, "last_error")); sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("batch 1 through the store"); }
    memset(big, 'y', 1000); big[1000] = 0;
    snprintf(sql, sizeof sql, "INSERT INTO s(v) VALUES('%s')", big);
    sqlite3_exec(w, sql, NULL, NULL, NULL);
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("batch 2 through the store"); }
    snprintf(contract, sizeof contract, "%s", status_text(w, "contract"));
    if (scalar(w, "SELECT anchored FROM hpplite_status") != 0 || scalar(w, "SELECT height FROM hpplite_status") != 3 || status_text(w, "last_batch_tx")[0]) {
        printf("(anchored %lld, height %lld, last_batch_tx '%s') ", (long long)scalar(w, "SELECT anchored FROM hpplite_status"), (long long)scalar(w, "SELECT height FROM hpplite_status"), status_text(w, "last_batch_tx"));
        sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("two batches at the store, no transaction yet");
    }
    if (scalar(w, "SELECT hpplite_anchor()") != 2 || scalar(w, "SELECT anchored FROM hpplite_status") != 2 || scalar(w, "SELECT last_checkpoint_height FROM hpplite_status") != 2) {
        printf("(%s) ", status_text(w, "last_error")); sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("the anchor should commit the range 1-2 and be the checkpoint");
    }
    long gasFirst = receipt_gas(status_text(w, "last_anchor_tx"));
    {
        /* each batch is also under its rollup's name at the store, signed by the sequencer; the service checked the signer
        ** against the coordinator's sequencerWallet() over the RPC (#165) */
        char da[43], c2[43], name[80], hex40[41], *p, url[64];
        unsigned char daBytes[20], who[20], me[20];
        uint8_t sig[65], *bytes;
        size_t n = 0;
        HppliteStore *st;
        int ok;
        snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
        st = hpplite_store_open(url, NULL, 0);
        (void)c2;
        snprintf(da, sizeof da, "%s", status_text(w, "da_contract"));
        snprintf(name, sizeof name, "%.40s-2", da + 2);
        for (p = name; *p && *p != '-'; p++) if (*p >= 'A' && *p <= 'F') *p = (char)(*p - 'A' + 'a');
        memcpy(hex40, name, 40); hex40[40] = 0; hpplite_hex_to_bytes(hex40, daBytes, 20);
        bytes = st ? hpplite_store_get_named(st, name, &n, sig, NULL) : NULL;
        {
            HppliteNode *nd = hpplite_get_node(w);
            unsigned char k[32];
            hpplite_hex_to_bytes(KEY2 + 2, k, 32);
            eth_address_from_privkey(k, me);
            ok = bytes && nd && hpplite_store_batch_signer(daBytes, 31337, nd->instanceId, 2, bytes, n, sig, who) == 0 && memcmp(who, me, 20) == 0;
        }
        if (!ok) printf("(name %s: %s) ", name, st ? hpplite_store_error(st) : "no store");
        if (st) hpplite_store_close(st);
        if (!ok) { free(bytes); sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("batch 2 under its rollup's name at the store, signed by the sequencer wallet"); }
        {
            /* a partitioned old sequencer (#167): the same wallet, another instance - not the one the chain's lease
            ** names - writes a different batch at height 2. The service refuses it; the name keeps the sequencer's */
            unsigned char k[32], other[32];
            uint8_t sig2[65], *again;
            size_t n2 = 0;
            static const char forged[] = "a batch the lease holder did not write";
            HppliteStore *old;
            HppliteNode *nd = hpplite_get_node(w);
            hpplite_hex_to_bytes(KEY2 + 2, k, 32);
            old = hpplite_store_open(url, k, 0);
            if (old) hpplite_store_set_bucket(old, daBytes, 0);
            memcpy(other, nd->instanceId, 32);
            other[0] ^= 0xff;
            int refused = old && hpplite_store_sign_batch(old, daBytes, 31337, other, 2, (const uint8_t *)forged, sizeof forged - 1, sig2) == 0
                          && hpplite_store_put_named(old, name, other, (const uint8_t *)forged, sizeof forged - 1, sig2) != 0
                          && strstr(hpplite_store_error(old), "409") != NULL;
            printf("(an old instance's batch at height 2: %s) ", refused ? hpplite_store_error(old) : "taken");
            if (old) hpplite_store_close(old);
            st = hpplite_store_open(url, NULL, 0);
            again = st ? hpplite_store_get_named(st, name, &n2, sig2, NULL) : NULL;
            int kept = again && n2 == n && memcmp(again, bytes, n) == 0;
            free(again);
            if (st) hpplite_store_close(st);
            free(bytes);
            if (!refused || !kept) { sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("a batch from an instance that does not hold the lease must be refused, the name kept (#167)"); }
        }
    }
    /* the first anchor of a rollup writes its slots for the first time (three fresh slots, ~20k each); the cost of an anchor is the second one's */
    sqlite3_exec(w, "INSERT INTO s(v) VALUES('three')", NULL, NULL, NULL);
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("batch 3 through the store"); }
    if (scalar(w, "SELECT hpplite_anchor()") != 3) { printf("(%s) ", status_text(w, "last_error")); sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("the second anchor, range 3-3"); }
    long gas = receipt_gas(status_text(w, "last_anchor_tx"));
    printf("(first anchor, fresh slots: %ld gas; second anchor, root inside, 138-byte bucket statement: %ld gas) ", gasFirst, gas);
    snprintf(want, sizeof want, "%s", status_text(w, "chain_hash"));
    {
        /* what the chain holds: one range, no per-height commitment, the manifest's digest and the store's receipt for it */
        HppliteL1 *peek = hpplite_l1_connect(RPC, contract);
        HppliteRange r;
        uint8_t hash[32], *receipt = NULL, *mbytes = NULL;
        char *zRef = NULL, cid[64];
        size_t n = 0, nReceipt = 0, mlen = 0;
        unsigned char signer[20], k[32];
        uint64_t expires = 0, h = 0;
        uint8_t chain[32];
        uint8_t *bytes = peek ? hpplite_l1_get_batch(peek, 2, &n) : NULL;
        int perHeight = peek && hpplite_l1_get_commitment(peek, 2, hash, &zRef, &receipt, &nReceipt) == 0;
        HppliteRange r3;
        int ok = peek && !bytes && !perHeight && hpplite_l1_get_range(peek, 2, &r) == 0 && r.fromHeight == 1 && r.toHeight == 2 && r.nReceipt > 0
                 && hpplite_l1_get_range(peek, 3, &r3) == 0 && r3.fromHeight == 3 && r3.toHeight == 3
                 && hpplite_l1_get_chain_hash(peek, &h, chain) == 0 && h == 3 && memcmp(chain, r3.chainHashAfter, 32) == 0;
        if (peek && r3.fromHeight) hpplite_l1_range_clear(&r3);
        if (ok) {
            /* the manifest at the store under the digest's CID; the receipt is the store's promise over it */
            HppliteStore *st;
            char url[64];
            snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
            st = hpplite_store_open(url, NULL, 0);
            hpplite_store_cid_from_sha256(r.manifestDigest, cid);
            mbytes = st ? hpplite_store_get(st, cid, &mlen) : NULL;
            if (mbytes) hpplite_keccak256(mbytes, mlen, k);
            /* the receipt field holds the bucket statement (#214): version 2, the store's signer, covered ahead */
            if (r.nReceipt == HPPLITE_STATEMENT_SIZE) {
                memcpy(signer, r.receipt + 1, 20);
                for (int i = 0; i < 8; i++) expires = (expires << 8) | r.receipt[57 + i];
            }
            ok = mbytes && strstr((const char *)mbytes, "\"batches\"") && strstr((const char *)mbytes, "\"from\":1") && strstr((const char *)mbytes, "\"to\":2")
                 && r.nReceipt == HPPLITE_STATEMENT_SIZE && r.receipt[0] == 2 && expires > (uint64_t)time(NULL)
                 && st && hpplite_store_signer(st, k) == 0 && memcmp(k, signer, 20) == 0;
            if (!ok) printf("(manifest %s: %zu bytes, receipt %zu bytes) ", cid, mlen, r.nReceipt);
            if (st) hpplite_store_close(st);
            hpplite_l1_range_clear(&r);
        } else {
            printf("(bytes on chain %zu, per-height commitment %d, range %s) ", n, perHeight, peek ? "not found or wrong" : "no connection");
        }
        free(bytes); free(mbytes);
        sqlite3_free(zRef); sqlite3_free(receipt);
        hpplite_l1_disconnect(peek);
        if (!ok) { sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("the chain should hold one range 1-2 with the manifest's digest and the store's receipt, and nothing per height"); }
    }
    if (gas <= 0 || gas >= 90000) { sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAILF("an anchor should cost under 90k gas, about what one commitBatch did (%ld)", gas); }

    snprintf(extra, sizeof extra, "&dbname=stored&role=verify&store=http://127.0.0.1:%d", port);
    sqlite3 *r = open_db("st_replica.db", KEY2, extra);
    if (!r) { sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("open replica"); }
    if (strcmp(status_text(r, "state"), "verify") != 0 || scalar(r, "SELECT count(*) FROM s") != 2
        || strcasecmp(strip0x(status_text(r, "chain_hash")), strip0x(want)) != 0 || scalar(r, "SELECT anchored FROM hpplite_status") != 3) {
        printf("(replica state %s, last_error '%s', rows %lld, anchored %lld) ", status_text(r, "state"), status_text(r, "last_error"), (long long)scalar(r, "SELECT count(*) FROM s"), (long long)scalar(r, "SELECT anchored FROM hpplite_status"));
        sqlite3_close(r); sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        FAIL("a replica with the store should rebuild equal from the range");
    }
    sqlite3_close(r);
    {
        /* hpplite-verify says what the chain anchored */
        char params[300];
        snprintf(params, sizeof params, "rpc=%s&factory=%s&chainid=31337&privkey=%s&dbname=stored&store=http://127.0.0.1:%d", RPC, FACTORY, KEY2, port);
        int rc = run_verify_with(params, NULL, out, sizeof out);
        if (rc != 0 || !strstr(out, "anchored to 3: the last range 3-3") || !strstr(out, "bucket statement signed by 0x")
            || !strstr(out, "(signature verifies)")) {
            printf("(hpplite-verify rc %d: %.300s) ", rc, out);
            sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0);
            FAIL("hpplite-verify should report the range the chain anchored and its bucket statement, verified (#215)");
        }
        /* without store=: the bytes are at a store the command line did not name - a gap in the parameters, not a
        ** verdict: exit 2 'could not verify ... give store=' (#247). By name, the names themselves are at the store,
        ** so the rollup is not even found; by address, the rollup is known and the chain-only facts are still reported */
        snprintf(params, sizeof params, "rpc=%s&factory=%s&chainid=31337&privkey=%s&dbname=stored", RPC, FACTORY, KEY2);
        rc = run_verify_with(params, NULL, out, sizeof out);
        if (rc != 2 || !strstr(out, "give store=") || !strstr(out, "could not verify") || strstr(out, "NOT verified")) {
            printf("(hpplite-verify by name without store= rc %d: %.400s) ", rc, out);
            sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0);
            FAIL("hpplite-verify by name without store= should exit 2 'could not verify ... give store=' (#247)");
        }
        snprintf(params, sizeof params, "rpc=%s&chainid=31337&privkey=%s&contract=%s", RPC, KEY2, contract);
        rc = run_verify_with(params, NULL, out, sizeof out);
        if (rc != 2 || !strstr(out, "give store=") || !strstr(out, "could not verify") || strstr(out, "NOT verified")
            || !strstr(out, "anchored to 3: the last range 3-3")) {
            printf("(hpplite-verify by address without store= rc %d: %.400s) ", rc, out);
            sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0);
            FAIL("hpplite-verify by address without store= should exit 2 'could not verify ... give store=' and still report the anchor (#247)");
        }
    }

    /* without the store the bytes are out of reach: nothing rebuilt, and the reason says so */
    sqlite3 *r2 = open_db("st_replica2.db", KEY2, "&dbname=stored&role=verify");
    char err2[300];
    snprintf(err2, sizeof err2, "%s", r2 ? status_text(r2, "last_error") : "");
    long long rows2 = r2 ? (long long)scalar(r2, "SELECT count(*) FROM s") : -2;
    int refused = !r2 || (rows2 != 2 && strstr(err2, "store") != NULL);
    printf("(without a store: rows %lld, last_error '%.60s') ", rows2, err2);
    if (r2) sqlite3_close(r2);
    /* the close anchors what the store holds since */
    sqlite3_exec(w, "INSERT INTO s(v) VALUES('four')", NULL, NULL, NULL);
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("batch 4 through the store"); }
    sqlite3_close(w);
    {
        HppliteL1 *peek = hpplite_l1_connect(RPC, contract);
        HppliteRange r4;
        int closed = peek && hpplite_l1_get_range(peek, 4, &r4) == 0 && r4.fromHeight == 4 && r4.toHeight == 4;
        if (closed) hpplite_l1_range_clear(&r4);
        hpplite_l1_disconnect(peek);
        if (!closed) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("the close should anchor batch 4 as a range of one"); }
    }
    {
        /* checkpoint= under a store (#256): the anchor carries the root, so the knob writes no contract - the rollup's
        ** interval stays and one stderr line says so (before: a paid setCheckpointInterval at every such open) */
        char cp[260];
        HppliteL1 *peek = hpplite_l1_connect(RPC, contract);
        HppliteSystemConfig *sc = peek ? hpplite_l1_get_system_config(peek) : NULL;
        unsigned long long before = sc ? (unsigned long long)sc->checkpointInterval : 0, after = 0;
        if (sc) hpplite_l1_system_config_free(sc);
        hpplite_l1_disconnect(peek);
        snprintf(cp, sizeof cp, "&dbname=stored&store=http://127.0.0.1:%d&checkpoint=7", port);   /* extra carries role=verify by now */
        sqlite3 *w3 = open_db("st_writer.db", KEY2, cp);
        if (w3) { sqlite3_exec(w3, "INSERT INTO s(v) VALUES('five')", NULL, NULL, NULL); scalar(w3, "SELECT hpplite_flush()"); sqlite3_close(w3); }
        peek = hpplite_l1_connect(RPC, contract);                 /* a fresh connection: the system config is cached per connection */
        sc = peek ? hpplite_l1_get_system_config(peek) : NULL;
        after = sc ? (unsigned long long)sc->checkpointInterval : 0;
        if (sc) hpplite_l1_system_config_free(sc);
        hpplite_l1_disconnect(peek);
        printf("(checkpoint=7 under the store: the contract's interval %llu before, %llu after) ", before, after);
        if (!w3 || before == 0 || after != before) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("checkpoint= under a store must write no contract: the interval stays"); }
    }
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    if (!refused) FAIL("a replica without a store should rebuild nothing and say why");
    PASS();
}

/* 20. The node pays its bucket (#214, hpplite-store/2): the reference store on the mock facilitator
** (MockUSDC, KEY1 the pay-to, KEY3 the store's signer and puller). The node's funding wallet - its
** key derived from the node's - holds MockUSDC and no ETH. With the default HPPLITE_STORE_PERMIT the node
** binds the bucket and gives a permit; its writes accrue and the store pulls them from the funding
** wallet. With HPPLITE_STORE_PERMIT=0 a write's 402 is paid by x402 from the funding wallet, up to HPPLITE_STORE_CAP:
** below the top-up nothing is signed and the batch waits. */
static long long anvil_usdc_of(const char *usdc, const char *addr) {
    char cmd[400], out[200];
    snprintf(cmd, sizeof cmd, "cast call %s 'balanceOf(address)(uint256)' %s --rpc-url %s", usdc, addr, RPC);
    return run_cmd(cmd, out, sizeof out) == 0 ? (long long)first_number(out) : -1;
}
static void test_store_paid(void) {
    TEST("the node pays its bucket (#214): the funding wallet (no ETH) gives a permit and the store pulls what the writes cost; with HPPLITE_STORE_PERMIT=0 a 402 is a top-up paid by x402 up to HPPLITE_STORE_CAP, below it nothing is signed and the batch waits");
    const char *svc = getenv("HPPLITE_PIN_SERVICE"), *usdc = getenv("HPPLITE_ANVIL_USDC");
    if (!svc || !usdc || !usdc[0]) { printf("SKIP (no store or MockUSDC in the environment) "); PASS(); return; }
    int port = test_free_port();
    char cmd[2400], payTo[80], extra[300], funder[80];
    FILE *f = fopen(DIR "/paid_signer.key", "w");
    if (!f) FAIL("signer key file");
    fprintf(f, "%s\n", KEY3);
    fclose(f);
    snprintf(cmd, sizeof cmd, "cast wallet address %s", KEY1);
    if (run_cmd(cmd, payTo, sizeof payTo) != 0) FAIL("cast wallet address");
    payTo[strcspn(payTo, "\r\n")] = 0;
    long long before = anvil_usdc_of(usdc, payTo);
    pid_t pid = fork();
    if (pid == 0) {
        char portStr[16], script[600];
        snprintf(portStr, sizeof portStr, "%d", port);
        snprintf(script, sizeof script, "%s/hpplite_store.py", svc);
        int fd = open(DIR "/paid_service.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execlp("python3", "python3", "-I", script, "--port", portStr, "--dir", DIR "/paid_pins", "--signer-key", DIR "/paid_signer.key",
               "--pay-to", payTo, "--facilitator", FACIL, "--rpc", RPC, "--asset", usdc, "--network", "eip155:31337",
               "--price-per-write", "1000", "--price-per-mib-month", "0", "--min-pay", "3000",
               "--pull-threshold", "2500", "--tick-interval", "0.5", (char *)NULL);
        _exit(127);
    }
    int up = 0;
    for (int i = 0; i < 50 && !up; i++) {
        char out[400];
        if (!test_child_alive(pid)) break;          /* the port was taken (#177) */
        snprintf(cmd, sizeof cmd, "curl -s -m 2 http://127.0.0.1:%d/status", port);
        up = run_cmd(cmd, out, sizeof out) == 0 && strstr(out, "\"signer\"") != NULL;
        if (!up) usleep(200 * 1000);
    }
    if (!up) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); FAIL("the store did not come up"); }
    setenv("HPPLITE_STORE_VALID_FOR", "315360000", 1);      /* anvil's clock is days ahead after the lease scenarios */
#define PD_FAIL(why) do { kill(pid, SIGTERM); waitpid(pid, NULL, 0); unsetenv("HPPLITE_STORE_VALID_FOR"); FAIL(why); } while (0)

    /* the permit: the node binds, gives the permit, writes; the store pulls */
    money_env("100000", NULL, NULL, NULL, NULL, NULL);
    snprintf(extra, sizeof extra, "&dbname=paidstore&store=http://127.0.0.1:%d", port);
    sqlite3 *w = open_db("pd_writer.db", KEY0, extra);
    if (!w) PD_FAIL("open writer");
    snprintf(funder, sizeof funder, "%s", status_text(w, "store_funder"));
    if (strlen(funder) != 42) { sqlite3_close(w); PD_FAIL("store_funder names the funding wallet"); }
    snprintf(cmd, sizeof cmd, "cast send --rpc-url %s --private-key %s %s 'transfer(address,uint256)' %s 50000 >/dev/null", RPC, KEY0, usdc, funder);
    if (system(cmd) != 0) { sqlite3_close(w); PD_FAIL("fund the funding wallet"); }
    sqlite3_exec(w, "CREATE TABLE p(id INTEGER PRIMARY KEY, v TEXT)", NULL, NULL, NULL);
    int ok1 = scalar(w, "SELECT hpplite_flush()") > 0;
    sqlite3_exec(w, "INSERT INTO p(v) VALUES('paid for')", NULL, NULL, NULL);
    int ok2 = scalar(w, "SELECT hpplite_flush()") > 0;
    if (!ok1 || !ok2) { printf("(%s) ", status_text(w, "last_error")); sqlite3_close(w); PD_FAIL("batches into the bucket"); }
    long long gained = 0;
    for (int i = 0; i < 60 && gained <= 0; i++) { usleep(250 * 1000); gained = anvil_usdc_of(usdc, payTo) - before; }
    char ethOut[80];
    snprintf(cmd, sizeof cmd, "cast balance %s --rpc-url %s", funder, RPC);
    run_cmd(cmd, ethOut, sizeof ethOut);
    long long spent = status_int(w, "store_spent"), balance = status_int(w, "store_balance");
    long long fundBal = anvil_usdc_of(usdc, funder);
    printf("(permit: the store pulled %lld units; funder %s holds %lld USDC.e, %ld wei; store_balance %lld, store_spent %lld, bucket %s) ",
           gained, funder, fundBal, first_number(ethOut), balance, spent, status_text(w, "store_bucket"));
    int okPermit = gained >= 2500 && first_number(ethOut) == 0 && balance == fundBal && spent == 0
                   && strcmp(status_text(w, "store_bucket"), "covered") == 0 && strcmp(status_text(w, "store_state"), "ok") == 0;
    sqlite3_close(w);
    if (!okPermit) PD_FAIL("the store should pull the writes' cost from the funding wallet, which holds no ETH; nothing paid by x402");

    /* HPPLITE_STORE_PERMIT=0: a fresh funding wallet (KEY9's) pays the 402s; HPPLITE_STORE_CAP below the top-up: nothing signed */
    money_env("10", "0", NULL, NULL, NULL, NULL);
    snprintf(extra, sizeof extra, "&dbname=paidx402&store=http://127.0.0.1:%d", port);
    sqlite3 *w2 = open_db("pd_x402.db", KEY9, extra);
    if (!w2) PD_FAIL("open the second writer");
    snprintf(funder, sizeof funder, "%s", status_text(w2, "store_funder"));
    snprintf(cmd, sizeof cmd, "cast send --rpc-url %s --private-key %s %s 'transfer(address,uint256)' %s 50000 >/dev/null", RPC, KEY0, usdc, funder);
    if (system(cmd) != 0) { sqlite3_close(w2); PD_FAIL("fund the second funding wallet"); }
    long long fundBefore = anvil_usdc_of(usdc, funder);
    sqlite3_exec(w2, "CREATE TABLE q(v); INSERT INTO q VALUES('over the cap')", NULL, NULL, NULL);
    long long h = scalar(w2, "SELECT hpplite_flush()");
    char err[300];
    snprintf(err, sizeof err, "%s", status_text(w2, "last_error"));
    int refused = h == 0 && strstr(err, "STORE_CAP") != NULL && scalar(w2, "SELECT pending FROM hpplite_status") > 0
                  && strcmp(status_text(w2, "store_state"), "over cap") == 0 && anvil_usdc_of(usdc, funder) == fundBefore;
    printf("(HPPLITE_STORE_PERMIT=0, cap 10: flush %lld, '%.60s', store_state %s) ", h, err, status_text(w2, "store_state"));
    sqlite3_close(w2);
    if (!refused) PD_FAIL("a top-up over storecap should be refused before signing, the batch kept pending, nothing paid");
    money_env("100000", "0", NULL, NULL, NULL, NULL);
    snprintf(extra, sizeof extra, "&dbname=paidx402&store=http://127.0.0.1:%d", port);
    w2 = open_db("pd_x402.db", KEY9, extra);
    if (!w2) PD_FAIL("reopen the second writer");
    h = scalar(w2, "SELECT hpplite_flush()");
    spent = status_int(w2, "store_spent");
    long long paid = fundBefore - anvil_usdc_of(usdc, funder);
    printf("(cap 100000: flush %lld, store_spent %lld, the funding wallet paid %lld) ", h, spent, paid);
    int okX402 = h > 0 && spent >= 3000 && spent == paid;
    sqlite3_close(w2);
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    unsetenv("HPPLITE_STORE_VALID_FOR");
#undef PD_FAIL
    if (!okX402) FAIL("under the cap the 402s are paid by x402 from the funding wallet, store_spent what it paid");
    PASS();
}

/* 21. The store does not answer (#11): with storefallback on (the default)
** the chain carries the bytes meanwhile, as before the store, and a replica
** with no store rebuilds; with HPPLITE_STORE_FALLBACK=off the batch waits and says
** why. KEY4, its own named database; the store is a port nobody listens on. */
static void test_store_fallback(void) {
    TEST("the store does not answer (#11): the chain carries the bytes meanwhile; HPPLITE_STORE_FALLBACK=off keeps the batch waiting instead");
    char extra[200];
    snprintf(extra, sizeof extra, "&dbname=fallback&store=http://127.0.0.1:9");
    sqlite3 *w = open_db("fb_writer.db", KEY4, extra);
    if (!w) FAIL("open writer");
    sqlite3_exec(w, "CREATE TABLE fb(id INTEGER PRIMARY KEY, v TEXT); INSERT INTO fb(v) VALUES('carried by the chain')", NULL, NULL, NULL);
    long long h = scalar(w, "SELECT hpplite_flush()");
    if (h != 1) { printf("(flush %lld, '%s') ", h, status_text(w, "last_error")); sqlite3_close(w); FAIL("the chain should have carried the batch"); }
    {
        char contract[80];
        snprintf(contract, sizeof contract, "%s", status_text(w, "contract"));
        HppliteL1 *peek = hpplite_l1_connect(RPC, contract);
        size_t n = 0;
        uint8_t *bytes = peek ? hpplite_l1_get_batch(peek, 1, &n) : NULL;
        int onChain = bytes != NULL && n > 0;
        free(bytes);
        hpplite_l1_disconnect(peek);
        if (!onChain) { sqlite3_close(w); FAIL("the bytes should be on the chain"); }
    }
    sqlite3 *r = open_db("fb_replica.db", KEY4, "&dbname=fallback&role=verify");
    if (!r) { sqlite3_close(w); FAIL("open replica"); }
    if (scalar(r, "SELECT count(*) FROM fb") != 1 || strcmp(status_text(r, "state"), "verify") != 0) {
        printf("(replica state %s, '%s') ", status_text(r, "state"), status_text(r, "last_error"));
        sqlite3_close(r); sqlite3_close(w);
        FAIL("a replica without a store should rebuild what the chain carries");
    }
    sqlite3_close(r);
    sqlite3_close(w);

    money_env(NULL, NULL, NULL, NULL, NULL, "off");
    snprintf(extra, sizeof extra, "&dbname=fallback&store=http://127.0.0.1:9");
    sqlite3 *w2 = open_db("fb_writer.db", KEY4, extra);
    if (!w2) FAIL("reopen writer");
    sqlite3_exec(w2, "INSERT INTO fb(v) VALUES('waits')", NULL, NULL, NULL);
    long long h2 = scalar(w2, "SELECT hpplite_flush()");
    const char *err = status_text(w2, "last_error");
    int waited = h2 == 0 && strstr(err, "store") != NULL && scalar(w2, "SELECT pending FROM hpplite_status") > 0;
    printf("(fallback off: flush %lld, '%.50s') ", h2, err);
    sqlite3_close(w2);
    if (!waited) FAIL("with HPPLITE_STORE_FALLBACK=off the batch should wait with the store's reason");
    PASS();
}

/* 22. The key given after open (#17): opened with the factory and no key,
** the node is disconnected and keeps the log; hpplite_key() connects it,
** resolves its rollup on the factory, takes the lease and posts. KEY0, a
** named database of its own. */
static void test_key_after_open(void) {
    TEST("a key given after open (#17): disconnected and keeping the log until hpplite_key(); then the rollup resolved, the lease taken, the batch posted");
    char uri[1024];
    sqlite3 *w = NULL;
    snprintf(uri, sizeof uri, "file:" DIR "/rtkey.db?hpplite=on&rpc=%s&factory=%s&chainid=31337&dbname=runtimekey&datadir=" DIR "&interval=0", RPC, FACTORY);
    if (sqlite3_open_v2(uri, &w, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL) != SQLITE_OK) { sqlite3_close(w); FAIL("open without a key"); }
    sqlite3_exec(w, "CREATE TABLE k(x); INSERT INTO k VALUES('kept')", NULL, NULL, NULL);
    long long h0 = scalar(w, "SELECT hpplite_flush()");
    char state0[40];
    snprintf(state0, sizeof state0, "%s", status_text(w, "state"));
    long long pending0 = scalar(w, "SELECT pending FROM hpplite_status");
    char sql[200];
    snprintf(sql, sizeof sql, "SELECT hpplite_key('%s')", KEY0);
    long long keyed = scalar(w, sql);
    long long h1 = scalar(w, "SELECT hpplite_flush()");
    printf("(before: flush %lld, state %s, pending %lld; hpplite_key %lld; after: flush %lld, state %s, lease %lld, contract %.10s...) ",
           h0, state0, pending0, keyed, h1, status_text(w, "state"), (long long)scalar(w, "SELECT lease FROM hpplite_status"), status_text(w, "contract"));
    int ok = h0 == 0 && strcmp(state0, "disconnected") == 0 && pending0 == 2 && keyed == 1 && h1 == 1
             && strcmp(status_text(w, "state"), "sequencing") == 0 && scalar(w, "SELECT lease FROM hpplite_status") == 1
             && status_text(w, "contract")[0] != 0;
    sqlite3_close(w);
    if (!ok) FAIL("the key after open should connect, resolve, take the lease and post");
    PASS();
}

/* 23. A signer whose key lives elsewhere (#17): the test's signer holds KEY0
** and counts its calls; the node opened on the rollup's contract with no key
** takes the lease and posts through it, the batch key given apart. */
typedef struct { HppliteCrypto *c; HppliteKeypair kp; int calls; } AnvilSigner;
static int anvil_signer_sign(void *arg, const unsigned char hash[32], unsigned char sig[64], int *recid) {
    AnvilSigner *t = (AnvilSigner *)arg;
    HppliteSignature s;
    t->calls++;
    if (hpplite_crypto_sign(t->c, &t->kp, hash, &s) != 0) return -1;
    memcpy(sig, s.sig, 64);
    *recid = s.recid;
    return 0;
}
static void test_signer_posts(void) {
    TEST("a signer instead of a key (#17): on the rollup's contract, the lease and the batch are signed through the callback, the batch sealed with a key given apart");
    char contract[80], uri[1024];
    sqlite3 *w = open_db("sg_owner.db", KEY0, "&dbname=signed");          /* the rollup exists, made with the key */
    if (!w) FAIL("open owner");
    snprintf(contract, sizeof contract, "%s", status_text(w, "contract"));
    sqlite3_close(w);                                                      /* its lease released */
    if (!contract[0]) FAIL("no contract");
    AnvilSigner ts = { hpplite_crypto_init(), {{0}}, 0 };
    unsigned char raw[32];
    HppliteSigner signer;
    hpplite_hex_to_bytes(KEY0, raw, 32);
    if (hpplite_crypto_keypair_from_privkey(ts.c, &ts.kp, raw) != 0) FAIL("keypair");
    signer.sign = anvil_signer_sign; signer.arg = &ts; memcpy(signer.address, ts.kp.address, 20);
    snprintf(uri, sizeof uri, "file:" DIR "/sg_signer.db?hpplite=on&rpc=%s&contract=%s&chainid=31337&datadir=" DIR "&interval=0", RPC, contract);
    sqlite3 *s = NULL;
    if (sqlite3_open_v2(uri, &s, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL) != SQLITE_OK) { sqlite3_close(s); FAIL("open on the contract without a key"); }
    HppliteNode *n = hpplite_get_node(s);
    if (!n || hpplite_node_set_signer(n, &signer) != 0) { sqlite3_close(s); FAIL("set signer"); }
    long long h0 = scalar(s, "SELECT hpplite_flush()");                   /* no batch key yet: nothing can be sealed or read */
    unsigned char batchKey[32]; char batchSql[120];
    hpplite_cipher_derive_key(raw, batchKey);                             /* the owner's derived batch key, given apart (#17) */
    snprintf(batchSql, sizeof batchSql, "SELECT hpplite_batch_key('0x"); hpplite_bytes_to_hex(batchKey, 32, batchSql + strlen(batchSql)); strcat(batchSql, "')");
    long long keyed = scalar(s, batchSql);
    sqlite3_exec(s, "CREATE TABLE sg(x); INSERT INTO sg VALUES('through the signer')", NULL, NULL, NULL);
    long long h1 = scalar(s, "SELECT hpplite_flush()");
    printf("(without the batch key: flush %lld; batch key %lld; then flush %lld, lease %lld, state %s, last_error '%s', signer calls %d) ",
           h0, keyed, h1, (long long)scalar(s, "SELECT lease FROM hpplite_status"), status_text(s, "state"), status_text(s, "last_error"), ts.calls);
    int ok = h0 == 0 && keyed == 1 && h1 >= 1 && scalar(s, "SELECT lease FROM hpplite_status") == 1 && ts.calls >= 2;
    sqlite3_close(s);
    hpplite_crypto_free(ts.c);
    if (!ok) FAIL("the signer should have signed the lease and the batch");
    PASS();
}

/* 24. The owner names the sequencer (#17): on the contract, KEY0 (owner,
** its own named database) hands the lease to KEY1; its own node can post no
** more, KEY1's node on the contract posts with the batch key handed over,
** a replica with KEY0 reads both. */
static void test_owner_names_sequencer(void) {
    TEST("the owner names a hot sequencer wallet (#17): its own node loses the lease, the hot wallet's node posts with the batch key handed over, a replica with the owner key reads both");
    char contract[80], uri[1024], sql[300], hotHex[48], batchHex[80];
    unsigned char raw[32], batchKey[32];
    HppliteCrypto *c = hpplite_crypto_init();
    HppliteKeypair hot;
    hpplite_hex_to_bytes(KEY1, raw, 32);
    if (hpplite_crypto_keypair_from_privkey(c, &hot, raw) != 0) FAIL("keypair");
    hotHex[0] = '0'; hotHex[1] = 'x'; hpplite_bytes_to_hex(hot.address, 20, hotHex + 2);
    hpplite_hex_to_bytes(KEY0, raw, 32);
    hpplite_cipher_derive_key(raw, batchKey);
    batchHex[0] = '0'; batchHex[1] = 'x'; hpplite_bytes_to_hex(batchKey, 32, batchHex + 2);
    sqlite3 *w = open_db("rot_owner.db", KEY0, "&dbname=rotated");
    if (!w) FAIL("open owner");
    sqlite3_exec(w, "CREATE TABLE rot(x); INSERT INTO rot VALUES('by the owner')", NULL, NULL, NULL);
    long long h1 = scalar(w, "SELECT hpplite_flush()");
    snprintf(contract, sizeof contract, "%s", status_text(w, "contract"));
    snprintf(sql, sizeof sql, "SELECT hpplite_set_sequencer('%s')", hotHex);
    int rotated = exec_sql(w, sql) == SQLITE_OK;
    sqlite3_exec(w, "INSERT INTO rot VALUES('after the rotation')", NULL, NULL, NULL);
    long long h2 = scalar(w, "SELECT hpplite_flush()");
    long long ownerLease = scalar(w, "SELECT lease FROM hpplite_status");
    printf("(owner after the rotation: state %s, last_error '%.90s') ", status_text(w, "state"), status_text(w, "last_error"));
    {
        HppliteL1 *peek = hpplite_l1_connect(RPC, contract);
        HppliteL1State *st = peek ? hpplite_l1_get_state(peek) : NULL;
        if (st) printf("(chain: leaseExpiry %llu, now %llu, instance %s) ", (unsigned long long)st->leaseExpiry, (unsigned long long)st->currentTimestamp,
                       memcmp(st->sequencerInstance, hpplite_get_node(w)->instanceId, 32) == 0 ? "ours" : "other");
        hpplite_l1_state_free(st);
        hpplite_l1_disconnect(peek);
    }
    sqlite3_close(w);
    snprintf(uri, sizeof uri, "file:" DIR "/rot_seq.db?hpplite=on&rpc=%s&contract=%s&chainid=31337&privkey=%s&datadir=" DIR "&interval=0", RPC, contract, KEY1);
    sqlite3 *s = NULL;
    if (sqlite3_open_v2(uri, &s, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL) != SQLITE_OK) { sqlite3_close(s); FAIL("open the sequencer's node"); }
    snprintf(sql, sizeof sql, "SELECT hpplite_batch_key('%s')", batchHex);
    long long keyed = scalar(s, sql);
    long long synced = scalar(s, "SELECT hpplite_sync()");
    long long rowsSeq = scalar(s, "SELECT count(*) FROM rot");
    sqlite3_exec(s, "INSERT INTO rot VALUES('by the sequencer')", NULL, NULL, NULL);
    long long h3 = scalar(s, "SELECT hpplite_flush()");
    long long seqLease = scalar(s, "SELECT lease FROM hpplite_status");
    printf("(owner: batch %lld, rotated %d, then flush %lld lease %lld; sequencer: batch key %lld, sync %lld, rows %lld, flush %lld lease %lld) ",
           h1, rotated, h2, ownerLease, keyed, synced, rowsSeq, h3, seqLease);
    sqlite3_close(s);
    snprintf(uri, sizeof uri, "file:" DIR "/rot_replica.db?hpplite=on&rpc=%s&contract=%s&chainid=31337&privkey=%s&role=verify&datadir=" DIR "&interval=0", RPC, contract, KEY0);
    sqlite3 *r = NULL;
    long long rowsR = -1;
    if (sqlite3_open_v2(uri, &r, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL) == SQLITE_OK) rowsR = scalar(r, "SELECT count(*) FROM rot");
    sqlite3_close(r);
    hpplite_crypto_free(c);
    if (h1 != 1 || !rotated) FAIL("the owner should post batch 1 and name the sequencer");
    if (h2 != 0 || ownerLease != 0) FAIL("after the rotation the owner's node should post no more");
    if (keyed != 1 || rowsSeq != 1 || h3 != 2 || seqLease != 1) FAIL("the sequencer's node should rebuild batch 1 with the batch key, take the lease and post batch 2");
    if (rowsR != 2) FAILF("a replica with the owner key should read both batches, got %lld rows", rowsR);
    PASS();
}

/* 25. A reader with a viewing key (#15): the writer grants its public key,
** posts; a replica opened on the contract with viewkey= alone, no wallet,
** rebuilds and equals the writer; a key never granted stops at batch 1. */
static void test_viewing_key_reads(void) {
    TEST("a viewing key (#15): granted by the writer, a replica with viewkey= alone rebuilds from the chain; a key never granted is refused");
    char contract[80], uri[1024], sql[200], pubHex[70];
    unsigned char priv[32], pub[33];
    const char *keyR = KEY5, *keyX = KEY6;
    hpplite_hex_to_bytes(keyR, priv, 32);
    if (hpplite_pk_pubkey(priv, pub) != 0) FAIL("pubkey");
    pubHex[0] = '0'; pubHex[1] = 'x'; hpplite_bytes_to_hex(pub, 33, pubHex + 2);
    sqlite3 *w = open_db("vk_writer.db", KEY0, "&dbname=viewing");
    if (!w) FAIL("open writer");
    snprintf(sql, sizeof sql, "SELECT hpplite_grant('%s')", pubHex);
    if (scalar(w, sql) != 1) { sqlite3_close(w); FAIL("grant"); }
    sqlite3_exec(w, "CREATE TABLE vk(x); INSERT INTO vk VALUES('for the reader')", NULL, NULL, NULL);
    long long h1 = scalar(w, "SELECT hpplite_flush()");
    sqlite3_exec(w, "INSERT INTO vk VALUES('and this')", NULL, NULL, NULL);
    long long h2 = scalar(w, "SELECT hpplite_flush()");
    snprintf(contract, sizeof contract, "%s", status_text(w, "contract"));
    long long readers = status_int(w, "readers");
    sqlite3_close(w);
    snprintf(uri, sizeof uri, "file:" DIR "/vk_reader.db?hpplite=on&rpc=%s&contract=%s&chainid=31337&viewkey=%s&role=verify&datadir=" DIR "&interval=0", RPC, contract, keyR);
    sqlite3 *r = NULL;
    long long rows = -1;
    char state[40] = "";
    if (sqlite3_open_v2(uri, &r, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL) == SQLITE_OK) {
        rows = count_rows(r, "vk");
        snprintf(state, sizeof state, "%s", status_text(r, "state"));
    }
    sqlite3_close(r);
    snprintf(uri, sizeof uri, "file:" DIR "/vk_other.db?hpplite=on&rpc=%s&contract=%s&chainid=31337&viewkey=%s&role=verify&datadir=" DIR "&interval=0", RPC, contract, keyX);
    sqlite3 *x = NULL;
    long long rowsX = -1;
    char errX[200] = "";
    if (sqlite3_open_v2(uri, &x, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL) == SQLITE_OK) {
        rowsX = count_rows(x, "vk");
        snprintf(errX, sizeof errX, "%s", status_text(x, "last_error"));
    }
    sqlite3_close(x);
    /* hpplite-verify with the viewing key alone: the same rebuild, the tool's verdict */
    char params[600], vout[4096] = "";
    int vrc = -2;
    snprintf(params, sizeof params, "rpc=%s&contract=%s&chainid=31337&viewkey=%s", RPC, contract, keyR);
    if (getenv("HPPLITE_VERIFY_BIN")) vrc = run_verify_with(params, "", vout, sizeof vout);
    printf("(writer: batches %lld %lld, readers %lld; reader: rows %lld, state %s; other key: rows %lld, '%.60s'; hpplite-verify with the viewing key: exit %d) ",
           h1, h2, readers, rows, state, rowsX, errX, vrc);
    if (h1 != 1 || h2 != 2 || readers != 1) FAIL("the writer posts two batches with one reader");
    if (rows != 2) FAIL("the reader with viewkey= alone should rebuild both batches");
    if (rowsX != -1 || !strstr(errX, "sealed")) FAIL("a key never granted should be refused at batch 1");
    if (vrc != 0 || !strstr(vout, "2 batches on chain, 2 replayed") || !strstr(vout, "\nverified")) { printf("\n%s", vout); FAIL("hpplite-verify with viewkey= should rebuild and say verified"); }
    PASS();
}

/* 26. Page capture (#146) on the real contract: a rollup of its own beside
** the wallet's first (a named database), batches of pages on chain, the
** checkpoint anchored; a replica rebuilds the file page by page; the verify
** tool finds the history is pages by itself and verifies it */
static void test_page_mode_end_to_end(void) {
    char out[4096] = "", params[600];
    int rc;
    TEST("page capture on the real contract: batches of pages, checkpoint anchored, replica equal, hpplite-verify finds the page history by itself");
    if (!getenv("HPPLITE_VERIFY_BIN")) FAIL("HPPLITE_VERIFY_BIN not set by anvil_test.sh");
    sqlite3 *db = open_db("pg.db", KEY0, "&dbname=pages&checkpoint=1");
    if (!db) FAIL("open");
    HppliteNode *n = hpplite_get_node(db);
    if (!n || !n->l1 || !n->pm) { sqlite3_close(db); FAIL("no L1 or no page mode after open"); }
    sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT); INSERT INTO t(v) VALUES('one'), ('two')", NULL, NULL, NULL);
    if (scalar(db, "SELECT hpplite_flush()") < 1) { printf("(%s) ", status_text(db, "last_error")); sqlite3_close(db); FAIL("flush should post the pages"); }
    sqlite3_exec(db, "UPDATE t SET v = 'three' WHERE id = 2; INSERT INTO t(v) VALUES('four')", NULL, NULL, NULL);
    if (scalar(db, "SELECT hpplite_flush()") < 2) { sqlite3_close(db); FAIL("second flush"); }
    long long height = scalar(db, "SELECT height FROM hpplite_status"), cp = scalar(db, "SELECT last_checkpoint_height FROM hpplite_status");
    if (strcmp(status_text(db, "capture"), "pages") != 0) { sqlite3_close(db); FAIL("hpplite_status.capture is not pages"); }
    if (cp < 1) { sqlite3_close(db); FAILF("checkpoint not anchored (last_checkpoint_height %lld, height %lld)", cp, height); }
    unsigned char rootW[32]; hpplite_node_get_state_root(n, rootW);
    /* a replica: the file written page by page from the chain */
    sqlite3 *r = open_db("pg_r.db", KEY0, "&dbname=pages&role=verify");
    if (!r) { sqlite3_close(db); FAIL("open the replica"); }
    HppliteNode *rn = hpplite_get_node(r);
    long long rows = scalar(r, "SELECT count(*) FROM t"), three = scalar(r, "SELECT count(*) FROM t WHERE v = 'three'"), mism = scalar(r, "SELECT root_mismatches FROM hpplite_status");
    unsigned char rootR[32]; if (rn) hpplite_node_get_state_root(rn, rootR);
    int same = rn && memcmp(rootW, rootR, 32) == 0;
    sqlite3_close(r);
    if (rows != 3 || three != 1 || mism != 0 || !same) { sqlite3_close(db); FAILF("replica: %lld rows, %lld 'three', %lld mismatches, roots %s", rows, three, mism, same ? "equal" : "differ"); }
    /* the verify tool, told nothing about pages: it finds out from the first batch */
    snprintf(params, sizeof params, "rpc=%s&factory=%s&chainid=31337&privkey=%s&dbname=pages", RPC, FACTORY, KEY0);
    rc = run_verify_with(params, NULL, out, sizeof out);
    sqlite3_close(db);
    if (rc != 0 || !strstr(out, "matches the rebuilt content") || !strstr(out, "\nverified")) FAILF("hpplite-verify on the page history: exit %d, output:\n%s", rc, out);
    PASS();
}

/* 27. A wallet that holds USDC.e and no ETH at all (#126): its rollup is
** created, its lease taken, its batches anchored and the lease released,
** each through the reference relay (operators/relay, #170) as an EIP-712
** signed form paid over x402; the batches at a pin service. A replica
** rebuilds; the wallet's ETH balance is 0 throughout. */
static long eth_balance_of(const char *addr) {
    char cmd[300], out[80];
    snprintf(cmd, sizeof cmd, "cast balance %s --rpc-url %s", addr, RPC);
    return run_cmd(cmd, out, sizeof out) == 0 ? first_number(out) : -1;
}
/* the reference relay (#170) on port, paying with KEY9; -1 when it did not come up */
static pid_t start_relay(const char *svc, const char *usdc, int port) {
    char cmd[300], out[400], script[600];
    FILE *f = fopen(DIR "/relay.key", "w");
    if (!f) return -1;
    fprintf(f, "%s\n", KEY9);
    fclose(f);
    snprintf(script, sizeof script, "%s/../relay/relay.py", svc);
    pid_t pid = fork();
    if (pid == 0) {
        char portStr[16];
        snprintf(portStr, sizeof portStr, "%d", port);
        int fd = open(DIR "/relay.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execlp("python3", "python3", "-I", script, "--port", portStr, "--rpc", RPC, "--key-file", DIR "/relay.key",
               "--factory", FACTORY, "--facilitator", FACIL, "--asset", usdc, "--eth-usd", "30", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 50; i++) {
        if (!test_child_alive(pid)) return -1;
        snprintf(cmd, sizeof cmd, "curl -s -m 2 http://127.0.0.1:%d/status", port);
        if (run_cmd(cmd, out, sizeof out) == 0 && strstr(out, "hpplite-relay")) return pid;
        usleep(200 * 1000);
    }
    kill(pid, SIGKILL); waitpid(pid, NULL, 0);
    return -1;
}
static void test_relay_no_eth(void) {
    TEST("a wallet with USDC.e and no ETH (#126): its rollup, lease and anchors through the relay, paid in USDC.e; a replica rebuilds; its ETH stays 0");
    const char *svc = getenv("HPPLITE_PIN_SERVICE"), *usdc = getenv("HPPLITE_ANVIL_USDC");
    static const char ZKEY[] = "0x7777777777777777777777777777777777777777777777777777777777777777";
    if (!svc || !usdc || !usdc[0]) { printf("SKIP (no pin service or MockUSDC in the environment) "); PASS(); return; }
    char cmd[2400], wallet[80], out[400], extra[400];
    int sport = test_free_port(), rport = test_free_port();
    snprintf(cmd, sizeof cmd, "cast wallet address %s", ZKEY);
    if (run_cmd(cmd, wallet, sizeof wallet) != 0) FAIL("cast wallet address");
    wallet[strcspn(wallet, "\r\n")] = 0;
    /* anvil's base fee climbs over the suite's hundreds of transactions: the relay's quotes with it (#126) */
    snprintf(cmd, sizeof cmd, "cast send --rpc-url %s --private-key %s %s 'transfer(address,uint256)' %s 1000000000 >/dev/null", RPC, KEY0, usdc, wallet);
    if (run_cmd(cmd, out, sizeof out) != 0) FAIL("fund the wallet with MockUSDC");
    if (eth_balance_of(wallet) != 0) FAIL("the wallet should start with no ETH");
    pid_t store = start_pin_service_free(svc, sport);
    if (store < 0) FAIL("the pin service did not come up");
    pid_t relay = start_relay(svc, usdc, rport);
#define STOP_SERVICES() do { if (relay > 0) { kill(relay, SIGTERM); waitpid(relay, NULL, 0); } kill(store, SIGTERM); waitpid(store, NULL, 0); unsetenv("HPPLITE_STORE_VALID_FOR"); } while (0)
    if (relay < 0) { STOP_SERVICES(); FAIL("the relay did not come up (see relay.log)"); }
    setenv("HPPLITE_STORE_VALID_FOR", "315360000", 1);      /* anvil's clock is days ahead after the lease scenarios */
    money_env(NULL, NULL, NULL, NULL, "200000000", NULL);
    snprintf(extra, sizeof extra, "&store=http://127.0.0.1:%d&relay=http://127.0.0.1:%d&anchor=0", sport, rport);
    sqlite3 *w = open_db("nz_writer.db", ZKEY, extra);
    if (!w) { STOP_SERVICES(); FAIL("open writer"); }
    char contract[80];
    snprintf(contract, sizeof contract, "%s", status_text(w, "contract"));
    if (!contract[0] || status_int(w, "lease") != 1) {
        printf("(contract '%s', lease %lld, %s) ", contract, status_int(w, "lease"), status_text(w, "last_error"));
        sqlite3_close(w); STOP_SERVICES(); FAIL("the rollup created and the lease taken through the relay");
    }
    sqlite3_exec(w, "CREATE TABLE z(id INTEGER PRIMARY KEY, v TEXT)", NULL, NULL, NULL);
    int ok1 = scalar(w, "SELECT hpplite_flush()") > 0;
    sqlite3_exec(w, "INSERT INTO z(v) VALUES('no eth'), ('only usdc.e')", NULL, NULL, NULL);
    int ok2 = scalar(w, "SELECT hpplite_flush()") > 0;
    long long anchored = scalar(w, "SELECT hpplite_anchor()");
    char path[20];
    snprintf(path, sizeof path, "%s", status_text(w, "last_tx_path"));
    long long spent = status_int(w, "relay_spent");
    printf("(contract %.10s, anchored %lld via %s, relay_spent %lld units) ", contract, anchored, path, spent);
    if (!ok1 || !ok2 || anchored <= 0 || strcmp(path, "relay") != 0 || spent <= 0) {
        printf("(%s) ", status_text(w, "last_error")); sqlite3_close(w); STOP_SERVICES();
        FAIL("batches at the store, the anchor through the relay, paid");
    }
    char replicaExtra[440];
    snprintf(replicaExtra, sizeof replicaExtra, "%s&role=verify", extra);
    const char *why = replica_equals_as(w, "nz_replica.db", ZKEY, replicaExtra);   /* an audit of the same wallet, through the same store */
    /* the relay down: the batch reaches the store, the anchor waits and says why (no ETH to send it itself);
    ** the relay back, the next anchor takes it */
    kill(relay, SIGTERM); waitpid(relay, NULL, 0); relay = -1;
    sqlite3_exec(w, "INSERT INTO z(v) VALUES('while the relay is down')", NULL, NULL, NULL);
    int ok3 = scalar(w, "SELECT hpplite_flush()") > 0;
    long long anchoredDown = scalar(w, "SELECT hpplite_anchor()"), afterDown = scalar(w, "SELECT anchored FROM hpplite_status");
    char errDown[300];
    snprintf(errDown, sizeof errDown, "%s", status_text(w, "last_error"));
    relay = start_relay(svc, usdc, rport);
    long long anchoredBack = relay > 0 ? scalar(w, "SELECT hpplite_anchor()") : -1;
    printf("(relay down: flush %d, anchor %lld, anchored %lld, '%.70s'; back: anchor %lld) ", ok3, anchoredDown, afterDown, errDown, anchoredBack);
    int downOk = ok3 && anchoredDown <= 0 && afterDown == anchored && strstr(errDown, "relay") != NULL && anchoredBack > anchored;
    sqlite3_close(w);                                                         /* the lease released through the relay */
    long ethAfter = eth_balance_of(wallet);
    snprintf(cmd, sizeof cmd, "cast call %s 'isLeaseActive()(bool)' --rpc-url %s", contract, RPC);
    run_cmd(cmd, out, sizeof out);
    int released = strncmp(out, "false", 5) == 0;
    STOP_SERVICES();
#undef STOP_SERVICES
    if (why) FAIL(why);
    if (!downOk) FAIL("with the relay down and no ETH the anchor should wait and say why, then go through once the relay is back");
    if (!released) FAIL("the close should release the lease through the relay");
    if (ethAfter != 0) FAILF("the wallet's ETH balance is %ld, want 0", ethAfter);
    PASS();
}

/* 28. A relay that does not answer, and a wallet with ETH (#126): every
** write goes from the wallet itself, as without a relay, and says so.
** KEY8, its own named database. */
static void test_relay_fallback(void) {
    TEST("relay= that does not answer, a wallet with ETH (#126): the writes go from the wallet itself; last_tx_path says direct; a replica rebuilds");
    char extra[200];
    int dead = test_free_port();                                  /* nobody listens there */
    money_env(NULL, NULL, NULL, NULL, "1000", NULL);
    snprintf(extra, sizeof extra, "&dbname=relayfallback&relay=http://127.0.0.1:%d", dead);
    sqlite3 *w = open_db("rf_writer.db", KEY8, extra);
    if (!w) FAIL("open writer");
    sqlite3_exec(w, "CREATE TABLE f(id INTEGER PRIMARY KEY, v TEXT); INSERT INTO f(v) VALUES('direct')", NULL, NULL, NULL);
    long long h = scalar(w, "SELECT hpplite_flush()");
    char path[20];
    snprintf(path, sizeof path, "%s", status_text(w, "last_tx_path"));
    printf("(flush %lld, last_tx_path %s, relay_spent %lld) ", h, path, status_int(w, "relay_spent"));
    const char *why = h > 0 ? replica_equals_as(w, "rf_replica.db", KEY8, "&dbname=relayfallback&role=verify") : "the batch did not post";
    sqlite3_close(w);
    if (why) FAIL(why);
    if (strcmp(path, "direct") != 0) FAIL("last_tx_path should say the wallet sent it");
    PASS();
}

/* helpers for the paid scenarios */
static long long cast_number(const char *cmd) {
    char out[200];
    if (run_cmd(cmd, out, sizeof out) != 0) return -1;
    return atoll(out);
}
static long long nonce_of(const char *addr) {
    char cmd[300];
    snprintf(cmd, sizeof cmd, "cast nonce %s --rpc-url %s", addr, RPC);
    return cast_number(cmd);
}
static long long usdc_balance(const char *usdc, const char *addr) {
    char cmd[400];
    snprintf(cmd, sizeof cmd, "cast call %s 'balanceOf(address)(uint256)' %s --rpc-url %s", usdc, addr, RPC);
    return cast_number(cmd);
}
/* 30. Spending over time (#173): HPPLITE_SPEND_CAP over a rolling window, the store
** and the relay together. Two puts at about 1,001 units fit 2500/hour; the
** third is refused before anything is signed and waits, saying spendcap;
** spent_window says what the window holds; the ledger is in the data
** directory, so a reopen does not reset it. A wallet with 5,000 units at
** that rate gets spend_warning (HPPLITE_LOW_BALANCE). */
static void test_spend_cap(void) {
    TEST("spending over time (#173): HPPLITE_SPEND_CAP refuses the put past the window's cap before signing, the batch waits; a reopen keeps the window; spend_warning names the balance that runs out");
    const char *svc = getenv("HPPLITE_PIN_SERVICE"), *usdc = getenv("HPPLITE_ANVIL_USDC");
    static const char SKEY[] = "0x3030303030303030303030303030303030303030303030303030303030303030";
    if (!svc || !usdc || !usdc[0]) { printf("SKIP (no pin service or MockUSDC in the environment) "); PASS(); return; }
    char cmd[2400], wallet[80], extra[400], err[300];
    int port = test_free_port();
    FILE *f = fopen(DIR "/spend_signer.key", "w");
    if (!f) FAIL("signer key file");
    fprintf(f, "%s\n", KEY3);
    fclose(f);
    snprintf(cmd, sizeof cmd, "cast wallet address %s", SKEY);
    if (run_cmd(cmd, wallet, sizeof wallet) != 0) FAIL("cast wallet address");
    wallet[strcspn(wallet, "\r\n")] = 0;
    snprintf(cmd, sizeof cmd, "cast send --rpc-url %s --private-key %s %s --value 10ether >/dev/null", RPC, KEY0, wallet);
    if (system(cmd) != 0) FAIL("fund the wallet");
    pid_t pid = fork();
    if (pid == 0) {
        char portStr[16], script[600];
        snprintf(portStr, sizeof portStr, "%d", port);
        snprintf(script, sizeof script, "%s/hpplite_store.py", svc);
        int fd = open(DIR "/spend_service.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execlp("python3", "python3", "-I", script, "--port", portStr, "--dir", DIR "/spend_pins", "--signer-key", DIR "/spend_signer.key",
               "--pay-to", "0x3030000000000000000000000000000000000030", "--facilitator", FACIL, "--rpc", RPC, "--asset", usdc,
               "--network", "eip155:31337", "--price-per-write", "1000", "--price-per-mib-month", "0", "--min-pay", "1000", (char *)NULL);
        _exit(127);
    }
    int up = 0;
    for (int i = 0; i < 50 && !up; i++) {
        char out[400];
        if (!test_child_alive(pid)) break;
        snprintf(cmd, sizeof cmd, "curl -s -m 2 http://127.0.0.1:%d/status", port);
        up = run_cmd(cmd, out, sizeof out) == 0 && strstr(out, "\"signer\"") != NULL;
        if (!up) usleep(200 * 1000);
    }
    if (!up) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); FAIL("the pin service did not come up"); }
    setenv("HPPLITE_STORE_VALID_FOR", "315360000", 1);      /* anvil's clock is days ahead after the lease scenarios */
    setenv("HPPLITE_SPEND_CHECK_SECONDS", "1", 1);          /* the warning's rate from a second of history, not five minutes */
#define SP_FAIL(why) do { kill(pid, SIGTERM); waitpid(pid, NULL, 0); unsetenv("HPPLITE_STORE_VALID_FOR"); unsetenv("HPPLITE_SPEND_CHECK_SECONDS"); FAIL(why); } while (0)
    /* HPPLITE_STORE_PERMIT=0: every write's 402 is a top-up of 1000 paid by x402 from the funding wallet, two per batch (its bytes, its name) */
    money_env("5000", "0", "4500/hour", "1000", NULL, NULL);
    snprintf(extra, sizeof extra, "&dbname=spendcap&store=http://127.0.0.1:%d", port);
    sqlite3 *w = open_db("spend_writer.db", SKEY, extra);
    if (!w) SP_FAIL("open writer");
    snprintf(cmd, sizeof cmd, "cast send --rpc-url %s --private-key %s %s 'transfer(address,uint256)' %s 5000 >/dev/null", RPC, KEY0, usdc, status_text(w, "store_funder"));
    if (system(cmd) != 0) { sqlite3_close(w); SP_FAIL("fund the funding wallet"); }
    sqlite3_exec(w, "CREATE TABLE s(id INTEGER PRIMARY KEY, v TEXT)", NULL, NULL, NULL);
    int ok1 = scalar(w, "SELECT hpplite_flush()") > 0;
    sqlite3_exec(w, "INSERT INTO s(v) VALUES('the second put')", NULL, NULL, NULL);
    int ok2 = scalar(w, "SELECT hpplite_flush()") > 0;
    if (!ok1 || !ok2) { printf("(%s) ", status_text(w, "last_error")); sqlite3_close(w); SP_FAIL("two puts within the cap"); }
    long long spent = status_int(w, "store_spent"), window = status_int(w, "spent_window");
    sqlite3_exec(w, "INSERT INTO s(v) VALUES('past the cap')", NULL, NULL, NULL);
    long long h = scalar(w, "SELECT hpplite_flush()");
    snprintf(err, sizeof err, "%s", status_text(w, "last_error"));
    int refused = h == 0 && strstr(err, "SPEND_CAP") && scalar(w, "SELECT pending FROM hpplite_status") > 0 && strcmp(status_text(w, "store_state"), "over cap") == 0;
    printf("(spent %lld, window %lld; third put: flush %lld, '%.80s') ", spent, window, h, err);
    if (spent != 4000 || window != spent || !refused) { sqlite3_close(w); SP_FAIL("the third batch should wait over spendcap, the window holding the two"); }
    usleep(1500 * 1000);
    char warn[300];
    snprintf(warn, sizeof warn, "%s", status_text(w, "spend_warning"));
    printf("(spend_warning '%s') ", warn);
    if (!strstr(warn, "USDC.e")) { sqlite3_close(w); SP_FAIL("5,000 units at this rate should warn"); }
    sqlite3_close(w);
    w = open_db("spend_writer.db", SKEY, extra);
    if (!w) SP_FAIL("reopen writer");
    sqlite3_exec(w, "INSERT INTO s(v) VALUES('after a reopen')", NULL, NULL, NULL);
    h = scalar(w, "SELECT hpplite_flush()");
    snprintf(err, sizeof err, "%s", status_text(w, "last_error"));
    long long window2 = status_int(w, "spent_window");
    sqlite3_close(w);
    printf("(after a reopen: flush %lld, window %lld) ", h, window2);
    if (h != 0 || !strstr(err, "SPEND_CAP") || window2 < spent) SP_FAIL("a reopen must not reset the window");
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    unsetenv("HPPLITE_STORE_VALID_FOR"); unsetenv("HPPLITE_SPEND_CHECK_SECONDS");
#undef SP_FAIL
    PASS();
}

/* 31. relaydeposit= is gone (#251): the relay's x402 batch-settlement channels were removed in v0.8 - the relay is paid
** per call by exact. A URI naming it refuses the open and says so; nothing is created */
static void test_relay_channel(void) {
    TEST("relaydeposit= refuses the open: the relay is paid per call (exact), channels were removed (#251)");
    char extra[200];
    snprintf(extra, sizeof extra, "&relay=http://127.0.0.1:%d&relaydeposit=500000000", test_free_port());
    sqlite3 *w = open_db("vr_writer.db", KEY0, extra);
    if (w) { sqlite3_close(w); FAIL("relaydeposit= should refuse the open"); }
    if (!hpplite_config_error() || !strstr(hpplite_config_error(), "relaydeposit")) { printf("(%s) ", hpplite_config_error() ? hpplite_config_error() : "no error"); FAIL("the refusal should name relaydeposit"); }
    PASS();
}

/* 32. hpplite-verify --to (#206): the database as of a height - a rollup with a checkpoint every two
** batches and one row per batch, rebuilt to 2 (a checkpoint) and to 3 (between two: the chain's checkpoint at 4
** is past it, not compared), each with its own posted root matched, kept with --keep and read back; --to past
** the chain is refused. KEY5. */
static void test_verify_to(void) {
    TEST("hpplite-verify --to <height> (#206): the database as of a checkpoint and between two, kept and read back; past the chain refused");
    if (!getenv("HPPLITE_VERIFY_BIN")) FAIL("HPPLITE_VERIFY_BIN not set by anvil_test.sh");
    sqlite3 *w = open_db("vt_writer.db", KEY5, "&dbname=verifyto&checkpoint=2");
    if (!w) FAIL("open writer");
    sqlite3_exec(w, "CREATE TABLE v(n INTEGER)", NULL, NULL, NULL);
    for (int i = 1; i <= 5; i++) {
        char sql[80];
        snprintf(sql, sizeof sql, "INSERT INTO v VALUES(%d)", i);
        sqlite3_exec(w, sql, NULL, NULL, NULL);
        if (scalar(w, "SELECT hpplite_flush()") <= 0) { printf("(%s) ", status_text(w, "last_error")); sqlite3_close(w); FAIL("a batch"); }
    }
    sqlite3_close(w);
    char params[400], out[4096], extra[300];
    snprintf(params, sizeof params, "rpc=%s&factory=%s&chainid=31337&privkey=%s&dbname=verifyto", RPC, FACTORY, KEY5);
    static const struct { int to; const char *says; } cases[] = {
        { 2, "the database as of batch 2: state root" }, { 3, "on-chain checkpoint at batch 4: past --to 3, not compared" } };
    for (int k = 0; k < 2; k++) {
        char dir[200];
        snprintf(dir, sizeof dir, DIR "/vt_keep%d", cases[k].to);
        snprintf(extra, sizeof extra, "--to %d --keep %s", cases[k].to, dir);
        out[0] = 0;
        int rc = run_verify_with(params, extra, out, sizeof out);
        char path[300];
        snprintf(path, sizeof path, "%s/rebuilt.db", dir);
        sqlite3 *kept = NULL;
        long long rows = -1;
        if (sqlite3_open_v2(path, &kept, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) rows = scalar(kept, "SELECT count(*) FROM v");
        sqlite3_close(kept);
        printf("(--to %d: exit %d, rows %lld) ", cases[k].to, rc, rows);
        if (rc != 0 || !strstr(out, cases[k].says) || !strstr(out, "posted at that height and matched") || !strstr(out, "\nverified")
            || rows != cases[k].to) FAILF("--to %d:\n%s", cases[k].to, out);
    }
    out[0] = 0;
    int rc = run_verify_with(params, "--to 9", out, sizeof out);
    if (rc != 2 || !strstr(out, "past the chain's height")) FAILF("--to past the chain: exit %d\n%s", rc, out);
    PASS();
}

/* 33. Every batch sealed to the owner too (#209): the owner (KEY8) creates a rollup and names another wallet
** (KEY9) its sequencer without handing over the batch key; that node, told the owner's public key (ownerkey=),
** writes two batches; the owner rebuilds them with its own key alone - a recovery key no node holds. */
static void test_sealed_to_owner(void) {
    TEST("every batch sealed to the owner too (#209): a sequencer named without the batch key writes; the owner reads it all with its own key");
    char contract[80], uri[1200], sql[200], seqHex[48], ownerPubHex[70];
    unsigned char raw[32], pub[33];
    HppliteCrypto *c = hpplite_crypto_init();
    HppliteKeypair seq;
    hpplite_hex_to_bytes(KEY9, raw, 32);
    if (hpplite_crypto_keypair_from_privkey(c, &seq, raw) != 0) FAIL("keypair");
    seqHex[0] = '0'; seqHex[1] = 'x'; hpplite_bytes_to_hex(seq.address, 20, seqHex + 2);
    hpplite_hex_to_bytes(KEY8, raw, 32);
    if (hpplite_pk_pubkey(raw, pub) != 0) FAIL("the owner's pubkey");
    ownerPubHex[0] = '0'; ownerPubHex[1] = 'x'; hpplite_bytes_to_hex(pub, 33, ownerPubHex + 2);
    hpplite_crypto_free(c);
    sqlite3 *w = open_db("so_owner.db", KEY8, "&dbname=sealowner");
    if (!w) FAIL("open the owner's node");
    snprintf(contract, sizeof contract, "%s", status_text(w, "contract"));
    snprintf(sql, sizeof sql, "SELECT hpplite_set_sequencer('%s')", seqHex);
    int rotated = exec_sql(w, sql) == SQLITE_OK;
    sqlite3_close(w);
    if (!rotated) FAIL("the owner names the sequencer");
    snprintf(uri, sizeof uri, "file:" DIR "/so_seq.db?hpplite=on&rpc=%s&contract=%s&chainid=31337&privkey=%s&ownerkey=%s&datadir=" DIR "&interval=0",
             RPC, contract, KEY9, ownerPubHex);
    sqlite3 *s = NULL;
    if (sqlite3_open_v2(uri, &s, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL) != SQLITE_OK) { sqlite3_close(s); FAIL("open the sequencer's node"); }
    sqlite3_exec(s, "CREATE TABLE so(x); INSERT INTO so VALUES('one')", NULL, NULL, NULL);
    long long h1 = scalar(s, "SELECT hpplite_flush()");
    sqlite3_exec(s, "INSERT INTO so VALUES('two')", NULL, NULL, NULL);
    long long h2 = scalar(s, "SELECT hpplite_flush()");
    printf("(sequencer: flush %lld, %lld; %s) ", h1, h2, status_text(s, "last_error"));
    sqlite3_close(s);
    if (h1 != 1 || h2 != 2) FAIL("the named sequencer writes two batches");
    snprintf(uri, sizeof uri, "file:" DIR "/so_replica.db?hpplite=on&rpc=%s&contract=%s&chainid=31337&privkey=%s&role=verify&datadir=" DIR "&interval=0", RPC, contract, KEY8);
    sqlite3 *r = NULL;
    long long rows = -1;
    char err[200] = "";
    if (sqlite3_open_v2(uri, &r, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL) == SQLITE_OK) {
        rows = scalar(r, "SELECT count(*) FROM so");
        snprintf(err, sizeof err, "%s", status_text(r, "last_error"));
    }
    sqlite3_close(r);
    printf("(the owner's rebuild: rows %lld, '%.80s') ", rows, err);
    if (rows != 2) FAIL("the owner, with its own key alone, reads every batch the sequencer wrote");
    PASS();
}

/* 34. Point-in-time recovery (#205): hpplite-verify --at <time> rebuilds what the writer had written by then, by the
** time each batch carries - three batches two seconds apart, the database as of a moment between the second and the
** third (kept, read back: two rows), as of before the first (nothing), and in ISO form. KEY6. */
static void test_verify_at(void) {
    TEST("hpplite-verify --at <time> (#205): the database as of a time, by the writer's time in each batch; before the first, nothing");
    if (!getenv("HPPLITE_VERIFY_BIN")) FAIL("HPPLITE_VERIFY_BIN not set by anvil_test.sh");
    sqlite3 *w = open_db("va_writer.db", KEY6, "&dbname=verifyat");
    if (!w) FAIL("open writer");
    sqlite3_exec(w, "CREATE TABLE a(n INTEGER)", NULL, NULL, NULL);
    long long t0 = (long long)time(NULL), between = 0;
    for (int i = 1; i <= 3; i++) {
        char sql[80];
        snprintf(sql, sizeof sql, "INSERT INTO a VALUES(%d)", i);
        sqlite3_exec(w, sql, NULL, NULL, NULL);
        if (scalar(w, "SELECT hpplite_flush()") <= 0) { printf("(%s) ", status_text(w, "last_error")); sqlite3_close(w); FAIL("a batch"); }
        if (i == 2) { sleep(2); between = (long long)time(NULL); sleep(2); }
        else if (i == 1) sleep(2);
    }
    sqlite3_close(w);
    char params[400], out[4096], extra[300], dir[200], path[300];
    snprintf(params, sizeof params, "rpc=%s&factory=%s&chainid=31337&privkey=%s&dbname=verifyat", RPC, FACTORY, KEY6);
    snprintf(dir, sizeof dir, DIR "/va_keep");
    snprintf(extra, sizeof extra, "--at %lld --keep %s", between, dir);
    out[0] = 0;
    int rc = run_verify_with(params, extra, out, sizeof out);
    snprintf(path, sizeof path, "%s/rebuilt.db", dir);
    sqlite3 *kept = NULL;
    long long rows = -1;
    if (sqlite3_open_v2(path, &kept, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) rows = scalar(kept, "SELECT count(*) FROM a");
    sqlite3_close(kept);
    printf("(--at between the second and third: exit %d, rows %lld) ", rc, rows);
    if (rc != 0 || rows != 2 || !strstr(out, "through batch 2, written") || !strstr(out, "\nverified")) FAILF("--at %lld:\n%s", between, out);
    snprintf(extra, sizeof extra, "--at %lld", t0 - 3600);
    out[0] = 0;
    rc = run_verify_with(params, extra, out, sizeof out);
    if (rc != 0 || !strstr(out, "no batch written by then")) FAILF("--at an hour before:\n%s", out);
    {
        char iso[40];
        time_t t = (time_t)between;
        strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
        snprintf(extra, sizeof extra, "--at %s", iso);
        out[0] = 0;
        rc = run_verify_with(params, extra, out, sizeof out);
        if (rc != 0 || !strstr(out, "through batch 2, written")) FAILF("--at %s:\n%s", iso, out);
    }
    PASS();
}

/* 35. Snapshots and pruning on a chain (#203: #221-#224): a page-mode sequencer at the reference store
** (reading the rollup's sequencer from the chain) makes a snapshot at each anchor and keeps 3 s of history;
** the store takes its Prune (the sequencer's signature, checked against the chain); hpplite-verify cannot read
** batch 1, starts from the oldest snapshot still kept - its root the one the chain anchored - and verifies the
** rest; --to before it is refused with the reason. KEY4, its own named database. */
static void test_snapshots_on_chain(void) {
    TEST("snapshots and keep= on a chain (#221-#224): pruned at the store by the sequencer's Prune; hpplite-verify verifies from the oldest snapshot kept; --to before it cannot be met");
    const char *svc = getenv("HPPLITE_PIN_SERVICE");
    if (!svc) { printf("SKIP (no pin service in the environment) "); PASS(); return; }
    if (!getenv("HPPLITE_VERIFY_BIN")) FAIL("HPPLITE_VERIFY_BIN not set by anvil_test.sh");
    int port = test_free_port();
    pid_t pid = start_pin_service_free(svc, port);
    if (pid < 0) FAIL("the store did not come up");
    char extra[300], params[500], out[8192], da[43], cmd[200], bucket[2000] = "";
    snprintf(extra, sizeof extra, "&dbname=snapshots&store=http://127.0.0.1:%d&anchor=0&snapshot=1ms&keep=3s", port);
    sqlite3 *w = open_db("sn_writer.db", KEY4, extra);
    if (!w) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("open writer"); }
    sqlite3_exec(w, "CREATE TABLE n(id INTEGER PRIMARY KEY, v TEXT)", NULL, NULL, NULL);
    static const int ends[] = { 3, 5, 6 };
    int b = 0;
    for (int k = 0; k < 3; k++) {
        if (k) sleep(4);                           /* keep=3s, by whole seconds */
        while (b < ends[k]) {
            sqlite3_exec(w, "INSERT INTO n(v) VALUES(hex(randomblob(500)))", NULL, NULL, NULL);
            if (scalar(w, "SELECT hpplite_flush()") <= 0) { printf("(%s) ", status_text(w, "last_error")); sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("a batch through the store"); }
            b++;
        }
        if (scalar(w, "SELECT hpplite_anchor()") != ends[k] || scalar(w, "SELECT snapshot_height FROM hpplite_status") != ends[k]) {
            printf("(anchor %d: %s) ", ends[k], status_text(w, "last_error")); sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("an anchor with a snapshot at its height");
        }
    }
    long long rows = scalar(w, "SELECT count(*) FROM n");
    snprintf(da, sizeof da, "%s", status_text(w, "da_contract"));
    for (char *p = da; *p; p++) if (*p >= 'A' && *p <= 'F') *p = (char)(*p - 'A' + 'a');
    sqlite3_close(w);
    snprintf(cmd, sizeof cmd, "curl -s -m 5 http://127.0.0.1:%d/bucket/%s", port, da);
    run_cmd(cmd, bucket, sizeof bucket);
    if (!strstr(bucket, "\"pruneHeight\": 5") && !strstr(bucket, "\"pruneHeight\":5")) { printf("(%s) ", bucket); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("the store took the Prune at the snapshot at 5"); }
    snprintf(params, sizeof params, "rpc=%s&factory=%s&chainid=31337&privkey=%s&dbname=snapshots&store=http://127.0.0.1:%d", RPC, FACTORY, KEY4, port);
    out[0] = 0;
    int rc = run_verify_with(params, "--keep " DIR "/sn_keep", out, sizeof out);
    sqlite3 *kept = NULL;
    long long keptRows = -1;
    if (sqlite3_open_v2(DIR "/sn_keep/rebuilt.db", &kept, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) keptRows = scalar(kept, "SELECT count(*) FROM n");
    sqlite3_close(kept);
    printf("(rows %lld, rebuilt %lld) ", rows, keptRows);
    if (rc != 0 || !strstr(out, "verified from the snapshot at 5") || !strstr(out, "\nverified") || keptRows != rows) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAILF("hpplite-verify from the oldest snapshot kept: exit %d\n%s", rc, out); }
    out[0] = 0;
    rc = run_verify_with(params, "--to 2", out, sizeof out);
    if (rc == 0 || !strstr(out, "cannot be rebuilt")) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAILF("--to 2, below the snapshot kept: exit %d\n%s", rc, out); }
    {
        /* from= is gone (#255; was #239's override): given, the tool cannot open and says so */
        char fromGenesis[600];
        snprintf(fromGenesis, sizeof fromGenesis, "%s&from=genesis", params);
        out[0] = 0;
        rc = run_verify_with(fromGenesis, "", out, sizeof out);
        if (rc != 2 || !strstr(out, "from= is gone")) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAILF("from=genesis given: the tool must refuse to open (exit 2) and say from= is gone; exit %d\n%s", rc, out); }
    }
    {
        /* a new verifier of the named rollup: found by the name its snapshot carries (#226); batch 1 is pruned, so it
        ** starts from the oldest snapshot kept, the one the tool verified from (#255) - and the lookup's reads of
        ** batch 1 say nothing on stderr (#228) */
        char rx[300], err[4096] = "";
        int saved = dup(2), fd = open(DIR "/sn_lookup.err", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        FILE *f;
        snprintf(rx, sizeof rx, "&dbname=snapshots&store=http://127.0.0.1:%d&role=verify", port);
        fflush(stderr); dup2(fd, 2); close(fd);
        sqlite3 *r = open_db("sn_replica.db", KEY4, rx);
        long long rRows = r ? scalar(r, "SELECT count(*) FROM n") : -1, rSnap = r ? scalar(r, "SELECT snapshot_height FROM hpplite_status") : -1;
        sqlite3_close(r);
        fflush(stderr); dup2(saved, 2); close(saved);
        if ((f = fopen(DIR "/sn_lookup.err", "r"))) { size_t got = fread(err, 1, sizeof err - 1, f); err[got] = 0; fclose(f); }
        printf("(replica: rows %lld from the snapshot at %lld) ", rRows, rSnap);
        if (rRows != rows || rSnap != 5 || strstr(err, "no manifest") || strstr(err, "has no")) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAILF("a verifier of the pruned, named rollup, quietly found, from the oldest snapshot kept (5):\n%s", err); }
    }
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    PASS();
}

/* 36. Two named rollups of one wallet whose first ranges have the same bounds (#234): the lookup by name reads
** batch 1 of each through the range manifest; a cache keyed by the bounds alone served the first rollup's
** manifest for the second, so the second was "not found" - a verifier had nothing to verify, a sequencer on a
** fresh file would have created a duplicate. KEY4 (the wallet may hold other rollups from earlier scenarios). */
static void test_two_rollups_same_range_bounds(void) {
    TEST("two named rollups of one wallet, each anchored 1-3 (#234): the second is found by its name, and verifies; a fresh file while the store does not answer creates no second rollup (#229)");
    const char *svc = getenv("HPPLITE_PIN_SERVICE");
    if (!svc) { printf("SKIP (no pin service in the environment) "); PASS(); return; }
    if (!getenv("HPPLITE_VERIFY_BIN")) FAIL("HPPLITE_VERIFY_BIN not set by anvil_test.sh");
    int port = test_free_port();
    pid_t pid = start_pin_service_free(svc, port);
    if (pid < 0) FAIL("the store did not come up");
    static const char *names[] = { "alpha", "beta" };
    long long rows[2] = { 0, 0 };
    char betaContract[64] = "";
    for (int r = 0; r < 2; r++) {
        char extra[300], file[64];
        snprintf(extra, sizeof extra, "&dbname=%s&store=http://127.0.0.1:%d&anchor=0&snapshot=1ms", names[r], port);   /* a snapshot at 3, nothing pruned (#239) */
        snprintf(file, sizeof file, "tr_%s.db", names[r]);
        char errPath[200];
        int saved = dup(2), fd;
        snprintf(errPath, sizeof errPath, DIR "/tr_open_%s.err", names[r]);
        fd = open(errPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        fflush(stderr); dup2(fd, 2); close(fd);                /* what the open says (#241) */
        sqlite3 *w = open_db(file, KEY4, extra);
        fflush(stderr); dup2(saved, 2); close(saved);
        if (!w) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAILF("open the sequencer of %s", names[r]); }
        sqlite3_exec(w, "CREATE TABLE n(id INTEGER PRIMARY KEY, v TEXT)", NULL, NULL, NULL);
        for (int b = 0; b < 3; b++) {
            sqlite3_exec(w, "INSERT INTO n(v) VALUES(hex(randomblob(200)))", NULL, NULL, NULL);
            if (scalar(w, "SELECT hpplite_flush()") <= 0) { printf("(%s) ", status_text(w, "last_error")); sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("a batch through the store"); }
        }
        if (scalar(w, "SELECT hpplite_anchor()") != 3) { printf("(%s) ", status_text(w, "last_error")); sqlite3_close(w); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAILF("%s: the anchor 1-3", names[r]); }
        rows[r] = scalar(w, "SELECT count(*) FROM n");
        if (r == 1) snprintf(betaContract, sizeof betaContract, "%s", status_text(w, "contract"));
        sqlite3_close(w);
    }
    {
        /* #241: creating beta, the lookup read alpha and (run alone) the wallet's empty primary rollup: the message
        ** names alpha, and nothing 'could not be read' with an empty reason. In a whole run earlier scenarios gave
        ** this wallet other databases and batches in its primary, so 'no batches' is said only when run alone */
        char said[2048] = "";
        FILE *f = fopen(DIR "/tr_open_beta.err", "r");
        if (f) { size_t got = fread(said, 1, sizeof said - 1, f); said[got] = 0; fclose(f); }
        if (!strstr(said, "creating one") || !strstr(said, "\"alpha\"") || strstr(said, "could not be read")) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAILF("creating beta should name alpha and nothing unreadable:\n%s", said); }
    }
    /* the second, by name: found, and its own batches verified (not the first's, served from a cache) */
    char params[500], out[8192];
    snprintf(params, sizeof params, "rpc=%s&factory=%s&chainid=31337&privkey=%s&dbname=beta&store=http://127.0.0.1:%d", RPC, FACTORY, KEY4, port);
    out[0] = 0;
    int rc = run_verify_with(params, "--keep " DIR "/tr_keep", out, sizeof out);
    sqlite3 *kept = NULL;
    long long keptRows = -1;
    if (sqlite3_open_v2(DIR "/tr_keep/rebuilt.db", &kept, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) keptRows = scalar(kept, "SELECT count(*) FROM n");
    sqlite3_close(kept);
    printf("(beta: %lld rows, rebuilt %lld) ", rows[1], keptRows);
    if (rc != 0 || !strstr(out, "\nverified") || keptRows != rows[1]) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAILF("hpplite-verify on the second named rollup: exit %d\n%s", rc, out); }
    if (strstr(out, "snapshot at")) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAILF("an unpruned history verifies from batch 1 and claims no snapshot start (#239):\n%s", out); }
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    {
        /* #229: a sequencer on a fresh file while the store does not answer cannot tell its rollup from the others'
        ** - it must not create a second one under the name; it writes locally and finds its own when the store is
        ** back. A store that answers but has none of the bytes (a fresh one: every read a 404) is no reason to wait */
        char extra[300], contract[64] = "", err[256] = "", later[64] = "", gamma[64] = "";
        snprintf(extra, sizeof extra, "&dbname=beta&store=http://127.0.0.1:%d&anchor=0", port);
        sqlite3 *w = open_db("tr_beta2.db", KEY4, extra);          /* the store is down: killed above */
        if (!w) FAIL("open a fresh file for beta with the store down");
        snprintf(contract, sizeof contract, "%s", status_text(w, "contract"));
        snprintf(err, sizeof err, "%s", status_text(w, "last_error"));
        sqlite3_exec(w, "CREATE TABLE n(id INTEGER PRIMARY KEY, v TEXT); INSERT INTO n(v) VALUES('while the store was down')", NULL, NULL, NULL);
        scalar(w, "SELECT hpplite_flush()");                        /* local; the chain is not told without a rollup */
        pid = start_pin_service_free(svc, port);                    /* the same bytes, back */
        if (pid < 0) { sqlite3_close(w); FAIL("the store did not come back"); }
        for (int i = 0; i < 15 && !later[0]; i++) {                /* a flush tries the chain again, at most every 10 s */
            sleep(1);
            scalar(w, "SELECT hpplite_flush()");
            snprintf(later, sizeof later, "%s", status_text(w, "contract"));
        }
        sqlite3_close(w);
        printf("(store down: contract '%s'; back: '%s') ", contract, later);
        if (contract[0]) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAILF("a fresh file with the store down took a rollup (%s): a second one under the name, or another's; last_error '%s'", contract, err); }
        if (!strstr(err, "did not answer")) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAILF("the open with the store down should say why it waits: '%s'", err); }
        if (strcmp(later, betaContract) != 0) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAILF("with the store back, the fresh file should find beta (%s), has '%s'", betaContract, later); }
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        pid = start_pin_service_dir(svc, port, DIR "/store_pins_empty");   /* answers, holds nothing: every read a 404 */
        if (pid < 0) FAIL("the empty store did not come up");
        snprintf(extra, sizeof extra, "&dbname=gamma&store=http://127.0.0.1:%d&anchor=0", port);
        w = open_db("tr_gamma.db", KEY4, extra);
        if (!w) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("open gamma at the empty store"); }
        snprintf(gamma, sizeof gamma, "%s", status_text(w, "contract"));
        sqlite3_close(w);
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        printf("(empty store: gamma %s) ", gamma[0] ? "created" : "not created");
        if (!gamma[0] || strcmp(gamma, betaContract) == 0) FAILF("a store that answers 404 to every read is no reason not to create: gamma '%s'", gamma);
    }
    PASS();
}

/* 37. The platform shape (hpp-io/hpplite#263, #268): one owner key owns the app's rollup and
** names the app's own key as its sequencer, a 24 h lease; the app writes through a plain
** connection with hpplite-sequencer beside it (contract=, a store, keeplease=on,
** anchorclose=off); it sleeps and wakes and the app's key sends no transaction; the host dies
** (kill -9), the owner expires the lease, a successor on another path rebuilds from the store,
** claims and ships; the old host, back, posts nothing over it. The owner never writes a batch,
** so the app's node seals with its own derived key and the successor reads without a batch key. */
static const char *sidecar_bin(void) {
    static char path[600];
    const char *v = getenv("HPPLITE_VERIFY_BIN");
    char *slash;
    if (!v) return NULL;
    snprintf(path, sizeof path, "%s", v);
    slash = strrchr(path, '/');
    snprintf(slash ? slash + 1 : path, sizeof path - (size_t)(slash ? slash + 1 - path : 0), "hpplite-sequencer");
    return path;
}
/* hpplite-sequencer on `file` with HPPLITE_PARAMS `params`, stdout and stderr to files */
static pid_t spawn_sidecar(const char *bin, const char *file, const char *params, const char *out, const char *err) {
    pid_t pid;
    unlink(out); unlink(err);                        /* a previous run's line must not pass for this run's */
    fflush(stdout); fflush(stderr);
    pid = fork();
    if (pid != 0) return pid;
    {
        int fo = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644), fe = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fo >= 0) dup2(fo, 1);
        if (fe >= 0) dup2(fe, 2);
        setenv("HPPLITE_PARAMS", params, 1);
        execl(bin, bin, file, (char *)NULL);
        _exit(127);
    }
}
/* the sidecar's "synced height N" line (#264): N; -1 after `secs`; -2 if it exited first */
static long wait_synced(const char *out, pid_t pid, double secs) {
    double t0 = now_s();
    do {
        FILE *f = fopen(out, "r");
        char line[700];
        if (f) {
            while (fgets(line, sizeof line, f)) {
                char *p = strstr(line, "synced height ");
                if (p) { fclose(f); return atol(p + 14); }
            }
            fclose(f);
        }
        if (waitpid(pid, NULL, WNOHANG) != 0) return -2;
        usleep(100000);
    } while (now_s() - t0 < secs);
    return -1;
}
static int stop_sidecar(pid_t pid, int sig, double secs) {
    int st = 0;
    double t0 = now_s();
    kill(pid, sig);
    while (waitpid(pid, &st, WNOHANG) == 0) {
        if (now_s() - t0 > secs) { kill(pid, SIGKILL); waitpid(pid, &st, 0); return -1; }
        usleep(100000);
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1);
}
/* a plain sqlite3 connection, no HPPLite: what the app does */
static int plain_exec(const char *file, const char *sql) {
    sqlite3 *db = NULL;
    int rc;
    if (sqlite3_open_v2(file, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) { sqlite3_close(db); return -1; }
    sqlite3_busy_timeout(db, 10000);
    rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
    return rc;
}
static long long plain_count(const char *file, const char *sql) {
    sqlite3 *db = NULL;
    long long n;
    if (sqlite3_open_v2(file, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) { sqlite3_close(db); return -1; }
    sqlite3_busy_timeout(db, 10000);
    n = scalar(db, sql);
    sqlite3_close(db);
    return n;
}
/* the chain's lease: 1 if `wallet` holds it unexpired; the expiry in *pExpiry */
static int lease_of_wallet(const char *contract, const unsigned char wallet[20], uint64_t *pExpiry) {
    HppliteL1 *peek = hpplite_l1_connect(RPC, contract);
    HppliteL1State *st = NULL;
    int held = -1;
    for (int attempt = 0; attempt < 3 && peek && !st; attempt++) {    /* a read the RPC drops is tried again */
        st = hpplite_l1_get_state(peek);
        if (!st) sleep(1);
    }
    if (st) {
        held = st->leaseExpiry > st->currentTimestamp && memcmp(st->sequencerWallet, wallet, 20) == 0;
        if (pExpiry) *pExpiry = st->leaseExpiry;
    }
    hpplite_l1_state_free(st);
    hpplite_l1_disconnect(peek);
    return held;
}
/* DIR/<host>/side.<run>.<ext>: each run of a sidecar keeps its own stdout and stderr */
static const char *side_path(int host, int run, const char *ext) {
    static char paths[4][200];
    static int which;
    char *p = paths[which++ & 3];
    snprintf(p, sizeof paths[0], DIR "/host%d/side.%d.%s", host, run, ext);
    return p;
}
/* every transaction `addr` sent, with its selector: a diagnostic for a nonce that moved when it should not have */
static void dump_txs(const char *addr) {
    char cmd[1400], out[4000];
    snprintf(cmd, sizeof cmd,
             "latest=$(cast block-number --rpc-url %s); for b in $(seq 0 $latest); do cast block $b --full --json --rpc-url %s 2>/dev/null | "
             "python3 -I -c 'import json,sys\n"
             "b=json.load(sys.stdin)\n"
             "for t in b.get(\"transactions\", []):\n"
             "    if isinstance(t, dict) and t.get(\"from\",\"\").lower()==sys.argv[1].lower(): print(\"block\", int(b[\"number\"],16), \"to\", t.get(\"to\"), \"selector\", (t.get(\"input\") or \"\")[:10])\n' %s; done; "
             "for f in 'claimSequencer(bytes32)' 'renewLease(bytes32)' 'releaseLease(bytes32)' 'forceExpireLease()' 'submitCheckpoint(bytes32,uint256,uint256,bytes32)' "
             "'setCheckpointInterval(uint256)' 'submitBatch(bytes32,uint256,bytes)' 'commitBatch(bytes32,uint256,bytes32,bytes,bytes)' 'commitRange(bytes32,uint256,uint256,bytes32,bytes32,bytes32,bytes)'; do printf '%%s %%s\\n' \"$(cast sig \"$f\")\" \"$f\"; done",
             RPC, RPC, addr);
    run_cmd(cmd, out, sizeof out);
    printf("\n    [transactions of %s:\n%s] ", addr, out);
}
static void test_platform_sidecar(void) {
    TEST("the platform shape (#263, #268): the owner creates and names the app's key, 24 h lease; a plain-connection app with hpplite-sequencer beside it; sleep/wake sends nothing; kill -9, the owner expires, a successor rebuilds from the store and ships; the old host posts nothing over it");
    const char *svc = getenv("HPPLITE_PIN_SERVICE"), *bin = sidecar_bin();
    static const char *APPKEY = "0x5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a";   /* the app's key, derived by the platform: no rollup of its own */
    char appAddr[48], cmd[600], out[400], contract[80], sql[300], params1[700], params2[700], extra[300];
    unsigned char appWallet[20];
    uint64_t expiry1 = 0, expiry2 = 0, expiry3 = 0, expiry4 = 0;
    long long n0, n1, n2, n3, n4;
    long h;
    int port;
    pid_t store, side;
    if (!svc) { printf("SKIP (no pin service in the environment) "); PASS(); return; }
    if (!bin || access(bin, X_OK) != 0) FAIL("hpplite-sequencer not beside hpplite-verify (HPPLITE_VERIFY_BIN)");
    (void)!system("rm -rf " DIR "/host1 " DIR "/host2 && mkdir -p " DIR "/host1 " DIR "/host2");
    snprintf(cmd, sizeof cmd, "cast wallet address %s 2>/dev/null", APPKEY);
    if (run_cmd(cmd, out, sizeof out) != 0 || strncmp(out, "0x", 2) != 0) FAIL("cast wallet address");
    snprintf(appAddr, sizeof appAddr, "%.42s", out);
    hpplite_hex_to_bytes(appAddr, appWallet, 20);
    snprintf(cmd, sizeof cmd, "cast rpc anvil_setBalance %s 0x21e19e0c9bab2400000 --rpc-url %s >/dev/null 2>&1", appAddr, RPC);   /* the app's key pays its own gas here; on HPP a relay would */
    if (system(cmd) != 0) FAIL("fund the app's key");

    /* the platform: KEY9 creates the app's rollup, names the app's key, sets a day's lease; it never writes */
    sqlite3 *owner = open_db("plat_owner.db", KEY9, "&dbname=somaro-app");
    if (!owner) FAIL("open the owner");
    snprintf(contract, sizeof contract, "%s", status_text(owner, "contract"));
    if (!contract[0] || scalar(owner, "SELECT lease FROM hpplite_status") != 1) { sqlite3_close(owner); FAIL("the owner's open creates the rollup and holds its lease"); }
    snprintf(sql, sizeof sql, "SELECT hpplite_set_sequencer('%s')", appAddr);
    if (exec_sql(owner, sql) != SQLITE_OK) { printf("(%s) ", sqlite3_errmsg(owner)); sqlite3_close(owner); FAIL("the owner names the app's key"); }
    if (exec_sql(owner, "SELECT hpplite_set_lease_duration(86400)") != SQLITE_OK) { printf("(%s) ", sqlite3_errmsg(owner)); sqlite3_close(owner); FAIL("the owner sets a 24 h lease"); }
    if (scalar(owner, "SELECT lease FROM hpplite_status") != 0) { sqlite3_close(owner); FAIL("the owner's own lease ended with the rotation"); }

    port = test_free_port();
    store = start_pin_service_free(svc, port);
    if (store < 0) { sqlite3_close(owner); FAIL("the store did not come up"); }
#define PLAT_CLEANUP() do { kill(store, SIGTERM); waitpid(store, NULL, 0); sqlite3_close(owner); } while (0)
    snprintf(params1, sizeof params1, "rpc=%s&contract=%s&chainid=31337&privkey=%s&datadir=" DIR "/host1&interval=300ms&store=http://127.0.0.1:%d&keeplease=on&anchorclose=off&anchor=24h", RPC, contract, APPKEY, port);
    snprintf(params2, sizeof params2, "rpc=%s&contract=%s&chainid=31337&privkey=%s&datadir=" DIR "/host2&interval=300ms&store=http://127.0.0.1:%d&keeplease=on&anchorclose=off&anchor=24h", RPC, contract, APPKEY, port);
    snprintf(extra, sizeof extra, "&contract=%s&store=http://127.0.0.1:%d&role=verify", contract, port);

    /* host 1: the sidecar first (an empty volume: wait for its line), then the app's plain writes */
    n0 = nonce_of(appAddr);
    side = spawn_sidecar(bin, DIR "/host1/app.db", params1, side_path(1, 1, "out"), side_path(1, 1, "err"));
    h = wait_synced(side_path(1, 1, "out"), side, 60);
    if (h < 0) { if (h == -1) kill(side, SIGKILL); waitpid(side, NULL, 0); PLAT_CLEANUP(); FAILF("host 1's sidecar did not say it was synced (%ld)", h); }
    n1 = nonce_of(appAddr);
    if (n1 != n0 + 1 || lease_of_wallet(contract, appWallet, &expiry1) != 1) { stop_sidecar(side, SIGKILL, 5); PLAT_CLEANUP(); FAILF("the first open is one claim by the app's key (nonce %lld -> %lld) and the lease is the app's", n0, n1); }
    if (plain_exec(DIR "/host1/app.db", "CREATE TABLE notes(id INTEGER PRIMARY KEY, body TEXT); INSERT INTO notes(body) VALUES('first wake')") != SQLITE_OK) { stop_sidecar(side, SIGKILL, 5); PLAT_CLEANUP(); FAIL("the app's plain write"); }
    sleep(3);                                                   /* the timer posts it to the store */

    /* sleep and wake, twice: the sidecar stops (no release, no anchor) and starts (the lease its own, no claim) */
    for (int cycle = 0; cycle < 2; cycle++) {
        int rc = stop_sidecar(side, SIGTERM, 60);
        n2 = nonce_of(appAddr);
        if (rc != 0 || n2 != n1) { dump_txs(appAddr); PLAT_CLEANUP(); FAILF("sleep %d: the sidecar should exit 0 (%d) and send nothing (nonce %lld -> %lld)", cycle + 1, rc, n1, n2); }
        side = spawn_sidecar(bin, DIR "/host1/app.db", params1, side_path(1, cycle + 2, "out"), side_path(1, cycle + 2, "err"));
        h = wait_synced(side_path(1, cycle + 2, "out"), side, 60);
        n2 = nonce_of(appAddr);
        {
            int held = lease_of_wallet(contract, appWallet, &expiry2);
            if (h < 2 || n2 != n1 || held != 1 || expiry2 != expiry1) {
                if (h == -1) kill(side, SIGKILL); waitpid(side, NULL, WNOHANG); PLAT_CLEANUP();
                FAILF("wake %d: synced at %ld, nonce %lld -> %lld, lease held %d, expiry %llu -> %llu: the wake should find the lease its own and send nothing", cycle + 1, h, n1, n2, held, (unsigned long long)expiry1, (unsigned long long)expiry2);
            }
        }
        snprintf(sql, sizeof sql, "INSERT INTO notes(body) VALUES('wake %d')", cycle + 1);
        if (plain_exec(DIR "/host1/app.db", sql) != SQLITE_OK) { stop_sidecar(side, SIGKILL, 5); PLAT_CLEANUP(); FAIL("a write after the wake"); }
        sleep(3);
    }

    /* the host dies; the platform knows and ends the lease; a successor on another path takes over */
    stop_sidecar(side, SIGKILL, 5);
    n2 = nonce_of(appAddr);
    if (exec_sql(owner, "SELECT hpplite_expire_lease()") != SQLITE_OK) { printf("(%s) ", sqlite3_errmsg(owner)); PLAT_CLEANUP(); FAIL("the owner expires the dead host's lease"); }
    if (lease_of_wallet(contract, appWallet, &expiry3) != 0) { PLAT_CLEANUP(); FAIL("the lease should be over on the chain"); }
    side = spawn_sidecar(bin, DIR "/host2/app.db", params2, side_path(2, 1, "out"), side_path(2, 1, "err"));
    h = wait_synced(side_path(2, 1, "out"), side, 90);
    n3 = nonce_of(appAddr);
    if (h < 0 || n3 != n2 + 1 || lease_of_wallet(contract, appWallet, &expiry3) != 1) {
        if (h == -1) kill(side, SIGKILL); waitpid(side, NULL, WNOHANG); PLAT_CLEANUP();
        FAILF("the successor should rebuild (synced %ld) and claim once (nonce %lld -> %lld)", h, n2, n3);
    }
    {
        long long rows = plain_count(DIR "/host2/app.db", "SELECT count(*) FROM notes");
        if (rows != 3) { stop_sidecar(side, SIGKILL, 5); PLAT_CLEANUP(); FAILF("the successor's file should hold the three rows from the store (sealed by the app's own key, no batch key handed over), got %lld", rows); }
    }
    if (plain_exec(DIR "/host2/app.db", "INSERT INTO notes(body) VALUES('successor')") != SQLITE_OK) { stop_sidecar(side, SIGKILL, 5); PLAT_CLEANUP(); FAIL("the successor's app writes"); }
    sleep(3);

    /* the old host comes back and its app writes: nothing of it reaches the chain or the history */
    {
        pid_t old = spawn_sidecar(bin, DIR "/host1/app.db", params1, side_path(1, 4, "out"), side_path(1, 4, "err"));
        long hOld = wait_synced(side_path(1, 4, "out"), old, 60);
        int rcOld;
        if (hOld != -2) plain_exec(DIR "/host1/app.db", "INSERT INTO notes(body) VALUES('stale host')");
        sleep(3);
        rcOld = hOld == -2 ? 0 : stop_sidecar(old, SIGTERM, 60);
        n4 = nonce_of(appAddr);
        {
            char err[2000] = "";
            FILE *f = fopen(side_path(1, 4, "err"), "r");
            if (f) { size_t got = fread(err, 1, sizeof err - 1, f); err[got] = 0; fclose(f); }
            printf("(old host back: synced %ld, exit %d, stderr: %.300s) ", hOld, rcOld, err);
        }
        if (n4 != n3 || lease_of_wallet(contract, appWallet, &expiry4) != 1 || expiry4 != expiry3) { stop_sidecar(side, SIGKILL, 5); PLAT_CLEANUP(); FAILF("the old host must send nothing (nonce %lld -> %lld) and leave the successor's lease (expiry %llu -> %llu)", n3, n4, (unsigned long long)expiry3, (unsigned long long)expiry4); }
    }
    {
        /* the history, from the chain and the store with the app's key: the three rows, the successor's, none of the old host's late one */
        char vuri[1200];
        sqlite3 *v = NULL;
        long long rows = -1, stale = -1;
        snprintf(vuri, sizeof vuri, "file:" DIR "/plat_verify.db?hpplite=on&rpc=%s&chainid=31337&privkey=%s&datadir=" DIR "&interval=0%s", RPC, APPKEY, extra);
        if (sqlite3_open_v2(vuri, &v, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL) == SQLITE_OK) {
            rows = scalar(v, "SELECT count(*) FROM notes");
            stale = scalar(v, "SELECT count(*) FROM notes WHERE body = 'stale host'");
        } else printf("(verifier open: %s) ", sqlite3_errmsg(v));
        sqlite3_close(v);
        if (rows != 4 || stale != 0) { stop_sidecar(side, SIGKILL, 5); PLAT_CLEANUP(); FAILF("a verifier should read four rows and no 'stale host' (rows %lld, stale %lld)", rows, stale); }
    }
    {
        int rc = stop_sidecar(side, SIGTERM, 60);
        n4 = nonce_of(appAddr);
        PLAT_CLEANUP();
        if (rc != 0 || n4 != n3) FAILF("the successor's stop should exit 0 (%d) and send nothing (nonce %lld -> %lld)", rc, n3, n4);
    }
#undef PLAT_CLEANUP
    printf("(transactions by the app's key: %lld in all - one claim per host) ", n4 - n0);
    PASS();
}


int main(void) {
    RPC = getenv("HPPLITE_ANVIL_RPC"); FACTORY = getenv("HPPLITE_ANVIL_FACTORY");
    KEY0 = getenv("HPPLITE_ANVIL_KEY0"); KEY1 = getenv("HPPLITE_ANVIL_KEY1"); KEY2 = getenv("HPPLITE_ANVIL_KEY2");
    KEY3 = getenv("HPPLITE_ANVIL_KEY3"); KEY4 = getenv("HPPLITE_ANVIL_KEY4"); ANVIL_PID = getenv("HPPLITE_ANVIL_PID");
    KEY5 = getenv("HPPLITE_ANVIL_KEY5"); KEY6 = getenv("HPPLITE_ANVIL_KEY6"); KEY7 = getenv("HPPLITE_ANVIL_KEY7");
    KEY8 = getenv("HPPLITE_ANVIL_KEY8"); KEY9 = getenv("HPPLITE_ANVIL_KEY9");
    FACIL = getenv("HPPLITE_ANVIL_FACILITATOR"); FACIL_ADDR = getenv("HPPLITE_ANVIL_FACILITATOR_ADDR");
    if (!RPC || !FACTORY || !KEY0 || !KEY1 || !KEY2 || !KEY3 || !KEY4 || !KEY5 || !KEY6 || !KEY7 || !KEY8 || !KEY9 || !ANVIL_PID || !FACIL || !FACIL_ADDR) { printf("run through tests/l1/anvil_test.sh\n"); return 77; }
    test_suite_init("anvil", NULL, DIR);
    reset_dir();
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    printf("=== HPPLite on anvil (%s) ===\n", RPC);
    if (wanted(1)) test_open_write_post_checkpoint();
    if (wanted(2)) test_verify_tool();
    if (wanted(3)) test_second_instance_same_wallet();
    if (wanted(4)) test_concurrent_rpc();
    if (wanted(5)) test_sleep_and_wake();
    if (wanted(6)) test_stranger_after_expiry();
    if (wanted(8)) test_big_transaction_end_to_end();
    if (wanted(9)) test_l1_outage();
    if (wanted(10)) test_reorg();
    if (wanted(11)) test_named_databases();
    if (wanted(12)) test_resume_elsewhere();
    if (wanted(13)) test_two_processes_one_file();
    if (wanted(14)) test_rpc_faults();
    if (wanted(15)) test_power_loss();
    if (soak_env("HPPLITE_SOAK_SECONDS", 0) > 0) test_soak();   /* 16: only when asked (#120) */
    if (wanted(17)) test_chain_hash();
    /* 18: the reference pin service's buyer selftest, gone with hpplite-store/1: test_store_buckets covers /2 (#213) */
    if (wanted(19)) test_store_commit();
    if (wanted(20)) test_store_paid();
    if (wanted(21)) test_store_fallback();
    if (wanted(22)) test_key_after_open();
    if (wanted(23)) test_signer_posts();
    if (wanted(24)) test_owner_names_sequencer();
    if (wanted(25)) test_viewing_key_reads();
    if (wanted(26)) test_page_mode_end_to_end();
    if (wanted(27)) test_relay_no_eth();
    if (wanted(28)) test_relay_fallback();
    /* 29: the store paid by voucher, gone with hpplite-store/1 (#214): a /2 bucket is paid by permit or x402 top-ups (20) */
    if (wanted(30)) test_spend_cap();
    if (wanted(31)) test_relay_channel();
    if (wanted(32)) test_verify_to();
    if (wanted(33)) test_sealed_to_owner();
    if (wanted(34)) test_verify_at();
    if (wanted(35)) test_snapshots_on_chain();
    if (wanted(36)) test_two_rollups_same_range_bounds();
    if (wanted(37)) test_platform_sidecar();
    if (tests_passed == tests_run) remove_dir();           /* a failure leaves the files to look at */
    return test_report();
}
