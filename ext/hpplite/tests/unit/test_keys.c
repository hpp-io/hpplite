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
    if (hpplite_node_pending_count(n) != 2) FAIL("pending must be kept");
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
    if (!raw || !memmem(raw, len, "visible", 7)) FAIL("encrypt=off should post plaintext");
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

int main(void) {
    printf("=== HPPLite Key Provisioning Tests ===\n");
    test_suite_init("keys", NULL, DIR);
    test_uri_key();
    test_env_key_and_precedence();
    test_keyfile();
    test_keyfile_rejections();
    test_no_key_never_posts();
    test_encrypt_off_is_explicit();
    remove_dir();
    return test_report();
}
