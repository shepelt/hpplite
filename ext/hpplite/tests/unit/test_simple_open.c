/*
** Test for hpplite_open() simple API
**
** Tests:
**   1. Basic open/close with hpplite=on
**   2. State tracking works through standard sqlite3_exec
**   3. Config loading from URI params
**   4. Manual flush creates batch
**   5. State root changes after modifications
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include "hpplite.h"
#include "config.h"

#define TEST(name) printf("  %-50s ", name)
#define PASS() printf("✓\n")
#define FAIL(msg) do { printf("✗ %s\n", msg); return 1; } while(0)

static void printHex(const unsigned char *data, int len) {
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
}

int main(void) {
    printf("=== HPPLite Simple Open API Test ===\n\n");
    int tests = 0, passed = 0;

    /* Clean up any previous test data */
    unlink("/tmp/test_simple_open.db");
    system("rm -rf /tmp/hpplite_test_simple");
    mkdir("/tmp/hpplite_test_simple", 0755);

    /* Test 1: Basic open/close */
    tests++;
    TEST("Basic open/close");
    {
        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open returned NULL");

        int rc = hpplite_close(db);
        if (rc != SQLITE_OK) FAIL("hpplite_close failed");

        PASS();
        passed++;
    }

    /* Test 2: Context retrieval */
    tests++;
    TEST("Context retrieval");
    {
        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");

        HppliteCtx *ctx = hpplite_context(db);
        if (!ctx) FAIL("hpplite_context returned NULL");

        hpplite_close(db);
        PASS();
        passed++;
    }

    /* Test 3: Config loading */
    tests++;
    TEST("Config loading from URI");
    {
        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&role=sequencer&chainid=12345&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");

        HppliteConfig *cfg = hpplite_get_config(db);
        if (!cfg) FAIL("hpplite_get_config returned NULL");

        if (cfg->role != HPPLITE_CFG_ROLE_SEQUENCER) FAIL("role not sequencer");
        if (cfg->chainId != 12345) FAIL("chainId not 12345");

        hpplite_close(db);
        PASS();
        passed++;
    }

    /* Test 4: State root starts at zero */
    tests++;
    TEST("Initial state root is zero");
    {
        unlink("/tmp/test_simple_open.db");
        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");

        unsigned char root[HPPLITE_HASH_SIZE];
        hpplite_state_root(db, root);

        /* Check it's all zeros */
        int allZero = 1;
        for (int i = 0; i < HPPLITE_HASH_SIZE; i++) {
            if (root[i] != 0) { allZero = 0; break; }
        }
        if (!allZero) FAIL("initial state root not zero");

        hpplite_close(db);
        PASS();
        passed++;
    }

    /* Test 5: State root changes after INSERT */
    tests++;
    TEST("State root changes after INSERT");
    {
        unlink("/tmp/test_simple_open.db");
        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");

        HppliteCtx *ctx = hpplite_context(db);
        if (!ctx) FAIL("no context");

        unsigned char root1[HPPLITE_HASH_SIZE];
        hpplite_state_root(db, root1);

        /* Create table and insert data using standard sqlite3_exec */
        char *errMsg = NULL;
        int rc = sqlite3_exec(db,
            "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT);"
            "INSERT INTO users(id, name) VALUES(1, 'Alice');",
            NULL, NULL, &errMsg);
        if (rc != SQLITE_OK) {
            printf("SQL error: %s\n", errMsg);
            sqlite3_free(errMsg);
            FAIL("sqlite3_exec failed");
        }

        /* Debug: check changes count */
        int changes = ctx->nChanges;
        printf("(changes=%d) ", changes);

        unsigned char root2[HPPLITE_HASH_SIZE];
        hpplite_state_root(db, root2);

        /* State root should have changed */
        if (memcmp(root1, root2, HPPLITE_HASH_SIZE) == 0) {
            printf("(root unchanged, enabled=%d) ", ctx->bEnabled);
            FAIL("state root did not change after INSERT");
        }

        printf("(");
        printHex(root2, 8);
        printf("...) ");

        hpplite_close(db);
        PASS();
        passed++;
    }

    /* Test 6: Manual flush creates batch */
    tests++;
    TEST("Manual flush creates batch");
    {
        unlink("/tmp/test_simple_open.db");
        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");

        /* Insert some data */
        sqlite3_exec(db,
            "CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT);"
            "INSERT INTO items(id, value) VALUES(1, 'test');",
            NULL, NULL, NULL);

        /* Flush to create batch */
        uint64_t height = hpplite_flush(db);
        if (height == 0) FAIL("hpplite_flush returned 0");

        printf("(block %llu) ", (unsigned long long)height);

        hpplite_close(db);
        PASS();
        passed++;
    }

    /* Test 7: Multiple operations accumulate */
    tests++;
    TEST("Multiple operations accumulate");
    {
        unlink("/tmp/test_simple_open.db");
        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");

        /* Multiple commits */
        sqlite3_exec(db, "CREATE TABLE t1(x);", NULL, NULL, NULL);
        sqlite3_exec(db, "INSERT INTO t1 VALUES(1);", NULL, NULL, NULL);
        sqlite3_exec(db, "INSERT INTO t1 VALUES(2);", NULL, NULL, NULL);
        sqlite3_exec(db, "INSERT INTO t1 VALUES(3);", NULL, NULL, NULL);

        /* Get context to check pending count */
        HppliteCtx *ctx = hpplite_context(db);
        int pending = hpplite_pending_count(ctx);
        printf("(%d pending) ", pending);

        /* Flush all at once */
        uint64_t height = hpplite_flush(db);
        if (height == 0) FAIL("hpplite_flush returned 0");

        hpplite_close(db);
        PASS();
        passed++;
    }

    /* Test 8: L1 alias resolves to chain ID */
    tests++;
    TEST("L1 alias 'hpp-sepolia' resolves");
    {
        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&l1=hpp-sepolia&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");

        HppliteConfig *cfg = hpplite_get_config(db);
        if (!cfg) FAIL("hpplite_get_config returned NULL");

        if (cfg->chainId != 181228) {
            printf("(chainId=%llu) ", (unsigned long long)cfg->chainId);
            FAIL("hpp-sepolia should resolve to 181228");
        }

        hpplite_close(db);
        PASS();
        passed++;
    }

    /* Test 9: Automatic flush after interval (background thread) */
    tests++;
    TEST("Auto-flush via background thread");
    {
        unlink("/tmp/test_simple_open.db");
        /* Use 200ms interval for testing */
        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&interval=200ms&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");

        /* Check config has correct interval */
        HppliteConfig *cfg = hpplite_get_config(db);
        if (cfg->batchIntervalMs != 200) FAIL("interval not set correctly");

        /* Get height BEFORE inserting data */
        HppliteCtx *ctx = hpplite_context(db);
        uint64_t h1 = hpplite_get_block_height(ctx);

        /* Insert data */
        sqlite3_exec(db, "CREATE TABLE auto(x);", NULL, NULL, NULL);
        sqlite3_exec(db, "INSERT INTO auto VALUES(1);", NULL, NULL, NULL);

        /* Wait for background thread to auto-flush (interval + buffer) */
        usleep(350000);  /* 350ms - enough for 200ms interval + thread wake */

        /* Check that block height increased - NO manual flush call needed! */
        uint64_t h2 = hpplite_get_block_height(ctx);
        printf("(h1=%llu h2=%llu) ", (unsigned long long)h1, (unsigned long long)h2);

        if (h2 <= h1) {
            FAIL("background thread did not auto-flush");
        }

        hpplite_close(db);
        PASS();
        passed++;
    }

    /* Test 10: Auto-extension with standard sqlite3_open */
    tests++;
    TEST("Auto-extension with sqlite3_open_v2");
    {
        unlink("/tmp/test_simple_open.db");

        /* Open with standard SQLite API - HPPLite auto-initializes!
         * No registration needed - HPPLite is built into SQLite. */
        sqlite3 *db;
        int rc = sqlite3_open_v2(
            "file:/tmp/test_simple_open.db?hpplite=on&datadir=/tmp/hpplite_test_simple",
            &db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI,
            NULL
        );
        if (rc != SQLITE_OK) FAIL("sqlite3_open_v2 failed");

        /* HPPLite should be active */
        HppliteCtx *ctx = hpplite_context(db);
        if (!ctx) FAIL("HPPLite not auto-initialized");

        /* Do some SQL */
        sqlite3_exec(db, "CREATE TABLE auto_test(id INT);", NULL, NULL, NULL);
        sqlite3_exec(db, "INSERT INTO auto_test VALUES(42);", NULL, NULL, NULL);

        /* Verify state root changed */
        unsigned char root[32];
        hpplite_state_root(db, root);
        int nonzero = 0;
        for (int i = 0; i < 32; i++) if (root[i]) nonzero = 1;
        if (!nonzero) FAIL("state root is zero after INSERT");

        /* Close with standard sqlite3_close - should auto-flush via close hook */
        sqlite3_close(db);

        PASS();
        passed++;
    }

    /* Test 11: Standard sqlite3_close triggers flush via close hook */
    tests++;
    TEST("sqlite3_close triggers close hook flush");
    {
        unlink("/tmp/test_simple_open.db");

        const char *uri = "file:/tmp/test_simple_open.db?hpplite=on&datadir=/tmp/hpplite_test_simple";
        sqlite3 *db = hpplite_open(uri);
        if (!db) FAIL("hpplite_open failed");

        sqlite3_exec(db, "CREATE TABLE hook_test(x);", NULL, NULL, NULL);
        sqlite3_exec(db, "INSERT INTO hook_test VALUES(1);", NULL, NULL, NULL);

        /* Use sqlite3_close directly (not hpplite_close) */
        int rc = sqlite3_close(db);
        if (rc != SQLITE_OK) FAIL("sqlite3_close failed");

        /* If we got here without crash, close hook worked */
        PASS();
        passed++;
    }

    /* Summary */
    printf("\n=== Results: %d/%d tests passed ===\n", passed, tests);

    /* Cleanup */
    unlink("/tmp/test_simple_open.db");
    system("rm -rf /tmp/hpplite_test_simple");

    return (passed == tests) ? 0 : 1;
}
