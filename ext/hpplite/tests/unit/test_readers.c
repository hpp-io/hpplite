/*
** Readers and viewing keys (#15): batches sealed to the wallet and to the
** readers granted, each opening them with its own key alone; a grant covers
** what follows, a revoke ends at the next series; one data key per series.
*/
#include "test_lib.h"
#include "envelope_pk.h"

#define DIR "/tmp/hpplite_test_readers"
#define KEY_W "0x0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
#define KEY_R "0x3131313131313131313131313131313131313131313131313131313131313131"
#define KEY_S "0x3232323232323232323232323232323232323232323232323232323232323232"
#define KEY_X "0x3333333333333333333333333333333333333333333333333333333333333333"

static void pubhex(const char *privHex, char out[70]) {
    unsigned char priv[32], pub[33];
    hpplite_hex_to_bytes(privHex, priv, 32);
    if (hpplite_pk_pubkey(priv, pub) != 0) { out[0] = 0; return; }
    out[0] = '0'; out[1] = 'x';
    hpplite_bytes_to_hex(pub, 33, out + 2);
}

/* a replica with the given URI extra, synced from l1: rows of t (-1: no table), and the last error */
static sqlite3_int64 replica_rows(HppliteL1 *l1, const char *file, const char *extra, char *err, size_t nErr) {
    char uri[600];
    sqlite3_int64 rows = -1;
    snprintf(uri, sizeof uri, "file:" DIR "/%s?hpplite=on&datadir=" DIR "%s", file, extra);
    sqlite3 *r = hpplite_open(uri);
    HppliteNode *rn = r ? hpplite_get_node(r) : NULL;
    if (rn) {
        hpplite_node_set_l1(rn, l1);
        hpplite_node_sync_from_l1(rn, 1);
        rows = count_rows(r, "t");
        snprintf(err, nErr, "%s", status_text(r, "last_error"));
        rn->l1 = NULL;
    }
    if (r) sqlite3_close(r);
    return rows;
}

static void test_grant_and_read(void) {
    TEST("a reader granted reads what follows with its viewing key alone; the wallet reads too; another key does not; history before the grant stays the wallet's");
    reset_dir();
    char pubR[70], err[300], sql[200];
    pubhex(KEY_R, pubR);
    HppliteL1 *l1 = hpplite_l1_create_mock();
    sqlite3 *w = hpplite_open("file:" DIR "/w.db?hpplite=on&datadir=" DIR "&privkey=" KEY_W);
    if (!w) FAIL("open writer");
    HppliteNode *wn = hpplite_get_node(w);
    hpplite_node_set_l1(wn, l1); hpplite_node_start(wn);
    sqlite3_exec(w, "CREATE TABLE t(x); INSERT INTO t VALUES('before any grant')", NULL, NULL, NULL);
    if (hpplite_flush(w) != 1) FAIL("batch 1");
    if (status_int(w, "readers") != 0) FAIL("no readers yet");
    snprintf(sql, sizeof sql, "SELECT hpplite_grant('%s')", pubR);
    if (scalar(w, sql) != 1 || status_int(w, "readers") != 1) FAIL("grant");
    if (exec_sql(w, "SELECT hpplite_grant('0xdeadbeef')") == SQLITE_OK) FAIL("a bad reader key should be refused");
    sqlite3_exec(w, "INSERT INTO t VALUES('for the reader')", NULL, NULL, NULL);
    if (hpplite_flush(w) != 2) FAIL("batch 2");
    {
        size_t len = 0;
        uint8_t *raw1 = hpplite_l1_get_batch(l1, 1, &len);
        int v2 = raw1 && hpplite_envelope_is_sealed(raw1, len) && !hpplite_envelope_is_pk(raw1, len);
        free(raw1);
        uint8_t *raw2 = hpplite_l1_get_batch(l1, 2, &len);
        int v3 = raw2 && hpplite_envelope_is_pk(raw2, len);
        uint8_t fps[8];
        int n = raw2 ? hpplite_envelope_pk_fingerprints(raw2, len, fps, 2) : -1;
        free(raw2);
        if (!v2) FAIL("batch 1, before the grant, is the symmetric envelope");
        if (!v3 || n != 2) FAIL("batch 2 is sealed to the wallet and the reader");
    }
    /* the reader, with its viewing key alone: batch 1 is not for it, so it stops there */
    sqlite3_int64 rows = replica_rows(l1, "r.db", "&viewkey=" KEY_R, err, sizeof err);
    if (rows != -1 || !err[0]) { printf("(rows %lld, '%s') ", (long long)rows, err); FAIL("a reader granted later cannot read the history before the grant"); }
    /* the wallet reads everything */
    rows = replica_rows(l1, "rw.db", "&privkey=" KEY_W, err, sizeof err);
    if (rows != 2) { printf("(rows %lld, '%s') ", (long long)rows, err); FAIL("the wallet reads both batches"); }
    wn->l1 = NULL; sqlite3_close(w);
    hpplite_l1_disconnect(l1);
    PASS();
}

