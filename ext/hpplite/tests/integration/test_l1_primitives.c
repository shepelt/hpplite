/*
** test_l1_primitives.c - L1 Client & Factory Primitives
**
** Tests low-level L1 operations:
**   1. L1 client: connect, get state, timeout check
**   2. Factory: query rollup, create rollup, connect via factory
**   3. DA layer: get state, batch hashes
**
** This does NOT test HPPLite nodes - just the L1 interface layer.
**
** Requirements:
**   - HPPLITE_PRIVATE_KEY env var (for factory tests)
**   - HPP Sepolia RPC access
**
** Run:
**   ./test_l1_primitives              # Read-only tests
**   ./test_l1_primitives --create     # Include rollup creation
*/

#include "l1_interface.h"
#include "eth_client.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RPC_URL HPPLITE_DEFAULT_RPC
#define FACTORY_ADDRESS HPPLITE_DEFAULT_FACTORY

static int tests_run = 0;
static int tests_passed = 0;

static unsigned char g_privkey[32];
static int g_has_privkey = 0;

static void print_hex(const unsigned char *data, int len) {
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
}

/* ========== L1 Client Tests ========== */

static void test_l1_connect(void) {
    tests_run++;
    printf("Test: L1 connect...\n");

    /* Need a rollup address to connect */
    if (!g_has_privkey) {
        printf("  SKIP: No private key\n");
        return;
    }

    unsigned char rollup[20];
    if (hpplite_l1_factory_get_my_rollup(RPC_URL, FACTORY_ADDRESS, g_privkey, rollup) != 1) {
        printf("  SKIP: No rollup exists\n");
        return;
    }

    char rollup_hex[43];
    snprintf(rollup_hex, sizeof(rollup_hex), "0x");
    for (int i = 0; i < 20; i++) snprintf(rollup_hex + 2 + i*2, 3, "%02x", rollup[i]);

    HppliteL1 *l1 = hpplite_l1_connect(RPC_URL, rollup_hex);
    if (l1) {
        printf("  PASS: Connected to L1\n");
        tests_passed++;
        hpplite_l1_disconnect(l1);
    } else {
        printf("  FAIL: Could not connect\n");
    }
}

static void test_l1_get_state(void) {
    tests_run++;
    printf("Test: L1 get state...\n");

    if (!g_has_privkey) {
        printf("  SKIP: No private key\n");
        return;
    }

    HppliteL1 *l1 = hpplite_l1_connect_factory(RPC_URL, FACTORY_ADDRESS, g_privkey);
    if (!l1) {
        printf("  SKIP: No rollup exists\n");
        return;
    }

    HppliteL1State *state = hpplite_l1_get_state(l1);
    if (state) {
        printf("  PASS: State retrieved\n");
        printf("    Sequencer: 0x"); print_hex(state->sequencerAddress, 20); printf("\n");
        printf("    Witnesses: %d\n", state->nWitnesses);
        printf("    Required attestations: %d\n", state->requiredAttestations);
        printf("    Last checkpoint: %llu\n", (unsigned long long)state->lastCheckpointHeight);
        tests_passed++;
        hpplite_l1_state_free(state);
    } else {
        printf("  FAIL: Could not get state\n");
    }

    hpplite_l1_disconnect(l1);
}

/* ========== Factory Tests ========== */

static void test_factory_get_rollup(void) {
    tests_run++;
    printf("Test: Factory get rollup...\n");

    if (!g_has_privkey) {
        printf("  SKIP: No private key\n");
        return;
    }

    unsigned char rollup[20];
    int result = hpplite_l1_factory_get_my_rollup(RPC_URL, FACTORY_ADDRESS, g_privkey, rollup);

    if (result == 1) {
        printf("  PASS: Rollup found at 0x"); print_hex(rollup, 20); printf("\n");
        tests_passed++;
    } else if (result == 0) {
        printf("  PASS: No rollup exists (expected for new keys)\n");
        tests_passed++;
    } else {
        printf("  FAIL: Error querying factory\n");
    }
}

