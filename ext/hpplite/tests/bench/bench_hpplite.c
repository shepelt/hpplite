/*
** HPPLite microbenchmark (mock L1, no network)
**
**   write overhead   statements/s with hpplite=on vs plain SQLite
**   flush            build + serialize + finalize a batch of N statements
**   root hash        ms per MB of database content
**   replay           statements/s rebuilding a fresh file from L1 batches
**   open             reopen latency with N pending statements
**
** Usage: bench_hpplite [--rows N] [--mb a,b,c] [--json file]
** Not part of ctest; run via `make bench` or directly. Numbers depend on the
** machine; doc/performance.md records a reference run.
*/

#include "node.h"
#include "cipher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DIR "/tmp/hpplite_bench"

static const unsigned char PRIVKEY[32] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20
};

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ---- results ---- */
typedef struct { const char *name; double value; const char *unit; } Result;
static Result results[64];
static int nResults = 0;
static void record(const char *name, double value, const char *unit) {
    results[nResults].name = name; results[nResults].value = value; results[nResults].unit = unit;
    nResults++;
    printf("  %-44s %12.1f %s\n", name, value, unit);
}

static void reset_dir(void) { system("rm -rf " DIR " && mkdir -p " DIR); }

static HppliteNode *open_node(const char *path, HppliteL1 *l1, int checkpointInterval) {
    HppliteNodeConfig config;
    HppliteNode *node;
    memset(&config, 0, sizeof(config));
    config.nodeId = "bench";
    memcpy(config.privkey, PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = DIR;
    config.dbPath = (char *)path;
    config.checkpointInterval = checkpointInterval;
    node = hpplite_node_create(&config);
    if (node && l1) { hpplite_node_set_l1(node, l1); hpplite_node_sync_from_l1(node, 1); hpplite_node_start(node); }
    return node;
}

static const char *CREATE_T = "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT, n REAL)";

/* Insert `rows` rows; autocommit or in transactions of txnSize. Returns ms. */
static int nextId = 1;   /* ids never repeat across phases */
static double insert_rows(sqlite3 *db, int rows, int txnSize) {
    char sql[256];
    double t0 = now_ms();
    for (int i = 0; i < rows; i++) {
        int id = nextId++;
        if (txnSize > 1 && i % txnSize == 0) sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES(%d,'row-%08d-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',%d.5)", id, id, i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
        if (txnSize > 1 && (i % txnSize == txnSize - 1 || i == rows - 1)) sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    }
    return now_ms() - t0;
}

static sqlite3 *open_plain(const char *path) {
    sqlite3 *db;
    sqlite3_open(path, &db);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);   /* same mode HPPLite uses */
    sqlite3_busy_timeout(db, 5000);
    sqlite3_exec(db, CREATE_T, NULL, NULL, NULL);
    return db;
}

static void bench_write_overhead(int rows) {
    /* Autocommit is bound by one fsync per statement (ms each), so it uses
    ** a tenth of the rows; the per-statement figure is what matters. */
    int autoRows = rows / 10 > 0 ? rows / 10 : 1;
    printf("\n[write overhead] INSERTs of 64-byte text, WAL (%d autocommit, %d in 1000/txn)\n", autoRows, rows);
    reset_dir();
    sqlite3 *plain = open_plain(DIR "/plain_auto.db");
    double pa = insert_rows(plain, autoRows, 1);
    sqlite3_close(plain);
    plain = open_plain(DIR "/plain_txn.db");
    double pt = insert_rows(plain, rows, 1000);
    sqlite3_close(plain);

    HppliteNode *n = open_node(DIR "/hpp_auto.db", NULL, 1000000);
    sqlite3_exec(n->db, CREATE_T, NULL, NULL, NULL);
    double ha = insert_rows(n->db, autoRows, 1);
    hpplite_node_destroy(n);
    n = open_node(DIR "/hpp_txn.db", NULL, 1000000);
    sqlite3_exec(n->db, CREATE_T, NULL, NULL, NULL);
    double ht = insert_rows(n->db, rows, 1000);
    hpplite_node_destroy(n);

    record("plain SQLite, autocommit", autoRows / pa * 1000, "stmt/s");
    record("hpplite=on, autocommit", autoRows / ha * 1000, "stmt/s");
    record("  overhead autocommit", ha / pa, "x");
    record("plain SQLite, 1000/txn", rows / pt * 1000, "stmt/s");
    record("hpplite=on, 1000/txn", rows / ht * 1000, "stmt/s");
    record("  overhead 1000/txn", ht / pt, "x");
}

