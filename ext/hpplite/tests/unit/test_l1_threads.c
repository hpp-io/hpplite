/*
** One process, many databases (hpplite-sequencer --dir): the chain client's first use may come from several threads at
** once - each database's timer thread connects on its own. The ABI selectors are computed once, behind a
** pthread_once; before, a plain flag guarded them and concurrent first use was a data race (ThreadSanitizer:
** 'data race ... init_selectors'). Run under -fsanitize=thread to see it; without the sanitizer this checks
** that the threads all connect.
*/
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "test_lib.h"
#include "l1_interface.h"

#define THREADS 8
static atomic_int g_ready, g_go, g_ok;

static void *connect_once(void *arg) {
    (void)arg;
    atomic_fetch_add(&g_ready, 1);
    while (!atomic_load(&g_go)) { }
    HppliteL1 *l1 = hpplite_l1_connect("http://127.0.0.1:9", "0x00000000000000000000000000000000000000a1");
    if (l1) { atomic_fetch_add(&g_ok, 1); hpplite_l1_disconnect(l1); }
    return NULL;
}

static void test_first_use_from_threads(void) {
    TEST("the chain client's first use from several threads at once: every thread connects (and no data race under ThreadSanitizer)");
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; i++) pthread_create(&th[i], NULL, connect_once, NULL);
    while (atomic_load(&g_ready) < THREADS) { }
    atomic_store(&g_go, 1);
    for (int i = 0; i < THREADS; i++) pthread_join(th[i], NULL);
    if (atomic_load(&g_ok) != THREADS) FAIL("a thread did not connect");
    PASS();
}

int main(void) {
    test_suite_init("l1_threads", NULL, NULL);
    printf("The chain client from many threads (many databases in one process)\n");
    test_first_use_from_threads();
    return test_report();
}
