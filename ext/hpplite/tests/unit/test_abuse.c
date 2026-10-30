/*
** HPPLite abuse tests
**
** Inputs chosen to break the guards rather than to exercise them: every
** spelling of a refused statement, bytes on L1 that no HPPLite ever posted,
** statements sized to the limit, and random mutations of valid batches.
** The invariants are small and the same everywhere:
**   - a refused statement leaves no row behind and no pending log entry
**   - a batch that is not ours never changes the local database
**   - nothing we read from L1 can crash the process
**   - a transaction is never split across batches
**   - what capture lets through fits in one batch as posted
*/

#include "node.h"
#include "cipher.h"
#include "batch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { printf("  Testing: %s... ", name); tests_run++; fflush(stdout); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)
#define FAILF(...) do { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); return; } while(0)

static const unsigned char PRIVKEY[32] = {
    0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,0x30,
    0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x3b,0x3c,0x3d,0x3e,0x3f,0x40
};

#define DIR "/tmp/hpplite_test_abuse"

static void reset_dir(void) {
    (void)!system("rm -rf " DIR " && mkdir -p " DIR);
}

static int count_rows(sqlite3 *db, const char *table) {
    char sql[128];
    sqlite3_stmt *stmt;
    int n = -1;
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM \"%s\"", table);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

