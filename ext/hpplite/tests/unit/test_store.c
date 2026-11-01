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
    test_errors(s);
    hpplite_store_close(s);
    stop_service();
    remove_dir();
    return test_report();
}
