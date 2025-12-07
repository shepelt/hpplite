/*
** HPPLite Milestone 2 Multi-Process Node Test
**
** Tests actual node-to-node communication via ZeroMQ:
** - L1 service runs in separate thread (accessed via ZMQ)
** - Sequencer runs in separate thread with ZMQ transport
** - Witnesses run in separate threads with ZMQ transport
** - Batches broadcast via ZMQ PUB/SUB
** - Attestations sent via ZMQ DEALER/ROUTER
*/

#include "node.h"
#include "l1_interface.h"
#include "l1_service.h"
#include "crypto.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>

#ifdef HPPLITE_ENABLE_ZMQ

/* Test configuration - use high ports to avoid conflicts */
#define L1_ENDPOINT "tcp://127.0.0.1:16555"
#define SEQ_PUB_ENDPOINT "tcp://*:16560"
#define SEQ_PUB_ADDR "tcp://127.0.0.1:16560"

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

/* Global state for threads */
static volatile int l1_running = 0;
static volatile int seq_running = 0;
static volatile int wit1_running = 0;
static volatile int wit2_running = 0;

static HppliteL1Service *l1_service = NULL;
static HppliteNode *seq_node = NULL;
static HppliteNode *wit1_node = NULL;
static HppliteNode *wit2_node = NULL;

/* Synchronization */
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static int batches_verified_wit1 = 0;
static int batches_verified_wit2 = 0;
static int checkpoints_submitted = 0;

/*
** Helper: Cleanup test directories
*/
static void cleanup_test_dirs(void) {
    system("rm -rf /tmp/hpplite_m2_node_test_*");
}

static char* create_test_dir(const char *suffix) {
    char *path = sqlite3_mprintf("/tmp/hpplite_m2_node_test_%s", suffix);
    char *cmd = sqlite3_mprintf("mkdir -p %s", path);
    system(cmd);
    sqlite3_free(cmd);
    return path;
}

/*
** L1 Service Thread
*/
static void *l1_service_thread(void *arg) {
    (void)arg;

    while (l1_running) {
        hpplite_l1_service_process(l1_service, 50);
    }

    return NULL;
}

/*
** Sequencer Node Thread
*/
static void *sequencer_thread(void *arg) {
    (void)arg;

    while (seq_running) {
        hpplite_node_process(seq_node, 50);
    }

    return NULL;
}

/*
** Witness Node Threads
*/
static void on_batch_verified_wit1(void *arg, HppliteBatch *batch, int valid) {
    (void)arg;
    (void)batch;
    if (valid) {
        pthread_mutex_lock(&state_mutex);
        batches_verified_wit1++;
        pthread_mutex_unlock(&state_mutex);
    }
}

static void on_batch_verified_wit2(void *arg, HppliteBatch *batch, int valid) {
    (void)arg;
    (void)batch;
    if (valid) {
        pthread_mutex_lock(&state_mutex);
        batches_verified_wit2++;
        pthread_mutex_unlock(&state_mutex);
    }
}

static void *witness1_thread(void *arg) {
    (void)arg;

    while (wit1_running) {
        hpplite_node_process(wit1_node, 50);
    }

    return NULL;
}

static void *witness2_thread(void *arg) {
    (void)arg;

    while (wit2_running) {
        hpplite_node_process(wit2_node, 50);
    }

    return NULL;
}

/*
** Checkpoint callback
*/
static void on_checkpoint_submitted(void *arg, HppliteCheckpoint *cp, const char *txHash) {
    (void)arg;
    (void)cp;
    pthread_mutex_lock(&state_mutex);
    checkpoints_submitted++;
    pthread_mutex_unlock(&state_mutex);
    printf("    [Callback] Checkpoint submitted: %s\n", txHash);
}

/*
** Test 1: Setup L1 service and register nodes
*/
static pthread_t l1_thread;

