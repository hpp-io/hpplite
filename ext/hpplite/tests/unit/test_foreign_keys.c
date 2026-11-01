/*
** HPPLite foreign keys and recursive triggers (#52)
**
** PRAGMA foreign_keys and PRAGMA recursive_triggers are connection state:
** they change what a statement does to other rows (ON DELETE CASCADE,
** SET NULL, SET DEFAULT, ON UPDATE CASCADE; a trigger that fires itself)
** without appearing in the statement's text. Applications (Django, Rails,
** Prisma) turn foreign keys on at connect. Whatever the application does
** with these pragmas, and whenever it does it, a replica rebuilt from L1
** must reach the writer's state root.
**
** Every test has the same shape: a writer on the mock L1 runs SQL with
** the pragmas set the way an application would, flushes, and a fresh
** replica rebuilt from L1 must have the same rows and the same root.
*/

#include "test_lib.h"
#include "batch.h"
#include "cipher.h"
#include <unistd.h>
#include <sys/wait.h>

static const unsigned char PRIVKEY[32] = {
    0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x4f,0x50,
    0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x5b,0x5c,0x5d,0x5e,0x5f,0x60
};

#define DIR "/tmp/hpplite_test_fk"

static HppliteNode *open_node(const char *dbPath, HppliteL1 *l1) { return test_open_node(dbPath, l1, NULL); }

/* The check every test ends with: one flush, rebuild a replica from L1,
** compare every row and the state root. Destroys the writer. */
static const char *replica_matches(HppliteNode *w, HppliteL1 *l1, const char *replicaFile) {
    return test_replica_check(w, l1, tpath(replicaFile), NULL, 0, 1, 1);
}

#define SCHEMA_PARENT "CREATE TABLE parent(id INTEGER PRIMARY KEY, name TEXT)"

