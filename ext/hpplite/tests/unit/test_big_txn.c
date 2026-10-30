/*
** HPPLite transactions larger than one DA batch (#57)
**
** A transaction bigger than a batch is posted across several: the batch it
** starts in is marked open, the ones it continues in are marked cont. A
** replica holds the open part back and applies the transaction whole when
** its end arrives - a reader between the halves sees none of it - and drops
** it, whole, if the end never comes. The invariants:
**   - a replica never shows part of a transaction
**   - a replica rebuilt from L1 reaches the writer's root
**   - no posted batch is over the DA limit
**   - a crash between the halves loses nothing
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
    0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x4b,0x4c,0x4d,0x4e,0x4f,0x50,
    0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x5b,0x5c,0x5d,0x5e,0x5f,0x60
};

#define DIR "/tmp/hpplite_test_big_txn"
#define LIMIT 2000                       /* DA batch limit in these tests */

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

static HppliteNode *open_node(const char *dbPath, HppliteL1 *l1) {
    HppliteNodeConfig config;
    HppliteNode *node;
    memset(&config, 0, sizeof(config));
    config.nodeId = "bigtxn";
    memcpy(config.privkey, PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = DIR;
    config.dbPath = (char *)dbPath;
    config.maxBatchBytes = LIMIT;
    config.checkpointInterval = 1000;
    node = hpplite_node_create(&config);
    if (node && l1) {
        hpplite_l1_mock_set_batch_size_limit(l1, LIMIT);
        hpplite_node_set_l1(node, l1);
        hpplite_node_sync_from_l1(node, 1);
        hpplite_node_start(node);
    }
    return node;
}

/* One transaction of n inserts of ~90 bytes each (n=60: ~3 batches) */
static int big_txn(HppliteNode *n, const char *table, int base, int rows) {
    char sql[256];
    if (hpplite_node_exec(n, "BEGIN", NULL) != SQLITE_OK) return -1;
    for (int i = 0; i < rows; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES(%d, '%060d')", table, base + i, base + i);
        if (sqlite3_exec(n->db, sql, NULL, NULL, NULL) != SQLITE_OK) {
            hpplite_node_exec(n, "ROLLBACK", NULL);
            return -1;
        }
    }
    return hpplite_node_exec(n, "COMMIT", NULL);
}

/* Copy batch h of one mock L1 to the next height of another */
static int copy_batch(HppliteL1 *from, HppliteL1 *to, uint64_t h) {
    size_t len = 0;
    uint8_t *raw = hpplite_l1_get_batch(from, h, &len);
    if (!raw) return -1;
    hpplite_l1_mock_inject_batch(to, raw, len);
    free(raw);
    return 0;
}

/* A sealed batch with the given statements (one group each unless grouped) */
static uint8_t *sealed(HppliteNode *n, uint64_t height, const char **aSql, const int *aGroup, int nSql,
                       int open, int cont, size_t *pLen) {
    HppliteBatch *b = hpplite_batch_new(height);
    char *json;
    uint8_t *out;
    for (int i = 0; i < nSql; i++) hpplite_batch_add_stmt(b, aSql[i], -1, aGroup ? aGroup[i] : i);
    b->openTxn = open;
    b->contTxn = cont;
    json = hpplite_batch_to_json(b);
    out = hpplite_envelope_seal(n->cipher, n->encKey, height, HPPLITE_ENCODING_NONE,
                                (const uint8_t *)json, strlen(json), pLen);
    sqlite3_free(json);
    hpplite_batch_free(b);
    return out;
}

static int same_root(HppliteNode *a, HppliteNode *b) {
    unsigned char ra[32], rb[32];
    hpplite_node_get_state_root(a, ra);
    hpplite_node_get_state_root(b, rb);
    return memcmp(ra, rb, 32) == 0;
}

/* ------------------------------------------------------------------------ */

static void test_format(void) {
    TEST("open/cont flags: in the JSON and the block hash only when set, round-trip");
    HppliteBatch *b = hpplite_batch_new(7);
    hpplite_batch_add_stmt(b, "INSERT INTO t VALUES(1)", -1, 0);
    char *plain = hpplite_batch_to_json(b);
    unsigned char prev[32] = {0}, h0[32], h1[32], h2[32];
    hpplite_block_hash(b, prev, h0);
    if (strstr(plain, "\"open\"") || strstr(plain, "\"cont\"")) FAIL("flags written when unset");
    b->openTxn = 1;
    hpplite_block_hash(b, prev, h1);
    b->contTxn = 1;
    hpplite_block_hash(b, prev, h2);
    char *json = hpplite_batch_to_json(b);
    HppliteBatch *p = hpplite_batch_from_json(json);
    int ok = p && p->openTxn == 1 && p->contTxn == 1 && strstr(json, "\"open\":1") && strstr(json, "\"cont\":1");
    HppliteBatch *q = hpplite_batch_from_json(plain);
    int okPlain = q && q->openTxn == 0 && q->contTxn == 0;
    sqlite3_free(plain); sqlite3_free(json);
    hpplite_batch_free(b); hpplite_batch_free(p); hpplite_batch_free(q);
    if (!ok) FAIL("flags did not round-trip");
    if (!okPlain) FAIL("a batch without flags parsed with flags");
    if (memcmp(h0, h1, 32) == 0 || memcmp(h1, h2, 32) == 0) FAIL("flags not in the block hash");
    PASS();
}

static void test_spans_batches(void) {
    TEST("a transaction three batches long is posted, marked open/cont, and replays whole");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/w.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT)", NULL);
    hpplite_node_exec(w, "CREATE TABLE s(id INTEGER PRIMARY KEY, v TEXT)", NULL);
    hpplite_node_exec(w, "INSERT INTO s VALUES(1, 'before')", NULL);
    if (big_txn(w, "t", 1000, 60) != SQLITE_OK) FAIL("the big transaction was refused at capture");
    hpplite_node_exec(w, "INSERT INTO s VALUES(2, 'after')", NULL);
    uint64_t last = hpplite_node_flush_batch(w);
    if (last < 4) FAILF("expected the transaction over 3+ batches, last height %llu", (unsigned long long)last);
    if (hpplite_node_pending_count(w) != 0) FAIL("pending left");
    int nOpen = 0, nCont = 0;
    for (uint64_t h = 1; h <= last; h++) {
        size_t len = 0;
        uint8_t *raw = hpplite_l1_get_batch(l1, h, &len);
        free(raw);
        if (len > LIMIT) FAILF("batch %llu is %zu bytes, over %d", (unsigned long long)h, len, LIMIT);
        HppliteBatch *b = hpplite_node_fetch_batch(w, h);
        if (!b) FAIL("fetch");
        nOpen += b->openTxn; nCont += b->contTxn;
        hpplite_batch_free(b);
    }
    if (nOpen < 2 || nCont < 2 || nOpen != nCont) FAILF("open %d / cont %d marks", nOpen, nCont);
    HppliteNode *r = open_node(DIR "/r.db", l1);
    if (!r) FAIL("replica");
    if (r->state == HPPLITE_STATE_ERROR) FAIL("replica sync failed");
    if (count_rows(r->db, "t") != 60 || count_rows(r->db, "s") != 2) FAIL("replica rows");
    if (!same_root(w, r)) FAIL("replica root differs");
    if (r->ctx->nReplaySkipped || r->ctx->nTxnsDropped) FAIL("replica skipped or dropped something");
    hpplite_node_destroy(r);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    PASS();
}

