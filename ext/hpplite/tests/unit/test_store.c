/*
** The store client (#11): put bytes into the rollup's bucket and get them back
** by the reference, the bucket statement, names, and the errors. Against the
** reference store, hpplite-store/2 (operators/store/hpplite_store.py, #214),
** free and without a chain, started here; argv[1] is that directory. The paid
** path (permit pulls, x402 top-ups) is anvil scenario 19.
*/
#include "test_lib.h"
#include "store.h"
#include "pages.h"
#include "cJSON.h"
#include "crypto.h"
#include "eth_client.h"
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <math.h>

static pid_t g_pid;

static int start_service(const char *dir, int port) {
    char portStr[16], storeDir[600], keyPath[600], script[600];
    FILE *f;
    snprintf(portStr, sizeof portStr, "%d", port);
    snprintf(storeDir, sizeof storeDir, "%s/pins", test_dir());
    snprintf(keyPath, sizeof keyPath, "%s/signer.key", test_dir());
    snprintf(script, sizeof script, "%s/hpplite_store.py", dir);
    f = fopen(keyPath, "w");
    if (!f) return -1;
    fputs("0x2020202020202020202020202020202020202020202020202020202020202020\n", f);
    fclose(f);
    g_pid = fork();
    if (g_pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execlp("python3", "python3", "-I", script, "--port", portStr, "--dir", storeDir, "--signer-key", keyPath,
               "--pay-to", "0x1111111111111111111111111111111111111111", "--price-per-write", "0", "--price-per-mib-month", "0",
               (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 50; i++) {
        char cmd[200];
        if (!test_child_alive(g_pid)) { g_pid = 0; return -1; }        /* the port was taken: never probe another's server (#177) */
        snprintf(cmd, sizeof cmd, "curl -s -m 2 http://127.0.0.1:%d/status 2>/dev/null | grep -q signer", port);
        if (system(cmd) == 0) return 0;
        usleep(200 * 1000);
    }
    return -1;
}

static void stop_service(void) {
    if (g_pid > 0) { kill(g_pid, SIGTERM); waitpid(g_pid, NULL, 0); g_pid = 0; }
}

/* #250: kubo:// and s3:// were store kinds with no payment, no names (Kubo) and no bucket statement; since v0.8 a store
** speaks hpplite-store/2 and nothing else opens */
static void test_no_kubo_s3(void) {
    TEST("kubo:// and s3:// are no stores (#250): neither opens");
    if (hpplite_store_open("kubo://127.0.0.1:5001", NULL, 0) != NULL) FAIL("kubo:// opened");
    setenv("AWS_ACCESS_KEY_ID", "k", 1); setenv("AWS_SECRET_ACCESS_KEY", "s", 1);
    HppliteStore *s3 = hpplite_store_open("s3://batches/hpplite", NULL, 0);
    unsetenv("AWS_ACCESS_KEY_ID"); unsetenv("AWS_SECRET_ACCESS_KEY");
    if (s3) { hpplite_store_close(s3); FAIL("s3:// opened"); }
    PASS();
}

static void test_roundtrip(HppliteStore *s) {
    TEST("put bytes into the rollup's bucket (bound at the first put), get them back by the reference");
    const uint8_t data[] = "a sealed batch, as the node would post it";
    char *zRef = hpplite_store_put_at(s, 1, data, sizeof(data) - 1);
    if (!zRef) FAILF("put: %s", hpplite_store_error(s));
    if (strncmp(zRef, "bafkrei", 7) != 0) { sqlite3_free(zRef); FAIL("the reference is not a CIDv1 raw sha-256"); }
    if (!hpplite_store_is_bucket(s) || !hpplite_store_bucket_state(s) || strcmp(hpplite_store_bucket_state(s), "covered") != 0) {
        sqlite3_free(zRef); FAIL("the bucket is bound and covered");
    }
    size_t n = 0;
    uint8_t *back = hpplite_store_get(s, zRef, &n);
    if (!back || n != sizeof(data) - 1 || memcmp(back, data, n) != 0 || back[n] != 0) { free(back); sqlite3_free(zRef); FAIL("get: bytes or terminator"); }
    free(back);
    /* the same bytes again: the same reference, held already */
    char *zRef2 = hpplite_store_put_at(s, 1, data, sizeof(data) - 1);
    if (!zRef2 || strcmp(zRef, zRef2) != 0) { sqlite3_free(zRef); sqlite3_free(zRef2); FAIL("a second put of the same bytes should give the same reference"); }
    sqlite3_free(zRef);
    sqlite3_free(zRef2);
    if (hpplite_store_spent(s, NULL) != 0) FAIL("nothing should have been paid in free mode");
    PASS();
}

static void test_statement(HppliteStore *s, const char *url) {
    TEST("the bucket statement: version 2, the store's published signer, checked; it verifies from its bytes alone, not for another DA, chain or a changed byte (#215); another key cannot bind or write the bucket");
    static const unsigned char k2[32] = { [31] = 0x02 };
    unsigned char signer[20], payTo[20], da[20];
    uint64_t chainId = 0;
    uint8_t st[HPPLITE_STATEMENT_SIZE];
    if (hpplite_store_signer(s, signer) != 0 || hpplite_store_domain(s, payTo, &chainId) != 0) FAIL("the store's /status should name its signer and domain");
    if (hpplite_store_statement(s, st) != HPPLITE_STATEMENT_SIZE) FAILF("statement: %s", hpplite_store_error(s));
    if (st[0] != 2 || memcmp(st + 1, signer, 20) != 0 || memcmp(st + 21, payTo, 20) != 0) FAIL("version 2, the signer, the domain's payTo");
    if (hpplite_store_covered_until(s) <= 0) FAIL("a free store covers the bucket");
    {
        /* as hpplite-verify checks it from the chain alone (#215): the right DA verifies; a changed byte, or another DA, does not */
        HppliteStatement fields;
        unsigned char daA[20], daB[20];
        uint8_t bad[HPPLITE_STATEMENT_SIZE];
        memset(daA, 0xa1, 20); memset(daB, 0xb2, 20);
        if (hpplite_store_statement_verify(st, sizeof st, chainId, daA, &fields) != 0 || memcmp(fields.signer, signer, 20) != 0
            || memcmp(fields.payTo, payTo, 20) != 0 || fields.coveredUntil == 0) FAIL("the statement verifies, its fields read back");
        /* a free store's 'covered forever' is 2^53-1: exact through any JSON parser - a double, an int64, JavaScript (#216) */
        if (fields.coveredUntil != 9007199254740991ULL || hpplite_store_covered_until(s) != 9007199254740991LL) {
            printf("(coveredUntil %llu, covered_until %lld) ", (unsigned long long)fields.coveredUntil, (long long)hpplite_store_covered_until(s));
            FAIL("a free store's statement says covered until 2^53-1, read back exactly");
        }
        if (hpplite_store_statement_verify(st, sizeof st, chainId, daB, NULL) == 0) FAIL("verified for another rollup's DA");
        memcpy(bad, st, sizeof bad); bad[48] ^= 1;               /* the height */
        if (hpplite_store_statement_verify(bad, sizeof bad, chainId, daA, NULL) == 0) FAIL("verified with a changed height");
        memcpy(bad, st, sizeof bad); bad[100] ^= 1;              /* a signature byte */
        if (hpplite_store_statement_verify(bad, sizeof bad, chainId, daA, NULL) == 0) FAIL("verified with a changed signature");
        if (hpplite_store_statement_verify(st, sizeof st, chainId + 1, daA, NULL) == 0) FAIL("verified on another chain");
    }
    HppliteStore *other = hpplite_store_open(url, k2, 0);
    memset(da, 0xa1, 20);
    hpplite_store_set_bucket(other, da, 0);
    char *zRef = other ? hpplite_store_put_at(other, 2, (const uint8_t *)"not yours", 9) : NULL;
    const char *why = other ? hpplite_store_error(other) : "open";
    printf("(another key: %s) ", zRef ? "taken" : why);
    sqlite3_free(zRef);
    int refused = !zRef && strstr(why, "bind");
    if (other) hpplite_store_close(other);
    if (!refused) FAIL("another key must not bind a bucket the first binder holds");
    PASS();
}

static void test_errors(HppliteStore *s) {
    TEST("an unknown reference is NULL with a reason; an empty put is refused; an unknown scheme does not open");
    size_t n = 1;
    uint8_t *back = hpplite_store_get(s, "bafkreiaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &n);
    if (back || n != 0) { free(back); FAIL("get of an unknown reference"); }
    if (!hpplite_store_error(s)[0]) FAIL("no reason given");
    if (hpplite_store_put(s, (const uint8_t *)"", 0) != NULL) FAIL("an empty put");
    if (hpplite_store_open("ftp://nowhere", NULL, 0) != NULL) FAIL("ftp:// opened");
    HppliteStore *dead = hpplite_store_open("http://127.0.0.1:9", NULL, 0);
    if (!dead) FAIL("open of an unreachable store should succeed (it is reached at the first call)");
    if (hpplite_store_put(dead, (const uint8_t *)"x", 1) != NULL) { hpplite_store_close(dead); FAIL("a put to a dead store"); }
    if (!strstr(hpplite_store_error(dead), "unreachable")) { hpplite_store_close(dead); FAILF("reason: %s", hpplite_store_error(dead)); }
    hpplite_store_close(dead);
    PASS();
}

/* 4. The status row (#11): a node on a store says where its bytes go, what
** it paid, what a put costs, and why a batch waits. Free store here (price
** and balance NULL); the paid figures are anvil scenario 20. */
static void test_node_status(const char *url) {
    TEST("hpplite_status on a store: store, store_state, store_calls, store_spent, store_price, store_balance; a replica reads the commitment; 'unreachable' when the store is down");
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url };
    HppliteNode *w = test_open_node(tpath("st_w.db"), l1, &o);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(x); INSERT INTO t VALUES('in the store')", NULL);
    if (hpplite_node_flush_batch(w) < 1) { printf("(%s) ", status_text(w->db, "last_error")); hpplite_node_destroy(w); FAIL("flush through the store"); }
    {
        char store[200];
        text_of(w->db, "SELECT store FROM hpplite_status", store, sizeof store);
        if (strcmp(store, url) != 0) { printf("(store '%s') ", store); hpplite_node_destroy(w); FAIL("store should be the store's URL"); }
    }
    if (strcmp(status_text(w->db, "store_state"), "ok") != 0) { printf("('%s') ", status_text(w->db, "store_state")); hpplite_node_destroy(w); FAIL("store_state should be ok after a put"); }
    if (status_int(w->db, "store_calls") != 1) { printf("(%lld) ", (long long)status_int(w->db, "store_calls")); hpplite_node_destroy(w); FAIL("store_calls should count the one put"); }
    if (status_int(w->db, "store_spent") != 0) { hpplite_node_destroy(w); FAIL("nothing is paid on a free store"); }
    if (scalar(w->db, "SELECT store_price IS NULL FROM hpplite_status") != 1) { hpplite_node_destroy(w); FAIL("a free store quotes no price"); }
    if (scalar(w->db, "SELECT store_balance IS NULL FROM hpplite_status") != 1) { hpplite_node_destroy(w); FAIL("no balance without a paid asset on a chain"); }
    /* a store node does not commit the batch itself: the anchor does (#164), one commitment for the range */
    if (status_int(w->db, "anchored") != 0) { printf("(anchored %lld) ", (long long)status_int(w->db, "anchored")); hpplite_node_destroy(w); FAIL("nothing is on the chain before the anchor"); }
    if (hpplite_node_anchor(w) != 1 || status_int(w->db, "anchored") != 1) { printf("(%s) ", status_text(w->db, "last_error")); hpplite_node_destroy(w); FAIL("the anchor should commit the one batch"); }
    {
        /* a replica with the same store rebuilds from the chain's commitment */
        TestNodeOpts ro = { .storeUrl = url, .verifyOnly = 1, .nodeId = "store_r" };
        HppliteNode *r = test_open_node(tpath("st_r.db"), l1, &ro);
        int rows = r ? count_rows(r->db, "t") : -1;
        if (r) { r->l1 = NULL; hpplite_node_destroy(r); }
        if (rows != 1) { printf("(replica rows %d) ", rows); hpplite_node_destroy(w); FAIL("a replica on the store should rebuild the committed batch"); }
    }
    hpplite_node_destroy(w);                                   /* disconnects l1 */

    /* the store down: the chain carries the bytes (storefallback on) and the row says why */
    HppliteL1 *l2 = hpplite_l1_create_mock();
    TestNodeOpts d = { .storeUrl = "http://127.0.0.1:9", .nodeId = "store_down" };
    HppliteNode *w2 = test_open_node(tpath("st_down.db"), l2, &d);
    if (!w2) FAIL("open with the store down");
    hpplite_node_exec(w2, "CREATE TABLE t(x); INSERT INTO t VALUES('on the chain')", NULL);
    if (hpplite_node_flush_batch(w2) < 1) { printf("(%s) ", status_text(w2->db, "last_error")); hpplite_node_destroy(w2); FAIL("the chain should carry the batch"); }
    if (strcmp(status_text(w2->db, "store_state"), "unreachable") != 0 || status_int(w2->db, "store_calls") != 0) {
        printf("('%s', calls %lld) ", status_text(w2->db, "store_state"), (long long)status_int(w2->db, "store_calls"));
        hpplite_node_destroy(w2);
        FAIL("store_state should say the store is unreachable, with no call counted");
    }
    hpplite_node_destroy(w2);
    PASS();
}

/* #172: the node's quote of a put (store_price) is the price the store's 402
** asks: per put, per MiB-month for the retention bought, rounded up to a
** whole unit. A paid service (its 402s need no facilitator: none answers there) */
static void test_quote(const char *dir) {
    TEST("the node quotes a write as a /2 store prices it (#172, #214): the write fee and a month of the bytes' storage, rounded up to a whole unit");
    int port = test_free_port();
    char portStr[16], script[600], url[64], keyPath[600];
    static const size_t sizes[] = { 183, 40000, 3 * 1048576 + 17 };
    snprintf(portStr, sizeof portStr, "%d", port);
    snprintf(script, sizeof script, "%s/hpplite_store.py", dir);
    snprintf(keyPath, sizeof keyPath, "%s/signer.key", test_dir());
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execlp("python3", "python3", "-I", script, "--port", portStr, "--dir", tpath("quote_pins"), "--signer-key", keyPath,
               "--pay-to", "0x1111111111111111111111111111111111111111", "--facilitator", "http://127.0.0.1:1",
               "--price-per-mib-month", "30.5", "--price-per-write", "10", (char *)NULL);
        _exit(127);
    }
    int up = 0;
    for (int i = 0; i < 50 && !up; i++) {
        char cmd[200];
        if (!test_child_alive(pid)) break;
        snprintf(cmd, sizeof cmd, "curl -s -m 2 http://127.0.0.1:%d/status 2>/dev/null | grep -q pricePerWrite", port);
        up = system(cmd) == 0;
        if (!up) usleep(200 * 1000);
    }
    if (!up) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); FAIL("the paid store did not start"); }
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    HppliteStore *st = hpplite_store_open(url, (const uint8_t *)"0123456789abcdef0123456789abcdef", 0);
    const char *why = st ? NULL : "open";
    for (size_t k = 0; !why && k < sizeof sizes / sizeof sizes[0]; k++) {
        long long want = 10 + (long long)ceil(30.5 * (double)sizes[k] / 1048576.0 - 1e-9);
        long long quoted = (long long)hpplite_store_quote(st, sizes[k]);
        printf("(%zu bytes: %lld) ", sizes[k], quoted);
        if (quoted != want) why = "the node's quote is not the store's price";
    }
    if (st) hpplite_store_close(st);
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    if (why) FAIL(why);
    PASS();
}

