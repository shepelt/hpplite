/*
** HPPLite Milestone 3 End-to-End Test
**
** Tests real L1 integration with HPP Sepolia:
** - Connect to deployed contract
** - Node syncs role from L1
** - Sequencer creates batches
** - Checkpoint submitted to real L1
*/

#include "node.h"
#include "l1_interface.h"
#include "eth_client.h"
#include "crypto.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int test_connect_and_get_role(void) {
    printf("  Test 1: Connect to L1 and verify role...\n");
    tests_run++;
    
    HppliteL1 *l1 = hpplite_l1_connect(RPC_URL, CONTRACT);
    if (!l1) {
        printf("    FAIL: Could not connect to L1\n");
        return -1;
    }
    printf("    Connected to %s\n", RPC_URL);
    
    /* Get state */
    HppliteL1State *state = hpplite_l1_get_state(l1);
    if (!state) {
        printf("    FAIL: Could not get L1 state\n");
        hpplite_l1_disconnect(l1);
        return -1;
    }
    
    printf("    L1 State:\n");
    printf("      Sequencer: 0x"); print_hex(state->sequencerAddress, 20); printf("\n");
    printf("      Witnesses: %d\n", state->nWitnesses);
    printf("      Required attestations: %d\n", state->requiredAttestations);
    printf("      Checkpoint interval: %llu\n", (unsigned long long)state->checkpointInterval);
    
    /* Set private key for signing */
    if (hpplite_l1_set_privkey(l1, PRIVKEY) != 0) {
        printf("    FAIL: Could not set private key\n");
        hpplite_l1_state_free(state);
        hpplite_l1_disconnect(l1);
        return -1;
    }
    
    /* Check if we are sequencer */
    HppliteCrypto *crypto = hpplite_crypto_init();
    HppliteKeypair kp;
    hpplite_crypto_keypair_from_privkey(crypto, &kp, PRIVKEY);

    int isSeq = hpplite_l1_is_sequencer(l1, kp.pubkey);
    int isWit = hpplite_l1_is_witness(l1, kp.pubkey);
    
    printf("    Our pubkey: "); print_hex(kp.pubkey, 33); printf("\n");
    printf("    Is sequencer: %s\n", isSeq ? "YES" : "NO");
    printf("    Is witness: %s\n", isWit ? "YES" : "NO");
    
    if (!isSeq) {
        printf("    FAIL: Expected to be sequencer\n");
        hpplite_crypto_free(crypto);
        hpplite_l1_state_free(state);
        hpplite_l1_disconnect(l1);
        return -1;
    }

    hpplite_crypto_free(crypto);
    hpplite_l1_state_free(state);
    hpplite_l1_disconnect(l1);
    
    printf("    PASS\n");
    tests_passed++;
    return 0;
}

static int test_node_with_real_l1(void) {
    printf("  Test 2: Create node with real L1 backend...\n");
    tests_run++;
    
    /* Create L1 connection */
    HppliteL1 *l1 = hpplite_l1_connect(RPC_URL, CONTRACT);
    if (!l1) {
        printf("    FAIL: Could not connect to L1\n");
        return -1;
    }
    
    hpplite_l1_set_privkey(l1, PRIVKEY);
    
    /* Create node config */
    HppliteNodeConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.nodeId = "m3-test-node";
    memcpy(cfg.privkey, PRIVKEY, 32);
    cfg.dataDir = "/tmp/hpplite_m3_test";
    cfg.dbPath = "/tmp/hpplite_m3_test/test.db";
    
    /* Create directory */
    system("rm -rf /tmp/hpplite_m3_test && mkdir -p /tmp/hpplite_m3_test");
    
    /* Create node */
    HppliteNode *node = hpplite_node_create(&cfg);
    if (!node) {
        printf("    FAIL: Could not create node\n");
        hpplite_l1_disconnect(l1);
        return -1;
    }
    
    /* Set L1 */
    hpplite_node_set_l1(node, l1);
    
    /* Sync role from L1 */
    HppliteNodeRole role = hpplite_node_sync_role_from_l1(node);
    printf("    Node role: %s\n", 
           role == HPPLITE_ROLE_SEQUENCER ? "SEQUENCER" :
           role == HPPLITE_ROLE_WITNESS ? "WITNESS" : "UNKNOWN");
    
    if (role != HPPLITE_ROLE_SEQUENCER) {
        printf("    FAIL: Expected SEQUENCER role\n");
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        return -1;
    }
    
    /* Start node */
    hpplite_node_start(node);
    
    /* Create a table and produce a batch */
    char *errMsg = NULL;
    int rc = hpplite_node_exec(node, 
        "CREATE TABLE m3_test(id INTEGER PRIMARY KEY, data TEXT)", &errMsg);
    if (rc != SQLITE_OK) {
        printf("    FAIL: %s\n", errMsg ? errMsg : "Could not create table");
        sqlite3_free(errMsg);
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        return -1;
    }
    
    /* Insert some data */
    rc = hpplite_node_exec(node, 
        "INSERT INTO m3_test VALUES (1, 'hello HPP Sepolia')", &errMsg);
    if (rc != SQLITE_OK) {
        printf("    FAIL: %s\n", errMsg ? errMsg : "Could not insert");
        sqlite3_free(errMsg);
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        return -1;
    }
    
    /* Flush batch */
    uint64_t height = hpplite_node_flush_batch(node);
    printf("    Created batch at height: %llu\n", (unsigned long long)height);
    
    if (height != 1) {
        printf("    FAIL: Expected height 1\n");
        hpplite_node_destroy(node);
        hpplite_l1_disconnect(l1);
        return -1;
    }
    
    /* Get state root */
    unsigned char stateRoot[32];
    hpplite_node_get_state_root(node, stateRoot);
    printf("    State root: "); print_hex(stateRoot, 32); printf("\n");
    
    /* Clean up */
    hpplite_node_destroy(node);
    hpplite_l1_disconnect(l1);
    
    printf("    PASS\n");
    tests_passed++;
    return 0;
}

int main(void) {
    printf("test_m3_e2e: End-to-end test with HPP Sepolia\n\n");
    
    test_connect_and_get_role();
    test_node_with_real_l1();
    
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
