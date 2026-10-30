/*
** HPPLite row capture (capture engine v2, #19)
**
** A writer with row capture runs a workload; its log is applied to an empty
** database - schema text executed, row records applied with triggers and
** foreign keys off - and the two must be equal: every table, every row, every
** rowid, sqlite_sequence included.
*/

#include "hpplite.h"
#include "capture_rows.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { printf("  Testing: %s... ", name); tests_run++; fflush(stdout); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)
#define FAILF(...) do { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); return; } while(0)

#define DIR "/tmp/hpplite_test_rows"

static void reset_dir(void) { (void)!system("rm -rf " DIR " && mkdir -p " DIR); }
static int exec(sqlite3 *db, const char *sql) {
    char *e = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &e);
    if (rc != SQLITE_OK && getenv("ROWS_VERBOSE")) fprintf(stderr, "  [%s]: %s\n", sql, e);
    sqlite3_free(e);
    return rc == SQLITE_OK;
}
static sqlite3_int64 scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st; sqlite3_int64 v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

typedef struct { sqlite3 *db; HppliteCtx *ctx; } W;
static int open_writer(const char *path, W *w) {
    if (sqlite3_open(path, &w->db) != SQLITE_OK) return 0;
    exec(w->db, "PRAGMA journal_mode=WAL");
    if (hpplite_init(w->db, &w->ctx) != SQLITE_OK) return 0;
    return hpplite_rows_enable(w->ctx) == SQLITE_OK;
}
static void close_writer(W *w) { hpplite_shutdown(w->ctx); sqlite3_close(w->db); }

/* Apply the writer's whole log, in order, to a plain database */
static const char *apply_log(sqlite3 *writer, sqlite3 *replica, int *pRecords, int *pTexts) {
    static char why[512];
    sqlite3_stmt *st;
    int on;
    *pRecords = *pTexts = 0;
    sqlite3_db_config(replica, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, &on);
    exec(replica, "PRAGMA foreign_keys=OFF");
    exec(replica, "BEGIN");
    if (sqlite3_prepare_v2(writer, "SELECT sql, tbl, op, rec FROM _hpplite_pending ORDER BY id", -1, &st, NULL) != SQLITE_OK) return "read log";
    while (sqlite3_step(st) == SQLITE_ROW) {
        int op = sqlite3_column_int(st, 2);
        if (op > 0) {
            char *err = NULL;
            if (hpplite_rows_apply(replica, (const char *)sqlite3_column_text(st, 1), op,
                                   sqlite3_column_blob(st, 3), sqlite3_column_bytes(st, 3), &err) != SQLITE_OK) {
                snprintf(why, sizeof why, "apply: %s", err ? err : "?");
                sqlite3_free(err); sqlite3_finalize(st); return why;
            }
            (*pRecords)++;
        } else {
            /* statement text: the schema, or a write logged the v1 way */
            const char *sql = (const char *)sqlite3_column_text(st, 0);
            int on2;
            sqlite3_db_config(replica, SQLITE_DBCONFIG_ENABLE_TRIGGER, 1, &on2);   /* a text write's triggers are its effects */
            exec(replica, "PRAGMA foreign_keys=ON");
            if (!exec(replica, sql)) { snprintf(why, sizeof why, "text: %.200s: %s", sql, sqlite3_errmsg(replica)); sqlite3_finalize(st); return why; }
            exec(replica, "PRAGMA foreign_keys=OFF");
            sqlite3_db_config(replica, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, &on2);
            (*pTexts)++;
        }
    }
    sqlite3_finalize(st);
    exec(replica, "COMMIT");
    return NULL;
}