/* 7. Ranges (#164): a store node's batches go to the store at the flush interval and to the
** chain as one commitment per anchor; a replica follows the manifest; the chain hash, the
** checkpoint and the lease come with the anchor; what a run leaves at the store, the next
** run anchors; the close anchors. */
static void test_ranges(const char *url) {
    TEST("ranges: five flushes put five batches and nothing on the chain until the anchor, then one range with the root; a replica rebuilds from it; the close anchors the rest");
    static const unsigned char kseq[32] = { [31] = 0x77 };
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url, .nodeId = "ranges_w", .privkey = kseq };
    HppliteNode *w = test_open_node(tpath("rg_w.db"), l1, &o);
    uint64_t last = 0, total = 0;
    unsigned char latest[32];
    HppliteRange r;
    int i;
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    for (i = 0; i < 5; i++) {
        char sql[80];
        snprintf(sql, sizeof sql, "INSERT INTO t(v) VALUES('batch %d')", i + 1);
        hpplite_node_exec(w, sql, NULL);
        if (hpplite_node_flush_batch(w) < 1) { printf("(flush %d: %s) ", i + 1, status_text(w->db, "last_error")); hpplite_node_destroy(w); FAIL("a flush through the store"); }
    }
    hpplite_l1_get_da_state(l1, &last, &total, latest);
    if (last != 0 || status_int(w->db, "height") != 6 || status_int(w->db, "anchored") != 0 || status_int(w->db, "pending") != 0) {
        printf("(chain %llu, height %lld, anchored %lld, pending %lld) ", (unsigned long long)last, (long long)status_int(w->db, "height"), (long long)status_int(w->db, "anchored"), (long long)status_int(w->db, "pending"));
        hpplite_node_destroy(w); FAIL("five batches at the store, none on the chain, nothing pending");
    }
    if (status_int(w->db, "store_calls") != 5) { printf("(%lld) ", (long long)status_int(w->db, "store_calls")); hpplite_node_destroy(w); FAIL("five puts"); }
    {
        /* each batch is also under its rollup's name at the store, signed by the sequencer (#165) */
        char c[43], d[43], name[80], *p;
        unsigned char da[20], me[20], who[20], hash[32], want[32];
        uint8_t sig[65], *bytes;
        size_t n = 0;
        sqlite3_stmt *q = NULL;
        HppliteStore *rs = hpplite_store_open(url, NULL, 0);
        hpplite_l1_get_addresses(l1, c, d);
        snprintf(name, sizeof name, "%.40s-3", (d[0] ? d : c) + 2);
        for (p = name; *p && *p != '-'; p++) if (*p >= 'A' && *p <= 'F') *p = (char)(*p - 'A' + 'a');
        { char hex40[41]; memcpy(hex40, name, 40); hex40[40] = 0; hpplite_hex_to_bytes(hex40, da, 20); }
        bytes = rs ? hpplite_store_get_named(rs, name, &n, sig, NULL) : NULL;
        if (!bytes) { printf("(%s: %s) ", name, rs ? hpplite_store_error(rs) : "no store"); if (rs) hpplite_store_close(rs); hpplite_node_destroy(w); FAIL("batch 3 under its name"); }
        hpplite_keccak256(bytes, n, hash);
        sqlite3_prepare_v2(w->ctx->bkDb, "SELECT hash FROM _hpplite_unanchored WHERE height = 3", -1, &q, NULL);
        if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_bytes(q, 0) == 32) memcpy(want, sqlite3_column_blob(q, 0), 32); else memset(want, 0, 32);
        sqlite3_finalize(q);
        if (memcmp(hash, want, 32) != 0) { free(bytes); hpplite_store_close(rs); hpplite_node_destroy(w); FAIL("the named bytes are the batch the row names"); }
        {
            int rr = hpplite_store_batch_signer(da, hpplite_l1_chain_id(l1) ? hpplite_l1_chain_id(l1) : hpplite_l1_default_chain_id(), w->instanceId, 3, bytes, n, sig, who);
            int ra = eth_address_from_privkey(kseq, me);           /* the wallet as the chain knows it (the mock derives its own) */
            if (rr != 0 || ra != 0 || memcmp(who, me, 20) != 0) {
                char h1[41], h2[41];
                hpplite_bytes_to_hex(who, 20, h1); hpplite_bytes_to_hex(me, 20, h2);
                printf("(recover %d -> %s, wallet %d -> %s, name %s, %zu bytes) ", rr, h1, ra, h2, name, n);
                free(bytes); hpplite_store_close(rs); hpplite_node_destroy(w); FAIL("the name's signature recovers to the sequencer's wallet");
            }
        }
        free(bytes); hpplite_store_close(rs);
    }
    if (hpplite_node_anchor(w) != 5) { printf("(%s) ", status_text(w->db, "last_error")); hpplite_node_destroy(w); FAIL("the anchor"); }
    hpplite_l1_get_da_state(l1, &last, &total, latest);
    if (last != 5 || status_int(w->db, "anchored") != 5 || status_int(w->db, "last_checkpoint_height") != 5) {
        printf("(chain %llu, anchored %lld, checkpoint %lld) ", (unsigned long long)last, (long long)status_int(w->db, "anchored"), (long long)status_int(w->db, "last_checkpoint_height"));
        hpplite_node_destroy(w); FAIL("one range 1-5 on the chain, the anchor the checkpoint");
    }
    if (hpplite_l1_get_range(l1, 3, &r) != 0 || r.fromHeight != 1 || r.toHeight != 5 || !r.nReceipt) { hpplite_node_destroy(w); FAIL("height 3 is inside range 1-5, anchored with the store's receipt for the manifest"); }
    {
        unsigned char root[32], chain[32];
        uint64_t h = 0;
        hpplite_get_state_root(w->ctx, root);
        if (memcmp(r.stateRoot, root, 32) != 0) { hpplite_l1_range_clear(&r); hpplite_node_destroy(w); FAIL("the anchor carries the state root after batch 5"); }
        /* the chain hash the anchor carries is the fold the node kept */
        if (hpplite_l1_get_chain_hash(l1, &h, chain) == 0 && (h != 5 || memcmp(chain, r.chainHashAfter, 32) != 0)) { hpplite_l1_range_clear(&r); hpplite_node_destroy(w); FAIL("the chain hash after the range"); }
        if (memcmp(w->ctx->chainHash, r.chainHashAfter, 32) != 0) { hpplite_l1_range_clear(&r); hpplite_node_destroy(w); FAIL("the node's chain hash is the anchored one"); }
    }
    hpplite_l1_range_clear(&r);
    if (hpplite_node_anchor(w) != 0) { hpplite_node_destroy(w); FAIL("nothing to anchor twice"); }
    {
        /* a replica rebuilds from the range: the manifest by its digest, each batch by its reference */
        TestNodeOpts ro = { .storeUrl = url, .verifyOnly = 1, .nodeId = "ranges_r", .privkey = kseq };
        HppliteNode *rp = test_open_node(tpath("rg_r.db"), l1, &ro);
        int rows = rp ? count_rows(rp->db, "t") : -1;
        int same = rp ? same_root(w, rp) : 0;
        sqlite3_int64 anch = rp ? status_int(rp->db, "anchored") : -1;
        if (rp) { rp->l1 = NULL; hpplite_node_destroy(rp); }
        if (rows != 5 || !same || anch != 5) { printf("(replica rows %d, same root %d, anchored %lld) ", rows, same, (long long)anch); hpplite_node_destroy(w); FAIL("a replica should rebuild the five batches from the range"); }
    }
    /* two more at the store, a range of two at the close */
    hpplite_node_exec(w, "INSERT INTO t(v) VALUES('six')", NULL);
    if (hpplite_node_flush_batch(w) < 1) { hpplite_node_destroy(w); FAIL("flush 6"); }
    hpplite_node_exec(w, "INSERT INTO t(v) VALUES('seven')", NULL);
    if (hpplite_node_flush_batch(w) < 1) { hpplite_node_destroy(w); FAIL("flush 7"); }
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    hpplite_l1_get_da_state(l1, &last, &total, latest);
    if (last != 7 || hpplite_l1_get_range(l1, 7, &r) != 0 || r.fromHeight != 6 || r.toHeight != 7) { printf("(chain %llu) ", (unsigned long long)last); hpplite_l1_disconnect(l1); FAIL("the close anchors batches 6-7 as one range"); }
    hpplite_l1_range_clear(&r);
    {
        TestNodeOpts ro = { .storeUrl = url, .verifyOnly = 1, .nodeId = "ranges_r2", .privkey = kseq };
        HppliteNode *rp = test_open_node(tpath("rg_r2.db"), l1, &ro);
        int rows = rp ? count_rows(rp->db, "t") : -1;
        if (rp) { rp->l1 = NULL; hpplite_node_destroy(rp); }
        if (rows != 7) { printf("(replica rows %d) ", rows); hpplite_l1_disconnect(l1); FAIL("a replica follows both ranges"); }
    }
    hpplite_l1_disconnect(l1);
    PASS();
}

/* the snapshot the manifest of the range ending at `to` names: its height and index CID; -1 none */
static int range_snapshot(HppliteL1 *l1, HppliteStore *rs, uint64_t to, uint64_t *pH, char zIndex[64]) {
    HppliteRange r;
    char cid[64];
    size_t n = 0;
    uint8_t *text;
    cJSON *m, *sn, *h, *ix;
    int rc = -1;
    memset(&r, 0, sizeof r);
    if (hpplite_l1_get_range(l1, to, &r) != 0) return -1;
    hpplite_store_cid_from_sha256(r.manifestDigest, cid);
    hpplite_l1_range_clear(&r);
    text = hpplite_store_get(rs, cid, &n);
    m = text ? cJSON_ParseWithLength((const char *)text, n) : NULL;
    free(text);
    sn = m ? cJSON_GetObjectItemCaseSensitive(m, "snapshot") : NULL;
    h = sn ? cJSON_GetObjectItemCaseSensitive(sn, "height") : NULL;
    ix = sn ? cJSON_GetObjectItemCaseSensitive(sn, "index") : NULL;
    if (cJSON_IsNumber(h) && cJSON_IsString(ix)) { *pH = (uint64_t)h->valuedouble; snprintf(zIndex, 64, "%s", ix->valuestring); rc = 0; }
    cJSON_Delete(m);
    return rc;
}

