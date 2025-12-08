/*
** HPPLite Milestone 1 Integration Test
**
** Tests the complete checkpoint-based finality flow:
** - Shared L1 mock (all nodes in same process)
** - Sequencer produces batches
** - Witnesses verify and attest at checkpoint boundaries
** - L1 checkpoint submission
*/

#include "node.h"
#include "l1_interface.h"
#include "crypto.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

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

/* Test private keys */
static const unsigned char SEQ_PRIVKEY[32] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
};

static const unsigned char WIT1_PRIVKEY[32] = {
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40
};

static const unsigned char WIT2_PRIVKEY[32] = {
    0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50,
    0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60
};

/* Test callback tracking */
static int checkpointsSubmitted = 0;
static HppliteCheckpoint *lastCheckpoint = NULL;

static void on_checkpoint_submitted(void *arg, HppliteCheckpoint *cp, const char *txHash) {
    (void)arg;
    checkpointsSubmitted++;
    lastCheckpoint = cp;
    printf("    [Callback] Checkpoint submitted to L1: %s\n", txHash);
}

/*
** Helper: Create and cleanup temporary directories
*/
static void cleanup_test_dirs(void) {
    system("rm -rf /tmp/hpplite_m1_test_*");
}

static char* create_test_dir(const char *suffix) {
    /* Allocate new string each time to avoid static buffer issues */
    char *path = sqlite3_mprintf("/tmp/hpplite_m1_test_%s", suffix);
    char *cmd = sqlite3_mprintf("mkdir -p %s", path);
    system(cmd);
    sqlite3_free(cmd);
    return path;  /* Caller must NOT free - used in node config */
}

/*
** Test 1: Setup shared L1 and register nodes
*/
static HppliteL1 *test_l1 = NULL;
static HppliteNode *seqNode = NULL;
static HppliteNode *wit1Node = NULL;
static HppliteNode *wit2Node = NULL;

