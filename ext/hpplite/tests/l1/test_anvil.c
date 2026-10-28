/*
** HPPLite against the real contracts on a local anvil chain (issue #47).
** Driven by tests/l1/anvil_test.sh, which provides HPPLITE_ANVIL_RPC,
** HPPLITE_ANVIL_FACTORY, HPPLITE_ANVIL_KEY0 (our wallet) and
** HPPLITE_ANVIL_KEY1 (a stranger). Time travel through `cast rpc`.
**
** What the mock cannot show: real claim/renew/release transactions and
** receipts, lease expiry by chain time, a second instance of our own
** wallet, a stranger's claim after expiry, sleeping for days.
*/
#include "hpplite.h"
#include "node.h"
#include "l1_interface.h"
#include "eth/eth_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define DIR "/tmp/hpplite_anvil"
static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { printf("  Testing: %s... ", name); fflush(stdout); tests_run++; } while (0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while (0)

static const char *RPC, *FACTORY, *KEY0, *KEY1;

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
    printf("(stranger claim %s) ", claimed ? "ACCEPTED by the current contract" : "rejected");
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

int main(void) {
    RPC = getenv("HPPLITE_ANVIL_RPC"); FACTORY = getenv("HPPLITE_ANVIL_FACTORY");
    KEY0 = getenv("HPPLITE_ANVIL_KEY0"); KEY1 = getenv("HPPLITE_ANVIL_KEY1");
    if (!RPC || !FACTORY || !KEY0 || !KEY1) { printf("run through tests/l1/anvil_test.sh\n"); return 77; }
    (void)!system("rm -rf " DIR " && mkdir -p " DIR);
    printf("=== HPPLite on anvil (%s) ===\n", RPC);
    test_open_write_post_checkpoint();
    test_second_instance_same_wallet();
    test_sleep_and_wake();
    test_stranger_after_expiry();
    printf("=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    (void)!system("rm -rf " DIR);
    return tests_passed == tests_run ? 0 : 1;
}
