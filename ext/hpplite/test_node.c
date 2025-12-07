/*
** Test HPPLite Node Module
**
** Tests sequencer and witness functionality in a single process.
** Simulates network communication by passing messages directly.
*/

#include "node.h"
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

/* Test private keys (for reproducible testing) */
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

/* Callback tracking */
static int batches_produced = 0;
static int batches_verified = 0;
static int commitments_finalized = 0;
static uint64_t last_finalized_height = 0;

static void on_batch_produced(void *arg, HppliteBatch *batch) {
    (void)arg;
    (void)batch;
    batches_produced++;
}

static void on_batch_verified(void *arg, HppliteBatch *batch, int valid) {
    (void)arg;
    (void)batch;
    if (valid) batches_verified++;
}

static void on_commitment_finalized(void *arg, uint64_t height, unsigned char *stateRoot) {
    (void)arg;
    (void)stateRoot;
    commitments_finalized++;
    last_finalized_height = height;
}

/* Helper: create test directory */
static void ensure_test_dir(const char *path) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", path);
    system(cmd);
}

/* Helper: cleanup test files */
static void cleanup_test_files(const char *dir) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);
}

/*
** Test 1: Node creation and basic properties
*/
static void test_node_create(void) {
    HppliteNodeConfig config;
    HppliteNode *node;
    unsigned char pubkey[HPPLITE_PUBKEY_SIZE];
    unsigned char address[HPPLITE_ADDRESS_SIZE];
    char *stats;

    TEST("node_create");

    ensure_test_dir("/tmp/hpplite_test_seq");

    memset(&config, 0, sizeof(config));
    config.nodeId = "test-sequencer";
    memcpy(config.privkey, SEQ_PRIVKEY, 32);
    config.dataDir = "/tmp/hpplite_test_seq";
    config.dbPath = "/tmp/hpplite_test_seq/test.db";
    config.requiredAttestations = 2;

    node = hpplite_node_create(&config);
    if (!node) {
        FAIL("Failed to create node");
        return;
    }

    /* Check pubkey is derived */
    hpplite_node_get_pubkey(node, pubkey);
    int hasNonZero = 0;
    for (int i = 0; i < HPPLITE_PUBKEY_SIZE; i++) {
        if (pubkey[i] != 0) hasNonZero = 1;
    }
    if (!hasNonZero) {
        FAIL("Pubkey is all zeros");
        hpplite_node_destroy(node);
        return;
    }

    /* Check address is derived */
    hpplite_node_get_address(node, address);
    hasNonZero = 0;
    for (int i = 0; i < HPPLITE_ADDRESS_SIZE; i++) {
        if (address[i] != 0) hasNonZero = 1;
    }
    if (!hasNonZero) {
        FAIL("Address is all zeros");
        hpplite_node_destroy(node);
        return;
    }

    /* Check stats */
    stats = hpplite_node_get_stats(node);
    if (!stats || strlen(stats) < 10) {
        FAIL("Stats not generated");
        hpplite_node_destroy(node);
        return;
    }
    sqlite3_free(stats);

    hpplite_node_destroy(node);
    cleanup_test_files("/tmp/hpplite_test_seq");

    PASS();
}

/*
** Test 2: Sequencer SQL execution and batch production
*/
static void test_sequencer_batches(void) {
    HppliteNodeConfig config;
    HppliteNode *sequencer;
    char *errMsg = NULL;
    int rc;
    uint64_t height;

    TEST("sequencer_batches");

    ensure_test_dir("/tmp/hpplite_test_seq2");

    memset(&config, 0, sizeof(config));
    config.nodeId = "test-sequencer";
    memcpy(config.privkey, SEQ_PRIVKEY, 32);
    config.dataDir = "/tmp/hpplite_test_seq2";
    config.dbPath = "/tmp/hpplite_test_seq2/test.db";
    config.requiredAttestations = 1;

    sequencer = hpplite_node_create(&config);
    if (!sequencer) {
        FAIL("Failed to create sequencer");
        return;
    }

    /* Set role to sequencer */
    hpplite_node_set_role(sequencer, HPPLITE_ROLE_SEQUENCER);

    /* Set callbacks */
    batches_produced = 0;
    sequencer->onBatchProduced = on_batch_produced;

    /* Execute SQL - should fail without role */
    rc = hpplite_node_exec(sequencer, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)", &errMsg);
    if (rc != 0) {
        FAIL(errMsg ? errMsg : "Failed to exec SQL");
        sqlite3_free(errMsg);
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_seq2");
        return;
    }

    /* Check pending count */
    if (hpplite_node_pending_count(sequencer) != 1) {
        FAIL("Pending count should be 1");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_seq2");
        return;
    }

    /* Execute more SQL */
    rc = hpplite_node_exec(sequencer, "INSERT INTO users (name) VALUES ('Alice')", &errMsg);
    if (rc != 0) {
        FAIL(errMsg ? errMsg : "Failed to insert");
        sqlite3_free(errMsg);
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_seq2");
        return;
    }

    /* Flush batch */
    height = hpplite_node_flush_batch(sequencer);
    if (height != 1) {
        FAIL("Expected batch height 1");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_seq2");
        return;
    }

    if (batches_produced != 1) {
        FAIL("Batch callback not called");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_seq2");
        return;
    }

    /* Pending should be 0 now */
    if (hpplite_node_pending_count(sequencer) != 0) {
        FAIL("Pending count should be 0 after flush");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_seq2");
        return;
    }

    hpplite_node_destroy(sequencer);
    cleanup_test_files("/tmp/hpplite_test_seq2");

    PASS();
}