/* 6b. Snapshots (#221): a page-mode sequencer with snapshot= makes one at an anchor - the database as of the
** anchored height, sealed as a batch, in chunks at the store under an index - and every later anchor names it */
static void test_snapshot_made(const char *url) {
    TEST("snapshots: snapshot= makes one at the anchor (the file at the anchored height, its root the range's), named by that manifest and every later one, a restart too");
    static const unsigned char kseq[32] = { [31] = 0x79 };
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url, .nodeId = "snap_w", .privkey = kseq, .snapshotMs = 3600 * 1000 };
    HppliteNode *w = test_open_node(tpath("sn_w.db"), l1, &o);
    HppliteStore *rs;
    HppliteRange r;
    uint64_t h = 0;
    char zIndex[64], cid[64];
    int i;
    if (!w) FAIL("open");
    rs = hpplite_store_open(url, NULL, 0);
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    for (i = 0; i < 3; i++) {
        hpplite_node_exec(w, "INSERT INTO t(v) VALUES(randomblob(3000))", NULL);
        if (hpplite_node_flush_batch(w) < 1) { printf("(%s) ", status_text(w->db, "last_error")); hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("flush"); }
    }
    if (hpplite_node_anchor(w) != 3) { printf("(%s) ", status_text(w->db, "last_error")); hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("the anchor 1-3"); }
    if (status_int(w->db, "snapshot_height") != 3 || range_snapshot(l1, rs, 3, &h, zIndex) != 0 || h != 3) {
        printf("(status %lld, manifest %llu) ", (long long)status_int(w->db, "snapshot_height"), (unsigned long long)h);
        hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("the anchor's manifest names a snapshot at 3");
    }
    {
        /* the index: the height, the range's root and chain hash; the chunks joined open to the file at 3 */
        size_t n = 0, total = 0;
        uint8_t *text = hpplite_store_get(rs, zIndex, &n), *all = NULL;
        cJSON *ix = text ? cJSON_ParseWithLength((const char *)text, n) : NULL, *c;
        unsigned char root[32], chain[32];
        const char *why = NULL;
        HppliteBatch *b = NULL;
        HpplitePageBatch *pb = NULL;
        free(text);
        memset(&r, 0, sizeof r);
        hpplite_l1_get_range(l1, 3, &r);
        if (!ix || cJSON_GetObjectItemCaseSensitive(ix, "height")->valuedouble != 3) why = "the index names height 3";
        else if (hpplite_hex_to_bytes(cJSON_GetObjectItemCaseSensitive(ix, "root")->valuestring + 2, root, 32) != 32 || memcmp(root, r.stateRoot, 32) != 0) why = "the index's root is the range's";
        else if (hpplite_hex_to_bytes(cJSON_GetObjectItemCaseSensitive(ix, "chain")->valuestring + 2, chain, 32) != 32 || memcmp(chain, r.chainHashAfter, 32) != 0) why = "the index's chain hash is the range's";
        cJSON *chunks = why ? NULL : cJSON_GetObjectItemCaseSensitive(ix, "chunks");
        cJSON_ArrayForEach(c, chunks) {
            uint8_t *part = hpplite_store_get(rs, cJSON_GetObjectItemCaseSensitive(c, "ref")->valuestring, &n);
            unsigned char d[32], want[32];
            if (!part) { why = "every chunk at the store"; break; }
            hpplite_store_sha256(part, n, d);
            if (hpplite_hex_to_bytes(cJSON_GetObjectItemCaseSensitive(c, "sha256")->valuestring + 2, want, 32) != 32 || memcmp(d, want, 32) != 0) { free(part); why = "each chunk is the sha-256 the index names"; break; }
            all = realloc(all, total + n); memcpy(all + total, part, n); total += n; free(part);
        }
        if (!why && total != (size_t)cJSON_GetObjectItemCaseSensitive(ix, "bytes")->valuedouble) why = "the chunks join to the index's size";
        if (!why && !(b = hpplite_node_decode(w, 3, all, total))) why = "the snapshot opens with the sequencer's key at its height";
        if (!why && (!b->bPages || !(pb = hpplite_pagebatch_decode(b->pPagesRaw, b->nPagesRaw)))) why = "the snapshot is a page batch";
        if (!why && (pb->nPages != pb->commitSize || memcmp(pb->root, r.stateRoot, 32) != 0)) why = "every page of the file, its root the anchored one";
        if (!why) for (i = 0; i < (int)pb->nPages; i++) if (pb->aPage[i].flags != HPPLITE_PAGE_FULL || pb->aPage[i].pgno != (unsigned)i + 1) { why = "every page whole, in order"; break; }
        if (pb) hpplite_pagebatch_free(pb);
        if (b) hpplite_batch_free(b);
        free(all); cJSON_Delete(ix); hpplite_l1_range_clear(&r);
        if (why) { hpplite_node_destroy(w); hpplite_store_close(rs); FAIL(why); }
    }
    /* two more batches: the next anchor names the same snapshot (snapshot= 1h: not yet another) */
    for (i = 0; i < 2; i++) { hpplite_node_exec(w, "INSERT INTO t(v) VALUES('later')", NULL); hpplite_node_flush_batch(w); }
    if (hpplite_node_anchor(w) != 5 || range_snapshot(l1, rs, 5, &h, cid) != 0 || h != 3 || strcmp(cid, zIndex) != 0) {
        hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("the next anchor carries the snapshot at 3 forward");
    }
    /* a new process: it finds the snapshot in the last anchor's manifest and carries it on */
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    w = test_open_node(tpath("sn_w.db"), l1, &o);
    if (!w) { hpplite_store_close(rs); hpplite_l1_disconnect(l1); FAIL("reopen"); }
    hpplite_node_exec(w, "INSERT INTO t(v) VALUES('after the restart')", NULL);
    hpplite_node_flush_batch(w);
    if (hpplite_node_anchor(w) != 6 || range_snapshot(l1, rs, 6, &h, cid) != 0 || h != 3 || strcmp(cid, zIndex) != 0 || status_int(w->db, "snapshot_height") != 3) {
        printf("(%s) ", status_text(w->db, "last_error"));
        hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("after a restart the anchor still names the snapshot at 3");
    }
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    hpplite_store_close(rs);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 6c. A replica from a snapshot (#222): it writes the snapshot the last anchor names, checked against the
** chain, and replays only the batches after it; the state is the one a replay from batch 1 reaches */
static void test_snapshot_replica(const char *url) {
    TEST("snapshots: a replica starting from the snapshot the last anchor names and replays only what follows; the same rows and root as one from batch 1");
    static const unsigned char kseq[32] = { [31] = 0x7a };
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url, .nodeId = "snaprep_w", .privkey = kseq, .snapshotMs = 3600 * 1000 };
    HppliteNode *w = test_open_node(tpath("snr_w.db"), l1, &o);
    int i;
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    for (i = 0; i < 5; i++) {
        hpplite_node_exec(w, "INSERT INTO t(v) VALUES(randomblob(2000))", NULL);
        if (hpplite_node_flush_batch(w) < 1) { hpplite_node_destroy(w); FAIL("flush"); }
        if (i == 2 && hpplite_node_anchor(w) != 3) { printf("(%s) ", status_text(w->db, "last_error")); hpplite_node_destroy(w); FAIL("the anchor 1-3, the snapshot at 3"); }
    }
    hpplite_node_exec(w, "UPDATE t SET v = 'changed after the snapshot' WHERE id = 2", NULL);
    if (hpplite_node_flush_batch(w) < 1 || hpplite_node_anchor(w) != 6 || status_int(w->db, "snapshot_height") != 3) { hpplite_node_destroy(w); FAIL("the anchor 4-6 names the snapshot at 3"); }
    {
        TestNodeOpts so = { .storeUrl = url, .verifyOnly = 1, .fromSnapshot = 1, .nodeId = "snaprep_s", .privkey = kseq };
        TestNodeOpts go = { .storeUrl = url, .verifyOnly = 1, .nodeId = "snaprep_g", .privkey = kseq };
        HppliteNode *sp = test_open_node(tpath("snr_s.db"), l1, &so);
        HppliteNode *gp = test_open_node(tpath("snr_g.db"), l1, &go);
        int sRows = sp ? count_rows(sp->db, "t") : -1, gRows = gp ? count_rows(gp->db, "t") : -1;
        int sSame = sp ? same_root(w, sp) : 0, gSame = gp ? same_root(w, gp) : 0;
        sqlite3_int64 sSnap = sp ? status_int(sp->db, "snapshot_height") : -1, gSnap = gp ? status_int(gp->db, "snapshot_height") : -1;
        sqlite3_int64 sCmp = sp ? status_int(sp->db, "roots_compared") : -1, gCmp = gp ? status_int(gp->db, "roots_compared") : -1;
        sqlite3_int64 sH = sp ? status_int(sp->db, "height") : -1;
        char changed[64] = "";
        if (sp) { sqlite3_stmt *q; if (sqlite3_prepare_v2(sp->db, "SELECT v FROM t WHERE id = 2", -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) snprintf(changed, sizeof changed, "%s", (const char *)sqlite3_column_text(q, 0)); sqlite3_finalize(q); }
        if (sp) { sp->l1 = NULL; hpplite_node_destroy(sp); }
        if (gp) { gp->l1 = NULL; hpplite_node_destroy(gp); }
        printf("(from the snapshot: rows %d, root same %d, snapshot %lld, roots compared %lld, height %lld; from batch 1: rows %d, same %d, snapshot %lld, compared %lld) ",
               sRows, sSame, (long long)sSnap, (long long)sCmp, (long long)sH, gRows, gSame, (long long)gSnap, (long long)gCmp);
        if (sRows != 5 || !sSame || sSnap != 3 || sH != 7 || strcmp(changed, "changed after the snapshot") != 0) { hpplite_node_destroy(w); FAIL("the replica from the snapshot reaches the sequencer's state"); }
        if (gRows != 5 || !gSame || gSnap != 0) { hpplite_node_destroy(w); FAIL("a verifier, by default, replays from batch 1"); }
        if (sCmp >= gCmp) { hpplite_node_destroy(w); FAIL("the replica from the snapshot applied fewer batches"); }
    }
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 6c'. A snapshot over one chunk (#221, #222): a 9 MB database is two chunks at the store; a replica joins
** them, each checked by the index's sha-256, and reaches the sequencer's state */
static void test_snapshot_chunks(const char *url) {
    TEST("snapshots: a database over 8 MiB is a snapshot in two chunks; a replica joins them and reaches the sequencer's state");
    static const unsigned char kseq[32] = { [31] = 0x7c };
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url, .nodeId = "snapch_w", .privkey = kseq, .snapshotMs = 3600 * 1000 };
    HppliteNode *w = test_open_node(tpath("snc_w.db"), l1, &o);
    HppliteStore *rs;
    uint64_t h = 0, a = 0;
    char zIndex[64];
    int nChunks = -1;
    if (!w) FAIL("open");
    rs = hpplite_store_open(url, NULL, 0);
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    hpplite_node_exec(w, "INSERT INTO t(v) VALUES(randomblob(9000000))", NULL);     /* does not compress: two chunks */
    if (hpplite_node_flush_batch(w) < 1) { printf("(%s) ", status_text(w->db, "last_error")); hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("the 9 MB batch"); }
    hpplite_node_exec(w, "INSERT INTO t(v) VALUES('small')", NULL);
    hpplite_node_flush_batch(w);
    {
        /* the 9 MB commit is cut over batches (#152): the anchor is at the last of them */
        int rsn;
        a = hpplite_node_anchor(w);
        rsn = range_snapshot(l1, rs, a, &h, zIndex);
        if (a < 2 || rsn != 0 || h != a) { printf("(anchor %llu, manifest %d, snapshot %llu, height %lld: %s) ", (unsigned long long)a, rsn, (unsigned long long)h, (long long)status_int(w->db, "height"), status_text(w->db, "last_error")); hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("the anchor with a snapshot at its height"); }
    }
    {
        size_t n = 0;
        uint8_t *text = hpplite_store_get(rs, zIndex, &n);
        cJSON *ix = text ? cJSON_ParseWithLength((const char *)text, n) : NULL;
        nChunks = ix ? cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(ix, "chunks")) : -1;
        free(text); cJSON_Delete(ix);
    }
    hpplite_node_exec(w, "INSERT INTO t(v) VALUES('after')", NULL);
    hpplite_node_flush_batch(w);
    if (hpplite_node_anchor(w) != a + 1) { hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("the next anchor"); }
    {
        TestNodeOpts so = { .storeUrl = url, .verifyOnly = 1, .fromSnapshot = 1, .nodeId = "snapch_s", .privkey = kseq };
        HppliteNode *sp = test_open_node(tpath("snc_s.db"), l1, &so);
        int rows = sp ? count_rows(sp->db, "t") : -1, same = sp ? same_root(w, sp) : 0;
        sqlite3_int64 snap = sp ? status_int(sp->db, "snapshot_height") : -1;
        if (sp) { sp->l1 = NULL; hpplite_node_destroy(sp); }
        printf("(snapshot at %llu in %d chunks; replica rows %d, same root %d, from %lld) ", (unsigned long long)a, nChunks, rows, same, (long long)snap);
        if (nChunks != 2 || rows != 3 || !same || snap != (sqlite3_int64)a) { hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("two chunks, joined by a replica from the snapshot"); }
    }
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    hpplite_store_close(rs);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* the reference of batch `h` in the manifest of the range ending at `to` */
static int range_batch_ref(HppliteL1 *l1, HppliteStore *rs, uint64_t to, uint64_t h, char zRef[128]) {
    HppliteRange r;
    char cid[64];
    size_t n = 0;
    uint8_t *text;
    cJSON *m, *b;
    int rc = -1;
    memset(&r, 0, sizeof r);
    if (hpplite_l1_get_range(l1, to, &r) != 0) return -1;
    hpplite_store_cid_from_sha256(r.manifestDigest, cid);
    text = hpplite_store_get(rs, cid, &n);
    m = text ? cJSON_ParseWithLength((const char *)text, n) : NULL;
    free(text);
    b = m ? cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(m, "batches"), (int)(h - r.fromHeight)) : NULL;
    if (b && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(b, "ref"))) { snprintf(zRef, 128, "%s", cJSON_GetObjectItemCaseSensitive(b, "ref")->valuestring); rc = 0; }
    hpplite_l1_range_clear(&r);
    cJSON_Delete(m);
    return rc;
}

static int store_has(HppliteStore *rs, const char *zRef) {
    size_t n = 0;
    uint8_t *d = hpplite_store_get(rs, zRef, &n);
    free(d);
    return d != NULL;
}

static long long range_earliest(HppliteL1 *l1, HppliteStore *rs, uint64_t to);

/* 1 when the manifest ending at `to` names a snapshot at `height` anywhere (the latest, or an older one kept); 0 not; -1 unreadable */
static int manifest_names_snapshot(HppliteL1 *l1, HppliteStore *rs, uint64_t to, uint64_t height) {
    HppliteRange r;
    char cid[64];
    size_t n = 0;
    uint8_t *text;
    cJSON *m;
    const cJSON *e, *h;
    int found = 0;
    memset(&r, 0, sizeof r);
    if (hpplite_l1_get_range(l1, to, &r) != 0) return -1;
    hpplite_store_cid_from_sha256(r.manifestDigest, cid);
    hpplite_l1_range_clear(&r);
    text = hpplite_store_get(rs, cid, &n);
    m = text ? cJSON_ParseWithLength((const char *)text, n) : NULL;
    free(text);
    if (!m) return -1;
    cJSON_ArrayForEach(e, m) {
        if (cJSON_IsObject(e) && (h = cJSON_GetObjectItemCaseSensitive(e, "height")) && cJSON_IsNumber(h) && (uint64_t)h->valuedouble == height) found = 1;
        if (cJSON_IsArray(e)) { const cJSON *x; cJSON_ArrayForEach(x, e) { h = cJSON_GetObjectItemCaseSensitive(x, "height"); if (cJSON_IsObject(x) && cJSON_IsNumber(h) && (uint64_t)h->valuedouble == height && cJSON_GetObjectItemCaseSensitive(x, "index")) found = 1; } }
    }
    cJSON_Delete(m);
    return found;
}