/* Every table, every row (rowid tables with their rowid), typed */
static void dump_value(sqlite3_stmt *st, int i, char **out, size_t *len, size_t *cap) {
    char buf[64]; const char *p = buf; int n;
    switch (sqlite3_column_type(st, i)) {
        case SQLITE_INTEGER: n = snprintf(buf, sizeof buf, "i%lld", (long long)sqlite3_column_int64(st, i)); break;
        case SQLITE_FLOAT: { double d = sqlite3_column_double(st, i); unsigned long long u; memcpy(&u, &d, 8); n = snprintf(buf, sizeof buf, "f%llx", u); break; }
        case SQLITE_NULL: n = snprintf(buf, sizeof buf, "n"); break;
        default: {
            n = sqlite3_column_bytes(st, i);
            const unsigned char *b = sqlite3_column_blob(st, i);
            size_t need = (size_t)n * 2 + 4;
            while (*len + need + 2 > *cap) { *cap *= 2; *out = realloc(*out, *cap); }
            (*out)[(*len)++] = sqlite3_column_type(st, i) == SQLITE_TEXT ? 't' : 'b';
            for (int k = 0; k < n; k++) *len += (size_t)sprintf(*out + *len, "%02x", b[k]);
            (*out)[(*len)++] = ','; (*out)[*len] = 0;
            return;
        }
    }
    while (*len + (size_t)n + 2 > *cap) { *cap *= 2; *out = realloc(*out, *cap); }
    memcpy(*out + *len, p, (size_t)n); *len += (size_t)n; (*out)[(*len)++] = ','; (*out)[*len] = 0;
}
static char *dump(sqlite3 *db) {
    size_t cap = 1 << 16, len = 0; char *out = malloc(cap); out[0] = 0;
    sqlite3_stmt *tabs;
    sqlite3_prepare_v2(db, "SELECT name FROM sqlite_schema WHERE type='table' AND name NOT LIKE '\\_hpplite\\_%' ESCAPE '\\'"
                           " AND sql NOT LIKE 'CREATE VIRTUAL%' ORDER BY name", -1, &tabs, NULL);
    while (sqlite3_step(tabs) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(tabs, 0);
        char sql[512];
        sqlite3_stmt *probe, *st;
        snprintf(sql, sizeof sql, "SELECT rowid FROM \"%s\" LIMIT 0", t);
        int hasRowid = sqlite3_prepare_v2(db, sql, -1, &probe, NULL) == SQLITE_OK;
        sqlite3_finalize(probe);
        if (hasRowid) snprintf(sql, sizeof sql, "SELECT rowid, * FROM \"%s\" ORDER BY rowid", t);
        else snprintf(sql, sizeof sql, "SELECT * FROM \"%s\" ORDER BY 1, 2", t);
        while (len + strlen(t) + 8 > cap) { cap *= 2; out = realloc(out, cap); }
        len += (size_t)sprintf(out + len, "[%s]", t);
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) continue;
        while (sqlite3_step(st) == SQLITE_ROW) {
            for (int i = 0; i < sqlite3_column_count(st); i++) dump_value(st, i, &out, &len, &cap);
            while (len + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            out[len++] = ';'; out[len] = 0;
        }
        sqlite3_finalize(st);
    }
    sqlite3_finalize(tabs);
    return out;
}

/* The check every test ends with */
static const char *replica_differs(W *w, const char *replicaPath, int *pRecords, int *pTexts) {
    static char why[400];
    sqlite3 *r;
    if (sqlite3_open(replicaPath, &r) != SQLITE_OK) return "open replica";
    const char *err = apply_log(w->db, r, pRecords, pTexts);
    if (err) { sqlite3_close(r); return err; }
    char *a = dump(w->db), *b = dump(r);
    int same = strcmp(a, b) == 0;
    if (!same) {
        size_t i = 0; while (a[i] && a[i] == b[i]) i++;
        size_t s = i > 60 ? i - 60 : 0;
        snprintf(why, sizeof why, "replica differs near\n      writer  ...%.140s\n      replica ...%.140s", a + s, b + s);
    }
    free(a); free(b);
    sqlite3_close(r);
    return same ? NULL : why;
}