static void test_reader_between_halves(void) {
    TEST("a replica between the halves shows none of the transaction, across its own reopen");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/w.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT)", NULL);
    hpplite_node_exec(w, "CREATE TABLE s(id INTEGER PRIMARY KEY, v TEXT)", NULL);
    if (hpplite_node_flush_batch(w) != 1) FAIL("batch 1");
    hpplite_node_exec(w, "INSERT INTO s VALUES(1, 'before')", NULL);
    if (big_txn(w, "t", 1000, 60) != SQLITE_OK) FAIL("big transaction refused");
    hpplite_node_exec(w, "INSERT INTO s VALUES(2, 'after')", NULL);
    uint64_t last = hpplite_node_flush_batch(w);
    if (last < 4) FAIL("not split");

    HppliteL1 *l2 = hpplite_l1_create_mock();
    hpplite_l1_mock_set_batch_size_limit(l2, LIMIT);
    for (uint64_t h = 1; h <= last; h++) {
        if (copy_batch(l1, l2, h) != 0) FAIL("copy");
        HppliteNode *r = open_node(DIR "/r.db", l2);        /* reopened every time: the held part is on disk */
        if (!r || r->state == HPPLITE_STATE_ERROR) FAIL("replica");
        int rows = count_rows(r->db, "t");
        uint64_t height = hpplite_node_get_height(r);
        int done = (h == last);
        hpplite_node_destroy(r);
        if (height != h + 1) FAILF("replica at height %llu after %llu batches", (unsigned long long)height, (unsigned long long)h);
        /* all or nothing at every height; nothing while the first half is all there is */
        if (rows != 0 && rows != 60) FAILF("partial transaction visible: %d rows after batch %llu", rows, (unsigned long long)h);
        if (h == 2 && rows != 0) FAIL("rows visible after the open batch alone");
        if (done && rows != 60) FAILF("transaction incomplete at the end: %d rows", rows);
    }
    HppliteNode *r = open_node(DIR "/r.db", l2);
    if (!r) FAIL("replica final");
    if (!same_root(w, r)) FAIL("replica root differs");
    if (count_rows(r->db, "s") != 2) FAIL("small transactions");
    hpplite_node_destroy(r);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    hpplite_l1_disconnect(l2);
    PASS();
}

