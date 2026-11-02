/*
** Key provisioning: every way a wallet key reaches HPPLite, on both open
** paths, and what happens when none does.
*/

#include "test_lib.h"
#include "config.h"
#include <sys/stat.h>

#define DIR "/tmp/hpplite_test_keys"
#define KEY_A "0x0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
#define KEY_B "0x2020202020202020202020202020202020202020202020202020202020202020"

/* a fresh directory, and no key from the environment */

static void reset_keys(void) { reset_dir(); unsetenv("HPPLITE_PRIVATE_KEY"); unsetenv("HPPLITE_PRIVATE_KEY_FILE"); }

static void write_file(const char *path, const char *content, mode_t mode) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(content, f); fclose(f); }
    chmod(path, mode);
}

static int key_first_byte(sqlite3 *db) {
    HppliteConfig *cfg = hpplite_get_config(db);
    if (!cfg || !cfg->hasPrivkey) return -1;
    return cfg->privkey[0];
}

static sqlite3 *open_v2(const char *uri) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(uri, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL) != SQLITE_OK) return NULL;
    return db;
}

static void test_uri_key(void) {
    TEST("privkey= in URI, both open paths");
    reset_keys();
    sqlite3 *db = hpplite_open("file:" DIR "/a.db?hpplite=on&datadir=" DIR "&privkey=" KEY_A);
    if (!db || key_first_byte(db) != 0x01) FAIL("hpplite_open");
    sqlite3_close(db);
    db = open_v2("file:" DIR "/b.db?hpplite=on&datadir=" DIR "&privkey=" KEY_A);
    if (!db || key_first_byte(db) != 0x01) FAIL("sqlite3_open_v2");
    sqlite3_close(db);
    PASS();
}

static void test_env_key_and_precedence(void) {
    TEST("HPPLITE_PRIVATE_KEY env; URI wins over env");
    reset_keys();
    setenv("HPPLITE_PRIVATE_KEY", KEY_B, 1);
    sqlite3 *db = hpplite_open("file:" DIR "/a.db?hpplite=on&datadir=" DIR);
    if (!db || key_first_byte(db) != 0x20) FAIL("env key via hpplite_open");
    sqlite3_close(db);
    db = open_v2("file:" DIR "/b.db?hpplite=on&datadir=" DIR);
    if (!db || key_first_byte(db) != 0x20) FAIL("env key via sqlite3_open_v2");
    sqlite3_close(db);
    db = open_v2("file:" DIR "/c.db?hpplite=on&datadir=" DIR "&privkey=" KEY_A);
    if (!db || key_first_byte(db) != 0x01) FAIL("URI should override env");
    sqlite3_close(db);
    unsetenv("HPPLITE_PRIVATE_KEY");
    PASS();
}

static void test_keyfile(void) {
    TEST("keyfile: 0600 accepted, formats tolerated, both open paths and env");
    reset_keys();
    write_file(DIR "/key", KEY_A "\n", 0600);
    sqlite3 *db = hpplite_open("file:" DIR "/a.db?hpplite=on&datadir=" DIR "&keyfile=" DIR "/key");
    if (!db || key_first_byte(db) != 0x01) FAIL("keyfile via hpplite_open");
    sqlite3_close(db);
    db = open_v2("file:" DIR "/b.db?hpplite=on&datadir=" DIR "&keyfile=" DIR "/key");
    if (!db || key_first_byte(db) != 0x01) FAIL("keyfile via sqlite3_open_v2");
    sqlite3_close(db);

    write_file(DIR "/key2", "  2020202020202020202020202020202020202020202020202020202020202020  \r\n", 0400);
    setenv("HPPLITE_PRIVATE_KEY_FILE", DIR "/key2", 1);
    db = open_v2("file:" DIR "/c.db?hpplite=on&datadir=" DIR);
    if (!db || key_first_byte(db) != 0x20) FAIL("bare hex with whitespace via HPPLITE_PRIVATE_KEY_FILE");
    sqlite3_close(db);
    unsetenv("HPPLITE_PRIVATE_KEY_FILE");
    PASS();
}

