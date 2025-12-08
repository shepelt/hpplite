/*
** HPPLite M3 Checkpoint Submission Test
**
** Tests the complete checkpoint flow with real L1:
** 1. Sequencer creates batches
** 2. Witnesses sign attestations
** 3. Checkpoint submitted to HPP Sepolia
** 4. Verify checkpoint recorded on L1
*/

#include "node.h"
#include "l1_interface.h"
#include "eth_client.h"
#include "crypto.h"
#include "batch.h"
#include "keccak256.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Private key from .env (0xF3Ac9af2367393d0faC75ae4d31cAe340ceb0051) */
static const unsigned char PRIVKEY[32] = {
    0x8a, 0x8b, 0x77, 0xa3, 0xd7, 0xad, 0xe1, 0xe8,
    0x4c, 0x34, 0x7e, 0x25, 0xc6, 0x17, 0x82, 0xa3,
    0x7a, 0x93, 0xe4, 0x56, 0x1b, 0x98, 0xf5, 0x3d,
    0xf7, 0x1e, 0xb4, 0x50, 0x2d, 0x6a, 0x12, 0x76
};

/* Contract and RPC config */
#define RPC_URL "https://sepolia.hpp.io"
#define CONTRACT "0x2aC7688dFd3f81f189294cd12586776f43F19492"

/* Extern for setting L1 private key */
extern int hpplite_l1_set_privkey(HppliteL1 *l1, const unsigned char privkey[32]);

static int tests_run = 0;
static int tests_passed = 0;

static void print_hex(const unsigned char *data, int len) {
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
}

