/*
** HPPLite row capture end to end (#19)
**
** capture=rows: a writer's records travel the whole way - log, batch, mock
** L1, sync - and a replica rebuilt from L1 alone must reach the writer's
** rows and state root. The cases are the ones statement capture had to
** refuse (non-deterministic values, application functions, the machine's
** time zone) and the ones records change the shape of (plaintext batches,
** size cuts, statements larger than a batch, hooks replaced by another
** component). What stays refused stays refused: ATTACH.
**
** test_rows.c checks capture and apply on one connection; this file checks
** the pipeline.
*/

#include "node.h"
#include "batch.h"
#include "cipher.h"
#include "capture_rows.h"
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
    0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,0x70,
    0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x7b,0x7c,0x7d,0x7e,0x7f,0x80
};

#define DIR "/tmp/hpplite_test_rows_e2e"

static void reset_dir(void) {
    (void)!system("rm -rf " DIR " && mkdir -p " DIR);
}

static sqlite3_int64 scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s;
    sqlite3_int64 v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return v;
}

static HppliteNode *open_node_ex(const char *dbPath, HppliteL1 *l1, size_t maxBatch, int noEncrypt) {
    HppliteNodeConfig config;
    HppliteNode *node;
    memset(&config, 0, sizeof(config));
    config.nodeId = "rows";
    memcpy(config.privkey, PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = DIR;
    config.dbPath = (char *)dbPath;
    config.checkpointInterval = 1000;
    config.maxBatchBytes = maxBatch;
    config.noEncrypt = noEncrypt;
    config.captureRows = 1;
    node = hpplite_node_create(&config);
    if (node && l1) {
        /* the DA limit is the contract's; the mock stands in for it */
        if (maxBatch) hpplite_l1_mock_set_batch_size_limit(l1, maxBatch);
        hpplite_node_set_l1(node, l1);
        hpplite_node_sync_from_l1(node, 1);
        hpplite_node_start(node);
    }
    return node;
}
static int g_plain;                     /* replicas of an encrypt=off history read plaintext */
static HppliteNode *open_node(const char *dbPath, HppliteL1 *l1) { return open_node_ex(dbPath, l1, 0, g_plain); }

static int run(HppliteNode *n, const char **aSql) {
    for (int i = 0; aSql[i]; i++) {
        char *err = NULL;
        if (sqlite3_exec(n->db, aSql[i], NULL, NULL, &err) != SQLITE_OK) {
            printf("[%s: %s] ", aSql[i], err ? err : "?");
            sqlite3_free(err);
            return -1;
        }
    }
    return 0;
}

/* Every row of every table (sqlite_sequence included), quoted, in order */
static char *dump(sqlite3 *db) {
    sqlite3_str *out = sqlite3_str_new(NULL);
    sqlite3_stmt *t;
    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE '\\_hpplite\\_%' ESCAPE '\\' "
                               "AND name NOT LIKE '%\\_data' ESCAPE '\\' AND name NOT LIKE '%\\_idx' ESCAPE '\\' "
                               "AND name NOT LIKE '%\\_docsize' ESCAPE '\\' AND name NOT LIKE '%\\_config' ESCAPE '\\' "
                               "AND name NOT LIKE '%\\_content' ESCAPE '\\' AND sql NOT LIKE 'CREATE VIRTUAL%' ORDER BY name",
                           -1, &t, NULL) == SQLITE_OK) {
        while (sqlite3_step(t) == SQLITE_ROW) {
            const char *zTab = (const char *)sqlite3_column_text(t, 0);
            char *q = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY 1, 2", zTab);
            sqlite3_stmt *r;
            sqlite3_str_appendf(out, "[%s]", zTab);
            if (sqlite3_prepare_v2(db, q, -1, &r, NULL) == SQLITE_OK) {
                while (sqlite3_step(r) == SQLITE_ROW) {
                    for (int i = 0; i < sqlite3_column_count(r); i++) {
                        sqlite3_value *v = sqlite3_column_value(r, i);
                        switch (sqlite3_value_type(v)) {
                        case SQLITE_INTEGER: sqlite3_str_appendf(out, "%lld,", sqlite3_value_int64(v)); break;
                        case SQLITE_FLOAT: sqlite3_str_appendf(out, "%!.17g,", sqlite3_value_double(v)); break;
                        case SQLITE_TEXT: sqlite3_str_appendf(out, "'%q',", sqlite3_value_text(v)); break;
                        case SQLITE_BLOB: sqlite3_str_appendf(out, "x%d:%d,", sqlite3_value_bytes(v),
                                                              sqlite3_value_bytes(v) ? ((const unsigned char *)sqlite3_value_blob(v))[0] : 0); break;
                        default: sqlite3_str_appendall(out, "NULL,");
                        }
                    }
                    sqlite3_str_appendall(out, ";");
                }
                sqlite3_finalize(r);
            }
            sqlite3_free(q);
        }
        sqlite3_finalize(t);
    }
    return sqlite3_str_finish(out);
}

