/*
** The pin-service store client (#11): put bytes, get them back by the
** reference, the receipt the provider signed, and the errors. Against the
** reference pin service in free mode (examples/pin_service), started here;
** argv[1] is that directory. The paid path (x402) is anvil scenario 19.
*/
#include "test_lib.h"
#include "store.h"
#include "crypto.h"
#include "eth_client.h"
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>

static pid_t g_pid;

static int start_service(const char *dir, int port) {
    char portStr[16], storeDir[600], keyPath[600], script[600];
    FILE *f;
    snprintf(portStr, sizeof portStr, "%d", port);
    snprintf(storeDir, sizeof storeDir, "%s/pins", test_dir());
    snprintf(keyPath, sizeof keyPath, "%s/signer.key", test_dir());
    snprintf(script, sizeof script, "%s/pin_service.py", dir);
    f = fopen(keyPath, "w");
    if (!f) return -1;
    fputs("0x2020202020202020202020202020202020202020202020202020202020202020\n", f);
    fclose(f);
    g_pid = fork();
    if (g_pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execlp("python3", "python3", "-I", script, "--port", portStr, "--dir", storeDir, "--signer-key", keyPath,
               "--pay-to", "0x1111111111111111111111111111111111111111", (char *)NULL);
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

/* a Kubo stand-in (examples/pin_service/kubo_mock.py), for the kubo:// store */
static pid_t g_kubo;
static int start_kubo(const char *dir, int port) {
    char portStr[16], script[600];
    snprintf(portStr, sizeof portStr, "%d", port);
    snprintf(script, sizeof script, "%s/kubo_mock.py", dir);
    g_kubo = fork();
    if (g_kubo == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execlp("python3", "python3", "-I", script, "--port", portStr, (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 50; i++) {
        char cmd[200];
        if (!test_child_alive(g_kubo)) { g_kubo = 0; return -1; }
        snprintf(cmd, sizeof cmd, "curl -s -m 2 -X POST 'http://127.0.0.1:%d/api/v0/cat?arg=x' 2>/dev/null | grep -q Message", port);
        if (system(cmd) == 0) return 0;
        usleep(200 * 1000);
    }
    return -1;
}
static void stop_kubo(void) {
    if (g_kubo > 0) { kill(g_kubo, SIGTERM); waitpid(g_kubo, NULL, 0); g_kubo = 0; }
}

/* an S3 stand-in (examples/pin_service/s3_mock.py): one bucket, Signature Version 4 checked */
static pid_t g_s3;
#define S3_KEY "AKIAHPPLITETEST"
#define S3_SECRET "hpplite-test-secret-key"
static int start_s3(const char *dir, int port) {
    char portStr[16], script[600];
    snprintf(portStr, sizeof portStr, "%d", port);
    snprintf(script, sizeof script, "%s/s3_mock.py", dir);
    g_s3 = fork();
    if (g_s3 == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execlp("python3", "python3", "-I", script, "--port", portStr, "--bucket", "batches",
               "--access-key", S3_KEY, "--secret-key", S3_SECRET, (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 50; i++) {
        char cmd[200];
        if (!test_child_alive(g_s3)) { g_s3 = 0; return -1; }
        snprintf(cmd, sizeof cmd, "curl -s -m 2 'http://127.0.0.1:%d/batches/x' 2>/dev/null | grep -q SignatureDoesNotMatch", port);
        if (system(cmd) == 0) return 0;
        usleep(200 * 1000);
    }
    return -1;
}
static void stop_s3(void) {
    if (g_s3 > 0) { kill(g_s3, SIGTERM); waitpid(g_s3, NULL, 0); g_s3 = 0; }
}

static void stop_service(void) {
    if (g_pid > 0) { kill(g_pid, SIGTERM); waitpid(g_pid, NULL, 0); g_pid = 0; }
}

static void test_roundtrip(HppliteStore *s) {
    TEST("put bytes, get them back by the reference, the receipt names the hash");
    const uint8_t data[] = "a sealed batch, as the node would post it";
    char *zReceipt = NULL, *zRef = hpplite_store_put(s, data, sizeof(data) - 1, &zReceipt);
    if (!zRef) FAILF("put: %s", hpplite_store_error(s));
    if (strncmp(zRef, "bafkrei", 7) != 0) { sqlite3_free(zRef); FAIL("the reference is not a CIDv1 raw sha-256"); }
    if (!zReceipt || !strstr(zReceipt, "\"signature\"")) { sqlite3_free(zRef); sqlite3_free(zReceipt); FAIL("no signed receipt came back"); }
    {
        unsigned char h[32];
        char hex[67] = "0x";
        hpplite_keccak256(data, sizeof(data) - 1, h);
        hpplite_bytes_to_hex(h, 32, hex + 2);
        if (!strstr(zReceipt, hex)) { sqlite3_free(zRef); sqlite3_free(zReceipt); FAILF("the receipt's hash is not keccak of the bytes: %s", zReceipt); }
    }
    size_t n = 0;
    uint8_t *back = hpplite_store_get(s, zRef, &n);
    if (!back || n != sizeof(data) - 1 || memcmp(back, data, n) != 0 || back[n] != 0) { free(back); sqlite3_free(zRef); sqlite3_free(zReceipt); FAIL("get: bytes or terminator"); }
    free(back);
    /* the same bytes again: the same reference, held already */
    char *zRef2 = hpplite_store_put(s, data, sizeof(data) - 1, NULL);
    if (!zRef2 || strcmp(zRef, zRef2) != 0) { sqlite3_free(zRef); sqlite3_free(zRef2); sqlite3_free(zReceipt); FAIL("a second put of the same bytes should give the same reference"); }
    sqlite3_free(zRef);
    sqlite3_free(zRef2);
    sqlite3_free(zReceipt);
    if (hpplite_store_spent(s, NULL) != 0) FAIL("nothing should have been paid in free mode");
    PASS();
}

static void test_receipt(HppliteStore *s) {
    TEST("the receipt's signature recovers to the provider's published signer; a changed byte or another signer does not verify");
    const uint8_t data[] = "bytes under a receipt";
    unsigned char signer[20], payTo[20], h[32];
    uint64_t chainId = 0;
    uint8_t compact[HPPLITE_RECEIPT_SIZE];
    if (hpplite_store_signer(s, signer) != 0 || hpplite_store_domain(s, payTo, &chainId) != 0) FAIL("the service's /status should name its signer and domain");
    char *zReceipt = NULL, *zRef = hpplite_store_put(s, data, sizeof(data) - 1, &zReceipt);
    if (!zRef || !zReceipt) { sqlite3_free(zRef); sqlite3_free(zReceipt); FAILF("put: %s", hpplite_store_error(s)); }
    if (hpplite_store_receipt_compact(zReceipt, payTo, compact) != HPPLITE_RECEIPT_SIZE) { sqlite3_free(zRef); sqlite3_free(zReceipt); FAIL("compact"); }
    hpplite_keccak256(data, sizeof(data) - 1, h);
    if (hpplite_store_receipt_verify(compact, sizeof compact, h, zRef, chainId, signer) != 0) { sqlite3_free(zRef); sqlite3_free(zReceipt); FAIL("a good receipt does not verify"); }
    {
        unsigned char other[20];
        memset(other, 0x42, 20);
        if (hpplite_store_receipt_verify(compact, sizeof compact, h, zRef, chainId, other) == 0) { sqlite3_free(zRef); sqlite3_free(zReceipt); FAIL("verified against another signer"); }
        uint8_t bad[HPPLITE_RECEIPT_SIZE];
        memcpy(bad, compact, sizeof bad);
        bad[41 + 7] ^= 1;                                  /* the expiry */
        if (hpplite_store_receipt_verify(bad, sizeof bad, h, zRef, chainId, signer) == 0) { sqlite3_free(zRef); sqlite3_free(zReceipt); FAIL("verified with a changed expiry"); }
        memcpy(bad, compact, sizeof bad);
        bad[60] ^= 1;                                      /* a signature byte */
        if (hpplite_store_receipt_verify(bad, sizeof bad, h, zRef, chainId, signer) == 0) { sqlite3_free(zRef); sqlite3_free(zReceipt); FAIL("verified with a changed signature"); }
        unsigned char h2[32];
        memcpy(h2, h, 32); h2[0] ^= 1;
        if (hpplite_store_receipt_verify(compact, sizeof compact, h2, zRef, chainId, signer) == 0) { sqlite3_free(zRef); sqlite3_free(zReceipt); FAIL("verified for other bytes"); }
        if (hpplite_store_receipt_verify(compact, sizeof compact, h, zRef, chainId + 1, signer) == 0) { sqlite3_free(zRef); sqlite3_free(zReceipt); FAIL("verified on another chain"); }
    }
    sqlite3_free(zRef);
    sqlite3_free(zReceipt);
    PASS();
}

static void test_errors(HppliteStore *s) {
    TEST("an unknown reference is NULL with a reason; an empty put is refused; an unknown scheme does not open");
    size_t n = 1;
    uint8_t *back = hpplite_store_get(s, "bafkreiaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &n);
    if (back || n != 0) { free(back); FAIL("get of an unknown reference"); }
    if (!hpplite_store_error(s)[0]) FAIL("no reason given");
    if (hpplite_store_put(s, (const uint8_t *)"", 0, NULL) != NULL) FAIL("an empty put");
    if (hpplite_store_open("ftp://nowhere", NULL, 0) != NULL) FAIL("ftp:// opened");
    HppliteStore *dead = hpplite_store_open("http://127.0.0.1:9", NULL, 0);
    if (!dead) FAIL("open of an unreachable store should succeed (it is reached at the first call)");
    if (hpplite_store_put(dead, (const uint8_t *)"x", 1, NULL) != NULL) { hpplite_store_close(dead); FAIL("a put to a dead store"); }
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

/* 5. A Kubo node as the store (#11): kubo://host:port speaks Kubo's own RPC
** (add with cid-version 1 and raw leaves, cat). No receipts, no payment:
** the chain commits (hash, CID) and nothing else. */
/* #172: the node's quote of a put (store_price) is the price the store's 402
** asks: per put, per MiB-month for the retention bought, rounded up to a
** whole unit. A paid service (the mock facilitator: its 402s need no chain) */
static void test_quote(const char *dir) {
    TEST("the node quotes a put as the store's 402 does (#172): per put + per MiB-month x retention, rounded up to a whole unit");
    int port = test_free_port();
    char portStr[16], script[600], url[64], keyPath[600];
    static const size_t sizes[] = { 183, 40000, 3 * 1048576 + 17 };
    snprintf(portStr, sizeof portStr, "%d", port);
    snprintf(script, sizeof script, "%s/pin_service.py", dir);
    snprintf(keyPath, sizeof keyPath, "%s/signer.key", test_dir());
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execlp("python3", "python3", "-I", script, "--port", portStr, "--dir", tpath("quote_pins"), "--signer-key", keyPath,
               "--pay-to", "0x1111111111111111111111111111111111111111", "--facilitator", "mock", "--rpc", "http://127.0.0.1:1",
               "--settle-key", "0x01", "--price-per-mib-month", "30.5", "--price-per-pin", "10", "--retention", "5184000", (char *)NULL);
        _exit(127);
    }
    int up = 0;
    for (int i = 0; i < 50 && !up; i++) {
        char cmd[200];
        if (!test_child_alive(pid)) break;
        snprintf(cmd, sizeof cmd, "curl -s -m 2 http://127.0.0.1:%d/status 2>/dev/null | grep -q pricePerPin", port);
        up = system(cmd) == 0;
        if (!up) usleep(200 * 1000);
    }
    if (!up) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); FAIL("the paid pin service did not start"); }
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    HppliteStore *st = hpplite_store_open(url, (const uint8_t *)"0123456789abcdef0123456789abcdef", 0);
    const char *why = st ? NULL : "open";
    for (size_t k = 0; !why && k < sizeof sizes / sizeof sizes[0]; k++) {
        char cmd[400], out[2000], *p;
        snprintf(cmd, sizeof cmd, "head -c %zu /dev/urandom > %s && curl -s -m 10 -X POST --data-binary @%s http://127.0.0.1:%d/put",
                 sizes[k], tpath("quote.bin"), tpath("quote.bin"), port);
        FILE *f = popen(cmd, "r");
        size_t n = f ? fread(out, 1, sizeof out - 1, f) : 0;
        if (f) pclose(f);
        out[n] = 0;
        p = strstr(out, "\"amount\": \"");
        long long asked = p ? atoll(p + 11) : -1;
        long long quoted = (long long)hpplite_store_quote(st, sizes[k]);
        printf("(%zu bytes: 402 asks %lld, node quotes %lld) ", sizes[k], asked, quoted);
        if (asked <= 0 || asked != quoted) why = "the node's quote is not the store's price";
    }
    if (st) hpplite_store_close(st);
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    if (why) FAIL(why);
    PASS();
}

static void test_kubo(const char *url, const char *tag) {   /* tag: the files' suffix, so a stand-in run and a live one do not share them */
    TEST("kubo://: put pins the bytes and names their CIDv1, get reads them back, no receipt; a node on it posts and a replica rebuilds from the commitment");
    HppliteStore *s = hpplite_store_open(url, NULL, 0);
    if (!s) FAIL("kubo:// did not open");
    const uint8_t data[] = "bytes on a Kubo node";
    char *zReceipt = (char *)"unset";
    char *zRef = hpplite_store_put(s, data, sizeof(data) - 1, &zReceipt);
    if (!zRef) { printf("(%s) ", hpplite_store_error(s)); hpplite_store_close(s); FAIL("put"); }
    if (strcmp(zRef, "bafkreihvqzvroh57j7qeruusyaoawj62ny72bxluzpmjzatjhtklrlwtte") != 0) {   /* CIDv1 raw sha-256 of the bytes */
        printf("(ref %s) ", zRef); sqlite3_free(zRef); hpplite_store_close(s); FAIL("the reference is not the bytes' CIDv1");
    }
    if (zReceipt) { sqlite3_free(zRef); hpplite_store_close(s); FAIL("a Kubo node signs nothing: the receipt must be NULL"); }
    {
        size_t n = 0;
        uint8_t *back = hpplite_store_get(s, zRef, &n);
        int ok = back && n == sizeof(data) - 1 && memcmp(back, data, n) == 0;
        free(back);
        if (!ok) { sqlite3_free(zRef); hpplite_store_close(s); FAIL("get"); }
        back = hpplite_store_get(s, "bafkreify2xemwfcdb5xl7y2zx3j4qgq6zcsffkqkkljogl5gehfeoiqgde", &n);
        if (back) { free(back); sqlite3_free(zRef); hpplite_store_close(s); FAIL("an unknown CID should be NULL"); }
    }
    sqlite3_free(zRef);
    if (hpplite_store_signer(s, NULL) != -1 || hpplite_store_calls(s) != 2 || hpplite_store_spent(s, NULL) != 0)
        { hpplite_store_close(s); FAIL("no signer, two calls, nothing paid"); }
    hpplite_store_close(s);

    HppliteL1 *l1 = hpplite_l1_create_mock();
    char fw[64], fr[64];
    snprintf(fw, sizeof fw, "kubo_w%s.db", tag);
    snprintf(fr, sizeof fr, "kubo_r%s.db", tag);
    TestNodeOpts o = { .storeUrl = url, .nodeId = "kubo_w" };
    HppliteNode *w = test_open_node(tpath(fw), l1, &o);
    if (!w) FAIL("open node");
    hpplite_node_exec(w, "CREATE TABLE k(x); INSERT INTO k VALUES('pinned')", NULL);
    if (hpplite_node_flush_batch(w) < 1) { printf("(%s) ", status_text(w->db, "last_error")); hpplite_node_destroy(w); FAIL("flush through Kubo"); }
    if (strcmp(status_text(w->db, "store_state"), "ok") != 0 || scalar(w->db, "SELECT store_price IS NULL FROM hpplite_status") != 1
        || strcmp(status_text(w->db, "store"), url) != 0) {
        printf("(state '%s', store '%s') ", status_text(w->db, "store_state"), status_text(w->db, "store"));
        hpplite_node_destroy(w);
        FAIL("the status row should name the Kubo store, ok, with no price");
    }
    if (hpplite_node_anchor(w) != 1) { printf("(%s) ", status_text(w->db, "last_error")); hpplite_node_destroy(w); FAIL("the anchor through Kubo"); }
    {
        HppliteStore *rs = hpplite_store_open(url, NULL, 0);
        uint8_t sig[65];
        size_t n = 0;
        if (rs && hpplite_store_get_named(rs, "dadadadadadadadadadadadadadadadadadadada-1", &n, sig, NULL) != NULL) { hpplite_store_close(rs); hpplite_node_destroy(w); FAIL("a Kubo node takes no names"); }
        if (rs) hpplite_store_close(rs);
    }
    {
        TestNodeOpts ro = { .storeUrl = url, .verifyOnly = 1, .nodeId = "kubo_r" };
        HppliteNode *r = test_open_node(tpath(fr), l1, &ro);
        int rows = r ? count_rows(r->db, "k") : -1;
        if (r) { r->l1 = NULL; hpplite_node_destroy(r); }
        if (rows != 1) { printf("(replica rows %d) ", rows); hpplite_node_destroy(w); FAIL("a replica on the Kubo node should rebuild the committed batch"); }
    }
    hpplite_node_destroy(w);
    PASS();
}

/* 6. An S3 bucket as the store (#11): s3://bucket/prefix, Signature Version
** 4 with the environment's credentials; the key is the bytes' CIDv1 under
** the prefix and the reference committed is the CID alone. No receipts. */
static void test_s3(const char *url, const char *secret, const char *tag) {
    TEST("s3://: put signs with the environment's credentials and names the bytes' CIDv1, get reads back, a wrong secret is refused with S3's reason; a node and a replica run through it");
    HppliteStore *s = hpplite_store_open(url, NULL, 0);
    if (!s) FAIL("s3:// did not open");
    const uint8_t data[] = "bytes on a Kubo node";                 /* the same bytes, the same CID as case 5 */
    char *zReceipt = (char *)"unset";
    char *zRef = hpplite_store_put(s, data, sizeof(data) - 1, &zReceipt);
    if (!zRef) { printf("(%s) ", hpplite_store_error(s)); hpplite_store_close(s); FAIL("put"); }
    if (strcmp(zRef, "bafkreihvqzvroh57j7qeruusyaoawj62ny72bxluzpmjzatjhtklrlwtte") != 0) {
        printf("(ref %s) ", zRef); sqlite3_free(zRef); hpplite_store_close(s); FAIL("the reference is not the bytes' CIDv1");
    }
    if (zReceipt) { sqlite3_free(zRef); hpplite_store_close(s); FAIL("a bucket signs nothing: the receipt must be NULL"); }
    {
        size_t n = 0;
        uint8_t *back = hpplite_store_get(s, zRef, &n);
        int ok = back && n == sizeof(data) - 1 && memcmp(back, data, n) == 0;
        free(back);
        if (!ok) { printf("(%s) ", hpplite_store_error(s)); sqlite3_free(zRef); hpplite_store_close(s); FAIL("get"); }
        back = hpplite_store_get(s, "bafkreify2xemwfcdb5xl7y2zx3j4qgq6zcsffkqkkljogl5gehfeoiqgde", &n);
        if (back) { free(back); sqlite3_free(zRef); hpplite_store_close(s); FAIL("an unknown CID should be NULL"); }
    }
    sqlite3_free(zRef);
    if (hpplite_store_signer(s, NULL) != -1 || hpplite_store_calls(s) != 2 || hpplite_store_spent(s, NULL) != 0)
        { hpplite_store_close(s); FAIL("no signer, two calls, nothing paid"); }
    hpplite_store_close(s);

    /* the wrong secret: refused with S3's reason, counted as an error, not an outage */
    setenv("AWS_SECRET_ACCESS_KEY", "not-the-secret", 1);
    s = hpplite_store_open(url, NULL, 0);
    if (secret) setenv("AWS_SECRET_ACCESS_KEY", secret, 1); else unsetenv("AWS_SECRET_ACCESS_KEY");
    if (!s) FAIL("reopen");
    zRef = hpplite_store_put(s, data, sizeof(data) - 1, NULL);
    if (zRef || !strstr(hpplite_store_error(s), "SignatureDoesNotMatch") || hpplite_store_last_failure(s) != HPPLITE_STORE_FAIL_OTHER) {
        printf("(ref %s, '%s') ", zRef ? zRef : "none", hpplite_store_error(s)); sqlite3_free(zRef); hpplite_store_close(s);
        FAIL("a wrong secret should be refused with SignatureDoesNotMatch");
    }
    hpplite_store_close(s);

    HppliteL1 *l1 = hpplite_l1_create_mock();
    char fw[64], fr[64];
    snprintf(fw, sizeof fw, "s3_w%s.db", tag);
    snprintf(fr, sizeof fr, "s3_r%s.db", tag);
    TestNodeOpts o = { .storeUrl = url, .nodeId = "s3_w" };
    HppliteNode *w = test_open_node(tpath(fw), l1, &o);
    if (!w) FAIL("open node");
    hpplite_node_exec(w, "CREATE TABLE b(x); INSERT INTO b VALUES('in the bucket')", NULL);
    if (hpplite_node_flush_batch(w) < 1) { printf("(%s) ", status_text(w->db, "last_error")); hpplite_node_destroy(w); FAIL("flush through S3"); }
    if (strcmp(status_text(w->db, "store_state"), "ok") != 0 || strcmp(status_text(w->db, "store"), url) != 0) {
        printf("(state '%s', store '%s') ", status_text(w->db, "store_state"), status_text(w->db, "store"));
        hpplite_node_destroy(w);
        FAIL("the status row should name the bucket, ok");
    }
    if (hpplite_node_anchor(w) != 1) { printf("(%s) ", status_text(w->db, "last_error")); hpplite_node_destroy(w); FAIL("the anchor through S3"); }
    {
        /* names on a bucket: a key per name (#165) */
        char c[43], d[43], name[80], *p;
        uint8_t sig[65], *bytes;
        size_t n = 0;
        HppliteStore *rs = hpplite_store_open(url, NULL, 0);
        hpplite_l1_get_addresses(l1, c, d);
        snprintf(name, sizeof name, "%.40s-1", (d[0] ? d : c) + 2);
        for (p = name; *p && *p != '-'; p++) if (*p >= 'A' && *p <= 'F') *p = (char)(*p - 'A' + 'a');
        bytes = rs ? hpplite_store_get_named(rs, name, &n, sig, NULL) : NULL;
        if (!bytes || n == 0) { printf("(%s: %s) ", name, rs ? hpplite_store_error(rs) : "no store"); if (rs) hpplite_store_close(rs); hpplite_node_destroy(w); FAIL("batch 1 under its name in the bucket"); }
        free(bytes); hpplite_store_close(rs);
    }
    {
        TestNodeOpts ro = { .storeUrl = url, .verifyOnly = 1, .nodeId = "s3_r" };
        HppliteNode *r = test_open_node(tpath(fr), l1, &ro);
        int rows = r ? count_rows(r->db, "b") : -1;
        if (r) { r->l1 = NULL; hpplite_node_destroy(r); }
        if (rows != 1) { printf("(replica rows %d) ", rows); hpplite_node_destroy(w); FAIL("a replica on the bucket should rebuild the committed batch"); }
    }
    hpplite_node_destroy(w);
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


/* 9. Named entries (#165): a batch under <da>-<height> with the sequencer's signature; the first
** signer holds a rollup's names at the pin service; a reader recovers the signer. */
static void test_named(const char *url) {
    TEST("named entries: put a signed batch under <da>-<height>, read it back with its signature, the signer recovers; another key is refused for that rollup, a bad signature too");
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
    if (hpplite_store_put_named(s1, "dadadadadadadadadadadadadadadadadadadada-7", inst, data, sizeof data, sig) != 0) { printf("(%s) ", hpplite_store_error(s1)); FAIL("put named"); }
    back = hpplite_store_get_named(s1, "dadadadadadadadadadadadadadadadadadadada-7", &n, got, NULL);
    if (!back || n != sizeof data || memcmp(back, data, n) != 0 || memcmp(got, sig, 65) != 0) { printf("(%s) ", hpplite_store_error(s1)); free(back); FAIL("get named: the bytes and the signature"); }
    free(back);
    if (hpplite_store_batch_signer(da, 181228, inst, 7, data, sizeof data, sig, me1) != 0) FAIL("the signer recovers");
    {
        /* and the service refuses a signature made for another chain: its domain is the chain it serves */
        uint8_t other[65];
        if (hpplite_store_sign_batch(s1, da, 31337, inst, 7, data, sizeof data, other) != 0 || hpplite_store_put_named(s1, "dadadadadadadadadadadadadadadadadadadada-70", inst, data, sizeof data, other) == 0) FAIL("a signature for another chain must be refused");
    }
    if (hpplite_store_sign_batch(s2, da, 181228, inst, 8, data, sizeof data, sig2) != 0 || hpplite_store_batch_signer(da, 181228, inst, 8, data, sizeof data, sig2, me2) != 0) FAIL("sign with the second key");
    if (memcmp(me1, me2, 20) == 0) FAIL("two keys, two signers");
    /* the second key on the first rollup's names: refused; on its own rollup: taken */
    if (hpplite_store_put_named(s2, "dadadadadadadadadadadadadadadadadadadada-8", inst, data, sizeof data, sig2) == 0 || !strstr(hpplite_store_error(s2), "403")) { printf("('%s') ", hpplite_store_error(s2)); FAIL("another signer must be refused for a rollup whose names are held"); }
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
    snprintf(path, sizeof path, "%s/pins/name-%s", test_dir(), name);
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
    HppliteStore *s = hpplite_store_open(url, NULL, 0);
    if (!s) { stop_service(); printf("FAIL: open\n"); return 1; }
    test_roundtrip(s);
    test_receipt(s);
    test_errors(s);
    hpplite_store_close(s);
    test_named(url);
    test_node_status(url);
    test_quote(dir);
    test_ranges(url);
    test_ranges_restart(url);
    test_anchor_timer(url);
    test_recovery(url);
    test_soft_replica(url);
    test_soft_disagreement(url);
    {
        int kport = test_free_port();
        char kurl[64];
        snprintf(kurl, sizeof kurl, "kubo://127.0.0.1:%d", kport);
        if (start_kubo(dir, kport) != 0) { stop_kubo(); stop_service(); printf("FAIL: the Kubo stand-in did not start\n"); return 1; }
        test_kubo(kurl, "");
        stop_kubo();
        if (getenv("HPPLITE_KUBO")) test_kubo(getenv("HPPLITE_KUBO"), "_live");    /* a real daemon, when one is named (kubo://127.0.0.1:5001) */
    }
    {
        int sport = test_free_port();
        char endpoint[64];
        /* a real bucket first, with the environment's own credentials (MinIO in a container, or AWS) */
        if (getenv("HPPLITE_S3")) test_s3(getenv("HPPLITE_S3"), getenv("AWS_SECRET_ACCESS_KEY"), "_live");
        snprintf(endpoint, sizeof endpoint, "http://127.0.0.1:%d", sport);
        if (start_s3(dir, sport) != 0) { stop_s3(); stop_service(); printf("FAIL: the S3 stand-in did not start\n"); return 1; }
        setenv("HPPLITE_S3_ENDPOINT", endpoint, 1);
        setenv("AWS_REGION", "us-east-1", 1);
        setenv("AWS_ACCESS_KEY_ID", S3_KEY, 1);
        setenv("AWS_SECRET_ACCESS_KEY", S3_SECRET, 1);
        test_s3("s3://batches/hpplite/test", S3_SECRET, "");
        stop_s3();
        unsetenv("HPPLITE_S3_ENDPOINT"); unsetenv("AWS_REGION"); unsetenv("AWS_ACCESS_KEY_ID"); unsetenv("AWS_SECRET_ACCESS_KEY");
    }
    stop_service();
    remove_dir();
    return test_report();
}