static int test_setup_l1_service(void) {
    HppliteL1Client *client;
    unsigned char seqPubkey[HPPLITE_PUBKEY_SIZE];
    unsigned char wit1Pubkey[HPPLITE_PUBKEY_SIZE];
    unsigned char wit2Pubkey[HPPLITE_PUBKEY_SIZE];
    HppliteCrypto *crypto;
    HppliteKeypair kp;
    int rc;

    TEST("setup_l1_service");

    /* Create L1 service */
    l1_service = hpplite_l1_service_create(L1_ENDPOINT);
    if (!l1_service) {
        FAIL("Failed to create L1 service");
        return -1;
    }

    /* Start L1 service thread */
    l1_running = 1;
    pthread_create(&l1_thread, NULL, l1_service_thread, NULL);
    usleep(100000);  /* Let service start */

    /* Create client to configure L1 */
    client = hpplite_l1_client_create(L1_ENDPOINT);
    if (!client) {
        FAIL("Failed to create L1 client");
        return -1;
    }

    /* Configure L1: checkpoint interval 3, required attestations 2 */
    rc = hpplite_l1_client_set_config(client, 2, 3, 3600);
    if (rc != 0) {
        FAIL("Failed to set L1 config");
        hpplite_l1_client_destroy(client);
        return -1;
    }

    /* Derive public keys from private keys */
    crypto = hpplite_crypto_init();

    hpplite_crypto_keypair_from_privkey(crypto, &kp, SEQ_PRIVKEY);
    memcpy(seqPubkey, kp.pubkey, HPPLITE_PUBKEY_SIZE);

    hpplite_crypto_keypair_from_privkey(crypto, &kp, WIT1_PRIVKEY);
    memcpy(wit1Pubkey, kp.pubkey, HPPLITE_PUBKEY_SIZE);

    hpplite_crypto_keypair_from_privkey(crypto, &kp, WIT2_PRIVKEY);
    memcpy(wit2Pubkey, kp.pubkey, HPPLITE_PUBKEY_SIZE);

    hpplite_crypto_free(crypto);

    /* Register witnesses */
    rc = hpplite_l1_client_register_witness(client, wit1Pubkey);
    if (rc != 0) { FAIL("Failed to register witness 1"); hpplite_l1_client_destroy(client); return -1; }

    rc = hpplite_l1_client_register_witness(client, wit2Pubkey);
    if (rc != 0) { FAIL("Failed to register witness 2"); hpplite_l1_client_destroy(client); return -1; }

    /* Set sequencer */
    rc = hpplite_l1_client_set_sequencer(client, seqPubkey);
    if (rc != 0) { FAIL("Failed to set sequencer"); hpplite_l1_client_destroy(client); return -1; }

    hpplite_l1_client_destroy(client);

    PASS();
    return 0;
}

/*
** Test 2: Create and start nodes with ZMQ transport
*/
static pthread_t seq_thread, wit1_thread_h, wit2_thread_h;