/* Flush everything, rebuild a replica from L1, compare all rows and the
** root. Destroys the writer. */
static const char *replica_matches(HppliteNode *w, HppliteL1 *l1, const char *replicaFile) {
    static char why[400];
    unsigned char rootW[32], rootR[32];
    char *a, *b;
    for (int i = 0; i < 50 && hpplite_node_pending_count(w) > 0; i++) hpplite_node_flush_batch(w);
    if (hpplite_node_pending_count(w) != 0) return "writer still has pending rows after flushing";
    a = dump(w->db);
    hpplite_state_root(w->db, rootW);
    hpplite_node_destroy(w);

    char path[256];
    snprintf(path, sizeof(path), DIR "/%s", replicaFile);
    HppliteNode *r = open_node(path, l1);
    if (!r) { sqlite3_free(a); return "replica open"; }
    if (r->state == HPPLITE_STATE_ERROR) { sqlite3_free(a); hpplite_node_destroy(r); return "replica sync failed"; }
    b = dump(r->db);
    hpplite_state_root(r->db, rootR);
    hpplite_node_destroy(r);
    int rows = strcmp(a, b) != 0, root = memcmp(rootW, rootR, 32) != 0;
    if (rows) {
        size_t i = 0; while (a[i] && a[i] == b[i]) i++;
        while (i > 0 && a[i - 1] != ';' && a[i - 1] != ']') i--;
        snprintf(why, sizeof(why), "replica rows differ: writer [%.70s] replica [%.70s]", a + i, b + i);
    } else if (root) {
        snprintf(why, sizeof(why), "replica rows equal but its state root differs");
    }
    sqlite3_free(a); sqlite3_free(b);
    return rows || root ? why : NULL;
}

static void app_fn(sqlite3_context *c, int n, sqlite3_value **v) {
    (void)n;
    sqlite3_result_int64(c, sqlite3_value_int64(v[0]) * 1000 + (rand() % 1000));   /* not on any replica */
}