static void test_keyfile_rejections(void) {
    TEST("keyfile: world/group-readable, missing, directory, malformed are refused");
    reset_keys();
    write_file(DIR "/loose", KEY_A, 0644);
    sqlite3 *db = open_v2("file:" DIR "/a.db?hpplite=on&datadir=" DIR "&keyfile=" DIR "/loose");
    if (!db) FAIL("open");
    if (key_first_byte(db) != -1) FAIL("world-readable key file was accepted");
    sqlite3_close(db);

    write_file(DIR "/group", KEY_A, 0640);
    db = open_v2("file:" DIR "/b.db?hpplite=on&datadir=" DIR "&keyfile=" DIR "/group");
    if (!db || key_first_byte(db) != -1) FAIL("group-readable key file was accepted");
    sqlite3_close(db);

    db = open_v2("file:" DIR "/c.db?hpplite=on&datadir=" DIR "&keyfile=" DIR "/missing");
    if (!db || key_first_byte(db) != -1) FAIL("missing key file produced a key");
    sqlite3_close(db);

    mkdir(DIR "/adir", 0700);
    db = open_v2("file:" DIR "/d.db?hpplite=on&datadir=" DIR "&keyfile=" DIR "/adir");
    if (!db || key_first_byte(db) != -1) FAIL("directory accepted as key file");
    sqlite3_close(db);

    write_file(DIR "/short", "0xdeadbeef\n", 0600);
    db = open_v2("file:" DIR "/e.db?hpplite=on&datadir=" DIR "&keyfile=" DIR "/short");
    if (!db || key_first_byte(db) != -1) FAIL("malformed key accepted");
    sqlite3_close(db);

    db = open_v2("file:" DIR "/f.db?hpplite=on&datadir=" DIR "&privkey=0xnothex");
    if (!db || key_first_byte(db) != -1) FAIL("malformed URI key accepted");
    sqlite3_close(db);
    PASS();
}

static void test_no_key_never_posts(void) {
    TEST("no key: writes captured, nothing posted, status says keyed=0");
    SKIP_IF_PAGES("a connection HPPLite did not open captures nothing in page mode yet (#155); the guard itself: test_pagemode");
    reset_keys();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNodeConfig config;
    memset(&config, 0, sizeof(config));
    config.nodeId = "nokey"; config.dataDir = DIR; config.dbPath = DIR "/nokey.db";
    HppliteNode *n = hpplite_node_create(&config);
    if (!n) FAIL("open");
    if (n->config.hasPrivkey || n->cipher) FAIL("should have no key and no cipher");
    hpplite_node_set_l1(n, l1);
    hpplite_node_exec(n, "CREATE TABLE t(x); INSERT INTO t VALUES('secret');", NULL);
    if (hpplite_node_flush_batch(n) != 0) FAIL("flush must not post without a key");
    if (!test_pending_is(hpplite_node_pending_count(n), 2)) FAIL("pending must be kept");
    size_t len = 0;
    uint8_t *raw = hpplite_l1_get_batch(l1, 1, &len);
    if (raw) { free(raw); FAIL("a batch reached L1 without a key"); }

    sqlite3_stmt *s;
    sqlite3_prepare_v2(n->db, "SELECT keyed, encryption FROM hpplite_status", -1, &s, NULL);
    if (sqlite3_step(s) != SQLITE_ROW || sqlite3_column_int(s, 0) != 0
        || strcmp((const char*)sqlite3_column_text(s, 1), "none") != 0) FAIL("status should report keyed=0, encryption=none");
    sqlite3_finalize(s);
    n->l1 = NULL;
    hpplite_node_destroy(n);

    /* the documented URI form: an L1 is named but (no key) cannot be reached.
    ** The flush must keep everything pending rather than finalize locally and
    ** advance the height past batches L1 never saw */
    sqlite3 *db = open_v2("file:" DIR "/nokey_uri.db?hpplite=on&l1=hpp-sepolia&datadir=" DIR);
    if (!db) FAIL("open with l1= and no key");
    sqlite3_exec(db, "CREATE TABLE t(x); INSERT INTO t VALUES(1); SELECT hpplite_flush();", NULL, NULL, NULL);
    sqlite3_prepare_v2(db, "SELECT height, pending FROM hpplite_status", -1, &s, NULL);
    if (sqlite3_step(s) != SQLITE_ROW || sqlite3_column_int(s, 0) != 1 || sqlite3_column_int(s, 1) != 2) {
        printf("(height=%d pending=%d) ", sqlite3_column_int(s, 0), sqlite3_column_int(s, 1));
        sqlite3_finalize(s); sqlite3_close(db);
        FAIL("with l1= configured and no connection, nothing may be finalized locally");
    }
    sqlite3_finalize(s);
    sqlite3_close(db);
    hpplite_l1_disconnect(l1);
    PASS();
}