/*
** Test 3: Message serialization round-trip
*/
static void test_message_serialization(void) {
    HppliteBatch *batch, *batch2;
    HppliteAttestation att, *att2;
    unsigned char *msg;
    int msgSize;
    char *batchRef = NULL;

    TEST("message_serialization");

    /* Create a test batch */
    batch = hpplite_batch_new(42);
    if (!batch) {
        FAIL("Failed to create batch");
        return;
    }
    batch->timestamp = 1234567890;
    memset(batch->preStateRoot, 0xAA, HPPLITE_HASH_SIZE);
    memset(batch->postStateRoot, 0xBB, HPPLITE_HASH_SIZE);
    hpplite_batch_add_txn(batch, "INSERT INTO foo VALUES (1)", 1);
    hpplite_batch_add_txn(batch, "UPDATE foo SET x=2", 3);

    /* Serialize batch message */
    msg = hpplite_node_serialize_batch_msg(NULL, batch, "batches/42.json", &msgSize);
    if (!msg) {
        FAIL("Failed to serialize batch message");
        hpplite_batch_free(batch);
        return;
    }

    /* Parse batch message */
    batch2 = hpplite_node_parse_batch_msg(msg, msgSize, &batchRef);
    sqlite3_free(msg);

    if (!batch2) {
        FAIL("Failed to parse batch message");
        hpplite_batch_free(batch);
        return;
    }

    /* Verify batch contents */
    if (batch2->height != 42 ||
        batch2->nTxns != 2 ||
        memcmp(batch2->preStateRoot, batch->preStateRoot, HPPLITE_HASH_SIZE) != 0 ||
        memcmp(batch2->postStateRoot, batch->postStateRoot, HPPLITE_HASH_SIZE) != 0 ||
        !batchRef ||
        strcmp(batchRef, "batches/42.json") != 0) {
        FAIL("Batch contents mismatch");
        hpplite_batch_free(batch);
        hpplite_batch_free(batch2);
        sqlite3_free(batchRef);
        return;
    }

    hpplite_batch_free(batch);
    hpplite_batch_free(batch2);
    sqlite3_free(batchRef);

    /* Test attestation serialization */
    memset(&att, 0, sizeof(att));
    att.height = 123;
    memset(att.stateRoot, 0xCC, HPPLITE_HASH_SIZE);
    memset(att.witnessPubkey, 0xDD, HPPLITE_PUBKEY_SIZE);
    memset(att.sig.sig, 0xEE, HPPLITE_SIGNATURE_SIZE);
    att.sig.recid = 1;
    att.batchRef = "batches/123.json";

    msg = hpplite_node_serialize_attestation_msg(NULL, &att, &msgSize);
    if (!msg) {
        FAIL("Failed to serialize attestation");
        return;
    }

    att2 = hpplite_node_parse_attestation_msg(msg, msgSize);
    sqlite3_free(msg);

    if (!att2) {
        FAIL("Failed to parse attestation");
        return;
    }

    if (att2->height != 123 ||
        att2->sig.recid != 1 ||
        memcmp(att2->stateRoot, att.stateRoot, HPPLITE_HASH_SIZE) != 0 ||
        memcmp(att2->witnessPubkey, att.witnessPubkey, HPPLITE_PUBKEY_SIZE) != 0 ||
        strcmp(att2->batchRef, "batches/123.json") != 0) {
        FAIL("Attestation contents mismatch");
        sqlite3_free(att2->batchRef);
        sqlite3_free(att2);
        return;
    }

    sqlite3_free(att2->batchRef);
    sqlite3_free(att2);

    PASS();
}