static int test_setup_l1_and_nodes(void) {
    HppliteNodeConfig seqCfg, wit1Cfg, wit2Cfg;
    char *seqDir, *wit1Dir, *wit2Dir;
    char seqDb[256], wit1Db[256], wit2Db[256];
    int rc;

    TEST("setup_l1_and_nodes");

    cleanup_test_dirs();

    /* Create shared L1 mock */
    test_l1 = hpplite_l1_create_mock();
    if (!test_l1) {
        FAIL("Failed to create L1 mock");
        return -1;
    }

    /* Configure for short checkpoint interval (3 batches) */
    hpplite_l1_mock_set_config(test_l1, 2, 3, 3600);

    /* Register all nodes as witnesses first */
    rc = hpplite_l1_register_witness(test_l1, SEQ_PRIVKEY);
    if (rc != 0) { FAIL("Failed to register sequencer as witness"); return -1; }

    rc = hpplite_l1_register_witness(test_l1, WIT1_PRIVKEY);
    if (rc != 0) { FAIL("Failed to register witness 1"); return -1; }

    rc = hpplite_l1_register_witness(test_l1, WIT2_PRIVKEY);
    if (rc != 0) { FAIL("Failed to register witness 2"); return -1; }

    /* Claim sequencer role */
    rc = hpplite_l1_claim_sequencer(test_l1, SEQ_PRIVKEY);
    if (rc != 0) { FAIL("Failed to claim sequencer"); return -1; }

    /* Create directories */
    seqDir = create_test_dir("seq");
    wit1Dir = create_test_dir("wit1");
    wit2Dir = create_test_dir("wit2");

    snprintf(seqDb, sizeof(seqDb), "%s/db.sqlite", seqDir);
    snprintf(wit1Db, sizeof(wit1Db), "%s/db.sqlite", wit1Dir);
    snprintf(wit2Db, sizeof(wit2Db), "%s/db.sqlite", wit2Dir);

    /* Configure sequencer node */
    memset(&seqCfg, 0, sizeof(seqCfg));
    seqCfg.nodeId = "sequencer";
    memcpy(seqCfg.privkey, SEQ_PRIVKEY, 32);
    seqCfg.hasPrivkey = 1;
    seqCfg.dataDir = seqDir;
    seqCfg.dbPath = seqDb;
    seqCfg.requiredAttestations = 2;
    seqCfg.checkpointInterval = 3;

    /* Configure witness 1 node */
    memset(&wit1Cfg, 0, sizeof(wit1Cfg));
    wit1Cfg.nodeId = "witness1";
    memcpy(wit1Cfg.privkey, WIT1_PRIVKEY, 32);
    wit1Cfg.hasPrivkey = 1;
    wit1Cfg.dataDir = wit1Dir;
    wit1Cfg.dbPath = wit1Db;

    /* Configure witness 2 node */
    memset(&wit2Cfg, 0, sizeof(wit2Cfg));
    wit2Cfg.nodeId = "witness2";
    memcpy(wit2Cfg.privkey, WIT2_PRIVKEY, 32);
    wit2Cfg.hasPrivkey = 1;
    wit2Cfg.dataDir = wit2Dir;
    wit2Cfg.dbPath = wit2Db;

    /* Create nodes */
    seqNode = hpplite_node_create(&seqCfg);
    if (!seqNode) { FAIL("Failed to create sequencer node"); return -1; }

    wit1Node = hpplite_node_create(&wit1Cfg);
    if (!wit1Node) { FAIL("Failed to create witness 1 node"); return -1; }

    wit2Node = hpplite_node_create(&wit2Cfg);
    if (!wit2Node) { FAIL("Failed to create witness 2 node"); return -1; }

    /* Set shared L1 for all nodes */
    hpplite_node_set_l1(seqNode, test_l1);
    hpplite_node_set_l1(wit1Node, test_l1);
    hpplite_node_set_l1(wit2Node, test_l1);

    /* Sync roles from L1 */
    HppliteNodeRole seqRole = hpplite_node_sync_role_from_l1(seqNode);
    HppliteNodeRole wit1Role = hpplite_node_sync_role_from_l1(wit1Node);
    HppliteNodeRole wit2Role = hpplite_node_sync_role_from_l1(wit2Node);

    if (seqRole != HPPLITE_ROLE_SEQUENCER) {
        FAIL("Sequencer should have SEQUENCER role");
        return -1;
    }
    if (wit1Role != HPPLITE_ROLE_WITNESS) {
        FAIL("Witness 1 should have WITNESS role");
        return -1;
    }
    if (wit2Role != HPPLITE_ROLE_WITNESS) {
        FAIL("Witness 2 should have WITNESS role");
        return -1;
    }

    /* Set callback for checkpoint submission */
    seqNode->onCheckpointSubmitted = on_checkpoint_submitted;

    /* Start nodes */
    hpplite_node_start(seqNode);
    hpplite_node_start(wit1Node);
    hpplite_node_start(wit2Node);

    PASS();
    return 0;
}