/* 6d. keep= (#223): the history behind the newest snapshot made at least `keep` ago is pruned at the store;
** a replica still starts from the snapshot, and the manifest names the oldest snapshot still kept (#257) */
static void test_keep(const char *url) {
    TEST("keep=: after an anchor, the batches and snapshots below the newest snapshot older than keep are pruned at the store; the snapshot and what follows stay; a replica still starts there; an anchor the chain refuses prunes nothing (#238)");
    static const unsigned char kseq[32] = { [31] = 0x7b };
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url, .nodeId = "keep_w", .privkey = kseq, .snapshotMs = 1, .keepSeconds = 3 };
    HppliteNode *w = test_open_node(tpath("kp_w.db"), l1, &o);
    HppliteStore *rs;
    char b1[128], b3[128], b4[128], zIndex3[64], zIndex[64];
    uint64_t h = 0;
    int i;
    if (!w) FAIL("open");
    rs = hpplite_store_open(url, NULL, 0);
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    for (i = 0; i < 3; i++) { hpplite_node_exec(w, "INSERT INTO t(v) VALUES('one of the first three')", NULL); hpplite_node_flush_batch(w); }
    if (hpplite_node_anchor(w) != 3 || range_snapshot(l1, rs, 3, &h, zIndex3) != 0 || h != 3) { hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("the anchor 1-3 with a snapshot at 3"); }
    /* a restart between the snapshots at 3 and 5: the process that did not make the snapshot at 3 learns it from the
    ** manifest at its first anchor - which the chain refuses below (#238) - and must still name it at the retry (#259) */
    hpplite_node_stop_timer(w); w->ownsL1 = 0; w->l1 = NULL; hpplite_node_destroy(w);
    w = test_open_node(tpath("kp_w.db"), l1, &o);
    if (!w) { hpplite_store_close(rs); FAIL("reopen between the snapshots"); }
    if (range_batch_ref(l1, rs, 3, 1, b1) != 0 || range_batch_ref(l1, rs, 3, 3, b3) != 0 || !store_has(rs, b1)) { hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("batch 1 at the store: nothing older than keep yet"); }
    sleep(4);                                   /* keep=3s, by whole seconds: a margin over the boundary */
    for (i = 0; i < 2; i++) { hpplite_node_exec(w, "INSERT INTO t(v) VALUES('four and five')", NULL); hpplite_node_flush_batch(w); }
    /* the anchor at which the prune is due, refused by the chain (#238): the store must still hold what the
    ** manifest on chain (the anchor at 3) names - batch 1 and the snapshot at 3; the prune waits for an anchor that lands */
    hpplite_l1_mock_set_fail_submit(l1, 1);
    if (hpplite_node_anchor(w) == 5) { hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("the mock should refuse this anchor"); }
    if (!store_has(rs, b1) || !store_has(rs, zIndex3)) { printf("(batch 1 %d, snapshot 3 %d) ", store_has(rs, b1), store_has(rs, zIndex3)); hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("a refused anchor must not prune: batch 1 and the snapshot at 3 still named by the chain's manifest"); }
    hpplite_l1_mock_set_fail_submit(l1, 0);
    if (hpplite_node_anchor(w) != 5 || range_snapshot(l1, rs, 5, &h, zIndex) != 0 || h != 5) { hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("the anchor 4-5 with a new snapshot at 5"); }
    if (range_earliest(l1, rs, 5) != 3) { printf("(earliest at 5: %lld) ", range_earliest(l1, rs, 5)); hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("the manifest at 5 names the snapshot at 3 as the oldest kept (#257)"); }
    if (manifest_names_snapshot(l1, rs, 5, 3) != 1) { hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("the manifest at 5 must still name the snapshot at 3 after a restart and a refused anchor (#259)"); }
    /* the snapshot at 3 is older than keep: below it is pruned; it and what follows stay */
    range_batch_ref(l1, rs, 5, 4, b4);
    if (store_has(rs, b1) || !store_has(rs, b3) || !store_has(rs, b4) || !store_has(rs, zIndex3)) {
        printf("(batch 1 %d, batch 3 %d, batch 4 %d, snapshot 3 %d) ", store_has(rs, b1), store_has(rs, b3), store_has(rs, b4), store_has(rs, zIndex3));
        hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("pruned below the snapshot at 3: batch 1 gone; batch 3, batch 4 and the snapshot at 3 kept");
    }
    sleep(4);                                   /* keep=3s, by whole seconds: a margin over the boundary */
    hpplite_node_exec(w, "INSERT INTO t(v) VALUES('six')", NULL); hpplite_node_flush_batch(w);
    if (hpplite_node_anchor(w) != 6) { hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("the anchor 6"); }
    if (range_earliest(l1, rs, 6) != 5) { printf("(earliest at 6: %lld) ", range_earliest(l1, rs, 6)); hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("the manifest at 6 names the snapshot at 5 as the oldest kept: the one at 3 is pruned (#257)"); }
    /* now the snapshot at 5 is older than keep: the one at 3 and batches 3-4 go */
    if (store_has(rs, zIndex3) || store_has(rs, b3) || store_has(rs, b4)) {
        printf("(snapshot 3 %d, batch 3 %d, batch 4 %d) ", store_has(rs, zIndex3), store_has(rs, b3), store_has(rs, b4));
        hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("pruned below the snapshot at 5: the snapshot at 3 and batches 3-4 gone");
    }
    {
        /* a replica starts from the latest snapshot and reaches the sequencer's state; one from batch 1 cannot */
        TestNodeOpts so = { .storeUrl = url, .verifyOnly = 1, .fromSnapshot = 1, .nodeId = "keep_s", .privkey = kseq };
        HppliteNode *sp = test_open_node(tpath("kp_s.db"), l1, &so);
        int rows = sp ? count_rows(sp->db, "t") : -1, same = sp ? same_root(w, sp) : 0;
        sqlite3_int64 snap = sp ? status_int(sp->db, "snapshot_height") : -1;
        if (sp) { sp->l1 = NULL; hpplite_node_destroy(sp); }
        if (rows != 6 || !same || snap < 5) { printf("(rows %d, same %d, snapshot %lld) ", rows, same, (long long)snap); hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("a replica from the snapshot after the prune"); }
    }
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    hpplite_store_close(rs);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* a store of this test's own, on a port and directory it chooses, so it can be killed and started again */
static pid_t start_store_at(const char *dir, int port, const char *storeDir, int failPut, int busyPut) {
    char portStr[16], keyPath[600], script[600], failStr[16], busyStr[16];
    pid_t pid;
    snprintf(portStr, sizeof portStr, "%d", port);
    snprintf(failStr, sizeof failStr, "%d", failPut);
    snprintf(busyStr, sizeof busyStr, "%d", busyPut);
    snprintf(keyPath, sizeof keyPath, "%s/signer.key", test_dir());
    snprintf(script, sizeof script, "%s/hpplite_store.py", dir);
    pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execlp("python3", "python3", "-I", script, "--port", portStr, "--dir", storeDir, "--signer-key", keyPath,
               "--pay-to", "0x1111111111111111111111111111111111111111", "--price-per-write", "0", "--price-per-mib-month", "0",
               "--fail-put", failStr,                      /* a test hook: the k-th content put answers 503; 0 none */
               "--busy-put", busyStr,                      /* and: the k-th answers 429 as an operator's rate limit does */
               (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 50; i++) {
        char cmd[200];
        if (!test_child_alive(pid)) return -1;
        snprintf(cmd, sizeof cmd, "curl -s -m 2 http://127.0.0.1:%d/status 2>/dev/null | grep -q signer", port);
        if (system(cmd) == 0) return pid;
        usleep(200 * 1000);
    }
    kill(pid, SIGKILL); waitpid(pid, NULL, 0);
    return -1;
}

/* 6f. A store that was down at open (#244): the first put after it answers must go through - the client's
** "keeps buckets" flag comes from /status, and the node told it the rollup's address only once it knew that */
static void test_store_down_at_open(const char *dir) {
    TEST("a store down at open, up later (#244): the first flush after it answers goes to the store");
    int port = test_free_port();
    char url[64], storeDir[600];
    pid_t pid;
    static const unsigned char kseq[32] = { [31] = 0x7e };
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o;
    HppliteNode *w;
    uint64_t h1, h2;
    char err[300] = "";
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    snprintf(storeDir, sizeof storeDir, "%s/do_pins", test_dir());
    memset(&o, 0, sizeof o);
    o.storeUrl = url; o.nodeId = "do_w"; o.privkey = kseq; o.storeFallbackOff = 1;
    w = test_open_node(tpath("do_w.db"), l1, &o);         /* nothing listens on the port yet */
    if (!w) FAIL("open with the store down");
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    hpplite_node_exec(w, "INSERT INTO t(v) VALUES('written while the store was down')", NULL);
    h1 = hpplite_node_flush_batch(w);                      /* waits: HPPLITE_STORE_FALLBACK=off */
    pid = start_store_at(dir, port, storeDir, 0, 0);
    if (pid < 0) { hpplite_node_destroy(w); FAIL("the test's store did not start"); }
    h2 = hpplite_node_flush_batch(w);                      /* the first put after the store answers */
    snprintf(err, sizeof err, "%s", w->ctx->zLastError);
    printf("(down: %llu; up: %llu) ", (unsigned long long)h1, (unsigned long long)h2);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    if (h1 != 0) FAIL("with the store down and HPPLITE_STORE_FALLBACK=off the batch waits");
    if (h2 != 1) FAILF("the first flush after the store answers should put batch 1: '%s'", err);
    PASS();
}

/* the oldest snapshot still kept that the manifest ending at `to` names ("earliest", #257): its height; 0 when the
** manifest names none; -1 when the manifest cannot be read or still carries the "earlier" list of before */
static long long range_earliest(HppliteL1 *l1, HppliteStore *rs, uint64_t to) {
    HppliteRange r;
    char cid[64];
    size_t n = 0;
    uint8_t *text;
    cJSON *m;
    const cJSON *e, *h;
    long long out;
    memset(&r, 0, sizeof r);
    if (hpplite_l1_get_range(l1, to, &r) != 0) return -1;
    hpplite_store_cid_from_sha256(r.manifestDigest, cid);
    hpplite_l1_range_clear(&r);
    text = hpplite_store_get(rs, cid, &n);
    m = text ? cJSON_ParseWithLength((const char *)text, n) : NULL;
    free(text);
    if (!m) return -1;
    if (cJSON_GetObjectItemCaseSensitive(m, "earlier")) { cJSON_Delete(m); return -1; }
    e = cJSON_GetObjectItemCaseSensitive(m, "earliest");
    h = e ? cJSON_GetObjectItemCaseSensitive(e, "height") : NULL;
    out = !e ? 0 : cJSON_IsNumber(h) ? (long long)h->valuedouble : -1;
    cJSON_Delete(m);
    return out;
}

/* 6e. A restart whose first anchor cannot read the previous manifest decides nothing (#236): with the store not
** answering it would have taken "no snapshot known", made a fresh one at the next anchor that landed and dropped
** the earlier list - the snapshots earlier anchors named referenced by nothing, never pruned, never found */
static void test_restart_store_down(const char *dir) {
    TEST("a restart with the store not answering at its first anchor (#236): no snapshot made, nothing decided; the next anchor reads the previous manifest and names the latest snapshot and the earlier ones");
    int port = test_free_port();
    char url[64], storeDir[600];
    uint64_t h = 0;
    long long n;
    pid_t pid;
    static const unsigned char kseq[32] = { [31] = 0x7c };
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o;
    HppliteNode *w;
    HppliteStore *rs;
    char zIndex[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    snprintf(storeDir, sizeof storeDir, "%s/sd_pins", test_dir());
    pid = start_store_at(dir, port, storeDir, 0, 0);
    if (pid < 0) FAIL("the test's store did not start");
    memset(&o, 0, sizeof o);
    o.storeUrl = url; o.nodeId = "sd_w"; o.privkey = kseq; o.snapshotMs = 1; o.keepSeconds = 3600; o.storeFallbackOff = 1;
    w = test_open_node(tpath("sd_w.db"), l1, &o);
    rs = hpplite_store_open(url, NULL, 0);
    if (!w || !rs) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("open"); }
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    for (int i = 0; i < 3; i++) { hpplite_node_exec(w, "INSERT INTO t(v) VALUES('first three')", NULL); hpplite_node_flush_batch(w); }
    if (hpplite_node_anchor(w) != 3 || range_snapshot(l1, rs, 3, &h, zIndex) != 0 || h != 3) { hpplite_node_destroy(w); hpplite_store_close(rs); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("the anchor 1-3 with a snapshot at 3"); }
    sleep(2);                                   /* snapshot=1ms, by whole seconds: the next anchor makes a new one */
    for (int i = 0; i < 2; i++) { hpplite_node_exec(w, "INSERT INTO t(v) VALUES('four and five')", NULL); hpplite_node_flush_batch(w); }
    if (hpplite_node_anchor(w) != 5 || range_snapshot(l1, rs, 5, &h, zIndex) != 0 || h != 5) { hpplite_node_destroy(w); hpplite_store_close(rs); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("the anchor 4-5 with a snapshot at 5"); }
    n = range_earliest(l1, rs, 5);
    if (n != 3) { printf("(earliest: %lld) ", n); hpplite_node_destroy(w); hpplite_store_close(rs); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("the manifest at 5 names the snapshot at 3 as the earliest kept"); }
    hpplite_node_exec(w, "INSERT INTO t(v) VALUES('six, at the store, unanchored')", NULL);
    if (hpplite_node_flush_batch(w) != 6) { hpplite_node_destroy(w); hpplite_store_close(rs); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("batch 6 at the store"); }
    /* the process dies (no anchor at close); the store goes down; the next run's first anchor cannot read the manifest at 5 */
    hpplite_node_stop_timer(w);
    w->ownsL1 = 0;
    w->l1 = NULL;
    hpplite_node_destroy(w);
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    w = test_open_node(tpath("sd_w.db"), l1, &o);
    if (!w) { hpplite_store_close(rs); FAIL("reopen"); }
    if (hpplite_node_anchor(w) == 6) { hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("the anchor with the store down should not land"); }
    pid = start_store_at(dir, port, storeDir, 0, 0);                 /* the same bytes, back */
    if (pid < 0) { hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("the test's store did not come back"); }
    sleep(2);
    {
        uint64_t a = hpplite_node_anchor(w);
        int got = range_snapshot(l1, rs, 6, &h, zIndex);
        if (a != 6 || got != 0 || h != 6) { printf("(anchor %llu, snapshot %d at %llu, error '%s') ", (unsigned long long)a, got, (unsigned long long)h, w->ctx->zLastError); hpplite_node_destroy(w); hpplite_store_close(rs); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("the anchor 6 with a snapshot at 6"); }
    }
    n = range_earliest(l1, rs, 6);
    printf("(earliest at 6: %lld) ", n);
    hpplite_node_destroy(w);
    hpplite_store_close(rs);
    hpplite_l1_disconnect(l1);
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    if (n != 3) FAIL("the manifest at 6 names the snapshot at 3 as the earliest kept: the restart kept its sidecar's list (the one at 5 is between)");
    PASS();
}


/* the index at the store names a "name" object (#226) */
static int index_has_name(HppliteStore *rs, const char *zIndex) {
    size_t n = 0;
    uint8_t *text = hpplite_store_get(rs, zIndex, &n);
    cJSON *ix = text ? cJSON_ParseWithLength((const char *)text, n) : NULL;
    int has = ix && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(ix, "name"));
    free(text); cJSON_Delete(ix);
    return has;
}

/* 6h. A snapshot whose name object the store did not take is not made (#235): an index without the name would
** leave the rollup unfindable by dbname= once a prune takes batch 1 (#226), with nothing said */
static void test_snapshot_name_refused(const char *dir) {
    TEST("a snapshot whose name object the store refuses is not made (#235): the anchor names none and says why; the next anchor makes one that carries the name");
    int port = test_free_port();
    char url[64], storeDir[600], zIndex[64];
    uint64_t h = 0;
    pid_t pid;
    static const unsigned char kseq[32] = { [31] = 0x7f };
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o;
    HppliteNode *w;
    HppliteStore *rs;
    int got;
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    snprintf(storeDir, sizeof storeDir, "%s/nr_pins", test_dir());
    pid = start_store_at(dir, port, storeDir, 5, 0);              /* puts (a batch's name counts as one, #296): batches 1-3, then the chunk (4), the name (5), the index */
    if (pid < 0) FAIL("the test's store did not start");
    memset(&o, 0, sizeof o);
    o.storeUrl = url; o.nodeId = "nr_w"; o.privkey = kseq; o.snapshotMs = 1; o.keepSeconds = 3600; o.dbName = "named";
    w = test_open_node(tpath("nr_w.db"), l1, &o);
    rs = hpplite_store_open(url, NULL, 0);
    if (!w || !rs) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("open"); }
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    for (int i = 0; i < 3; i++) { hpplite_node_exec(w, "INSERT INTO t(v) VALUES('first three')", NULL); if (hpplite_node_flush_batch(w) == 0) { hpplite_node_destroy(w); hpplite_store_close(rs); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("a batch at the store"); } }
    if (hpplite_node_anchor(w) != 3) { printf("(%s) ", w->ctx->zLastError); hpplite_node_destroy(w); hpplite_store_close(rs); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("the anchor 1-3"); }
    got = range_snapshot(l1, rs, 3, &h, zIndex);
    if (got == 0) { printf("(snapshot at %llu, name %s) ", (unsigned long long)h, index_has_name(rs, zIndex) ? "present" : "missing"); hpplite_node_destroy(w); hpplite_store_close(rs); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("the anchor must name no snapshot: the store refused the name object"); }
    sleep(2);
    hpplite_node_exec(w, "INSERT INTO t(v) VALUES('four')", NULL); hpplite_node_flush_batch(w);
    if (hpplite_node_anchor(w) != 4 || range_snapshot(l1, rs, 4, &h, zIndex) != 0 || h != 4) { printf("(%s) ", w->ctx->zLastError); hpplite_node_destroy(w); hpplite_store_close(rs); kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("the next anchor makes a snapshot at 4"); }
    got = index_has_name(rs, zIndex);
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    hpplite_store_close(rs);
    hpplite_l1_disconnect(l1);
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    if (!got) FAIL("the snapshot at 4 carries the name");
    PASS();
}