static void test_encrypt_off_is_explicit(void) {
    TEST("encrypt=off with a key posts plaintext; with a key and default, sealed");
    SKIP_IF_ROWS("the plaintext batch carries records (test_rows_e2e)");
    reset_keys();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    sqlite3 *db = hpplite_open("file:" DIR "/p.db?hpplite=on&encrypt=off&datadir=" DIR "&privkey=" KEY_A);
    if (!db) FAIL("open");
    HppliteNode *n = hpplite_get_node(db);
    hpplite_node_set_l1(n, l1); hpplite_node_start(n);
    sqlite3_exec(db, "CREATE TABLE t(x); INSERT INTO t VALUES('visible');", NULL, NULL, NULL);
    if (hpplite_flush(db) != 1) FAIL("flush plaintext");
    size_t len = 0;
    uint8_t *raw = hpplite_l1_get_batch(l1, 1, &len);
    {
        size_t n = 0;
        uint8_t *readable = raw ? test_readable_bytes(raw, len, &n) : NULL;   /* deflated since #12, still anyone's to read */
        int ok = readable && memmem(readable, n, "visible", 7) != NULL;
        free(readable);
        if (!ok) FAIL("encrypt=off should post plaintext");
    }
    free(raw);
    sqlite3_close(db);

    db = hpplite_open("file:" DIR "/s.db?hpplite=on&datadir=" DIR "&privkey=" KEY_B);
    if (!db) FAIL("open 2");
    n = hpplite_get_node(db);
    HppliteL1 *l2 = hpplite_l1_create_mock();
    hpplite_node_set_l1(n, l2); hpplite_node_start(n);
    sqlite3_exec(db, "CREATE TABLE t(x); INSERT INTO t VALUES('hidden');", NULL, NULL, NULL);
    if (hpplite_flush(db) != 1) FAIL("flush sealed");
    raw = hpplite_l1_get_batch(l2, 1, &len);
    if (!raw || memmem(raw, len, "hidden", 6)) FAIL("default should seal");
    free(raw);
    sqlite3_close(db);
    hpplite_l1_disconnect(l1); hpplite_l1_disconnect(l2);
    PASS();
}

