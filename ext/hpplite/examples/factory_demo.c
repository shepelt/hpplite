/*
** HPPLite Example: Factory Demo
**
** Demonstrates the factory pattern for auto-deploy:
** - Private key determines your rollup (1:1 wallet:rollup)
** - Factory auto-discovers or creates your rollup
** - No need to deploy contracts manually
**
** Each run generates a fresh private key and creates a new rollup.
**
** Requires: cast (from foundry), jq
**
** Usage:
**   ./factory_demo
**
** Build:
**   cd ../build && make factory_demo
*/

#include "hpplite.h"
#include "l1_interface.h"
#include "test_util.h"
#include <stdio.h>

static void print_hex(const unsigned char *data, int len) {
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
}

int main(void) {
    printf("HPPLite Factory Demo\n");
    printf("====================\n\n");

    /* Check dependencies */
    if (!test_util_has_cast()) {
        printf("ERROR: 'cast' not found. Install foundry: https://getfoundry.sh\n");
        return 1;
    }

    /* Create funded wallet for this run */
    printf("Creating fresh funded wallet...\n");
    char privkey_hex[128], address[64];
    if (test_util_create_funded_wallet(privkey_hex, sizeof(privkey_hex),
                                        address, sizeof(address),
                                        HPPLITE_DEFAULT_RPC, "0.01ether") != 0) {
        return 1;
    }
    printf("  Address: %s\n\n", address);

    /* Parse private key to bytes */
    unsigned char privkey[32];
    test_util_hex_to_bytes(privkey_hex, privkey, 32);

    /* Check if we already have a rollup (we won't, it's a fresh key) */
    printf("Checking factory for existing rollup...\n");
    unsigned char rollup_address[20];
    int result = hpplite_l1_factory_get_my_rollup(
        HPPLITE_DEFAULT_RPC, HPPLITE_DEFAULT_FACTORY, privkey, rollup_address);

    if (result == 1) {
        printf("Found your rollup: 0x");
        print_hex(rollup_address, 20);
        printf("\n\n");
    } else if (result == 0) {
        printf("No rollup found. Creating via factory...\n\n");

        unsigned char new_rollup[20];
        char *tx_hash = hpplite_l1_factory_get_or_create_rollup(
            HPPLITE_DEFAULT_RPC, HPPLITE_DEFAULT_FACTORY, privkey, new_rollup);

        if (tx_hash) {
            printf("Transaction: %s\n", tx_hash);
            printf("Waiting for confirmation...\n");

            HppliteL1 *temp_l1 = hpplite_l1_connect(HPPLITE_DEFAULT_RPC, HPPLITE_DEFAULT_FACTORY);
            if (temp_l1) {
                int status = hpplite_l1_wait_for_tx(temp_l1, tx_hash, 60);
                hpplite_l1_disconnect(temp_l1);
                if (status == 1) {
                    printf("Confirmed!\n");
                    hpplite_l1_factory_get_my_rollup(
                        HPPLITE_DEFAULT_RPC, HPPLITE_DEFAULT_FACTORY, privkey, rollup_address);
                    printf("Your rollup: 0x");
                    print_hex(rollup_address, 20);
                    printf("\n\n");
                } else {
                    printf("Transaction failed\n");
                    free(tx_hash);
                    return 1;
                }
            }
            free(tx_hash);
        } else {
            printf("ERROR: Failed to create rollup\n");
            return 1;
        }
    } else {
        printf("ERROR: Failed to query factory\n");
        return 1;
    }

    /* Connect to your rollup */
    printf("Connecting to rollup...\n");
    HppliteL1 *l1 = hpplite_l1_connect_factory(HPPLITE_DEFAULT_RPC, HPPLITE_DEFAULT_FACTORY, privkey);
    if (!l1) {
        printf("ERROR: Failed to connect\n");
        return 1;
    }

    /* Get rollup state */
    HppliteL1State *state = hpplite_l1_get_state(l1);
    if (state) {
        printf("\nRollup State:\n");
        printf("  Sequencer: 0x");
        print_hex(state->sequencerAddress, 20);
        printf("\n");
        printf("  Witnesses: %d\n", state->nWitnesses);
        printf("  Required attestations: %d\n", state->requiredAttestations);
        hpplite_l1_state_free(state);
    }

    /* Get DA state */
    uint64_t last_batch, total_batches;
    unsigned char latest_hash[32];
    if (hpplite_l1_get_da_state(l1, &last_batch, &total_batches, latest_hash) == 0) {
        printf("\nDA State:\n");
        printf("  Last batch: %llu\n", (unsigned long long)last_batch);
        printf("  Total batches: %llu\n", (unsigned long long)total_batches);
    }

    hpplite_l1_disconnect(l1);

    printf("\nDone! Use your rollup with:\n");
    printf("  sqlite3_open_v2(\"file:db?hpplite=on&factory=%s&privkey=...\", ...);\n",
           HPPLITE_DEFAULT_FACTORY);

    return 0;
}