static int test_create_nodes(void) {
    HppliteNodeConfig seqCfg, wit1Cfg, wit2Cfg;
    char *seqDir, *wit1Dir, *wit2Dir;
    char seqDb[256], wit1Db[256], wit2Db[256];

    TEST("create_nodes_with_zmq");

    cleanup_test_dirs();

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
    seqCfg.dataDir = seqDir;
    seqCfg.dbPath = seqDb;
    seqCfg.bindAddress = SEQ_PUB_ENDPOINT;  /* ZMQ bind address */
    seqCfg.requiredAttestations = 2;
    seqCfg.checkpointInterval = 3;

    /* Configure witness 1 node */
    memset(&wit1Cfg, 0, sizeof(wit1Cfg));
    wit1Cfg.nodeId = "witness1";
    memcpy(wit1Cfg.privkey, WIT1_PRIVKEY, 32);
    wit1Cfg.dataDir = wit1Dir;
    wit1Cfg.dbPath = wit1Db;
    wit1Cfg.sequencerAddress = SEQ_PUB_ADDR;  /* ZMQ connect address */

    /* Configure witness 2 node */
    memset(&wit2Cfg, 0, sizeof(wit2Cfg));
    wit2Cfg.nodeId = "witness2";
    memcpy(wit2Cfg.privkey, WIT2_PRIVKEY, 32);
    wit2Cfg.dataDir = wit2Dir;
    wit2Cfg.dbPath = wit2Db;
    wit2Cfg.sequencerAddress = SEQ_PUB_ADDR;  /* ZMQ connect address */

    /* Create nodes */
    seq_node = hpplite_node_create(&seqCfg);
    if (!seq_node) { FAIL("Failed to create sequencer node"); return -1; }

    wit1_node = hpplite_node_create(&wit1Cfg);
    if (!wit1_node) { FAIL("Failed to create witness 1 node"); return -1; }

    wit2_node = hpplite_node_create(&wit2Cfg);
    if (!wit2_node) { FAIL("Failed to create witness 2 node"); return -1; }

    /* Set callbacks */
    seq_node->onCheckpointSubmitted = on_checkpoint_submitted;
    wit1_node->onBatchVerified = on_batch_verified_wit1;
    wit2_node->onBatchVerified = on_batch_verified_wit2;

    /*
    ** Note: For M2, nodes should connect to L1 via ZMQ client.
    ** For now, we create local L1 mock for each node since
    ** the remote L1 wrapper isn't fully implemented yet.
    ** This tests the ZMQ node-to-node communication.
    */
    HppliteL1 *seqL1 = hpplite_l1_create_mock();
    HppliteL1 *wit1L1 = hpplite_l1_create_mock();
    HppliteL1 *wit2L1 = hpplite_l1_create_mock();

    /* Configure L1 mocks identically */
    hpplite_l1_mock_set_config(seqL1, 2, 3, 3600);
    hpplite_l1_mock_set_config(wit1L1, 2, 3, 3600);
    hpplite_l1_mock_set_config(wit2L1, 2, 3, 3600);

    /* Get pubkeys */
    unsigned char seqPubkey[HPPLITE_PUBKEY_SIZE];
    unsigned char wit1Pubkey[HPPLITE_PUBKEY_SIZE];
    unsigned char wit2Pubkey[HPPLITE_PUBKEY_SIZE];
    hpplite_node_get_pubkey(seq_node, seqPubkey);
    hpplite_node_get_pubkey(wit1_node, wit1Pubkey);
    hpplite_node_get_pubkey(wit2_node, wit2Pubkey);

    /* Register witnesses and set sequencer on each L1 mock */
    hpplite_l1_mock_add_witness(seqL1, wit1Pubkey);
    hpplite_l1_mock_add_witness(seqL1, wit2Pubkey);
    hpplite_l1_mock_set_sequencer(seqL1, seqPubkey);

    hpplite_l1_mock_add_witness(wit1L1, wit1Pubkey);
    hpplite_l1_mock_add_witness(wit1L1, wit2Pubkey);
    hpplite_l1_mock_set_sequencer(wit1L1, seqPubkey);

    hpplite_l1_mock_add_witness(wit2L1, wit1Pubkey);
    hpplite_l1_mock_add_witness(wit2L1, wit2Pubkey);
    hpplite_l1_mock_set_sequencer(wit2L1, seqPubkey);

    hpplite_node_set_l1(seq_node, seqL1);
    hpplite_node_set_l1(wit1_node, wit1L1);
    hpplite_node_set_l1(wit2_node, wit2L1);

    /* Sync roles from L1 */
    HppliteNodeRole seqRole = hpplite_node_sync_role_from_l1(seq_node);
    HppliteNodeRole wit1Role = hpplite_node_sync_role_from_l1(wit1_node);
    HppliteNodeRole wit2Role = hpplite_node_sync_role_from_l1(wit2_node);

    if (seqRole != HPPLITE_ROLE_SEQUENCER) {
        FAIL("Sequencer should have SEQUENCER role");
        return -1;
    }
    if (wit1Role != HPPLITE_ROLE_WITNESS || wit2Role != HPPLITE_ROLE_WITNESS) {
        FAIL("Witnesses should have WITNESS role");
        return -1;
    }

    /* Start nodes - this initializes ZMQ transport */
    if (hpplite_node_start(seq_node) != 0) {
        FAIL("Failed to start sequencer");
        return -1;
    }
    usleep(100000);  /* Let sequencer bind */

    if (hpplite_node_start(wit1_node) != 0) {
        FAIL("Failed to start witness 1");
        return -1;
    }

    if (hpplite_node_start(wit2_node) != 0) {
        FAIL("Failed to start witness 2");
        return -1;
    }

    usleep(200000);  /* Let connections establish */

    /* Start node threads */
    seq_running = 1;
    wit1_running = 1;
    wit2_running = 1;

    pthread_create(&seq_thread, NULL, sequencer_thread, NULL);
    pthread_create(&wit1_thread_h, NULL, witness1_thread, NULL);
    pthread_create(&wit2_thread_h, NULL, witness2_thread, NULL);

    usleep(100000);  /* Let threads start */

    PASS();
    return 0;
}