/* 9b. A manifest whose hex field is too short (#240): refused as malformed, never read past its end */
static void test_manifest_short_hex(const char *url) {
    TEST("ranges: a manifest with a hex field shorter than its prefix (chain_before: \"\") is refused as malformed, not read past its end (#240)");
    static const unsigned char kseq[32] = { [31] = 0x6a };
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url, .nodeId = "short_w", .privkey = kseq };
    HppliteNode *w = test_open_node(tpath("short_w.db"), l1, &o);
    HppliteRange bad;
    char *tx, *zRef, *text;
    unsigned char digest[32];
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(v); INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(w) < 1 || hpplite_node_anchor(w) != 1) { hpplite_node_destroy(w); FAIL("batch 1 anchored"); }
    hpplite_node_exec(w, "INSERT INTO t VALUES(2)", NULL);
    if (hpplite_node_flush_batch(w) < 1) { hpplite_node_destroy(w); FAIL("batch 2 at the store"); }
    /* the manifest of 2-2 as a sequencer bug might write it: chain_before an empty string; the chain names its digest */
    text = sqlite3_mprintf("{\"v\":1,\"batches\":[{\"h\":2,\"hash\":\"0x%064d\",\"ref\":\"nothing\"}],\"from\":2,\"to\":2,\"chain_before\":\"\",\"chain_after\":\"0x%064d\",\"root\":\"0x%064d\"}", 0, 0, 0);
    hpplite_store_sha256((const uint8_t *)text, strlen(text), digest);
    zRef = hpplite_store_put_at(w->store, 2, (const uint8_t *)text, strlen(text));
    if (!zRef) { printf("(%s) ", hpplite_store_error(w->store)); sqlite3_free(text); hpplite_node_destroy(w); FAIL("the crafted manifest at the store"); }
    sqlite3_free(zRef); sqlite3_free(text);
    memset(&bad, 0, sizeof bad);
    bad.fromHeight = 2; bad.toHeight = 2;
    memcpy(bad.manifestDigest, digest, 32);
    tx = hpplite_l1_commit_range(l1, w->instanceId, &bad);
    if (!tx) { hpplite_node_destroy(w); FAIL("the mock should take the range"); }
    sqlite3_free(tx);
    {
        TestNodeOpts ro = { .storeUrl = url, .verifyOnly = 1, .noSync = 1, .nodeId = "short_r", .privkey = kseq };
        HppliteNode *rp = test_open_node(tpath("short_r.db"), l1, &ro);
        int synced = rp ? hpplite_node_sync_from_l1(rp, 1) : -2;
        char err[300];
        snprintf(err, sizeof err, "%s", rp ? status_text(rp->db, "last_error") : "");
        if (rp) { rp->l1 = NULL; hpplite_node_destroy(rp); }
        printf("(synced %d, '%.80s') ", synced, err);
        if (!strstr(err, "malformed")) { hpplite_node_destroy(w); FAIL("the manifest is refused as malformed"); }
    }
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    PASS();
}


/* a shell command's stdout (curl), its exit status */
static int sh_out(const char *cmd, char *out, size_t n) {
    FILE *f = popen(cmd, "r");
    size_t k;
    if (!f) return -1;
    k = fread(out, 1, n - 1, f);
    out[k] = 0;
    return pclose(f);
}
static void hex0x(const unsigned char *b, size_t n, char *out) {
    out[0] = '0'; out[1] = 'x';
    for (size_t i = 0; i < n; i++) snprintf(out + 2 + 2 * i, 3, "%02x", b[i]);
}
/* the platform's control plane: Bind(da, treasury, now) with the app's half (the node's key, as hpplite_bind_signature gives it)
** and the treasury's half, posted to the store. 0 when the store took it */
static int console_bind(const char *url, HppliteCrypto *cr, const HppliteKeypair *treasury, const unsigned char seqKey[32],
                        const unsigned char da[20], char *out, size_t n) {
    HppliteStore *con = hpplite_store_open(url, seqKey, 0);
    HppliteSignature hs;
    unsigned char seqSig[65], funSig[65], digest[32], v;
    char tre[43], seqHex[133], funHex[133], cmd[1200], json[600];
    uint64_t issued = (uint64_t)time(NULL);
    FILE *f;
    if (!con) return -1;
    hpplite_store_set_bucket(con, da, 0);
    if (hpplite_store_sign_bind(con, treasury->address, issued, seqSig) != 0 || hpplite_store_bind_digest(con, treasury->address, issued, digest) != 0) { hpplite_store_close(con); return -1; }
    hpplite_store_close(con);
    if (hpplite_crypto_sign(cr, treasury, digest, &hs) != 0) return -1;
    hpplite_sig_to_eth_format(&hs, &v, funSig, funSig + 32);
    funSig[64] = v;
    hex0x(treasury->address, 20, tre); hex0x(seqSig, 65, seqHex); hex0x(funSig, 65, funHex);
    snprintf(json, sizeof json, "{\"funder\":\"%s\",\"issuedAt\":%llu,\"sequencerSignature\":\"%s\",\"funderSignature\":\"%s\"}", tre, (unsigned long long)issued, seqHex, funHex);
    f = fopen(tpath("console_bind.json"), "w");
    if (!f) return -1;
    fputs(json, f); fclose(f);
    {
        char daHex[43];
        hex0x(da, 20, daHex);
        snprintf(cmd, sizeof cmd, "curl -s -m 5 -X POST -H 'Content-Type: application/json' --data-binary @%s %s/bucket/%s", tpath("console_bind.json"), url, daHex);
    }
    return sh_out(cmd, out, n) == 0 && strstr(out, "\"covered\"") ? 0 : -1;
}
static int da_of(HppliteL1 *l1, unsigned char da[20], char daHex[43]) {
    char c[43], d[43];
    if (hpplite_l1_get_addresses(l1, c, d) != 0) return -1;
    snprintf(daHex, 43, "%s", d[0] ? d : c);
    return hpplite_hex_to_bytes(daHex + 2, da, 20) == 20 ? 0 : -1;
}

/* the platform's console bounds the bucket's day (#274): Cap(da, writes, bytes, now) signed by the treasury, posted to the store */
static int console_cap(const char *url, HppliteCrypto *cr, const HppliteKeypair *treasury, const unsigned char da[20],
                       uint64_t writes, uint64_t nbytes, int later, char *out, size_t n) {
    HppliteStore *con = hpplite_store_open(url, NULL, 0);
    HppliteSignature hs;
    unsigned char digest[32], sig[65], v;
    char tre[43], sigHex[133], daHex[43], cmd[800], json[400];
    uint64_t issued = (uint64_t)time(NULL) + (uint64_t)later;        /* a newer cap than the last, within the same second */
    FILE *f;
    if (!con) return -1;
    hpplite_store_set_bucket(con, da, 0);
    if (hpplite_store_cap_digest(con, writes, nbytes, issued, digest) != 0) { hpplite_store_close(con); return -1; }
    hpplite_store_close(con);
    if (hpplite_crypto_sign(cr, treasury, digest, &hs) != 0) return -1;
    hpplite_sig_to_eth_format(&hs, &v, sig, sig + 32);
    sig[64] = v;
    hex0x(treasury->address, 20, tre); hex0x(sig, 65, sigHex); hex0x(da, 20, daHex);
    snprintf(json, sizeof json, "{\"funder\":\"%s\",\"writesPerDay\":%llu,\"bytesPerDay\":%llu,\"issuedAt\":%llu,\"signature\":\"%s\"}",
             tre, (unsigned long long)writes, (unsigned long long)nbytes, (unsigned long long)issued, sigHex);
    f = fopen(tpath("console_cap.json"), "w");
    if (!f) return -1;
    fputs(json, f); fclose(f);
    snprintf(cmd, sizeof cmd, "curl -s -m 5 -X POST -H 'Content-Type: application/json' --data-binary @%s %s/bucket/%s/cap", tpath("console_cap.json"), url, daHex);
    return sh_out(cmd, out, n) == 0 && strstr(out, "\"cap\"") ? 0 : -1;
}
/* the bucket's writes today (GET /bucket/<da> today.writes), -1 unknown */
static long long bucket_writes_today(const char *url, const char *daHex) {
    char cmd[300], out[3000];
    const char *p;
    snprintf(cmd, sizeof cmd, "curl -s -m 5 %s/bucket/%s", url, daHex);
    if (sh_out(cmd, out, sizeof out) != 0 || !(p = strstr(out, "\"writes\":"))) return -1;
    return atoll(p + 9);
}

/* 6k. The funder's daily cap (#274): a write past it is 429 at the store; the node keeps the batch pending and says so;
** lifted, the next flush goes through. Red before: the node said 'store answered 429 to the name ...'. */
static void test_bucket_cap(const char *url) {
    TEST("the funder's daily cap (#274): past it the store answers 429, the node keeps the batch pending and says why; lifted, the flush goes through");
    static const unsigned char kseq[32] = { [31] = 0x74 }, ktre[32] = { [31] = 0x75 };
    HppliteCrypto *cr = hpplite_crypto_init();
    HppliteKeypair treasury;
    unsigned char da[20];
    char daHex[43], out[4000];
    const char *why = NULL;
    long long writes;
    HppliteL1 *l1 = hpplite_l1_create_mock();
    if (!cr || hpplite_crypto_keypair_from_privkey(cr, &treasury, ktre) != 0) FAIL("the treasury's key");
    TestNodeOpts o = { .storeUrl = url, .nodeId = "cap_w", .privkey = kseq, .funder = treasury.address };
    HppliteNode *w = test_open_node(tpath("cap_w.db"), l1, &o);
    if (!w) FAIL("open with funder=");
    if (da_of(l1, da, daHex) != 0 || console_bind(url, cr, &treasury, kseq, da, out, sizeof out) != 0) { printf("(%s) ", out); hpplite_node_destroy(w); FAIL("the platform binds the bucket"); }
    hpplite_node_exec(w, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(w) < 1) { printf("(%s) ", status_text(w->db, "last_error")); why = "the first flush"; }
    if (!why && (writes = bucket_writes_today(url, daHex)) < 1) why = "the store should count the day's writes";
    /* a batch is one write at the store, its name (#296): room for one more batch */
    if (!why && console_cap(url, cr, &treasury, da, (uint64_t)writes + 1, 0, 0, out, sizeof out) != 0) { printf("(%s) ", out); why = "the funder's cap"; }
    if (!why) {
        hpplite_node_exec(w, "INSERT INTO t VALUES(2)", NULL);
        if (hpplite_node_flush_batch(w) < 1) { printf("(%s) ", status_text(w->db, "last_error")); why = "the flush within the cap"; }
    }
    if (!why) {
        hpplite_node_exec(w, "INSERT INTO t VALUES(3)", NULL);
        if (hpplite_node_flush_batch(w) >= 1) why = "a flush past the cap should keep the batch pending";
        else if (!strstr(status_text(w->db, "last_error"), "daily cap")) { printf("(%s) ", status_text(w->db, "last_error")); why = "the node should say the bucket's daily cap is reached"; }
    }
    if (!why) {
        /* said once per episode, as the wait on a funder is (#272): not again at every flush while the cap holds (#288) */
        hpplite_error_clear(w->ctx);
        if (hpplite_node_flush_batch(w) >= 1) why = "the batch should still be pending at the cap";
        else if (status_text(w->db, "last_error")[0]) { printf("(said again: %.120s) ", status_text(w->db, "last_error")); why = "the cap's refusal should be said once, not at every flush (#288)"; }
        else if (status_int(w->db, "pending") < 1) why = "the batch is pending while nothing is said";
    }
    if (!why && console_cap(url, cr, &treasury, da, 0, 0, 1, out, sizeof out) != 0) { printf("(%s) ", out); why = "lifting the cap"; }
    if (!why && hpplite_node_flush_batch(w) < 1) { printf("(%s) ", status_text(w->db, "last_error")); why = "the flush once the cap is lifted"; }
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    hpplite_crypto_free(cr);
    if (why) FAIL(why);
    PASS();
}

/* 6k2. One write per batch (#296): with a bucket store and names, a batch goes to the store once - its name, which
** carries the bytes - not as a content put and then a name. Red before: two writes a batch at the store. */
static void test_one_write_per_batch(const char *url) {
    TEST("one store write per batch (#296): a batch is its name write, which carries the bytes; the anchor's ref is their CID, read back by it");
    static const unsigned char kseq[32] = { [31] = 0x96 };
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url, .nodeId = "once_w", .privkey = kseq };
    HppliteNode *w = test_open_node(tpath("once_w.db"), l1, &o);
    unsigned char da[20];
    char daHex[43];
    const char *why = NULL;
    long long w1 = -1, w2 = -1;
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(w) < 1) { printf("(%s) ", status_text(w->db, "last_error")); why = "the first flush"; }
    if (!why && (da_of(l1, da, daHex) != 0 || (w1 = bucket_writes_today(url, daHex)) < 1)) why = "the store counts the bucket's writes";
    if (!why) {
        hpplite_node_exec(w, "INSERT INTO t VALUES(2)", NULL);
        if (hpplite_node_flush_batch(w) < 1) { printf("(%s) ", status_text(w->db, "last_error")); why = "the second flush"; }
    }
    if (!why) {
        w2 = bucket_writes_today(url, daHex);
        printf("(writes at the store for one batch: %lld) ", w2 - w1);
        if (w2 - w1 != 1) why = "a batch should be one write at the store, its name (#296)";
    }
    if (!why && hpplite_node_anchor(w) != 2) { printf("(%s) ", status_text(w->db, "last_error")); why = "the anchor of the two batches, by their CIDs"; }
    if (!why) {
        TestNodeOpts ro = { .storeUrl = url, .verifyOnly = 1, .nodeId = "once_r", .privkey = kseq };
        HppliteNode *r = test_open_node(tpath("once_r.db"), l1, &ro);
        int rows = r ? count_rows(r->db, "t") : -1, same = r ? same_root(w, r) : 0;
        printf("(a verifier: rows %d, same root %d) ", rows, same);
        if (r) { r->l1 = NULL; hpplite_node_destroy(r); }
        if (rows != 2 || !same) why = "a verifier rebuilds both batches from the store";
    }
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 6l. A store's own rate limit (#280): a 429 that is not bucket_capped. The node keeps the batch pending and names
** the rate limit in the store's words, not a daily cap nobody set; the next flush goes through. Red before: 'the
** bucket's daily cap is reached, set by its funder; the write waits for the next window (429 bucket_capped)'. */
static void test_store_rate_limited(const char *dir) {
    TEST("a store's rate limit (#280): a 429 that is not bucket_capped keeps the batch pending and is named as the store's rate limit, not a daily cap; the next flush goes through");
    int port = test_free_port();
    char url[64], storeDir[600];
    const char *why = NULL, *err;
    pid_t pid;
    static const unsigned char kseq[32] = { [31] = 0x76 };
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o;
    HppliteNode *w;
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    snprintf(storeDir, sizeof storeDir, "%s/rl_pins", test_dir());
    pid = start_store_at(dir, port, storeDir, 0, 2);             /* the second content put: batch 2's bytes */
    if (pid < 0) FAIL("the test's store did not start");
    memset(&o, 0, sizeof o);
    o.storeUrl = url; o.nodeId = "rl_w"; o.privkey = kseq;
    w = test_open_node(tpath("rl_w.db"), l1, &o);
    if (!w) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("open"); }
    hpplite_node_exec(w, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(w) < 1) { printf("(%s) ", status_text(w->db, "last_error")); why = "the first flush"; }
    if (!why) {
        hpplite_node_exec(w, "INSERT INTO t VALUES(2)", NULL);
        if (hpplite_node_flush_batch(w) >= 1) why = "a flush the store answers 429 should keep the batch pending";
        else {
            err = status_text(w->db, "last_error");
            if (strstr(err, "daily cap") || !strstr(err, "rate limit") || !strstr(err, "too many requests")) { printf("(%s) ", err); why = "the node should name the store's rate limit, not a daily cap"; }
        }
    }
    if (!why && hpplite_node_flush_batch(w) < 1) { printf("(%s) ", status_text(w->db, "last_error")); why = "the next flush"; }
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    if (why) FAIL(why);
    PASS();
}

