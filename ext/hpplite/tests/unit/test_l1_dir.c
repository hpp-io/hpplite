/*
** The directory backend (#35, step b): l1=file:///path is a coordinator and
** a store in a directory. Lease, checkpoints and batches are files written
** by temporary name and rename; every process on the directory reads them
** on every call, so several see one state. No wallet is needed.
**
** Before this, "file://" was handed to the Ethereum client, which could not
** connect, and a node given l1=file:// kept its log pending for ever.
*/

#include "test_lib.h"
#include "l1_interface.h"
#include "crypto.h"
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

static const unsigned char INST_A[32] = { 1 };
static const unsigned char INST_B[32] = { 2 };
#define KEY_A "0x0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"

static const char *dir_url(const char *sub) {
    static char buf[2][1200];
    static int i;
    char *z = buf[i++ & 1];
    snprintf(z, sizeof(buf[0]), "file://%s/%s", test_dir(), sub);
    return z;
}

/* Leftovers of an interrupted write: <name>.tmp or the claim lock */
static int leftover_files(const char *sub) {
    DIR *d = opendir(tpath(sub));
    struct dirent *e;
    int n = 0;
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if ((len > 4 && strcmp(e->d_name + len - 4, ".tmp") == 0) || strcmp(e->d_name, "lease.lock") == 0) n++;
    }
    closedir(d);
    return n;
}

static int file_exists(const char *sub, const char *name) {
    char path[1200];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", tpath(sub), name);
    return stat(path, &st) == 0;
}

static void free_tx(char *tx) { sqlite3_free(tx); }

