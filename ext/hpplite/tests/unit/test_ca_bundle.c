/*
** HPPLITE_CA_BUNDLE (#269): the certificate file both curl clients trust - the chain RPC's
** (eth/eth_client.c) and the store's (store.c). A static binary carries no system bundle,
** and a container image may keep its own; the variable names the file. Against a TLS
** server signed by a certificate of its own: with the variable naming another
** certificate the handshakes fail; naming the server's, both clients connect. A failed
** check names itself (#377): the host, and the bundle it was checked against.
**   test_ca_bundle <path/to/tests/unit/tls_stub.py>
*/
#include "test_lib.h"
#include "l1_interface.h"
#include "store.h"
#include "tls.h"
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define DIR "/tmp/hpplite_test_ca_bundle"
static pid_t g_stub;

static int make_cert(const char *name) {
    char cmd[700];
    snprintf(cmd, sizeof cmd, "openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout '%s/%s.key' -out '%s/%s.pem'"
             " -subj /CN=127.0.0.1 -addext subjectAltName=IP:127.0.0.1 -days 2 >/dev/null 2>&1", DIR, name, DIR, name);
    return system(cmd);
}

static int start_stub(const char *script, int port) {
    char portStr[16];
    snprintf(portStr, sizeof portStr, "%d", port);
    g_stub = fork();
    if (g_stub == 0) {
        execlp("python3", "python3", "-I", script, "--port", portStr, "--cert", DIR "/server.pem", "--key", DIR "/server.key", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 50; i++) {
        char cmd[300];
        if (!test_child_alive(g_stub)) return -1;
        snprintf(cmd, sizeof cmd, "curl -s -m 2 --cacert " DIR "/server.pem https://127.0.0.1:%d/status 2>/dev/null | grep -q hpplite-store", port);
        if (system(cmd) == 0) return 0;
        usleep(100000);
    }
    return -1;
}
static void stop_stub(void) { if (g_stub > 0) { kill(g_stub, SIGTERM); waitpid(g_stub, NULL, 0); g_stub = 0; } }

/* the two clients against the stub under the bundle set now: 1 = the RPC answered, store 1 = /status read;
** zRpcWhy and zStoreWhy: what each said of a certificate it could not check, empty where it said nothing */
static void try_both(const char *url, int *pRpc, int *pStore, char *zRpcWhy, char *zStoreWhy, size_t n) {
    HppliteL1 *l1 = hpplite_l1_connect(url, "0x1111111111111111111111111111111111111111");
    HppliteSystemConfig *cfg = l1 ? hpplite_l1_get_system_config(l1) : NULL;
    HppliteStore *s;
    *pRpc = cfg != NULL;
    snprintf(zRpcWhy, n, "%s", hpplite_tls_why() ? hpplite_tls_why() : "");
    hpplite_l1_system_config_free(cfg);
    hpplite_l1_disconnect(l1);
    s = hpplite_store_open(url, NULL, 0);
    *pStore = s != NULL && hpplite_store_take_status(s) == 200;    /* /status was read */
    zStoreWhy[0] = 0;
    if (s) {
        /* the open forgives a store that does not answer yet: a put says why it could not be sent */
        sqlite3_free(hpplite_store_put(s, (const uint8_t *)"x", 1));
        if (strstr(hpplite_store_error(s), "certificate")) snprintf(zStoreWhy, n, "%s", hpplite_store_error(s));
    }
    if (s) hpplite_store_close(s);
}

static void test_bundle(const char *script) {
    TEST("HPPLITE_CA_BUNDLE: the RPC and the store over TLS fail with another certificate's bundle, saying so (#377), and connect with the server's (#269)");
    int port = test_free_port(), rpcWrong, storeWrong, rpcRight, storeRight;
    char url[64], rpcWhy[300], storeWhy[300], rpcWhyRight[300], storeWhyRight[300];
    const char *zWant = "the certificate of 127.0.0.1 could not be checked against " DIR "/other.pem (HPPLITE_CA_BUNDLE)";
    reset_dir();
    if (make_cert("server") != 0 || make_cert("other") != 0) FAIL("openssl could not make the certificates");
    if (start_stub(script, port) != 0) { stop_stub(); FAIL("the TLS stub did not start"); }
    snprintf(url, sizeof url, "https://127.0.0.1:%d", port);
    setenv("HPPLITE_CA_BUNDLE", DIR "/other.pem", 1);
    try_both(url, &rpcWrong, &storeWrong, rpcWhy, storeWhy, sizeof rpcWhy);
    setenv("HPPLITE_CA_BUNDLE", DIR "/server.pem", 1);
    try_both(url, &rpcRight, &storeRight, rpcWhyRight, storeWhyRight, sizeof rpcWhyRight);
    unsetenv("HPPLITE_CA_BUNDLE");
    stop_stub();
    printf("(another's bundle: rpc %d store %d; the server's: rpc %d store %d) ", rpcWrong, storeWrong, rpcRight, storeRight);
    if (rpcWrong || storeWrong) FAIL("with a bundle that lacks the server's issuer both handshakes must fail");
    if (!rpcRight || !storeRight) FAIL("with the server's certificate as the bundle both clients should connect");
    if (strcmp(rpcWhy, zWant) != 0 || strcmp(storeWhy, zWant) != 0) {
        printf("(rpc: '%s'; store: '%s') ", rpcWhy, storeWhy);
        FAIL("a certificate that could not be checked should be said: the host and the bundle it was checked against (#377)");
    }
    if (rpcWhyRight[0] || storeWhyRight[0]) FAIL("a certificate that was checked leaves nothing said");
    PASS();
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: test_ca_bundle <tls_stub.py>\n"); return 2; }
    test_suite_init("ca_bundle", NULL, DIR);
    printf("HPPLITE_CA_BUNDLE (#269)\n");
    test_bundle(argv[1]);
    remove_dir();
    return test_report();
}
