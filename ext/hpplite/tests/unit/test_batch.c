/*
** Test program for HPPLite Batch serialization
*/

#include <stdio.h>
#include <string.h>
#include "sqlite3.h"
#include "batch.h"

static void print_hex(const unsigned char *data, int len) {
  for (int i = 0; i < len; i++) {
    printf("%02x", data[i]);
  }
}

int main(void) {
  HppliteBatch *batch, *parsed;
  char *json;
  unsigned char preRoot[32] = {0};
  unsigned char postRoot[32] = {0xa1, 0xb2, 0xc3, 0xd4};

  printf("=== HPPLite Batch Serialization Test ===\n\n");

  /* Test 1: Create and serialize a batch */
  printf("--- Test 1: Create batch ---\n");
  batch = hpplite_batch_new(1);
  if (!batch) {
    printf("FAIL: Could not create batch\n");
    return 1;
  }

  hpplite_batch_set_pre_state(batch, preRoot);
  hpplite_batch_add_txn(batch, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)", 0);
  hpplite_batch_add_txn(batch, "INSERT INTO users (name) VALUES ('Alice')", 1);
  hpplite_batch_add_txn(batch, "INSERT INTO users (name) VALUES ('Bob')", 1);
  hpplite_batch_set_post_state(batch, postRoot);

  printf("Batch height: %llu\n", (unsigned long long)batch->height);
  printf("Transactions: %d\n", batch->nTxns);
  printf("Pre-state:  "); print_hex(batch->preStateRoot, 8); printf("...\n");
  printf("Post-state: "); print_hex(batch->postStateRoot, 8); printf("...\n");
  printf("PASS\n\n");

  /* Test 2: Serialize to JSON */
  printf("--- Test 2: Serialize to JSON ---\n");
  json = hpplite_batch_to_json(batch);
  if (!json) {
    printf("FAIL: Could not serialize batch\n");
    return 1;
  }
  printf("%s\n", json);
  printf("PASS\n\n");

  /* Test 3: Parse from JSON */
  printf("--- Test 3: Parse from JSON ---\n");
  parsed = hpplite_batch_from_json(json);
  if (!parsed) {
    printf("FAIL: Could not parse batch\n");
    return 1;
  }

  if (parsed->height != batch->height) {
    printf("FAIL: Height mismatch\n");
    return 1;
  }
  if (parsed->nTxns != batch->nTxns) {
    printf("FAIL: Transaction count mismatch\n");
    return 1;
  }
  if (memcmp(parsed->preStateRoot, batch->preStateRoot, 32) != 0) {
    printf("FAIL: Pre-state root mismatch\n");
    return 1;
  }
  if (memcmp(parsed->postStateRoot, batch->postStateRoot, 32) != 0) {
    printf("FAIL: Post-state root mismatch\n");
    return 1;
  }

  printf("Parsed height: %llu\n", (unsigned long long)parsed->height);
  printf("Parsed txns: %d\n", parsed->nTxns);
  for (int i = 0; i < parsed->nTxns; i++) {
    printf("  [%d] %s (rows: %lld)\n", i,
           parsed->aTxns[i].zSql,
           (long long)parsed->aTxns[i].nRowsAffected);
  }
  printf("PASS\n\n");

  /* Test 4: SQL with special characters */
  printf("--- Test 4: SQL with special characters ---\n");
  HppliteBatch *batch2 = hpplite_batch_new(2);
  hpplite_batch_add_txn(batch2, "INSERT INTO t VALUES ('it''s \"quoted\"')", 1);
  hpplite_batch_add_txn(batch2, "INSERT INTO t VALUES ('line1\nline2\ttab')", 1);

  char *json2 = hpplite_batch_to_json(batch2);
  printf("JSON with escapes:\n%s\n", json2);

  HppliteBatch *parsed2 = hpplite_batch_from_json(json2);
  if (!parsed2 || parsed2->nTxns != 2) {
    printf("FAIL: Could not parse batch with special chars\n");
    return 1;
  }
  printf("Parsed SQL 1: %s\n", parsed2->aTxns[0].zSql);
  printf("Parsed SQL 2: %s\n", parsed2->aTxns[1].zSql);
  printf("PASS\n\n");

  /* Cleanup */
  sqlite3_free(json);
  sqlite3_free(json2);
  hpplite_batch_free(batch);
  hpplite_batch_free(parsed);
  hpplite_batch_free(batch2);
  hpplite_batch_free(parsed2);

  printf("=== All Tests Passed ===\n");
  return 0;
}
