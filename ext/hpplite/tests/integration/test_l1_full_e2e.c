/*
** HPPLite Full E2E Test with Real L1 (HPP Sepolia)
**
** Uses transparent sqlite3_open_v2 API with factory auto-deploy:
**   1. Create funded wallet via cast
**   2. sqlite3_open_v2 with factory URI -> auto-deploys rollup (~3-4s)
**   3. Run SQL operations (auto-batched to L1)
**   4. Close and reopen -> state persists from L1
**
** This demonstrates the simplest possible L1 integration - just use SQLite!
**
** URI format:
**   file:path?hpplite=on&rpc=URL&factory=ADDR&privkey=KEY&datadir=PATH
**
** Requirements:
**   - cast (foundry) installed
**   - Master wallet with ETH (via env HPPLITE_PRIVATE_KEY)
**   - HPP Sepolia RPC access
**
** Run:
**   ./test_l1_full_e2e              # Default: 50 operations
**   ./test_l1_full_e2e --quick      # Quick: 10 operations
*/

#include "hpplite.h"
#include "l1_interface.h"
#include "test_util.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

/* Get time in milliseconds */
static long get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

#define RPC_URL HPPLITE_DEFAULT_RPC
#define FACTORY_ADDRESS HPPLITE_DEFAULT_FACTORY

static int g_total_ops = 50;

static void parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ops") == 0 && i + 1 < argc) {
            g_total_ops = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--quick") == 0) {
            g_total_ops = 10;
        } else if (strcmp(argv[i], "--extended") == 0) {
            g_total_ops = 100;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: test_l1_full_e2e [options]\n");
            printf("Options:\n");
            printf("  --ops N      Run N operations (default: 50)\n");
            printf("  --quick      Quick mode: 10 operations\n");
            printf("  --extended   Extended mode: 100 operations\n");
            exit(0);
        }
    }
}

/* Count rows in a table */
static int count_rows(sqlite3 *db, const char *table) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    sqlite3_stmt *stmt;
    int count = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

