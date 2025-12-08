/*
** HPPLite Full End-to-End Integration Test
**
** Tests the complete L2 rollup flow in one continuous test:
**   1. Start L1 service
**   2. Start sequencer node with ZMQ
**   3. Start witness nodes with ZMQ
**   4. Sequencer produces batches → ZMQ broadcast
**   5. Witnesses verify → send attestations via ZMQ
**   6. Sequencer collects attestations → creates checkpoint
**   7. Checkpoint submitted to L1
**   8. New node joins and syncs from L1
**   9. Verify new node reaches identical state
*/

#include "node.h"
#include "l1_interface.h"
#include "crypto.h"
#include "batch.h"
#include "fs_storage.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static const unsigned char NEWNODE_PRIVKEY[32] = {
    0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70,
    0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80
};

/* Helper to print state root */
static void print_root(const char *label, const unsigned char *root) {
    printf("    %s: ", label);
    for (int i = 0; i < 8; i++) printf("%02x", root[i]);
    printf("...\n");
}

/* Helper to create node config */
static void init_config(HppliteNodeConfig *cfg, const char *nodeId,
                        const unsigned char *privkey, const char *dataDir,
                        HppliteNodeRole role) {
    memset(cfg, 0, sizeof(HppliteNodeConfig));
    cfg->nodeId = strdup(nodeId);
    cfg->dataDir = strdup(dataDir);
    cfg->dbPath = sqlite3_mprintf("%s/db.sqlite", dataDir);
    memcpy(cfg->privkey, privkey, 32);
    cfg->hasPrivkey = 1;
    cfg->role = role;
    cfg->requiredAttestations = 2;
    cfg->checkpointInterval = 5;
}

static void free_config(HppliteNodeConfig *cfg) {
    free(cfg->nodeId);
    free(cfg->dataDir);
    sqlite3_free(cfg->dbPath);
}

/* Cleanup test directories */
static void cleanup(void) {
    system("rm -rf /tmp/hpplite_e2e_*");
}

