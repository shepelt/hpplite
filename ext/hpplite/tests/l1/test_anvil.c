/*
** HPPLite against the real contracts on a local anvil chain (issue #47).
** Driven by tests/l1/anvil_test.sh, which provides HPPLITE_ANVIL_RPC,
** HPPLITE_ANVIL_FACTORY, HPPLITE_ANVIL_KEY0 (our wallet) and
** HPPLITE_ANVIL_KEY1 (a stranger). Time travel through `cast rpc`.
**
** What the mock cannot show: real claim/renew/release transactions and
** receipts, lease expiry by chain time, a second instance of our own
** wallet, a stranger's claim after expiry, sleeping for days, and (#51)
** that what capture accepts survives the real path: sealed, posted to
** contract storage at the size limit, replayed by a fresh replica.
*/
#include "hpplite.h"
#include "node.h"
#include "l1_interface.h"
#include "eth/eth_client.h"
#include "batch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#define DIR "/tmp/hpplite_anvil"
static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { printf("  Testing: %s... ", name); fflush(stdout); tests_run++; } while (0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while (0)

static const char *RPC, *FACTORY, *KEY0, *KEY1, *KEY2;

static void chain_sleep(int seconds) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "cast rpc evm_increaseTime %d --rpc-url %s >/dev/null && cast rpc evm_mine --rpc-url %s >/dev/null", seconds, RPC, RPC);
    (void)!system(cmd);
}

