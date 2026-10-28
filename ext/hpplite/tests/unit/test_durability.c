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
#include "cipher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { printf("  Testing: %s... ", name); tests_run++; } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

#define PRIVKEY_HEX "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
static const unsigned char PRIVKEY[32] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20
};

#define DIR "/tmp/hpplite_test_durability"

static void reset_dir(void) {
    (void)!system("rm -rf " DIR " && mkdir -p " DIR);
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

    HppliteBatch *b = hpplite_node_fetch_batch(n, 1);
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
    HppliteBatch *batch = hpplite_node_fetch_batch(b, 2);
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
    HppliteBatch *batch = hpplite_node_fetch_batch(n, 1);
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
    if (!b->hasLease) FAIL("b should hold the lease now");

    /* issue #21: the holder renews once the lease is past half its life,
    ** so a busy sequencer never lets its own lease lapse */
    hpplite_l1_mock_advance_time(l1, 200);
    hpplite_node_exec(b, "INSERT INTO t VALUES(2)", NULL);
    if (hpplite_node_flush_batch(b) != 3) FAIL("submit near expiry");
    if (hpplite_l1_get_lease_time_remaining(l1) != 300) { printf("(left=%llu) ", (unsigned long long)hpplite_l1_get_lease_time_remaining(l1)); FAIL("lease should have been renewed"); }

    /* and a failing L1 makes the timer back off instead of retrying every interval */
    hpplite_l1_mock_set_fail_submit(l1, 1);
    hpplite_node_exec(b, "INSERT INTO t VALUES(3)", NULL);
    if (hpplite_node_flush_batch(b) != 0) FAIL("submit should fail");
    if (b->failStreak != 1 || b->retryAt == 0) FAIL("failure should schedule a delayed retry");
    hpplite_node_exec(b, "INSERT INTO t VALUES(4)", NULL);
    hpplite_node_flush_batch(b);
    if (b->failStreak != 2) FAIL("second failure doubles the backoff");
    hpplite_l1_mock_set_fail_submit(l1, 0);
    if (hpplite_node_flush_batch(b) != 4) FAIL("explicit flush retries at once");
    if (b->failStreak != 0 || b->retryAt != 0) FAIL("success clears the backoff");
    hpplite_node_destroy(b);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 9. Confirmation lost after a successful submit: next flush adopts the L1 batch */
static void test_adopt_after_lost_confirmation(void) {
    TEST("lost confirmation: batch on L1 is adopted, not resubmitted");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/adopt.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1)", NULL);

    hpplite_l1_mock_set_lose_confirmation(l1, 1);
    /* submit stores the batch on L1 but reports failure; flush must notice
    ** the batch is there and finalize it instead of giving up */
    if (hpplite_node_flush_batch(n) != 1) FAIL("flush should adopt the batch it just lost track of");
    if (hpplite_node_pending_count(n) != 0) FAIL("pending should be empty after adoption");
    if (hpplite_node_get_height(n) != 2) FAIL("height should be 2");
    hpplite_l1_mock_set_lose_confirmation(l1, 0);

    /* and the normal case afterwards continues at the right height */
    hpplite_node_exec(n, "INSERT INTO t VALUES(2)", NULL);
    if (hpplite_node_flush_batch(n) != 2) FAIL("next batch should be height 2");
    HppliteBatch *b = hpplite_node_fetch_batch(n, 2);
    if (!b || b->nTxns != 1) FAIL("batch 2 should hold only the new INSERT");
    hpplite_batch_free(b);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 10. Crash between submit and finalize, then reopen: adopted on open */
static void test_adopt_on_reopen(void) {
    TEST("crash after submit: reopen adopts the batch from L1");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/adopt2.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1)", NULL);
    /* put the batch on L1 "by hand" without finalizing locally */
    sqlite3_int64 maxId = 0;
    HppliteBatch *b = hpplite_build_batch(n->ctx, &maxId);
    char *json = hpplite_batch_to_json(b);
    char *tx = hpplite_l1_submit_batch(l1, n->instanceId, b->height, (const uint8_t *)json, strlen(json));
    if (!tx) FAIL("manual submit");
    sqlite3_free(tx); sqlite3_free(json); hpplite_batch_free(b);
    /* crash: tear the node down without the flush destroy would do */
    n->hasLease = 0;
    n->l1 = NULL;
    hpplite_node_stop_timer(n);
    hpplite_unregister_node(n->db);
    hpplite_shutdown(n->ctx); n->ctx = NULL;
    if (n->bkDb) { sqlite3_close(n->bkDb); n->bkDb = NULL; }
    sqlite3_close(n->db); n->db = NULL;
    n->ownsDb = 0;
    hpplite_node_destroy(n);

    n = open_node(DIR "/adopt2.db", l1);
    if (!n) FAIL("reopen");
    if (hpplite_node_pending_count(n) != 0) FAIL("reopen should have adopted the L1 batch");
    if (hpplite_node_get_height(n) != 2) FAIL("height should be 2 after adoption");
    if (count_rows(n->db, "t") != 1) FAIL("local rows must be intact (no replay over them)");
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 11. L1 holds a batch at our height that is not ours: refuse to fork */
static void test_divergence_refused(void) {
    TEST("pending SQL that does not match the L1 batch is refused, not forked");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/div_a.db", l1);
    if (!a) FAIL("open a");
    hpplite_node_exec(a, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(a, "INSERT INTO t VALUES('from a')", NULL);
    if (hpplite_node_flush_batch(a) != 1) FAIL("a batch 1");
    hpplite_node_destroy(a);

    /* b was started against an empty L1 and wrote its own history locally */
    HppliteL1 *empty = hpplite_l1_create_mock();
    HppliteNode *b = open_node(DIR "/div_b.db", empty);
    if (!b) FAIL("open b");
    hpplite_node_exec(b, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(b, "INSERT INTO t VALUES('from b')", NULL);
    hpplite_node_set_l1(b, l1);                /* now points at a's L1 */
    if (hpplite_node_sync_from_l1(b, 1) != -1) FAIL("sync should report divergence");
    if (hpplite_node_pending_count(b) != 2) FAIL("local pending must be untouched");
    if (hpplite_node_flush_batch(b) != 0) FAIL("flush must not succeed over diverged history");
    if (count_rows(b->db, "t") != 1) FAIL("local content must not be replayed over");
    b->l1 = NULL;                               /* don't let destroy keep trying */
    hpplite_node_destroy(b);
    hpplite_l1_disconnect(l1);
    hpplite_l1_disconnect(empty);
    PASS();
}

/* 12. Background flush while the application holds an open transaction */
static void test_timer_vs_open_transaction(void) {
    TEST("background flush never sees or disturbs an open transaction");
    reset_dir();
    sqlite3 *db = hpplite_open("file:" DIR "/timer.db?hpplite=on&interval=100ms&datadir=" DIR);
    if (!db) FAIL("open");
    HppliteCtx *ctx = hpplite_context(db);
    char *err = NULL;
    if (sqlite3_exec(db, "CREATE TABLE t(x)", NULL, NULL, &err) != SQLITE_OK) FAIL("create");
    usleep(300000);                                  /* timer flushes the CREATE */
    uint64_t h1 = hpplite_get_block_height(ctx);
    if (h1 != 2) { printf("(h=%llu) ", (unsigned long long)h1); FAIL("CREATE should have been flushed by the timer"); }

    if (sqlite3_exec(db, "BEGIN; INSERT INTO t VALUES(1); INSERT INTO t VALUES(2);", NULL, NULL, &err) != SQLITE_OK) FAIL("begin");
    usleep(400000);                                  /* several timer ticks inside the txn */
    if (hpplite_get_block_height(ctx) != 2) FAIL("timer must not flush uncommitted rows");
    if (sqlite3_exec(db, "INSERT INTO t VALUES(3); COMMIT;", NULL, NULL, &err) != SQLITE_OK) {
        printf("(%s) ", err ? err : "?"); sqlite3_free(err);
        FAIL("COMMIT failed while the timer thread was active (SQLITE_BUSY?)");
    }
    usleep(300000);
    if (hpplite_get_block_height(ctx) != 3) FAIL("committed transaction should be flushed as one batch");
    if (hpplite_pending_count(ctx) != 0) FAIL("pending should be empty");
    sqlite3_close(db);
    PASS();
}

/* 13. Checkpoint progress survives reopen */
static void test_checkpoint_persists(void) {
    TEST("checkpoint progress persists across reopen");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNodeConfig config;
    memset(&config, 0, sizeof(config));
    config.nodeId = "cp"; memcpy(config.privkey, PRIVKEY, 32); config.hasPrivkey = 1;
    config.dataDir = DIR; config.dbPath = DIR "/cp.db"; config.checkpointInterval = 2;
    HppliteNode *n = hpplite_node_create(&config);
    if (!n) FAIL("open");
    hpplite_node_set_l1(n, l1); hpplite_node_start(n);
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL); hpplite_node_flush_batch(n);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1)", NULL); hpplite_node_flush_batch(n);
    if (hpplite_l1_mock_get_checkpoint_count(l1) != 1) FAIL("flush should have submitted the checkpoint");
    hpplite_node_destroy(n);

    n = hpplite_node_create(&config);
    if (!n) FAIL("reopen");
    hpplite_node_set_l1(n, l1);
    if (n->lastCheckpointHeight != 2) { printf("(cp=%llu) ", (unsigned long long)n->lastCheckpointHeight); FAIL("last checkpoint height not restored"); }
    if (n->ctx->lastCheckpointHeight != 2) FAIL("ctx checkpoint height not restored");
    const HppliteCheckpoint *cp = hpplite_l1_mock_get_last_checkpoint(l1);
    if (!cp || memcmp(cp->postStateRoot, n->lastCheckpointRoot, 32) != 0) FAIL("checkpoint root not restored");
    /* issue #22: a process that writes one batch and exits, repeatedly, must
    ** still reach the next checkpoint - the count is derived from heights */
    hpplite_node_exec(n, "INSERT INTO t VALUES(2)", NULL); hpplite_node_flush_batch(n);
    hpplite_node_destroy(n);
    n = hpplite_node_create(&config);
    if (!n) FAIL("reopen 2");
    hpplite_node_set_l1(n, l1);
    if (n->batchesSinceCheckpoint != 1) { printf("(since=%d) ", n->batchesSinceCheckpoint); FAIL("batches since checkpoint not derived on reopen"); }
    hpplite_node_exec(n, "INSERT INTO t VALUES(3)", NULL); hpplite_node_flush_batch(n);
    if (hpplite_l1_mock_get_checkpoint_count(l1) != 2) FAIL("second checkpoint should follow across restarts");
    if (n->lastCheckpointHeight != 4) FAIL("second checkpoint height");
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 14. :memory: database through the node (bookkeeping on the same connection) */
static void test_memory_database(void) {
    TEST(":memory: database works with bookkeeping on the same connection");
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(":memory:", l1);
    if (!n) FAIL("open");
    if (n->bkDb != NULL) FAIL("no second connection expected for :memory:");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_pending_count(n) != 2) FAIL("pending");
    if (hpplite_node_flush_batch(n) != 1) FAIL("flush");
    if (hpplite_node_pending_count(n) != 0) FAIL("pending after flush");
    /* flush inside an open transaction must refuse rather than read uncommitted rows */
    hpplite_node_exec(n, "BEGIN; INSERT INTO t VALUES(2);", NULL);
    if (hpplite_node_flush_batch(n) != 0) FAIL("flush inside an open transaction should do nothing");
    hpplite_node_exec(n, "COMMIT", NULL);
    if (hpplite_node_flush_batch(n) != 2) FAIL("flush after commit");
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 15. hpplite_sync() SQL function pulls new batches from L1 */
static void test_sync_function(void) {
    TEST("SELECT hpplite_sync() replays new L1 batches into a reader");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *reader = open_node(DIR "/reader.db", l1);   /* opened against an empty L1 */
    if (!reader) FAIL("open reader");
    hpplite_node_release_lease(reader);                       /* a reader does not sequence */
    HppliteNode *writer = open_node(DIR "/writer.db", l1);
    if (!writer) FAIL("open writer");
    hpplite_node_exec(writer, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(writer, "INSERT INTO t VALUES(1)", NULL);
    hpplite_node_exec(writer, "INSERT INTO t VALUES(2)", NULL);
    if (hpplite_node_flush_batch(writer) != 1) FAIL("writer batch");

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(reader->db, "SELECT hpplite_sync()", -1, &s, NULL) != SQLITE_OK) FAIL("prepare sync");
    int synced = (sqlite3_step(s) == SQLITE_ROW) ? sqlite3_column_int(s, 0) : -1;
    sqlite3_finalize(s);
    if (synced != 1) { printf("(synced=%d) ", synced); FAIL("expected 1 batch synced"); }
    if (count_rows(reader->db, "t") != 2) FAIL("rows not replayed");
    if (hpplite_node_pending_count(reader) != 0) FAIL("replayed SQL must not be pending");
    if (hpplite_node_get_height(reader) != 2) FAIL("reader height");
    reader->l1 = NULL;   /* reader never held the lease; nothing to release */
    hpplite_node_destroy(reader);
    hpplite_node_destroy(writer);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 16. Pending SQL beyond the DA size limit is submitted as several batches */
static void test_batch_size_limit(void) {
    TEST("pending SQL over the DA limit is split into batches that each fit");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    hpplite_l1_mock_set_batch_size_limit(l1, 3000);
    HppliteNode *n = open_node(DIR "/big.db", l1);
    if (!n) FAIL("open");
    if (n->config.maxBatchBytes != 3000) FAIL("limit not learned from L1 system config");
    n->config.checkpointInterval = 1;   /* the series must end with a root + checkpoint */
    hpplite_node_exec(n, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT)", NULL);
    char sql[512], pad[301];
    memset(pad, 'x', 300); pad[300] = 0;
    for (int i = 1; i <= 30; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES(%d,'%s')", i, pad);
        hpplite_node_exec(n, sql, NULL);
    }
    hpplite_node_exec(n, "BEGIN; INSERT INTO t VALUES(31,'a'); INSERT INTO t VALUES(32,'b'); COMMIT;", NULL);

    uint64_t last = hpplite_node_flush_batch(n);
    if (last < 3) { printf("(last=%llu) ", (unsigned long long)last); FAIL("expected several batches"); }
    if (hpplite_node_pending_count(n) != 0) FAIL("everything should have been flushed");
    if (hpplite_node_get_height(n) != last + 1) FAIL("height");

    unsigned char rootA[32];
    hpplite_node_get_state_root(n, rootA);
    int stmts = 0;
    for (uint64_t h = 1; h <= last; h++) {
        size_t len = 0;
        uint8_t *data = hpplite_l1_get_batch(l1, h, &len);
        if (!data) FAIL("batch missing on L1");
        if (len > 3000) FAIL("a batch (sealed) exceeds the limit");
        if (!hpplite_envelope_is_sealed(data, len)) FAIL("batch should be sealed by default");
        free(data);
        HppliteBatch *b = hpplite_node_fetch_batch(n, h);
        if (!b) FAIL("parse");
        if (h < last && !b->partial) FAIL("intermediate batch should be marked partial");
        if (h == last && b->partial) FAIL("last batch must not be partial");
        if (b->partial && b->hasRoot) FAIL("a partial batch must not carry a root");
        if (h == last && (!b->hasRoot || memcmp(b->postStateRoot, rootA, 32) != 0))
            FAIL("the series' last batch must carry the content root");
        /* the explicit 2-statement transaction must not be split */
        for (int i = 0; i < b->nTxns; i++) {
            if (strstr(b->aTxns[i].zSql, "VALUES(31,'a')") &&
                (i + 1 >= b->nTxns || !strstr(b->aTxns[i + 1].zSql, "VALUES(32,'b')")))
                FAIL("transaction split across batches");
        }
        stmts += b->nTxns;
        hpplite_batch_free(b);
    }
    if (stmts != 33) { printf("(stmts=%d) ", stmts); FAIL("statement count across batches"); }
    if (hpplite_l1_mock_get_checkpoint_count(l1) < 1) FAIL("checkpoint should follow the series");
    if (memcmp(hpplite_l1_mock_get_last_checkpoint(l1)->postStateRoot, rootA, 32) != 0) FAIL("checkpoint root");

    /* a fresh node rebuilt from the chunked history ends with the same content */
    HppliteNode *r = open_node(DIR "/big_r.db", l1);
    if (!r) FAIL("open reader");
    unsigned char rootB[32];
    hpplite_node_get_state_root(r, rootB);
    if (count_rows(r->db, "t") != 32 || memcmp(rootA, rootB, 32) != 0) FAIL("reconstruction from chunks differs");
    r->l1 = NULL;
    hpplite_node_destroy(r);

    /* a single statement that cannot fit is refused when it runs (the app
    ** sees SQLITE_INTERRUPT) instead of stalling every later batch forever */
    char huge[3600];
    memset(huge, 'y', 3500); huge[3500] = 0;
    char *big = sqlite3_mprintf("INSERT INTO t VALUES(99,'%s')", huge);
    int rc = sqlite3_exec(n->db, big, NULL, NULL, NULL);
    sqlite3_free(big);
    if (rc != SQLITE_INTERRUPT) { printf("(rc=%d) ", rc); FAIL("oversized statement should fail with SQLITE_INTERRUPT"); }
    if (count_rows(n->db, "t") != 32) FAIL("refused statement must not be applied");
    if (hpplite_node_pending_count(n) != 0) FAIL("refused statement must not be logged");
    if (n->ctx->nLogErrors == 0) FAIL("oversized statement should be reported");
    /* the same inside an explicit transaction: the transaction is gone, the app knows */
    sqlite3_exec(n->db, "BEGIN; INSERT INTO t VALUES(100,'ok')", NULL, NULL, NULL);
    big = sqlite3_mprintf("INSERT INTO t VALUES(101,'%s')", huge);
    rc = sqlite3_exec(n->db, big, NULL, NULL, NULL);
    sqlite3_free(big);
    if (rc != SQLITE_INTERRUPT) FAIL("oversized statement in a transaction should fail");
    if (!sqlite3_get_autocommit(n->db)) sqlite3_exec(n->db, "ROLLBACK", NULL, NULL, NULL);
    if (count_rows(n->db, "t") != 32 || hpplite_node_pending_count(n) != 0) FAIL("nothing from the aborted transaction survives");
    /* later writes flow normally */
    hpplite_node_exec(n, "INSERT INTO t VALUES(102,'after')", NULL);
    if (hpplite_node_flush_batch(n) == 0) FAIL("writes after a refused statement must still flush");
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 17. Transaction stays pending past the wait: adopted once it lands */
static void test_slow_confirmation(void) {
    TEST("submit whose transaction is still pending after the wait is adopted later");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_node(DIR "/slow.db", l1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);

    /* the mock stores the batch at submit time, so "pending" here means the
    ** reply timed out but the transaction did land: adoption finds it */
    hpplite_l1_mock_set_tx_pending(l1, 1);
    if (hpplite_node_flush_batch(n) != 1) FAIL("landed-but-unconfirmed batch should be adopted");
    hpplite_l1_mock_set_tx_pending(l1, 0);
    if (hpplite_node_pending_count(n) != 0) FAIL("pending");
    if (hpplite_node_get_height(n) != 2) FAIL("height");
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 18. A statement that failed at the sequencer but was logged (trigger changed
**     rows before the abort) is skipped on replay; content and root match */
static void test_replay_skips_failed_statement(void) {
    TEST("statement failed at sequencer is skipped on replay, content identical");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/skip_a.db", l1);
    if (!a) FAIL("open a");
    hpplite_node_exec(a, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    hpplite_node_exec(a, "CREATE TRIGGER tr AFTER INSERT ON t BEGIN UPDATE t SET v=upper(v) WHERE id=NEW.id; END", NULL);
    hpplite_node_exec(a, "INSERT INTO t VALUES(1,'a')", NULL);
    hpplite_node_exec(a, "BEGIN", NULL);
    hpplite_node_exec(a, "INSERT INTO t VALUES(50,'x'),(1,'dup')", NULL);   /* trigger ran, then abort */
    hpplite_node_exec(a, "INSERT INTO t VALUES(2,'b')", NULL);
    hpplite_node_exec(a, "COMMIT", NULL);
    if (count_rows(a->db, "t") != 2) FAIL("sequencer content");
    if (hpplite_node_flush_batch(a) != 1) FAIL("flush");
    unsigned char ra[32]; hpplite_node_get_state_root(a, ra);
    hpplite_node_destroy(a);

    HppliteNode *b = open_node(DIR "/skip_b.db", l1);
    if (!b) FAIL("open b (replay)");
    if (count_rows(b->db, "t") != 2) FAIL("replayed content differs");
    unsigned char rb[32]; hpplite_node_get_state_root(b, rb);
    if (memcmp(ra, rb, 32) != 0) FAIL("root differs after replay");
    if (b->ctx->nReplaySkipped > 1) FAIL("at most the aborted statement should be skipped");
    b->l1 = NULL;
    hpplite_node_destroy(b);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 19. Batches are sealed by default; only the wallet key opens them */
static void test_encryption_default(void) {
    TEST("batches are sealed with the wallet-derived key; wrong key cannot reconstruct");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/enc_a.db", l1);
    if (!a) FAIL("open a");
    if (!a->cipher) FAIL("encryption should be on by default when a key exists");
    hpplite_node_exec(a, "CREATE TABLE secrets(id INTEGER PRIMARY KEY, v TEXT)", NULL);
    hpplite_node_exec(a, "INSERT INTO secrets VALUES(1,'the-plaintext-marker')", NULL);
    if (hpplite_node_flush_batch(a) != 1) FAIL("flush");

    size_t len = 0;
    uint8_t *raw = hpplite_l1_get_batch(l1, 1, &len);
    if (!raw) FAIL("batch missing");
    if (!hpplite_envelope_is_sealed(raw, len)) FAIL("posted bytes are not an envelope");
    if (memmem(raw, len, "the-plaintext-marker", 20)) FAIL("plaintext leaked to L1");
    if (hpplite_batch_from_json((const char *)raw) != NULL) FAIL("sealed bytes parsed as JSON");
    free(raw);
    HppliteBatch *b = hpplite_node_fetch_batch(a, 1);
    if (!b || b->nTxns != 2) FAIL("owner cannot read its own batch");
    hpplite_batch_free(b);
    unsigned char ra[32]; hpplite_node_get_state_root(a, ra);
    hpplite_node_destroy(a);

    /* same wallet, fresh file: reconstructs */
    HppliteNode *r = open_node(DIR "/enc_r.db", l1);
    if (!r) FAIL("open reader");
    if (count_rows(r->db, "secrets") != 1) FAIL("owner reconstruction");
    unsigned char rr[32]; hpplite_node_get_state_root(r, rr);
    if (memcmp(ra, rr, 32) != 0) FAIL("root after reconstruction");
    r->l1 = NULL; hpplite_node_destroy(r);

    /* different wallet: sees ciphertext only, replays nothing, does not fork */
    HppliteNodeConfig config;
    memset(&config, 0, sizeof(config));
    config.nodeId = "stranger"; memcpy(config.privkey, PRIVKEY, 32); config.privkey[31] ^= 1;
    config.hasPrivkey = 1; config.dataDir = DIR; config.dbPath = DIR "/enc_x.db";
    HppliteNode *x = hpplite_node_create(&config);
    if (!x) FAIL("open stranger");
    hpplite_node_set_l1(x, l1);
    if (hpplite_node_sync_from_l1(x, 1) >= 0) FAIL("stranger should sync nothing and be told (issue #29)");
    if (x->state != HPPLITE_STATE_ERROR) FAIL("a node that cannot read L1 history is in error state");
    if (hpplite_node_get_height(x) != 1) FAIL("stranger height must not advance");
    if (hpplite_node_fetch_batch(x, 1) != NULL) FAIL("stranger opened the batch");
    x->l1 = NULL; hpplite_node_destroy(x);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 20. encrypt=off posts plaintext; plaintext batches replay into an encrypting node */
static void test_encryption_off_and_legacy(void) {
    TEST("encrypt=off posts plaintext JSON; plaintext history replays into an encrypting node");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    /* same wallet as open_node(), through the URI, with encryption switched off */
    sqlite3 *db = hpplite_open("file:" DIR "/plain.db?hpplite=on&encrypt=off&datadir=" DIR
        "&privkey=0x0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20");
    if (!db) FAIL("open");
    HppliteNode *p = hpplite_get_node(db);
    if (!p || p->cipher != NULL) FAIL("encrypt=off not honoured");
    hpplite_node_set_l1(p, l1); hpplite_node_start(p);
    sqlite3_exec(db, "CREATE TABLE t(x); INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    if (hpplite_flush(db) != 1) FAIL("flush");
    size_t len = 0;
    uint8_t *raw = hpplite_l1_get_batch(l1, 1, &len);
    if (!raw || hpplite_envelope_is_sealed(raw, len) || !memmem(raw, len, "INSERT INTO t", 13)) FAIL("expected plaintext JSON on L1");
    free(raw);
    sqlite3_close(db);

    HppliteNode *e = open_node(DIR "/enc.db", l1);     /* encrypting node reads the plaintext history */
    if (!e) FAIL("open encrypting node");
    if (count_rows(e->db, "t") != 1) FAIL("plaintext batch not replayed");
    hpplite_node_exec(e, "INSERT INTO t VALUES(2)", NULL);
    if (hpplite_node_flush_batch(e) != 2) FAIL("flush 2");
    raw = hpplite_l1_get_batch(l1, 2, &len);
    if (!raw || !hpplite_envelope_is_sealed(raw, len)) FAIL("new batch should be sealed");
    free(raw);
    hpplite_node_destroy(e);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 21. A background flush must never make the application lose a capture */
#include <pthread.h>
typedef struct { HppliteNode *n; int rows; volatile int done; } WriterArgs;
static void *writer_thread(void *arg) {
    WriterArgs *w = arg;
    char sql[128];
    for (int i = 1; i <= w->rows; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES(%d,'r%d')", i, i);
        sqlite3_exec(w->n->db, sql, NULL, NULL, NULL);          /* autocommit, app thread */
    }
    w->done = 1;
    return NULL;
}
static void test_flush_thread_never_loses_captures(void) {
    TEST("concurrent flush thread loses no captures (issue #20)");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    hpplite_l1_mock_set_batch_size_limit(l1, 64 * 1024 * 1024);
    HppliteNode *n = open_node(DIR "/race.db", l1);
    if (!n) FAIL("open");
    n->config.checkpointInterval = 1;                         /* every flush hashes the database */
    hpplite_node_exec(n, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    hpplite_node_flush_batch(n);
    /* some bulk so the root hash (and thus the bookkeeping window) is not trivial */
    hpplite_node_exec(n, "WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i<200000)"
                          " INSERT INTO t SELECT -i, 'bulk' FROM s", NULL);
    hpplite_node_flush_batch(n);
    uint64_t h0 = hpplite_node_get_height(n);

    WriterArgs w = { n, 4000, 0 };
    pthread_t th;
    pthread_create(&th, NULL, writer_thread, &w);
    int flushes = 0;
    while (!w.done) { hpplite_node_flush_batch(n); flushes++; }  /* bookkeeping hammering the flag */
    pthread_join(th, NULL);
    uint64_t last = hpplite_node_flush_batch(n);
    if (last == 0) last = hpplite_node_get_height(n) - 1;

    int captured = hpplite_node_pending_count(n);
    for (uint64_t h = h0; h <= last; h++) {
        HppliteBatch *b = hpplite_node_fetch_batch(n, h);
        if (!b) FAIL("batch missing");
        captured += b->nTxns;
        hpplite_batch_free(b);
    }
    printf("(%d flushes, %d/%d captured) ", flushes, captured, w.rows);
    if (captured != w.rows) FAIL("captures were lost while the flusher ran");
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 22. Capture fidelity corner cases found by review */
static void test_capture_corner_cases(void) {
    TEST("_hpplite_ in a string literal, lowercase 'without rowid', long statements");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNodeConfig config;
    memset(&config, 0, sizeof(config));
    config.nodeId = "corner"; memcpy(config.privkey, PRIVKEY, 32); config.hasPrivkey = 1;
    config.dataDir = DIR; config.dbPath = DIR "/corner.db"; config.checkpointInterval = 1;
    HppliteNode *n = hpplite_node_create(&config);
    if (!n) FAIL("open");
    hpplite_node_set_l1(n, l1); hpplite_node_start(n);

    /* our table name inside user data is user data */
    hpplite_node_exec(n, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1,'see _hpplite_pending for details')", NULL);
    hpplite_node_exec(n, "INSERT INTO t VALUES(2,\"_hpplite_meta\")", NULL);
    if (hpplite_node_pending_count(n) != 3) { printf("(pending=%d) ", hpplite_node_pending_count(n)); FAIL("literal containing _hpplite_ must be captured"); }
    /* a real reference to our tables is still not captured */
    sqlite3_exec(n->db, "SELECT count(*) FROM _hpplite_pending", NULL, NULL, NULL);
    if (hpplite_node_pending_count(n) != 3) FAIL("bookkeeping reads must not be captured");

    /* WITHOUT ROWID spelled any way still hashes (lowercase broke every checkpoint before) */
    hpplite_node_exec(n, "CREATE TABLE kv(k TEXT PRIMARY KEY, v) without   rowid", NULL);
    hpplite_node_exec(n, "INSERT INTO kv VALUES('b',2),('a',1)", NULL);
    uint64_t errs = n->ctx->nLogErrors;
    if (hpplite_node_flush_batch(n) == 0) FAIL("flush with a lowercase without-rowid table");
    if (n->ctx->nLogErrors != errs) FAIL("hash error reported");
    if (hpplite_l1_mock_get_checkpoint_count(l1) != 1) FAIL("checkpoint with a without-rowid table");

    /* non-determinism is detected anywhere in the statement, not only in the first 512 bytes */
    char pad[700]; memset(pad, 'p', 650); pad[650] = 0;
    char *sql = sqlite3_mprintf("UPDATE t SET v = '%s' || random() WHERE id = -1", pad);
    uint64_t nd = n->ctx->nNondeterministic;
    hpplite_node_exec(n, sql, NULL);   /* touches no row: detected, then unlogged by effect */
    sqlite3_free(sql);
    if (n->ctx->nNondeterministic != nd + 1) FAIL("random() past byte 512 not detected");

    /* the reconstruction matches, including the without-rowid table */
    hpplite_node_flush_batch(n);
    unsigned char rootA[32], rootB[32];
    hpplite_node_get_state_root(n, rootA);
    hpplite_node_destroy(n);
    HppliteNode *r = open_node(DIR "/corner_r.db", l1);
    if (!r) FAIL("open reader");
    hpplite_node_get_state_root(r, rootB);
    if (count_rows(r->db, "t") != 2 || count_rows(r->db, "kv") != 2) FAIL("reconstructed rows");
    if (memcmp(rootA, rootB, 32) != 0) FAIL("reconstruction root differs");
    r->l1 = NULL;
    hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 23. hpplite=on that cannot be honoured fails the open instead of capturing nothing */
static void test_open_failure_is_loud(void) {
    TEST("sqlite3_open_v2 with hpplite=on and a bad key fails instead of silently skipping HPPLite");
    reset_dir();
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2("file:" DIR "/loud.db?hpplite=on&privkey=0x0000000000000000000000000000000000000000000000000000000000000000",
                             &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    if (rc == SQLITE_OK) { sqlite3_close(db); FAIL("open should fail"); }
    if (!strstr(sqlite3_errmsg(db), "hpplite")) { printf("(%s) ", sqlite3_errmsg(db)); FAIL("error should name hpplite"); }
    sqlite3_close(db);
    /* and a good key opens */
    rc = sqlite3_open_v2("file:" DIR "/loud.db?hpplite=on&privkey=0x" PRIVKEY_HEX,
                         &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    if (rc != SQLITE_OK) FAIL("open with a valid key");
    if (!hpplite_context(db)) FAIL("hpplite active");
    sqlite3_close(db);
    PASS();
}

/* 24. :memory: never gets a timer thread (it would run inside the app's transactions) */
static void test_memory_no_timer(void) {
    TEST(":memory: with an interval: no timer, transactions never see a foreign BEGIN");
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNodeConfig config;
    memset(&config, 0, sizeof(config));
    config.nodeId = "mem"; memcpy(config.privkey, PRIVKEY, 32); config.hasPrivkey = 1;
    config.dataDir = DIR; config.dbPath = ":memory:"; config.batchIntervalMs = 1;
    HppliteNode *n = hpplite_node_create(&config);
    if (!n) FAIL("open");
    hpplite_node_set_l1(n, l1);
    hpplite_node_start(n);
    if (n->timerRunning) FAIL("timer must not start for a shared bookkeeping connection");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    for (int i = 0; i < 300; i++) {
        if (sqlite3_exec(n->db, "BEGIN; INSERT INTO t VALUES(1); COMMIT", NULL, NULL, NULL) != SQLITE_OK)
            FAIL("transaction failed");
    }
    if (hpplite_node_flush_batch(n) == 0) FAIL("explicit flush");
    hpplite_node_destroy(n);
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
    test_adopt_after_lost_confirmation();
    test_adopt_on_reopen();
    test_divergence_refused();
    test_timer_vs_open_transaction();
    test_checkpoint_persists();
    test_memory_database();
    test_sync_function();
    test_batch_size_limit();
    test_slow_confirmation();
    test_replay_skips_failed_statement();
    test_encryption_default();
    test_encryption_off_and_legacy();
    test_flush_thread_never_loses_captures();
    test_capture_corner_cases();
    test_open_failure_is_loud();
    test_memory_no_timer();
    (void)!system("rm -rf " DIR);
    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
