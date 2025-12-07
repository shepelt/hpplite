/*
** HPPLite Integrated Test
**
** Tests the full flow: execute SQL → track state → create batch → store → reconstruct
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sqlite3.h"
#include "hpplite.h"
#include "batch.h"
#include "fs_storage.h"

#ifdef _WIN32
#include <direct.h>
#define rmdir _rmdir
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#endif

static void print_hex(const unsigned char *data, int len) {
  for (int i = 0; i < len; i++) printf("%02x", data[i]);
}

/* Remove directory recursively */
static void remove_dir(const char *path) {
#ifndef _WIN32
  DIR *d = opendir(path);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      char *full = sqlite3_mprintf("%s/%s", path, e->d_name);
      struct stat st;
      if (stat(full, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
          remove_dir(full);
        } else {
          unlink(full);
        }
      }
      sqlite3_free(full);
    }
    closedir(d);
  }
  rmdir(path);
#endif
}

int main(void) {
  const char *dataDir = "/tmp/hpplite_test";
  HppliteStorage *storage = NULL;
  sqlite3 *db = NULL;
  HppliteCtx *ctx = NULL;
  char *dbPath = NULL;
  int rc;

  printf("=== HPPLite Integrated Test ===\n\n");

  /* Clean up any previous test */
  remove_dir(dataDir);

  /* Test 1: Initialize storage */
  printf("--- Test 1: Initialize storage ---\n");
  rc = hpplite_storage_init(dataDir, &storage);
  if (rc != 0) {
    printf("FAIL: Could not initialize storage\n");
    return 1;
  }
  printf("Storage initialized at: %s\n", hpplite_storage_get_dir(storage));
  printf("Latest height: %llu\n", (unsigned long long)hpplite_storage_get_latest_height(storage));
  printf("PASS\n\n");

  /* Test 2: Open database with HPPLite tracking */
  printf("--- Test 2: Open database with state tracking ---\n");
  dbPath = hpplite_storage_get_db_path(storage);
  rc = sqlite3_open(dbPath, &db);
  if (rc != SQLITE_OK) {
    printf("FAIL: Could not open database: %s\n", sqlite3_errmsg(db));
    return 1;
  }

  rc = hpplite_init(db, &ctx);
  if (rc != SQLITE_OK) {
    printf("FAIL: Could not initialize HPPLite\n");
    return 1;
  }

  unsigned char stateRoot[32];
  hpplite_get_state_root(ctx, stateRoot);
  printf("Initial state: "); print_hex(stateRoot, 16); printf("...\n");
  printf("PASS\n\n");

  /* Test 3: Execute SQL and create batch */
  printf("--- Test 3: Execute SQL and create batch ---\n");

  /* Create batch structure */
  HppliteBatch *batch = hpplite_batch_new(1);
  hpplite_batch_set_pre_state(batch, stateRoot);

  /* Execute SQL */
  char *errMsg = NULL;
  const char *sql1 = "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, email TEXT)";
  rc = sqlite3_exec(db, sql1, NULL, NULL, &errMsg);
  if (rc != SQLITE_OK) {
    printf("FAIL: SQL error: %s\n", errMsg);
    return 1;
  }
  hpplite_batch_add_txn(batch, sql1, 0);

  const char *sql2 = "INSERT INTO users (name, email) VALUES ('Alice', 'alice@example.com')";
  rc = sqlite3_exec(db, sql2, NULL, NULL, &errMsg);
  hpplite_batch_add_txn(batch, sql2, 1);

  const char *sql3 = "INSERT INTO users (name, email) VALUES ('Bob', 'bob@example.com')";
  rc = sqlite3_exec(db, sql3, NULL, NULL, &errMsg);
  hpplite_batch_add_txn(batch, sql3, 1);

  /* Get new state root */
  hpplite_get_state_root(ctx, stateRoot);
  hpplite_batch_set_post_state(batch, stateRoot);

  printf("Batch height: %llu\n", (unsigned long long)batch->height);
  printf("Transactions: %d\n", batch->nTxns);
  printf("Post-state: "); print_hex(stateRoot, 16); printf("...\n");
  printf("PASS\n\n");

  /* Test 4: Store batch and create commitment */
  printf("--- Test 4: Store batch and commitment ---\n");
  char *batchRef = hpplite_storage_store_batch(storage, batch);
  if (!batchRef) {
    printf("FAIL: Could not store batch\n");
    return 1;
  }
  printf("Batch stored at: %s\n", batchRef);

  HppliteCommitment *commit = hpplite_commitment_new(batch->height, stateRoot, batchRef);
  rc = hpplite_storage_post_commitment(storage, commit);
  if (rc != 0) {
    printf("FAIL: Could not post commitment\n");
    return 1;
  }
  printf("Commitment posted\n");
  printf("PASS\n\n");

  /* Test 5: Create second batch */
  printf("--- Test 5: Create second batch ---\n");
  HppliteBatch *batch2 = hpplite_batch_new(2);
  hpplite_batch_set_pre_state(batch2, stateRoot);

  const char *sql4 = "UPDATE users SET email = 'alice@newdomain.com' WHERE name = 'Alice'";
  sqlite3_exec(db, sql4, NULL, NULL, NULL);
  hpplite_batch_add_txn(batch2, sql4, 1);

  const char *sql5 = "DELETE FROM users WHERE name = 'Bob'";
  sqlite3_exec(db, sql5, NULL, NULL, NULL);
  hpplite_batch_add_txn(batch2, sql5, 1);

  hpplite_get_state_root(ctx, stateRoot);
  hpplite_batch_set_post_state(batch2, stateRoot);

  char *batchRef2 = hpplite_storage_store_batch(storage, batch2);
  HppliteCommitment *commit2 = hpplite_commitment_new(batch2->height, stateRoot, batchRef2);
  hpplite_storage_post_commitment(storage, commit2);

  printf("Batch 2 stored at: %s\n", batchRef2);
  printf("Final state: "); print_hex(stateRoot, 16); printf("...\n");
  printf("PASS\n\n");

  /* Test 6: Verify storage state */
  printf("--- Test 6: Verify storage state ---\n");
  printf("Latest height: %llu\n", (unsigned long long)hpplite_storage_get_latest_height(storage));

  HppliteCommitment **commits;
  int nCommits = hpplite_storage_get_commitments(storage, &commits);
  printf("Total commitments: %d\n", nCommits);

  for (int i = 0; i < nCommits; i++) {
    printf("  [%d] height=%llu root=", i, (unsigned long long)commits[i]->height);
    print_hex(commits[i]->stateRoot, 8);
    printf("... ref=%s\n", commits[i]->zBatchRef);
    hpplite_commitment_free(commits[i]);
  }
  sqlite3_free(commits);
  printf("PASS\n\n");

  /* Test 7: Load batch and verify */
  printf("--- Test 7: Load batch and verify ---\n");
  HppliteBatch *loadedBatch = hpplite_storage_load_batch(storage, batchRef);
  if (!loadedBatch) {
    printf("FAIL: Could not load batch\n");
    return 1;
  }

  printf("Loaded batch height: %llu\n", (unsigned long long)loadedBatch->height);
  printf("Loaded transactions: %d\n", loadedBatch->nTxns);
  for (int i = 0; i < loadedBatch->nTxns; i++) {
    printf("  [%d] %s\n", i, loadedBatch->aTxns[i].zSql);
  }

  if (memcmp(loadedBatch->postStateRoot, batch->postStateRoot, 32) != 0) {
    printf("FAIL: State root mismatch after load\n");
    return 1;
  }
  printf("State roots match: PASS\n\n");

  /* Test 8: Simulate reconstruction */
  printf("--- Test 8: Reconstruction simulation ---\n");

  /* Open fresh database */
  sqlite3 *db2 = NULL;
  HppliteCtx *ctx2 = NULL;
  const char *reconstructPath = "/tmp/hpplite_test/reconstructed.sqlite";

  sqlite3_open(reconstructPath, &db2);
  hpplite_init(db2, &ctx2);

  unsigned char reconstructedRoot[32];
  hpplite_get_state_root(ctx2, reconstructedRoot);
  printf("Initial reconstructed state: "); print_hex(reconstructedRoot, 8); printf("...\n");

  /* Replay batch 1 */
  HppliteBatch *b1 = hpplite_storage_load_batch(storage, batchRef);
  for (int i = 0; i < b1->nTxns; i++) {
    sqlite3_exec(db2, b1->aTxns[i].zSql, NULL, NULL, NULL);
  }
  hpplite_get_state_root(ctx2, reconstructedRoot);
  printf("After batch 1: "); print_hex(reconstructedRoot, 8); printf("...\n");

  if (memcmp(reconstructedRoot, b1->postStateRoot, 32) != 0) {
    printf("FAIL: Batch 1 state mismatch\n");
    print_hex(b1->postStateRoot, 8); printf(" expected\n");
    return 1;
  }
  printf("Batch 1 verified!\n");

  /* Replay batch 2 */
  HppliteBatch *b2 = hpplite_storage_load_batch(storage, batchRef2);
  for (int i = 0; i < b2->nTxns; i++) {
    sqlite3_exec(db2, b2->aTxns[i].zSql, NULL, NULL, NULL);
  }
  hpplite_get_state_root(ctx2, reconstructedRoot);
  printf("After batch 2: "); print_hex(reconstructedRoot, 8); printf("...\n");

  if (memcmp(reconstructedRoot, b2->postStateRoot, 32) != 0) {
    printf("FAIL: Batch 2 state mismatch\n");
    return 1;
  }
  printf("Batch 2 verified!\n");

  /* Compare final states */
  if (memcmp(reconstructedRoot, stateRoot, 32) == 0) {
    printf("\n*** RECONSTRUCTION SUCCESSFUL ***\n");
    printf("Original and reconstructed states match!\n");
  } else {
    printf("FAIL: Final state mismatch\n");
    return 1;
  }
  printf("PASS\n\n");

  /* Cleanup */
  hpplite_batch_free(batch);
  hpplite_batch_free(batch2);
  hpplite_batch_free(loadedBatch);
  hpplite_batch_free(b1);
  hpplite_batch_free(b2);
  hpplite_commitment_free(commit);
  hpplite_commitment_free(commit2);
  sqlite3_free(batchRef);
  sqlite3_free(batchRef2);
  sqlite3_free(dbPath);

  hpplite_shutdown(ctx);
  hpplite_shutdown(ctx2);
  sqlite3_close(db);
  sqlite3_close(db2);
  hpplite_storage_close(storage);

  printf("=== All Tests Passed ===\n");
  return 0;
}