static void test_crash_between_halves(void) {
    TEST("the writer dies after posting the first half: reopen posts the rest, nothing lost");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/w.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT)", NULL);
    if (hpplite_node_flush_batch(w) != 1) FAIL("batch 1");
    if (big_txn(w, "t", 1000, 60) != SQLITE_OK) FAIL("big transaction refused");
    hpplite_l1_mock_set_fail_submit_after(l1, 1);          /* the open half lands, the rest does not */
    hpplite_node_flush_batch(w);
    int pendingAfter = hpplite_node_pending_count(w);
    hpplite_node_destroy(w);                                /* "crash": the rest is in the log */
    if (pendingAfter == 0) FAIL("test premise: everything was posted");
    HppliteBatch *b = NULL;
    {
        HppliteNode *peek = open_node(DIR "/peek.db", NULL);
        hpplite_node_set_l1(peek, l1);
        b = hpplite_node_fetch_batch(peek, 2);
        int open = b && b->openTxn;
        hpplite_batch_free(b);
        hpplite_node_destroy(peek);
        if (!open) FAIL("test premise: batch 2 is not open");
    }
    HppliteNode *mid = open_node(DIR "/mid.db", l1);
    if (!mid) FAIL("mid replica");
    int midRows = count_rows(mid->db, "t");
    hpplite_node_destroy(mid);
    if (midRows != 0) FAILF("a replica shows %d rows of a transaction whose end is not posted", midRows);

    hpplite_l1_mock_set_fail_submit(l1, 0);
    w = open_node(DIR "/w.db", l1);
    if (!w) FAIL("reopen writer");
    hpplite_node_flush_batch(w);
    if (hpplite_node_pending_count(w) != 0) FAIL("rest not posted");
    HppliteNode *r = open_node(DIR "/r.db", l1);
    if (!r) FAIL("replica");
    if (count_rows(r->db, "t") != 60) FAILF("replica has %d of 60 rows", count_rows(r->db, "t"));
    if (!same_root(w, r)) FAIL("replica root differs");
    hpplite_node_destroy(r);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    PASS();
}

static void test_lost_confirmation_of_open_batch(void) {
    TEST("the open half landed but its confirmation was lost: adopted, and the rest continues it");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/w.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT)", NULL);
    if (hpplite_node_flush_batch(w) != 1) FAIL("batch 1");
    if (big_txn(w, "t", 1000, 60) != SQLITE_OK) FAIL("big transaction refused");
    hpplite_l1_mock_set_lose_confirmation(l1, 1);
    hpplite_node_flush_batch(w);
    hpplite_l1_mock_set_lose_confirmation(l1, 0);
    hpplite_node_destroy(w);
    w = open_node(DIR "/w.db", l1);                          /* adopts what landed */
    if (!w || w->state == HPPLITE_STATE_ERROR) FAIL("reopen writer");
    hpplite_node_flush_batch(w);
    if (hpplite_node_pending_count(w) != 0) FAIL("rest not posted");
    HppliteNode *r = open_node(DIR "/r.db", l1);
    if (!r) FAIL("replica");
    if (count_rows(r->db, "t") != 60) FAILF("replica has %d of 60 rows", count_rows(r->db, "t"));
    if (!same_root(w, r)) FAIL("replica root differs");
    hpplite_node_destroy(r);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    PASS();
}

