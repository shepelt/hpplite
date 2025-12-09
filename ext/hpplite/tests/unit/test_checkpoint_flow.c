/*
** Test Checkpoint Flow - Singleton Sequencer Model
**
** Tests the checkpoint submission flow for the simplified model.
** No witnesses - sequencer submits checkpoints directly to L1.
*/

#include "node.h"
#include "l1_interface.h"
#include "batch.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Test counters */
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    printf("  Testing: %s... ", name); \
    tests_run++; \
} while(0)

#define PASS() do { \
    printf("PASS\n"); \
    tests_passed++; \
} while(0)

#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
} while(0)

/* Test private key */
static const unsigned char SEQ_PRIVKEY[32] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
};

/* Helpers */
static void ensure_test_dir(const char *path) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", path);
    system(cmd);
}

static void cleanup_test_files(const char *dir) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);
}

/*
** Test 1: Basic checkpoint creation and submission
*/
static void test_checkpoint_basic(void) {
    HppliteNodeConfig config;
    HppliteNode *node;
    HppliteL1 *l1;
    char *txHash;

    TEST("checkpoint_basic");

    ensure_test_dir("/tmp/hpplite_test_cp1");

    /* Create L1 mock */
    l1 = hpplite_l1_create_mock();
    if (!l1) {
        FAIL("Failed to create L1 mock");
        return;
    }

    /* Create node */
    memset(&config, 0, sizeof(config));
    config.nodeId = "test-seq";
    memcpy(config.privkey, SEQ_PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = "/tmp/hpplite_test_cp1";
    config.dbPath = "/tmp/hpplite_test_cp1/test.db";
    config.checkpointInterval = 3;

    node = hpplite_node_create(&config);
    if (!node) {
        FAIL("Failed to create node");
        hpplite_l1_disconnect(l1);
        return;
    }

    /* Set L1 */
    hpplite_node_set_l1(node, l1);

    /* Produce some batches */
    for (int i = 0; i < 3; i++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "CREATE TABLE t%d (x INTEGER)", i);
        hpplite_node_exec(node, sql, NULL);
        hpplite_node_flush_batch(node);
    }

    /* Should be ready for checkpoint */
    if (!hpplite_node_should_checkpoint(node)) {
        FAIL("Should checkpoint after 3 batches");
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        cleanup_test_files("/tmp/hpplite_test_cp1");
        return;
    }

    /* Submit checkpoint */
    txHash = hpplite_node_submit_checkpoint(node);
    if (!txHash) {
        FAIL("Failed to submit checkpoint");
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        cleanup_test_files("/tmp/hpplite_test_cp1");
        return;
    }
    sqlite3_free(txHash);

    /* Verify checkpoint was stored on L1 */
    if (hpplite_l1_mock_get_checkpoint_count(l1) != 1) {
        FAIL("Checkpoint not stored on L1");
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        cleanup_test_files("/tmp/hpplite_test_cp1");
        return;
    }

    /* Should not need checkpoint now */
    if (hpplite_node_should_checkpoint(node)) {
        FAIL("Should not need checkpoint after submitting");
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        cleanup_test_files("/tmp/hpplite_test_cp1");
        return;
    }

    hpplite_node_destroy(node);
    hpplite_l1_disconnect(l1);
    cleanup_test_files("/tmp/hpplite_test_cp1");

    PASS();
}