static HppliteNode *open_node_ex(const char *dbPath, HppliteL1 *l1, size_t maxBatchBytes, int noEncrypt) {
    HppliteNodeConfig config;
    HppliteNode *node;
    memset(&config, 0, sizeof(config));
    config.nodeId = "abuse";
    memcpy(config.privkey, PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = DIR;
    config.dbPath = (char *)dbPath;
    config.maxBatchBytes = maxBatchBytes;
    config.noEncrypt = noEncrypt;
    config.checkpointInterval = 1000;
    node = hpplite_node_create(&config);
    if (node && l1) {
        /* the DA limit is the contract's; the mock stands in for it */
        if (maxBatchBytes) hpplite_l1_mock_set_batch_size_limit(l1, maxBatchBytes);
        hpplite_node_set_l1(node, l1);
        hpplite_node_sync_from_l1(node, 1);
        hpplite_node_start(node);
    }
    return node;
}
static HppliteNode *open_node(const char *dbPath, HppliteL1 *l1) { return open_node_ex(dbPath, l1, 0, 0); }

/* Post arbitrary bytes at the next height under the node's own instance id,
** as a sequencer holding the lease (or an attacker who stole it) could. */
static int post_raw(HppliteNode *n, HppliteL1 *l1, uint64_t height, const uint8_t *data, size_t len) {
    char *tx = hpplite_l1_submit_batch(l1, n->instanceId, height, data, len);
    if (!tx) return -1;
    sqlite3_free(tx);
    return 0;
}

/* A valid batch at the given height, sealed with the node's key */
static uint8_t *sealed_batch(HppliteNode *n, uint64_t height, uint64_t claimedHeight, const char *sql, size_t *pLen) {
    HppliteBatch *b = hpplite_batch_new(claimedHeight);
    char *json;
    uint8_t *sealed;
    hpplite_batch_add_stmt(b, sql, -1, 0);
    json = hpplite_batch_to_json(b);
    sealed = hpplite_envelope_seal(n->cipher, n->encKey, height, HPPLITE_ENCODING_NONE,
                                   (const uint8_t *)json, strlen(json), pLen);
    sqlite3_free(json);
    hpplite_batch_free(b);
    return sealed;
}

static uint32_t rng_state = 0x9e3779b9u;
static uint32_t rng(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

/* ------------------------------------------------------------------------
** 1. Guard spelling corpus: every way of writing a refused statement
** ---------------------------------------------------------------------- */
static const char *aRefused[] = {
    /* our tables, through the parser (quoting, case, comments, schema prefix) */
    "INSERT INTO _hpplite_pending(txn, sql) VALUES(0, 'x')",
    "INSERT INTO \"_hpplite_pending\"(txn, sql) VALUES(0, 'x')",
    "INSERT INTO [_hpplite_pending](txn, sql) VALUES(0, 'x')",
    "INSERT INTO `_hpplite_pending`(txn, sql) VALUES(0, 'x')",
    "insert into _HPPLITE_PENDING(txn, sql) values(0, 'x')",
    "/* hi */ INSERT /* there */ INTO\n_hpplite_pending(txn, sql) VALUES(0, 'x')",
    "INSERT INTO main._hpplite_pending(txn, sql) VALUES(0, 'x')",
    "DELETE FROM _hpplite_pending",
    "DELETE FROM main.\"_hpplite_pending\" WHERE 1",
    "UPDATE _hpplite_meta SET value = 0",
    "UPDATE \"_HPPLITE_META\" SET value = 0 WHERE key = 'block_height'",
    "DROP TABLE _hpplite_pending",
    "DROP TABLE IF EXISTS [_hpplite_meta]",
    "ALTER TABLE _hpplite_pending RENAME TO t2",
    "ALTER TABLE _hpplite_pending ADD COLUMN z",
    "ALTER TABLE t RENAME TO _hpplite_t",
    "CREATE TABLE _hpplite_evil(x)",
    "CREATE TEMP TABLE _hpplite_evil(x)",
    "CREATE VIEW _hpplite_v AS SELECT 1",
    "CREATE INDEX ix ON _hpplite_pending(txn)",
    "CREATE TRIGGER tr AFTER INSERT ON t BEGIN DELETE FROM _hpplite_pending; END",
    "CREATE TRIGGER tr AFTER INSERT ON _hpplite_pending BEGIN INSERT INTO t VALUES(99); END",
    "CREATE TEMP TRIGGER tr AFTER INSERT ON _hpplite_pending BEGIN INSERT INTO t VALUES(99); END",
    "INSERT INTO t SELECT 1 FROM _hpplite_pending",
    "INSERT INTO t VALUES((SELECT count(*) FROM _hpplite_pending))",
    "INSERT INTO t SELECT sql FROM _hpplite_pending",
    "INSERT INTO t SELECT value FROM _hpplite_meta",
    "UPDATE t SET x = (SELECT value FROM _hpplite_meta WHERE key = 'block_height')",
    /* writes to another file are not captured */
    "ATTACH ':memory:' AS other",
    "ATTACH DATABASE '" DIR "/other.db' AS other",
    /* values replay cannot reproduce */
    "INSERT INTO t VALUES(random())",
    "INSERT INTO t VALUES(RANDOM())",
    "INSERT INTO t VALUES(random ( ))",
    "INSERT INTO t VALUES(\"random\"())",
    "INSERT INTO t VALUES(randomblob(4))",
    "INSERT INTO t VALUES(abs(random()))",
    "INSERT INTO t SELECT random()",
    "UPDATE t SET x = random()",
    "UPDATE t SET x = x WHERE random() > 0",
    "DELETE FROM t WHERE x = random()",
    "INSERT INTO t VALUES(last_insert_rowid())",
    "INSERT INTO t VALUES(changes())",
    "INSERT INTO t VALUES(total_changes())",
    "INSERT INTO t VALUES(sqlite_version())",
    "INSERT INTO t VALUES(sqlite_source_id())",
    "INSERT INTO t VALUES(sqlite_compileoption_used('THREADSAFE'))",
    "INSERT INTO t VALUES(datetime('now'))",
    "INSERT INTO t VALUES(DATETIME ( 'now' ))",
    "INSERT INTO t VALUES(datetime('NOW'))",
    "INSERT INTO t VALUES(datetime('now', 'localtime'))",
    "INSERT INTO t VALUES(datetime ( 'now' , 'localtime' ))",
    "INSERT INTO t VALUES(strftime('%s', 'now', 'utc'))",
    "INSERT INTO t VALUES(datetime(\n'now'))",
    "INSERT INTO t VALUES(timediff('now', '2024-01-01'))",
    "INSERT INTO t VALUES(upper(datetime('now')))",
    "INSERT INTO t VALUES(date('now'))",
    "INSERT INTO t VALUES(time('now'))",
    "INSERT INTO t VALUES(julianday('now'))",
    "INSERT INTO t VALUES(unixepoch('now'))",
    "INSERT INTO t VALUES(datetime())",
    "INSERT INTO t VALUES(date())",
    "INSERT INTO t VALUES(date( ))",
    "INSERT INTO t VALUES(time())",
    "INSERT INTO t VALUES(julianday())",
    "INSERT INTO t VALUES(unixepoch())",
    "INSERT INTO t VALUES(strftime('%s'))",
    "INSERT INTO t VALUES(strftime('%Y-%m-%d'))",
    "INSERT INTO t VALUES(strftime('%s', 'now'))",
    "INSERT INTO t VALUES(CURRENT_TIMESTAMP)",
    "INSERT INTO t VALUES(current_timestamp)",
    "INSERT INTO t VALUES(CURRENT_TIME)",
    "INSERT INTO t VALUES(CURRENT_DATE)",
    "INSERT INTO t VALUES(1 + (SELECT CURRENT_DATE))",
    "UPDATE t SET x = CURRENT_TIMESTAMP",
    "INSERT INTO t VALUES('x' || datetime('now'))",
    "INSERT INTO t VALUES(coalesce(NULL, datetime('now')))",
    "INSERT INTO t VALUES((SELECT datetime('now')))",
    "INSERT INTO t VALUES(CASE WHEN 1 THEN random() ELSE 0 END)",
    "INSERT INTO t VALUES(1) RETURNING random()",
    "CREATE TABLE d(x DEFAULT CURRENT_TIMESTAMP)",
    "CREATE TABLE d(x DEFAULT (random()))",
    "CREATE TABLE d(x, y AS (random()))",
    NULL
};

/* Allowed: function names in data, time functions with a fixed argument,
** reads of our tables, pragmas that do not change replay. */
static const char *aAllowed[] = {
    "INSERT INTO t VALUES('random()')",
    "INSERT INTO t VALUES('now')",
    "INSERT INTO t VALUES('datetime(''now'')')",
    "INSERT INTO t VALUES('CURRENT_TIMESTAMP')",
    "INSERT INTO t VALUES('_hpplite_pending')",
    "INSERT INTO t VALUES('DELETE FROM _hpplite_pending')",
    "INSERT INTO t VALUES('it''s now or never')",
    "INSERT INTO t VALUES(upper('now'))",
    "INSERT INTO t SELECT 1 WHERE 'x' IN ('now', 'later')",
    "INSERT INTO t VALUES(length(('now')))",
    "INSERT INTO t VALUES(datetime('2024-01-01', '+1 day'))",
    "INSERT INTO t VALUES(datetime('2024-01-01 00:00:00'))",
    "INSERT INTO t VALUES(date('2024-01-01', '+1 day'))",
    "INSERT INTO t VALUES(strftime('%Y', '2024-01-01'))",
    "INSERT INTO t VALUES(unixepoch('2024-01-01'))",
    "INSERT INTO t VALUES(julianday('2024-01-01'))",
    "INSERT INTO t VALUES(abs(-1))",
    "INSERT INTO t VALUES(length('now'))",
    "CREATE TABLE nowadays(x)",
    "INSERT INTO nowadays VALUES(1)",
    "CREATE TABLE random_things(date_col, time_col)",
    "INSERT INTO random_things VALUES(1, 2)",
    "CREATE TABLE \"date\"(x)",
    "INSERT INTO \"date\" VALUES(1)",
    "UPDATE t SET x = x WHERE x = 'now'",
    /* foreign keys and recursive triggers replay in the writer's mode (#52) */
    "PRAGMA foreign_keys",
    "PRAGMA foreign_keys = ON",
    "PRAGMA foreign_keys=1",
    "PRAGMA foreign_keys(1)",
    "pragma FOREIGN_KEYS = true",
    "PRAGMA main.foreign_keys = yes",
    "PRAGMA foreign_keys = OFF",
    "PRAGMA recursive_triggers = ON",
    "PRAGMA recursive_triggers(1)",
    "PRAGMA recursive_triggers = 0",
    "PRAGMA journal_mode",
    "PRAGMA user_version = 3",
    /* ALTER runs SQLite's internal functions (sqlite_rename_table ...),
    ** which are not in pragma_function_list: still SQLite's, not the app's */
    "CREATE TABLE alt(a, b, c)",
    "ALTER TABLE alt RENAME COLUMN b TO bb",
    "ALTER TABLE alt RENAME TO alt2",
    "ALTER TABLE alt2 DROP COLUMN c",
    "ALTER TABLE alt2 ADD COLUMN d DEFAULT 4",
    /* reads never reach the log: nothing to replay, nothing to refuse (#54) */
    "SELECT random()",
    "SELECT randomblob(4)",
    "SELECT x FROM t ORDER BY random()",
    "SELECT last_insert_rowid()",
    "SELECT changes(), total_changes()",
    "SELECT sqlite_version()",
    "WITH r(v) AS (SELECT random()) SELECT v FROM r",
    "SELECT * FROM t WHERE x > abs(random()) % 2",
    /* a cascade with foreign keys on is replayed in that mode (#52) */
    "PRAGMA foreign_keys = ON",
    "CREATE TABLE ap(id INTEGER PRIMARY KEY)",
    "CREATE TABLE ac(id INTEGER PRIMARY KEY, p INTEGER REFERENCES ap(id) ON DELETE CASCADE)",
    "INSERT INTO ap VALUES(1)",
    "INSERT INTO ac VALUES(10, 1)",
    "DELETE FROM ap WHERE id = 1",
    "PRAGMA foreign_keys = OFF",
    NULL
};

static void test_guard_spellings(void) {
    TEST("every spelling of a refused statement is refused; look-alikes pass");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/guards.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(0)", NULL);
    int pendingBefore = hpplite_node_pending_count(n);
    int rowsBefore = count_rows(n->db, "t");
    uint64_t heightBefore = hpplite_node_get_height(n);

    for (int i = 0; aRefused[i]; i++) {
        char *err = NULL;
        int rc = sqlite3_exec(n->db, aRefused[i], NULL, NULL, &err);
        if (rc == SQLITE_OK) FAILF("accepted: %s", aRefused[i]);
        sqlite3_free(err);
        if (hpplite_node_pending_count(n) != pendingBefore) FAILF("pending log changed by: %s", aRefused[i]);
        if (count_rows(n->db, "t") != rowsBefore) FAILF("rows changed by: %s", aRefused[i]);
        if (count_rows(n->db, "_hpplite_pending") < 0) FAILF("log table gone after: %s", aRefused[i]);
        if (hpplite_node_get_height(n) != heightBefore) FAILF("height changed by: %s", aRefused[i]);
        if (sqlite3_get_autocommit(n->db) == 0) FAILF("left a transaction open: %s", aRefused[i]);
    }
    /* none of the DDL took effect either */
    if (count_rows(n->db, "_hpplite_evil") != -1) FAIL("_hpplite_evil was created");
    if (count_rows(n->db, "d") != -1) FAIL("table with CURRENT_TIMESTAMP default was created");
    if (sqlite3_db_filename(n->db, "other") != NULL) FAIL("ATTACH took effect");

    for (int i = 0; aAllowed[i]; i++) {
        char *err = NULL;
        int rc = sqlite3_exec(n->db, aAllowed[i], NULL, NULL, &err);
        if (rc != SQLITE_OK) FAILF("refused: %s (%s)", aAllowed[i], err ? err : "?");
        sqlite3_free(err);
    }
    /* what was allowed replays to the same root on a fresh replica */
    if (hpplite_node_flush_batch(n) <= 0) FAIL("flush");
    unsigned char rootA[32], rootB[32];
    hpplite_node_get_state_root(n, rootA);
    HppliteNode *r = open_node(DIR "/guards_replica.db", l1);
    if (!r) FAIL("open replica");
    if (r->state == HPPLITE_STATE_ERROR) FAIL("replica sync failed");
    hpplite_node_get_state_root(r, rootB);
    if (memcmp(rootA, rootB, 32) != 0) FAIL("allowed statements did not replay to the same root");
    if (count_rows(r->db, "t") != count_rows(n->db, "t")) FAIL("replica row count differs");
    hpplite_node_destroy(r);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* An application-defined function (Django registers ~40 on every
** connection) is not on the replica: a write that calls it, directly or
** through a view or trigger, cannot replay and must be refused at the
** write; so must a generated column or index that would call it. Reads that call it are fine (#53). */
static void appFn(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    sqlite3_result_int64(ctx, sqlite3_value_int64(argv[0]) * 7);
}
static const char *aAppRefused[] = {
    "INSERT INTO t VALUES(app_fn(1))",
    "INSERT INTO t VALUES(APP_FN(1))",
    "INSERT INTO t SELECT app_fn(x) FROM t",
    "UPDATE t SET x = app_fn(x)",
    "UPDATE t SET x = x + 1 WHERE app_fn(x) = 7",
    "DELETE FROM t WHERE app_fn(x) = 14",
    "INSERT INTO t VALUES(1) RETURNING app_fn(x)",
    "INSERT INTO t SELECT y FROM v_app",                     /* through a view */
    "INSERT INTO trig_src VALUES(1)",                        /* through a trigger body */
    /* a generated column or expression index calls it on every later write,
    ** where the authorizer does not look: so the CREATE itself is refused */
    "CREATE TABLE d(x, y AS (app_fn(x)))",
    "CREATE INDEX ix_app ON t(app_fn(x))",
    NULL
};
static void test_app_defined_functions(void) {
    TEST("writes that call an application-defined function are refused; reads are not");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/appfn.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1)", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(2)", NULL);
    /* the objects that hide a call; created before the function exists, as a
    ** replica would see them, so their creation itself is a legal write */
    hpplite_node_exec(n, "CREATE VIEW v_app AS SELECT app_fn(x) AS y FROM t", NULL);
    hpplite_node_exec(n, "CREATE TABLE trig_src(a)", NULL);
    hpplite_node_exec(n, "CREATE TRIGGER tr_app AFTER INSERT ON trig_src BEGIN INSERT INTO t VALUES(app_fn(new.a)); END", NULL);
    if (sqlite3_create_function(n->db, "app_fn", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, appFn, NULL, NULL) != SQLITE_OK)
        FAIL("register app_fn");
    int pendingBefore = hpplite_node_pending_count(n);
    int rowsBefore = count_rows(n->db, "t");
    for (int i = 0; aAppRefused[i]; i++) {
        char *err = NULL;
        int rc = sqlite3_exec(n->db, aAppRefused[i], NULL, NULL, &err);
        if (rc == SQLITE_OK) FAILF("accepted: %s", aAppRefused[i]);
        sqlite3_free(err);
        if (hpplite_node_pending_count(n) != pendingBefore) FAILF("pending log changed by: %s", aAppRefused[i]);
        if (count_rows(n->db, "t") != rowsBefore) FAILF("rows changed by: %s", aAppRefused[i]);
        if (sqlite3_get_autocommit(n->db) == 0) FAILF("left a transaction open: %s", aAppRefused[i]);
    }
    /* reads may call it */
    static const char *aRead[] = { "SELECT app_fn(x) FROM t", "SELECT * FROM v_app",
                                   "SELECT x FROM t WHERE app_fn(x) = 7", NULL };
    for (int i = 0; aRead[i]; i++) {
        char *err = NULL;
        if (sqlite3_exec(n->db, aRead[i], NULL, NULL, &err) != SQLITE_OK) FAILF("read refused: %s (%s)", aRead[i], err ? err : "?");
        sqlite3_free(err);
    }
    /* a prepared write is checked on every run, not only the first: the
    ** same text is fine until a trigger that calls the function appears */
    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(n->db, "INSERT INTO t VALUES(?1)", -1, &ins, NULL) != SQLITE_OK) FAIL("prepare");
    sqlite3_bind_int(ins, 1, 5);
    if (sqlite3_step(ins) != SQLITE_DONE) FAIL("plain prepared insert refused");
    sqlite3_reset(ins);
    hpplite_node_exec(n, "CREATE TRIGGER tr_t AFTER INSERT ON t WHEN new.x = 6 BEGIN INSERT INTO trig_src VALUES(0); END", NULL);
    sqlite3_finalize(ins);
    ins = NULL;
    if (sqlite3_prepare_v2(n->db, "INSERT INTO t VALUES(?1)", -1, &ins, NULL) != SQLITE_OK) FAIL("prepare 2");
    sqlite3_bind_int(ins, 1, 6);
    if (sqlite3_step(ins) == SQLITE_DONE) FAIL("insert that fires a trigger calling app_fn was accepted");
    sqlite3_finalize(ins);
    hpplite_node_exec(n, "DROP TRIGGER tr_t", NULL);
    /* everything accepted replays to the writer's root */
    if (hpplite_node_flush_batch(n) <= 0) FAIL("flush");
    unsigned char rootA[32], rootB[32];
    hpplite_node_get_state_root(n, rootA);
    HppliteNode *r = open_node(DIR "/appfn_replica.db", l1);
    if (!r) FAIL("open replica");
    if (r->state == HPPLITE_STATE_ERROR) FAIL("replica sync failed");
    if (r->ctx->nReplaySkipped != 0) FAIL("replica skipped a statement");
    hpplite_node_get_state_root(r, rootB);
    int rowsR = count_rows(r->db, "t"), rowsW = count_rows(n->db, "t");
    hpplite_node_destroy(r);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    if (memcmp(rootA, rootB, 32) != 0) FAIL("replica root differs");
    if (rowsR != rowsW) FAIL("replica row count differs");
    PASS();
}

/* ------------------------------------------------------------------------
** 2. Adversarial bytes on L1
** ---------------------------------------------------------------------- */

/* Sequencer A posts a real batch 1, then <bytes> land at height 2 (whoever
** posted them). A fresh replica syncs. Returns the replica; the caller
** checks state, height and rows. */
static HppliteNode *sync_after(HppliteL1 **pL1, const uint8_t *bytes, size_t len, const char *tag) {
    HppliteL1 *l1 = hpplite_l1_create_mock();
    char path[256];
    snprintf(path, sizeof(path), DIR "/adv_%s.db", tag);
    HppliteNode *a = open_node(path, l1);
    if (!a) return NULL;
    hpplite_node_exec(a, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(a, "INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(a) != 1) { hpplite_node_destroy(a); return NULL; }
    if (post_raw(a, l1, 2, bytes, len) != 0) { hpplite_node_destroy(a); return NULL; }
    hpplite_node_destroy(a);
    snprintf(path, sizeof(path), DIR "/adv_%s_replica.db", tag);
    *pL1 = l1;
    return open_node(path, l1);
}

static void test_wrong_height_batch(void) {
    TEST("a sealed batch claiming another height is corrupt, not applied");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/wh.db", l1);
    if (!a) FAIL("open");
    hpplite_node_exec(a, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(a, "INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(a) != 1) FAIL("batch 1");
    size_t len = 0;
    /* sealed for slot 2 (the AEAD passes) but the JSON says height 9 */
    uint8_t *bytes = sealed_batch(a, 2, 9, "DELETE FROM t", &len);
    if (!bytes || post_raw(a, l1, 2, bytes, len) != 0) FAIL("post");
    free(bytes);
    hpplite_node_destroy(a);

    HppliteNode *r = open_node(DIR "/wh_replica.db", l1);
    if (!r) FAIL("open replica");
    if (count_rows(r->db, "t") != 1) FAIL("the DELETE was applied");
    if (hpplite_node_get_height(r) != 2) FAIL("height moved past the corrupt batch");
    if (r->state != HPPLITE_STATE_ERROR) FAIL("replica should stop in ERROR");
    if (r->ctx->nForeignBatches != 0) FAIL("corrupt is not foreign: it must not be skipped");
    hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    PASS();
}

static void test_plaintext_on_encrypting_rollup(void) {
    TEST("plaintext SQL at height 2 of an encrypting rollup is skipped as foreign");
    reset_dir();
    HppliteL1 *l1 = NULL;
    const char *evil = "{\"height\":2,\"timestamp\":0,\"txns\":[{\"sql\":\"DELETE FROM t\",\"txn\":0}]}";
    HppliteNode *r = sync_after(&l1, (const uint8_t *)evil, strlen(evil), "plain");
    if (!r) FAIL("setup");
    if (count_rows(r->db, "t") != 1) FAIL("plaintext SQL was applied");
    if (r->ctx->nForeignBatches != 1) FAIL("should be skipped as a foreign batch");
    if (hpplite_node_get_height(r) != 3) FAIL("height should step over the foreign batch");
    if (r->state == HPPLITE_STATE_ERROR) FAIL("a skipped batch is not an error");
    hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    PASS();
}

static void test_plaintext_at_height_1(void) {
    TEST("plaintext batch 1 on an encrypting rollup: refuse, never replay");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/p1.db", l1);
    if (!a) FAIL("open");
    const char *evil = "{\"height\":1,\"timestamp\":0,\"txns\":[{\"sql\":\"CREATE TABLE t(x)\",\"txn\":0},{\"sql\":\"INSERT INTO t VALUES(1)\",\"txn\":0}]}";
    if (post_raw(a, l1, 1, (const uint8_t *)evil, strlen(evil)) != 0) FAIL("post");
    hpplite_node_destroy(a);
    HppliteNode *r = open_node(DIR "/p1_replica.db", l1);
    if (!r) FAIL("open replica");
    if (count_rows(r->db, "t") != -1) FAIL("plaintext batch 1 was replayed");
    if (r->state != HPPLITE_STATE_ERROR) FAIL("replica should be in ERROR");
    if (hpplite_node_get_height(r) != 1) FAIL("height must not move");
    /* and it must not post over it either */
    hpplite_node_exec(r, "CREATE TABLE u(x)", NULL);
    if (hpplite_node_flush_batch(r) > 0) FAIL("an ERROR node must not post");
    hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    PASS();
}

static void test_garbage_on_l1(void) {
    TEST("garbage, truncated and oversized envelopes at height 2: never applied, never crash");
    reset_dir();
    static const char *aText[] = {
        "", "{", "}", "[]", "null", "{\"height\":2}", "{\"height\":2,\"txns\":null}",
        "{\"height\":2,\"txns\":[{\"sql\":null}]}", "{\"height\":2,\"txns\":[{\"sql\":\"DELETE FROM t\",\"txn\":-1}]}",
        "{\"height\":\"2\",\"txns\":[]}", "{\"height\":18446744073709551616,\"txns\":[]}",
        "{\"height\":2,\"txns\":[{\"sql\":\"\\u0000DELETE FROM t\"}]}",
        "{\"height\":2,\"txns\":[{\"sql\":\"DELETE FROM t\"}],\"height\":3}",
        "HPE2", "HPE2\x00\x00\x00\x00", "HPE1", "HPE9garbage", "\xff\xfe\xfd", "\x00\x00\x00\x00\x00\x00\x00\x00",
        NULL
    };
    char tag[32];
    for (int i = 0; aText[i]; i++) {
        HppliteL1 *l1 = NULL;
        size_t len = (i == 0) ? 0 : strlen(aText[i]);
        if (i == 14) len = 8;          /* the one with embedded NULs */
        if (i == 18) len = 8;
        snprintf(tag, sizeof(tag), "g%d", i);
        HppliteNode *r = sync_after(&l1, (const uint8_t *)aText[i], len, tag);
        if (!r) { if (len == 0) continue; FAILF("setup %d", i); }   /* the mock refuses empty data: fine */
        if (count_rows(r->db, "t") != 1) FAILF("case %d changed the table", i);
        if (hpplite_node_get_height(r) < 2) FAILF("case %d lost batch 1", i);
        hpplite_node_destroy(r);
        hpplite_l1_disconnect(l1);
    }
    /* a valid envelope, truncated at every length, and extended */
    HppliteL1 *l0 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/trunc_src.db", l0);
    if (!a) FAIL("open");
    size_t fullLen = 0;
    uint8_t *full = sealed_batch(a, 2, 2, "DELETE FROM t", &fullLen);
    if (!full) FAIL("seal");
    hpplite_node_destroy(a);
    hpplite_l1_disconnect(l0);
    for (size_t cut = 1; cut < fullLen; cut += (fullLen > 64 ? fullLen / 32 : 1)) {
        HppliteL1 *l1 = NULL;
        snprintf(tag, sizeof(tag), "t%zu", cut);
        HppliteNode *r = sync_after(&l1, full, cut, tag);
        if (!r) FAILF("setup cut %zu", cut);
        if (count_rows(r->db, "t") != 1) FAILF("truncated at %zu bytes was applied", cut);
        hpplite_node_destroy(r);
        hpplite_l1_disconnect(l1);
    }
    {
        uint8_t *ext = malloc(fullLen + 4096);
        memcpy(ext, full, fullLen);
        memset(ext + fullLen, 'A', 4096);
        HppliteL1 *l1 = NULL;
        HppliteNode *r = sync_after(&l1, ext, fullLen + 4096, "ext");
        if (!r) FAIL("setup ext");
        if (count_rows(r->db, "t") != 1) FAIL("extended envelope was applied");
        hpplite_node_destroy(r);
        hpplite_l1_disconnect(l1);
        free(ext);
    }
    free(full);
    PASS();
}

static void test_mutation_fuzz(void) {
    TEST("random mutations of a valid sealed batch: open() rejects every one, replica untouched");
    reset_dir();
    HppliteL1 *l0 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/fuzz_src.db", l0);
    if (!a) FAIL("open");
    size_t fullLen = 0;
    uint8_t *full = sealed_batch(a, 2, 2, "DELETE FROM t", &fullLen);
    if (!full) FAIL("seal");
    uint8_t key[32];
    memcpy(key, a->encKey, 32);
    hpplite_node_destroy(a);
    hpplite_l1_disconnect(l0);

    /* the sanity half: the unmutated envelope opens */
    {
        const HppliteCipher *scheme = NULL;
        uint8_t *p = hpplite_envelope_open(key, 2, full, fullLen, NULL, &scheme);
        if (!p) FAIL("the valid envelope does not open");
        free(p);
        p = hpplite_envelope_open(key, 3, full, fullLen, NULL, &scheme);
        if (p) { free(p); FAIL("the envelope opens at the wrong height"); }
    }
    /* 2000 direct mutations: bit flips, byte sets, truncations, insertions */
    uint8_t *m = malloc(fullLen + 64);
    int rejected = 0;
    for (int i = 0; i < 2000; i++) {
        size_t len = fullLen;
        memcpy(m, full, fullLen);
        switch (rng() % 4) {
            case 0: m[rng() % len] ^= (uint8_t)(1u << (rng() % 8)); break;
            case 1: m[rng() % len] = (uint8_t)rng(); break;
            case 2: len = 1 + rng() % (len - 1); break;
            case 3: { size_t at = rng() % len; memmove(m + at + 1, m + at, len - at); m[at] = (uint8_t)rng(); len++; break; }
        }
        if (len == fullLen && memcmp(m, full, fullLen) == 0) { rejected++; continue; }   /* a no-op mutation */
        const HppliteCipher *scheme = NULL;
        uint8_t *p = hpplite_envelope_open(key, 2, m, len, NULL, &scheme);
        if (p) { free(p); FAILF("mutation %d opened", i); }
        rejected++;
    }
    /* and through the node, a sample of them */
    char tag[32];
    for (int i = 0; i < 24; i++) {
        size_t len = fullLen;
        memcpy(m, full, fullLen);
        if (i % 3 == 0) m[rng() % len] ^= (uint8_t)(1u << (rng() % 8));
        else if (i % 3 == 1) len = 1 + rng() % (len - 1);
        else m[rng() % len] ^= 0xff;
        HppliteL1 *l1 = NULL;
        snprintf(tag, sizeof(tag), "m%d", i);
        HppliteNode *r = sync_after(&l1, m, len, tag);
        if (!r) FAILF("setup %d", i);
        if (count_rows(r->db, "t") != 1) FAILF("mutation %d through the node was applied", i);
        hpplite_node_destroy(r);
        hpplite_l1_disconnect(l1);
    }
    free(m);
    free(full);
    if (rejected != 2000) FAIL("count");
    PASS();
}

static void test_json_fuzz(void) {
    TEST("batch JSON parser survives mutations of a valid batch");
    HppliteBatch *b = hpplite_batch_new(7);
    hpplite_batch_add_stmt(b, "INSERT INTO t VALUES('a\"b\\c\\n')", 3, 0);
    hpplite_batch_add_stmt(b, "UPDATE t SET x = 'é'", -1, 1);
    char *json = hpplite_batch_to_json(b);
    hpplite_batch_free(b);
    if (!json) FAIL("to_json");
    size_t n = strlen(json);
    char *m = malloc(n + 64);
    for (int i = 0; i < 3000; i++) {
        size_t len = n;
        memcpy(m, json, n + 1);
        switch (rng() % 4) {
            case 0: m[rng() % len] = (char)("{}[]\",:\\0n9-tfu "[rng() % 17]); break;
            case 1: m[rng() % len] = (char)rng(); break;
            case 2: len = rng() % len; m[len] = 0; break;
            case 3: { size_t at = rng() % len; memmove(m + at + 1, m + at, len - at + 1); m[at] = (char)("{}[]\",:\\"[rng() % 8]); len++; break; }
        }
        for (size_t k = 0; k < len; k++) if (m[k] == 0) m[k] = ' ';   /* keep it a C string */
        HppliteBatch *p = hpplite_batch_from_json(m);
        if (p) {
            /* whatever parsed must be internally consistent */
            for (int t = 0; t < p->nTxns; t++) if (!p->aTxns[t].zSql) { free(m); sqlite3_free(json); FAILF("mutation %d: NULL sql in parsed batch", i); }
            hpplite_batch_free(p);
        }
    }
    free(m);
    sqlite3_free(json);
    PASS();
}

/* ------------------------------------------------------------------------
** 3. Size limits
** ---------------------------------------------------------------------- */
static void test_escaped_size_limit(void) {
    TEST("capture measures the statement as posted: quote-heavy SQL cannot overflow a batch");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    const size_t LIMIT = 4096;
    HppliteNode *n = open_node_ex(DIR "/size.db", l1, LIMIT, 0);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    if (hpplite_node_flush_batch(n) != 1) FAIL("batch 1");

    /* raw length well under the limit, escaped length over it: every
    ** character is a quote or a control byte that JSON doubles or sextuples */
    char *sql = malloc(LIMIT);
    size_t at = 0;
    at += (size_t)snprintf(sql, LIMIT, "INSERT INTO t VALUES('");
    while (at < 1600) { sql[at++] = '"'; sql[at++] = '\\'; sql[at++] = '\x01'; sql[at++] = '\n'; }
    at += (size_t)snprintf(sql + at, LIMIT - at, "')");
    if (at >= LIMIT - 200) FAIL("test statement too long for the limit");
    int rc = sqlite3_exec(n->db, sql, NULL, NULL, NULL);
    if (rc == SQLITE_OK) FAIL("a statement whose escaped form exceeds the batch was accepted");
    if (count_rows(n->db, "t") != 0) FAIL("refused statement left a row");
    if (hpplite_node_pending_count(n) != 0) FAIL("refused statement left a log entry");

    /* same raw length, plain characters: accepted, and the batch as posted fits */
    at = (size_t)snprintf(sql, LIMIT, "INSERT INTO t VALUES('");
    while (at < 1600) sql[at++] = 'a';
    at += (size_t)snprintf(sql + at, LIMIT - at, "')");
    rc = sqlite3_exec(n->db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) FAIL("a plain statement of the same length was refused");
    /* near the limit, still plain, still must fit when posted */
    at = (size_t)snprintf(sql, LIMIT, "INSERT INTO t VALUES('");
    while (at < LIMIT - 400) sql[at++] = 'b';
    at += (size_t)snprintf(sql + at, LIMIT - at, "')");
    rc = sqlite3_exec(n->db, sql, NULL, NULL, NULL);
    int secondAccepted = (rc == SQLITE_OK);
    free(sql);
    int h;
    while ((h = hpplite_node_flush_batch(n)) > 0) {
        size_t len = 0;
        uint8_t *raw = hpplite_l1_get_batch(l1, (uint64_t)h, &len);
        if (!raw) FAIL("batch missing on L1");
        free(raw);
        if (len > LIMIT) FAILF("batch %d is %zu bytes as posted, limit %zu", h, len, LIMIT);
    }
    if (hpplite_node_pending_count(n) != 0) FAIL("pending rows left after flushing");
    if (count_rows(n->db, "t") != 1 + secondAccepted) FAIL("row count");
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* Each transaction inserts rows tagged with its number. Decode every batch
** posted and check that no tag appears in two batches. */
static void test_transaction_boundary(void) {
    TEST("the size cut never splits a transaction across batches");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    const size_t LIMIT = 3000;
    HppliteNode *n = open_node_ex(DIR "/txn.db", l1, LIMIT, 0);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(tag, pad)", NULL);
    if (hpplite_node_flush_batch(n) != 1) FAIL("batch 1");
    /* 12 transactions of varying size: 1..9 statements of ~120 bytes */
    const int NTXN = 12;
    for (int k = 0; k < NTXN; k++) {
        int nStmt = 1 + (k * 7) % 9;
        hpplite_node_exec(n, "BEGIN", NULL);
        for (int s = 0; s < nStmt; s++) {
            char sql[256];
            snprintf(sql, sizeof(sql), "INSERT INTO t VALUES('txn%02d', '%080d')", k, s);
            if (sqlite3_exec(n->db, sql, NULL, NULL, NULL) != SQLITE_OK) FAIL("insert");
        }
        hpplite_node_exec(n, "COMMIT", NULL);
    }
    int seenIn[12];
    for (int k = 0; k < NTXN; k++) seenIn[k] = -1;
    int last = hpplite_node_flush_batch(n);      /* posts the whole backlog, batch after batch */
    if (last < 2) FAIL("flush");
    int nBatches = last - 1;
    for (int h = 2; h <= last; h++) {
        HppliteBatch *b = hpplite_node_fetch_batch(n, (uint64_t)h);
        if (!b) FAIL("fetch posted batch");
        for (int i = 0; i < b->nTxns; i++) {
            const char *p = strstr(b->aTxns[i].zSql, "'txn");
            if (!p) continue;
            int k = atoi(p + 4);
            if (seenIn[k] == -1) seenIn[k] = h;
            else if (seenIn[k] != h) { hpplite_batch_free(b); FAILF("transaction %d appears in batches %d and %d", k, seenIn[k], h); }
            /* group ids within a batch are contiguous and start at 0 */
            if (i > 0 && b->aTxns[i].txn != b->aTxns[i - 1].txn && b->aTxns[i].txn != b->aTxns[i - 1].txn + 1) {
                hpplite_batch_free(b); FAIL("transaction groups not contiguous");
            }
        }
        size_t len = 0;
        uint8_t *raw = hpplite_l1_get_batch(l1, (uint64_t)h, &len);
        if (raw) free(raw);
        if (len > LIMIT) { hpplite_batch_free(b); FAILF("batch %d is %zu bytes, limit %zu", h, len, LIMIT); }
        hpplite_batch_free(b);
    }
    if (nBatches < 2) FAIL("the limit did not split the backlog (test is not exercising the cut)");
    for (int k = 0; k < NTXN; k++) if (seenIn[k] == -1) FAILF("transaction %d was never posted", k);
    if (hpplite_node_pending_count(n) != 0) FAIL("pending left");
    /* replica agrees */
    unsigned char rootA[32], rootB[32];
    hpplite_node_get_state_root(n, rootA);
    HppliteNode *r = open_node_ex(DIR "/txn_replica.db", l1, LIMIT, 0);
    if (!r) FAIL("replica");
    hpplite_node_get_state_root(r, rootB);
    if (memcmp(rootA, rootB, 32) != 0) FAIL("replica root differs");
    hpplite_node_destroy(r);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

static void test_oversized_transaction(void) {
    TEST("a transaction that outgrows a batch is refused at the write, not wedged in the sequencer");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    const size_t LIMIT = 2000;
    HppliteNode *n = open_node_ex(DIR "/big.db", l1, LIMIT, 0);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    if (hpplite_node_flush_batch(n) != 1) FAIL("batch 1");
    hpplite_node_exec(n, "BEGIN", NULL);
    int refusedAt = -1;
    for (int s = 0; s < 40; s++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES('%060d')", s);
        if (sqlite3_exec(n->db, sql, NULL, NULL, NULL) != SQLITE_OK) { refusedAt = s; break; }
    }
    if (refusedAt < 1) FAIL("the transaction was never refused (or refused at its first statement)");
    /* whatever the application does next, the sequencer must be able to post */
    hpplite_node_exec(n, "COMMIT", NULL);
    hpplite_node_exec(n, "ROLLBACK", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES('after')", NULL);
    int h = hpplite_node_flush_batch(n);
    if (h < 2) FAIL("the sequencer is wedged: nothing could be posted");
    if (hpplite_node_pending_count(n) != 0) FAIL("pending rows left: the sequencer cannot drain");
    for (int k = 2; k <= h; k++) {
        size_t len = 0;
        uint8_t *raw = hpplite_l1_get_batch(l1, (uint64_t)k, &len);
        if (raw) free(raw);
        if (len > LIMIT) FAILF("batch %d is %zu bytes, limit %zu", k, len, LIMIT);
    }
    /* a transaction that fits is still fine, in one batch */
    hpplite_node_exec(n, "BEGIN", NULL);
    for (int s = 0; s < 8; s++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES('%060d')", 100 + s);
        if (sqlite3_exec(n->db, sql, NULL, NULL, NULL) != SQLITE_OK) FAIL("a transaction that fits was refused");
    }
    if (hpplite_node_exec(n, "COMMIT", NULL) != SQLITE_OK) FAIL("commit");
    int h2 = hpplite_node_flush_batch(n);
    if (h2 != h + 1) FAIL("a fitting transaction should be one batch");
    unsigned char rootA[32], rootB[32];
    hpplite_node_get_state_root(n, rootA);
    HppliteNode *r = open_node_ex(DIR "/big_replica.db", l1, LIMIT, 0);
    if (!r) FAIL("replica");
    hpplite_node_get_state_root(r, rootB);
    if (memcmp(rootA, rootB, 32) != 0) FAIL("replica root differs");
    if (count_rows(r->db, "t") != count_rows(n->db, "t")) FAIL("replica rows");
    hpplite_node_destroy(r);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

int main(void) {
    printf("=== HPPLite abuse tests ===\n");
    test_guard_spellings();
    test_app_defined_functions();
    test_wrong_height_batch();
    test_plaintext_on_encrypting_rollup();
    test_plaintext_at_height_1();
    test_garbage_on_l1();
    test_mutation_fuzz();
    test_json_fuzz();
    test_escaped_size_limit();
    test_transaction_boundary();
    test_oversized_transaction();
    printf("\n%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