int main(int argc, char **argv) {
    parse_args(argc, argv);

    printf("=== HPPLite Transparent API E2E Test ===\n");
    printf("    Operations: %d\n\n", g_total_ops);

    int success = 1;
    sqlite3 *db = NULL;
    char uri[1024];
    char privkey_hex[128];
    char address[64];

    /* Check for cast */
    if (!test_util_has_cast()) {
        printf("ERROR: 'cast' not found. Install foundry: https://getfoundry.sh\n");
        return 1;
    }

    /* Check for master key */
    const char *master_key = getenv("HPPLITE_PRIVATE_KEY");
    if (!master_key) master_key = getenv("HPPLITE_MASTER_KEY");
    if (!master_key) {
        printf("ERROR: HPPLITE_PRIVATE_KEY not set\n");
        return 1;
    }

    /* Cleanup */
    system("rm -rf /tmp/hpplite_e2e_test");
    system("mkdir -p /tmp/hpplite_e2e_test");

    /* ============================================================
     * PHASE 1: Create funded wallet
     * ============================================================ */
    printf("Phase 1: Create funded wallet\n");
    time_t t0 = time(NULL);
    if (test_util_create_funded_wallet(privkey_hex, sizeof(privkey_hex),
                                        address, sizeof(address),
                                        RPC_URL, "0.01ether") != 0) {
        printf("  FAIL: Could not create wallet\n");
        return 1;
    }
    printf("  Wallet: %s (%lds)\n", address, time(NULL)-t0);

    /* ============================================================
     * PHASE 2: Open database with factory URI (auto-deploys!)
     * ============================================================ */
    printf("\nPhase 2: Open database (auto-deploy via factory)\n");

    /* First check if this wallet already has a rollup */
    uint8_t privkey_bytes[32];
    test_util_hex_to_bytes(privkey_hex, privkey_bytes, 32);
    uint8_t existing_rollup[20];
    int has_rollup = hpplite_l1_factory_get_my_rollup(RPC_URL, FACTORY_ADDRESS,
                                                       privkey_bytes, existing_rollup);
    if (has_rollup == 1) {
        printf("  Note: Wallet already has rollup (unexpected for new wallet!)\n");
    } else {
        printf("  Wallet has no rollup - will auto-deploy\n");
    }

    /* Build URI: file:path?hpplite=on&rpc=...&factory=...&privkey=... */
    snprintf(uri, sizeof(uri),
        "file:/tmp/hpplite_e2e_test/db.sqlite"
        "?hpplite=on"
        "&rpc=%s"
        "&factory=%s"
        "&privkey=%s"
        "&datadir=/tmp/hpplite_e2e_test",
        RPC_URL, FACTORY_ADDRESS, privkey_hex);

    long t_ms = get_time_ms();
    int rc = sqlite3_open_v2(uri, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    long open_ms = get_time_ms() - t_ms;
    if (rc != SQLITE_OK) {
        printf("  FAIL: sqlite3_open_v2 failed: %s\n", sqlite3_errmsg(db));
        success = 0;
        goto cleanup;
    }
    printf("  Database opened in %ldms (%.1fs)\n", open_ms, open_ms/1000.0);

    /* Verify L1 connection was established */
    HppliteL1 *l1 = hpplite_get_l1(db);
    if (!l1) {
        printf("  FAIL: L1 connection not established!\n");
        printf("  (Factory auto-deploy may have failed silently)\n");
        success = 0;
        goto cleanup;
    }
    printf("  L1 connection: OK\n");

    /* ============================================================
     * PHASE 3: Create schema and insert data
     * ============================================================ */
    printf("\nPhase 3: Run SQL operations\n");

    /* Create tables */
    rc = sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS users(id INTEGER PRIMARY KEY, name TEXT, email TEXT);"
        "CREATE TABLE IF NOT EXISTS products(id INTEGER PRIMARY KEY, name TEXT, price REAL);",
        NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        printf("  FAIL: CREATE TABLE failed: %s\n", sqlite3_errmsg(db));
        success = 0;
        goto cleanup;
    }
    printf("  Schema created\n");

    /* Insert data */
    printf("  Inserting %d rows...\n", g_total_ops);
    for (int i = 1; i <= g_total_ops; i++) {
        char sql[256];
        if (i % 2 == 0) {
            snprintf(sql, sizeof(sql),
                "INSERT INTO users VALUES(%d, 'User %d', 'user%d@test.com')", i, i, i);
        } else {
            snprintf(sql, sizeof(sql),
                "INSERT INTO products VALUES(%d, 'Product %d', %.2f)", i, i, i * 1.5);
        }
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            printf("  FAIL: INSERT failed at %d: %s\n", i, sqlite3_errmsg(db));
            success = 0;
            goto cleanup;
        }
        if (i % 10 == 0) {
            printf("    %d/%d rows inserted\n", i, g_total_ops);
        }
    }

    int users_count = count_rows(db, "users");
    int products_count = count_rows(db, "products");
    printf("  Inserted: %d users, %d products\n", users_count, products_count);

    /* Force flush pending batches */
    sqlite3_exec(db, "SELECT hpplite_flush()", NULL, NULL, NULL);
    printf("  Batches flushed to L1\n");

    /* Close database */
    sqlite3_close(db);
    db = NULL;
    printf("  Database closed\n");

    /* ============================================================
     * PHASE 4: Reopen and verify state persisted
     * ============================================================ */
    printf("\nPhase 4: Reopen and verify persistence\n");

    /* Small delay to ensure L1 state is available */
    sleep(2);

    rc = sqlite3_open_v2(uri, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI, NULL);
    if (rc != SQLITE_OK) {
        printf("  FAIL: Reopen failed: %s\n", sqlite3_errmsg(db));
        success = 0;
        goto cleanup;
    }
    printf("  Database reopened\n");

    /* Verify counts match */
    int users_after = count_rows(db, "users");
    int products_after = count_rows(db, "products");
    printf("  After reopen: %d users, %d products\n", users_after, products_after);

    if (users_after == users_count && products_after == products_count) {
        printf("  PASS: State persisted correctly!\n");
    } else {
        printf("  FAIL: Row counts don't match!\n");
        printf("    Expected: %d users, %d products\n", users_count, products_count);
        printf("    Got: %d users, %d products\n", users_after, products_after);
        success = 0;
    }

    /* ============================================================
     * PHASE 5: Query data to verify integrity
     * ============================================================ */
    printf("\nPhase 5: Verify data integrity\n");

    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, "SELECT id, name FROM users ORDER BY id LIMIT 3", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        printf("  Sample users:\n");
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            printf("    id=%d, name=%s\n",
                sqlite3_column_int(stmt, 0),
                sqlite3_column_text(stmt, 1));
        }
        sqlite3_finalize(stmt);
    }

    rc = sqlite3_prepare_v2(db, "SELECT id, name, price FROM products ORDER BY id LIMIT 3", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        printf("  Sample products:\n");
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            printf("    id=%d, name=%s, price=%.2f\n",
                sqlite3_column_int(stmt, 0),
                sqlite3_column_text(stmt, 1),
                sqlite3_column_double(stmt, 2));
        }
        sqlite3_finalize(stmt);
    }

cleanup:
    printf("\n=== Cleanup ===\n");
    if (db) sqlite3_close(db);
    system("rm -rf /tmp/hpplite_e2e_test");

    printf("\n=== Result: %s ===\n", success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}