/*
** Test 4: Full sequencer-witness flow (in-process simulation)
*/
static void test_full_flow(void) {
    HppliteNodeConfig seqConfig, wit1Config, wit2Config;
    HppliteNode *sequencer, *witness1, *witness2;
    char *errMsg = NULL;
    int rc;
    uint64_t height;
    unsigned char wit1Pubkey[HPPLITE_PUBKEY_SIZE];
    unsigned char wit2Pubkey[HPPLITE_PUBKEY_SIZE];

    TEST("full_flow");

    ensure_test_dir("/tmp/hpplite_test_flow/seq");
    ensure_test_dir("/tmp/hpplite_test_flow/wit1");
    ensure_test_dir("/tmp/hpplite_test_flow/wit2");

    /* Create sequencer */
    memset(&seqConfig, 0, sizeof(seqConfig));
    seqConfig.nodeId = "sequencer";
    memcpy(seqConfig.privkey, SEQ_PRIVKEY, 32);
    seqConfig.dataDir = "/tmp/hpplite_test_flow/seq";
    seqConfig.dbPath = "/tmp/hpplite_test_flow/seq/test.db";
    seqConfig.requiredAttestations = 2;

    sequencer = hpplite_node_create(&seqConfig);
    if (!sequencer) {
        FAIL("Failed to create sequencer");
        return;
    }
    hpplite_node_set_role(sequencer, HPPLITE_ROLE_SEQUENCER);
    hpplite_node_start(sequencer);

    /* Create witness 1 */
    memset(&wit1Config, 0, sizeof(wit1Config));
    wit1Config.nodeId = "witness1";
    memcpy(wit1Config.privkey, WIT1_PRIVKEY, 32);
    wit1Config.dataDir = "/tmp/hpplite_test_flow/wit1";
    wit1Config.dbPath = "/tmp/hpplite_test_flow/wit1/test.db";

    witness1 = hpplite_node_create(&wit1Config);
    if (!witness1) {
        FAIL("Failed to create witness1");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_flow");
        return;
    }
    hpplite_node_set_role(witness1, HPPLITE_ROLE_WITNESS);
    hpplite_node_start(witness1);

    /* Create witness 2 */
    memset(&wit2Config, 0, sizeof(wit2Config));
    wit2Config.nodeId = "witness2";
    memcpy(wit2Config.privkey, WIT2_PRIVKEY, 32);
    wit2Config.dataDir = "/tmp/hpplite_test_flow/wit2";
    wit2Config.dbPath = "/tmp/hpplite_test_flow/wit2/test.db";

    witness2 = hpplite_node_create(&wit2Config);
    if (!witness2) {
        FAIL("Failed to create witness2");
        hpplite_node_destroy(sequencer);
        hpplite_node_destroy(witness1);
        cleanup_test_files("/tmp/hpplite_test_flow");
        return;
    }
    hpplite_node_set_role(witness2, HPPLITE_ROLE_WITNESS);
    hpplite_node_start(witness2);

    /* Register witnesses with sequencer */
    hpplite_node_get_pubkey(witness1, wit1Pubkey);
    hpplite_node_get_pubkey(witness2, wit2Pubkey);
    hpplite_node_add_witness(sequencer, wit1Pubkey);
    hpplite_node_add_witness(sequencer, wit2Pubkey);

    /* Set callbacks */
    batches_produced = 0;
    batches_verified = 0;
    commitments_finalized = 0;
    sequencer->onBatchProduced = on_batch_produced;
    sequencer->onCommitmentFinalized = on_commitment_finalized;
    witness1->onBatchVerified = on_batch_verified;
    witness2->onBatchVerified = on_batch_verified;

    /* Sequencer: Create schema and insert data */
    rc = hpplite_node_exec(sequencer, "CREATE TABLE accounts (id INTEGER PRIMARY KEY, balance INTEGER)", &errMsg);
    if (rc != 0) {
        FAIL(errMsg ? errMsg : "Failed to create table");
        goto cleanup;
    }

    rc = hpplite_node_exec(sequencer, "INSERT INTO accounts (balance) VALUES (100), (200), (300)", &errMsg);
    if (rc != 0) {
        FAIL(errMsg ? errMsg : "Failed to insert");
        goto cleanup;
    }

    /* Witnesses: Execute same SQL to get same state */
    hpplite_exec(witness1->ctx, "CREATE TABLE accounts (id INTEGER PRIMARY KEY, balance INTEGER)", NULL);
    hpplite_exec(witness1->ctx, "INSERT INTO accounts (balance) VALUES (100), (200), (300)", NULL);

    hpplite_exec(witness2->ctx, "CREATE TABLE accounts (id INTEGER PRIMARY KEY, balance INTEGER)", NULL);
    hpplite_exec(witness2->ctx, "INSERT INTO accounts (balance) VALUES (100), (200), (300)", NULL);

    /* Flush the initial setup as block 1 for witnesses */
    hpplite_flush_block(witness1->ctx);
    hpplite_flush_block(witness2->ctx);

    /* Sequencer: Flush batch */
    height = hpplite_node_flush_batch(sequencer);
    if (height != 1) {
        FAIL("Expected batch height 1");
        goto cleanup;
    }

    /* Simulate batch transmission - in real system this goes over ZeroMQ */
    /* Get the batch ref and load batch from storage */
    char batchRefPath[64];
    snprintf(batchRefPath, sizeof(batchRefPath), "batches/%08llu.json", (unsigned long long)height);

    HppliteBatch *batch = hpplite_storage_load_batch(sequencer->storage, batchRefPath);
    if (!batch) {
        FAIL("Failed to load batch from storage");
        goto cleanup;
    }

    const char *batchRef = batchRefPath;

    /* Witnesses verify batch and produce attestations */
    /* Note: In this test, witnesses already have the state, so verification should pass */
    /* But since they already have the state through direct execution, we need to verify
       the post-state root matches */

    unsigned char seqStateRoot[HPPLITE_HASH_SIZE];
    unsigned char wit1StateRoot[HPPLITE_HASH_SIZE];
    unsigned char wit2StateRoot[HPPLITE_HASH_SIZE];

    hpplite_node_get_state_root(sequencer, seqStateRoot);
    hpplite_node_get_state_root(witness1, wit1StateRoot);
    hpplite_node_get_state_root(witness2, wit2StateRoot);

    if (memcmp(seqStateRoot, wit1StateRoot, HPPLITE_HASH_SIZE) != 0 ||
        memcmp(seqStateRoot, wit2StateRoot, HPPLITE_HASH_SIZE) != 0) {
        FAIL("State roots don't match between nodes");
        hpplite_batch_free(batch);
        goto cleanup;
    }

    /* Create attestations manually (simulating what witnesses would do) */
    HppliteAttestation att1, att2;
    unsigned char msgHash[HPPLITE_HASH_SIZE];

    /* Witness 1 attestation */
    memset(&att1, 0, sizeof(att1));
    att1.height = height;
    memcpy(att1.stateRoot, seqStateRoot, HPPLITE_HASH_SIZE);
    att1.batchRef = batchRef;
    hpplite_node_get_pubkey(witness1, att1.witnessPubkey);
    hpplite_hash_commitment(height, seqStateRoot, batchRef, msgHash);
    hpplite_crypto_sign(witness1->crypto, &witness1->keypair, msgHash, &att1.sig);

    /* Witness 2 attestation */
    memset(&att2, 0, sizeof(att2));
    att2.height = height;
    memcpy(att2.stateRoot, seqStateRoot, HPPLITE_HASH_SIZE);
    att2.batchRef = batchRef;
    hpplite_node_get_pubkey(witness2, att2.witnessPubkey);
    hpplite_crypto_sign(witness2->crypto, &witness2->keypair, msgHash, &att2.sig);

    /* Sequencer receives attestations */
    rc = hpplite_node_receive_attestation(sequencer, &att1);
    if (rc != 0) {
        FAIL("Failed to receive attestation 1");
        hpplite_batch_free(batch);
        goto cleanup;
    }

    rc = hpplite_node_receive_attestation(sequencer, &att2);
    if (rc != 0) {
        FAIL("Failed to receive attestation 2");
        hpplite_batch_free(batch);
        goto cleanup;
    }

    /* Process to trigger finalization check */
    hpplite_node_process(sequencer, 0);

    if (commitments_finalized != 1 || last_finalized_height != 1) {
        FAIL("Commitment should be finalized");
        hpplite_batch_free(batch);
        goto cleanup;
    }

    hpplite_batch_free(batch);

    /* Print stats */
    char *stats = hpplite_node_get_stats(sequencer);
    printf("\n    Sequencer stats: %s\n", stats);
    sqlite3_free(stats);

    PASS();

cleanup:
    hpplite_node_destroy(sequencer);
    hpplite_node_destroy(witness1);
    hpplite_node_destroy(witness2);
    cleanup_test_files("/tmp/hpplite_test_flow");
}