/*
** Test 2: Multiple checkpoints
*/
static void test_checkpoint_multiple(void) {
    HppliteNodeConfig config;
    HppliteNode *node;
    HppliteL1 *l1;
    char *txHash;

    TEST("checkpoint_multiple");

    ensure_test_dir("/tmp/hpplite_test_cp2");

    l1 = hpplite_l1_create_mock();
    if (!l1) {
        FAIL("Failed to create L1 mock");
        return;
    }

    memset(&config, 0, sizeof(config));
    config.nodeId = "test-seq";
    memcpy(config.privkey, SEQ_PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = "/tmp/hpplite_test_cp2";
    config.dbPath = "/tmp/hpplite_test_cp2/test.db";
    config.checkpointInterval = 2;

    node = hpplite_node_create(&config);
    if (!node) {
        FAIL("Failed to create node");
        hpplite_l1_disconnect(l1);
        return;
    }

    hpplite_node_set_l1(node, l1);

    /* First checkpoint interval */
    hpplite_node_exec(node, "CREATE TABLE t1 (x)", NULL);
    hpplite_node_flush_batch(node);
    hpplite_node_exec(node, "INSERT INTO t1 VALUES (1)", NULL);
    hpplite_node_flush_batch(node);

    txHash = hpplite_node_submit_checkpoint(node);
    if (!txHash) {
        FAIL("First checkpoint failed");
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        cleanup_test_files("/tmp/hpplite_test_cp2");
        return;
    }
    sqlite3_free(txHash);

    /* Second checkpoint interval */
    hpplite_node_exec(node, "INSERT INTO t1 VALUES (2)", NULL);
    hpplite_node_flush_batch(node);
    hpplite_node_exec(node, "INSERT INTO t1 VALUES (3)", NULL);
    hpplite_node_flush_batch(node);

    txHash = hpplite_node_submit_checkpoint(node);
    if (!txHash) {
        FAIL("Second checkpoint failed");
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        cleanup_test_files("/tmp/hpplite_test_cp2");
        return;
    }
    sqlite3_free(txHash);

    /* Verify both checkpoints */
    if (hpplite_l1_mock_get_checkpoint_count(l1) != 2) {
        FAIL("Should have 2 checkpoints");
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        cleanup_test_files("/tmp/hpplite_test_cp2");
        return;
    }

    hpplite_node_destroy(node);
    hpplite_l1_disconnect(l1);
    cleanup_test_files("/tmp/hpplite_test_cp2");

    PASS();
}

/*
** Test 3: State root progression in checkpoints
*/
static void test_checkpoint_state_roots(void) {
    HppliteNodeConfig config;
    HppliteNode *node;
    HppliteL1 *l1;
    unsigned char root1[32], root2[32];
    char *txHash;

    TEST("checkpoint_state_roots");

    ensure_test_dir("/tmp/hpplite_test_cp3");

    l1 = hpplite_l1_create_mock();
    if (!l1) {
        FAIL("Failed to create L1 mock");
        return;
    }

    memset(&config, 0, sizeof(config));
    config.nodeId = "test-seq";
    memcpy(config.privkey, SEQ_PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = "/tmp/hpplite_test_cp3";
    config.dbPath = "/tmp/hpplite_test_cp3/test.db";
    config.checkpointInterval = 1;  /* Checkpoint every batch */

    node = hpplite_node_create(&config);
    if (!node) {
        FAIL("Failed to create node");
        hpplite_l1_disconnect(l1);
        return;
    }

    hpplite_node_set_l1(node, l1);

    /* First batch and checkpoint */
    hpplite_node_exec(node, "CREATE TABLE t (x INTEGER)", NULL);
    hpplite_node_flush_batch(node);
    hpplite_node_get_state_root(node, root1);

    txHash = hpplite_node_submit_checkpoint(node);
    if (!txHash) {
        FAIL("First checkpoint failed");
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        cleanup_test_files("/tmp/hpplite_test_cp3");
        return;
    }
    sqlite3_free(txHash);

    /* Second batch and checkpoint */
    hpplite_node_exec(node, "INSERT INTO t VALUES (42)", NULL);
    hpplite_node_flush_batch(node);
    hpplite_node_get_state_root(node, root2);

    txHash = hpplite_node_submit_checkpoint(node);
    if (!txHash) {
        FAIL("Second checkpoint failed");
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        cleanup_test_files("/tmp/hpplite_test_cp3");
        return;
    }
    sqlite3_free(txHash);

    /* State roots should be different */
    if (memcmp(root1, root2, 32) == 0) {
        FAIL("State roots should differ after INSERT");
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        cleanup_test_files("/tmp/hpplite_test_cp3");
        return;
    }

    /* Verify checkpoint state roots match */
    const HppliteCheckpoint *cp2 = hpplite_l1_mock_get_last_checkpoint(l1);
    if (!cp2 || memcmp(cp2->postStateRoot, root2, 32) != 0) {
        FAIL("Checkpoint state root mismatch");
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        cleanup_test_files("/tmp/hpplite_test_cp3");
        return;
    }

    hpplite_node_destroy(node);
    hpplite_l1_disconnect(l1);
    cleanup_test_files("/tmp/hpplite_test_cp3");

    PASS();
}

int main(void) {
    printf("\n=== Checkpoint Flow Tests (Singleton Model) ===\n\n");

    test_checkpoint_basic();
    test_checkpoint_multiple();
    test_checkpoint_state_roots();

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