/*
** Test 3: Sequencer produces batches, witnesses receive via ZMQ
*/
static int test_zmq_batch_broadcast(void) {
    char *errMsg = NULL;
    int rc;
    uint64_t height;
    int timeout_ms = 5000;  /* 5 second timeout */
    int elapsed = 0;

    TEST("zmq_batch_broadcast");

    /* Reset counters */
    pthread_mutex_lock(&state_mutex);
    batches_verified_wit1 = 0;
    batches_verified_wit2 = 0;
    pthread_mutex_unlock(&state_mutex);

    /* Sequencer creates table and flushes batch */
    rc = hpplite_node_exec(seq_node, "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT)", &errMsg);
    if (rc != SQLITE_OK) {
        FAIL(errMsg ? errMsg : "Failed to create table");
        sqlite3_free(errMsg);
        return -1;
    }

    height = hpplite_node_flush_batch(seq_node);
    if (height != 1) {
        FAIL("Expected batch height 1");
        return -1;
    }

    printf("\n    Sequencer produced batch %llu\n", (unsigned long long)height);

    /* Wait for witnesses to receive and verify batch via ZMQ */
    while (elapsed < timeout_ms) {
        pthread_mutex_lock(&state_mutex);
        int v1 = batches_verified_wit1;
        int v2 = batches_verified_wit2;
        pthread_mutex_unlock(&state_mutex);

        if (v1 >= 1 && v2 >= 1) {
            printf("    Witnesses received batch via ZMQ\n");
            break;
        }

        usleep(50000);  /* 50ms */
        elapsed += 50;
    }

    pthread_mutex_lock(&state_mutex);
    int final_v1 = batches_verified_wit1;
    int final_v2 = batches_verified_wit2;
    pthread_mutex_unlock(&state_mutex);

    if (final_v1 < 1 || final_v2 < 1) {
        printf("\n    Witness 1 verified: %d, Witness 2 verified: %d\n", final_v1, final_v2);
        FAIL("Witnesses did not receive batch via ZMQ");
        return -1;
    }

    PASS();
    return 0;
}

/*
** Test 4: Multiple batches and state consistency
*/
static int test_multiple_batches(void) {
    char *errMsg = NULL;
    int rc;
    uint64_t height;
    int timeout_ms = 5000;
    int elapsed;

    TEST("multiple_batches_via_zmq");

    /* Produce batch 2 */
    rc = hpplite_node_exec(seq_node, "INSERT INTO users(id, name) VALUES(1, 'Alice')", &errMsg);
    if (rc != SQLITE_OK) { FAIL(errMsg); sqlite3_free(errMsg); return -1; }

    height = hpplite_node_flush_batch(seq_node);
    if (height != 2) { FAIL("Expected batch height 2"); return -1; }

    /* Produce batch 3 */
    rc = hpplite_node_exec(seq_node, "INSERT INTO users(id, name) VALUES(2, 'Bob')", &errMsg);
    if (rc != SQLITE_OK) { FAIL(errMsg); sqlite3_free(errMsg); return -1; }

    height = hpplite_node_flush_batch(seq_node);
    if (height != 3) { FAIL("Expected batch height 3"); return -1; }

    printf("\n    Produced batches up to height %llu\n", (unsigned long long)height);

    /* Wait for witnesses to verify all batches */
    elapsed = 0;
    while (elapsed < timeout_ms) {
        pthread_mutex_lock(&state_mutex);
        int v1 = batches_verified_wit1;
        int v2 = batches_verified_wit2;
        pthread_mutex_unlock(&state_mutex);

        if (v1 >= 3 && v2 >= 3) {
            printf("    Witnesses verified all 3 batches\n");
            break;
        }

        usleep(50000);
        elapsed += 50;
    }

    /* Verify state consistency */
    unsigned char seqRoot[32], wit1Root[32], wit2Root[32];
    hpplite_node_get_state_root(seq_node, seqRoot);
    hpplite_node_get_state_root(wit1_node, wit1Root);
    hpplite_node_get_state_root(wit2_node, wit2Root);

    if (memcmp(seqRoot, wit1Root, 32) != 0) {
        FAIL("Sequencer and Witness 1 state roots differ");
        return -1;
    }

    if (memcmp(seqRoot, wit2Root, 32) != 0) {
        FAIL("Sequencer and Witness 2 state roots differ");
        return -1;
    }

    printf("    All nodes have consistent state\n");

    PASS();
    return 0;
}

