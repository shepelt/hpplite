/*
** HPPLite Test Program
**
** Tests basic state root tracking functionality.
*/

#include <stdio.h>
#include <string.h>
#include "sqlite3.h"
#include "hpplite.h"

/* Helper to print hash as hex */
static void print_hash(const char *label, unsigned char *hash) {
  int i;
  printf("%s: ", label);
  for (i = 0; i < HPPLITE_HASH_SIZE; i++) {
    printf("%02x", hash[i]);
  }
  printf("\n");
}

/* Callback for commit notifications */
static void on_commit(void *arg, unsigned char *stateRoot, int nChanges) {
  (void)arg;
  printf("COMMIT: %d changes\n", nChanges);
  print_hash("New state root", stateRoot);
}

/* Callback for rollback notifications */
static void on_rollback(void *arg) {
  (void)arg;
  printf("ROLLBACK\n");
}

int main(int argc, char **argv) {
  sqlite3 *db;
  HppliteCtx *ctx;
  unsigned char stateRoot[HPPLITE_HASH_SIZE];
  int rc;
  char *errMsg = NULL;

  (void)argc;
  (void)argv;

  printf("=== HPPLite State Root Tracking Test ===\n\n");
  printf("SQLite version: %s\n", sqlite3_libversion());
  printf("Preupdate hook enabled: %d\n\n",
         sqlite3_compileoption_used("ENABLE_PREUPDATE_HOOK"));

  /* Open in-memory database */
  rc = sqlite3_open(":memory:", &db);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
    return 1;
  }

  /* Initialize HPPLite */
  rc = hpplite_init(db, &ctx);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "Cannot initialize HPPLite: %d\n", rc);
    sqlite3_close(db);
    return 1;
  }

  /* Set callbacks */
  hpplite_set_callbacks(ctx, NULL, on_commit, on_rollback);

  /* Get initial state root (should be zeros) */
  hpplite_get_state_root(ctx, stateRoot);
  print_hash("Initial state root", stateRoot);
  printf("\n");

  /* Test 1: CREATE TABLE (DDL - no row changes tracked) */
  printf("--- Test 1: CREATE TABLE ---\n");
  rc = sqlite3_exec(db, "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT, age INT);",
                    NULL, NULL, &errMsg);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "SQL error: %s\n", errMsg);
    sqlite3_free(errMsg);
  }
  hpplite_get_state_root(ctx, stateRoot);
  print_hash("After CREATE TABLE", stateRoot);
  printf("\n");

  /* Test 2: INSERT */
  printf("--- Test 2: INSERT ---\n");
  rc = sqlite3_exec(db, "INSERT INTO users(name, age) VALUES('Alice', 30);",
                    NULL, NULL, &errMsg);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "SQL error: %s\n", errMsg);
    sqlite3_free(errMsg);
  }
  hpplite_get_state_root(ctx, stateRoot);
  print_hash("After INSERT Alice", stateRoot);
  printf("\n");

  /* Test 3: Multiple INSERTs in transaction */
  printf("--- Test 3: Multiple INSERTs in transaction ---\n");
  rc = sqlite3_exec(db,
    "BEGIN;"
    "INSERT INTO users(name, age) VALUES('Bob', 25);"
    "INSERT INTO users(name, age) VALUES('Charlie', 35);"
    "COMMIT;",
    NULL, NULL, &errMsg);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "SQL error: %s\n", errMsg);
    sqlite3_free(errMsg);
  }
  hpplite_get_state_root(ctx, stateRoot);
  print_hash("After INSERT Bob & Charlie", stateRoot);
  printf("\n");

  /* Test 4: UPDATE */
  printf("--- Test 4: UPDATE ---\n");
  rc = sqlite3_exec(db, "UPDATE users SET age = 31 WHERE name = 'Alice';",
                    NULL, NULL, &errMsg);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "SQL error: %s\n", errMsg);
    sqlite3_free(errMsg);
  }
  hpplite_get_state_root(ctx, stateRoot);
  print_hash("After UPDATE Alice age", stateRoot);
  printf("\n");

  /* Test 5: DELETE */
  printf("--- Test 5: DELETE ---\n");
  rc = sqlite3_exec(db, "DELETE FROM users WHERE name = 'Bob';",
                    NULL, NULL, &errMsg);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "SQL error: %s\n", errMsg);
    sqlite3_free(errMsg);
  }
  hpplite_get_state_root(ctx, stateRoot);
  print_hash("After DELETE Bob", stateRoot);
  printf("\n");

  /* Test 6: ROLLBACK */
  printf("--- Test 6: ROLLBACK ---\n");
  unsigned char beforeRollback[HPPLITE_HASH_SIZE];
  hpplite_get_state_root(ctx, beforeRollback);

  rc = sqlite3_exec(db,
    "BEGIN;"
    "INSERT INTO users(name, age) VALUES('David', 40);"
    "ROLLBACK;",
    NULL, NULL, &errMsg);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "SQL error: %s\n", errMsg);
    sqlite3_free(errMsg);
  }
  hpplite_get_state_root(ctx, stateRoot);
  print_hash("After ROLLBACK", stateRoot);

  if (memcmp(beforeRollback, stateRoot, HPPLITE_HASH_SIZE) == 0) {
    printf("State root unchanged after rollback: PASS\n");
  } else {
    printf("State root changed after rollback: FAIL\n");
  }
  printf("\n");

  /* Test 7: Verify determinism - same operations should yield same hash */
  printf("--- Test 7: Determinism check ---\n");
  sqlite3 *db2;
  HppliteCtx *ctx2;
  unsigned char stateRoot2[HPPLITE_HASH_SIZE];

  sqlite3_open(":memory:", &db2);
  hpplite_init(db2, &ctx2);

  sqlite3_exec(db2, "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT, age INT);", NULL, NULL, NULL);
  sqlite3_exec(db2, "INSERT INTO users(name, age) VALUES('Alice', 30);", NULL, NULL, NULL);
  sqlite3_exec(db2, "BEGIN; INSERT INTO users(name, age) VALUES('Bob', 25); INSERT INTO users(name, age) VALUES('Charlie', 35); COMMIT;", NULL, NULL, NULL);
  sqlite3_exec(db2, "UPDATE users SET age = 31 WHERE name = 'Alice';", NULL, NULL, NULL);
  sqlite3_exec(db2, "DELETE FROM users WHERE name = 'Bob';", NULL, NULL, NULL);

  hpplite_get_state_root(ctx2, stateRoot2);
  print_hash("DB2 state root", stateRoot2);

  if (memcmp(stateRoot, stateRoot2, HPPLITE_HASH_SIZE) == 0) {
    printf("Same operations yield same state root: PASS\n");
  } else {
    printf("State roots differ: FAIL\n");
    print_hash("DB1", stateRoot);
    print_hash("DB2", stateRoot2);
  }

  hpplite_shutdown(ctx2);
  sqlite3_close(db2);
  printf("\n");

  /* Cleanup */
  printf("=== Tests Complete ===\n");
  hpplite_shutdown(ctx);
  sqlite3_close(db);

  return 0;
}
