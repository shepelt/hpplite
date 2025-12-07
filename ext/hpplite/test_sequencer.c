/*
** HPPLite Sequencer Test
**
** Tests the sequencer-style block builder API:
** - hpplite_exec() to execute and track SQL
** - hpplite_flush_block() to create batches
** - Block height and chaining
*/

#include <stdio.h>
#include <string.h>
#include "sqlite3.h"
#include "hpplite.h"
#include "batch.h"

static void print_hex(const unsigned char *data, int len) {
  for (int i = 0; i < len; i++) printf("%02x", data[i]);
}

int main(void) {
  sqlite3 *db = NULL;
  HppliteCtx *ctx = NULL;
  HppliteBatch *batch = NULL;
  int rc;
  char *errMsg = NULL;

  printf("=== HPPLite Sequencer Test ===\n\n");

  /* Open in-memory database */
  rc = sqlite3_open(":memory:", &db);
  if (rc != SQLITE_OK) {
    printf("FAIL: Could not open database\n");
    return 1;
  }

  /* Initialize HPPLite */
  rc = hpplite_init(db, &ctx);
  if (rc != SQLITE_OK) {
    printf("FAIL: Could not initialize HPPLite\n");
    return 1;
  }

  /* Test 1: Initial state */
  printf("--- Test 1: Initial state ---\n");
  printf("Block height: %llu\n", (unsigned long long)hpplite_get_block_height(ctx));
  printf("Pending count: %d\n", hpplite_pending_count(ctx));

  unsigned char prevHash[32];
  hpplite_get_prev_block_hash(ctx, prevHash);
  printf("Prev block hash: "); print_hex(prevHash, 8); printf("... (should be zeros)\n");

  if (hpplite_get_block_height(ctx) != 1) {
    printf("FAIL: Initial block height should be 1\n");
    return 1;
  }
  printf("PASS\n\n");

  /* Test 2: Execute SQL via hpplite_exec */
  printf("--- Test 2: Execute SQL via hpplite_exec ---\n");

  rc = hpplite_exec(ctx, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)", &errMsg);
  if (rc != SQLITE_OK) {
    printf("FAIL: CREATE TABLE failed: %s\n", errMsg);
    return 1;
  }
  printf("Pending after CREATE: %d\n", hpplite_pending_count(ctx));

  rc = hpplite_exec(ctx, "INSERT INTO users (name) VALUES ('Alice')", &errMsg);
  if (rc != SQLITE_OK) {
    printf("FAIL: INSERT failed: %s\n", errMsg);
    return 1;
  }
  printf("Pending after INSERT 1: %d\n", hpplite_pending_count(ctx));

  rc = hpplite_exec(ctx, "INSERT INTO users (name) VALUES ('Bob')", &errMsg);
  printf("Pending after INSERT 2: %d\n", hpplite_pending_count(ctx));

  rc = hpplite_exec(ctx, "INSERT INTO users (name) VALUES ('Charlie')", &errMsg);
  printf("Pending after INSERT 3: %d\n", hpplite_pending_count(ctx));

  if (hpplite_pending_count(ctx) != 4) {
    printf("FAIL: Should have 4 pending SQL\n");
    return 1;
  }
  printf("PASS\n\n");

  /* Test 3: Flush to create block 1 */
  printf("--- Test 3: Flush to create Block 1 ---\n");

  batch = hpplite_flush_block(ctx);
  if (!batch) {
    printf("FAIL: Could not create batch\n");
    return 1;
  }

  printf("Batch height: %llu\n", (unsigned long long)batch->height);
  printf("Batch transactions: %d\n", batch->nTxns);
  printf("Pre-state: "); print_hex(batch->preStateRoot, 8); printf("...\n");
  printf("Post-state: "); print_hex(batch->postStateRoot, 8); printf("...\n");

  if (batch->height != 1) {
    printf("FAIL: Batch height should be 1\n");
    return 1;
  }
  if (batch->nTxns != 4) {
    printf("FAIL: Batch should have 4 transactions\n");
    return 1;
  }

  /* Print transactions */
  printf("Transactions:\n");
  for (int i = 0; i < batch->nTxns; i++) {
    printf("  [%d] %s\n", i, batch->aTxns[i].zSql);
  }

  hpplite_batch_free(batch);
  batch = NULL;

  /* Check state after flush */
  printf("\nAfter flush:\n");
  printf("Block height: %llu (should be 2)\n", (unsigned long long)hpplite_get_block_height(ctx));
  printf("Pending count: %d (should be 0)\n", hpplite_pending_count(ctx));
  hpplite_get_prev_block_hash(ctx, prevHash);
  printf("Prev block hash: "); print_hex(prevHash, 8); printf("... (should be non-zero)\n");

  if (hpplite_get_block_height(ctx) != 2) {
    printf("FAIL: Block height should be 2 after flush\n");
    return 1;
  }
  if (hpplite_pending_count(ctx) != 0) {
    printf("FAIL: Pending should be 0 after flush\n");
    return 1;
  }
  printf("PASS\n\n");

  /* Test 4: Create second block */
  printf("--- Test 4: Create Block 2 ---\n");

  unsigned char block1Hash[32];
  hpplite_get_prev_block_hash(ctx, block1Hash);

  rc = hpplite_exec(ctx, "UPDATE users SET name = 'Alicia' WHERE id = 1", &errMsg);
  rc = hpplite_exec(ctx, "DELETE FROM users WHERE name = 'Charlie'", &errMsg);

  printf("Pending: %d\n", hpplite_pending_count(ctx));

  batch = hpplite_flush_block(ctx);
  if (!batch) {
    printf("FAIL: Could not create batch 2\n");
    return 1;
  }

  printf("Batch height: %llu\n", (unsigned long long)batch->height);
  printf("Batch transactions: %d\n", batch->nTxns);
  printf("Transactions:\n");
  for (int i = 0; i < batch->nTxns; i++) {
    printf("  [%d] %s\n", i, batch->aTxns[i].zSql);
  }

  if (batch->height != 2) {
    printf("FAIL: Batch 2 height should be 2\n");
    return 1;
  }

  hpplite_batch_free(batch);
  printf("PASS\n\n");

  /* Test 5: Block chaining */
  printf("--- Test 5: Block chaining ---\n");

  unsigned char block2Hash[32];
  hpplite_get_prev_block_hash(ctx, block2Hash);

  printf("Block 1 hash: "); print_hex(block1Hash, 16); printf("...\n");
  printf("Block 2 hash: "); print_hex(block2Hash, 16); printf("...\n");
  printf("Current height: %llu\n", (unsigned long long)hpplite_get_block_height(ctx));

  if (memcmp(block1Hash, block2Hash, 32) == 0) {
    printf("FAIL: Block hashes should be different\n");
    return 1;
  }
  if (hpplite_get_block_height(ctx) != 3) {
    printf("FAIL: Height should be 3\n");
    return 1;
  }
  printf("PASS\n\n");

  /* Test 6: Empty flush returns NULL */
  printf("--- Test 6: Empty flush returns NULL ---\n");
  batch = hpplite_flush_block(ctx);
  if (batch != NULL) {
    printf("FAIL: Empty flush should return NULL\n");
    return 1;
  }
  printf("Empty flush correctly returned NULL\n");
  printf("PASS\n\n");

  /* Test 7: Discard pending */
  printf("--- Test 7: Discard pending ---\n");
  hpplite_exec(ctx, "INSERT INTO users (name) VALUES ('Dave')", &errMsg);
  hpplite_exec(ctx, "INSERT INTO users (name) VALUES ('Eve')", &errMsg);
  printf("Pending before discard: %d\n", hpplite_pending_count(ctx));

  hpplite_discard_pending(ctx);
  printf("Pending after discard: %d\n", hpplite_pending_count(ctx));

  if (hpplite_pending_count(ctx) != 0) {
    printf("FAIL: Pending should be 0 after discard\n");
    return 1;
  }

  /* Note: The SQL was already executed, so the data is in DB.
     Discard just clears the pending list, doesn't rollback DB. */
  printf("PASS\n\n");

  /* Test 8: Serialize batch to JSON */
  printf("--- Test 8: Batch JSON serialization ---\n");
  hpplite_exec(ctx, "INSERT INTO users (name) VALUES ('Frank')", &errMsg);
  batch = hpplite_flush_block(ctx);

  char *json = hpplite_batch_to_json(batch);
  printf("Batch JSON:\n%s\n", json);

  /* Parse it back */
  HppliteBatch *parsed = hpplite_batch_from_json(json);
  if (!parsed) {
    printf("FAIL: Could not parse batch JSON\n");
    return 1;
  }

  if (parsed->height != batch->height) {
    printf("FAIL: Parsed height mismatch\n");
    return 1;
  }
  if (parsed->nTxns != batch->nTxns) {
    printf("FAIL: Parsed nTxns mismatch\n");
    return 1;
  }
  if (memcmp(parsed->postStateRoot, batch->postStateRoot, 32) != 0) {
    printf("FAIL: Parsed postStateRoot mismatch\n");
    return 1;
  }

  printf("Parsed back successfully!\n");

  sqlite3_free(json);
  hpplite_batch_free(batch);
  hpplite_batch_free(parsed);
  printf("PASS\n\n");

  /* Cleanup */
  hpplite_shutdown(ctx);
  sqlite3_close(db);

  printf("=== All Sequencer Tests Passed ===\n");
  return 0;
}
