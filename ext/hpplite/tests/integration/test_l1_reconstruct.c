/*
** HPPLite L1 Reconstruction Integration Test
**
** Tests sequencer recovery from L1:
** 1. Create funded wallet via factory
** 2. Sequencer writes data → post to L1 → close
** 3. New sequencer instance (same privkey, fresh DB) → sync from L1
** 4. Verify reconstructed data matches original
**
** This tests that a sequencer can recover state from L1 if local data is lost.
**
** Requires: cast (from foundry), jq, .env with master key
**
** Run: ./test_l1_reconstruct
*/

#include <sqlite3.h>
#include "test_util.h"
#include "test_env.h"
#include "hpplite.h"
#include "l1_interface.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DATA_DIR "/tmp/hpplite_reconstruct"
#define DB_PATH DATA_DIR "/state.db"

static int tests_run = 0;
static int tests_passed = 0;

static int count_rows(sqlite3 *db, const char *table) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s", table);
    sqlite3_stmt *stmt;
    int count = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

static int test_l1_reconstruct(void) {
    printf("Test: Sequencer recovery from L1\n");
    tests_run++;

    /* Check dependencies */
    if (!test_util_has_cast()) {
        printf("  SKIP: cast not found\n");
        return -1;
    }

    /* Create funded wallet */
    printf("  Creating funded wallet...\n");
    char privkey[128], address[64];
    if (test_util_create_funded_wallet(privkey, sizeof(privkey),
                                        address, sizeof(address),
                                        HPPLITE_DEFAULT_RPC, "0.02ether") != 0) {
        printf("  FAIL: Could not create funded wallet\n");
        return -1;
    }
    printf("    Address: %s\n", address);

    /* Clean up directory */
    system("rm -rf " DATA_DIR " && mkdir -p " DATA_DIR);

    /*
     * Phase 1: Sequencer writes data to L1
     */
    printf("  Phase 1: Sequencer writes data to L1...\n");

    char uri[1024];
    snprintf(uri, sizeof(uri),
        "file:" DB_PATH
        "?hpplite=on"
        "&factory=%s"
        "&privkey=%s"
        "&l1=hpp-sepolia",
        HPPLITE_DEFAULT_FACTORY,
        privkey
    );

    sqlite3 *db1;
    int rc = sqlite3_open_v2(uri, &db1,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    if (rc != SQLITE_OK) {
        printf("  FAIL: Could not open sequencer DB: %s\n", sqlite3_errmsg(db1));
        return -1;
    }

    /* Create table and insert data */
    sqlite3_exec(db1, "CREATE TABLE test_data (id INTEGER PRIMARY KEY, value TEXT)", NULL, NULL, NULL);
    sqlite3_exec(db1, "INSERT INTO test_data VALUES (1, 'alpha')", NULL, NULL, NULL);
    sqlite3_exec(db1, "INSERT INTO test_data VALUES (2, 'beta')", NULL, NULL, NULL);
    sqlite3_exec(db1, "INSERT INTO test_data VALUES (3, 'gamma')", NULL, NULL, NULL);

    int original_count = count_rows(db1, "test_data");
    printf("    Wrote %d rows\n", original_count);

    /* Explicit flush to L1 */
    printf("    Flushing to L1...\n");
    uint64_t batchHeight = hpplite_flush(db1);
    printf("    Flushed batch height: %llu\n", (unsigned long long)batchHeight);

    /* Check L1 state */
    HppliteL1 *l1 = hpplite_get_l1(db1);
    if (l1) {
        printf("    L1 connection: OK\n");
        uint64_t lastBatch = 0, totalBatches = 0;
        unsigned char latestHash[32];
        if (hpplite_l1_get_da_state(l1, &lastBatch, &totalBatches, latestHash) == 0) {
            printf("    L1 DA state: lastBatch=%llu, totalBatches=%llu\n",
                   (unsigned long long)lastBatch, (unsigned long long)totalBatches);
        } else {
            printf("    L1 DA state: query failed\n");
        }
    } else {
        printf("    L1 connection: NONE\n");
    }

    /* Wait for L1 transaction to be mined */
    printf("    Waiting for L1 confirmation...\n");
    sleep(10);

    /* Check L1 state again */
    if (l1) {
        uint64_t lastBatch = 0, totalBatches = 0;
        unsigned char latestHash[32];
        if (hpplite_l1_get_da_state(l1, &lastBatch, &totalBatches, latestHash) == 0) {
            printf("    L1 DA state after wait: lastBatch=%llu, totalBatches=%llu\n",
                   (unsigned long long)lastBatch, (unsigned long long)totalBatches);
        }
    }

    sqlite3_close(db1);
    printf("    Sequencer closed\n");

    /* Delete local data to simulate data loss */
    printf("    Deleting local data (simulating data loss)...\n");
    system("rm -rf " DATA_DIR " && mkdir -p " DATA_DIR);

    /*
     * Phase 2: New sequencer instance recovers from L1
     */
    printf("  Phase 2: New sequencer recovers from L1...\n");

    /* Same URI - same rollup (same privkey), fresh local DB */
    snprintf(uri, sizeof(uri),
        "file:" DB_PATH
        "?hpplite=on"
        "&factory=%s"
        "&privkey=%s"
        "&l1=hpp-sepolia",
        HPPLITE_DEFAULT_FACTORY,
        privkey
    );

    sqlite3 *db2;
    rc = sqlite3_open_v2(uri, &db2,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    if (rc != SQLITE_OK) {
        printf("  FAIL: Could not open recovered DB: %s\n", sqlite3_errmsg(db2));
        return -1;
    }

    /* Check L1 connection and DA state */
    HppliteL1 *l1_2 = hpplite_get_l1(db2);
    if (l1_2) {
        printf("    L1 connection: OK\n");
        uint64_t lastBatch = 0, totalBatches = 0;
        unsigned char latestHash[32];
        if (hpplite_l1_get_da_state(l1_2, &lastBatch, &totalBatches, latestHash) == 0) {
            printf("    L1 DA state: lastBatch=%llu, totalBatches=%llu\n",
                   (unsigned long long)lastBatch, (unsigned long long)totalBatches);
        } else {
            printf("    L1 DA state: query failed\n");
        }
    } else {
        printf("    L1 connection: NONE (this is the problem!)\n");
    }

    /* L1 sync happens during open, so no need to wait */
    printf("    Sync from L1 should have happened during open\n");

    /*
     * Phase 3: Verify recovered data matches
     */
    printf("  Phase 3: Verify recovery...\n");

    int recovered_count = count_rows(db2, "test_data");
    printf("    Original rows: %d\n", original_count);
    printf("    Recovered rows: %d\n", recovered_count);

    if (recovered_count == original_count && original_count > 0) {
        printf("  PASS: Row counts match\n");
        tests_passed++;
    } else {
        printf("  FAIL: Row counts don't match (L1 sync not implemented yet)\n");
    }

    /* Verify specific data */
    if (recovered_count > 0) {
        sqlite3_stmt *stmt;
        rc = sqlite3_prepare_v2(db2, "SELECT value FROM test_data WHERE id = 2", -1, &stmt, NULL);
        if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            const char *value = (const char *)sqlite3_column_text(stmt, 0);
            if (value && strcmp(value, "beta") == 0) {
                printf("  PASS: Data content verified (id=2 -> 'beta')\n");
            } else {
                printf("  FAIL: Data content mismatch (expected 'beta', got '%s')\n", value);
            }
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db2);

    return 0;
}

int main(int argc, char **argv) {
    printf("test_l1_reconstruct: L1 data availability round-trip\n\n");

    test_l1_reconstruct();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
