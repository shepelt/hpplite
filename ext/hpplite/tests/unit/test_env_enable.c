/*
** HPPLite turned on from the environment (#55)
**
** Drivers that cannot open URI filenames (better-sqlite3, the default of
** Drizzle and Knex) open a plain path. HPPLITE_FILES names the files HPPLite
** handles - glob patterns, ':'-separated, matched against the full path and
** the file name - and HPPLITE_PARAMS carries what the URI would have
** (l1=hpp-sepolia&interval=5s ...). Everything else opens as plain SQLite.
*/

#include "node.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { printf("  Testing: %s... ", name); tests_run++; fflush(stdout); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

#define DIR "/tmp/hpplite_test_env"
#define KEY "0x8182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0"

static void reset_dir(void) { (void)!system("rm -rf " DIR " && mkdir -p " DIR); }

static sqlite3_int64 scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st;
    sqlite3_int64 v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static void test_plain_path_enabled(void) {
    TEST("a plain-path open of a file named in HPPLITE_FILES is an HPPLite connection");
    reset_dir();
    setenv("HPPLITE_FILES", "*/app*.db:" DIR "/exact.db", 1);
    setenv("HPPLITE_PARAMS", "privkey=" KEY "&datadir=" DIR "&interval=0", 1);
    sqlite3 *db = NULL;
    if (sqlite3_open(DIR "/app.db", &db) != SQLITE_OK) FAIL("open");
    HppliteNode *n = hpplite_get_node(db);
    if (!n) { sqlite3_close(db); FAIL("HPPLite did not start on a matching file"); }
    if (n->db != db) { sqlite3_close(db); FAIL("node is not on the application connection"); }
    if (n->bkDb && hpplite_get_node(n->bkDb)) { sqlite3_close(db); FAIL("the bookkeeping connection enabled HPPLite on itself"); }
    sqlite3_exec(db, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL, NULL, NULL);
    sqlite3_int64 pending = scalar(db, "SELECT pending FROM hpplite_status");
    sqlite3_close(db);
    if (pending != 2) FAIL("writes not captured");

    if (sqlite3_open(DIR "/exact.db", &db) != SQLITE_OK) FAIL("open exact");
    int exact = hpplite_get_node(db) != NULL;
    sqlite3_close(db);
    if (!exact) FAIL("a full-path pattern did not match");
    PASS();
}

static void test_others_untouched(void) {
    TEST("files not named, :memory: and an unset HPPLITE_FILES stay plain SQLite");
    reset_dir();
    setenv("HPPLITE_FILES", "*/app*.db", 1);
    setenv("HPPLITE_PARAMS", "privkey=" KEY "&datadir=" DIR "&interval=0", 1);
    sqlite3 *db = NULL;
    sqlite3_open(DIR "/cache.db", &db);
    int other = hpplite_get_node(db) != NULL;
    sqlite3_close(db);
    sqlite3_open(":memory:", &db);
    int mem = hpplite_get_node(db) != NULL;
    sqlite3_close(db);
    unsetenv("HPPLITE_FILES");
    sqlite3_open(DIR "/app.db", &db);
    int unset = hpplite_get_node(db) != NULL;
    sqlite3_close(db);
    if (other) FAIL("a file not named was enabled");
    if (mem) FAIL(":memory: was enabled");
    if (unset) FAIL("enabled without HPPLITE_FILES");
    PASS();
}

static void test_uri_still_wins(void) {
    TEST("a URI with hpplite= decides for itself; HPPLITE_PARAMS do not override it");
    reset_dir();
    setenv("HPPLITE_FILES", "*", 1);
    setenv("HPPLITE_PARAMS", "privkey=" KEY "&datadir=" DIR "&interval=0", 1);
    sqlite3 *db = NULL;
    sqlite3_open_v2("file:" DIR "/off.db?hpplite=off", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    int off = hpplite_get_node(db) == NULL;
    sqlite3_close(db);
    unsetenv("HPPLITE_FILES");
    unsetenv("HPPLITE_PARAMS");
    if (!off) FAIL("hpplite=off in the URI was overridden by the environment");
    PASS();
}

static void test_truncating_sqlite_refused(void) {
    TEST("a SQLite that truncates bound values in expanded SQL (better-sqlite3): statement capture is refused at open");
    reset_dir();
    setenv("HPPLITE_TEST_TRUNCATING_EXPAND", "1", 1);      /* as SQLITE_TRACE_SIZE_LIMIT=32 builds do */
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2("file:" DIR "/trunc.db?hpplite=on&capture=text&privkey=" KEY "&datadir=" DIR, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    const char *msg = db ? sqlite3_errmsg(db) : "";
    int named = strstr(msg, "SQLITE_TRACE_SIZE_LIMIT") != NULL;
    sqlite3_close(db);
    unsetenv("HPPLITE_TEST_TRUNCATING_EXPAND");
    if (rc == SQLITE_OK) FAIL("opened: writes would replay with truncated values");
    if (!named) FAIL("the error does not say why");
    PASS();
}

/* #62: row capture does not read bound values back from expanded SQL, so
** such a SQLite serves it; what it still logs as text may bind nothing */
static void test_truncating_sqlite_rows(void) {
    TEST("the same SQLite with row capture: opens, records long bound values whole, refuses a bound text-logged write");
    reset_dir();
    setenv("HPPLITE_TEST_TRUNCATING_EXPAND", "1", 1);
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    char longv[301];
    memset(longv, 'x', 300); longv[300] = 0;
    int rc = sqlite3_open_v2("file:" DIR "/trunc_rows.db?hpplite=on&privkey=" KEY "&datadir=" DIR, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    int created = 0, bound = 0, ftsRefused = 0, ftsLiteral = 0;
    long long recBytes = -1;
    if (rc == SQLITE_OK) {
        created = sqlite3_exec(db, "CREATE TABLE t(v); CREATE VIRTUAL TABLE f USING fts5(body)", NULL, NULL, NULL) == SQLITE_OK;
        if (sqlite3_prepare_v2(db, "INSERT INTO t VALUES(?1)", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, longv, -1, SQLITE_STATIC);
            bound = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
        }
        if (sqlite3_prepare_v2(db, "INSERT INTO f VALUES(?1)", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, longv, -1, SQLITE_STATIC);
            ftsRefused = sqlite3_step(st) == SQLITE_INTERRUPT;
            sqlite3_finalize(st);
        }
        ftsLiteral = sqlite3_exec(db, "INSERT INTO f VALUES('values in the text')", NULL, NULL, NULL) == SQLITE_OK;
        if (sqlite3_prepare_v2(db, "SELECT max(length(rec)) FROM _hpplite_pending WHERE tbl = 't'", -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) recBytes = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
    }
    sqlite3_close(db);
    unsetenv("HPPLITE_TEST_TRUNCATING_EXPAND");
    if (rc != SQLITE_OK) FAIL("row capture refused at open");
    if (!created || !bound) FAIL("schema or the bound insert failed");
    if (recBytes < 300) { printf("(%lld) ", recBytes); FAIL("the record does not carry the whole value"); }
    if (!ftsRefused) FAIL("a bound write logged as text ran: it would replay cut");
    if (!ftsLiteral) FAIL("a text-logged write without bound values must run");
    PASS();
}

int main(void) {
    printf("=== HPPLite enabled from the environment (#55) ===\n");
    test_plain_path_enabled();
    test_others_untouched();
    test_uri_still_wins();
    test_truncating_sqlite_refused();
    test_truncating_sqlite_rows();
    printf("\n%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
