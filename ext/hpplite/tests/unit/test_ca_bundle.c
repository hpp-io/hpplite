/*
** HPPLITE_CA_BUNDLE (#269): the certificate file both curl clients trust - the chain RPC's
** (eth/eth_client.c) and the store's (store.c). A static binary carries no system bundle,
** and a container image may keep its own; the variable names the file. Against a TLS
** server signed by a certificate of its own: with the variable naming another
** certificate the handshakes fail; naming the server's, both clients connect.
**   test_ca_bundle <path/to/tests/unit/tls_stub.py>
*/
#include "test_lib.h"
#include "l1_interface.h"
#include "store.h"
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

/* the two clients against the stub under the bundle set now: 1 = the RPC answered, store 1 = /status read */
static void try_both(const char *url, int *pRpc, int *pStore) {
    HppliteL1 *l1 = hpplite_l1_connect(url, "0x1111111111111111111111111111111111111111");
    HppliteSystemConfig *cfg = l1 ? hpplite_l1_get_system_config(l1) : NULL;
    HppliteStore *s;
    *pRpc = cfg != NULL;
    hpplite_l1_system_config_free(cfg);
    hpplite_l1_disconnect(l1);
    s = hpplite_store_open(url, NULL, 0);
    *pStore = s != NULL && hpplite_store_take_status(s) == 200;    /* /status was read */
    if (s) hpplite_store_close(s);
}

static void test_bundle(const char *script) {
    TEST("HPPLITE_CA_BUNDLE: the RPC and the store over TLS fail with another certificate's bundle and connect with the server's (#269)");
    int port = test_free_port(), rpcWrong, storeWrong, rpcRight, storeRight;
    char url[64];
    reset_dir();
    if (make_cert("server") != 0 || make_cert("other") != 0) FAIL("openssl could not make the certificates");
    if (start_stub(script, port) != 0) { stop_stub(); FAIL("the TLS stub did not start"); }
    snprintf(url, sizeof url, "https://127.0.0.1:%d", port);
    setenv("HPPLITE_CA_BUNDLE", DIR "/other.pem", 1);
    try_both(url, &rpcWrong, &storeWrong);
    setenv("HPPLITE_CA_BUNDLE", DIR "/server.pem", 1);
    try_both(url, &rpcRight, &storeRight);
    unsetenv("HPPLITE_CA_BUNDLE");
    stop_stub();
    printf("(another's bundle: rpc %d store %d; the server's: rpc %d store %d) ", rpcWrong, storeWrong, rpcRight, storeRight);
    if (rpcWrong || storeWrong) FAIL("with a bundle that lacks the server's issuer both handshakes must fail");
    if (!rpcRight || !storeRight) FAIL("with the server's certificate as the bundle both clients should connect");
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