/* #17 step 1: the key given to the connection after open, the SQLite-extension
** idiom: nothing posts until then; then the node is keyed, the lease taken,
** batches sealed and posted; a different key while keyed is refused. */
static void test_key_at_runtime(void) {
    TEST("hpplite_key()/hpplite_keyfile() after open: keyed, lease, sealed batches posted; another key while keyed, a malformed key, a loose key file refused (#17)");
    reset_keys();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    sqlite3 *db = hpplite_open("file:" DIR "/rt.db?hpplite=on&datadir=" DIR);          /* no key anywhere */
    if (!db) FAIL("open");
    HppliteNode *n = hpplite_get_node(db);
    hpplite_node_set_l1(n, l1); hpplite_node_start(n);
    sqlite3_exec(db, "CREATE TABLE t(x); INSERT INTO t VALUES('before the key')", NULL, NULL, NULL);
    if (hpplite_flush(db) != 0) FAIL("nothing may post without a key");
    if (status_int(db, "keyed") != 0) FAIL("keyed should be 0 before the key");
    if (exec_sql(db, "SELECT hpplite_key('0xnothex')") == SQLITE_OK) FAIL("a malformed key should be an error");
    if (scalar(db, "SELECT hpplite_key('" KEY_A "')") != 1) { printf("(%s) ", sqlite3_errmsg(db)); FAIL("hpplite_key should answer 1"); }
    if (status_int(db, "keyed") != 1 || status_int(db, "lease") != 1) FAIL("keyed and the lease after hpplite_key");
    if (hpplite_flush(db) != 1) FAIL("the flush after the key should post");
    {
        size_t len = 0;
        uint8_t *raw = hpplite_l1_get_batch(l1, 1, &len);
        int ok = raw && !memmem(raw, len, "before the key", 14);
        free(raw);
        if (!ok) FAIL("the batch should be posted, sealed with the key");
    }
    if (exec_sql(db, "SELECT hpplite_key('" KEY_B "')") == SQLITE_OK) FAIL("another key while keyed should be refused");
    if (scalar(db, "SELECT hpplite_key('" KEY_A "')") != 1) FAIL("the same key again should be fine");
    {
        /* a replica with the key reads what was posted */
        sqlite3 *r = hpplite_open("file:" DIR "/rt_r.db?hpplite=on&datadir=" DIR "&privkey=" KEY_A);
        HppliteNode *rn = r ? hpplite_get_node(r) : NULL;
        sqlite3_int64 rows = -1;
        if (rn) { hpplite_node_set_l1(rn, l1); hpplite_node_sync_from_l1(rn, 1); rows = scalar(r, "SELECT count(*) FROM t"); rn->l1 = NULL; }
        if (r) sqlite3_close(r);
        if (rows != 1) { printf("(replica rows %lld) ", (long long)rows); FAIL("a replica with the key should read the batch"); }
    }
    n->l1 = NULL;
    sqlite3_close(db);

    write_file(DIR "/rtkey", KEY_B "\n", 0600);
    write_file(DIR "/rtloose", KEY_B "\n", 0644);
    sqlite3 *db2 = hpplite_open("file:" DIR "/rt2.db?hpplite=on&datadir=" DIR);
    if (!db2) FAIL("open 2");
    if (exec_sql(db2, "SELECT hpplite_keyfile('" DIR "/rtloose')") == SQLITE_OK) FAIL("a world-readable key file should be refused");
    if (status_int(db2, "keyed") != 0) FAIL("still unkeyed after the refusal");
    if (scalar(db2, "SELECT hpplite_keyfile('" DIR "/rtkey')") != 1 || hpplite_get_node(db2)->config.privkey[0] != 0x20) FAIL("hpplite_keyfile should key the node from a 0600 file");
    sqlite3_close(db2);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* #17 step 2: an Ethereum keystore V3 file, at open (keystore= with the
** passphrase from HPPLITE_KEYSTORE_PASSWORD) and after it (hpplite_keystore) */
#ifndef HPPLITE_FIXTURES_DIR
#define HPPLITE_FIXTURES_DIR "tests/fixtures"
#endif
static const char *g_fixtures = HPPLITE_FIXTURES_DIR;
static void test_keystore(void) {
    TEST("keystore= with HPPLITE_KEYSTORE_PASSWORD keys the open; without the passphrase it opens unkeyed; hpplite_keystore(path, passphrase) keys it later; a wrong passphrase is an error (#17)");
    reset_keys();
    char uri[700], sql[700];
    unsigned char addr[20];
    snprintf(uri, sizeof uri, "file:" DIR "/ks.db?hpplite=on&datadir=" DIR "&keystore=%s/keystore_scrypt.json", g_fixtures);
    setenv("HPPLITE_KEYSTORE_PASSWORD", "test-pass", 1);
    sqlite3 *db = open_v2(uri);
    unsetenv("HPPLITE_KEYSTORE_PASSWORD");
    if (!db || status_int(db, "keyed") != 1) FAIL("keystore= with the passphrase should key the open");
    memcpy(addr, hpplite_get_node(db)->keypair.address, 20);
    sqlite3_close(db);
    if (addr[0] != 0xde || addr[19] != 0x31) FAIL("the key should be cast's 0xDE98...2631");
    db = open_v2(uri);                                                  /* no passphrase anywhere: unkeyed, said on stderr */
    if (!db || status_int(db, "keyed") != 0) FAIL("without the passphrase the open should be unkeyed");
    snprintf(sql, sizeof sql, "SELECT hpplite_keystore('%s/keystore_scrypt.json', 'wrong-pass')", g_fixtures);
    if (exec_sql(db, sql) == SQLITE_OK) FAIL("a wrong passphrase should be an error");
    snprintf(sql, sizeof sql, "SELECT hpplite_keystore('%s/keystore_scrypt.json', 'test-pass')", g_fixtures);
    if (scalar(db, sql) != 1 || status_int(db, "keyed") != 1) FAIL("hpplite_keystore should key the connection");
    sqlite3_close(db);
    PASS();
}