/* hpplite-verify on a directory: the binary comes from argv[1] (ctest) or HPPLITE_VERIFY_BIN */
static const char *verifyBin;
static int run_verify(const char *params, const char *liveDb, char *out, size_t n) {
    char cmd[3000];
    FILE *p;
    size_t got = 0;
    int status;
    out[0] = 0;
    if (!verifyBin) return -2;
    snprintf(cmd, sizeof cmd, "HPPLITE_PARAMS='%s' '%s' --db '%s' 2>&1", params, verifyBin, liveDb);
    p = popen(cmd, "r");
    if (!p) return -2;
    while (got + 1 < n && fgets(out + got, (int)(n - got), p)) got = strlen(out);
    status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void test_lease(void) {
    TEST("the lease: one instance at a time, seen by every connection, renewed and released by its holder");
    reset_dir();
    HppliteL1 *a = hpplite_l1_connect(dir_url("da"), NULL);
    HppliteL1 *b = hpplite_l1_connect(dir_url("da"), NULL);
    if (!a || !b) FAIL("connect(file://) returned NULL");
    const char *name = hpplite_l1_backend_name(a);
    if (!name || strcmp(name, "dir") != 0) FAILF("backend is '%s', want dir", name ? name : "(null)");

    char *tx = hpplite_l1_claim_sequencer(a, INST_A);
    if (!tx) FAIL("A could not claim a free lease");
    free_tx(tx);
    if (!hpplite_l1_is_lease_active(a) || !hpplite_l1_is_lease_active(b)) FAIL("the lease is not active on both connections");
    if (hpplite_l1_claim_sequencer(b, INST_B) != NULL) FAIL("B claimed a lease A holds");

    HppliteL1State *st = hpplite_l1_get_state(b);
    if (!st) FAIL("get_state");
    uint64_t now = (uint64_t)time(NULL);
    if (memcmp(st->sequencerInstance, INST_A, 32) != 0) { hpplite_l1_state_free(st); FAIL("state names the wrong instance"); }
    if (st->leaseExpiry <= st->currentTimestamp) { hpplite_l1_state_free(st); FAIL("lease expiry not in the future"); }
    if (st->currentTimestamp + 5 < now || st->currentTimestamp > now + 5) { hpplite_l1_state_free(st); FAIL("the directory's clock is the wall clock"); }
    if (st->leaseDuration == 0) { hpplite_l1_state_free(st); FAIL("lease duration not reported"); }
    hpplite_l1_state_free(st);
    if (hpplite_l1_get_lease_time_remaining(b) == 0) FAIL("time remaining is 0 while held");

    if (hpplite_l1_renew_lease(b, INST_B) != NULL) FAIL("B renewed A's lease");
    tx = hpplite_l1_renew_lease(a, INST_A);
    if (!tx) FAIL("A could not renew its own lease");
    free_tx(tx);
    if (hpplite_l1_release_lease(b, INST_B) != NULL) FAIL("B released A's lease");
    tx = hpplite_l1_release_lease(a, INST_A);
    if (!tx) FAIL("A could not release its lease");
    free_tx(tx);
    if (hpplite_l1_is_lease_active(a) || hpplite_l1_is_lease_active(b)) FAIL("released lease still active");
    tx = hpplite_l1_claim_sequencer(b, INST_B);
    if (!tx) FAIL("B could not claim the released lease");
    free_tx(tx);
    if (leftover_files("da") != 0) FAIL("temporary files or the claim lock left behind");
    hpplite_l1_disconnect(a);
    hpplite_l1_disconnect(b);
    PASS();
}

static void test_lease_expires(void) {
    TEST("a lease expires by the wall clock: a crashed holder is replaced after leaseDuration");
    reset_dir();
    mkdir(tpath("da2"), 0700);
    FILE *f = fopen(tpath("da2/config"), "w");
    if (!f) FAIL("write config");
    fprintf(f, "checkpointInterval 100\nleaseDuration 1\n");
    fclose(f);
    HppliteL1 *a = hpplite_l1_connect(dir_url("da2"), NULL);
    if (!a) FAIL("connect");
    char *tx = hpplite_l1_claim_sequencer(a, INST_A);
    if (!tx) FAIL("claim");
    free_tx(tx);
    hpplite_l1_disconnect(a);                              /* the holder is gone without releasing */
    usleep(1500 * 1000);
    HppliteL1 *b = hpplite_l1_connect(dir_url("da2"), NULL);
    if (!b) FAIL("reconnect");
    if (hpplite_l1_is_lease_active(b)) FAIL("lease still active after leaseDuration");
    if (hpplite_l1_get_lease_time_remaining(b) != 0) FAIL("time remaining after expiry");
    tx = hpplite_l1_claim_sequencer(b, INST_B);
    if (!tx) FAIL("B could not claim the expired lease");
    free_tx(tx);
    hpplite_l1_disconnect(b);
    PASS();
}

static void test_store(void) {
    TEST("the store: sequential heights by the lease holder, bytes and hashes back, state survives reconnect");
    reset_dir();
    HppliteL1 *a = hpplite_l1_connect(dir_url("da3"), NULL);
    if (!a) FAIL("connect");
    const uint8_t one[] = "batch one", two[] = "batch two, longer";
    if (hpplite_l1_submit_batch(a, INST_A, 1, one, sizeof(one) - 1) != NULL) FAIL("a batch was taken without the lease");
    char *tx = hpplite_l1_claim_sequencer(a, INST_A);
    if (!tx) FAIL("claim");
    free_tx(tx);
    if (hpplite_l1_submit_batch(a, INST_A, 2, one, sizeof(one) - 1) != NULL) FAIL("height 2 was taken before height 1");
    if (hpplite_l1_submit_batch(a, INST_B, 1, one, sizeof(one) - 1) != NULL) FAIL("another instance posted under A's lease");
    tx = hpplite_l1_submit_batch(a, INST_A, 1, one, sizeof(one) - 1);
    if (!tx) FAIL("submit height 1");
    free_tx(tx);
    tx = hpplite_l1_submit_batch(a, INST_A, 2, two, sizeof(two) - 1);
    if (!tx) FAIL("submit height 2");
    free_tx(tx);

    size_t n = 0;
    uint8_t *back = hpplite_l1_get_batch(a, 1, &n);
    if (!back || n != sizeof(one) - 1 || memcmp(back, one, n) != 0 || back[n] != 0) { free(back); FAIL("get_batch(1): bytes or the terminator"); }
    free(back);
    back = hpplite_l1_get_batch(a, 3, &n);
    if (back || n != 0) { free(back); FAIL("get_batch(3) should be NULL with length 0"); }
    uint8_t want[32], got[32];
    hpplite_keccak256(two, sizeof(two) - 1, want);
    if (hpplite_l1_get_batch_hash(a, 2, got) != 1 || memcmp(got, want, 32) != 0) FAIL("get_batch_hash(2) is not keccak of the bytes");
    if (hpplite_l1_get_batch_hash(a, 3, got) != 0) FAIL("get_batch_hash(3) should say no batch");
    uint64_t last = 0, total = 0;
    if (hpplite_l1_get_da_state(a, &last, &total, got) != 0 || last != 2 || total != 2 || memcmp(got, want, 32) != 0) FAIL("da_state");

    tx = hpplite_l1_release_lease(a, INST_A);
    if (!tx) FAIL("release");
    free_tx(tx);
    if (hpplite_l1_submit_batch(a, INST_A, 3, one, sizeof(one) - 1) != NULL) FAIL("a batch was taken after the release");
    hpplite_l1_disconnect(a);

    HppliteL1 *b = hpplite_l1_connect(dir_url("da3"), NULL);
    if (!b) FAIL("reconnect");
    if (hpplite_l1_get_da_state(b, &last, &total, NULL) != 0 || last != 2) FAIL("height lost across reconnect");
    back = hpplite_l1_get_batch(b, 1, &n);
    if (!back || n != sizeof(one) - 1 || memcmp(back, one, n) != 0) { free(back); FAIL("batch 1 lost across reconnect"); }
    free(back);
    if (!file_exists("da3", "batch-1.bin") || !file_exists("da3", "batch-2.bin")) FAIL("batch files not where documented");
    if (leftover_files("da3") != 0) FAIL("temporary files left behind");
    hpplite_l1_disconnect(b);
    PASS();
}

static void test_checkpoint_and_config(void) {
    TEST("checkpoints and the checkpoint interval are kept in the directory and read back by a fresh connection");
    reset_dir();
    HppliteL1 *a = hpplite_l1_connect(dir_url("da4"), NULL);
    if (!a) FAIL("connect");
    HppliteCheckpoint cp;
    memset(&cp, 0, sizeof(cp));
    cp.fromHeight = 1; cp.toHeight = 2; cp.timestamp = 12345;
    memset(cp.postStateRoot, 0xab, 32);
    if (hpplite_l1_submit_checkpoint(a, &cp, INST_A) != NULL) FAIL("a checkpoint was taken without the lease");
    char *tx = hpplite_l1_claim_sequencer(a, INST_A);
    if (!tx) FAIL("claim");
    free_tx(tx);
    tx = hpplite_l1_submit_checkpoint(a, &cp, INST_A);
    if (!tx) FAIL("submit_checkpoint");
    free_tx(tx);
    tx = hpplite_l1_set_checkpoint_interval(a, 7);
    if (!tx) FAIL("set_checkpoint_interval");
    free_tx(tx);
    hpplite_l1_disconnect(a);

    HppliteL1 *b = hpplite_l1_connect(dir_url("da4"), NULL);
    if (!b) FAIL("reconnect");
    HppliteL1State *st = hpplite_l1_get_state(b);
    if (!st) FAIL("get_state");
    unsigned char root[32];
    memset(root, 0xab, 32);
    if (st->lastCheckpointHeight != 2 || memcmp(st->lastStateRoot, root, 32) != 0 || st->lastCheckpointTime == 0) {
        hpplite_l1_state_free(st); FAIL("checkpoint not read back");
    }
    if (st->checkpointInterval != 7) { hpplite_l1_state_free(st); FAIL("checkpoint interval not read back"); }
    hpplite_l1_state_free(st);
    HppliteSystemConfig *cfg = hpplite_l1_get_system_config(b);
    if (!cfg) FAIL("get_system_config");
    /* batchSizeLimit 0: a directory imposes none, so the node's own batch limit stands */
    if (!cfg->daScheme || strcmp(cfg->daScheme, "file") != 0 || cfg->checkpointInterval != 7 || cfg->batchSizeLimit != 0 || cfg->leaseDuration == 0) {
        hpplite_l1_system_config_free(cfg); FAIL("system config");
    }
    hpplite_l1_system_config_free(cfg);
    if (hpplite_l1_connect_factory(dir_url("da4"), "0x1111111111111111111111111111111111111111", INST_A) != NULL) FAIL("a directory has no factory");
    hpplite_l1_disconnect(b);
    PASS();
}

/* The reason for all of it: a node on l1=file:// posts, a replica rebuilds from the directory */
static void node_case(const char *tag, const char *keyParams) {
    char uri[2048];
    reset_dir();
    snprintf(uri, sizeof(uri), "file:%s/w.db?hpplite=on&l1=%s&interval=0&datadir=%s%s",
             test_dir(), dir_url("node_da"), test_dir(), keyParams);
    sqlite3 *w = hpplite_open(uri);
    if (!w) { printf("FAIL: %s: open writer\n", tag); return; }
    HppliteNode *wn = hpplite_get_node(w);
    if (!wn) { hpplite_close(w); printf("FAIL: %s: no node\n", tag); return; }
    exec_sql(w, "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT)");
    exec_sql(w, "INSERT INTO t(b) VALUES('one'),('two'),('three')");
    uint64_t h = hpplite_node_flush_batch(wn);
    if (h != 1) { hpplite_close(w); printf("FAIL: %s: first flush gave height %llu, want 1 (the log stayed pending)\n", tag, (unsigned long long)h); return; }
    if (hpplite_node_pending_count(wn) != 0) { hpplite_close(w); printf("FAIL: %s: rows pending after the flush\n", tag); return; }
    if (strcmp(status_text(w, "state"), "sequencing") != 0) { printf("FAIL: %s: state is '%s', want sequencing\n", tag, status_text(w, "state")); hpplite_close(w); return; }
    if (!file_exists("node_da", "batch-1.bin")) { hpplite_close(w); printf("FAIL: %s: batch-1.bin not in the directory\n", tag); return; }
    exec_sql(w, "UPDATE t SET b = b || '!' WHERE a = 2; DELETE FROM t WHERE a = 3; INSERT INTO t(b) VALUES('four')");
    h = hpplite_node_flush_batch(wn);
    if (h != 2) { hpplite_close(w); printf("FAIL: %s: second flush gave %llu\n", tag, (unsigned long long)h); return; }

    snprintf(uri, sizeof(uri), "file:%s/r.db?hpplite=on&l1=%s&role=verify&interval=0&datadir=%s%s",
             test_dir(), dir_url("node_da"), test_dir(), keyParams);
    sqlite3 *r = hpplite_open(uri);
    if (!r) { hpplite_close(w); printf("FAIL: %s: open replica\n", tag); return; }
    char *dw = test_dump(w, 0), *dr = test_dump(r, 0);
    int equal = dw && dr && strcmp(dw, dr) == 0;
    if (!equal) printf("FAIL: %s: replica differs from the writer at offset %zu\n  writer: %s\n  replica: %s\n", tag, test_dump_diff(dw, dr), dw ? dw : "(null)", dr ? dr : "(null)");
    sqlite3_free(dw);
    sqlite3_free(dr);
    if (!equal) { hpplite_close(r); hpplite_close(w); return; }
    /* hpplite_status.height is the next height; the directory itself says two batches are posted */
    if (status_int(r, "height") != status_int(w, "height")) { printf("FAIL: %s: replica height %lld, writer %lld\n", tag, (long long)status_int(r, "height"), (long long)status_int(w, "height")); hpplite_close(r); hpplite_close(w); return; }
    {
        HppliteL1 *peek = hpplite_l1_connect(dir_url("node_da"), NULL);
        uint64_t last = 0, total = 0;
        int ok = peek && hpplite_l1_get_da_state(peek, &last, &total, NULL) == 0 && last == 2;
        hpplite_l1_disconnect(peek);
        if (!ok) { printf("FAIL: %s: the directory holds %llu batches, want 2\n", tag, (unsigned long long)last); hpplite_close(r); hpplite_close(w); return; }
    }
    if (strcmp(status_text(w, "state_root"), status_text(r, "state_root")) != 0) { printf("FAIL: %s: roots differ\n", tag); hpplite_close(r); hpplite_close(w); return; }
    hpplite_close(r);

    /* hpplite-verify, the offline audit: rebuilds from the directory alone while the writer holds the lease */
    if (verifyBin) {
        char params[1500], out[4096], live[1200];
        snprintf(params, sizeof params, "l1=%s%s", dir_url("node_da"), keyParams);
        snprintf(live, sizeof live, "%s/w.db", test_dir());
        int rc = run_verify(params, live, out, sizeof out);
        if (rc != 0 || !strstr(out, "\nverified") || !strstr(out, "equals the chain")) {
            printf("FAIL: %s: hpplite-verify exit %d:\n%s", tag, rc, out); hpplite_close(w); return;
        }
        if (!hpplite_get_node(w) || strcmp(status_text(w, "state"), "sequencing") != 0) { printf("FAIL: %s: the audit took the writer's lease\n", tag); hpplite_close(w); return; }
    } else {
        printf("(no hpplite-verify binary given: audit skipped) ");
    }
    hpplite_close(w);                                      /* releases the lease */

    /* the writer comes back as a new instance: the lease was released at close, so it posts at once */
    snprintf(uri, sizeof(uri), "file:%s/w.db?hpplite=on&l1=%s&interval=0&datadir=%s%s",
             test_dir(), dir_url("node_da"), test_dir(), keyParams);
    w = hpplite_open(uri);
    if (!w) { printf("FAIL: %s: reopen writer\n", tag); return; }
    wn = hpplite_get_node(w);
    exec_sql(w, "INSERT INTO t(b) VALUES('five')");
    h = hpplite_node_flush_batch(wn);
    if (h != 3) { hpplite_close(w); printf("FAIL: %s: flush after reopen gave %llu, want 3\n", tag, (unsigned long long)h); return; }
    hpplite_close(w);
    if (leftover_files("node_da") != 0) { printf("FAIL: %s: temporary files left behind\n", tag); return; }
    printf("PASS\n");
    tests_passed++;
}

static void test_node_keyless(void) {
    TEST("a node on l1=file:// with no key posts plaintext batches; a replica rebuilds from the directory");
    node_case("keyless", "&encrypt=off");
}

static void test_node_keyed(void) {
    TEST("with a key the batches are sealed as on chain; a replica with the key rebuilds");
    node_case("keyed", "&privkey=" KEY_A);
}

int main(int argc, char **argv) {
    verifyBin = argc > 1 ? argv[1] : getenv("HPPLITE_VERIFY_BIN");
    test_suite_init("l1dir", NULL, NULL);
    printf("Directory backend (#35)\n");
    test_lease();
    test_lease_expires();
    test_store();
    test_checkpoint_and_config();
    test_node_keyless();
    test_node_keyed();
    remove_dir();
    return test_report();
}
