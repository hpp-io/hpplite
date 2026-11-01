/*
** HPPLite batch serialization tests
*/

#include "test_lib.h"
#include "batch.h"

static void test_roundtrip(void) {
    HppliteBatch *b, *p;
    char *json;
    unsigned char post[32] = {0xa1, 0xb2, 0xc3, 0xd4};
    TEST("JSON round trip preserves every field");
    b = hpplite_batch_new(7);
    if (!b) FAIL("new");
    b->timestamp = 1700000000;
    hpplite_batch_set_post_state(b, post);
    hpplite_batch_add_stmt(b, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)", 0, 0);
    hpplite_batch_add_stmt(b, "INSERT INTO users (name) VALUES ('Alice')", 1, 1);
    hpplite_batch_add_stmt(b, "INSERT INTO users (name) VALUES ('Bob')", -1, 1);
    json = hpplite_batch_to_json(b);
    if (!json) FAIL("to_json");
    if (!strstr(json, "\"height\":7")) FAIL("height missing");
    if (!strstr(json, "\"txn\":1")) FAIL("txn group missing");
    if (!strstr(json, "\"state_root\":\"a1b2c3d4")) FAIL("state_root missing");
    if (strchr(json, '\n') || strstr(json, ": ")) FAIL("batch JSON should be compact");
    if (strstr(json, "\"partial\"")) FAIL("partial must be absent for a full batch");
    p = hpplite_batch_from_json(json);
    if (!p) FAIL("from_json");
    if (p->height != 7 || p->timestamp != 1700000000) FAIL("height/timestamp");
    if (!p->hasRoot || memcmp(p->postStateRoot, post, 32)) FAIL("root");
    if (p->nTxns != 3) FAIL("statement count");
    if (strcmp(p->aTxns[0].zSql, b->aTxns[0].zSql)) FAIL("sql 0");
    if (p->aTxns[1].nRowsAffected != 1 || p->aTxns[2].nRowsAffected != -1) FAIL("rows_affected");
    if (p->aTxns[0].txn != 0 || p->aTxns[1].txn != 1 || p->aTxns[2].txn != 1) FAIL("txn groups");
    if (p->partial) FAIL("partial should be 0");
    sqlite3_free(json);
    hpplite_batch_free(b);
    hpplite_batch_free(p);
    PASS();
}

static void test_escapes(void) {
    HppliteBatch *b, *p;
    char *json;
    const char *s1 = "INSERT INTO t VALUES ('it''s \"quoted\" \\ backslash')";
    const char *s2 = "INSERT INTO t VALUES ('line1\nline2\ttab\r')";
    const char *s3 = "INSERT INTO t VALUES ('\x01 control', 'unicode \xc3\xa9\xe2\x9c\x93')";
    TEST("quotes, backslashes, control and UTF-8 bytes survive the round trip");
    b = hpplite_batch_new(2);
    hpplite_batch_add_txn(b, s1, 1);
    hpplite_batch_add_txn(b, s2, 1);
    hpplite_batch_add_txn(b, s3, 1);
    json = hpplite_batch_to_json(b);
    if (!json) FAIL("to_json");
    if (strstr(json, "line1\nline2")) FAIL("raw newline not escaped");
    p = hpplite_batch_from_json(json);
    if (!p || p->nTxns != 3) FAIL("parse");
    if (strcmp(p->aTxns[0].zSql, s1)) FAIL("quotes/backslash");
    if (strcmp(p->aTxns[1].zSql, s2)) FAIL("newline/tab/cr");
    if (strcmp(p->aTxns[2].zSql, s3)) FAIL("control/utf-8");
    if (p->aTxns[0].txn != 0 || p->aTxns[2].txn != 2) FAIL("add_txn should give each statement its own group");
    sqlite3_free(json);
    hpplite_batch_free(b);
    hpplite_batch_free(p);
    PASS();
}

static void test_legacy_and_partial(void) {
    HppliteBatch *p, *b;
    char *json;
    const char *legacy =
        "{\"height\": 3, \"timestamp\": 1,"
        " \"pre_state_root\": \"0000000000000000000000000000000000000000000000000000000000000000\","
        " \"post_state_root\": \"a1b2c3d400000000000000000000000000000000000000000000000000000000\","
        " \"transactions\": [{\"sql\": \"A\"}, {\"sql\": \"B\", \"rows_affected\": 2}, {\"sql\": \"C\"}]}";
    TEST("batches without txn/partial fields parse as one group per statement");
    p = hpplite_batch_from_json(legacy);
    if (!p || p->nTxns != 3) FAIL("legacy parse");
    if (p->aTxns[0].txn != 0 || p->aTxns[1].txn != 1 || p->aTxns[2].txn != 2) FAIL("legacy groups");
    if (p->aTxns[1].nRowsAffected != 2) FAIL("legacy rows_affected");
    if (p->partial) FAIL("legacy partial");
    if (!p->hasRoot) FAIL("legacy post_state_root should count as a root");
    hpplite_batch_free(p);

    /* a batch without a root serializes without the key and parses back as such */
    b = hpplite_batch_new(5);
    hpplite_batch_add_txn(b, "Y", -1);
    json = hpplite_batch_to_json(b);
    if (!json || strstr(json, "state_root")) FAIL("rootless batch must not emit state_root");
    p = hpplite_batch_from_json(json);
    if (!p || p->hasRoot) FAIL("rootless batch parsed with a root");
    sqlite3_free(json);
    hpplite_batch_free(b);
    hpplite_batch_free(p);

    b = hpplite_batch_new(4);
    b->partial = 1;
    hpplite_batch_add_txn(b, "X", -1);
    json = hpplite_batch_to_json(b);
    if (!json || !strstr(json, "\"partial\":1")) FAIL("partial not serialized");
    p = hpplite_batch_from_json(json);
    if (!p || !p->partial) FAIL("partial not parsed");
    sqlite3_free(json);
    hpplite_batch_free(b);
    hpplite_batch_free(p);
    PASS();
}

static void test_empty_and_malformed(void) {
    HppliteBatch *b, *p;
    char *json;
    TEST("empty batch round-trips; malformed JSON is rejected");
    b = hpplite_batch_new(1);
    json = hpplite_batch_to_json(b);
    p = hpplite_batch_from_json(json);
    if (!p || p->nTxns != 0 || p->height != 1) FAIL("empty batch");
    sqlite3_free(json);
    hpplite_batch_free(b);
    hpplite_batch_free(p);
    if (hpplite_batch_from_json("not json") != NULL) FAIL("garbage accepted");
    if (hpplite_batch_from_json("") != NULL) FAIL("empty string accepted");
    if (hpplite_batch_from_json(NULL) != NULL) FAIL("NULL accepted");
    PASS();
}

static void test_hex(void) {
    unsigned char in[4] = {0xde, 0xad, 0xbe, 0xef}, out[4];
    char hex[9];
    TEST("hex helpers");
    hpplite_to_hex(in, 4, hex);
    if (strcmp(hex, "deadbeef")) FAIL("to_hex");
    if (hpplite_from_hex("DEADBEEF", out, 4) != 0 || memcmp(in, out, 4)) FAIL("from_hex upper");
    if (hpplite_from_hex("zz", out, 1) != -1) FAIL("from_hex should reject non-hex");
    PASS();
}

int main(void) {
    printf("=== HPPLite Batch Tests ===\n");
    test_roundtrip();
    test_escapes();
    test_legacy_and_partial();
    test_empty_and_malformed();
    test_hex();
    return test_report();
}