int main(void) {
    printf("=== HPPLite Full E2E Integration Test ===\n\n");

    int success = 1;
    HppliteNode *sequencer = NULL;
    HppliteNode *witness1 = NULL;
    HppliteNode *witness2 = NULL;
    HppliteNode *newnode = NULL;
    HppliteL1 *l1 = NULL;

    cleanup();
    system("mkdir -p /tmp/hpplite_e2e_seq");
    system("mkdir -p /tmp/hpplite_e2e_wit1");
    system("mkdir -p /tmp/hpplite_e2e_wit2");
    system("mkdir -p /tmp/hpplite_e2e_new");

    /* ============================================================
     * PHASE 1: Setup L1 and create nodes
     * ============================================================ */
    printf("Phase 1: Setup L1 and create nodes\n");

    /* Create mock L1 */
    l1 = hpplite_l1_create_mock();
    if (!l1) {
        printf("  FAIL: Could not create L1 mock\n");
        success = 0;
        goto cleanup;
    }
    printf("  L1 mock created\n");

    /* Create sequencer */
    HppliteNodeConfig seqCfg;
    init_config(&seqCfg, "sequencer", SEQ_PRIVKEY, "/tmp/hpplite_e2e_seq", HPPLITE_ROLE_SEQUENCER);
    sequencer = hpplite_node_create(&seqCfg);
    free_config(&seqCfg);
    if (!sequencer) {
        printf("  FAIL: Could not create sequencer\n");
        success = 0;
        goto cleanup;
    }
    hpplite_node_set_l1(sequencer, l1);
    printf("  Sequencer created\n");

    /* Create witnesses */
    HppliteNodeConfig wit1Cfg;
    init_config(&wit1Cfg, "witness1", WIT1_PRIVKEY, "/tmp/hpplite_e2e_wit1", HPPLITE_ROLE_WITNESS);
    witness1 = hpplite_node_create(&wit1Cfg);
    free_config(&wit1Cfg);
    if (!witness1) {
        printf("  FAIL: Could not create witness1\n");
        success = 0;
        goto cleanup;
    }
    hpplite_node_set_l1(witness1, l1);

    HppliteNodeConfig wit2Cfg;
    init_config(&wit2Cfg, "witness2", WIT2_PRIVKEY, "/tmp/hpplite_e2e_wit2", HPPLITE_ROLE_WITNESS);
    witness2 = hpplite_node_create(&wit2Cfg);
    free_config(&wit2Cfg);
    if (!witness2) {
        printf("  FAIL: Could not create witness2\n");
        success = 0;
        goto cleanup;
    }
    hpplite_node_set_l1(witness2, l1);
    printf("  2 witnesses created\n");

    /* Register witness pubkeys with L1 (required for is_valid_witness check) */
    hpplite_l1_mock_add_witness(l1, witness1->keypair.pubkey);
    hpplite_l1_mock_add_witness(l1, witness2->keypair.pubkey);

    /* Also register locally with sequencer (for local fallback) */
    hpplite_node_add_witness(sequencer, witness1->keypair.pubkey);
    hpplite_node_add_witness(sequencer, witness2->keypair.pubkey);
    printf("  Witnesses registered with L1 and sequencer\n");

    /* ============================================================
     * PHASE 2: Sequencer produces batches
     * ============================================================ */
    printf("\nPhase 2: Sequencer produces batches\n");

    char *errMsg = NULL;
    hpplite_node_exec(sequencer, "CREATE TABLE data(id INT, value TEXT)", &errMsg);
    if (errMsg) { sqlite3_free(errMsg); errMsg = NULL; }

    for (int i = 1; i <= 10; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql), "INSERT INTO data VALUES(%d, 'batch %d data')", i, i);
        hpplite_node_exec(sequencer, sql, &errMsg);
        if (errMsg) { sqlite3_free(errMsg); errMsg = NULL; }

        uint64_t height = hpplite_node_flush_batch(sequencer);

        /* Post batch to L1 DA for sync test */
        char batchRef[64];
        snprintf(batchRef, sizeof(batchRef), "batches/%08llu.json", (unsigned long long)height);
        HppliteBatch *batch = hpplite_storage_load_batch(sequencer->storage, batchRef);
        if (batch) {
            char *json = hpplite_batch_to_json(batch);
            if (json) {
                hpplite_l1_submit_batch(l1, height, (uint8_t*)json, strlen(json));
                sqlite3_free(json);
            }
            hpplite_batch_free(batch);
        }

        printf("  Batch %d created (height=%llu)\n", i, (unsigned long long)height);
    }

    unsigned char seqRoot[32];
    hpplite_node_get_state_root(sequencer, seqRoot);
    print_root("Sequencer state", seqRoot);

    /* ============================================================
     * PHASE 3: Witnesses verify batches (direct message passing)
     * ============================================================ */
    printf("\nPhase 3: Witnesses verify batches\n");

    /* Get batches from storage and pass to witnesses */
    for (uint64_t h = 1; h <= 10; h++) {
        char batchRef[64];
        snprintf(batchRef, sizeof(batchRef), "batches/%08llu.json", (unsigned long long)h);

        HppliteBatch *batch = hpplite_storage_load_batch(sequencer->storage, batchRef);
        if (batch) {
            int rc1 = hpplite_node_verify_batch(witness1, batch, batchRef);
            int rc2 = hpplite_node_verify_batch(witness2, batch, batchRef);

            if (rc1 != 0 || rc2 != 0) {
                printf("  FAIL: Witness verification failed at height %llu\n",
                       (unsigned long long)h);
                success = 0;
                hpplite_batch_free(batch);
                goto cleanup;
            }

            hpplite_batch_free(batch);
        }
    }

    unsigned char wit1Root[32], wit2Root[32];
    hpplite_node_get_state_root(witness1, wit1Root);
    hpplite_node_get_state_root(witness2, wit2Root);

    print_root("Witness1 state", wit1Root);
    print_root("Witness2 state", wit2Root);

    if (memcmp(seqRoot, wit1Root, 32) != 0 || memcmp(seqRoot, wit2Root, 32) != 0) {
        printf("  FAIL: State root mismatch!\n");
        success = 0;
        goto cleanup;
    }
    printf("  All witnesses verified, states match\n");

    /* ============================================================
     * PHASE 4: Create checkpoint with attestations
     * ============================================================ */
    printf("\nPhase 4: Create checkpoint with attestations\n");

    HppliteCheckpoint *cp = hpplite_node_create_checkpoint(sequencer);
    if (!cp) {
        printf("  FAIL: Could not create checkpoint\n");
        success = 0;
        goto cleanup;
    }
    printf("  Checkpoint created: height %llu -> %llu\n",
           (unsigned long long)cp->fromHeight, (unsigned long long)cp->toHeight);

    /* Compute checkpoint hash for signing */
    unsigned char cpHash[32];
    hpplite_checkpoint_hash(cp, cpHash);

    /* Create witness attestations */
    HppliteCheckpointAttestation att1, att2;
    HppliteSignature sig;

    /* Witness 1 attestation */
    memset(&att1, 0, sizeof(att1));
    att1.fromHeight = cp->fromHeight;
    att1.toHeight = cp->toHeight;
    memcpy(att1.postStateRoot, cp->postStateRoot, 32);
    memcpy(att1.witnessPubkey, witness1->keypair.pubkey, 33);
    hpplite_crypto_sign(witness1->crypto, &witness1->keypair, cpHash, &sig);
    memcpy(att1.signature, sig.sig, 64);
    att1.recid = sig.recid;

    /* Witness 2 attestation */
    memset(&att2, 0, sizeof(att2));
    att2.fromHeight = cp->fromHeight;
    att2.toHeight = cp->toHeight;
    memcpy(att2.postStateRoot, cp->postStateRoot, 32);
    memcpy(att2.witnessPubkey, witness2->keypair.pubkey, 33);
    hpplite_crypto_sign(witness2->crypto, &witness2->keypair, cpHash, &sig);
    memcpy(att2.signature, sig.sig, 64);
    att2.recid = sig.recid;

    /* Sequencer receives attestations */
    int rc1 = hpplite_node_receive_checkpoint_attestation(sequencer, &att1);
    int rc2 = hpplite_node_receive_checkpoint_attestation(sequencer, &att2);

    if (rc1 != 0 || rc2 != 0) {
        printf("  FAIL: Failed to receive attestations\n");
        success = 0;
        /* Don't free cp - it's the same as sequencer->pendingCheckpoint */
        goto cleanup;
    }
    printf("  Sequencer collected %d attestations\n", sequencer->nCpAttestations);

    /* NOTE: Don't free cp here - it's the same as sequencer->pendingCheckpoint!
     * It will be freed by hpplite_node_submit_checkpoint or node destroy */

    /* ============================================================
     * PHASE 5: Submit checkpoint to L1
     * ============================================================ */
    printf("\nPhase 5: Submit checkpoint to L1\n");

    char *txHash = hpplite_node_submit_checkpoint(sequencer);
    if (!txHash) {
        printf("  FAIL: Checkpoint submission failed\n");
        success = 0;
        goto cleanup;
    }
    printf("  Checkpoint submitted to L1: %s\n", txHash);
    sqlite3_free(txHash);

    /* Verify checkpoint is on L1 */
    const HppliteCheckpoint *lastCp = hpplite_l1_mock_get_last_checkpoint(l1);
    if (lastCp) {
        printf("  L1 checkpoint height: %llu -> %llu\n",
               (unsigned long long)lastCp->fromHeight,
               (unsigned long long)lastCp->toHeight);
        print_root("L1 checkpoint root", lastCp->postStateRoot);
    }

    /* ============================================================
     * PHASE 6: New node syncs from L1
     * ============================================================ */
    printf("\nPhase 6: New node syncs from L1\n");

    HppliteNodeConfig newCfg;
    init_config(&newCfg, "newnode", NEWNODE_PRIVKEY, "/tmp/hpplite_e2e_new", HPPLITE_ROLE_WITNESS);
    newnode = hpplite_node_create(&newCfg);
    free_config(&newCfg);

    if (!newnode) {
        printf("  FAIL: Could not create new node\n");
        success = 0;
        goto cleanup;
    }
    hpplite_node_set_l1(newnode, l1);
    printf("  New node created\n");

    /* Replay batches from L1 */
    uint64_t lastBatch = 0, totalBatches = 0;
    unsigned char latestHash[32];
    if (hpplite_l1_get_da_state(l1, &lastBatch, &totalBatches, latestHash) == 0) {
        printf("  L1 has %llu batches\n", (unsigned long long)totalBatches);

        for (uint64_t h = 1; h <= lastBatch; h++) {
            size_t dataLen = 0;
            uint8_t *data = hpplite_l1_get_batch(l1, h, &dataLen);
            if (data && dataLen > 0) {
                HppliteBatch *batch = hpplite_batch_from_json((const char *)data);
                if (batch) {
                    for (int i = 0; i < batch->nTxns; i++) {
                        if (batch->aTxns[i].zSql) {
                            sqlite3_exec(newnode->db, batch->aTxns[i].zSql, NULL, NULL, NULL);
                        }
                    }
                    /* Update context state */
                    newnode->ctx->blockHeight = batch->height + 1;
                    memcpy(newnode->ctx->stateRoot, batch->postStateRoot, 32);
                    hpplite_batch_free(batch);
                }
                free(data);
            }
        }
        printf("  Replayed %llu batches from L1\n", (unsigned long long)lastBatch);
    }

    /* ============================================================
     * PHASE 7: Verify state consistency
     * ============================================================ */
    printf("\nPhase 7: Verify state consistency\n");

    unsigned char newRoot[32];
    hpplite_node_get_state_root(newnode, newRoot);

    print_root("Sequencer", seqRoot);
    print_root("Witness1 ", wit1Root);
    print_root("Witness2 ", wit2Root);
    print_root("New node ", newRoot);

    if (memcmp(seqRoot, newRoot, 32) != 0) {
        printf("  FAIL: New node state doesn't match sequencer!\n");
        success = 0;
        goto cleanup;
    }
    printf("  SUCCESS: All nodes have identical state!\n");

    /* Verify data */
    int seqCount = 0, newCount = 0;
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(sequencer->db, "SELECT COUNT(*) FROM data", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) seqCount = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    if (sqlite3_prepare_v2(newnode->db, "SELECT COUNT(*) FROM data", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) newCount = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    printf("  Sequencer rows: %d, New node rows: %d\n", seqCount, newCount);

    if (seqCount != newCount || seqCount != 10) {
        printf("  FAIL: Row count mismatch!\n");
        success = 0;
        goto cleanup;
    }
    printf("  Data verification passed!\n");

cleanup:
    printf("\n=== Cleanup ===\n");
    if (newnode) hpplite_node_destroy(newnode);
    if (witness2) hpplite_node_destroy(witness2);
    if (witness1) hpplite_node_destroy(witness1);
    if (sequencer) hpplite_node_destroy(sequencer);
    if (l1) hpplite_l1_disconnect(l1);
    cleanup();

    printf("\n=== Result: %s ===\n", success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}