/* #17 step 3: a signer whose key lives elsewhere. The test's signer keeps
** the key itself and counts the calls; the batch key is given apart. */
typedef struct { HppliteCrypto *c; HppliteKeypair kp; int calls; } TestSigner;
static int test_signer_sign(void *arg, const unsigned char hash[32], unsigned char sig[64], int *recid) {
    TestSigner *t = (TestSigner *)arg;
    HppliteSignature s;
    t->calls++;
    if (hpplite_crypto_sign(t->c, &t->kp, hash, &s) != 0) return -1;
    memcpy(sig, s.sig, 64);
    *recid = s.recid;
    return 0;
}
static void test_signer(void) {
    TEST("a signer instead of a key: keyed, the batch key given apart, the lease taken, a sealed batch posted a replica with the batch key reads; a key after a signer is refused (#17)");
    reset_keys();
    TestSigner ts = { hpplite_crypto_init(), {{0}}, 0 };
    unsigned char raw[32], batchKey[32];
    HppliteSigner signer;
    hpplite_hex_to_bytes(KEY_A, raw, 32);
    if (hpplite_crypto_keypair_from_privkey(ts.c, &ts.kp, raw) != 0) FAIL("keypair");
    memset(batchKey, 0x42, 32);
    signer.sign = test_signer_sign; signer.arg = &ts; memcpy(signer.address, ts.kp.address, 20);
    HppliteL1 *l1 = hpplite_l1_create_mock();
    sqlite3 *db = hpplite_open("file:" DIR "/sg.db?hpplite=on&datadir=" DIR);
    if (!db) FAIL("open");
    HppliteNode *n = hpplite_get_node(db);
    hpplite_node_set_l1(n, l1); hpplite_node_start(n);
    sqlite3_exec(db, "CREATE TABLE t(x); INSERT INTO t VALUES('signed for')", NULL, NULL, NULL);
    if (hpplite_node_set_signer(n, &signer) != 0) FAIL("set signer");
    if (status_int(db, "keyed") != 1) FAIL("a signer makes the node keyed");
    if (hpplite_flush(db) != 0) FAIL("a signer without a batch key posts nothing sealed");
    if (scalar(db, "SELECT hpplite_batch_key('0x4242424242424242424242424242424242424242424242424242424242424242')") != 1) FAIL("hpplite_batch_key");
    if (status_int(db, "lease") != 1) FAIL("the lease after the batch key");
    if (hpplite_flush(db) != 1) FAIL("the flush with signer and batch key should post");
    {
        size_t len = 0;
        uint8_t *rawb = hpplite_l1_get_batch(l1, 1, &len);
        int ok = rawb && !memmem(rawb, len, "signed for", 10);
        free(rawb);
        if (!ok) FAIL("the batch should be posted, sealed with the batch key");
    }
    if (hpplite_node_set_key(n, raw) != -2) FAIL("a key after a signer should be refused");
    {
        sqlite3 *r = hpplite_open("file:" DIR "/sg_r.db?hpplite=on&datadir=" DIR);
        HppliteNode *rn = r ? hpplite_get_node(r) : NULL;
        sqlite3_int64 rows = -1;
        if (rn) { hpplite_node_set_batch_key(rn, batchKey); hpplite_node_set_l1(rn, l1); hpplite_node_sync_from_l1(rn, 1); rows = scalar(r, "SELECT count(*) FROM t"); rn->l1 = NULL; }
        if (r) sqlite3_close(r);
        if (rows != 1) { printf("(replica rows %lld) ", (long long)rows); FAIL("a replica with the batch key should read the batch"); }
    }
    printf("(the mock verifies no signature: %d signer calls) ", ts.calls);
    n->l1 = NULL;
    sqlite3_close(db);
    hpplite_l1_disconnect(l1);
    hpplite_crypto_free(ts.c);
    PASS();
}