/* 6j. funder= (#272): the bucket's account is a platform's treasury, bound and permitted from its control plane; the node
** derives no funding key, binds nothing, gives no permit, takes the bucket as the platform left it, and a 402 is
** "waiting for the funder", not a cap refusal. Red before: the node bound the bucket to its own derived wallet. */
static void test_external_funder(const char *url, const char *dir) {
    TEST("funder= (#272): the node binds nothing and gives no permit; the platform's binding to its treasury is taken as found; a 402 waits on the funder, pays nothing");
    static const unsigned char kseq[32] = { [31] = 0x72 }, ktre[32] = { [31] = 0x73 };
    HppliteCrypto *cr = hpplite_crypto_init();
    HppliteKeypair treasury;
    unsigned char da[20], got[20], derived[20];
    char daHex[43], tre[43], derivedHex[43], cmd[600], out[4000];
    const char *why = NULL;
    HppliteL1 *l1 = hpplite_l1_create_mock();
    if (!cr || hpplite_crypto_keypair_from_privkey(cr, &treasury, ktre) != 0) FAIL("the treasury's key");
    hex0x(treasury.address, 20, tre);
    TestNodeOpts o = { .storeUrl = url, .nodeId = "ext_w", .privkey = kseq, .funder = treasury.address };
    HppliteNode *w = test_open_node(tpath("ext_w.db"), l1, &o);
    if (!w) FAIL("open with funder=");
    if (da_of(l1, da, daHex) != 0) { hpplite_node_destroy(w); FAIL("the rollup's address"); }
    {   /* the node's own derived wallet: it must never appear at the store */
        HppliteStore *own = hpplite_store_open(url, kseq, 0);
        if (!own || hpplite_store_funder(own, derived) != 0) { if (own) hpplite_store_close(own); hpplite_node_destroy(w); FAIL("the derived wallet"); }
        hpplite_store_close(own);
        hex0x(derived, 20, derivedHex);
    }
    /* 1. unbound: the node writes nothing and binds nothing; it waits for the funder */
    hpplite_node_exec(w, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(w) >= 1) why = "the node should bind nothing and keep the batch pending";
    if (!why) {
        snprintf(cmd, sizeof cmd, "curl -s -m 5 -o /dev/null -w '%%{http_code}' %s/bucket/%s", url, daHex);
        sh_out(cmd, out, sizeof out);
        if (strcmp(out, "404") != 0) { printf("(bucket %s) ", out); why = "no bucket should exist before the platform binds it"; }
    }
    if (!why && !strstr(status_text(w->db, "last_error"), "waiting for the funder")) { printf("(%s) ", status_text(w->db, "last_error")); why = "the node should say it waits for the funder"; }
    if (!why && hpplite_store_funder(w->store, got) == 0 && memcmp(got, treasury.address, 20) != 0) why = "store_funder should be the external funder";
    /* 2. the platform binds the bucket to its treasury; the node's next flush goes through, the binding untouched */
    if (!why && console_bind(url, cr, &treasury, kseq, da, out, sizeof out) != 0) { printf("(%s) ", out); why = "the console's binding"; }
    if (!why && hpplite_node_flush_batch(w) < 1) { printf("(%s) ", status_text(w->db, "last_error")); why = "the flush after the platform's binding"; }
    if (!why) {
        snprintf(cmd, sizeof cmd, "curl -s -m 5 %s/bucket/%s", url, daHex);
        sh_out(cmd, out, sizeof out);
        if (!strstr(out, tre)) { printf("(%s) ", out); why = "the bucket should stay bound to the treasury (red: rebound to the node's derived wallet)"; }
    }
    if (!why) {
        snprintf(cmd, sizeof cmd, "curl -s -m 5 -o /dev/null -w '%%{http_code}' %s/account/%s", url, derivedHex);
        sh_out(cmd, out, sizeof out);
        if (strcmp(out, "200") == 0) why = "the node's derived wallet should have no account at the store (no binding, no permit from it)";
    }
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);

    /* 3. a priced store (no chain, nothing can pull): the 402 is the funder's to lift; the node pays nothing and says so */
    {
        int port = test_free_port(), up = 0;
        char portStr[16], script[600], url2[64], keyPath[600];
        snprintf(portStr, sizeof portStr, "%d", port);
        snprintf(script, sizeof script, "%s/hpplite_store.py", dir);
        snprintf(keyPath, sizeof keyPath, "%s/signer.key", test_dir());
        pid_t pid = fork();
        if (pid == 0) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
            execlp("python3", "python3", "-I", script, "--port", portStr, "--dir", tpath("ext_paid_pins"), "--signer-key", keyPath,
                   "--pay-to", "0x1111111111111111111111111111111111111111", "--facilitator", "http://127.0.0.1:1",
                   "--price-per-mib-month", "30", "--price-per-write", "10", (char *)NULL);
            _exit(127);
        }
        for (int i = 0; i < 50 && !up; i++) {
            if (!test_child_alive(pid)) break;
            snprintf(cmd, sizeof cmd, "curl -s -m 2 http://127.0.0.1:%d/status 2>/dev/null | grep -q pricePerWrite", port);
            up = system(cmd) == 0;
            if (!up) usleep(200 * 1000);
        }
        if (!up) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); FAIL("the paid store did not start"); }
        snprintf(url2, sizeof url2, "http://127.0.0.1:%d", port);
        HppliteL1 *l1b = hpplite_l1_create_mock();
        TestNodeOpts o2 = { .storeUrl = url2, .nodeId = "ext_w2", .privkey = kseq, .funder = treasury.address };
        HppliteNode *w2 = test_open_node(tpath("ext_w2.db"), l1b, &o2);
        if (!w2) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); FAIL("open on the paid store"); }
        if (da_of(l1b, da, daHex) != 0 || console_bind(url2, cr, &treasury, kseq, da, out, sizeof out) != 0) { printf("(%s) ", out); why = "the console's binding at the paid store"; }
        if (!why) {
            hpplite_node_exec(w2, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL);
            if (hpplite_node_flush_batch(w2) >= 1) why = "an uncovered account should keep the batch pending";
        }
        if (!why) {
            const char *e = status_text(w2->db, "last_error");
            if (!strstr(e, "waiting for the funder") || strstr(e, "HPPLITE_STORE_CAP")) { printf("(%s) ", e); why = "the 402 should read as waiting for the funder, not a cap refusal"; }
        }
        if (!why && hpplite_store_spent(w2->store, NULL) != 0) why = "the node should pay nothing";
        hpplite_node_destroy(w2);
        hpplite_l1_disconnect(l1b);
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        if (why) FAIL(why);
    }
    hpplite_crypto_free(cr);
    PASS();
}

/* the objects the store holds in the rollup's bucket (GET /bucket/<da>), -1 unknown */
static long long bucket_objects(const char *url, HppliteL1 *l1) {
    char c[43], d[43], cmd[300], out[2000] = "";
    const char *p;
    FILE *f;
    if (hpplite_l1_get_addresses(l1, c, d) != 0) return -1;
    snprintf(cmd, sizeof cmd, "curl -s -m 5 %s/bucket/%s", url, d[0] ? d : c);
    f = popen(cmd, "r");
    if (!f) return -1;
    (void)!fread(out, 1, sizeof out - 1, f);
    pclose(f);
    p = strstr(out, "\"objects\":");
    return p ? atoll(p + 10) : -1;
}

/* 6i. A snapshot the chain refused is reused at the retry (#237): the same height and root, so the same bytes; a
** refused attempt uploaded the whole image once, not again at every retry */
static void test_snapshot_reused_after_refusal(const char *url) {
    TEST("a snapshot made for an anchor the chain refused is reused when the retry anchors the same height and root (#237): no second upload");
    static const unsigned char kseq[32] = { [31] = 0x6b };
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url, .nodeId = "reuse_w", .privkey = kseq, .snapshotMs = 1 };
    HppliteNode *w = test_open_node(tpath("reuse_w.db"), l1, &o);
    HppliteStore *rs = hpplite_store_open(url, NULL, 0);
    long long before, refused, after;
    uint64_t h = 0;
    char zIndex[64];
    if (!w || !rs) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    for (int i = 0; i < 3; i++) { hpplite_node_exec(w, "INSERT INTO t(v) VALUES('three')", NULL); hpplite_node_flush_batch(w); }
    before = bucket_objects(url, l1);
    hpplite_l1_mock_set_fail_submit(l1, 1);
    if (hpplite_node_anchor(w) != 0) { hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("the mock should refuse the anchor"); }
    refused = bucket_objects(url, l1);                    /* the image's chunk, its index and the manifest went up */
    hpplite_l1_mock_set_fail_submit(l1, 0);
    if (hpplite_node_anchor(w) != 3 || range_snapshot(l1, rs, 3, &h, zIndex) != 0 || h != 3) { printf("(%s) ", w->ctx->zLastError); hpplite_node_destroy(w); hpplite_store_close(rs); FAIL("the retry anchors 1-3 with a snapshot at 3"); }
    after = bucket_objects(url, l1);
    printf("(objects: %lld, after the refused anchor %lld, after the retry %lld) ", before, refused, after);
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    hpplite_store_close(rs);
    hpplite_l1_disconnect(l1);
    if (before < 0 || refused <= before) FAIL("the refused anchor uploaded the snapshot and the manifest");
    if (after - refused > 1) FAIL("the retry should put at most its manifest: the snapshot at 3 is the one already there");   /* the same manifest bytes: the store holds them once */
    PASS();
}

/* anchorclose=off (#266): the close flushes what is pending to the store and anchors nothing;
** the anchor is the timer's (anchor=) or hpplite_anchor()'s. The next run finds the rows waiting */
static void test_no_anchor_at_close(const char *url) {
    TEST("anchorclose=off: the close puts the pending batch at the store and anchors nothing; the next run finds both waiting and anchors them on request; its close has nothing left (#266)");
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url, .nodeId = "noanchor_w", .noAnchorAtClose = 1 };
    HppliteNode *w = test_open_node(tpath("na_w.db"), l1, &o);
    uint64_t last = 0, total = 0;
    unsigned char latest[32];
    HppliteRange r;
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(v)", NULL);
    hpplite_node_exec(w, "INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(w) < 1) { printf("(%s) ", status_text(w->db, "last_error")); hpplite_node_destroy(w); FAIL("flush 1"); }
    hpplite_node_exec(w, "INSERT INTO t VALUES(2)", NULL);     /* pending at the close: the close puts it at the store */
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    hpplite_l1_get_da_state(l1, &last, &total, latest);
    if (last != 0) { printf("(chain %llu) ", (unsigned long long)last); hpplite_l1_disconnect(l1); FAIL("the close should anchor nothing"); }
    w = test_open_node(tpath("na_w.db"), l1, &o);
    if (!w) { hpplite_l1_disconnect(l1); FAIL("reopen"); }
    {
        long long rows = scalar(w->ctx->bkDb, "SELECT count(*) FROM _hpplite_unanchored");
        if (rows != 2 || status_int(w->db, "height") != 3 || status_int(w->db, "anchored") != 0) {   /* height: the next to post */
            printf("(rows %lld, height %lld, anchored %lld) ", rows, (long long)status_int(w->db, "height"), (long long)status_int(w->db, "anchored"));
            hpplite_node_destroy(w); FAIL("both batches at the store and waiting for the chain after the restart");
        }
    }
    if (hpplite_node_anchor(w) != 2 || hpplite_l1_get_range(l1, 1, &r) != 0 || r.fromHeight != 1 || r.toHeight != 2) { printf("(%s) ", status_text(w->db, "last_error")); hpplite_node_destroy(w); FAIL("hpplite_anchor() anchors them as one range 1-2"); }
    hpplite_l1_range_clear(&r);
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    hpplite_l1_get_da_state(l1, &last, &total, latest);
    hpplite_l1_disconnect(l1);
    if (last != 2) FAIL("the second close has nothing to anchor and sends nothing");
    PASS();
}

