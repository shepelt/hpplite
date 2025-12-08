/*
** HPPLite M3 L2 Reconstruction Test
**
** Self-contained test that:
** 1. Creates a sequencer node and generates batches
** 2. Records the state root
** 3. Creates a fresh node and replays batches from storage
** 4. Verifies reconstructed state matches original
**
** This demonstrates that any node can reconstruct L2 state
** from batch data (which would be fetched from DA layer in production).
*/

#include "node.h"
#include "l1_interface.h"
#include "batch.h"
#include "fs_storage.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test directories */
#define SEQUENCER_DIR "/tmp/hpplite_reconstruct_seq"
#define REPLAY_DIR "/tmp/hpplite_reconstruct_replay"

static int tests_run = 0;
static int tests_passed = 0;

static void print_hex(const unsigned char *data, int len) {
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
}

/* Test key - doesn't need to match L1 for this local test */
static const unsigned char TEST_PRIVKEY[32] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
};

/*
** Load batch from storage
*/
static HppliteBatch *load_batch(const char *dataDir, uint64_t height) {
    char path[256];
    snprintf(path, sizeof(path), "%s/batches/%08llu.json",
             dataDir, (unsigned long long)height);

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("    Could not open batch file: %s\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *json = malloc(len + 1);
    fread(json, 1, len, f);
    json[len] = '\0';
    fclose(f);

    HppliteBatch *batch = hpplite_batch_from_json(json);
    free(json);
    return batch;
}

/*
** Test L2 state reconstruction from batch data
*/
static int test_reconstruction(void) {
    printf("Test: L2 reconstruction from batch data\n");
    tests_run++;

    int numBatches = 10;
    unsigned char originalStateRoot[32];
    char *errMsg = NULL;
    int rc;

    /* === Phase 1: Create sequencer and generate batches === */
    printf("  Phase 1: Generate batches with sequencer\n");

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", SEQUENCER_DIR, SEQUENCER_DIR);
    system(cmd);

    HppliteNodeConfig seqCfg;
    memset(&seqCfg, 0, sizeof(seqCfg));
    seqCfg.nodeId = "sequencer";
    memcpy(seqCfg.privkey, TEST_PRIVKEY, 32);
    seqCfg.dataDir = SEQUENCER_DIR;
    snprintf(cmd, sizeof(cmd), "%s/seq.db", SEQUENCER_DIR);
    seqCfg.dbPath = cmd;

    HppliteNode *seqNode = hpplite_node_create(&seqCfg);
    if (!seqNode) {
        printf("  FAIL: Could not create sequencer node\n");
        return -1;
    }
    hpplite_node_set_role(seqNode, HPPLITE_ROLE_SEQUENCER);
    hpplite_node_start(seqNode);
    printf("    Sequencer node created\n");

    /* Create table */
    rc = hpplite_node_exec(seqNode, "CREATE TABLE test_data(id INTEGER PRIMARY KEY, value TEXT)", &errMsg);
    if (rc != SQLITE_OK) {
        printf("  FAIL: CREATE TABLE: %s\n", errMsg);
        sqlite3_free(errMsg);
        hpplite_node_destroy(seqNode);
        return -1;
    }

    /* Create batches with inserts */
    for (int i = 1; i <= numBatches; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO test_data VALUES(%d, 'batch %d data')", i, i);

        rc = hpplite_node_exec(seqNode, sql, &errMsg);
        if (rc != SQLITE_OK) {
            printf("  FAIL: INSERT %d: %s\n", i, errMsg);
            sqlite3_free(errMsg);
            hpplite_node_destroy(seqNode);
            return -1;
        }

        uint64_t height = hpplite_node_flush_batch(seqNode);
        if (i <= 3 || i >= numBatches - 1) {
            printf("    Created batch %llu\n", (unsigned long long)height);
        } else if (i == 4) {
            printf("    ...\n");
        }
    }

    /* Get original state root */
    hpplite_node_get_state_root(seqNode, originalStateRoot);
    printf("    Original state root: ");
    print_hex(originalStateRoot, 32);
    printf("\n");

    /* Count rows in original */
    int originalRowCount = 0;
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(seqNode->db, "SELECT COUNT(*) FROM test_data", -1, &stmt, NULL);
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        originalRowCount = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    printf("    Original row count: %d\n", originalRowCount);

    hpplite_node_destroy(seqNode);

    /* === Phase 2: Create fresh node and replay batches === */
    printf("  Phase 2: Reconstruct state from batches\n");

    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", REPLAY_DIR, REPLAY_DIR);
    system(cmd);

    HppliteNodeConfig replayCfg;
    memset(&replayCfg, 0, sizeof(replayCfg));
    replayCfg.nodeId = "reconstructor";
    /* Different privkey - doesn't matter for replay */
    for (int i = 0; i < 32; i++) replayCfg.privkey[i] = i + 0x80;
    replayCfg.dataDir = REPLAY_DIR;
    snprintf(cmd, sizeof(cmd), "%s/replay.db", REPLAY_DIR);
    replayCfg.dbPath = cmd;

    HppliteNode *replayNode = hpplite_node_create(&replayCfg);
    if (!replayNode) {
        printf("  FAIL: Could not create replay node\n");
        return -1;
    }
    hpplite_node_set_role(replayNode, HPPLITE_ROLE_WITNESS);
    hpplite_node_start(replayNode);
    printf("    Fresh replay node created\n");

    /* Replay all batches from sequencer's storage */
    for (int h = 1; h <= numBatches; h++) {
        HppliteBatch *batch = load_batch(SEQUENCER_DIR, h);
        if (!batch) {
            printf("  FAIL: Could not load batch %d\n", h);
            hpplite_node_destroy(replayNode);
            return -1;
        }

        char batchPath[256];
        snprintf(batchPath, sizeof(batchPath), "%s/batches/%08d.json", SEQUENCER_DIR, h);
        rc = hpplite_node_verify_batch(replayNode, batch, batchPath);
        if (rc != 0) {
            printf("  FAIL: Could not verify/replay batch %d\n", h);
            hpplite_batch_free(batch);
            hpplite_node_destroy(replayNode);
            return -1;
        }

        if (h <= 3 || h >= numBatches - 1) {
            printf("    Replayed batch %d\n", h);
        } else if (h == 4) {
            printf("    ...\n");
        }

        hpplite_batch_free(batch);
    }

    /* === Phase 3: Verify state matches === */
    printf("  Phase 3: Verify state root\n");

    unsigned char reconstructedRoot[32];
    hpplite_node_get_state_root(replayNode, reconstructedRoot);

    printf("    Original:      ");
    print_hex(originalStateRoot, 32);
    printf("\n    Reconstructed: ");
    print_hex(reconstructedRoot, 32);
    printf("\n");

    if (memcmp(originalStateRoot, reconstructedRoot, 32) == 0) {
        printf("  State roots MATCH!\n");
    } else {
        printf("  FAIL: State roots do not match\n");
        hpplite_node_destroy(replayNode);
        return -1;
    }

    /* Verify row count */
    int replayedRowCount = 0;
    rc = sqlite3_prepare_v2(replayNode->db, "SELECT COUNT(*) FROM test_data", -1, &stmt, NULL);
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        replayedRowCount = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    printf("    Reconstructed row count: %d\n", replayedRowCount);

    if (replayedRowCount == originalRowCount) {
        printf("  Row counts MATCH!\n");
        tests_passed++;
    } else {
        printf("  FAIL: Row counts don't match (%d vs %d)\n",
               originalRowCount, replayedRowCount);
    }

    /* Query some data to verify content */
    rc = sqlite3_prepare_v2(replayNode->db,
        "SELECT value FROM test_data WHERE id=5", -1, &stmt, NULL);
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        printf("    Sample data (id=5): %s\n", sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);

    hpplite_node_destroy(replayNode);
    return 0;
}

int main(void) {
    printf("test_m3_reconstruct: L2 state reconstruction from batch data\n\n");

    test_reconstruction();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