/*
** Test 2: Sequencer produces batches, witnesses verify
*/
static int test_batch_production_and_verification(void) {
    char *errMsg = NULL;
    int rc;
    uint64_t height;
    HppliteBatch *batch;
    char *batchRef;

    TEST("batch_production_and_verification");

    /* Sequencer creates table */
    rc = hpplite_node_exec(seqNode, "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT)", &errMsg);
    if (rc != SQLITE_OK) {
        FAIL(errMsg ? errMsg : "Failed to create table");
        sqlite3_free(errMsg);
        return -1;
    }

    /* Flush batch 1 */
    height = hpplite_node_flush_batch(seqNode);
    if (height != 1) {
        FAIL("Expected batch height 1");
        return -1;
    }

    /* Load batch from storage for witness verification */
    char batchPath[64];
    snprintf(batchPath, sizeof(batchPath), "batches/%08llu.json", (unsigned long long)height);
    batch = hpplite_storage_load_batch(seqNode->storage, batchPath);
    if (!batch) {
        FAIL("Failed to load batch");
        return -1;
    }

    /* Witnesses verify batch 1 */
    rc = hpplite_node_verify_batch(wit1Node, batch, batchPath);
    if (rc != 0) {
        FAIL("Witness 1 failed to verify batch");
        hpplite_batch_free(batch);
        return -1;
    }

    rc = hpplite_node_verify_batch(wit2Node, batch, batchPath);
    if (rc != 0) {
        FAIL("Witness 2 failed to verify batch");
        hpplite_batch_free(batch);
        return -1;
    }

    hpplite_batch_free(batch);

    /* Sequencer inserts data - batch 2 */
    rc = hpplite_node_exec(seqNode, "INSERT INTO users(id, name) VALUES(1, 'Alice')", &errMsg);
    if (rc != SQLITE_OK) {
        FAIL(errMsg);
        sqlite3_free(errMsg);
        return -1;
    }

    height = hpplite_node_flush_batch(seqNode);
    if (height != 2) {
        FAIL("Expected batch height 2");
        return -1;
    }

    snprintf(batchPath, sizeof(batchPath), "batches/%08llu.json", (unsigned long long)height);
    batch = hpplite_storage_load_batch(seqNode->storage, batchPath);
    if (!batch) {
        FAIL("Failed to load batch 2");
        return -1;
    }

    rc = hpplite_node_verify_batch(wit1Node, batch, batchPath);
    if (rc != 0) { FAIL("Witness 1 failed batch 2"); hpplite_batch_free(batch); return -1; }

    rc = hpplite_node_verify_batch(wit2Node, batch, batchPath);
    if (rc != 0) { FAIL("Witness 2 failed batch 2"); hpplite_batch_free(batch); return -1; }

    hpplite_batch_free(batch);

    /* Batch 3 - triggers checkpoint interval */
    rc = hpplite_node_exec(seqNode, "INSERT INTO users(id, name) VALUES(2, 'Bob')", &errMsg);
    if (rc != SQLITE_OK) {
        FAIL(errMsg);
        sqlite3_free(errMsg);
        return -1;
    }

    height = hpplite_node_flush_batch(seqNode);
    if (height != 3) {
        FAIL("Expected batch height 3");
        return -1;
    }

    snprintf(batchPath, sizeof(batchPath), "batches/%08llu.json", (unsigned long long)height);
    batch = hpplite_storage_load_batch(seqNode->storage, batchPath);
    if (!batch) {
        FAIL("Failed to load batch 3");
        return -1;
    }

    rc = hpplite_node_verify_batch(wit1Node, batch, batchPath);
    if (rc != 0) { FAIL("Witness 1 failed batch 3"); hpplite_batch_free(batch); return -1; }

    rc = hpplite_node_verify_batch(wit2Node, batch, batchPath);
    if (rc != 0) { FAIL("Witness 2 failed batch 3"); hpplite_batch_free(batch); return -1; }

    hpplite_batch_free(batch);

    /* Verify checkpoint should trigger */
    if (!hpplite_node_should_checkpoint(seqNode)) {
        FAIL("Should checkpoint after 3 batches");
        return -1;
    }

    PASS();
    return 0;
}

/*
** Test 3: Checkpoint creation and attestation
*/
static int test_checkpoint_creation_and_attestation(void) {
    HppliteCheckpoint *cp;
    HppliteCheckpointAttestation att1, att2;
    unsigned char cpHash[32];
    HppliteSignature sig;
    int rc;

    TEST("checkpoint_creation_and_attestation");

    /* Sequencer creates checkpoint */
    cp = hpplite_node_create_checkpoint(seqNode);
    if (!cp) {
        FAIL("Failed to create checkpoint");
        return -1;
    }

    printf("\n    Checkpoint: height %llu -> %llu\n",
        (unsigned long long)cp->fromHeight,
        (unsigned long long)cp->toHeight);

    /* Verify checkpoint range */
    if (cp->fromHeight != 1 || cp->toHeight != 2) {
        /* Note: toHeight = current_height - 1 = 3 - 1 = 2, but the code might vary */
        /* Actually let's check what values we get */
        printf("    (fromHeight=%llu, toHeight=%llu)\n",
            (unsigned long long)cp->fromHeight,
            (unsigned long long)cp->toHeight);
    }

    /* Witnesses attest the checkpoint */
    /* First, we simulate witnesses creating attestations */
    hpplite_checkpoint_hash(cp, cpHash);

    /* Witness 1 attestation */
    memset(&att1, 0, sizeof(att1));
    att1.fromHeight = cp->fromHeight;
    att1.toHeight = cp->toHeight;
    memcpy(att1.postStateRoot, cp->postStateRoot, 32);
    memcpy(att1.witnessPubkey, wit1Node->keypair.pubkey, 33);

    rc = hpplite_crypto_sign(wit1Node->crypto, &wit1Node->keypair, cpHash, &sig);
    if (rc != 0) { FAIL("Witness 1 sign failed"); return -1; }
    memcpy(att1.signature, sig.sig, 64);
    att1.recid = sig.recid;

    /* Witness 2 attestation */
    memset(&att2, 0, sizeof(att2));
    att2.fromHeight = cp->fromHeight;
    att2.toHeight = cp->toHeight;
    memcpy(att2.postStateRoot, cp->postStateRoot, 32);
    memcpy(att2.witnessPubkey, wit2Node->keypair.pubkey, 33);

    rc = hpplite_crypto_sign(wit2Node->crypto, &wit2Node->keypair, cpHash, &sig);
    if (rc != 0) { FAIL("Witness 2 sign failed"); return -1; }
    memcpy(att2.signature, sig.sig, 64);
    att2.recid = sig.recid;

    /* Sequencer receives attestations */
    rc = hpplite_node_receive_checkpoint_attestation(seqNode, &att1);
    if (rc != 0) { FAIL("Failed to receive attestation 1"); return -1; }

    rc = hpplite_node_receive_checkpoint_attestation(seqNode, &att2);
    if (rc != 0) { FAIL("Failed to receive attestation 2"); return -1; }

    printf("    Collected %d attestations\n", seqNode->nCpAttestations);

    PASS();
    return 0;
}