/*
** Test 5: Checkpoint flow with ZMQ attestations
*/
static int test_checkpoint_via_zmq(void) {
    HppliteCheckpoint *cp;
    int timeout_ms = 5000;
    int elapsed = 0;

    TEST("checkpoint_attestations_via_zmq");

    /* Verify checkpoint should trigger (3 batches) */
    if (!hpplite_node_should_checkpoint(seq_node)) {
        FAIL("Should checkpoint after 3 batches");
        return -1;
    }

    /* Reset counter */
    pthread_mutex_lock(&state_mutex);
    checkpoints_submitted = 0;
    pthread_mutex_unlock(&state_mutex);

    /* Create checkpoint - this broadcasts to witnesses via ZMQ */
    cp = hpplite_node_create_checkpoint(seq_node);
    if (!cp) {
        FAIL("Failed to create checkpoint");
        return -1;
    }

    printf("\n    Created checkpoint: height %llu -> %llu\n",
           (unsigned long long)cp->fromHeight,
           (unsigned long long)cp->toHeight);

    /* Wait for attestations to arrive via ZMQ and checkpoint to be submitted */
    int submitted = 0;
    int attestationsReceived = 0;
    while (elapsed < timeout_ms) {
        /* Check if we have enough attestations */
        if (seq_node->nCpAttestations >= 2 && !submitted) {
            attestationsReceived = seq_node->nCpAttestations;
            printf("    Received %d checkpoint attestations via ZMQ\n", attestationsReceived);

            /* Submit checkpoint */
            char *txHash = hpplite_node_submit_checkpoint(seq_node);
            if (txHash) {
                printf("    Checkpoint submitted: %s\n", txHash);
                sqlite3_free(txHash);
                submitted = 1;
                break;
            }
        }

        usleep(50000);
        elapsed += 50;
    }

    if (!submitted) {
        printf("\n    Only received %d attestations (needed 2)\n", seq_node->nCpAttestations);
        FAIL("Did not receive enough attestations via ZMQ");
        return -1;
    }

    PASS();
    return 0;
}