/*
** Test checkpoint creation and submission
*/
static int test_checkpoint_submission(void) {
    printf("Test: Checkpoint submission to HPP Sepolia\n");
    tests_run++;

    /* Create L1 connection */
    HppliteL1 *l1 = hpplite_l1_connect(RPC_URL, CONTRACT);
    if (!l1) {
        printf("  FAIL: Could not connect to L1\n");
        return -1;
    }

    if (hpplite_l1_set_privkey(l1, PRIVKEY) != 0) {
        printf("  FAIL: Could not set L1 private key\n");
        hpplite_l1_disconnect(l1);
        return -1;
    }
    printf("  Connected to L1\n");

    /* Get current L1 state */
    HppliteL1State *state = hpplite_l1_get_state(l1);
    if (!state) {
        printf("  FAIL: Could not get L1 state\n");
        hpplite_l1_disconnect(l1);
        return -1;
    }

    uint64_t lastHeight = state->lastCheckpointHeight;
    printf("  Last checkpoint height: %llu\n", (unsigned long long)lastHeight);
    printf("  Checkpoint interval: %llu\n", (unsigned long long)state->checkpointInterval);
    printf("  Required attestations: %d\n", state->requiredAttestations);
    hpplite_l1_state_free(state);

    /* Create sequencer node */
    system("rm -rf /tmp/hpplite_cp_test && mkdir -p /tmp/hpplite_cp_test");

    HppliteNodeConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.nodeId = "checkpoint-test";
    memcpy(cfg.privkey, PRIVKEY, 32);
    cfg.dataDir = "/tmp/hpplite_cp_test";
    cfg.dbPath = "/tmp/hpplite_cp_test/test.db";

    HppliteNode *node = hpplite_node_create(&cfg);
    if (!node) {
        printf("  FAIL: Could not create node\n");
        hpplite_l1_disconnect(l1);
        return -1;
    }

    hpplite_node_set_l1(node, l1);
    hpplite_node_sync_role_from_l1(node);
    hpplite_node_start(node);
    printf("  Node created with role: SEQUENCER\n");

    /* Create batches up to checkpoint interval */
    char *errMsg = NULL;
    int rc;

    rc = hpplite_node_exec(node, "CREATE TABLE cp_test(id INTEGER PRIMARY KEY, val TEXT)", &errMsg);
    if (rc != SQLITE_OK) {
        printf("  FAIL: %s\n", errMsg ? errMsg : "create table failed");
        sqlite3_free(errMsg);
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        return -1;
    }

    /* Create 10 batches (checkpoint interval) */
    for (int i = 1; i <= 10; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO cp_test VALUES (%d, 'batch %d')", i, i);
        rc = hpplite_node_exec(node, sql, &errMsg);
        if (rc != SQLITE_OK) {
            printf("  FAIL: insert %d: %s\n", i, errMsg);
            sqlite3_free(errMsg);
            hpplite_node_destroy(node);
            hpplite_l1_disconnect(l1);
            return -1;
        }

        uint64_t height = hpplite_node_flush_batch(node);
        if (i <= 3 || i >= 9) {
            printf("  Created batch %llu\n", (unsigned long long)height);
        } else if (i == 4) {
            printf("  ...\n");
        }
    }

    /* Get state root */
    unsigned char stateRoot[32];
    hpplite_node_get_state_root(node, stateRoot);
    printf("  Final state root: ");
    print_hex(stateRoot, 32);
    printf("\n");

    /* Create checkpoint - start from last checkpoint + 1 */
    HppliteCheckpoint checkpoint;
    memset(&checkpoint, 0, sizeof(checkpoint));
    checkpoint.fromHeight = (lastHeight == 0) ? 1 : lastHeight + 1;
    checkpoint.toHeight = checkpoint.fromHeight + 9;  /* 10 batches */
    memcpy(checkpoint.postStateRoot, stateRoot, 32);

    printf("  Checkpoint: height %llu -> %llu\n",
           (unsigned long long)checkpoint.fromHeight,
           (unsigned long long)checkpoint.toHeight);

    /* Create attestation (we are both sequencer and witness) */
    HppliteCrypto *crypto = hpplite_crypto_init();
    HppliteKeypair kp;
    hpplite_crypto_keypair_from_privkey(crypto, &kp, PRIVKEY);

    /* Hash the checkpoint for signing - must match contract:
       message = keccak256(abi.encodePacked(fromHeight, toHeight, stateRoot))
       ethSignedHash = keccak256("\x19Ethereum Signed Message:\n32" || message)
    */
    unsigned char message[32];
    {
        /* abi.encodePacked(uint256, uint256, bytes32) = 32 + 32 + 32 bytes */
        unsigned char data[96];
        memset(data, 0, 96);
        /* fromHeight as uint256 (big-endian, right-aligned) */
        data[24] = (checkpoint.fromHeight >> 56) & 0xFF;
        data[25] = (checkpoint.fromHeight >> 48) & 0xFF;
        data[26] = (checkpoint.fromHeight >> 40) & 0xFF;
        data[27] = (checkpoint.fromHeight >> 32) & 0xFF;
        data[28] = (checkpoint.fromHeight >> 24) & 0xFF;
        data[29] = (checkpoint.fromHeight >> 16) & 0xFF;
        data[30] = (checkpoint.fromHeight >> 8) & 0xFF;
        data[31] = checkpoint.fromHeight & 0xFF;
        /* toHeight as uint256 */
        data[56] = (checkpoint.toHeight >> 56) & 0xFF;
        data[57] = (checkpoint.toHeight >> 48) & 0xFF;
        data[58] = (checkpoint.toHeight >> 40) & 0xFF;
        data[59] = (checkpoint.toHeight >> 32) & 0xFF;
        data[60] = (checkpoint.toHeight >> 24) & 0xFF;
        data[61] = (checkpoint.toHeight >> 16) & 0xFF;
        data[62] = (checkpoint.toHeight >> 8) & 0xFF;
        data[63] = checkpoint.toHeight & 0xFF;
        /* stateRoot as bytes32 */
        memcpy(data + 64, stateRoot, 32);
        keccak256(data, 96, message);
    }

    /* Apply EIP-191 prefix: "\x19Ethereum Signed Message:\n32" + message */
    unsigned char cpHash[32];
    {
        unsigned char prefixed[32 + 28];  /* prefix is 28 bytes */
        memcpy(prefixed, "\x19" "Ethereum Signed Message:\n32", 28);
        memcpy(prefixed + 28, message, 32);
        keccak256(prefixed, 60, cpHash);
    }

    printf("  Message hash: ");
    print_hex(message, 32);
    printf("\n");
    printf("  EIP-191 hash: ");
    print_hex(cpHash, 32);
    printf("\n");

    /* Sign */
    HppliteSignature sig;
    if (hpplite_crypto_sign(crypto, &kp, cpHash, &sig) != 0) {
        printf("  FAIL: Could not sign checkpoint\n");
        hpplite_crypto_free(crypto);
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        return -1;
    }

    /* Create attestation */
    HppliteCheckpointAttestation att;
    memset(&att, 0, sizeof(att));
    att.fromHeight = checkpoint.fromHeight;
    att.toHeight = checkpoint.toHeight;
    memcpy(att.postStateRoot, stateRoot, 32);
    memcpy(att.witnessPubkey, kp.pubkey, 33);
    memcpy(att.signature, sig.sig, 64);
    att.recid = sig.recid;

    printf("  Signature r: ");
    print_hex(sig.sig, 32);
    printf("\n  Signature s: ");
    print_hex(sig.sig + 32, 32);
    printf("\n  Signature v: %d (recid=%d)\n", sig.recid + 27, sig.recid);
    printf("  Attestation signed by witness\n");

    /* For testing, we'll create a second attestation with the same key
       (in production, this would be from a different witness) */
    HppliteCheckpointAttestation attestations[2];
    attestations[0] = att;
    attestations[1] = att;  /* Same attestation - contract may reject this */

    /* Submit checkpoint to L1 */
    printf("  Submitting checkpoint to L1...\n");
    char *txHash = hpplite_l1_submit_checkpoint(l1, &checkpoint, attestations, 2);

    if (!txHash) {
        printf("  FAIL: Checkpoint submission failed\n");
        printf("  Note: Contract requires 2 different witnesses\n");
        hpplite_crypto_free(crypto);
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        return -1;
    }

    printf("  TX Hash: %s\n", txHash);

    /* Wait for confirmation */
    printf("  Waiting for confirmation (up to 60s)...\n");
    int txStatus = hpplite_l1_wait_for_tx(l1, txHash, 60);
    printf("  TX result: %s\n",
           txStatus == 1 ? "SUCCESS" :
           txStatus == 0 ? "REVERTED" : "PENDING/ERROR");

    free(txHash);

    /* Verify checkpoint on L1 */
    state = hpplite_l1_get_state(l1);
    if (state) {
        printf("  L1 state after checkpoint:\n");
        printf("    Last checkpoint height: %llu\n",
               (unsigned long long)state->lastCheckpointHeight);
        printf("    Last state root: ");
        print_hex(state->lastStateRoot, 32);
        printf("\n");

        if (state->lastCheckpointHeight >= checkpoint.toHeight) {
            printf("  PASS: Checkpoint recorded on L1 (height=%llu)\n",
                   (unsigned long long)state->lastCheckpointHeight);
            tests_passed++;
        } else {
            printf("  FAIL: Checkpoint height not updated (expected >=%llu, got %llu)\n",
                   (unsigned long long)checkpoint.toHeight,
                   (unsigned long long)state->lastCheckpointHeight);
        }
        hpplite_l1_state_free(state);
    }

    hpplite_crypto_free(crypto);
    hpplite_node_destroy(node);
    hpplite_l1_disconnect(l1);

    return 0;
}

int main(void) {
    printf("test_m3_checkpoint: Checkpoint submission to HPP Sepolia\n\n");

    test_checkpoint_submission();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