static void test_ranges_restart(const char *url) {
    TEST("ranges: a run that dies with batches at the store leaves rows for the chain; the next run anchors them before its own; a manifest with a swapped hash is refused");
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url, .nodeId = "ranges_rs" };
    HppliteNode *w = test_open_node(tpath("rs_w.db"), l1, &o);
    uint64_t last = 0, total = 0;
    unsigned char latest[32];
    HppliteRange r;
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(v)", NULL);
    hpplite_node_exec(w, "INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(w) < 1) { hpplite_node_destroy(w); FAIL("flush 1"); }
    hpplite_node_exec(w, "INSERT INTO t VALUES(2)", NULL);
    if (hpplite_node_flush_batch(w) < 1) { hpplite_node_destroy(w); FAIL("flush 2"); }
    if (scalar(w->ctx->bkDb, "SELECT count(*) FROM _hpplite_unanchored") != 2) { hpplite_node_destroy(w); FAIL("two rows wait for the chain"); }
    /* the process dies: no close, no anchor */
    hpplite_node_stop_timer(w);
    w->ownsL1 = 0;
    w->l1 = NULL;                                   /* the mock survives; the node is torn down without reaching the chain */
    hpplite_node_destroy(w);
    hpplite_l1_get_da_state(l1, &last, &total, latest);
    if (last != 0) { hpplite_l1_disconnect(l1); FAIL("nothing reached the chain"); }
    /* the next run: the rows are there, the anchor takes them with the new batch */
    w = test_open_node(tpath("rs_w.db"), l1, &o);
    if (!w) { hpplite_l1_disconnect(l1); FAIL("reopen"); }
    if (scalar(w->ctx->bkDb, "SELECT count(*) FROM _hpplite_unanchored") != 2 || status_int(w->db, "height") != 3) { printf("(rows %lld, height %lld) ", (long long)scalar(w->ctx->bkDb, "SELECT count(*) FROM _hpplite_unanchored"), (long long)status_int(w->db, "height")); hpplite_node_destroy(w); FAIL("the rows survived the restart, the height too"); }
    hpplite_node_exec(w, "INSERT INTO t VALUES(3)", NULL);
    if (hpplite_node_flush_batch(w) < 1) { printf("(%s) ", status_text(w->db, "last_error")); hpplite_node_destroy(w); FAIL("flush 3 after the restart"); }
    if (hpplite_node_anchor(w) != 3 || hpplite_l1_get_range(l1, 1, &r) != 0 || r.fromHeight != 1 || r.toHeight != 3) { printf("(%s) ", status_text(w->db, "last_error")); hpplite_node_destroy(w); FAIL("one range 1-3"); }
    hpplite_l1_range_clear(&r);
    if (scalar(w->ctx->bkDb, "SELECT count(*) FROM _hpplite_unanchored") != 0) { hpplite_node_destroy(w); FAIL("the rows are cleared once anchored"); }
    {
        /* a manifest that does not fold to the anchored chain hash: a range whose digest names other bytes */
        HppliteRange bad;
        char *tx;
        hpplite_node_exec(w, "INSERT INTO t VALUES(4)", NULL);
        if (hpplite_node_flush_batch(w) < 1) { hpplite_node_destroy(w); FAIL("flush 4"); }
        memset(&bad, 0, sizeof bad);
        bad.fromHeight = 4; bad.toHeight = 4;
        memset(bad.chainHashAfter, 0x11, 32); memset(bad.stateRoot, 0x22, 32); memset(bad.manifestDigest, 0x33, 32);
        tx = hpplite_l1_commit_range(l1, w->instanceId, &bad);     /* the mock takes what the sequencer signs, as the contract would */
        if (!tx) { hpplite_node_destroy(w); FAIL("the mock should take the range"); }
        sqlite3_free(tx);
        {
            TestNodeOpts ro = { .storeUrl = url, .verifyOnly = 1, .noSync = 1, .nodeId = "ranges_rs_r" };
            HppliteNode *rp = test_open_node(tpath("rs_r.db"), l1, &ro);
            int synced = rp ? hpplite_node_sync_from_l1(rp, 1) : -2;
            int rows = rp ? count_rows(rp->db, "t") : -1;
            char err[300];
            snprintf(err, sizeof err, "%s", rp ? status_text(rp->db, "last_error") : "");
            if (rp) { rp->l1 = NULL; hpplite_node_destroy(rp); }
            if (rows != 3 || synced != 3 || !strstr(err, "manifest")) { printf("(synced %d, rows %d, error '%s') ", synced, rows, err); hpplite_node_destroy(w); FAIL("the replica rebuilds 1-3 and refuses the range whose manifest the store does not hold"); }
        }
    }
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    PASS();
}


/* 8. The timer anchors (#164): a node opened by URI (the way an application opens one) with a
** store and anchor= anchors on its own; nothing calls hpplite_anchor(). */
static void test_anchor_timer(const char *url) {
    TEST("ranges: a node opened by URI with store= and anchor=1s anchors from its timer, without hpplite_anchor()");
    char uri[700];
    sqlite3 *db;
    sqlite3_int64 anchored = -1;
    int i;
    snprintf(uri, sizeof uri, "file:%s?hpplite=on&rpc=mock&contract=0x0000000000000000000000000000000000000001"
             "&privkey=0x1111111111111111111111111111111111111111111111111111111111111111&store=%s&interval=500ms&anchor=1s&datadir=%s",
             tpath("tm_w.db"), url, test_dir());
    db = hpplite_open(uri);
    if (!db) FAIL("open by URI on the mock chain");
    sqlite3_exec(db, "CREATE TABLE t(v); INSERT INTO t VALUES(1)", NULL, NULL, NULL);
    if (scalar(db, "SELECT hpplite_flush()") < 1) { printf("(%s) ", status_text(db, "last_error")); sqlite3_close(db); FAIL("flush through the store"); }
    for (i = 0; i < 80 && anchored < 1; i++) { usleep(100000); anchored = status_int(db, "anchored"); }
    if (anchored != 1) { printf("(anchored %lld after 8 s, last_error '%s') ", (long long)anchored, status_text(db, "last_error")); sqlite3_close(db); FAIL("the timer should have anchored the batch within a few seconds"); }
    sqlite3_close(db);
    PASS();
}