static void test_missing_continuation(void) {
    TEST("an open batch whose continuation never comes: the held part is dropped whole");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    hpplite_l1_mock_set_batch_size_limit(l1, LIMIT);
    HppliteNode *w = open_node(DIR "/w.db", l1);             /* only for its key and lease */
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT)", NULL);
    if (hpplite_node_flush_batch(w) != 1) FAIL("batch 1");
    const char *a2[] = { "INSERT INTO t VALUES(1, 'kept')", "INSERT INTO t VALUES(2, 'half')", "INSERT INTO t VALUES(3, 'half')" };
    const int g2[] = { 0, 1, 1 };
    const char *a3[] = { "INSERT INTO t VALUES(4, 'next writer')" };
    const char *a4[] = { "INSERT INTO t VALUES(5, 'orphan tail')", "INSERT INTO t VALUES(6, 'own')" };
    const int g4[] = { 0, 1 };
    size_t len;
    uint8_t *b;
    b = sealed(w, 2, a2, g2, 3, 1, 0, &len); hpplite_l1_mock_inject_batch(l1, b, len); free(b);
    b = sealed(w, 3, a3, NULL, 1, 0, 0, &len); hpplite_l1_mock_inject_batch(l1, b, len); free(b);
    /* a continuation with nothing held: its head was dropped, so is it */
    b = sealed(w, 4, a4, g4, 2, 0, 1, &len); hpplite_l1_mock_inject_batch(l1, b, len); free(b);
    hpplite_node_destroy(w);
    HppliteNode *r = open_node(DIR "/r.db", l1);
    if (!r || r->state == HPPLITE_STATE_ERROR) FAIL("replica");
    int rows = count_rows(r->db, "t");
    sqlite3_int64 has2 = 0, has5 = 0;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(r->db, "SELECT sum(id=2 OR id=3), sum(id=5) FROM t", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) { has2 = sqlite3_column_int64(st, 0); has5 = sqlite3_column_int64(st, 1); }
        sqlite3_finalize(st);
    }
    uint64_t dropped = r->ctx->nTxnsDropped;
    hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    if (has2) FAIL("half a transaction was applied");
    if (has5) FAIL("a continuation without its head was applied");
    if (rows != 3) FAILF("expected rows 1, 4 and 6, got %d rows", rows);
    if (dropped != 2) FAILF("dropped counter %llu, expected 2", (unsigned long long)dropped);
    PASS();
}

static void test_multi_connection_groups(void) {
    TEST("a big transaction between small ones from a second connection keeps every group whole");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/w.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT)", NULL);
    hpplite_node_exec(w, "CREATE TABLE s(id INTEGER PRIMARY KEY, v TEXT)", NULL);
    sqlite3 *f = NULL;
    if (sqlite3_open_v2("file:" DIR "/w.db?hpplite=on&datadir=" DIR, &f,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, NULL) != SQLITE_OK) FAIL("follower");
    sqlite3_busy_timeout(f, 5000);
    for (int k = 0; k < 3; k++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "BEGIN; INSERT INTO s VALUES(%d, 'a'); INSERT INTO s VALUES(%d, 'b'); COMMIT;", 10 * k, 10 * k + 1);
        if (sqlite3_exec(f, sql, NULL, NULL, NULL) != SQLITE_OK) FAIL("follower write");
        if (big_txn(w, "t", 1000 * (k + 1), 30) != SQLITE_OK) FAIL("big transaction refused");
    }
    sqlite3_close(f);
    hpplite_node_flush_batch(w);
    if (hpplite_node_pending_count(w) != 0) FAIL("pending left");
    HppliteNode *r = open_node(DIR "/r.db", l1);
    if (!r) FAIL("replica");
    if (count_rows(r->db, "t") != 90 || count_rows(r->db, "s") != 6) FAIL("replica rows");
    if (!same_root(w, r)) FAIL("replica root differs");
    hpplite_node_destroy(r);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    PASS();
}

int main(void) {
    printf("=== HPPLite transactions across batches (#57) ===\n");
    test_format();
    test_spans_batches();
    test_reader_between_halves();
    test_crash_between_halves();
    test_lost_confirmation_of_open_batch();
    test_missing_continuation();
    test_multi_connection_groups();
    printf("\n%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
