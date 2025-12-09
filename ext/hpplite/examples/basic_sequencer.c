/*
** HPPLite Example: Basic Sequencer + Replay
**
** Demonstrates transparent SQLite integration with L1:
** 1. Write data using standard SQLite API
** 2. Data is automatically posted to L1
** 3. Open fresh database and reconstruct from L1
**
** This is 100% standard SQLite API - no HPPLite-specific calls needed!
**
** Requires: cast (from foundry), jq
**
** Usage:
**   ./basic_sequencer    # Loads master key from ../.env
**
** Build:
**   cd ../build && make basic_sequencer
*/

#include <sqlite3.h>
#include "test_util.h"
#include <stdio.h>
#include <unistd.h>

/* Force linker to include hpplite library (needed for static linking) */
extern void hpplite_register(void);

#define DATA_DIR "/tmp/hpplite_example"
#define DATA_DIR2 "/tmp/hpplite_example2"

static void query_users(sqlite3 *db) {
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, "SELECT * FROM users ORDER BY id", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            printf("  id=%d name=%s email=%s\n",
                   sqlite3_column_int(stmt, 0),
                   (const char*)sqlite3_column_text(stmt, 1),
                   (const char*)sqlite3_column_text(stmt, 2));
        }
        sqlite3_finalize(stmt);
    }
}

int main(void) {
    /* Force linker to include hpplite library */
    hpplite_register();

    printf("HPPLite Basic Sequencer + Replay Example\n");
    printf("=========================================\n");
    printf("(Using 100%% standard SQLite API)\n\n");

    /* Check dependencies */
    if (!test_util_has_cast()) {
        printf("ERROR: 'cast' not found. Install foundry: https://getfoundry.sh\n");
        return 1;
    }

    /* Create funded wallet for this run */
    printf("Creating fresh funded wallet...\n");
    char privkey[128], address[64];
    if (test_util_create_funded_wallet(privkey, sizeof(privkey),
                                        address, sizeof(address),
                                        HPPLITE_DEFAULT_RPC, "0.01ether") != 0) {
        return 1;
    }
    printf("  Address: %s\n\n", address);

    /* Clean up data directories */
    system("rm -rf " DATA_DIR " " DATA_DIR2 " && mkdir -p " DATA_DIR " " DATA_DIR2);

    /*
     * PART 1: Write data to L1
     */
    printf("=== PART 1: Write data ===\n\n");

    char uri[1024];
    snprintf(uri, sizeof(uri),
        "file:" DATA_DIR "/state.db"
        "?hpplite=on"
        "&l1=hpp-sepolia"  /* factory auto-set from network alias */
        "&privkey=%s",
        privkey
    );

    sqlite3 *db;
    int rc = sqlite3_open_v2(uri, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    if (rc != SQLITE_OK) {
        printf("ERROR: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    printf("Connected to L1, creating rollup...\n\n");

    /* Write some data */
    printf("Writing data:\n");
    sqlite3_exec(db, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, email TEXT)",
                 NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO users VALUES (2, 'Bob', 'bob@example.com')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO users VALUES (3, 'Charlie', 'charlie@example.com')", NULL, NULL, NULL);

    query_users(db);

    /* Wait for flush to L1 */
    printf("\nFlushing to L1...\n");
    sleep(3);

    sqlite3_close(db);
    printf("Database closed. Data is now on L1.\n\n");

    /*
     * PART 2: Reconstruct from L1 in fresh database
     */
    printf("=== PART 2: Reconstruct from L1 ===\n\n");

    /* Open a DIFFERENT database file, but same rollup (same privkey) */
    snprintf(uri, sizeof(uri),
        "file:" DATA_DIR2 "/reconstructed.db"
        "?hpplite=on"
        "&l1=hpp-sepolia"
        "&privkey=%s"
        "&role=replica",  /* replica mode - sync from L1 */
        privkey
    );

    sqlite3 *db2;
    rc = sqlite3_open_v2(uri, &db2,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    if (rc != SQLITE_OK) {
        printf("ERROR: %s\n", sqlite3_errmsg(db2));
        return 1;
    }

    printf("Opened fresh database, syncing from L1...\n");
    sleep(2);  /* Wait for sync */

    printf("\nReconstructed data:\n");
    query_users(db2);

    sqlite3_close(db2);

    printf("\nSuccess! Data reconstructed from L1.\n");

    return 0;
}