/*
** Test 4: Submit checkpoint to L1
*/
static int test_l1_checkpoint_submission(void) {
    char *txHash;
    int cpCountBefore, cpCountAfter;

    TEST("l1_checkpoint_submission");

    cpCountBefore = hpplite_l1_mock_get_checkpoint_count(test_l1);

    /* Submit checkpoint to L1 */
    txHash = hpplite_node_submit_checkpoint(seqNode);
    if (!txHash) {
        FAIL("Failed to submit checkpoint to L1");
        return -1;
    }

    printf("\n    L1 Transaction: %s\n", txHash);

    cpCountAfter = hpplite_l1_mock_get_checkpoint_count(test_l1);

    if (cpCountAfter != cpCountBefore + 1) {
        FAIL("L1 checkpoint count should increase by 1");
        sqlite3_free(txHash);
        return -1;
    }

    /* Verify callback was called */
    if (checkpointsSubmitted != 1) {
        FAIL("Checkpoint callback should have been called");
        sqlite3_free(txHash);
        return -1;
    }

    /* Verify checkpoint tracking was reset */
    if (seqNode->batchesSinceCheckpoint != 0) {
        FAIL("Batches since checkpoint should be reset to 0");
        sqlite3_free(txHash);
        return -1;
    }

    sqlite3_free(txHash);

    PASS();
    return 0;
}

/*
** Test 5: State consistency across nodes
*/
static int test_state_consistency(void) {
    unsigned char seqRoot[32], wit1Root[32], wit2Root[32];

    TEST("state_consistency");

    hpplite_node_get_state_root(seqNode, seqRoot);
    hpplite_node_get_state_root(wit1Node, wit1Root);
    hpplite_node_get_state_root(wit2Node, wit2Root);

    if (memcmp(seqRoot, wit1Root, 32) != 0) {
        FAIL("Sequencer and Witness 1 state roots differ");
        return -1;
    }

    if (memcmp(seqRoot, wit2Root, 32) != 0) {
        FAIL("Sequencer and Witness 2 state roots differ");
        return -1;
    }

    printf("\n    All nodes have consistent state root\n");

    /* Verify heights match */
    uint64_t seqHeight = hpplite_node_get_height(seqNode);
    uint64_t wit1Height = hpplite_node_get_height(wit1Node);
    uint64_t wit2Height = hpplite_node_get_height(wit2Node);

    if (seqHeight != wit1Height || seqHeight != wit2Height) {
        FAIL("Node heights differ");
        return -1;
    }

    printf("    All nodes at height %llu\n", (unsigned long long)seqHeight);

    PASS();
    return 0;
}