/* #17 step 4: the owner (cold) names the sequencer (hot). The owner's own
** node loses the lease; a node with the sequencer key posts, sealing with
** the batch key the owner hands it; a replica with the owner's key reads. */
static void test_owner_names_sequencer(void) {
    TEST("hpplite_set_sequencer: the owner names a hot wallet; the owner's node posts no more, the sequencer's node posts with the batch key given apart, a replica with the owner key reads; a non-owner is refused (#17)");
    reset_keys();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteCrypto *c = hpplite_crypto_init();
    HppliteKeypair hot;
    unsigned char raw[32], batchKey[32];
    char hotHex[48], sql[200], batchHex[80];
    hpplite_hex_to_bytes(KEY_B, raw, 32);
    if (hpplite_crypto_keypair_from_privkey(c, &hot, raw) != 0) FAIL("keypair");
    hotHex[0] = '0'; hotHex[1] = 'x'; hpplite_bytes_to_hex(hot.address, 20, hotHex + 2);
    hpplite_hex_to_bytes(KEY_A, raw, 32);
    hpplite_cipher_derive_key(raw, batchKey);                             /* the owner's batch key, handed to the sequencer */
    batchHex[0] = '0'; batchHex[1] = 'x'; hpplite_bytes_to_hex(batchKey, 32, batchHex + 2);

    sqlite3 *owner = hpplite_open("file:" DIR "/own.db?hpplite=on&datadir=" DIR "&privkey=" KEY_A);
    if (!owner) FAIL("open owner");
    HppliteNode *on = hpplite_get_node(owner);
    hpplite_node_set_l1(on, l1); hpplite_node_start(on);
    sqlite3_exec(owner, "CREATE TABLE t(x); INSERT INTO t VALUES('by the owner')", NULL, NULL, NULL);
    if (hpplite_flush(owner) != 1) FAIL("the owner posts batch 1 (it is the sequencer until it says otherwise)");
    snprintf(sql, sizeof sql, "SELECT hpplite_set_sequencer('%s')", hotHex);
    if (exec_sql(owner, sql) != SQLITE_OK) { printf("(%s) ", sqlite3_errmsg(owner)); FAIL("the owner should be able to name the sequencer"); }
    sqlite3_exec(owner, "INSERT INTO t VALUES('after the rotation')", NULL, NULL, NULL);
    if (hpplite_flush(owner) != 0 || status_int(owner, "lease") != 0) FAIL("the owner's node should post no more: the lease is the hot wallet's");

    sqlite3 *seq = hpplite_open("file:" DIR "/seq.db?hpplite=on&datadir=" DIR "&privkey=" KEY_B);
    if (!seq) FAIL("open sequencer");
    HppliteNode *sn = hpplite_get_node(seq);
    hpplite_node_set_l1(sn, l1);
    snprintf(sql, sizeof sql, "SELECT hpplite_batch_key('%s')", batchHex);
    if (scalar(seq, sql) != 1) FAIL("batch key on the sequencer");
    hpplite_node_start(sn);
    if (hpplite_node_sync_from_l1(sn, 1) < 0 || scalar(seq, "SELECT count(*) FROM t") != 1) FAIL("the sequencer's node rebuilds batch 1 with the batch key");
    if (exec_sql(seq, "SELECT hpplite_set_sequencer('0x1111111111111111111111111111111111111111')") == SQLITE_OK) FAIL("a non-owner naming a sequencer should be refused");
    sqlite3_exec(seq, "INSERT INTO t VALUES('by the sequencer')", NULL, NULL, NULL);
    if (hpplite_flush(seq) != 2 || status_int(seq, "lease") != 1) { printf("(%s) ", status_text(seq, "last_error")); FAIL("the sequencer's node should take the lease and post batch 2"); }
    {
        sqlite3 *r = hpplite_open("file:" DIR "/own_r.db?hpplite=on&datadir=" DIR "&privkey=" KEY_A);
        HppliteNode *rn = r ? hpplite_get_node(r) : NULL;
        sqlite3_int64 rows = -1;
        if (rn) { hpplite_node_set_l1(rn, l1); hpplite_node_sync_from_l1(rn, 1); rows = scalar(r, "SELECT count(*) FROM t"); rn->l1 = NULL; }
        if (r) sqlite3_close(r);
        if (rows != 2) { printf("(replica rows %lld) ", (long long)rows); FAIL("a replica with the owner's key should read both batches"); }
    }
    sn->l1 = NULL; sqlite3_close(seq);
    on->l1 = NULL; sqlite3_close(owner);
    hpplite_l1_disconnect(l1);
    hpplite_crypto_free(c);
    PASS();
}

