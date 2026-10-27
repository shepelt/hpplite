/*
** HPPLite durability tests
**
** The local database file is the only durable state. These tests cover:
**   - reopening an existing file continues height, no replay over local data
**   - a process that dies with unflushed commits loses nothing
**   - a failed L1 submission keeps the batch pending and retries at the same height
**   - batches replayed from L1 are not logged again as new pending SQL
**   - bound parameters are expanded so batches are replayable
**   - the pending log is atomic with the user's transaction
**   - the state root is a function of content, not of history
*/

#include "node.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { printf("  Testing: %s... ", name); tests_run++; } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static const unsigned char PRIVKEY[32] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20
};

#define DIR "/tmp/hpplite_test_durability"

static void reset_dir(void) {
    system("rm -rf " DIR " && mkdir -p " DIR);
}

static int count_rows(sqlite3 *db, const char *table) {
    char sql[128];
    sqlite3_stmt *stmt;
    int n = -1;
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s", table);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

static HppliteNode *open_node(const char *dbPath, HppliteL1 *l1) {
    HppliteNodeConfig config;
    HppliteNode *node;
    memset(&config, 0, sizeof(config));
    config.nodeId = "durability";
    memcpy(config.privkey, PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = DIR;
    config.dbPath = (char *)dbPath;
    node = hpplite_node_create(&config);
    if (node && l1) {
        hpplite_node_set_l1(node, l1);
        hpplite_node_sync_from_l1(node, 1);
        hpplite_node_start(node);
    }
    return node;
}

/* 1. Reopening an existing file continues where it left off */
static void test_reopen_continues(void) {
    TEST("reopen continues height, no duplicate rows");
    reset_dir();
    const char *path = DIR "/reopen.db";

    HppliteNode *n = open_node(path, NULL);
    if (!n) FAIL("open 1");
    hpplite_node_exec(n, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1,'a')", NULL);
    if (hpplite_node_flush_batch(n) != 1) FAIL("first flush should be height 1");
    hpplite_node_destroy(n);

    n = open_node(path, NULL);
    if (!n) FAIL("open 2");
    if (hpplite_node_get_height(n) != 2) FAIL("height should be 2 after reopen");
    if (hpplite_node_pending_count(n) != 0) FAIL("nothing should be pending after reopen");
    if (count_rows(n->db, "t") != 1) FAIL("row count changed by reopen");
    hpplite_node_exec(n, "INSERT INTO t VALUES(2,'b')", NULL);
    if (hpplite_node_flush_batch(n) != 2) FAIL("second flush should be height 2");
    hpplite_node_destroy(n);

    n = open_node(path, NULL);
    if (!n) FAIL("open 3");
    if (hpplite_node_get_height(n) != 3) FAIL("height should be 3");
    if (count_rows(n->db, "t") != 2) FAIL("expected 2 rows");
    hpplite_node_destroy(n);
    PASS();
}

/* 2. A process that dies with unflushed commits loses nothing */
static void test_crash_recovers_pending(void) {
    TEST("unflushed commits survive a crash");
    reset_dir();
    const char *path = DIR "/crash.db";

    pid_t pid = fork();
    if (pid == 0) {
        HppliteNode *n = open_node(path, NULL);
        if (!n) _exit(1);
        hpplite_node_exec(n, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
        hpplite_node_exec(n, "INSERT INTO t VALUES(1,'a')", NULL);
        hpplite_node_exec(n, "BEGIN; INSERT INTO t VALUES(2,'b'); INSERT INTO t VALUES(3,'c'); COMMIT;", NULL);
        _exit(0);  /* no close, no flush: simulates a crash */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) FAIL("child did not run");

    HppliteNode *n = open_node(path, NULL);
    if (!n) FAIL("reopen after crash");
    if (count_rows(n->db, "t") != 3) FAIL("SQLite lost committed rows (unexpected)");
    int pending = hpplite_node_pending_count(n);
    if (pending != 4) { printf("(pending=%d) ", pending); FAIL("expected 4 pending statements"); }
    if (hpplite_node_flush_batch(n) != 1) FAIL("recovered pending should flush as batch 1");
    if (hpplite_node_pending_count(n) != 0) FAIL("pending should be empty after flush");
    hpplite_node_destroy(n);
    PASS();
}

/* 3. A failed L1 submission keeps the batch pending; retry uses the same height */
static void test_submit_failure_retries(void) {
    TEST("failed L1 submit keeps batch pending and retries");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/retry.db", l1);
    if (!n) FAIL("open");

    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1)", NULL);

    hpplite_l1_mock_set_fail_submit(l1, 1);
    if (hpplite_node_flush_batch(n) != 0) FAIL("flush should report failure");
    if (hpplite_node_get_height(n) != 1) FAIL("height must not advance on failure");
    if (hpplite_node_pending_count(n) != 2) FAIL("pending rows must be kept");

    hpplite_node_exec(n, "INSERT INTO t VALUES(2)", NULL);  /* more work while L1 is down */

    hpplite_l1_mock_set_fail_submit(l1, 0);
    if (hpplite_node_flush_batch(n) != 1) FAIL("retry should submit at height 1");
    if (hpplite_node_pending_count(n) != 0) FAIL("pending should be empty after retry");

    size_t len = 0;
    uint8_t *data = hpplite_l1_get_batch(l1, 1, &len);
    if (!data) FAIL("batch 1 missing on L1");
    HppliteBatch *b = hpplite_batch_from_json((const char *)data);
    free(data);
    if (!b || b->nTxns != 3) FAIL("batch 1 should carry all 3 statements");
    hpplite_batch_free(b);

    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 4. Batches replayed from L1 are not logged again as pending SQL */
static void test_replay_not_relogged(void) {
    TEST("replay from L1 is not re-logged; next batch holds only new SQL");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();

    HppliteNode *a = open_node(DIR "/a.db", l1);
    if (!a) FAIL("open a");
    hpplite_node_exec(a, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    hpplite_node_exec(a, "INSERT INTO t VALUES(1,'a')", NULL);
    hpplite_node_exec(a, "INSERT INTO t VALUES(2,'b')", NULL);
    if (hpplite_node_flush_batch(a) != 1) FAIL("batch 1");
    unsigned char rootA[32];
    hpplite_node_get_state_root(a, rootA);
    hpplite_node_destroy(a);

    HppliteNode *b = open_node(DIR "/b.db", l1);   /* fresh file, rebuilt from L1 */
    if (!b) FAIL("open b");
    if (count_rows(b->db, "t") != 2) FAIL("reconstruction incomplete");
    if (hpplite_node_pending_count(b) != 0) FAIL("replayed SQL must not be pending");
    if (hpplite_node_get_height(b) != 2) FAIL("height should be 2 after replay");
    unsigned char rootB[32];
    hpplite_node_get_state_root(b, rootB);
    if (memcmp(rootA, rootB, 32) != 0) FAIL("state root differs after reconstruction");

    hpplite_node_exec(b, "INSERT INTO t VALUES(3,'c')", NULL);
    if (hpplite_node_flush_batch(b) != 2) FAIL("batch 2");
    size_t len = 0;
    uint8_t *data = hpplite_l1_get_batch(l1, 2, &len);
    if (!data) FAIL("batch 2 missing on L1");
    HppliteBatch *batch = hpplite_batch_from_json((const char *)data);
    free(data);
    if (!batch || batch->nTxns != 1) {
        printf("(nTxns=%d) ", batch ? batch->nTxns : -1);
        FAIL("batch 2 should contain exactly the new INSERT");
    }
    hpplite_batch_free(batch);
    hpplite_node_destroy(b);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 5. Bound parameters are expanded into the logged SQL */
static void test_bound_params_expanded(void) {
    TEST("bound parameters are expanded in the batch");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/bind.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT, b BLOB)", NULL);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(n->db, "INSERT INTO t VALUES(?, ?, ?)", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, 7);
    sqlite3_bind_text(stmt, 2, "it's", -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 3, "\x01\x02", 2, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
    sqlite3_bind_int(stmt, 1, 8);
    sqlite3_bind_text(stmt, 2, "two", -1, SQLITE_STATIC);
    sqlite3_bind_null(stmt, 3);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (hpplite_node_flush_batch(n) != 1) FAIL("flush");
    size_t len = 0;
    uint8_t *data = hpplite_l1_get_batch(l1, 1, &len);
    if (!data) FAIL("batch missing");
    HppliteBatch *batch = hpplite_batch_from_json((const char *)data);
    free(data);
    if (!batch || batch->nTxns != 3) FAIL("expected 3 statements");
    if (strstr(batch->aTxns[1].zSql, "?")) FAIL("placeholders not expanded");
    if (!strstr(batch->aTxns[1].zSql, "'it''s'")) FAIL("text not expanded/escaped");
    if (!strstr(batch->aTxns[1].zSql, "x'0102'")) FAIL("blob not expanded");
    if (!strstr(batch->aTxns[2].zSql, "NULL")) FAIL("null not expanded");
    hpplite_batch_free(batch);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 6. The pending log is atomic with the user's transaction */
static void test_log_atomic_with_txn(void) {
    TEST("pending log rolls back with the user's transaction");
    reset_dir();
    HppliteNode *n = open_node(DIR "/atomic.db", NULL);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(id INTEGER PRIMARY KEY)", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1)", NULL);
    int base = hpplite_node_pending_count(n);
    if (base != 2) FAIL("expected 2 pending");

    hpplite_node_exec(n, "BEGIN; INSERT INTO t VALUES(2); INSERT INTO t VALUES(3); ROLLBACK;", NULL);
    if (hpplite_node_pending_count(n) != base) FAIL("rolled-back statements were logged");

    char *err = NULL;
    hpplite_node_exec(n, "INSERT INTO t VALUES(1)", &err);  /* fails: duplicate key */
    sqlite3_free(err);
    if (hpplite_node_pending_count(n) != base) FAIL("failed statement was logged");

    hpplite_node_exec(n, "BEGIN; INSERT INTO t VALUES(4); INSERT INTO t VALUES(5); COMMIT;", NULL);
    if (hpplite_node_pending_count(n) != base + 2) FAIL("committed statements not logged");

    HppliteBatch *batch = hpplite_flush_block(n->ctx);
    if (!batch) FAIL("flush");
    if (batch->nTxns != 4) FAIL("expected 4 statements in batch");
    if (batch->aTxns[2].txn != batch->aTxns[3].txn) FAIL("statements of one txn should share a group");
    if (batch->aTxns[1].txn == batch->aTxns[2].txn) FAIL("separate txns should not share a group");
    hpplite_batch_free(batch);
    hpplite_node_destroy(n);
    PASS();
}

/* 7. State root depends on content only */
static void test_state_root_is_content(void) {
    TEST("state root is a function of content");
    reset_dir();
    HppliteNode *a = open_node(DIR "/ra.db", NULL);
    HppliteNode *b = open_node(DIR "/rb.db", NULL);
    if (!a || !b) FAIL("open");
    unsigned char ra[32], rb[32], zero[32] = {0};
    hpplite_node_get_state_root(a, ra);
    if (memcmp(ra, zero, 32) != 0) FAIL("empty database should have a zero root");

    hpplite_node_exec(a, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    hpplite_node_exec(a, "INSERT INTO t VALUES(1,'x')", NULL);
    hpplite_node_exec(a, "INSERT INTO t VALUES(2,'y')", NULL);
    hpplite_node_exec(a, "UPDATE t SET v='z' WHERE id=2", NULL);

    hpplite_node_exec(b, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    hpplite_node_exec(b, "INSERT INTO t VALUES(2,'z')", NULL);   /* different history */
    hpplite_node_exec(b, "INSERT INTO t VALUES(1,'x')", NULL);

    hpplite_node_get_state_root(a, ra);
    hpplite_node_get_state_root(b, rb);
    if (memcmp(ra, rb, 32) != 0) FAIL("same content should give the same root");

    hpplite_node_exec(b, "DELETE FROM t WHERE id=1", NULL);
    hpplite_node_get_state_root(b, rb);
    if (memcmp(ra, rb, 32) == 0) FAIL("different content should give a different root");

    unsigned char direct[32];
    hpplite_db_hash(a->db, direct);
    if (memcmp(ra, direct, 32) != 0) FAIL("hpplite_db_hash should match the node's root");

    hpplite_node_destroy(a);
    hpplite_node_destroy(b);
    PASS();
}

/* 8. After a crash the old lease blocks submission only until it expires */
static void test_stale_lease_after_crash(void) {
    TEST("stale lease after crash: pending kept, submits once lease expires");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/lease_a.db", l1);
    if (!a) FAIL("open a");
    hpplite_node_exec(a, "CREATE TABLE t(x)", NULL);
    if (hpplite_node_flush_batch(a) != 1) FAIL("batch 1");
    a->hasLease = 0;              /* pretend a crashed: lease is never released */
    hpplite_node_destroy(a);

    HppliteNode *b = open_node(DIR "/lease_b.db", l1);
    if (!b) FAIL("open b");
    if (count_rows(b->db, "t") != 0) FAIL("reconstruction");
    hpplite_node_exec(b, "INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(b) != 0) FAIL("submit should fail while a's lease is active");
    if (hpplite_node_pending_count(b) != 1) FAIL("pending must be kept");

    hpplite_l1_mock_advance_time(l1, 10 * 60);   /* lease expires */
    if (hpplite_node_flush_batch(b) != 2) FAIL("submit should succeed after expiry");
    if (hpplite_node_pending_count(b) != 0) FAIL("pending should be empty");
    hpplite_node_destroy(b);
    hpplite_l1_disconnect(l1);
    PASS();
}

int main(void) {
    printf("=== HPPLite Durability Tests ===\n");
    test_reopen_continues();
    test_crash_recovers_pending();
    test_submit_failure_retries();
    test_replay_not_relogged();
    test_bound_params_expanded();
    test_log_atomic_with_txn();
    test_state_root_is_content();
    test_stale_lease_after_crash();
    system("rm -rf " DIR);
    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