/* 1. ON DELETE CASCADE: deleting a parent deletes its children on the writer */
static void test_delete_cascade(void) {
    TEST("foreign_keys=ON, ON DELETE CASCADE: replica deletes the children too");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/cascade.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "PRAGMA foreign_keys = ON",
        SCHEMA_PARENT,
        "CREATE TABLE child(id INTEGER PRIMARY KEY, parent_id INTEGER REFERENCES parent(id) ON DELETE CASCADE)",
        "INSERT INTO parent VALUES(1, 'a'), (2, 'b')",
        "INSERT INTO child VALUES(10, 1), (11, 1), (12, 2)",
        "DELETE FROM parent WHERE id = 1",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT count(*) FROM child") != 1) FAIL("the writer did not cascade (test premise)");
    const char *why = replica_matches(w, l1, "cascade_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 2. ON DELETE SET NULL (Prisma's default for optional relations) */
static void test_delete_set_null(void) {
    TEST("foreign_keys=ON, ON DELETE SET NULL: replica nulls the references too");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/setnull.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "PRAGMA foreign_keys = ON",
        SCHEMA_PARENT,
        "CREATE TABLE child(id INTEGER PRIMARY KEY, parent_id INTEGER REFERENCES parent(id) ON DELETE SET NULL)",
        "INSERT INTO parent VALUES(1, 'a')",
        "INSERT INTO child VALUES(10, 1), (11, 1)",
        "DELETE FROM parent WHERE id = 1",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT count(*) FROM child WHERE parent_id IS NULL") != 2) FAIL("test premise");
    const char *why = replica_matches(w, l1, "setnull_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 3. ON DELETE SET DEFAULT */
static void test_delete_set_default(void) {
    TEST("foreign_keys=ON, ON DELETE SET DEFAULT: replica applies the default too");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/setdefault.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "PRAGMA foreign_keys = ON",
        SCHEMA_PARENT,
        "CREATE TABLE child(id INTEGER PRIMARY KEY, parent_id INTEGER DEFAULT 0 REFERENCES parent(id) ON DELETE SET DEFAULT)",
        "INSERT INTO parent VALUES(0, 'orphanage'), (1, 'a')",
        "INSERT INTO child VALUES(10, 1)",
        "DELETE FROM parent WHERE id = 1",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT parent_id FROM child WHERE id = 10") != 0) FAIL("test premise");
    const char *why = replica_matches(w, l1, "setdefault_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 4. ON UPDATE CASCADE: renumbering a parent renumbers the children */
static void test_update_cascade(void) {
    TEST("foreign_keys=ON, ON UPDATE CASCADE: replica renumbers the children too");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/updcascade.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "PRAGMA foreign_keys = ON",
        "CREATE TABLE parent(code TEXT PRIMARY KEY)",
        "CREATE TABLE child(id INTEGER PRIMARY KEY, code TEXT REFERENCES parent(code) ON UPDATE CASCADE)",
        "INSERT INTO parent VALUES('old')",
        "INSERT INTO child VALUES(1, 'old'), (2, 'old')",
        "UPDATE parent SET code = 'new'",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT count(*) FROM child WHERE code = 'new'") != 2) FAIL("test premise");
    const char *why = replica_matches(w, l1, "updcascade_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 5. Enforcement only (no actions): statements that passed on the writer
** replay to the same rows; one that failed was never logged */
static void test_enforcement_only(void) {
    TEST("foreign_keys=ON without actions: a rejected insert is not replayed");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/enforce.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "PRAGMA foreign_keys = ON",
        SCHEMA_PARENT,
        "CREATE TABLE child(id INTEGER PRIMARY KEY, parent_id INTEGER REFERENCES parent(id))",
        "INSERT INTO parent VALUES(1, 'a')",
        "INSERT INTO child VALUES(10, 1)",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (sqlite3_exec(w->db, "INSERT INTO child VALUES(11, 99)", NULL, NULL, NULL) == SQLITE_OK) FAIL("the writer accepted an orphan (test premise)");
    if (sqlite3_exec(w->db, "DELETE FROM parent WHERE id = 1", NULL, NULL, NULL) == SQLITE_OK) FAIL("the writer deleted a referenced parent (test premise)");
    const char *why = replica_matches(w, l1, "enforce_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 6. Deferred constraints (Django creates its foreign keys DEFERRABLE
** INITIALLY DEFERRED): a child inserted before its parent in one transaction */
static void test_deferred(void) {
    TEST("deferred foreign keys (Django style): child before parent in one transaction");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/deferred.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "PRAGMA foreign_keys = ON",
        SCHEMA_PARENT,
        "CREATE TABLE child(id INTEGER PRIMARY KEY, parent_id INTEGER REFERENCES parent(id) ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED)",
        "BEGIN; INSERT INTO child VALUES(10, 1); INSERT INTO parent VALUES(1, 'a'); COMMIT",
        "BEGIN; DELETE FROM parent WHERE id = 1; COMMIT",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT count(*) FROM child") != 0) FAIL("test premise");
    const char *why = replica_matches(w, l1, "deferred_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 7. recursive_triggers: a trigger that fires itself runs to completion on
** the writer; without the pragma it fires once */
static void test_recursive_triggers(void) {
    TEST("recursive_triggers=ON: replica runs the recursion too");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/rectrig.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "PRAGMA recursive_triggers = ON",
        "CREATE TABLE n(v INTEGER)",
        "CREATE TRIGGER countdown AFTER INSERT ON n WHEN new.v > 0 BEGIN INSERT INTO n VALUES(new.v - 1); END",
        "INSERT INTO n VALUES(5)",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT count(*) FROM n") != 6) FAIL("test premise");
    const char *why = replica_matches(w, l1, "rectrig_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 8. The application changes its mind: on, off, on again, in one batch */
static void test_mode_changes_midstream(void) {
    TEST("foreign_keys toggled between statements: each replays in the mode it ran in");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/toggle.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        SCHEMA_PARENT,
        "CREATE TABLE child(id INTEGER PRIMARY KEY, parent_id INTEGER REFERENCES parent(id) ON DELETE CASCADE)",
        "INSERT INTO parent VALUES(1, 'a'), (2, 'b'), (3, 'c')",
        "INSERT INTO child VALUES(10, 1), (20, 2), (30, 3)",
        "PRAGMA foreign_keys = ON",
        "DELETE FROM parent WHERE id = 1",          /* cascades */
        "PRAGMA foreign_keys = OFF",
        "DELETE FROM parent WHERE id = 2",          /* leaves child 20 orphaned */
        "PRAGMA foreign_keys = ON",
        "DELETE FROM parent WHERE id = 3",          /* cascades */
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT group_concat(id) FROM child") != 20) FAIL("test premise: only child 20 should be left");
    const char *why = replica_matches(w, l1, "toggle_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 9. The mode across batches and across a reopen: the pragma is per
** connection, so a reopened writer starts with foreign keys off */
static void test_mode_across_batches_and_reopen(void) {
    TEST("foreign_keys across batches and a writer reopen");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/reopen.db", l1);
    if (!w) FAIL("open");
    const char *a[] = {
        "PRAGMA foreign_keys = ON",
        SCHEMA_PARENT,
        "CREATE TABLE child(id INTEGER PRIMARY KEY, parent_id INTEGER REFERENCES parent(id) ON DELETE CASCADE)",
        "INSERT INTO parent VALUES(1, 'a'), (2, 'b')",
        "INSERT INTO child VALUES(10, 1), (20, 2)",
        NULL };
    if (run(w, a) != 0) FAIL("writer SQL 1");
    if (hpplite_node_flush_batch(w) != 1) FAIL("batch 1");
    if (sqlite3_exec(w->db, "DELETE FROM parent WHERE id = 1", NULL, NULL, NULL) != SQLITE_OK) FAIL("delete 1");
    if (hpplite_node_flush_batch(w) != 2) FAIL("batch 2");
    hpplite_node_destroy(w);

    w = open_node(DIR "/reopen.db", l1);        /* foreign keys off again */
    if (!w) FAIL("reopen");
    if (sqlite3_exec(w->db, "DELETE FROM parent WHERE id = 2", NULL, NULL, NULL) != SQLITE_OK) FAIL("delete 2");
    if (scalar(w->db, "SELECT count(*) FROM child") != 1) FAIL("test premise: child 20 survives with foreign keys off");
    const char *why = replica_matches(w, l1, "reopen_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 10. A crash before the flush: the mode a pending statement ran in must
** survive with the statement, not be read from whoever flushes later */
static void test_mode_survives_crash(void) {
    TEST("a pending cascade survives a crash and replays in the mode it ran in");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock_dir(DIR "/l1");
    if (!l1) FAIL("mock dir");
    pid_t pid = fork();
    if (pid == 0) {
        HppliteL1 *cl = hpplite_l1_create_mock_dir(DIR "/l1");
        HppliteNode *n = open_node(DIR "/crash.db", cl);
        if (!n) _exit(1);
        const char *sql[] = {
            "PRAGMA foreign_keys = ON",
            SCHEMA_PARENT,
            "CREATE TABLE child(id INTEGER PRIMARY KEY, parent_id INTEGER REFERENCES parent(id) ON DELETE CASCADE)",
            "INSERT INTO parent VALUES(1, 'a')",
            "INSERT INTO child VALUES(10, 1), (11, 1)",
            "DELETE FROM parent WHERE id = 1",
            NULL };
        if (run(n, sql) != 0) _exit(2);
        _exit(0);                                /* no flush, no close */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) FAIL("child did not run");

    HppliteNode *w = open_node(DIR "/crash.db", l1);   /* foreign keys off on this connection */
    if (!w) FAIL("reopen");
    if (hpplite_node_pending_count(w) == 0) FAIL("pending rows lost");
    if (scalar(w->db, "SELECT count(*) FROM child") != 0) FAIL("test premise: the cascade ran before the crash");
    const char *why = replica_matches(w, l1, "crash_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 11. Replay leaves the replica's own application connection alone */
static void test_replica_connection_untouched(void) {
    TEST("replaying a foreign_keys=ON history does not change the replica's own pragmas");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/own.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "PRAGMA foreign_keys = ON",
        "PRAGMA recursive_triggers = ON",
        "CREATE TABLE t(x)",
        "INSERT INTO t VALUES(1)",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    hpplite_node_flush_batch(w);
    hpplite_node_destroy(w);
    HppliteNode *r = open_node(DIR "/own_r.db", l1);
    if (!r) FAIL("replica");
    int fk = scalar(r->db, "PRAGMA foreign_keys");
    int rt = scalar(r->db, "PRAGMA recursive_triggers");
    hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    if (fk != 0 || rt != 0) FAIL("the replica's application connection inherited the writer's pragmas");
    PASS();
}

/* 12. The batch format carries the mode, only when set, and the block hash
** covers it - with mode-0 hashes unchanged (batches already on L1) */
static void test_format_and_hash(void) {
    TEST("batch JSON carries the mode only when set; the block hash covers it");
    HppliteBatch *b = hpplite_batch_new(4);
    hpplite_batch_add_stmt(b, "DELETE FROM parent", -1, 0);
    hpplite_batch_add_stmt(b, "INSERT INTO n VALUES(1)", -1, 1);
    unsigned char prev[32] = {0}, h0[32], h1[32];
    hpplite_block_hash(b, prev, h0);
    char *json0 = hpplite_batch_to_json(b);
    if (!json0 || strstr(json0, "\"mode\"")) { sqlite3_free(json0); hpplite_batch_free(b); FAIL("mode written for a mode-0 batch"); }
    b->aTxns[0].mode = HPPLITE_MODE_FOREIGN_KEYS;
    b->aTxns[1].mode = HPPLITE_MODE_FOREIGN_KEYS | HPPLITE_MODE_RECURSIVE_TRIGGERS;
    hpplite_block_hash(b, prev, h1);
    char *json1 = hpplite_batch_to_json(b);
    HppliteBatch *p = hpplite_batch_from_json(json1);
    int ok = p && p->nTxns == 2 && p->aTxns[0].mode == 1 && p->aTxns[1].mode == 3;
    unsigned char hp[32];
    if (p) hpplite_block_hash(p, prev, hp);
    int hashOk = p && memcmp(hp, h1, 32) == 0;
    /* a mode-0 batch parsed from JSON written before #52 hashes as before */
    HppliteBatch *old = hpplite_batch_from_json(json0);
    unsigned char ho[32];
    if (old) hpplite_block_hash(old, prev, ho);
    int oldOk = old && memcmp(ho, h0, 32) == 0;
    int differs = memcmp(h0, h1, 32) != 0;
    size_t est = hpplite_batch_json_overhead() + hpplite_batch_stmt_json_size("DELETE FROM parent", -1, 0)
               + hpplite_batch_stmt_json_size("INSERT INTO n VALUES(1)", -1, 1);
    int sizeOk = strlen(json1) <= est;
    sqlite3_free(json0); sqlite3_free(json1);
    hpplite_batch_free(b); hpplite_batch_free(p); hpplite_batch_free(old);
    if (!ok) FAIL("mode lost in the JSON round trip");
    if (!hashOk) FAIL("hash of the parsed batch differs");
    if (!oldOk) FAIL("a mode-0 batch hashes differently than before");
    if (!differs) FAIL("the block hash does not cover the mode");
    if (!sizeOk) FAIL("the size estimate does not cover the mode field");
    PASS();
}

/* 13. A mode bit this version does not know: stop, do not replay into a
** different state */
static void test_unknown_mode_refused(void) {
    TEST("a batch with an unknown mode bit is not replayed");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/unknown.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(x)", NULL);
    if (hpplite_node_flush_batch(w) != 1) FAIL("batch 1");
    HppliteBatch *b = hpplite_batch_new(2);
    hpplite_batch_add_stmt(b, "INSERT INTO t VALUES(1)", -1, 0);
    b->aTxns[0].mode = 128;                    /* 4 is a row record since #19, 8-64 connection pragmas since #28 */
    char *json = hpplite_batch_to_json(b);
    size_t len = 0;
    uint8_t *sealed = hpplite_envelope_seal(w->cipher, w->encKey, 2, HPPLITE_ENCODING_NONE,
                                            (const uint8_t *)json, strlen(json), &len);
    char *tx = sealed ? hpplite_l1_submit_batch(l1, w->instanceId, 2, sealed, len) : NULL;
    sqlite3_free(json); free(sealed); hpplite_batch_free(b);
    if (!tx) FAIL("post");
    sqlite3_free(tx);
    hpplite_node_destroy(w);
    HppliteNode *r = open_node(DIR "/unknown_r.db", l1);
    if (!r) FAIL("replica");
    int rows = scalar(r->db, "SELECT count(*) FROM t");
    uint64_t height = hpplite_node_get_height(r);
    hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    if (rows != 0) FAIL("the statement was replayed");
    if (height != 2) FAIL("height moved past a batch that was not applied");
    PASS();
}

/* 14. A file from before #52: its log has no mode column. Opening adds it;
** rows already there replay with the default state */
static void test_old_log_migrated(void) {
    TEST("a pending log from before #52 gains the mode column; its rows still post");
    reset_dir();
    sqlite3 *raw = NULL;
    if (sqlite3_open(DIR "/old.db", &raw) != SQLITE_OK) FAIL("raw open");
    sqlite3_exec(raw,
        "CREATE TABLE _hpplite_pending(id INTEGER PRIMARY KEY, txn INTEGER NOT NULL, sql TEXT NOT NULL);"
        "CREATE TABLE t(x);"
        "INSERT INTO _hpplite_pending(txn, sql) VALUES(0, 'CREATE TABLE t(x)'), (0, 'INSERT INTO t VALUES(1)');"
        "INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    sqlite3_close(raw);
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/old.db", l1);
    if (!w) FAIL("open");
    if (scalar(w->db, "SELECT count(*) FROM pragma_table_info('_hpplite_pending') WHERE name = 'mode'") != 1) FAIL("column not added");
    if (hpplite_node_pending_count(w) != 2) FAIL("old rows lost");
    if (sqlite3_exec(w->db, "PRAGMA foreign_keys = ON; INSERT INTO t VALUES(2)", NULL, NULL, NULL) != SQLITE_OK) FAIL("new write");
    const char *why = replica_matches(w, l1, "old_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 15. Crash between submit and finalize: the batch on L1 carries the mode,
** the pending rows carry it too, and reopening adopts rather than forks */
static void test_adopt_with_mode(void) {
    TEST("crash after submitting a foreign_keys=ON batch: reopen adopts it");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/adopt.db", l1);
    if (!n) FAIL("open");
    const char *sql[] = {
        "PRAGMA foreign_keys = ON",
        SCHEMA_PARENT,
        "CREATE TABLE child(id INTEGER PRIMARY KEY, parent_id INTEGER REFERENCES parent(id) ON DELETE CASCADE)",
        "INSERT INTO parent VALUES(1, 'a')",
        "INSERT INTO child VALUES(10, 1)",
        "DELETE FROM parent WHERE id = 1",
        NULL };
    if (run(n, sql) != 0) FAIL("writer SQL");
    sqlite3_int64 maxId = 0;
    HppliteBatch *b = hpplite_build_batch(n->ctx, &maxId);
    if (!b) FAIL("build");
    int sawMode = 0;
    for (int i = 0; i < b->nTxns; i++) if (b->aTxns[i].mode & HPPLITE_MODE_FOREIGN_KEYS) sawMode = 1;
    char *json = hpplite_batch_to_json(b);
    size_t len = 0;
    uint8_t *sealed = hpplite_envelope_seal(n->cipher, n->encKey, b->height, HPPLITE_ENCODING_NONE,
                                            (const uint8_t *)json, strlen(json), &len);
    char *tx = hpplite_l1_submit_batch(l1, n->instanceId, b->height, sealed, len);
    sqlite3_free(json); free(sealed); hpplite_batch_free(b);
    if (!sawMode) FAIL("the built batch does not carry the mode");
    if (!tx) FAIL("manual submit");
    sqlite3_free(tx);
    /* crash: no flush, no finalize */
    n->hasLease = 0;
    n->l1 = NULL;
    hpplite_node_stop_timer(n);
    hpplite_unregister_node(n->db);
    hpplite_shutdown(n->ctx); n->ctx = NULL;
    if (n->bkDb) { sqlite3_close(n->bkDb); n->bkDb = NULL; }
    sqlite3_close(n->db); n->db = NULL;
    n->ownsDb = 0;
    hpplite_node_destroy(n);

    n = open_node(DIR "/adopt.db", l1);
    if (!n) FAIL("reopen");
    int pending = hpplite_node_pending_count(n);
    uint64_t height = hpplite_node_get_height(n);
    if (pending != 0) { hpplite_node_destroy(n); hpplite_l1_disconnect(l1); FAIL("reopen did not adopt the batch (mode compared wrong?)"); }
    if (height != 2) { hpplite_node_destroy(n); hpplite_l1_disconnect(l1); FAIL("height should be 2"); }
    const char *why = replica_matches(n, l1, "adopt_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 16. The URI path, closed with sqlite3_close: HPPLite's cached statements
** (including the recursive_triggers read) are released exactly once.
** The anvil run found a double finalize here that the node API hid. */
static void test_close_after_recursive_triggers(void) {
    TEST("sqlite3_close after recursive_triggers was read: no double finalize, close succeeds");
    reset_dir();
    sqlite3 *db = hpplite_open("file:" DIR "/close.db?hpplite=on&encrypt=off&datadir=" DIR);
    if (!db) FAIL("open");
    if (sqlite3_exec(db, "PRAGMA recursive_triggers = ON; PRAGMA foreign_keys = ON; CREATE TABLE t(x); INSERT INTO t VALUES(1)",
                     NULL, NULL, NULL) != SQLITE_OK) { sqlite3_close(db); FAIL("writes"); }
    int rc = sqlite3_close(db);
    if (rc != SQLITE_OK) FAIL("close reported busy (a cached statement counted as the application's)");
    db = hpplite_open("file:" DIR "/close.db?hpplite=on&encrypt=off&datadir=" DIR);
    if (!db) FAIL("reopen");
    int n = scalar(db, "SELECT count(*) FROM t");
    sqlite3_close(db);
    if (n != 1) FAIL("row lost");
    PASS();
}

int main(void) {
    printf("=== HPPLite foreign keys and recursive triggers (#52) ===\n");
    test_suite_init("fk", PRIVKEY, DIR);
    test_delete_cascade();
    test_delete_set_null();
    test_delete_set_default();
    test_update_cascade();
    test_enforcement_only();
    test_deferred();
    test_recursive_triggers();
    test_mode_changes_midstream();
    test_mode_across_batches_and_reopen();
    test_mode_survives_crash();
    test_replica_connection_untouched();
    test_format_and_hash();
    test_unknown_mode_refused();
    test_old_log_migrated();
    test_adopt_with_mode();
    test_close_after_recursive_triggers();
    return test_report();
}