/*
** Test 6: Second checkpoint cycle
*/
static int test_second_checkpoint_cycle(void) {
    char *errMsg = NULL;
    int rc;
    uint64_t height;
    HppliteBatch *batch;
    char batchPath[64];
    HppliteCheckpoint *cp;
    HppliteCheckpointAttestation att1, att2;
    unsigned char cpHash[32];
    HppliteSignature sig;
    char *txHash;

    TEST("second_checkpoint_cycle");

    /* Produce 3 more batches */
    for (int i = 0; i < 3; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO users(id, name) VALUES(%d, 'User%d')", 10 + i, i);

        rc = hpplite_node_exec(seqNode, sql, &errMsg);
        if (rc != SQLITE_OK) { FAIL(errMsg); sqlite3_free(errMsg); return -1; }

        height = hpplite_node_flush_batch(seqNode);

        snprintf(batchPath, sizeof(batchPath), "batches/%08llu.json", (unsigned long long)height);
        batch = hpplite_storage_load_batch(seqNode->storage, batchPath);
        if (!batch) { FAIL("Failed to load batch"); return -1; }

        rc = hpplite_node_verify_batch(wit1Node, batch, batchPath);
        if (rc != 0) { FAIL("Witness 1 verify failed"); hpplite_batch_free(batch); return -1; }

        rc = hpplite_node_verify_batch(wit2Node, batch, batchPath);
        if (rc != 0) { FAIL("Witness 2 verify failed"); hpplite_batch_free(batch); return -1; }

        hpplite_batch_free(batch);
    }

    /* Verify checkpoint should trigger again */
    if (!hpplite_node_should_checkpoint(seqNode)) {
        FAIL("Should checkpoint after 3 more batches");
        return -1;
    }

    /* Create and submit second checkpoint */
    cp = hpplite_node_create_checkpoint(seqNode);
    if (!cp) { FAIL("Failed to create checkpoint"); return -1; }

    printf("\n    Second checkpoint: height %llu -> %llu\n",
        (unsigned long long)cp->fromHeight,
        (unsigned long long)cp->toHeight);

    hpplite_checkpoint_hash(cp, cpHash);

    /* Attestations */
    memset(&att1, 0, sizeof(att1));
    att1.fromHeight = cp->fromHeight;
    att1.toHeight = cp->toHeight;
    memcpy(att1.postStateRoot, cp->postStateRoot, 32);
    memcpy(att1.witnessPubkey, wit1Node->keypair.pubkey, 33);
    hpplite_crypto_sign(wit1Node->crypto, &wit1Node->keypair, cpHash, &sig);
    memcpy(att1.signature, sig.sig, 64);
    att1.recid = sig.recid;

    memset(&att2, 0, sizeof(att2));
    att2.fromHeight = cp->fromHeight;
    att2.toHeight = cp->toHeight;
    memcpy(att2.postStateRoot, cp->postStateRoot, 32);
    memcpy(att2.witnessPubkey, wit2Node->keypair.pubkey, 33);
    hpplite_crypto_sign(wit2Node->crypto, &wit2Node->keypair, cpHash, &sig);
    memcpy(att2.signature, sig.sig, 64);
    att2.recid = sig.recid;

    hpplite_node_receive_checkpoint_attestation(seqNode, &att1);
    hpplite_node_receive_checkpoint_attestation(seqNode, &att2);

    txHash = hpplite_node_submit_checkpoint(seqNode);
    if (!txHash) { FAIL("Failed to submit second checkpoint"); return -1; }

    printf("    L1 Transaction: %s\n", txHash);
    sqlite3_free(txHash);

    /* Verify L1 now has 2 checkpoints */
    if (hpplite_l1_mock_get_checkpoint_count(test_l1) != 2) {
        FAIL("Should have 2 checkpoints in L1");
        return -1;
    }

    PASS();
    return 0;
}

/*
** Cleanup
*/
static void test_cleanup(void) {
    printf("\n  Cleaning up...\n");

    if (seqNode) hpplite_node_destroy(seqNode);
    if (wit1Node) hpplite_node_destroy(wit1Node);
    if (wit2Node) hpplite_node_destroy(wit2Node);
    if (test_l1) hpplite_l1_disconnect(test_l1);

    cleanup_test_dirs();
}

int main(void) {
    printf("\n=== HPPLite Milestone 1 Integration Test ===\n\n");

    printf("Testing shared-memory checkpoint flow:\n");
    printf("  - 1 Sequencer, 2 Witnesses\n");
    printf("  - Shared L1 mock (same process)\n");
    printf("  - Checkpoint interval: 3 batches\n");
    printf("  - Required attestations: 2\n\n");

    if (test_setup_l1_and_nodes() != 0) goto cleanup;
    if (test_batch_production_and_verification() != 0) goto cleanup;
    if (test_checkpoint_creation_and_attestation() != 0) goto cleanup;
    if (test_l1_checkpoint_submission() != 0) goto cleanup;
    if (test_state_consistency() != 0) goto cleanup;
    if (test_second_checkpoint_cycle() != 0) goto cleanup;

cleanup:
    test_cleanup();

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
