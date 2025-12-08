/*
** Test L1 Factory Integration
**
** Tests factory pattern for auto-deploy rollups:
** - Query factory for existing rollup
** - Create new rollup via factory
** - Connect using factory helper
**
** Requires .env file with:
**   HPPLITE_L1_RPC=https://sepolia.hpp.io
**   HPPLITE_FACTORY_ADDRESS=0x...
**   HPPLITE_PRIVATE_KEY=0x...
**
** Run manually (not in ctest):
**   ./test_factory
*/

#include "l1_interface.h"
#include "eth_client.h"
#include "test_env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

static const char *rpc_url = NULL;
static const char *factory_address = NULL;
static unsigned char privkey[32];
static int has_privkey = 0;

static void print_hex(const unsigned char *data, int len) {
    for (int i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
}

static int is_zero_address(const unsigned char *addr) {
    for (int i = 0; i < 20; i++) {
        if (addr[i] != 0) return 0;
    }
    return 1;
}

static void test_derive_address(void) {
    tests_run++;
    printf("Test: Derive address from private key...\n");

    if (!has_privkey) {
        printf("  SKIP: No private key configured\n");
        return;
    }

    unsigned char address[20];
    int rc = eth_address_from_privkey(privkey, address);

    if (rc == 0) {
        printf("  PASS: Derived address: 0x");
        print_hex(address, 20);
        printf("\n");
        tests_passed++;
    } else {
        printf("  FAIL: Could not derive address\n");
    }
}

static void test_factory_get_rollup(void) {
    tests_run++;
    printf("Test: Query factory for rollup...\n");

    if (!has_privkey) {
        printf("  SKIP: No private key configured\n");
        return;
    }

    unsigned char rollup[20];
    int result = hpplite_l1_factory_get_my_rollup(
        rpc_url, factory_address, privkey, rollup
    );

    if (result == 1) {
        printf("  PASS: Found rollup at 0x");
        print_hex(rollup, 20);
        printf("\n");
        tests_passed++;
    } else if (result == 0) {
        printf("  PASS: No rollup exists for this wallet (expected for new keys)\n");
        tests_passed++;
    } else {
        printf("  FAIL: Error querying factory (result=%d)\n", result);
    }
}

static void test_factory_has_rollup(void) {
    tests_run++;
    printf("Test: Check if wallet has rollup...\n");

    if (!has_privkey) {
        printf("  SKIP: No private key configured\n");
        return;
    }

    unsigned char address[20];
    eth_address_from_privkey(privkey, address);

    int result = hpplite_l1_factory_has_rollup(rpc_url, factory_address, address);

    if (result >= 0) {
        printf("  PASS: has_rollup = %s\n", result ? "true" : "false");
        tests_passed++;
    } else {
        printf("  FAIL: Error checking factory\n");
    }
}

static void test_factory_connect(void) {
    tests_run++;
    printf("Test: Connect via factory...\n");

    if (!has_privkey) {
        printf("  SKIP: No private key configured\n");
        return;
    }

    /* First check if we have a rollup */
    unsigned char rollup[20];
    int has_rollup = hpplite_l1_factory_get_my_rollup(
        rpc_url, factory_address, privkey, rollup
    );

    if (has_rollup != 1) {
        printf("  SKIP: No rollup exists for this wallet\n");
        printf("        (Run test_factory_create to create one first)\n");
        return;
    }

    /* Connect using factory helper */
    HppliteL1 *l1 = hpplite_l1_connect_factory(rpc_url, factory_address, privkey);

    if (l1) {
        printf("  PASS: Connected to rollup via factory\n");

        /* Get state to verify connection works */
        HppliteL1State *state = hpplite_l1_get_state(l1);
        if (state) {
            printf("    Sequencer: 0x");
            print_hex(state->sequencerAddress, 20);
            printf("\n");
            printf("    Witnesses: %d\n", state->nWitnesses);
            printf("    Last checkpoint: %llu\n",
                   (unsigned long long)state->lastCheckpointHeight);
            hpplite_l1_state_free(state);
        }

        hpplite_l1_disconnect(l1);
        tests_passed++;
    } else {
        printf("  FAIL: Could not connect via factory\n");
    }
}

static void test_factory_create_rollup(void) {
    tests_run++;
    printf("Test: Create rollup via factory...\n");

    if (!has_privkey) {
        printf("  SKIP: No private key configured\n");
        return;
    }

    /* Check if rollup already exists */
    unsigned char rollup[20];
    int has_rollup = hpplite_l1_factory_get_my_rollup(
        rpc_url, factory_address, privkey, rollup
    );

    if (has_rollup == 1) {
        printf("  SKIP: Rollup already exists at 0x");
        print_hex(rollup, 20);
        printf("\n");
        tests_passed++;  /* This is still a pass - rollup exists */
        return;
    }

    /* Create rollup */
    printf("  Creating new rollup...\n");
    unsigned char new_rollup[20];
    char *tx_hash = hpplite_l1_factory_get_or_create_rollup(
        rpc_url, factory_address, privkey, new_rollup
    );

    if (!tx_hash) {
        printf("  FAIL: Could not submit transaction\n");
        return;
    }

    printf("    Transaction: %s\n", tx_hash);
    printf("    Waiting for confirmation (up to 60s)...\n");

    /* Create temp L1 to wait for tx */
    HppliteL1 *temp = hpplite_l1_connect(rpc_url, factory_address);
    if (!temp) {
        printf("  FAIL: Could not connect to wait for tx\n");
        free(tx_hash);
        return;
    }

    int status = hpplite_l1_wait_for_tx(temp, tx_hash, 60);
    hpplite_l1_disconnect(temp);
    free(tx_hash);

    if (status == 1) {
        printf("    Transaction confirmed!\n");

        /* Get the new rollup address */
        has_rollup = hpplite_l1_factory_get_my_rollup(
            rpc_url, factory_address, privkey, rollup
        );

        if (has_rollup == 1) {
            printf("  PASS: Created rollup at 0x");
            print_hex(rollup, 20);
            printf("\n");
            tests_passed++;
        } else {
            printf("  FAIL: Rollup not found after creation\n");
        }
    } else if (status == 0) {
        printf("  FAIL: Transaction reverted\n");
    } else {
        printf("  FAIL: Transaction timeout or error\n");
    }
}

static void test_batch_da_functions(void) {
    tests_run++;
    printf("Test: Batch DA functions...\n");

    if (!has_privkey) {
        printf("  SKIP: No private key configured\n");
        return;
    }

    /* Connect via factory */
    HppliteL1 *l1 = hpplite_l1_connect_factory(rpc_url, factory_address, privkey);
    if (!l1) {
        printf("  SKIP: No rollup exists for this wallet\n");
        return;
    }

    /* Get DA state */
    uint64_t last_height, total;
    unsigned char latest_hash[32];

    int rc = hpplite_l1_get_da_state(l1, &last_height, &total, latest_hash);
    if (rc == 0) {
        printf("  PASS: DA state retrieved\n");
        printf("    Last batch height: %llu\n", (unsigned long long)last_height);
        printf("    Total batches: %llu\n", (unsigned long long)total);

        if (last_height > 0) {
            /* Try to get a batch hash */
            unsigned char hash[32];
            int has_batch = hpplite_l1_get_batch_hash(l1, 1, hash);
            if (has_batch == 1) {
                printf("    Batch 1 hash: ");
                print_hex(hash, 32);
                printf("\n");
            }
        }
    } else {
        printf("  FAIL: Could not get DA state\n");
    }

    hpplite_l1_disconnect(l1);
    if (rc == 0) tests_passed++;
}

int main(int argc, char **argv) {
    printf("=== L1 Factory Integration Tests ===\n\n");

    /* Load .env configuration */
    test_env_load();

    rpc_url = test_env_get("HPPLITE_L1_RPC");
    factory_address = test_env_get("HPPLITE_FACTORY_ADDRESS");

    if (!rpc_url) rpc_url = "https://sepolia.hpp.io";
    if (!factory_address) factory_address = "0x0000000000000000000000000000000000000000";

    /* Load private key */
    if (test_env_get_privkey("HPPLITE_PRIVATE_KEY", privkey) == 0) {
        has_privkey = 1;
    }

    printf("Config:\n");
    printf("  RPC: %s\n", rpc_url);
    printf("  Factory: %s\n", factory_address);
    printf("  Private key: %s\n\n", has_privkey ? "(set)" : "(not set)");

    /* Check for zero factory address */
    unsigned char zero_addr[20] = {0};
    unsigned char factory_bytes[20];
    eth_hex_decode(factory_address, factory_bytes, 20);
    if (memcmp(factory_bytes, zero_addr, 20) == 0) {
        printf("WARNING: Factory address is zero - tests will likely fail.\n");
        printf("         Deploy HPPLiteFactory and set HPPLITE_FACTORY_ADDRESS.\n\n");
    }

    /* Check for --create flag to run create test */
    int run_create = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--create") == 0) {
            run_create = 1;
        }
    }

    /* Run tests */
    test_derive_address();
    test_factory_get_rollup();
    test_factory_has_rollup();

    if (run_create) {
        test_factory_create_rollup();
    } else {
        printf("\nTest: Create rollup via factory...\n");
        printf("  SKIP: Run with --create flag to test rollup creation\n");
    }

    test_factory_connect();
    test_batch_da_functions();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);

    if (!has_privkey) {
        printf("\nNote: Set HPPLITE_PRIVATE_KEY in .env for full test coverage\n");
    }

    return tests_passed == tests_run ? 0 : 1;
}
