/*
** HPPLite Example: Basic Sequencer
**
** Demonstrates transparent SQLite integration:
** - Use standard sqlite3_open_v2() with ?hpplite=on URI parameter
** - Use standard sqlite3_exec() for all database operations
** - Automatic state root tracking
** - Manual and automatic batch creation
** - Transparent close via sqlite3_close() (close hook handles cleanup)
**
** No registration needed - HPPLite is built into SQLite!
**
** Build:
**   cd ../build && make
**   gcc -I.. -I../../build -o basic_sequencer basic_sequencer.c \
**       ../build/libhpplite.a ../build/libsqlite3.a \
**       -L/opt/homebrew/lib -lsecp256k1 -lcurl -lzmq -lpthread
*/

#include "hpplite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA_DIR "/tmp/hpplite_example"

static void print_hex(const unsigned char *data, int len) {
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
}

int main(void) {
    printf("HPPLite Basic Sequencer Example\n");
    printf("================================\n\n");

    /* Clean up and create data directory */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", DATA_DIR, DATA_DIR);
    system(cmd);

    /*
     * Open database with standard SQLite API.
     * HPPLite auto-initializes when ?hpplite=on is in the URI!
     *
     * URI parameters:
     *   hpplite=on        - Enable HPPLite
     *   role=sequencer    - This node produces batches
     *   datadir=/path     - Where to store batches
     */
    printf("Opening database with HPPLite...\n");
    sqlite3 *db;
    int rc = sqlite3_open_v2(
        "file:" DATA_DIR "/state.db"
        "?hpplite=on"
        "&role=sequencer"
        "&datadir=" DATA_DIR,
        &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI,
        NULL
    );
    if (rc != SQLITE_OK) {
        printf("ERROR: Failed to open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    /* Get initial state root (all zeros for new db) */
    unsigned char stateRoot[32];
    hpplite_state_root(db, stateRoot);
    printf("Initial state root: ");
    print_hex(stateRoot, 32);
    printf("\n\n");

    /* Use standard sqlite3_exec - changes are automatically tracked! */
    char *errMsg = NULL;
    int rc;

    printf("Creating table...\n");
    rc = sqlite3_exec(db,
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, email TEXT)",
        NULL, NULL, &errMsg);
    if (rc != SQLITE_OK) {
        printf("ERROR: %s\n", errMsg);
        sqlite3_free(errMsg);
        return 1;
    }

    printf("Inserting data...\n");
    sqlite3_exec(db, "INSERT INTO users VALUES (1, 'Alice', 'alice@example.com')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO users VALUES (2, 'Bob', 'bob@example.com')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO users VALUES (3, 'Charlie', 'charlie@example.com')", NULL, NULL, NULL);

    /* State root has changed after each commit */
    hpplite_state_root(db, stateRoot);
    printf("State root after inserts: ");
    print_hex(stateRoot, 32);
    printf("\n");

    /* Flush batch - creates a batch with state root transition */
    printf("\nFlushing batch...\n");
    uint64_t height = hpplite_flush(db);
    printf("Batch created at height: %llu\n", (unsigned long long)height);

    /* More operations */
    printf("\nUpdating data...\n");
    sqlite3_exec(db, "UPDATE users SET email = 'alice@newdomain.com' WHERE id = 1", NULL, NULL, NULL);
    sqlite3_exec(db, "DELETE FROM users WHERE id = 3", NULL, NULL, NULL);

    hpplite_state_root(db, stateRoot);
    printf("State root after updates: ");
    print_hex(stateRoot, 32);
    printf("\n");

    height = hpplite_flush(db);
    printf("Batch created at height: %llu\n", (unsigned long long)height);

    /* Query the data - standard SQLite API */
    printf("\nQuerying data:\n");
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, "SELECT * FROM users", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            printf("  id=%d name=%s email=%s\n",
                   sqlite3_column_int(stmt, 0),
                   sqlite3_column_text(stmt, 1),
                   sqlite3_column_text(stmt, 2));
        }
        sqlite3_finalize(stmt);
    }

    /* Show batch files */
    printf("\nBatch files created:\n");
    snprintf(cmd, sizeof(cmd), "ls -la %s/batches/", DATA_DIR);
    system(cmd);

    /* Close with standard sqlite3_close() - the close hook handles cleanup!
     * Any pending changes are flushed automatically. */
    sqlite3_close(db);

    printf("\nDone!\n");
    return 0;
}