/*
** Test 5: Attestation validation
*/
static void test_attestation_validation(void) {
    HppliteNodeConfig seqConfig, witConfig;
    HppliteNode *sequencer, *witness;
    HppliteAttestation att;
    unsigned char witPubkey[HPPLITE_PUBKEY_SIZE];
    unsigned char stateRoot[HPPLITE_HASH_SIZE];
    unsigned char msgHash[HPPLITE_HASH_SIZE];
    int rc;

    TEST("attestation_validation");

    ensure_test_dir("/tmp/hpplite_test_att/seq");
    ensure_test_dir("/tmp/hpplite_test_att/wit");

    /* Create sequencer */
    memset(&seqConfig, 0, sizeof(seqConfig));
    seqConfig.nodeId = "sequencer";
    memcpy(seqConfig.privkey, SEQ_PRIVKEY, 32);
    seqConfig.dataDir = "/tmp/hpplite_test_att/seq";
    seqConfig.dbPath = "/tmp/hpplite_test_att/seq/test.db";
    seqConfig.requiredAttestations = 1;

    sequencer = hpplite_node_create(&seqConfig);
    if (!sequencer) {
        FAIL("Failed to create sequencer");
        return;
    }
    hpplite_node_set_role(sequencer, HPPLITE_ROLE_SEQUENCER);

    /* Create witness */
    memset(&witConfig, 0, sizeof(witConfig));
    witConfig.nodeId = "witness";
    memcpy(witConfig.privkey, WIT1_PRIVKEY, 32);
    witConfig.dataDir = "/tmp/hpplite_test_att/wit";
    witConfig.dbPath = "/tmp/hpplite_test_att/wit/test.db";

    witness = hpplite_node_create(&witConfig);
    if (!witness) {
        FAIL("Failed to create witness");
        hpplite_node_destroy(sequencer);
        cleanup_test_files("/tmp/hpplite_test_att");
        return;
    }
    hpplite_node_set_role(witness, HPPLITE_ROLE_WITNESS);

    /* Register witness */
    hpplite_node_get_pubkey(witness, witPubkey);
    hpplite_node_add_witness(sequencer, witPubkey);

    /* Execute SQL and flush batch */
    hpplite_node_exec(sequencer, "CREATE TABLE t (x)", NULL);
    hpplite_node_flush_batch(sequencer);

    /* Get state root */
    hpplite_node_get_state_root(sequencer, stateRoot);

    /* Create valid attestation */
    memset(&att, 0, sizeof(att));
    att.height = 1;
    memcpy(att.stateRoot, stateRoot, HPPLITE_HASH_SIZE);
    att.batchRef = "test_batch";
    memcpy(att.witnessPubkey, witPubkey, HPPLITE_PUBKEY_SIZE);
    hpplite_hash_commitment(1, stateRoot, "test_batch", msgHash);
    hpplite_crypto_sign(witness->crypto, &witness->keypair, msgHash, &att.sig);

    /* Should succeed */
    rc = hpplite_node_receive_attestation(sequencer, &att);
    if (rc != 0) {
        FAIL("Valid attestation rejected");
        goto cleanup;
    }

    /* Test: unknown witness should be rejected */
    unsigned char unknownPubkey[HPPLITE_PUBKEY_SIZE];
    memset(unknownPubkey, 0x99, HPPLITE_PUBKEY_SIZE);
    memcpy(att.witnessPubkey, unknownPubkey, HPPLITE_PUBKEY_SIZE);

    rc = hpplite_node_receive_attestation(sequencer, &att);
    if (rc == 0) {
        FAIL("Unknown witness should be rejected");
        goto cleanup;
    }

    /* Test: invalid signature should be rejected */
    memcpy(att.witnessPubkey, witPubkey, HPPLITE_PUBKEY_SIZE);
    att.sig.sig[0] ^= 0xFF; /* Corrupt signature */

    rc = hpplite_node_receive_attestation(sequencer, &att);
    if (rc == 0) {
        FAIL("Invalid signature should be rejected");
        goto cleanup;
    }

    PASS();

cleanup:
    hpplite_node_destroy(sequencer);
    hpplite_node_destroy(witness);
    cleanup_test_files("/tmp/hpplite_test_att");
}

int main(void) {
    printf("\n=== HPPLite Node Tests ===\n\n");

    test_node_create();
    test_sequencer_batches();
    test_message_serialization();
    test_full_flow();
    test_attestation_validation();

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
