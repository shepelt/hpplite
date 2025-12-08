/*
** HPPLite M3 Multi-Node E2E Test
**
** Full end-to-end test with multiple nodes:
** 1. Sequencer produces batches
** 2. Multiple witnesses verify and attest
** 3. Checkpoint quorum is achieved
** 4. State is consistent across all nodes
*/

#include "node.h"
#include "crypto.h"
#include "batch.h"
#include "l1_interface.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEQ_DIR "/tmp/hpplite_multinode_seq"
#define WIT1_DIR "/tmp/hpplite_multinode_wit1"
#define WIT2_DIR "/tmp/hpplite_multinode_wit2"
#define WIT3_DIR "/tmp/hpplite_multinode_wit3"

static int tests_run = 0;
static int tests_passed = 0;

static void print_hex(const unsigned char *data, int len) {
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
}

/* Node keys */
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

static const unsigned char WIT3_PRIVKEY[32] = {
    0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70,
    0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80
};

static HppliteBatch *load_batch(const char *dataDir, uint64_t height) {
    char path[256];
    snprintf(path, sizeof(path), "%s/batches/%08llu.json",
             dataDir, (unsigned long long)height);

    FILE *f = fopen(path, "r");
    if (!f) return NULL;

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

static HppliteNode *create_node(const char *id, const char *dir,
                                  const unsigned char *privkey, int role) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
    system(cmd);

    static char dbPath[256];
    snprintf(dbPath, sizeof(dbPath), "%s/db.sqlite", dir);

    HppliteNodeConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.nodeId = id;
    memcpy(cfg.privkey, privkey, 32);
    cfg.dataDir = dir;
    cfg.dbPath = dbPath;

    HppliteNode *node = hpplite_node_create(&cfg);
    if (!node) return NULL;

    hpplite_node_set_role(node, role);
    hpplite_node_start(node);
    return node;
}