static void test_basic_types(void) {
    TEST("inserts, updates, deletes of every type replay exactly; nothing is logged as text");
    reset_dir();
    W w;
    if (!open_writer(DIR "/basic.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE t(id INTEGER PRIMARY KEY, i INTEGER, r REAL, s TEXT, b BLOB, n)");
    exec(w.db, "CREATE TABLE plain(a, b)");          /* rowid table without an INTEGER PRIMARY KEY */
    exec(w.db, "INSERT INTO t VALUES(1, 9007199254740993, 0.1, 'héllo', X'00ff10', NULL)");
    exec(w.db, "INSERT INTO t(i, r, s) VALUES(-9223372036854775808, -0.0, '')");
    exec(w.db, "INSERT INTO t(i, r, s, b) VALUES(9223372036854775807, 1e308, 'x', zeroblob(3))");
    exec(w.db, "UPDATE t SET s = 'changed', r = 2.5 WHERE id = 1");
    exec(w.db, "UPDATE t SET i = i + 1");            /* every row */
    exec(w.db, "DELETE FROM t WHERE id = 2");
    exec(w.db, "INSERT INTO plain VALUES(1, 'a'), (2, 'b'), (3, 'c')");
    exec(w.db, "DELETE FROM plain WHERE a = 2");
    exec(w.db, "UPDATE plain SET rowid = 100 WHERE a = 3");   /* the rowid itself moves */
    exec(w.db, "UPDATE t SET id = 50 WHERE id = 3");          /* an INTEGER PRIMARY KEY moves */
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/basic_r.db", &nr, &nt);
    int texts = (int)scalar(w.db, "SELECT count(*) FROM _hpplite_pending WHERE op = 0 AND sql LIKE 'INSERT%' OR sql LIKE 'UPDATE%' OR sql LIKE 'DELETE%'");
    close_writer(&w);
    printf("(%d records, %d schema statements) ", nr, nt);
    if (why) FAIL(why);
    if (texts) FAIL("a write was logged as text");
    PASS();
}

static void test_effects_not_text(void) {
    TEST("random(), the clock and an application-defined function in writes: accepted, replica equal");
    reset_dir();
    W w;
    if (!open_writer(DIR "/nd.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v, at DEFAULT CURRENT_TIMESTAMP, tok)");
    /* what WordPress's SQLite plugin does: REGEXP as a function only the application has */
    sqlite3_create_function(w.db, "regexp", 2, SQLITE_UTF8, NULL, NULL, NULL, NULL);
    int ok1 = exec(w.db, "INSERT INTO t(v, tok) VALUES(random(), hex(randomblob(8)))");
    int ok2 = exec(w.db, "INSERT INTO t(v) VALUES(datetime('now'))");
    int ok3 = exec(w.db, "UPDATE t SET tok = lower(hex(randomblob(4))) WHERE id = 1");
    exec(w.db, "INSERT INTO t(v) VALUES('rss_0123456789abcdef0123456789abcdef'), ('keep')");
    int ok4 = exec(w.db, "DELETE FROM t WHERE v GLOB 'rss_*'");
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/nd_r.db", &nr, &nt);
    close_writer(&w);
    if (!ok1 || !ok2 || !ok3 || !ok4) FAIL("a write was refused");
    if (why) FAIL(why);
    PASS();
}

static void test_cascades_and_app_triggers(void) {
    TEST("foreign-key cascades and the application's triggers: captured as rows, not replayed twice");
    reset_dir();
    W w;
    if (!open_writer(DIR "/fk.db", &w)) FAIL("open");
    exec(w.db, "PRAGMA foreign_keys=ON");
    exec(w.db, "CREATE TABLE parent(id INTEGER PRIMARY KEY, name);"
               "CREATE TABLE child(id INTEGER PRIMARY KEY, pid REFERENCES parent(id) ON DELETE CASCADE ON UPDATE CASCADE, v);"
               "CREATE TABLE audit(id INTEGER PRIMARY KEY, what);"
               "CREATE TRIGGER child_audit AFTER INSERT ON child BEGIN INSERT INTO audit(what) VALUES('child ' || new.id); END;"
               "CREATE TRIGGER parent_count AFTER DELETE ON parent BEGIN INSERT INTO audit(what) VALUES('parent gone ' || old.id); END;");
    exec(w.db, "INSERT INTO parent VALUES(1, 'a'), (2, 'b')");
    exec(w.db, "INSERT INTO child(pid, v) VALUES(1, 'x'), (1, 'y'), (2, 'z')");
    exec(w.db, "UPDATE parent SET id = 10 WHERE id = 2");        /* cascades to child.pid */
    exec(w.db, "DELETE FROM parent WHERE id = 1");               /* cascades: deletes two children */
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/fk_r.db", &nr, &nt);
    sqlite3_int64 audit = scalar(w.db, "SELECT count(*) FROM audit");
    close_writer(&w);
    if (audit != 4) FAILF("writer audit rows %lld, want 4", (long long)audit);
    if (why) FAIL(why);
    PASS();
}

static void test_conflicts_upserts_bulk(void) {
    TEST("REPLACE conflicts, upserts, INSERT ... SELECT, bulk updates and deletes");
    reset_dir();
    W w;
    if (!open_writer(DIR "/bulk.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE u(id INTEGER PRIMARY KEY, email TEXT UNIQUE, n INTEGER DEFAULT 0)");
    exec(w.db, "CREATE TABLE src(x)");
    exec(w.db, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 500) INSERT INTO src SELECT x FROM c");
    exec(w.db, "INSERT INTO u(email) SELECT 'user' || x || '@x' FROM src");
    exec(w.db, "INSERT OR REPLACE INTO u(id, email) VALUES(9999, 'user7@x')");   /* replaces row 7 by its UNIQUE email */
    exec(w.db, "INSERT INTO u(email, n) VALUES('user8@x', 1) ON CONFLICT(email) DO UPDATE SET n = n + 1");
    exec(w.db, "INSERT INTO u(email) VALUES('user9@x') ON CONFLICT DO NOTHING");
    exec(w.db, "UPDATE u SET n = n + id % 3");
    exec(w.db, "DELETE FROM u WHERE id % 5 = 0");
    exec(w.db, "UPDATE u SET n = n WHERE id < 0");                               /* no-op */
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/bulk_r.db", &nr, &nt);
    close_writer(&w);
    printf("(%d records) ", nr);
    if (why) FAIL(why);
    PASS();
}

static void test_without_rowid_and_generated(void) {
    TEST("WITHOUT ROWID tables (composite key, key changes) and generated columns");
    reset_dir();
    W w;
    if (!open_writer(DIR "/wr.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE kv(a TEXT, b INTEGER, v, PRIMARY KEY(b, a)) WITHOUT ROWID");
    exec(w.db, "CREATE TABLE g(id INTEGER PRIMARY KEY, x, y AS (x * 2), z AS (x || '!') STORED)");
    exec(w.db, "INSERT INTO kv VALUES('k1', 1, 'one'), ('k2', 1, 'two'), ('k1', 2, 'three')");
    exec(w.db, "UPDATE kv SET v = 'uno' WHERE a = 'k1' AND b = 1");
    exec(w.db, "UPDATE kv SET b = 5 WHERE a = 'k2'");          /* key moves */
    exec(w.db, "DELETE FROM kv WHERE b = 2");
    exec(w.db, "INSERT INTO g(x) VALUES(1), (2), ('a')");
    exec(w.db, "UPDATE g SET x = 10 WHERE id = 1");
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/wr_r.db", &nr, &nt);
    close_writer(&w);
    if (why) FAIL(why);
    PASS();
}

static void test_table_rebuild_migration(void) {
    TEST("a migration that rebuilds a table in a transaction: the copied rows are captured");
    reset_dir();
    W w;
    if (!open_writer(DIR "/mig.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE item(id INTEGER PRIMARY KEY, name, price)");
    exec(w.db, "INSERT INTO item(name, price) VALUES('a', 1), ('b', 2), ('c', 3)");
    /* what Django, Alembic batch mode and drizzle-kit do for most ALTERs */
    int ok = exec(w.db, "BEGIN;"
                        "CREATE TABLE new_item(id INTEGER PRIMARY KEY, name TEXT NOT NULL, price REAL, sku TEXT DEFAULT 'none');"
                        "INSERT INTO new_item(id, name, price) SELECT id, name, price * 1.5 FROM item;"
                        "DROP TABLE item;"
                        "ALTER TABLE new_item RENAME TO item;"
                        "COMMIT;");
    exec(w.db, "INSERT INTO item(name, price, sku) VALUES('d', 4, 'D-4')");
    exec(w.db, "UPDATE item SET sku = 'A-1' WHERE name = 'a'");
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/mig_r.db", &nr, &nt);
    close_writer(&w);
    printf("(%d records, %d schema statements) ", nr, nt);
    if (!ok) FAIL("migration failed");
    if (why) FAIL(why);
    PASS();
}

static void test_table_from_another_connection(void) {
    TEST("a table created by another connection: the first write to it is logged as text, then rows");
    reset_dir();
    W w, w2;
    if (!open_writer(DIR "/other.db", &w) || !open_writer(DIR "/other.db", &w2)) FAIL("open");
    exec(w.db, "CREATE TABLE a(x)");
    exec(w.db, "INSERT INTO a VALUES(1)");
    exec(w2.db, "CREATE TABLE b(id INTEGER PRIMARY KEY, y)");        /* w's triggers do not know b yet */
    int ok1 = exec(w.db, "INSERT INTO b(y) VALUES('first')");
    int ok2 = exec(w.db, "INSERT INTO b(y) VALUES('second')");
    if (getenv("ROWS_VERBOSE")) {
        sqlite3_stmt *q;
        sqlite3_prepare_v2(w.db, "SELECT id, txn, op, tbl, substr(sql, 1, 60) FROM _hpplite_pending ORDER BY id", -1, &q, NULL);
        while (sqlite3_step(q) == SQLITE_ROW) fprintf(stderr, "    log %lld txn %lld op %d %s %s\n", (long long)sqlite3_column_int64(q, 0),
            (long long)sqlite3_column_int64(q, 1), sqlite3_column_int(q, 2), sqlite3_column_text(q, 3), sqlite3_column_text(q, 4));
        sqlite3_finalize(q);
    }
    sqlite3_int64 texts = scalar(w.db, "SELECT count(*) FROM _hpplite_pending WHERE op = 0 AND sql LIKE 'INSERT INTO b%'");
    sqlite3_int64 recs = scalar(w.db, "SELECT count(*) FROM _hpplite_pending WHERE op = 1 AND tbl = 'b'");
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/other_r.db", &nr, &nt);
    close_writer(&w2);
    close_writer(&w);
    if (!ok1 || !ok2) FAIL("writes failed");
    if (texts != 1 || recs != 1) FAILF("want 1 text + 1 record for b, got %lld + %lld", (long long)texts, (long long)recs);
    if (why) FAIL(why);
    PASS();
}

static void test_autoincrement_and_fts5(void) {
    TEST("AUTOINCREMENT (sqlite_sequence) and an FTS5 table (its internal tables): replica equal and searchable");
    reset_dir();
    W w;
    if (!open_writer(DIR "/fts.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE s(id INTEGER PRIMARY KEY AUTOINCREMENT, v)");
    exec(w.db, "INSERT INTO s(v) VALUES('a'), ('b'), ('c')");
    exec(w.db, "DELETE FROM s WHERE id = 3");                         /* sqlite_sequence stays at 3 */
    exec(w.db, "INSERT INTO s(v) VALUES('d')");
    int fts = exec(w.db, "CREATE VIRTUAL TABLE docs USING fts5(title, body)");
    if (fts) {
        exec(w.db, "INSERT INTO docs VALUES('one', 'row based capture'), ('two', 'statement based replication')");
        exec(w.db, "UPDATE docs SET body = 'row capture again' WHERE title = 'two'");
        exec(w.db, "DELETE FROM docs WHERE title = 'one'");
        /* external content, kept in step by the application's triggers */
        exec(w.db, "CREATE TABLE notes(id INTEGER PRIMARY KEY, body TEXT);"
                   "CREATE VIRTUAL TABLE notes_fts USING fts5(body, content='notes', content_rowid='id');"
                   "CREATE TRIGGER notes_ai AFTER INSERT ON notes BEGIN"
                   "  INSERT INTO notes_fts(rowid, body) VALUES(new.id, new.body); END;"
                   "CREATE TRIGGER notes_ad AFTER DELETE ON notes BEGIN"
                   "  INSERT INTO notes_fts(notes_fts, rowid, body) VALUES('delete', old.id, old.body); END;"
                   "CREATE TRIGGER notes_au AFTER UPDATE ON notes BEGIN"
                   "  INSERT INTO notes_fts(notes_fts, rowid, body) VALUES('delete', old.id, old.body);"
                   "  INSERT INTO notes_fts(rowid, body) VALUES(new.id, new.body); END;");
        exec(w.db, "INSERT INTO notes(body) VALUES('capture rows'), ('replicate text'), ('capture all')");
        exec(w.db, "UPDATE notes SET body = 'nothing here' WHERE id = 3");
        exec(w.db, "DELETE FROM notes WHERE id = 2");
        exec(w.db, "CREATE TABLE plain(x); INSERT INTO plain VALUES(1)");   /* still rows */
    }
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/fts_r.db", &nr, &nt);
    sqlite3 *r; sqlite3_open(DIR "/fts_r.db", &r);
    sqlite3_int64 hits = fts ? scalar(r, "SELECT count(*) FROM docs WHERE docs MATCH 'capture'") : -1;
    sqlite3_int64 nhits = fts ? scalar(r, "SELECT count(*) FROM notes_fts WHERE notes_fts MATCH 'capture'") : -1;
    sqlite3_int64 plainRows = fts ? scalar(w.db, "SELECT count(*) FROM _hpplite_pending WHERE tbl = 'plain'") : -1;
    sqlite3_close(r);
    close_writer(&w);
    if (why) FAIL(why);
    if (fts && hits != 1) FAILF("replica search: %lld hits, want 1", (long long)hits);
    if (fts && nhits != 1) FAILF("replica search, external content: %lld hits, want 1", (long long)nhits);
    if (fts && plainRows != 1) FAILF("%lld row records for an ordinary table, want 1", (long long)plainRows);
    PASS();
}

static void test_failures_and_rollbacks(void) {
    TEST("failed statements and rolled-back transactions leave no records; changes() and last_insert_rowid() as plain SQLite");
    reset_dir();
    W w;
    if (!open_writer(DIR "/fail.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v UNIQUE)");
    exec(w.db, "INSERT INTO t VALUES(1, 'a')");
    sqlite3_int64 before = scalar(w.db, "SELECT count(*) FROM _hpplite_pending WHERE op > 0");
    exec(w.db, "INSERT INTO t VALUES(2, 'a')");                       /* UNIQUE fails */
    exec(w.db, "BEGIN; INSERT INTO t VALUES(3, 'c'); ROLLBACK");
    exec(w.db, "BEGIN; INSERT INTO t VALUES(4, 'd'); INSERT INTO t VALUES(5, 'a'); COMMIT");   /* the second fails, the txn commits */
    exec(w.db, "INSERT INTO t VALUES(6, 'f')");
    sqlite3_int64 lir = sqlite3_last_insert_rowid(w.db);
    exec(w.db, "UPDATE t SET v = v WHERE id = 6");
    int ch = sqlite3_changes(w.db);
    sqlite3_int64 after = scalar(w.db, "SELECT count(*) FROM _hpplite_pending WHERE op > 0");
    int nr, nt;
    const char *why = replica_differs(&w, DIR "/fail_r.db", &nr, &nt);
    close_writer(&w);
    if (lir != 6) FAILF("last_insert_rowid %lld, want 6", (long long)lir);
    if (ch != 1) FAILF("changes() %d, want 1", ch);
    if (after - before != 3) FAILF("%lld records, want 3 (rows 4, 6, the update of 6)", (long long)(after - before));
    if (why) FAIL(why);
    PASS();
}

static void test_groups(void) {
    TEST("records of one transaction share its group, as v1 names them");
    reset_dir();
    W w;
    if (!open_writer(DIR "/grp.db", &w)) FAIL("open");
    exec(w.db, "CREATE TABLE t(v)");
    exec(w.db, "INSERT INTO t VALUES(1)");
    exec(w.db, "BEGIN; INSERT INTO t VALUES(2); UPDATE t SET v = v + 10; DELETE FROM t WHERE v = 11; COMMIT");
    exec(w.db, "INSERT INTO t VALUES(3)");
    sqlite3_int64 groups = scalar(w.db, "SELECT count(DISTINCT CASE WHEN txn = 0 THEN id ELSE txn END) FROM _hpplite_pending WHERE op > 0");
    sqlite3_int64 biggest = scalar(w.db, "SELECT max(n) FROM (SELECT count(*) n FROM _hpplite_pending WHERE op > 0 GROUP BY CASE WHEN txn = 0 THEN id ELSE txn END)");
    close_writer(&w);
    if (groups != 3 || biggest != 4) FAILF("groups %lld (want 3), largest %lld records (want 4: the insert, the update of 2 rows, the delete)", (long long)groups, (long long)biggest);
    PASS();
}

int main(void) {
    printf("HPPLite row capture\n");
    test_basic_types();
    test_effects_not_text();
    test_cascades_and_app_triggers();
    test_conflicts_upserts_bulk();
    test_without_rowid_and_generated();
    test_table_rebuild_migration();
    test_table_from_another_connection();
    test_autoincrement_and_fts5();
    test_failures_and_rollbacks();
    test_groups();
    printf("\n%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
