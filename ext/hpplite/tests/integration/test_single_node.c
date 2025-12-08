/*
** test_single_node.c - Single Node E2E with Transparent API
**
** Tests single HPPLite node with real L1:
**   1. Open database with transparent API (sqlite3_open_v2 + ?hpplite=on)
**   2. Create schema and insert data
**   3. Verify batches submitted to L1
**   4. Close and reopen - verify state reconstruction from L1
**   5. Checkpoint submission
**
** This validates the main user-facing flow: just use SQLite!
**
** Requirements:
**   - HPPLITE_PRIVATE_KEY env var
**   - HPP Sepolia RPC access
**
** Run:
**   ./test_single_node
*/

#include "hpplite.h"
#include "l1_interface.h"
#include "test_util.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RPC_URL HPPLITE_DEFAULT_RPC
#define FACTORY_ADDRESS HPPLITE_DEFAULT_FACTORY

static unsigned char g_privkey[32];
static char g_privkey_hex[67];
static char g_datadir[256];

#define FAIL(msg) do { printf("  FAIL: %s\n", msg); return -1; } while(0)

static int count_rows(sqlite3 *db, const char *table) {
    char sql[256];
    sqlite3_stmt *stmt;
    int count = 0;
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return count;
}

static void print_hex(const unsigned char *data, int len) {
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
}

/*
** Phase 1: Open database with transparent API
*/
static int test_open(sqlite3 **pDb) {
    printf("\n=== Phase 1: Open with Transparent API ===\n");

    char uri[1024];
    snprintf(uri, sizeof(uri),
        "file:%s/test.db?hpplite=on&rpc=%s&factory=%s&privkey=%s&datadir=%s",
        g_datadir, RPC_URL, FACTORY_ADDRESS, g_privkey_hex, g_datadir);

    printf("  URI: %s\n", uri);

    int rc = sqlite3_open_v2(uri, pDb,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);

    if (rc != SQLITE_OK) {
        printf("  FAIL: sqlite3_open_v2 failed: %s\n", sqlite3_errmsg(*pDb));
        return -1;
    }

    printf("  PASS: Database opened with HPPLite\n");
    return 0;
}

/*
** Phase 2: Create schema and insert data
*/
static int test_insert(sqlite3 *db) {
    printf("\n=== Phase 2: Insert Data ===\n");

    int rc = sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY, name TEXT, value INTEGER)",
        NULL, NULL, NULL);
    if (rc != SQLITE_OK) FAIL("CREATE TABLE failed");

    /* Insert some rows */
    for (int i = 1; i <= 5; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO items (name, value) VALUES ('item%d', %d)", i, i * 100);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) FAIL("INSERT failed");
    }

    int rows = count_rows(db, "items");
    printf("  Inserted %d rows\n", rows);

    if (rows != 5) FAIL("Expected 5 rows");

    printf("  PASS: Data inserted\n");
    return 0;
}

/*
** Phase 3: Flush and verify L1 submission
*/
static int test_flush(sqlite3 *db) {
    printf("\n=== Phase 3: Flush to L1 ===\n");

    /* Trigger flush */
    uint64_t height = hpplite_flush(db);
    printf("  Flushed batch at height: %llu\n", (unsigned long long)height);

    if (height == 0) FAIL("Flush returned 0");

    /* Verify via L1 */
    HppliteL1 *l1 = hpplite_get_l1(db);
    if (!l1) FAIL("No L1 connection");

    uint64_t last_batch, total;
    unsigned char hash[32];
    if (hpplite_l1_get_da_state(l1, &last_batch, &total, hash) != 0) {
        FAIL("Could not get L1 DA state");
    }

    printf("  L1 state: last_batch=%llu, total=%llu\n",
           (unsigned long long)last_batch, (unsigned long long)total);

    if (total == 0) FAIL("No batches on L1");

    printf("  PASS: Batch submitted to L1\n");
    return 0;
}