/*
** Full multi-node E2E test
*/
static int test_multinode_e2e(void) {
    printf("Test: Multi-node E2E with checkpoint quorum\n");
    tests_run++;

    int numBatches = 10;
    int requiredAttestations = 2;  /* 2-of-3 quorum */
    char *errMsg = NULL;
    int rc;

    /* === Phase 1: Create all nodes === */
    printf("\n  Phase 1: Create network nodes\n");

    HppliteNode *seqNode = create_node("sequencer", SEQ_DIR, SEQ_PRIVKEY, HPPLITE_ROLE_SEQUENCER);
    HppliteNode *wit1Node = create_node("witness1", WIT1_DIR, WIT1_PRIVKEY, HPPLITE_ROLE_WITNESS);
    HppliteNode *wit2Node = create_node("witness2", WIT2_DIR, WIT2_PRIVKEY, HPPLITE_ROLE_WITNESS);
    HppliteNode *wit3Node = create_node("witness3", WIT3_DIR, WIT3_PRIVKEY, HPPLITE_ROLE_WITNESS);

    if (!seqNode || !wit1Node || !wit2Node || !wit3Node) {
        printf("  FAIL: Could not create all nodes\n");
        return -1;
    }

    printf("    Created: 1 sequencer + 3 witnesses\n");

    /* Get witness pubkeys */
    HppliteCrypto *crypto = hpplite_crypto_init();
    HppliteKeypair kp1, kp2, kp3;
    hpplite_crypto_keypair_from_privkey(crypto, &kp1, WIT1_PRIVKEY);
    hpplite_crypto_keypair_from_privkey(crypto, &kp2, WIT2_PRIVKEY);
    hpplite_crypto_keypair_from_privkey(crypto, &kp3, WIT3_PRIVKEY);

    /* === Phase 2: Sequencer creates batches === */
    printf("\n  Phase 2: Sequencer creates batches\n");

    rc = hpplite_node_exec(seqNode, "CREATE TABLE multinode_test(id INT PRIMARY KEY, val TEXT)", &errMsg);
    if (rc != SQLITE_OK) {
        printf("  FAIL: CREATE: %s\n", errMsg);
        sqlite3_free(errMsg);
        goto cleanup;
    }

    for (int i = 1; i <= numBatches; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO multinode_test VALUES(%d, 'row %d')", i, i);

        rc = hpplite_node_exec(seqNode, sql, &errMsg);
        if (rc != SQLITE_OK) {
            printf("  FAIL: INSERT %d: %s\n", i, errMsg);
            sqlite3_free(errMsg);
            goto cleanup;
        }

        uint64_t height = hpplite_node_flush_batch(seqNode);
        if (i == 1 || i == numBatches) {
            printf("    Batch %d created (height=%llu)\n", i, (unsigned long long)height);
        } else if (i == 2) {
            printf("    ...\n");
        }
    }

    unsigned char seqStateRoot[32];
    hpplite_node_get_state_root(seqNode, seqStateRoot);
    printf("    Sequencer final state: ");
    print_hex(seqStateRoot, 32);
    printf("\n");

    /* === Phase 3: All witnesses verify batches === */
    printf("\n  Phase 3: Witnesses verify batches\n");

    HppliteNode *witnesses[3] = {wit1Node, wit2Node, wit3Node};
    const char *witNames[3] = {"Witness 1", "Witness 2", "Witness 3"};
    unsigned char witStateRoots[3][32];

    for (int w = 0; w < 3; w++) {
        for (int h = 1; h <= numBatches; h++) {
            HppliteBatch *batch = load_batch(SEQ_DIR, h);
            if (!batch) {
                printf("  FAIL: Could not load batch %d for %s\n", h, witNames[w]);
                goto cleanup;
            }

            char batchPath[256];
            snprintf(batchPath, sizeof(batchPath), "%s/batches/%08d.json", SEQ_DIR, h);
            rc = hpplite_node_verify_batch(witnesses[w], batch, batchPath);
            hpplite_batch_free(batch);

            if (rc != 0) {
                printf("  FAIL: %s rejected batch %d\n", witNames[w], h);
                goto cleanup;
            }
        }

        hpplite_node_get_state_root(witnesses[w], witStateRoots[w]);
        printf("    %s verified all batches, state: ", witNames[w]);
        print_hex(witStateRoots[w], 8);
        printf("...\n");
    }

    /* === Phase 4: Verify state consistency === */
    printf("\n  Phase 4: Verify state consistency\n");

    int stateMatch = 1;
    for (int w = 0; w < 3; w++) {
        if (memcmp(seqStateRoot, witStateRoots[w], 32) != 0) {
            printf("    MISMATCH: %s has different state!\n", witNames[w]);
            stateMatch = 0;
        }
    }

    if (stateMatch) {
        printf("    All 4 nodes have identical state\n");
    } else {
        printf("  FAIL: State mismatch detected\n");
        goto cleanup;
    }

    /* === Phase 5: Create checkpoint with quorum === */
    printf("\n  Phase 5: Create checkpoint with 2-of-3 quorum\n");

    HppliteCheckpoint checkpoint;
    memset(&checkpoint, 0, sizeof(checkpoint));
    checkpoint.fromHeight = 1;
    checkpoint.toHeight = numBatches;
    memcpy(checkpoint.postStateRoot, seqStateRoot, 32);

    unsigned char cpHash[32];
    hpplite_checkpoint_hash(&checkpoint, cpHash);

    printf("    Checkpoint: heights %d-%d\n", 1, numBatches);
    printf("    Checkpoint hash: ");
    print_hex(cpHash, 32);
    printf("\n");

    /* Collect attestations from 2 witnesses (quorum) */
    HppliteCheckpointAttestation attestations[2];
    HppliteKeypair *keypairs[3] = {&kp1, &kp2, &kp3};
    int attCount = 0;

    for (int w = 0; w < 3 && attCount < requiredAttestations; w++) {
        HppliteSignature sig;
        rc = hpplite_crypto_sign(crypto, keypairs[w], cpHash, &sig);
        if (rc != 0) {
            printf("    %s failed to sign\n", witNames[w]);
            continue;
        }

        memset(&attestations[attCount], 0, sizeof(attestations[attCount]));
        attestations[attCount].fromHeight = checkpoint.fromHeight;
        attestations[attCount].toHeight = checkpoint.toHeight;
        memcpy(attestations[attCount].postStateRoot, checkpoint.postStateRoot, 32);
        memcpy(attestations[attCount].witnessPubkey, keypairs[w]->pubkey, 33);
        memcpy(attestations[attCount].signature, sig.sig, 64);
        attestations[attCount].recid = sig.recid;

        printf("    %s signed attestation (v=%d)\n", witNames[w], sig.recid + 27);
        attCount++;
    }

    if (attCount >= requiredAttestations) {
        printf("    Quorum achieved: %d/%d attestations\n", attCount, requiredAttestations);
    } else {
        printf("  FAIL: Could not achieve quorum\n");
        goto cleanup;
    }

    /* === Phase 6: Verify attestations === */
    printf("\n  Phase 6: Verify attestation signatures\n");

    int validSigs = 0;
    for (int i = 0; i < attCount; i++) {
        HppliteSignature sig;
        memcpy(sig.sig, attestations[i].signature, 64);
        sig.recid = attestations[i].recid;

        rc = hpplite_crypto_verify(crypto, attestations[i].witnessPubkey, cpHash, &sig);
        if (rc == 1) {
            validSigs++;
            printf("    Attestation %d: VALID\n", i + 1);
        } else {
            printf("    Attestation %d: INVALID\n", i + 1);
        }
    }

    if (validSigs >= requiredAttestations) {
        printf("\n  PASS: Checkpoint ready for L1 submission (%d valid signatures)\n", validSigs);
        tests_passed++;
    } else {
        printf("  FAIL: Not enough valid signatures\n");
    }

    /* === Phase 7: Query data on all nodes === */
    printf("\n  Phase 7: Verify data on all nodes\n");

    HppliteNode *allNodes[4] = {seqNode, wit1Node, wit2Node, wit3Node};
    const char *nodeNames[4] = {"Sequencer", "Witness 1", "Witness 2", "Witness 3"};

    for (int n = 0; n < 4; n++) {
        sqlite3_stmt *stmt;
        rc = sqlite3_prepare_v2(allNodes[n]->db,
            "SELECT COUNT(*) FROM multinode_test", -1, &stmt, NULL);
        if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            int count = sqlite3_column_int(stmt, 0);
            printf("    %s: %d rows\n", nodeNames[n], count);
        }
        sqlite3_finalize(stmt);
    }

cleanup:
    hpplite_crypto_free(crypto);
    hpplite_node_destroy(wit3Node);
    hpplite_node_destroy(wit2Node);
    hpplite_node_destroy(wit1Node);
    hpplite_node_destroy(seqNode);
    return 0;
}

int main(void) {
    printf("test_m3_multinode: Multi-node E2E test\n");
    printf("=====================================\n");

    test_multinode_e2e();

    printf("\n=====================================\n");
    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