/* 1. What statement capture refused: the values exist only on the writer */
static void test_nondeterministic_values(void) {
    TEST("random(), randomblob(), the clock, 'localtime', CURRENT_TIMESTAMP defaults, last_insert_rowid(), an app function: replica equal");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/nd.db", l1);
    if (!w) FAIL("open");
    sqlite3_create_function(w->db, "app_fn", 1, SQLITE_UTF8, NULL, app_fn, NULL, NULL);
    const char *sql[] = {
        "CREATE TABLE t(id INTEGER PRIMARY KEY, r, b, at, local, made DEFAULT CURRENT_TIMESTAMP, prev, f)",
        "INSERT INTO t(r, b, at, local) VALUES(random(), randomblob(16), datetime('now'), datetime('now', 'localtime'))",
        "INSERT INTO t(r, prev) VALUES(random(), last_insert_rowid())",
        "INSERT INTO t(f) VALUES(app_fn(7))",
        "UPDATE t SET r = abs(random()) % 1000, at = strftime('%s', 'now') WHERE id <= 2",
        "INSERT INTO t(r, b) SELECT random(), randomblob(4) FROM t",
        "DELETE FROM t WHERE id = (SELECT min(id) FROM t WHERE r % 2 = 0)",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT count(*) FROM t") < 5) FAIL("writer premise");
    const char *why = replica_matches(w, l1, "nd_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 2. sqlite_sequence: a zero-row INSERT creates an entry no record carries;
** a dropped and recreated table restarts it */
static void test_sequence(void) {
    TEST("sqlite_sequence: zero-row INSERT into a new table, DROP and recreate, deletes past the top: replica equal");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/seq.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "CREATE TABLE src(v)",
        "CREATE TABLE a(id INTEGER PRIMARY KEY AUTOINCREMENT, v)",
        "INSERT INTO a(v) SELECT v FROM src",                           /* (a, 0) */
        "CREATE TABLE b(id INTEGER PRIMARY KEY AUTOINCREMENT, v)",
        "INSERT INTO b(v) VALUES(1), (2), (3)",
        "DELETE FROM b WHERE id = 3",                                   /* seq stays 3 */
        "BEGIN", "CREATE TABLE c(id INTEGER PRIMARY KEY AUTOINCREMENT, v)",
        "INSERT INTO c(v) SELECT v FROM src", "COMMIT",                 /* a migration's copy of nothing */
        "DROP TABLE b",
        "CREATE TABLE b(id INTEGER PRIMARY KEY AUTOINCREMENT, v)",
        "INSERT INTO b(v) SELECT v FROM src",                           /* (b, 0) again */
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT count(*) FROM sqlite_sequence") != 3) FAIL("writer premise: three entries");
    const char *why = replica_matches(w, l1, "seq_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 3. Mixed: schema changes and virtual tables are text in the same
** transactions as records, foreign keys on */
static void test_mixed_text_and_records(void) {
    TEST("one transaction mixing DDL (text), FTS5 writes (text) and records, foreign keys on: replica equal and searchable");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/mixed.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "PRAGMA foreign_keys = ON",
        "BEGIN",
        "CREATE TABLE post(id INTEGER PRIMARY KEY, body TEXT, at DEFAULT CURRENT_TIMESTAMP)",
        "CREATE TABLE tag(id INTEGER PRIMARY KEY, post INTEGER REFERENCES post(id) ON DELETE CASCADE, name)",
        "CREATE VIRTUAL TABLE post_fts USING fts5(body, content='post', content_rowid='id')",
        "CREATE TRIGGER post_ai AFTER INSERT ON post BEGIN INSERT INTO post_fts(rowid, body) VALUES(new.id, new.body); END",
        "CREATE TRIGGER post_ad AFTER DELETE ON post BEGIN INSERT INTO post_fts(post_fts, rowid, body) VALUES('delete', old.id, old.body); END",
        "INSERT INTO post(body) VALUES('rows are captured'), ('text is replayed'), ('rows again')",
        "INSERT INTO tag(post, name) SELECT id, 'n' || random() FROM post",
        "COMMIT",
        "DELETE FROM post WHERE id = 2",                                /* cascade to tag, FTS delete */
        "UPDATE tag SET name = upper(name) WHERE post = 1",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT count(*) FROM tag") != 2) FAIL("writer premise: cascade");
    const char *why = replica_matches(w, l1, "mixed_r.db");
    if (!why) {
        HppliteNode *r = open_node(DIR "/mixed_r.db", l1);
        sqlite3_int64 hits = r ? scalar(r->db, "SELECT count(*) FROM post_fts WHERE post_fts MATCH 'rows'") : -1;
        if (r) hpplite_node_destroy(r);
        if (hits != 2) { hpplite_l1_disconnect(l1); FAILF("replica search: %lld hits, want 2", (long long)hits); }
    }
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 4. Records larger than a batch: pieces (#59); transactions larger than a
** batch: open/cont (#57) */
static void test_records_over_batch_size(void) {
    TEST("a 300 KB blob and a 2000-row transaction with 16 KB batches: cut into pieces, replica equal");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node_ex(DIR "/big.db", l1, 16 * 1024, 0);
    if (!w) FAIL("open");
    const char *sql[] = {
        "CREATE TABLE blobs(id INTEGER PRIMARY KEY, data BLOB)",
        "INSERT INTO blobs(data) VALUES(randomblob(300000))",
        "CREATE TABLE many(id INTEGER PRIMARY KEY, v TEXT)",
        "BEGIN",
        "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 2000) "
        "INSERT INTO many(v) SELECT hex(randomblob(20)) FROM n",
        "UPDATE many SET v = lower(v) WHERE id % 3 = 0",
        "COMMIT",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    int last = hpplite_node_flush_batch(w);
    if (last < 20) FAILF("last height %d: the records were not cut into 16 KB batches", last);
    /* the dump compares a blob's length and first byte; the root, all of it */
    const char *why = replica_matches(w, l1, "big_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 5. Size cut: a transaction of records is never split across batches
** unless it is larger than a batch */
static void test_size_cut_keeps_transactions(void) {
    TEST("the size cut never splits a transaction of records across batches");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node_ex(DIR "/cut.db", l1, 3000, 0);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(tag, pad)", NULL);
    if (hpplite_node_flush_batch(w) != 1) FAIL("batch 1");
    for (int k = 0; k < 12; k++) {
        int nStmt = 1 + (k * 7) % 9;
        hpplite_node_exec(w, "BEGIN", NULL);
        for (int s = 0; s < nStmt; s++) {
            char q[256];
            snprintf(q, sizeof(q), "INSERT INTO t VALUES(%d, '%080d')", k, s);
            if (sqlite3_exec(w->db, q, NULL, NULL, NULL) != SQLITE_OK) FAIL("insert");
        }
        hpplite_node_exec(w, "COMMIT", NULL);
    }
    int last = hpplite_node_flush_batch(w);
    if (last < 3) FAILF("flush: last height %d, want several batches", last);
    /* every entry posted is a record that decodes; that no group is split
    ** is what replay checks (a split group is held and dropped whole) */
    for (int h = 2; h <= last; h++) {
        HppliteBatch *b = hpplite_node_fetch_batch(w, (uint64_t)h);
        if (!b) FAIL("fetch posted batch");
        for (int i = 0; i < b->nTxns; i++) {
            int op = 0; char *zTab = NULL;
            if (!(b->aTxns[i].mode & HPPLITE_MODE_ROW)) continue;
            if (hpplite_rows_entry_info(b->aTxns[i].zSql, &op, &zTab) != SQLITE_OK) { hpplite_batch_free(b); FAIL("entry undecodable"); }
            sqlite3_free(zTab);
        }
        hpplite_batch_free(b);
    }
    const char *why = replica_matches(w, l1, "cut_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 6. Plaintext batches carry records, marked so a version without row
** capture refuses them */
static void test_plaintext_entries(void) {
    TEST("encrypt=off: the batch on L1 is plaintext JSON whose entries are records (mode 4), decodable");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    g_plain = 1;
    HppliteNode *w = open_node_ex(DIR "/plain.db", l1, 0, 1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(x)", NULL);
    hpplite_node_exec(w, "INSERT INTO t VALUES(random())", NULL);
    if (hpplite_node_flush_batch(w) != 1) FAIL("flush");
    size_t len = 0;
    uint8_t *raw = hpplite_l1_get_batch(l1, 1, &len);
    if (!raw || hpplite_envelope_is_sealed(raw, len)) { free(raw); FAIL("expected plaintext on L1"); }
    char *json = malloc(len + 1);
    memcpy(json, raw, len); json[len] = 0;
    free(raw);
    HppliteBatch *b = hpplite_batch_from_json(strchr(json, '{'));
    free(json);
    if (!b || b->nTxns != 2) { if (b) hpplite_batch_free(b); FAIL("two entries: the CREATE TABLE and a record"); }
    int op = 0; char *zTab = NULL;
    int ok = !(b->aTxns[0].mode & HPPLITE_MODE_ROW) && strncmp(b->aTxns[0].zSql, "CREATE TABLE", 12) == 0
          && (b->aTxns[1].mode & HPPLITE_MODE_ROW)
          && hpplite_rows_entry_info(b->aTxns[1].zSql, &op, &zTab) == SQLITE_OK
          && op == HPPLITE_ROW_INSERT && strcmp(zTab, "t") == 0;
    sqlite3_free(zTab);
    hpplite_batch_free(b);
    if (!ok) FAIL("entries are not what was written");
    const char *why = replica_matches(w, l1, "plain_r.db");
    g_plain = 0;
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 7. Still refused with row capture: ATTACH (writes to another file) */
static void test_attach_refused(void) {
    TEST("ATTACH is still refused with row capture on");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/att.db", l1);
    if (!w) FAIL("open");
    int rc = sqlite3_exec(w->db, "ATTACH '" DIR "/other.db' AS other", NULL, NULL, NULL);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    if (rc == SQLITE_OK) FAIL("ATTACH accepted");
    PASS();
}

static int foreign_commit_hook(void *p) { (void)p; return 0; }
static int foreign_trace(unsigned m, void *c, void *p, void *x) { (void)m; (void)c; (void)p; (void)x; return 0; }

/* 8. Another component replaces the trace hook: the triggers still record
** writes to the tables they cover; the replacement is still reported */
static void test_hooks_replaced(void) {
    TEST("another component replaces the hooks: writes are still recorded, the replacement still reported, replica equal");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/hooks.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL);
    sqlite3_commit_hook(w->db, foreign_commit_hook, NULL);
    sqlite3_trace_v2(w->db, SQLITE_TRACE_STMT, foreign_trace, NULL);
    sqlite3_exec(w->db, "INSERT INTO t VALUES(random())", NULL, NULL, NULL);
    if (hpplite_node_pending_count(w) != 3) { printf("(pending %d) ", hpplite_node_pending_count(w)); FAIL("the write must be recorded by the triggers"); }
    if (scalar(w->db, "SELECT hooks_replaced FROM hpplite_status") != 1) FAIL("replacement should be counted");
    sqlite3_exec(w->db, "INSERT INTO t VALUES(3)", NULL, NULL, NULL);
    const char *why = replica_matches(w, l1, "hooks_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 9. Pending records survive a reopen and are posted by the next writer */
static void test_reopen(void) {
    TEST("records pending at close are posted after a reopen; a text-mode reopen posts them too");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/re.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    hpplite_node_flush_batch(w);
    hpplite_node_exec(w, "INSERT INTO t(v) VALUES(random()), (random())", NULL);
    hpplite_node_exec(w, "UPDATE t SET v = randomblob(3) WHERE id = 1", NULL);
    /* close without the final flush posting: the mock refuses */
    hpplite_l1_mock_set_fail_submit(l1, 1);
    hpplite_node_destroy(w);
    hpplite_l1_mock_set_fail_submit(l1, 0);
    /* reopened without row capture: the records already logged still post */
    HppliteNodeConfig config;
    memset(&config, 0, sizeof(config));
    config.nodeId = "rows";
    memcpy(config.privkey, PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = DIR;
    config.dbPath = DIR "/re.db";
    config.checkpointInterval = 1000;
    w = hpplite_node_create(&config);
    if (!w) FAIL("reopen");
    hpplite_node_set_l1(w, l1);
    hpplite_node_sync_from_l1(w, 1);
    hpplite_node_start(w);
    hpplite_node_exec(w, "INSERT INTO t(v) VALUES('text')", NULL);
    const char *why = replica_matches(w, l1, "re_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 10. VACUUM may renumber the rowids of tables without an INTEGER PRIMARY
** KEY, and records address their rows by rowid: the replica's VACUUM must
** renumber the same way for the records after it to hit the same rows */
static void test_vacuum(void) {
    TEST("VACUUM on tables without INTEGER PRIMARY KEY (gaps, a WITHOUT ROWID table), then updates and deletes by rowid: replica equal");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/vac.db", l1);
    if (!w) FAIL("open");
    const char *sql[] = {
        "CREATE TABLE plain(name TEXT, v)",
        "CREATE TABLE keyed(k TEXT PRIMARY KEY, v)",
        "CREATE TABLE wr(k TEXT PRIMARY KEY, v) WITHOUT ROWID",
        "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 300) "
        "INSERT INTO plain SELECT 'p' || i, i FROM n",
        "INSERT INTO keyed SELECT name, v FROM plain",
        "INSERT INTO wr SELECT name, v FROM plain",
        "DELETE FROM plain WHERE v % 3 = 0",
        "DELETE FROM keyed WHERE v % 4 = 0",
        "DELETE FROM wr WHERE v % 5 = 0",
        "VACUUM",
        "UPDATE plain SET v = -v WHERE rowid IN (1, 2, 50, 150)",
        "DELETE FROM plain WHERE rowid = 10",
        "UPDATE keyed SET v = random() WHERE rowid < 20",
        "INSERT INTO plain VALUES('after', 0)",
        NULL };
    if (run(w, sql) != 0) FAIL("writer SQL");
    if (scalar(w->db, "SELECT max(rowid) FROM plain") != 201) FAILF("writer premise: VACUUM renumbered plain to %lld rows", scalar(w->db, "SELECT max(rowid) FROM plain"));
    const char *why = replica_matches(w, l1, "vac_r.db");
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 11. The capture triggers are HPPLite's: the application cannot drop or
** shadow them; a table's own drop takes its triggers, and they come back
** with the table */
static void test_trigger_guard(void) {
    TEST("the application cannot drop or create hpplite_cap_ triggers; DROP TABLE still works and capture resumes on a new table");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/guard.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(x)", NULL);
    char name[64] = "";
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(w->db, "SELECT name FROM temp.sqlite_schema WHERE type = 'trigger' AND name LIKE 'hpplite_cap_i%'", -1, &q, NULL) == SQLITE_OK) {
        if (sqlite3_step(q) == SQLITE_ROW) snprintf(name, sizeof(name), "%s", sqlite3_column_text(q, 0));
        sqlite3_finalize(q);
    }
    if (!name[0]) FAIL("no capture trigger to test with");
    char sql[160];
    snprintf(sql, sizeof(sql), "DROP TRIGGER temp.\"%s\"", name);
    int dropRc = sqlite3_exec(w->db, sql, NULL, NULL, NULL);
    int createRc = sqlite3_exec(w->db, "CREATE TEMP TRIGGER hpplite_cap_x AFTER INSERT ON main.t BEGIN SELECT 1; END", NULL, NULL, NULL);
    hpplite_node_exec(w, "INSERT INTO t VALUES(random())", NULL);
    int dropTableRc = sqlite3_exec(w->db, "DROP TABLE t", NULL, NULL, NULL);
    hpplite_node_exec(w, "CREATE TABLE t(y); INSERT INTO t VALUES(randomblob(4))", NULL);
    sqlite3_int64 recs = scalar(w->db, "SELECT count(*) FROM _hpplite_pending WHERE op > 0");
    const char *why = replica_matches(w, l1, "guard_r.db");
    hpplite_l1_disconnect(l1);
    if (dropRc == SQLITE_OK) FAIL("DROP TRIGGER of a capture trigger accepted");
    if (createRc == SQLITE_OK) FAIL("CREATE TRIGGER with the capture prefix accepted");
    if (dropTableRc != SQLITE_OK) FAIL("DROP TABLE refused");
    if (recs != 2) FAILF("%lld records, want 2 (before and after the drop)", (long long)recs);
    if (why) FAIL(why);
    PASS();
}

int main(void) {
    printf("=== HPPLite row capture end to end (#19) ===\n");
    srand(1);
    test_nondeterministic_values();
    test_sequence();
    test_mixed_text_and_records();
    test_records_over_batch_size();
    test_size_cut_keeps_transactions();
    test_plaintext_entries();
    test_attach_refused();
    test_hooks_replaced();
    test_reopen();
    test_vacuum();
    test_trigger_guard();
    printf("\n%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