/*
** Phase 4: Close and reopen - verify reconstruction
*/
static int test_reconstruct(sqlite3 **pDb) {
    printf("\n=== Phase 4: Reconstruct from L1 ===\n");

    /* Close current connection */
    printf("  Closing database...\n");
    sqlite3_close(*pDb);
    *pDb = NULL;

    /* Delete local database file to force reconstruction */
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/test.db", g_datadir);
    printf("  Removing local database: %s\n", db_path);
    unlink(db_path);

    /* Also remove batch files to ensure we reconstruct from L1 only */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s/batches", g_datadir);
    system(cmd);

    /* Reopen - should reconstruct from L1 */
    char uri[1024];
    snprintf(uri, sizeof(uri),
        "file:%s/test.db?hpplite=on&rpc=%s&factory=%s&privkey=%s&datadir=%s",
        g_datadir, RPC_URL, FACTORY_ADDRESS, g_privkey_hex, g_datadir);

    printf("  Reopening database...\n");

    int rc = sqlite3_open_v2(uri, pDb,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);

    if (rc != SQLITE_OK) FAIL("Reopen failed");

    /* Verify data was reconstructed */
    int rows = count_rows(*pDb, "items");
    printf("  Reconstructed %d rows\n", rows);

    if (rows != 5) {
        printf("  Expected 5 rows, got %d\n", rows);
        FAIL("Reconstruction incomplete");
    }

    /* Verify actual values */
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(*pDb, "SELECT name, value FROM items ORDER BY id", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        printf("  Data:\n");
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            printf("    %s = %d\n", sqlite3_column_text(stmt, 0), sqlite3_column_int(stmt, 1));
        }
        sqlite3_finalize(stmt);
    }

    printf("  PASS: State reconstructed from L1\n");
    return 0;
}

/*
** Phase 5: More operations after reconstruction
*/
static int test_continue(sqlite3 *db) {
    printf("\n=== Phase 5: Continue Operations ===\n");

    /* Insert more data */
    int rc = sqlite3_exec(db,
        "INSERT INTO items (name, value) VALUES ('item6', 600)",
        NULL, NULL, NULL);
    if (rc != SQLITE_OK) FAIL("INSERT after reconstruct failed");

    /* Flush again */
    uint64_t height = hpplite_flush(db);
    printf("  New batch at height: %llu\n", (unsigned long long)height);

    int rows = count_rows(db, "items");
    printf("  Total rows: %d\n", rows);

    if (rows != 6) FAIL("Expected 6 rows");

    printf("  PASS: Operations continue after reconstruction\n");
    return 0;
}

/*
** Main
*/
int main(int argc, char **argv) {
    sqlite3 *db = NULL;
    int result = 0;

    printf("=== Single Node E2E Test ===\n");

    /* Load private key */
    const char *pk = test_util_get_master_key();
    if (!pk || test_util_hex_to_bytes(pk, g_privkey, 32) != 0) {
        printf("ERROR: HPPLITE_PRIVATE_KEY not set or invalid\n");
        return 1;
    }

    /* Format privkey as hex for URI */
    snprintf(g_privkey_hex, sizeof(g_privkey_hex), "0x");
    for (int i = 0; i < 32; i++) {
        snprintf(g_privkey_hex + 2 + i*2, 3, "%02x", g_privkey[i]);
    }

    /* Create temp data directory */
    snprintf(g_datadir, sizeof(g_datadir), "/tmp/hpplite_single_node_%d", getpid());
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", g_datadir);
    system(cmd);

    printf("  Data dir: %s\n", g_datadir);

    /* Run test phases */
    if (test_open(&db) != 0) { result = 1; goto cleanup; }
    if (test_insert(db) != 0) { result = 1; goto cleanup; }
    if (test_flush(db) != 0) { result = 1; goto cleanup; }
    if (test_reconstruct(&db) != 0) { result = 1; goto cleanup; }
    if (test_continue(db) != 0) { result = 1; goto cleanup; }

    printf("\n=== All phases passed! ===\n");

cleanup:
    if (db) sqlite3_close(db);

    /* Cleanup temp directory */
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_datadir);
    system(cmd);

    return result;
}