static void bench_flush_and_replay(int rows) {
    printf("\n[flush + replay] %d pending statements\n", rows);
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    hpplite_l1_mock_set_batch_size_limit(l1, 64 * 1024 * 1024);   /* one batch */
    HppliteNode *n = open_node(DIR "/flush.db", l1, 1000000);
    sqlite3_exec(n->db, CREATE_T, NULL, NULL, NULL);
    insert_rows(n->db, rows, 1000);

    double t0 = now_ms();
    uint64_t h = hpplite_node_flush_batch(n);                       /* no root (interval huge) */
    double flushNoRoot = now_ms() - t0;
    if (h != 1) printf("  (unexpected flush height %llu)\n", (unsigned long long)h);
    record("flush, no root", flushNoRoot, "ms");
    record("  flush throughput", rows / flushNoRoot * 1000, "stmt/s");

    insert_rows(n->db, rows, 1000);
    n->config.checkpointInterval = 1;                                /* next batch carries a root */
    t0 = now_ms();
    hpplite_node_flush_batch(n);
    double flushRoot = now_ms() - t0;
    record("flush with root + checkpoint", flushRoot, "ms");

    size_t len = 0; uint8_t *data = hpplite_l1_get_batch(l1, 1, &len);
    record("posted batch size (bytes/stmt)", (double)len / rows, "B");
    if (n->cipher && data) {
        /* seal / open cost for the whole batch, measured on the posted bytes */
        size_t plainLen = 0;
        uint8_t *plain = hpplite_envelope_open(n->encKey, 1, data, len, &plainLen, NULL);
        if (plain) {
            double t1 = now_ms();
            for (int i = 0; i < 5; i++) {
                size_t sl = 0;
                uint8_t *s = hpplite_envelope_seal(n->cipher, n->encKey, 1, HPPLITE_ENCODING_NONE, plain, plainLen, &sl);
                free(s);
            }
            double seal = (now_ms() - t1) / 5;
            t1 = now_ms();
            for (int i = 0; i < 5; i++) {
                uint8_t *o = hpplite_envelope_open(n->encKey, 1, data, len, NULL, NULL);
                free(o);
            }
            double open = (now_ms() - t1) / 5;
            record("seal batch (chacha20poly1305)", seal, "ms");
            record("  seal throughput", plainLen / seal / 1000.0, "MB/s");
            record("open batch", open, "ms");
            free(plain);
        }
    }
    free(data);
    hpplite_node_destroy(n);

    t0 = now_ms();
    HppliteNode *r = open_node(DIR "/replay.db", l1, 1000000);      /* replays 2 batches */
    double replay = now_ms() - t0;
    int ok = r && hpplite_node_get_height(r) == 3;
    record("replay from L1", replay, "ms");
    record("  replay throughput", (2.0 * rows) / replay * 1000, "stmt/s");
    if (!ok) printf("  (replay incomplete)\n");
    if (r) hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
}

static void bench_root_hash(const char *mbList) {
    printf("\n[root hash] content hash of a 3-column table\n");
    char buf[128]; strncpy(buf, mbList, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        int mb = atoi(tok);
        if (mb <= 0) continue;
        reset_dir();
        sqlite3 *db = open_plain(DIR "/hash.db");
        /* ~100 bytes per row on disk */
        char sql[512];
        snprintf(sql, sizeof(sql),
            "WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i<%d)"
            " INSERT INTO t SELECT i, printf('row-%%08d-%%s', i, hex(randomblob(24))), i*1.5 FROM s",
            mb * 10000);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)", NULL, NULL, NULL);
        unsigned char root[32];
        hpplite_db_hash(db, root);                                   /* warm the page cache */
        double t0 = now_ms();
        hpplite_db_hash(db, root);
        double ms = now_ms() - t0;
        sqlite3_stmt *st; sqlite3_int64 bytes = 0;
        sqlite3_prepare_v2(db, "SELECT page_count * page_size FROM pragma_page_count, pragma_page_size", -1, &st, NULL);
        if (sqlite3_step(st) == SQLITE_ROW) bytes = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        sqlite3_close(db);
        static char name[8][64]; static int k = 0;
        snprintf(name[k % 8], 64, "root hash, %lld MB", (long long)(bytes / (1024 * 1024)));
        record(name[k % 8], ms, "ms");
        snprintf(name[(k + 1) % 8], 64, "  per MB (%d MB db)", mb);
        record(name[(k + 1) % 8], ms / ((double)bytes / (1024 * 1024)), "ms/MB");
        k += 2;
    }
}

static void bench_open_with_pending(int rows) {
    printf("\n[open] reopen with %d pending statements\n", rows);
    reset_dir();
    HppliteNode *n = open_node(DIR "/open.db", NULL, 1000000);
    sqlite3_exec(n->db, CREATE_T, NULL, NULL, NULL);
    insert_rows(n->db, rows, 1000);
    /* tear down without the flush destroy would do (simulated crash) */
    n->l1 = NULL;
    hpplite_node_stop_timer(n);
    hpplite_unregister_node(n->db);
    hpplite_shutdown(n->ctx); n->ctx = NULL;
    if (n->bkDb) { sqlite3_close(n->bkDb); n->bkDb = NULL; }
    sqlite3_close(n->db); n->db = NULL; n->ownsDb = 0;
    hpplite_node_destroy(n);

    double t0 = now_ms();
    n = open_node(DIR "/open.db", NULL, 1000000);
    double openMs = now_ms() - t0;
    record("reopen with pending", openMs, "ms");
    t0 = now_ms();
    hpplite_node_flush_batch(n);
    record("first flush after reopen", now_ms() - t0, "ms");
    hpplite_node_destroy(n);
}

static void write_json(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "{\n  \"sqlite\": \"%s\",\n  \"results\": [\n", sqlite3_libversion());
    for (int i = 0; i < nResults; i++) {
        fprintf(f, "    {\"name\": \"%s\", \"value\": %.3f, \"unit\": \"%s\"}%s\n",
                results[i].name, results[i].value, results[i].unit, i + 1 < nResults ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    printf("\nwrote %s\n", path);
}

int main(int argc, char **argv) {
    int rows = 20000;
    const char *mbList = "1,10,50";
    const char *json = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rows") && i + 1 < argc) rows = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mb") && i + 1 < argc) mbList = argv[++i];
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) json = argv[++i];
        else { fprintf(stderr, "usage: %s [--rows N] [--mb a,b,c] [--json file]\n", argv[0]); return 2; }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== HPPLite microbenchmark (SQLite %s, mock L1) ===\n", sqlite3_libversion());
    bench_write_overhead(rows);
    bench_flush_and_replay(rows);
    bench_root_hash(mbList);
    bench_open_with_pending(rows);
    if (json) write_json(json);
    system("rm -rf " DIR);
    return 0;
}
