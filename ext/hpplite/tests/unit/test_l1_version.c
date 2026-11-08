/*
** #252: contracts from factories before VERSION 8 are not read. The chain client reads the coordinator's
** VERSION from getSystemConfig(); below 8 it refuses (NULL, one line on stderr) and nothing else of the
** rollup is read. A JSON-RPC stand-in (rpc_stub.py) reports the version the test asks for.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include "test_lib.h"
#include "l1_interface.h"

static pid_t g_stub;

static int start_stub(const char *script, int port, int version) {
    char portStr[16], verStr[16];
    snprintf(portStr, sizeof portStr, "%d", port);
    snprintf(verStr, sizeof verStr, "%d", version);
    g_stub = fork();
    if (g_stub == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execlp("python3", "python3", "-I", script, "--port", portStr, "--version", verStr, (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 50; i++) {
        char cmd[260];
        if (!test_child_alive(g_stub)) { g_stub = 0; return -1; }
        snprintf(cmd, sizeof cmd, "curl -s -m 2 -X POST -H 'Content-Type: application/json' -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_chainId\"}' http://127.0.0.1:%d 2>/dev/null | grep -q 7a69", port);
        if (system(cmd) == 0) return 0;
        usleep(200 * 1000);
    }
    return -1;
}

static void stop_stub(void) {
    if (g_stub > 0) { kill(g_stub, SIGTERM); waitpid(g_stub, NULL, 0); g_stub = 0; }
}

/* the config the client reads from a chain reporting `version`: NULL when refused */
static HppliteSystemConfig *config_at(const char *script, int version, uint64_t *pVersion) {
    int port = test_free_port();
    char url[64];
    HppliteSystemConfig *cfg = NULL;
    if (pVersion) *pVersion = 0;
    if (start_stub(script, port, version) != 0) { stop_stub(); return NULL; }
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    HppliteL1 *l1 = hpplite_l1_connect(url, "0x1111111111111111111111111111111111111111");
    if (l1) {
        cfg = hpplite_l1_get_system_config(l1);
        if (cfg && pVersion) *pVersion = cfg->version;
        hpplite_l1_disconnect(l1);
    }
    stop_stub();
    return cfg;
}

static void test_versions(const char *script) {
    uint64_t v = 0;
    TEST("VERSION 10 is read: the system config comes back with its version");
    HppliteSystemConfig *ok = config_at(script, 10, &v);
    if (!ok) FAIL("a VERSION 10 chain should be read (is the stub up? curl present?)");
    hpplite_l1_system_config_free(ok);
    if (v != 10) FAIL("the version read is not 10");
    PASS();

    TEST("VERSION 7 is refused: no system config (the factories before VERSION 8 are not read since v0.8, #252)");
    HppliteSystemConfig *old = config_at(script, 7, &v);
    if (old) { hpplite_l1_system_config_free(old); FAIL("a VERSION 7 chain was read"); }
    PASS();

    TEST("VERSION 8 is still read (DA version 2: the bytes in the log)");
    HppliteSystemConfig *v8 = config_at(script, 8, &v);
    if (!v8) FAIL("a VERSION 8 chain should be read");
    hpplite_l1_system_config_free(v8);
    PASS();
}

int main(int argc, char **argv) {
    const char *script = argc > 1 ? argv[1] : "tests/unit/rpc_stub.py";
    test_suite_init("l1_version", NULL, NULL);
    printf("The chain client and the coordinator's VERSION (#252)\n");
    test_versions(script);
    remove_dir();
    return test_report();
}