static sqlite3 *open_db(const char *file, const char *key, const char *extra) {
    char uri[1024];
    sqlite3 *db = NULL;
    snprintf(uri, sizeof(uri), "file:" DIR "/%s?hpplite=on&rpc=%s&factory=%s&chainid=31337&privkey=%s&datadir=" DIR "&interval=0%s",
             file, RPC, FACTORY, key, extra ? extra : "");
    if (sqlite3_open_v2(uri, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL) != SQLITE_OK) {
        fprintf(stderr, "open: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

static sqlite3_int64 scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s; sqlite3_int64 v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return v;
}

static void dump_state(HppliteL1 *l1, const unsigned char *mine, const char *why) {
    HppliteL1State *st = hpplite_l1_get_state(l1);
    if (!st) { printf("[%s: no state] ", why); return; }
    printf("\n    [%s] holder=", why); for (int i = 0; i < 12; i++) printf("%02x", st->sequencerInstance[i]);
    printf(" mine="); for (int i = 0; i < 12; i++) printf("%02x", mine[i]);
    printf(" expiry=%llu chainNow=%llu wall=%llu\n",
           (unsigned long long)st->leaseExpiry, (unsigned long long)st->currentTimestamp, (unsigned long long)time(NULL));
    hpplite_l1_state_free(st);
}

static int lease_is(HppliteL1 *l1, const unsigned char *instanceId) {
    HppliteL1State *st = hpplite_l1_get_state(l1);
    int ours;
    if (!st) return -1;
    ours = st->leaseExpiry > st->currentTimestamp && memcmp(st->sequencerInstance, instanceId, 32) == 0;
    hpplite_l1_state_free(st);
    return ours;
}

/* 1. The documented quick start, end to end on a real chain */
static void test_open_write_post_checkpoint(void) {
    TEST("open via factory, write, batch on chain, checkpoint anchored");
    sqlite3 *db = open_db("t1.db", KEY0, "&checkpoint=1");
    if (!db) FAIL("open");
    HppliteNode *n = hpplite_get_node(db);
    if (!n || !n->l1) FAIL("no L1 after open with factory");
    sqlite3_exec(db, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL, NULL, NULL);
    if (scalar(db, "SELECT hpplite_flush()") != 1) FAIL("flush should post batch 1");
    if (scalar(db, "SELECT height FROM hpplite_status") != 2) FAIL("height 2");
    if (scalar(db, "SELECT last_checkpoint_height FROM hpplite_status") != 1) FAIL("checkpoint 1 on chain");
    if (scalar(db, "SELECT lease FROM hpplite_status") != 1) FAIL("lease held");
    char c[43], d[43]; hpplite_l1_get_addresses(n->l1, c, d);
    unsigned char inst[32]; memcpy(inst, n->instanceId, 32);
    sqlite3_close(db);
    HppliteL1 *peek = hpplite_l1_connect(RPC, c);
    if (lease_is(peek, inst) != 0) { dump_state(peek, inst, "after close"); hpplite_l1_disconnect(peek); FAIL("close must release the lease"); }
    hpplite_l1_disconnect(peek);
    PASS();
}

/* 2. A second instance of our own wallet must not disturb the running sequencer (issue #40) */
static void test_second_instance_same_wallet(void) {
    TEST("second instance, same wallet: claim reverts, close does not expire the live lease");
    sqlite3 *a = open_db("t2a.db", KEY0, NULL);
    if (!a) FAIL("open a");
    HppliteNode *na = hpplite_get_node(a);
    sqlite3_exec(a, "INSERT INTO t VALUES(2)", NULL, NULL, NULL);
    sqlite3_int64 h0 = scalar(a, "SELECT hpplite_flush()");
    if (h0 == 0) { dump_state(na->l1, na->instanceId, "a's flush"); sqlite3_close(a); FAIL("a posts"); }
    unsigned char instA[32]; memcpy(instA, na->instanceId, 32);
    if (lease_is(na->l1, instA) != 1) FAIL("a holds the lease");

    /* the operator mistake: the same key opened again elsewhere (the C API claims at open) */
    char uri[1024];
    snprintf(uri, sizeof(uri), "file:" DIR "/t2b.db?hpplite=on&rpc=%s&factory=%s&chainid=31337&privkey=%s&datadir=" DIR "&interval=0", RPC, FACTORY, KEY0);
    sqlite3 *b = hpplite_open(uri);
    if (!b) FAIL("open b");
    HppliteNode *nb = hpplite_get_node(b);
    int bad = nb->hasLease;
    sqlite3_exec(b, "INSERT INTO t VALUES(99)", NULL, NULL, NULL);
    sqlite3_int64 hb = scalar(b, "SELECT hpplite_flush()");
    sqlite3_close(b);                                   /* must not forceExpireLease */
    int stillA = lease_is(na->l1, instA);
    sqlite3_exec(a, "INSERT INTO t VALUES(3)", NULL, NULL, NULL);
    sqlite3_int64 ha = scalar(a, "SELECT hpplite_flush()");
    sqlite3_close(a);
    if (bad) FAIL("b believes it holds a lease that a holds (blind claim, #40)");
    if (hb != 0) FAIL("b must not post into a's lease");
    if (stillA != 1) FAIL("a's lease was cut by b's close (#40)");
    if (ha == 0) FAIL("a keeps posting");
    PASS();
}

/* 3. Sleep for days, wake, write: one claim, then business as usual */
/* 5. The application calls into L1 (hpplite_get_l1) while the timer thread flushes: one curl handle */
static void *app_thread_rpcs(void *arg) {
    HppliteL1 *l1 = arg;
    for (int i = 0; i < 40; i++) {
        HppliteL1State *st = hpplite_l1_get_state(l1);
        if (st) hpplite_l1_state_free(st);
        HppliteSystemConfig *c = hpplite_l1_get_system_config(l1);
        if (c) hpplite_l1_system_config_free(c);
    }
    return NULL;
}
static void test_concurrent_rpc(void) {
    TEST("application RPCs through hpplite_get_l1() while the timer flushes (shared curl handle)");
    sqlite3 *a = open_db("t5.db", KEY0, "&interval=200ms");
    if (!a) FAIL("open");
    HppliteNode *na = hpplite_get_node(a);
    sqlite3_exec(a, "CREATE TABLE IF NOT EXISTS t(x)", NULL, NULL, NULL);
    pthread_t th;
    pthread_create(&th, NULL, app_thread_rpcs, na->l1);
    for (int i = 0; i < 20; i++) {
        sqlite3_exec(a, "INSERT INTO t VALUES(7)", NULL, NULL, NULL);
        usleep(100000);
    }
    pthread_join(th, NULL);
    if (na->state == HPPLITE_STATE_ERROR) FAIL("node in error");
    sqlite3_close(a);
    PASS();
}

static void test_sleep_and_wake(void) {
    TEST("lease expired by chain time after a long sleep: wake, claim, post");
    sqlite3 *a = open_db("t3.db", KEY0, NULL);
    if (!a) FAIL("open");
    chain_sleep(3 * 24 * 3600);
    HppliteNode *na = hpplite_get_node(a);
    if (hpplite_l1_is_lease_active(na->l1)) FAIL("lease should have expired");
    sqlite3_exec(a, "INSERT INTO t VALUES(4)", NULL, NULL, NULL);
    if (scalar(a, "SELECT hpplite_flush()") == 0) FAIL("flush after sleep");
    if (lease_is(na->l1, na->instanceId) != 1) FAIL("lease re-claimed");
    sqlite3_close(a);
    PASS();
}

/* 4. A stranger after expiry: what the contract allows today (#48 will make this revert) */
static void test_stranger_after_expiry(void) {
    TEST("stranger claims after expiry: node must not fork and must say so");
    sqlite3 *a = open_db("t4.db", KEY0, NULL);
    if (!a) FAIL("open");
    HppliteNode *na = hpplite_get_node(a);
    char c[43], d[43];
    hpplite_l1_get_addresses(na->l1, c, d);
    chain_sleep(301);
    HppliteL1 *stranger = hpplite_l1_connect(RPC, c);
    unsigned char k1[32]; eth_hex_decode(KEY1, k1, 32);
    hpplite_l1_set_privkey(stranger, k1);
    unsigned char inst[32]; memset(inst, 0xEE, 32);
    char *tx = hpplite_l1_claim_sequencer(stranger, inst);
    int claimed = tx && hpplite_l1_wait_for_tx(stranger, tx, 30) == 1;
    sqlite3_free(tx);
    printf("(stranger claim %s) ", claimed ? "ACCEPTED (old contract)" : "rejected");
    if (claimed) FAIL("the contract let a stranger claim the lease (#48)");
    sqlite3_exec(a, "INSERT INTO t VALUES(5)", NULL, NULL, NULL);
    uint64_t h = (uint64_t)scalar(a, "SELECT hpplite_flush()");
    if (claimed) {
        /* with the old contract: our node must keep the row pending, not post, not error */
        if (h != 0) FAIL("posted into a stranger's lease");
        if (scalar(a, "SELECT pending FROM hpplite_status") != 1) FAIL("row must stay pending");
        chain_sleep(301);
        if (scalar(a, "SELECT hpplite_flush()") == 0) FAIL("after the stranger's lease expires, we post again");
    } else if (h == 0) {
        FAIL("flush");
    }
    hpplite_l1_disconnect(stranger);
    sqlite3_close(a);
    PASS();
}


/* 6. Edge-case SQL and full-size batches end to end (#51). KEY2's own,
** fresh rollup: writer posts, a replica on another file rebuilds from the
** chain, roots must match, every posted batch within the contract's limit. */
static const char *aAllowed[] = {
    "CREATE TABLE e(id INTEGER PRIMARY KEY, v)",
    "INSERT INTO e(v) VALUES('random()')",
    "INSERT INTO e(v) VALUES('now')",
    "INSERT INTO e(v) VALUES('datetime(''now'')')",
    "INSERT INTO e(v) VALUES('CURRENT_TIMESTAMP')",
    "INSERT INTO e(v) VALUES('DELETE FROM _hpplite_pending')",
    "INSERT INTO e(v) VALUES('it''s now or never')",
    "INSERT INTO e(v) VALUES(upper('now'))",
    "INSERT INTO e(v) VALUES(datetime('2024-01-01 00:00:00'))",
    "INSERT INTO e(v) VALUES(date('2024-01-01', '+1 day'))",
    "INSERT INTO e(v) VALUES(strftime('%Y-%m-%d %H', '2024-02-29 13:00'))",
    "INSERT INTO e(v) VALUES(unixepoch('2024-01-01'))",
    "INSERT INTO e(v) VALUES(julianday('2024-01-01'))",
    "SELECT count(*) FROM _hpplite_pending",                  /* reads are fine; a write that reads the log is refused */
    "PRAGMA foreign_keys = OFF",
    "INSERT INTO e(v) VALUES('back\\slash \"double\" ''single'' tab\there')",
    "INSERT INTO e(v) VALUES('line one' || char(10) || 'line two' || char(13) || char(1) || char(31) || char(127))",
    "INSERT INTO e(v) VALUES('한국어 日本語 emoji 🚀🔥 ünïcödé')",
    "INSERT INTO e(v) VALUES(char(55357, 56832))",
    "INSERT INTO e(v) VALUES(x'00ff10ab')",
    "INSERT INTO e(v) VALUES(zeroblob(64))",
    "INSERT INTO e(v) VALUES(9223372036854775807)",
    "INSERT INTO e(v) VALUES(-9223372036854775808)",
    "INSERT INTO e(v) VALUES(1e308)",
    "INSERT INTO e(v) VALUES(NULL)",
    "INSERT INTO e(v) VALUES('')",
    "UPDATE e SET v = v || '/' WHERE typeof(v) = 'text'",
    "CREATE TABLE \"we\"\"ird \"\"name\"(\"col \"\"x\"\"\")",
    "INSERT INTO \"we\"\"ird \"\"name\" VALUES('}]\\\",{\"sql\":\"DROP TABLE e\"}')",
    NULL
};

/* An INSERT whose posted (JSON-escaped) size is exactly `target` bytes:
** an escape-heavy head, then plain padding one byte at a time. */
static char *sized_insert(size_t target) {
    size_t cap = target + 64, at;
    char *sql = malloc(cap);
    at = (size_t)snprintf(sql, cap, "INSERT INTO big VALUES('");
    for (int i = 0; i < 200; i++) { sql[at++] = '"'; sql[at++] = '\\'; sql[at++] = '\n'; }
    while (at + 3 < cap) {
        memcpy(sql + at, "')", 3);                      /* close the statement to measure it */
        size_t sz = hpplite_batch_stmt_json_size(sql, -1, 0);
        if (sz == target) return sql;
        if (sz > target) break;
        sql[at++] = 'a';                                /* plain byte: +1 posted byte */
    }
    free(sql);
    return NULL;
}

static void test_edge_sql_end_to_end(void) {
    TEST("edge-case SQL and full-size batches: posted on the real contract, replica root matches");
    sqlite3 *w = open_db("e_writer.db", KEY2, NULL);
    if (!w) FAIL("open writer");
    HppliteNode *nw = hpplite_get_node(w);
    if (!nw || !nw->l1 || !nw->ctx) FAIL("writer has no L1");
    HppliteSystemConfig *cfg = hpplite_l1_get_system_config(nw->l1);
    if (!cfg) FAIL("system config");
    size_t limit = (size_t)cfg->batchSizeLimit;
    hpplite_l1_system_config_free(cfg);
    size_t stmtLimit = nw->ctx->maxStmtBytes;
    if (limit == 0 || stmtLimit == 0 || stmtLimit >= limit) FAIL("no batch limit learned from the contract");

    for (int i = 0; aAllowed[i]; i++) {
        char *err = NULL;
        if (sqlite3_exec(w, aAllowed[i], NULL, NULL, &err) != SQLITE_OK) {
            printf("[%s: %s] ", aAllowed[i], err ? err : "?"); sqlite3_free(err); sqlite3_close(w); FAIL("an allowed statement was refused");
        }
    }
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); FAIL("corpus batch not posted"); }

    /* one statement at exactly the per-statement limit, alone in its batch:
    ** the largest batch the node will ever post, on the real contract */
    sqlite3_exec(w, "CREATE TABLE big(v)", NULL, NULL, NULL);
    if (scalar(w, "SELECT hpplite_flush()") <= 0) { sqlite3_close(w); FAIL("flush before the big statement"); }
    char *atLimit = sized_insert(stmtLimit);
    char *overLimit = sized_insert(stmtLimit + 1);
    if (!atLimit || !overLimit) { sqlite3_close(w); FAIL("could not build statements of the target size"); }
    int rcOver = sqlite3_exec(w, overLimit, NULL, NULL, NULL);
    int rcAt = sqlite3_exec(w, atLimit, NULL, NULL, NULL);
    free(atLimit); free(overLimit);
    if (rcOver == SQLITE_OK) { sqlite3_close(w); FAIL("a statement one byte over the limit was accepted"); }
    if (rcAt != SQLITE_OK) { sqlite3_close(w); FAIL("a statement exactly at the limit was refused"); }
    uint64_t hBig = (uint64_t)scalar(w, "SELECT hpplite_flush()");
    if (hBig == 0) { dump_state(nw->l1, nw->instanceId, "big flush"); sqlite3_close(w); FAIL("the contract did not take a batch at the size limit (gas or size)"); }
    size_t bigLen = 0;
    uint8_t *raw = hpplite_l1_get_batch(nw->l1, hBig, &bigLen);
    free(raw);
    printf("(limit %zu, largest batch %zu) ", limit, bigLen);
    if (bigLen > limit) { sqlite3_close(w); FAIL("posted batch over the contract's limit"); }
    if (bigLen < limit - limit / 20) { sqlite3_close(w); FAIL("the at-limit batch is not near the limit: the test is not exercising it"); }

    /* a backlog of escape-heavy rows that fills several batches */
    uint64_t hBefore = (uint64_t)scalar(w, "SELECT height FROM hpplite_status");
    sqlite3_exec(w, "CREATE TABLE fill(i, v)", NULL, NULL, NULL);
    char row[1200];
    for (int i = 0; i < 300; i++) {
        int at = snprintf(row, sizeof(row), "INSERT INTO fill VALUES(%d, '", i);
        for (int k = 0; k < 250; k++) { row[at++] = (k % 3) ? 'q' : '"'; row[at++] = (k % 5) ? 'z' : '\\'; }
        snprintf(row + at, sizeof(row) - (size_t)at, " %d🚀')", i);
        if (sqlite3_exec(w, row, NULL, NULL, NULL) != SQLITE_OK) { sqlite3_close(w); FAIL("fill insert"); }
    }
    uint64_t hLast = (uint64_t)scalar(w, "SELECT hpplite_flush()");
    /* hBefore is the next height to post, so the backlog is hBefore..hLast */
    if (hLast < hBefore) { sqlite3_close(w); FAIL("backlog not posted"); }
    if (hLast == hBefore) { sqlite3_close(w); FAIL("the backlog fit in one batch: the test is not exercising the cut"); }
    for (uint64_t h = hBefore; h <= hLast; h++) {
        size_t len = 0;
        uint8_t *b = hpplite_l1_get_batch(nw->l1, h, &len);
        free(b);
        if (len > limit) { sqlite3_close(w); FAIL("a backlog batch is over the contract's limit"); }
    }
    if (scalar(w, "SELECT pending FROM hpplite_status") != 0) { sqlite3_close(w); FAIL("pending left after flush"); }

    unsigned char rootW[32], rootR[32];
    hpplite_state_root(w, rootW);
    sqlite3_int64 rowsW = scalar(w, "SELECT (SELECT count(*) FROM e) + (SELECT count(*) FROM big) + (SELECT count(*) FROM fill)");
    sqlite3_int64 heightW = scalar(w, "SELECT height FROM hpplite_status");
    sqlite3_close(w);                       /* releases the lease */

    sqlite3 *r = open_db("e_replica.db", KEY2, NULL);
    if (!r) FAIL("open replica");
    if (scalar(r, "SELECT height FROM hpplite_status") != heightW) { sqlite3_close(r); FAIL("replica did not sync to the writer's height"); }
    hpplite_state_root(r, rootR);
    sqlite3_int64 rowsR = scalar(r, "SELECT (SELECT count(*) FROM e) + (SELECT count(*) FROM big) + (SELECT count(*) FROM fill)");
    sqlite3_int64 bigR = scalar(r, "SELECT length(v) FROM big");
    sqlite3_close(r);
    if (rowsR != rowsW) FAIL("replica row count differs");
    if (bigR <= 0) FAIL("the at-limit row is missing on the replica");
    if (memcmp(rootW, rootR, 32) != 0) FAIL("replica state root differs from the writer's");
    PASS();
}

int main(void) {
    RPC = getenv("HPPLITE_ANVIL_RPC"); FACTORY = getenv("HPPLITE_ANVIL_FACTORY");
    KEY0 = getenv("HPPLITE_ANVIL_KEY0"); KEY1 = getenv("HPPLITE_ANVIL_KEY1"); KEY2 = getenv("HPPLITE_ANVIL_KEY2");
    if (!RPC || !FACTORY || !KEY0 || !KEY1 || !KEY2) { printf("run through tests/l1/anvil_test.sh\n"); return 77; }
    (void)!system("rm -rf " DIR " && mkdir -p " DIR);
    printf("=== HPPLite on anvil (%s) ===\n", RPC);
    test_open_write_post_checkpoint();
    test_second_instance_same_wallet();
    test_concurrent_rpc();
    test_sleep_and_wake();
    test_stranger_after_expiry();
    test_edge_sql_end_to_end();
    printf("=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    (void)!system("rm -rf " DIR);
    return tests_passed == tests_run ? 0 : 1;
}