/* #270: the timer's first-tick decision ('a previous run left rows: anchor them soon, else the cadence') must
** not count a batch this run posted in the same tick, and with anchorclose=off the rows the last run left wait
** for the cadence like everything else; with anchorclose on, a run that died before its close-anchor is still
** anchored soon */
static sqlite3_int64 wait_anchored(HppliteNode *w, int tenths) {
    sqlite3_int64 anchored = 0;
    for (int i = 0; i < tenths; i++) { usleep(100000); anchored = status_int(w->db, "anchored"); if (anchored > 0) break; }
    return anchored;
}
static void test_first_tick_anchor(const char *url) {
    TEST("the timer's first tick: a batch written before it is not anchored 'soon' (#270); with anchorclose=off a wake leaves the last run's rows to the cadence; with anchorclose on it anchors them soon");
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url, .nodeId = "firsttick_w", .intervalMs = 1000, .anchorMs = 60000, .noAnchorAtClose = 1 };
    HppliteNode *w = test_open_node(tpath("ft_w.db"), l1, &o);
    sqlite3_int64 anchored, height;
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(v); INSERT INTO t VALUES(1)", NULL);         /* before the first tick (1 s) */
    hpplite_node_start_timer(w);
    anchored = wait_anchored(w, 40);
    height = status_int(w->db, "height");
    if (height != 2) { printf("(height %lld, last_error '%s') ", (long long)height, status_text(w->db, "last_error")); hpplite_node_destroy(w); FAIL("the first tick posts the batch to the store"); }
    if (anchored != 0) { hpplite_node_destroy(w); FAILF("a batch posted by the first tick was anchored at once (anchored %lld); the cadence is a minute", (long long)anchored); }
    w->ownsL1 = 0;
    hpplite_node_destroy(w);                                                             /* anchorclose=off: the row stays at the store */
    w = test_open_node(tpath("ft_w.db"), l1, &o);
    if (!w) { hpplite_l1_disconnect(l1); FAIL("reopen"); }
    if (status_int(w->db, "anchored") != 0) { hpplite_node_destroy(w); FAIL("nothing anchored across the close"); }
    hpplite_node_start_timer(w);
    anchored = wait_anchored(w, 40);
    if (anchored != 0) { hpplite_node_destroy(w); FAILF("with anchorclose=off the wake anchored what the last run left (anchored %lld); it should wait for the cadence", (long long)anchored); }
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    o.noAnchorAtClose = 0;                                                               /* anchorclose on: a run that never anchored left rows; they are anchored soon */
    w = test_open_node(tpath("ft_w.db"), l1, &o);
    if (!w) { hpplite_l1_disconnect(l1); FAIL("reopen with anchorclose on"); }
    hpplite_node_start_timer(w);
    anchored = wait_anchored(w, 60);
    if (anchored != 1) { printf("(anchored %lld after 6 s, last_error '%s') ", (long long)anchored, status_text(w->db, "last_error")); hpplite_node_destroy(w); FAIL("with anchorclose on, the rows a previous run left should be anchored soon"); }
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 9. Named entries (#165): a batch under <da>-<height> with the sequencer's signature; the first
** signer holds a rollup's names at the pin service; a reader recovers the signer. */
static void test_named(const char *url) {
    TEST("named entries: a signed batch under <da>-<height> is the write (no content put first), read back with its signature and by its CID; another key is refused for that rollup, a bad signature too");
    static const unsigned char k1[32] = { [31] = 0x11 }, k2[32] = { [31] = 0x22 };
    unsigned char da[20], da2[20], inst[32], me1[20], me2[20], who[20];
    uint8_t sig[65], sig2[65], got[65], bad[65];
    const uint8_t data[] = "a batch under its name";
    size_t n = 0;
    uint8_t *back;
    HppliteStore *s1 = hpplite_store_open(url, k1, 0), *s2 = hpplite_store_open(url, k2, 0);
    memset(da, 0xda, 20); memset(da2, 0xdb, 20); memset(inst, 0x1e, 32);
    if (!s1 || !s2) FAIL("open with keys");
    if (hpplite_store_sign_batch(s1, da, 181228, inst, 7, data, sizeof data, sig) != 0) FAIL("sign");
    hpplite_store_set_bucket(s1, da, 0);
    hpplite_store_set_bucket(s2, da, 0);
    if (hpplite_store_put_named(s1, "dadadadadadadadadadadadadadadadadadadada-7", inst, data, sizeof data, sig) != 0) { printf("(%s) ", hpplite_store_error(s1)); FAIL("put named"); }
    back = hpplite_store_get_named(s1, "dadadadadadadadadadadadadadadadadadadada-7", &n, got, NULL);
    if (!back || n != sizeof data || memcmp(back, data, n) != 0 || memcmp(got, sig, 65) != 0) { printf("(%s) ", hpplite_store_error(s1)); free(back); FAIL("get named: the bytes and the signature"); }
    free(back);
    {
        /* the name's bytes are content too: read by their CID */
        unsigned char sh[32];
        char cid[64];
        hpplite_store_sha256(data, sizeof data, sh);
        hpplite_store_cid_from_sha256(sh, cid);
        back = hpplite_store_get(s1, cid, &n);
        if (!back || n != sizeof data || memcmp(back, data, n) != 0) { free(back); FAIL("a name's bytes by their CID"); }
        free(back);
    }
    if (hpplite_store_batch_signer(da, 181228, inst, 7, data, sizeof data, sig, me1) != 0) FAIL("the signer recovers");
    {
        /* and the service refuses a signature made for another chain: its domain is the chain it serves */
        uint8_t other[65];
        if (hpplite_store_sign_batch(s1, da, 31337, inst, 7, data, sizeof data, other) != 0 || hpplite_store_put_named(s1, "dadadadadadadadadadadadadadadadadadadada-70", inst, data, sizeof data, other) == 0) FAIL("a signature for another chain must be refused");
    }
    if (hpplite_store_sign_batch(s2, da, 181228, inst, 8, data, sizeof data, sig2) != 0 || hpplite_store_batch_signer(da, 181228, inst, 8, data, sizeof data, sig2, me2) != 0) FAIL("sign with the second key");
    if (memcmp(me1, me2, 20) == 0) FAIL("two keys, two signers");
    /* the second key on the first rollup's names: refused; on its own rollup: taken */
    if (hpplite_store_put_named(s2, "dadadadadadadadadadadadadadadadadadadada-8", inst, data, sizeof data, sig2) == 0) { FAIL("another signer must be refused for a rollup whose bucket is held"); }
    printf("(another key: %s) ", hpplite_store_error(s2));
    hpplite_store_set_bucket(s2, da2, 0);
    if (hpplite_store_sign_batch(s2, da2, 181228, inst, 1, data, sizeof data, sig2) != 0 || hpplite_store_put_named(s2, "dbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdbdb-1", inst, data, sizeof data, sig2) != 0) { printf("(%s) ", hpplite_store_error(s2)); FAIL("the second key on its own rollup"); }
    memcpy(bad, sig, 65); bad[10] ^= 1;
    if (hpplite_store_put_named(s1, "dadadadadadadadadadadadadadadadadadadada-9", inst, data, sizeof data, bad) == 0) FAIL("a signature that does not recover to the holder must be refused");
    if (hpplite_store_batch_signer(da, 181228, inst, 7, data, sizeof data, bad, who) == 0 && memcmp(who, me1, 20) == 0) FAIL("a changed signature must not recover to the signer");
    if (hpplite_store_get_named(s1, "dadadadadadadadadadadadadadadadadadadada-9", &n, got, NULL) != NULL) FAIL("nothing under a refused name");
    if (hpplite_store_get_named(s1, "not a name", &n, got, NULL) != NULL) FAIL("a malformed name is refused by the client");
    hpplite_store_close(s1); hpplite_store_close(s2);
    PASS();
}


/* 10. Recovery (#167): a sequencer that dies with batches at the store and none anchored; another
** with the same key on a fresh file reads them by name, applies them, anchors them; nothing lost. */
static void test_recovery(const char *url) {
    TEST("recovery: the sequencer dies with three batches at the store, none anchored; a new one on a fresh file with the same key recovers them by name, anchors them, a replica sees all of it");
    static const unsigned char kseq[32] = { [31] = 0x79 };
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url, .nodeId = "rec_a", .privkey = kseq };
    HppliteNode *w = test_open_node(tpath("rec_a.db"), l1, &o), *w2;
    uint64_t last = 0, total = 0;
    unsigned char latest[32];
    int i;
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    for (i = 0; i < 3; i++) {
        char sql[80];
        snprintf(sql, sizeof sql, "INSERT INTO t(v) VALUES('before the crash %d')", i);
        hpplite_node_exec(w, sql, NULL);
        if (hpplite_node_flush_batch(w) < 1) { printf("(%s) ", status_text(w->db, "last_error")); hpplite_node_destroy(w); FAIL("a flush"); }
    }
    /* the machine is gone: no close, no anchor, the file with it */
    hpplite_node_stop_timer(w);
    w->ownsL1 = 0;
    w->l1 = NULL;
    hpplite_node_destroy(w);
    hpplite_l1_get_da_state(l1, &last, &total, latest);
    if (last != 0) { hpplite_l1_disconnect(l1); FAIL("nothing reached the chain"); }
    hpplite_l1_mock_advance_time(l1, 400);                        /* the dead one's lease runs out */
    {
        TestNodeOpts o2 = { .storeUrl = url, .nodeId = "rec_b", .privkey = kseq };
        w2 = test_open_node(tpath("rec_b.db"), l1, &o2);
    }
    if (!w2) { hpplite_l1_disconnect(l1); FAIL("open the successor"); }
    if (status_int(w2->db, "height") != 4 || count_rows(w2->db, "t") != 3 || scalar(w2->ctx->bkDb, "SELECT count(*) FROM _hpplite_unanchored") != 3) {
        printf("(height %lld, rows %d, unanchored %lld, last_error '%s') ", (long long)status_int(w2->db, "height"), count_rows(w2->db, "t"),
               (long long)scalar(w2->ctx->bkDb, "SELECT count(*) FROM _hpplite_unanchored"), status_text(w2->db, "last_error"));
        w2->ownsL1 = 0; hpplite_node_destroy(w2); hpplite_l1_disconnect(l1); FAIL("the successor should hold the three batches the store had");
    }
    hpplite_node_exec(w2, "INSERT INTO t(v) VALUES('after')", NULL);
    if (hpplite_node_flush_batch(w2) < 1 || hpplite_node_anchor(w2) != 4) {
        printf("(%s) ", status_text(w2->db, "last_error")); w2->ownsL1 = 0; hpplite_node_destroy(w2); hpplite_l1_disconnect(l1); FAIL("its own batch, then one anchor 1-4");
    }
    {
        TestNodeOpts ro = { .storeUrl = url, .verifyOnly = 1, .nodeId = "rec_r", .privkey = kseq };
        HppliteNode *r = test_open_node(tpath("rec_r.db"), l1, &ro);
        int rows = r ? count_rows(r->db, "t") : -1, same = r ? same_root(w2, r) : 0;
        if (r) { r->l1 = NULL; hpplite_node_destroy(r); }
        if (rows != 4 || !same) { printf("(replica rows %d, same root %d) ", rows, same); w2->ownsL1 = 0; hpplite_node_destroy(w2); hpplite_l1_disconnect(l1); FAIL("a replica sees the recovered batches and the new one"); }
    }
    w2->ownsL1 = 0;
    hpplite_node_destroy(w2);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 11. The soft tier (#166): a verifier with the store follows the sequencer's batches as they reach the
** store, before any anchor, and they turn hard when the anchor names what it applied. */
static void test_soft_replica(const char *url) {
    TEST("soft replica: a verifier applies the store's batches past the anchored height (soft_height), then the anchor makes them hard (height)");
    static const unsigned char kseq[32] = { [31] = 0x66 };
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url, .nodeId = "soft_w", .privkey = kseq };
    HppliteNode *w = test_open_node(tpath("soft_w.db"), l1, &o), *r;
    int i;
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    for (i = 0; i < 2; i++) {
        hpplite_node_exec(w, "INSERT INTO t(v) VALUES('soft')", NULL);
        if (hpplite_node_flush_batch(w) < 1) { printf("(%s) ", status_text(w->db, "last_error")); hpplite_node_destroy(w); FAIL("a flush"); }
    }
    {
        TestNodeOpts ro = { .storeUrl = url, .verifyOnly = 1, .nodeId = "soft_r", .privkey = kseq };
        r = test_open_node(tpath("soft_r.db"), l1, &ro);
    }
    if (!r) { w->ownsL1 = 0; hpplite_node_destroy(w); hpplite_l1_disconnect(l1); FAIL("open the verifier"); }
#define SOFT_DONE() do { r->l1 = NULL; hpplite_node_destroy(r); w->ownsL1 = 0; hpplite_node_destroy(w); hpplite_l1_disconnect(l1); } while (0)
    long long hard = status_int(r->db, "height"), soft = status_int(r->db, "soft_height");
    int rows = count_rows(r->db, "t");
    printf("(before any anchor: height %lld, soft_height %lld, rows %d) ", hard, soft, rows);
    if (hard != 1 || soft != 3 || rows != 2) { SOFT_DONE(); FAIL("two batches at the store, none anchored: soft to 3, hard at 1"); }
    hpplite_node_exec(w, "INSERT INTO t(v) VALUES('a third')", NULL);
    if (hpplite_node_flush_batch(w) < 1) { SOFT_DONE(); FAIL("the third flush"); }
    hpplite_node_sync_from_l1(r, 1);
    if (status_int(r->db, "soft_height") != 4 || count_rows(r->db, "t") != 3) { SOFT_DONE(); FAIL("the next batch follows within a sync"); }
    if (hpplite_node_anchor(w) != 3) { printf("(%s) ", status_text(w->db, "last_error")); SOFT_DONE(); FAIL("the anchor 1-3"); }
    hpplite_node_sync_from_l1(r, 1);
    hard = status_int(r->db, "height"); soft = status_int(r->db, "soft_height");
    printf("(after the anchor: height %lld, soft_height %lld, state %s) ", hard, soft, status_text(r->db, "state"));
    int same = same_root(w, r);
    if (hard != 4 || soft != 4 || !same || r->state == HPPLITE_STATE_ERROR) { SOFT_DONE(); FAIL("the anchor names what was applied: all hard, the same root"); }
    /* a batch past the anchor: chain_hash stays the hash at the anchored height, as height does - the one the
    ** contract holds and hpplite-verify compares it with (#286) */
    hpplite_node_exec(w, "INSERT INTO t(v) VALUES('past the anchor')", NULL);
    if (hpplite_node_flush_batch(w) < 1) { SOFT_DONE(); FAIL("the fourth flush"); }
    hpplite_node_sync_from_l1(r, 1);
    {
        HppliteRange r3;
        char want[67] = "0x";
        memset(&r3, 0, sizeof r3);
        if (hpplite_l1_get_range(l1, 3, &r3) != 0 || r3.toHeight != 3) { SOFT_DONE(); FAIL("the range the anchor wrote, 1-3"); }
        hpplite_to_hex(r3.chainHashAfter, 32, want + 2);
        hpplite_l1_range_clear(&r3);
        hard = status_int(r->db, "height"); soft = status_int(r->db, "soft_height");
        if (hard != 4 || soft != 5 || strcasecmp(status_text(r->db, "chain_hash"), want) != 0) {
            printf("(height %lld, soft_height %lld, chain_hash %s, the anchor's %s) ", hard, soft, status_text(r->db, "chain_hash"), want);
            SOFT_DONE();
            FAIL("with a batch past the anchor, chain_hash is the hash at the anchored height, the contract's (#286)");
        }
    }
    SOFT_DONE();
#undef SOFT_DONE
    PASS();
}

/* 12. ... and when the chain commits another batch than the one the store served (the store lost it, a
** successor wrote its own at that height), the verifier says its soft state is not the rollup's */
static void test_soft_disagreement(const char *url) {
    TEST("soft replica: the store served a batch the chain then did not commit (a successor wrote its own at that height): the verifier says so and rebuilds itself to what the chain committed");
    static const unsigned char kseq[32] = { [31] = 0x67 };
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url, .nodeId = "dis_a", .privkey = kseq };
    HppliteNode *a = test_open_node(tpath("dis_a.db"), l1, &o), *b, *r;
    char name[80], path[600], c[43], d[43];
    if (!a) FAIL("open");
    hpplite_node_exec(a, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    if (hpplite_node_flush_batch(a) < 1 || hpplite_node_anchor(a) != 1) { hpplite_node_destroy(a); FAIL("batch 1, anchored"); }
    hpplite_node_exec(a, "INSERT INTO t(v) VALUES('the one the store served')", NULL);
    if (hpplite_node_flush_batch(a) < 1) { hpplite_node_destroy(a); FAIL("batch 2 at the store"); }
    {
        TestNodeOpts ro = { .storeUrl = url, .verifyOnly = 1, .nodeId = "dis_r", .privkey = kseq };
        r = test_open_node(tpath("dis_r.db"), l1, &ro);
    }
    if (!r || status_int(r->db, "soft_height") != 3) { printf("(soft_height %lld) ", r ? status_int(r->db, "soft_height") : -1); FAIL("the verifier follows batch 2"); }
    /* the sequencer dies; its batch 2 is lost to the store (its name gone); a successor writes another at 2 and anchors it */
    hpplite_l1_get_addresses(l1, c, d);
    snprintf(name, sizeof name, "%.40s-2", (d[0] ? d : c) + 2);
    for (char *p = name; *p && *p != '-'; p++) if (*p >= 'A' && *p <= 'F') *p = (char)(*p - 'A' + 'a');
    snprintf(path, sizeof path, "%s/pins/b/0x%.40s/n/2", test_dir(), name);
    hpplite_node_stop_timer(a); a->ownsL1 = 0; a->l1 = NULL; hpplite_node_destroy(a);
    if (unlink(path) != 0) { printf("(%s) ", path); FAIL("the name file of batch 2"); }
    hpplite_l1_mock_advance_time(l1, 400);
    {
        TestNodeOpts ob = { .storeUrl = url, .nodeId = "dis_b", .privkey = kseq };
        b = test_open_node(tpath("dis_b.db"), l1, &ob);
    }
    if (!b) FAIL("open the successor");
    hpplite_node_exec(b, "INSERT INTO t(v) VALUES('the one the chain committed')", NULL);
    if (hpplite_node_flush_batch(b) < 1 || hpplite_node_anchor(b) != 2) { printf("(%s) ", status_text(b->db, "last_error")); FAIL("the successor's batch 2, anchored"); }
    /* a reader of the replica's file, open across the rebuild (another connection, as an application's) */
    sqlite3 *reader = NULL;
    char seen[80] = "", seenAfter[80] = "";
    sqlite3_open_v2(tpath("dis_r.db"), &reader, SQLITE_OPEN_READONLY, NULL);
    text_of(reader, "SELECT v FROM t WHERE id = 1", seen, sizeof seen);
    hpplite_node_sync_from_l1(r, 1);
    text_of(reader, "SELECT v FROM t WHERE id = 1", seenAfter, sizeof seenAfter);
    sqlite3_close(reader);
    printf("(a reader open across it: '%s', then '%s') ", seen, seenAfter);
    long long dis = status_int(r->db, "soft_disagreements"), hard = status_int(r->db, "height");
    char v[80] = "";
    text_of(r->db, "SELECT v FROM t WHERE id = 1", v, sizeof v);
    int same = same_root(b, r);
    printf("(the verifier: state %s, disagreements %lld, height %lld, row '%s', same root %d) ", status_text(r->db, "state"), dis, hard, v, same);
    int ok = r->state != HPPLITE_STATE_ERROR && dis == 1 && hard == 3 && strcmp(v, "the one the chain committed") == 0 && same
             && strcmp(seen, "the one the store served") == 0 && strcmp(seenAfter, "the one the chain committed") == 0;
    /* rebuilt, it follows on: the successor's next batch is soft at once */
    hpplite_node_exec(b, "INSERT INTO t(v) VALUES('after the rebuild')", NULL);
    if (ok && hpplite_node_flush_batch(b) >= 1) {
        hpplite_node_sync_from_l1(r, 1);
        printf("(then: soft_height %lld, rows %d) ", status_int(r->db, "soft_height"), count_rows(r->db, "t"));
        ok = status_int(r->db, "soft_height") == 4 && count_rows(r->db, "t") == 2;
    }
    r->l1 = NULL; hpplite_node_destroy(r);
    b->ownsL1 = 0; hpplite_node_destroy(b);
    hpplite_l1_disconnect(l1);
    if (!ok) FAIL("the verifier should count the disagreement and hold, rebuilt, what the chain committed");
    PASS();
}

/* #188: the services say which protocol they speak (/status "protocol": "hpplite-store/1"); the node puts
** nothing to a store speaking a version it does not know, or to a relay given as store= */
static void test_protocol(const char *dir) {
    static const char *claims[] = { "hpplite-store/1", "hpplite-relay/1" };
    TEST("the node refuses a store speaking another protocol (#188): hpplite-store/1 (per-item, gone with #214), or a relay given as store=");
    for (int k = 0; k < 2; k++) {
        int port = test_free_port();
        char portStr[16], script[600], url[64], keyPath[600], tag[32];
        snprintf(portStr, sizeof portStr, "%d", port);
        snprintf(script, sizeof script, "%s/hpplite_store.py", dir);
        snprintf(keyPath, sizeof keyPath, "%s/signer.key", test_dir());
        snprintf(tag, sizeof tag, "proto_pins%d", k);
        pid_t pid = fork();
        if (pid == 0) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
            execlp("python3", "python3", "-I", script, "--port", portStr, "--dir", tpath(tag), "--signer-key", keyPath,
                   "--pay-to", "0x1111111111111111111111111111111111111111", "--advertise-protocol", claims[k], (char *)NULL);
            _exit(127);
        }
        int up = 0;
        for (int i = 0; i < 50 && !up; i++) {
            char cmd[200];
            if (!test_child_alive(pid)) break;
            snprintf(cmd, sizeof cmd, "curl -s -m 2 http://127.0.0.1:%d/status 2>/dev/null | grep -q signer", port);
            up = system(cmd) == 0;
            if (!up) usleep(200 * 1000);
        }
        if (!up) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); FAIL("the store did not start"); }
        snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
        HppliteStore *st = hpplite_store_open(url, NULL, 0);
        char *ref = st ? hpplite_store_put(st, (const uint8_t *)"protocol", 8) : NULL;
        const char *err = st ? hpplite_store_error(st) : "open";
        printf("(%s: %s) ", claims[k], ref ? "taken" : err);
        int refused = !ref && err && strstr(err, claims[k]);
        sqlite3_free(ref);
        if (st) hpplite_store_close(st);
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        if (!refused) FAIL("the node put to a store that speaks another protocol");
    }
    PASS();
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : getenv("HPPLITE_PIN_SERVICE");
    int port = test_free_port();
    test_suite_init("store", NULL, NULL);
    reset_dir();
    printf("Store client (#11)\n");
    if (!dir) { printf("SKIP: the pin service directory is not given\n"); return 77; }
    if (start_service(dir, port) != 0) { stop_service(); printf("FAIL: the pin service did not start\n"); return 1; }
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    static const unsigned char k0[32] = { [31] = 0x01 };
    unsigned char da0[20];
    HppliteStore *s = hpplite_store_open(url, k0, 0);
    if (!s) { stop_service(); printf("FAIL: open\n"); return 1; }
    memset(da0, 0xa1, 20);
    hpplite_store_set_bucket(s, da0, 0);
    test_roundtrip(s);
    test_statement(s, url);
    test_errors(s);
    hpplite_store_close(s);
    test_no_kubo_s3();
    test_named(url);
    test_node_status(url);
    test_quote(dir);
    test_protocol(dir);
    test_ranges(url);
    test_ranges_restart(url);
    test_no_anchor_at_close(url);
    test_external_funder(url, dir);
    test_bucket_cap(url);
    test_one_write_per_batch(url);
    test_store_rate_limited(dir);
    test_snapshot_made(url);
    test_snapshot_replica(url);
    test_snapshot_chunks(url);
    test_keep(url);
    test_store_down_at_open(dir);
    test_restart_store_down(dir);
    test_snapshot_name_refused(dir);
    test_manifest_short_hex(url);
    test_snapshot_reused_after_refusal(url);
    test_anchor_timer(url);
    test_first_tick_anchor(url);
    test_recovery(url);
    test_soft_replica(url);
    test_soft_disagreement(url);
    stop_service();
    remove_dir();
    return test_report();
}