/* A file this SQLite cannot read - encrypted by SQLCipher or SEE and not yet
** keyed, or not a database at all - must fail the open and say so, not open
** and fail at the first flush (#17, step 5 deferred). */
static void test_unreadable_file_fails_open(void) {
    TEST("a file this SQLite cannot read fails the open with a message that says the file is not readable, not a generic one");
    reset_keys();
    unsigned char junk[8192];
    for (size_t i = 0; i < sizeof junk; i++) junk[i] = (unsigned char)(i * 131 + 7);
    FILE *f = fopen(DIR "/junk.db", "wb");
    if (!f) FAIL("write");
    fwrite(junk, 1, sizeof junk, f);
    fclose(f);
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2("file:" DIR "/junk.db?hpplite=on&datadir=" DIR "&privkey=" KEY_A, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    char msg[300];
    snprintf(msg, sizeof msg, "%s", db ? sqlite3_errmsg(db) : "");
    if (db) sqlite3_close(db);
    printf("(rc %d: '%s') ", rc, msg);
    if (rc == SQLITE_OK) FAIL("the open should fail: HPPLite cannot keep a log in a file it cannot read");
    if (!strstr(msg, "not a database") && !strstr(msg, "encrypted")) FAIL("the message should say the file is unreadable (encrypted, or not a database)");
    PASS();
}

int main(int argc, char **argv) {
    if (argc > 1) g_fixtures = argv[1];
    printf("=== HPPLite Key Provisioning Tests ===\n");
    test_suite_init("keys", NULL, DIR);
    test_uri_key();
    test_env_key_and_precedence();
    test_keyfile();
    test_keyfile_rejections();
    test_no_key_never_posts();
    test_encrypt_off_is_explicit();
    test_key_at_runtime();
    test_keystore();
    test_signer();
    test_owner_names_sequencer();
    test_unreadable_file_fails_open();
    remove_dir();
    return test_report();
}
