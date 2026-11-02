/*
** The pin-service store client (#11): put bytes, get them back by the
** reference, the receipt the provider signed, and the errors. Against the
** reference pin service in free mode (examples/pin_service), started here;
** argv[1] is that directory. The paid path (x402) is anvil scenario 19.
*/
#include "test_lib.h"
#include "store.h"
#include "crypto.h"
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

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : getenv("HPPLITE_PIN_SERVICE");
    int port = 8470 + (int)(getpid() % 200);
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
    test_node_status(url);
    {
        int kport = 8670 + (int)(getpid() % 200);
        char kurl[64];
        snprintf(kurl, sizeof kurl, "kubo://127.0.0.1:%d", kport);
        if (start_kubo(dir, kport) != 0) { stop_kubo(); stop_service(); printf("FAIL: the Kubo stand-in did not start\n"); return 1; }
        test_kubo(kurl, "");
        stop_kubo();
        if (getenv("HPPLITE_KUBO")) test_kubo(getenv("HPPLITE_KUBO"), "_live");    /* a real daemon, when one is named (kubo://127.0.0.1:5001) */
    }
    {
        int sport = 8870 + (int)(getpid() % 200);
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
