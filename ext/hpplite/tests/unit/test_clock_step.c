/*
** The wall clock stepped back under a running node (somaro: a VM's guest agent setting it from the RTC every 10 s).
** Intervals are monotonic, so a step changes none of them: the timer's next tick, the RPC client's short "down" window
** after a timeout. The clock is really stepped (sudo date), so this runs only where that harms nothing - a throwaway
** CI runner, HPPLITE_CLOCK_STEP=1 with passwordless sudo; anywhere else it skips (77).
*/
#include "test_lib.h"
#include "eth/eth_client.h"
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

static const unsigned char PRIVKEY[32] = { [0] = 0xc1, [31] = 0x5e };
#define DIR "/tmp/hpplite_test_clock_step"

static int step(int seconds) {
    char cmd[200];
    snprintf(cmd, sizeof cmd, "sudo -n date -s @$(( $(date +%%s) + (%d) )) >/dev/null", seconds);
    return system(cmd);
}
static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* 1. the timer thread: a step back during its wait must not hold it there for the step */
static void test_timer_after_step(void) {
    TEST("the wall clock stepped back 120 s under a running sequencer: the timer's next batch comes within its interval, not 120 s later");
    reset_dir();
    TestNodeOpts o = { .checkpointInterval = TEST_INTERVAL_1000, .intervalMs = 200 };
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = test_open_node(DIR "/c.db", l1, &o);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(v)", NULL);
    hpplite_node_start_timer(n);
    sqlite3_int64 h = -1;
    for (int i = 0; i < 50 && (h = status_int(n->db, "height")) < 2; i++) usleep(100000);
    usleep(500000);
    h = status_int(n->db, "height");
    int stepped = step(-120) == 0;
    hpplite_node_exec(n, "INSERT INTO t VALUES(1)", NULL);
    double t0 = mono();
    sqlite3_int64 h2 = h;
    for (int i = 0; i < 100 && (h2 = status_int(n->db, "height")) <= h; i++) usleep(100000);
    double took = mono() - t0;
    if (stepped) step(120);
    hpplite_node_stop_timer(n);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    printf("(the batch after the step in %.1f s) ", took);
    if (!stepped) FAIL("the clock could not be stepped");
    if (h2 <= h || took > 3.0) FAIL("the timer waited out the step: no batch within 3 s of a write");
    PASS();
}

/* 2. the RPC client: after a timeout it rests 10 s, measured on the monotonic clock */
static volatile int gAnswer;
static int gListen = -1;
static void *rpc_server(void *arg) {
    (void)arg;
    for (;;) {
        int c = accept(gListen, NULL, NULL);
        if (c < 0) return NULL;
        char buf[4096];
        if (!gAnswer) { sleep(3); close(c); continue; }   /* the frozen endpoint: no answer before the client's timeout */
        (void)!read(c, buf, sizeof buf);
        const char *body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"number\":\"0x10\",\"timestamp\":\"0x65000000\"}}";
        char resp[512];
        int n = snprintf(resp, sizeof resp, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", strlen(body), body);
        (void)!write(c, resp, (size_t)n);
        close(c);
    }
}
static void test_rpc_rest_after_step(void) {
    TEST("the wall clock stepped back 60 s just after an RPC timeout: the client tries again after its 10 s rest, not 70 s");
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t len = sizeof a;
    gListen = socket(AF_INET, SOCK_STREAM, 0);
    if (gListen < 0 || bind(gListen, (struct sockaddr *)&a, sizeof a) != 0 || listen(gListen, 16) != 0 || getsockname(gListen, (struct sockaddr *)&a, &len) != 0) FAIL("listen");
    pthread_t th;
    pthread_create(&th, NULL, rpc_server, NULL);
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d", ntohs(a.sin_port));
    setenv("HPPLITE_RPC_TIMEOUT", "1", 1);
    EthClient *c = eth_client_create(url, 31337);
    uint64_t num = 0, ts = 0;
    int first = eth_client_latest_block(c, &num, &ts);           /* times out: the client rests */
    int stepped = step(-60) == 0;
    gAnswer = 1;
    sleep(11);                                                   /* past the rest, on any clock but a stepped wall clock */
    int after = eth_client_latest_block(c, &num, &ts);
    if (stepped) step(60);
    eth_client_destroy(c);
    unsetenv("HPPLITE_RPC_TIMEOUT");
    printf("(first call %d, after 11 s %d, block %llu) ", first, after, (unsigned long long)num);
    if (!stepped) FAIL("the clock could not be stepped");
    if (first == 0) FAIL("the frozen endpoint answered");
    if (after != 0 || num != 0x10) FAIL("still resting after 11 s: the rest was measured on the stepped wall clock");
    PASS();
}

int main(void) {
    if (!getenv("HPPLITE_CLOCK_STEP") || system("sudo -n true 2>/dev/null") != 0) {
        printf("SKIP: steps the wall clock (sudo date): HPPLITE_CLOCK_STEP=1 on a throwaway machine only\n");
        return 77;
    }
    test_suite_init("clock", PRIVKEY, DIR);
    (void)!system("sudo -n timedatectl set-ntp false 2>/dev/null");
    printf("HPPLite under a wall clock stepped back\n");
    test_timer_after_step();
    test_rpc_rest_after_step();
    return test_report();
}