static void test_reader_from_the_start_and_revoke(void) {
    TEST("readers granted before the first batch rebuild everything; a revoked reader reads history and not the next series; the other reader goes on; hpplite_viewkey() keys a replica after open");
    reset_dir();
    char pubR[70], pubS[70], err[300], sql[200];
    pubhex(KEY_R, pubR); pubhex(KEY_S, pubS);
    HppliteL1 *l1 = hpplite_l1_create_mock();
    sqlite3 *w = hpplite_open("file:" DIR "/w2.db?hpplite=on&datadir=" DIR "&privkey=" KEY_W);
    if (!w) FAIL("open writer");
    HppliteNode *wn = hpplite_get_node(w);
    hpplite_node_set_l1(wn, l1); hpplite_node_start(wn);
    snprintf(sql, sizeof sql, "SELECT hpplite_grant('%s')", pubR); if (scalar(w, sql) != 1) FAIL("grant R");
    snprintf(sql, sizeof sql, "SELECT hpplite_grant('%s')", pubS); if (scalar(w, sql) != 1) FAIL("grant S");
    sqlite3_exec(w, "CREATE TABLE t(x); INSERT INTO t VALUES('one')", NULL, NULL, NULL);
    if (hpplite_flush(w) != 1) FAIL("batch 1");
    sqlite3_exec(w, "INSERT INTO t VALUES('two')", NULL, NULL, NULL);
    if (hpplite_flush(w) != 2) FAIL("batch 2");
    sqlite3_int64 rows = replica_rows(l1, "r2.db", "&viewkey=" KEY_R, err, sizeof err);
    if (rows != 2) { printf("(rows %lld, '%s') ", (long long)rows, err); FAIL("reader R rebuilds both batches with its viewing key"); }
    rows = replica_rows(l1, "x2.db", "&viewkey=" KEY_X, err, sizeof err);
    if (rows != -1 || !strstr(err, "sealed")) { printf("(rows %lld, '%s') ", (long long)rows, err); FAIL("a key never granted is told so"); }
    {
        /* the viewing key given after open */
        sqlite3 *r = hpplite_open("file:" DIR "/r2b.db?hpplite=on&datadir=" DIR);
        HppliteNode *rn = r ? hpplite_get_node(r) : NULL;
        sqlite3_int64 got = -1;
        if (rn) {
            if (scalar(r, "SELECT hpplite_viewkey('" KEY_S "')") != 1) { rn->l1 = NULL; sqlite3_close(r); FAIL("hpplite_viewkey"); }
            hpplite_node_set_l1(rn, l1); hpplite_node_sync_from_l1(rn, 1);
            got = count_rows(r, "t");
            rn->l1 = NULL;
        }
        if (r) sqlite3_close(r);
        if (got != 2) FAIL("a viewing key given after open reads the batches");
    }
    snprintf(sql, sizeof sql, "SELECT hpplite_revoke('%s')", pubR);
    if (scalar(w, sql) != 1 || status_int(w, "readers") != 1) FAIL("revoke R");
    sqlite3_exec(w, "INSERT INTO t VALUES('three, after the revoke')", NULL, NULL, NULL);
    if (hpplite_flush(w) != 3) FAIL("batch 3");
    rows = replica_rows(l1, "r3.db", "&viewkey=" KEY_R, err, sizeof err);
    if (rows != 2 || !strstr(err, "sealed")) { printf("(rows %lld, '%s') ", (long long)rows, err); FAIL("the revoked reader reads the history and stops at the next series"); }
    rows = replica_rows(l1, "s3.db", "&viewkey=" KEY_S, err, sizeof err);
    if (rows != 3) { printf("(rows %lld, '%s') ", (long long)rows, err); FAIL("the other reader goes on"); }
    wn->l1 = NULL; sqlite3_close(w);
    hpplite_l1_disconnect(l1);
    PASS();
}


int main(void) {
    printf("=== Readers and viewing keys (#15) ===\n");
    test_suite_init("readers", NULL, DIR);
    test_grant_and_read();
    test_reader_from_the_start_and_revoke();
    remove_dir();
    return test_report();
}