/*
** Test 6: L1-Anchored Replay from Genesis
** Fetch checkpoint from L1, replay batches, verify state matches L1 commitment
*/
static int test_genesis_replay(void) {
    HppliteNodeConfig replayCfg;
    HppliteNode *replayNode;
    char *replayDir;
    char replayDb[256];
    unsigned char replayRoot[32];
    int i;

    TEST("l1_anchored_genesis_replay");

    /* Fetch last checkpoint from L1 */
    HppliteL1 *l1 = seq_node->l1;
    const HppliteCheckpoint *l1Checkpoint = hpplite_l1_mock_get_last_checkpoint(l1);
    if (!l1Checkpoint) {
        FAIL("No checkpoint found in L1");
        return -1;
    }

    printf("\n    L1 Checkpoint: height %llu -> %llu\n",
           (unsigned long long)l1Checkpoint->fromHeight,
           (unsigned long long)l1Checkpoint->toHeight);

    /* Create fresh replay node */
    replayDir = create_test_dir("replay");
    snprintf(replayDb, sizeof(replayDb), "%s/db.sqlite", replayDir);

    memset(&replayCfg, 0, sizeof(replayCfg));
    replayCfg.nodeId = "replay";
    memcpy(replayCfg.privkey, WIT1_PRIVKEY, 32);
    replayCfg.dataDir = replayDir;
    replayCfg.dbPath = replayDb;

    replayNode = hpplite_node_create(&replayCfg);
    if (!replayNode) {
        FAIL("Failed to create replay node");
        return -1;
    }

    hpplite_node_set_role(replayNode, HPPLITE_ROLE_WITNESS);

    /* Replay batches up to L1 checkpoint height */
    printf("    Replaying batches to L1 checkpoint height...\n");

    int batchesReplayed = 0;
    for (i = 1; i <= (int)l1Checkpoint->toHeight; i++) {
        char batchPath[64];
        HppliteBatch *batch;

        /* In production: fetch from DA layer using batch hash from L1 */
        /* For test: load from sequencer storage (simulating DA fetch) */
        snprintf(batchPath, sizeof(batchPath), "batches/%08llu.json", (unsigned long long)i);
        batch = hpplite_storage_load_batch(seq_node->storage, batchPath);

        if (!batch) {
            printf("    Failed to fetch batch %d\n", i);
            FAIL("Batch not available");
            hpplite_node_destroy(replayNode);
            return -1;
        }

        /* Verify batch hash matches L1 commitment (if available) */
        /* TODO: L1 should store batch hashes for verification */

        /* Apply batch */
        int rc = hpplite_node_verify_batch(replayNode, batch, batchPath);
        if (rc != 0) {
            printf("    Failed to apply batch %d\n", i);
            FAIL("Batch replay failed");
            hpplite_batch_free(batch);
            hpplite_node_destroy(replayNode);
            return -1;
        }

        batchesReplayed++;
        hpplite_batch_free(batch);
    }

    printf("    Replayed %d batches\n", batchesReplayed);

    /* Verify state matches L1 checkpoint commitment */
    hpplite_node_get_state_root(replayNode, replayRoot);

    if (memcmp(l1Checkpoint->postStateRoot, replayRoot, 32) != 0) {
        printf("    State root does not match L1 checkpoint!\n");
        FAIL("L1 checkpoint state mismatch");
        hpplite_node_destroy(replayNode);
        return -1;
    }

    printf("    State verified against L1 checkpoint\n");

    hpplite_node_destroy(replayNode);

    PASS();
    return 0;
}

/*
** Cleanup
*/
static void test_cleanup(void) {
    printf("\n  Cleaning up...\n");

    /* Stop threads */
    seq_running = 0;
    wit1_running = 0;
    wit2_running = 0;
    l1_running = 0;

    /* Wait for threads */
    pthread_join(seq_thread, NULL);
    pthread_join(wit1_thread_h, NULL);
    pthread_join(wit2_thread_h, NULL);
    pthread_join(l1_thread, NULL);

    /* Destroy nodes */
    if (seq_node) {
        if (seq_node->l1) hpplite_l1_disconnect(seq_node->l1);
        hpplite_node_destroy(seq_node);
    }
    if (wit1_node) {
        if (wit1_node->l1) hpplite_l1_disconnect(wit1_node->l1);
        hpplite_node_destroy(wit1_node);
    }
    if (wit2_node) {
        if (wit2_node->l1) hpplite_l1_disconnect(wit2_node->l1);
        hpplite_node_destroy(wit2_node);
    }

    /* Destroy L1 service */
    if (l1_service) hpplite_l1_service_destroy(l1_service);

    cleanup_test_dirs();
}

#endif /* HPPLITE_ENABLE_ZMQ */

int main(void) {
    printf("\n=== HPPLite Milestone 2 Multi-Process Node Test ===\n\n");

#ifdef HPPLITE_ENABLE_ZMQ
    printf("Testing ZeroMQ node-to-node communication:\n");
    printf("  - L1 service in separate thread\n");
    printf("  - Sequencer with ZMQ PUB/ROUTER sockets\n");
    printf("  - Witnesses with ZMQ SUB/DEALER sockets\n");
    printf("  - Batch broadcast via PUB/SUB\n");
    printf("  - Attestations via DEALER/ROUTER\n\n");

    if (test_setup_l1_service() != 0) goto cleanup;
    if (test_create_nodes() != 0) goto cleanup;
    if (test_zmq_batch_broadcast() != 0) goto cleanup;
    if (test_multiple_batches() != 0) goto cleanup;
    if (test_checkpoint_via_zmq() != 0) goto cleanup;
    if (test_genesis_replay() != 0) goto cleanup;

cleanup:
    test_cleanup();

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
#else
    printf("ZeroMQ not enabled, skipping M2 node tests\n");
    return 0;
#endif
}