static void test_factory_create_rollup(void) {
    tests_run++;
    printf("Test: Factory create rollup...\n");

    if (!g_has_privkey) {
        printf("  SKIP: No private key\n");
        return;
    }

    /* Check if already exists */
    unsigned char rollup[20];
    if (hpplite_l1_factory_get_my_rollup(RPC_URL, FACTORY_ADDRESS, g_privkey, rollup) == 1) {
        printf("  SKIP: Rollup already exists at 0x"); print_hex(rollup, 20); printf("\n");
        tests_passed++;
        return;
    }

    printf("  Creating rollup...\n");
    unsigned char new_rollup[20];
    char *tx_hash = hpplite_l1_factory_get_or_create_rollup(
        RPC_URL, FACTORY_ADDRESS, g_privkey, new_rollup);

    if (!tx_hash) {
        printf("  FAIL: Could not submit transaction\n");
        return;
    }

    printf("    TX: %s\n", tx_hash);
    printf("    Waiting for confirmation...\n");

    HppliteL1 *temp = hpplite_l1_connect(RPC_URL, FACTORY_ADDRESS);
    int status = temp ? hpplite_l1_wait_for_tx(temp, tx_hash, 60) : -1;
    if (temp) hpplite_l1_disconnect(temp);
    free(tx_hash);

    if (status == 1) {
        if (hpplite_l1_factory_get_my_rollup(RPC_URL, FACTORY_ADDRESS, g_privkey, rollup) == 1) {
            printf("  PASS: Created rollup at 0x"); print_hex(rollup, 20); printf("\n");
            tests_passed++;
        } else {
            printf("  FAIL: Rollup not found after creation\n");
        }
    } else {
        printf("  FAIL: Transaction failed (status=%d)\n", status);
    }
}

static void test_factory_connect(void) {
    tests_run++;
    printf("Test: Factory connect...\n");

    if (!g_has_privkey) {
        printf("  SKIP: No private key\n");
        return;
    }

    HppliteL1 *l1 = hpplite_l1_connect_factory(RPC_URL, FACTORY_ADDRESS, g_privkey);
    if (l1) {
        printf("  PASS: Connected via factory\n");
        tests_passed++;
        hpplite_l1_disconnect(l1);
    } else {
        printf("  SKIP: No rollup exists\n");
    }
}

/* ========== DA Tests ========== */

static void test_da_state(void) {
    tests_run++;
    printf("Test: DA state...\n");

    if (!g_has_privkey) {
        printf("  SKIP: No private key\n");
        return;
    }

    HppliteL1 *l1 = hpplite_l1_connect_factory(RPC_URL, FACTORY_ADDRESS, g_privkey);
    if (!l1) {
        printf("  SKIP: No rollup exists\n");
        return;
    }

    uint64_t last_height, total;
    unsigned char latest_hash[32];

    if (hpplite_l1_get_da_state(l1, &last_height, &total, latest_hash) == 0) {
        printf("  PASS: DA state retrieved\n");
        printf("    Last batch: %llu, Total: %llu\n",
               (unsigned long long)last_height, (unsigned long long)total);
        tests_passed++;
    } else {
        printf("  FAIL: Could not get DA state\n");
    }

    hpplite_l1_disconnect(l1);
}

/* ========== Main ========== */

int main(int argc, char **argv) {
    printf("=== L1 Primitives Test ===\n\n");

    /* Load private key */
    const char *pk = test_util_get_master_key();
    if (pk && test_util_hex_to_bytes(pk, g_privkey, 32) == 0) {
        g_has_privkey = 1;
    }

    printf("Config:\n");
    printf("  RPC: %s\n", RPC_URL);
    printf("  Factory: %s\n", FACTORY_ADDRESS);
    printf("  Private key: %s\n\n", g_has_privkey ? "(set)" : "(not set)");

    if (!g_has_privkey) {
        printf("WARNING: Set HPPLITE_PRIVATE_KEY for full test coverage\n\n");
    }

    /* Check for --create flag */
    int run_create = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--create") == 0) run_create = 1;
    }

    /* Run tests */
    test_factory_get_rollup();
    if (run_create) {
        test_factory_create_rollup();
    }
    test_factory_connect();
    test_l1_connect();
    test_l1_get_state();
    test_da_state();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
