/*
** Test L1 Ethereum client against HPP Sepolia
**
** Requires .env file with:
**   HPPLITE_L1_RPC=https://sepolia.hpp.io
**   HPPLITE_CONTRACT_ADDRESS=0x...
*/

#include "l1_interface.h"
#include "test_env.h"
#include <stdio.h>
#include <string.h>

/* External function not in l1_interface.h (defined in l1_eth.c) */
extern int hpplite_l1_set_privkey(HppliteL1 *l1, const unsigned char privkey[32]);

static int tests_run = 0;
static int tests_passed = 0;

static const char *rpc_url = NULL;
static const char *contract = NULL;

static void print_hex(const unsigned char *data, int len) {
    for (int i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
}

static void test_connect(void) {
    tests_run++;

    HppliteL1 *l1 = hpplite_l1_connect(rpc_url, contract);

    if (l1) {
        printf("  PASS: connect to L1\n");
        tests_passed++;
        hpplite_l1_disconnect(l1);
    } else {
        printf("  FAIL: connect to L1\n");
    }
}

static void test_get_state(void) {
    tests_run++;

    HppliteL1 *l1 = hpplite_l1_connect(rpc_url, contract);
    if (!l1) {
        printf("  FAIL: get_state - couldn't connect\n");
        return;
    }
    
    HppliteL1State *state = hpplite_l1_get_state(l1);
    if (state) {
        printf("  PASS: get_state\n");
        printf("    Sequencer: 0x");
        print_hex(state->sequencerAddress, 20);
        printf("\n");
        printf("    Witnesses: %d\n", state->nWitnesses);
        printf("    Required attestations: %d\n", state->requiredAttestations);
        printf("    Checkpoint interval: %llu\n", (unsigned long long)state->checkpointInterval);
        printf("    Sequencer timeout: %llu\n", (unsigned long long)state->sequencerTimeout);
        printf("    Last checkpoint height: %llu\n", (unsigned long long)state->lastCheckpointHeight);
        tests_passed++;
        hpplite_l1_state_free(state);
    } else {
        printf("  FAIL: get_state\n");
    }
    
    hpplite_l1_disconnect(l1);
}

static void test_sequencer_timeout(void) {
    tests_run++;

    HppliteL1 *l1 = hpplite_l1_connect(rpc_url, contract);
    if (!l1) {
        printf("  FAIL: sequencer_timeout - couldn't connect\n");
        return;
    }

    int expired = hpplite_l1_sequencer_timeout_expired(l1);
    printf("  PASS: sequencer_timeout_expired = %d\n", expired);
    tests_passed++;

    hpplite_l1_disconnect(l1);
}

int main(void) {
    printf("=== L1 Client Tests ===\n\n");

    /* Load .env configuration */
    test_env_load();

    rpc_url = test_env_get("HPPLITE_L1_RPC");
    contract = test_env_get("HPPLITE_CONTRACT_ADDRESS");

    if (!rpc_url) rpc_url = "https://sepolia.hpp.io";
    if (!contract) contract = "0x2Cd27d02C3b36c8926B849Bc53745Fab661572Dc";

    printf("Config:\n");
    printf("  RPC: %s\n", rpc_url);
    printf("  Contract: %s\n\n", contract);

    test_connect();
    test_get_state();
    test_sequencer_timeout();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
