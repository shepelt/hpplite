/*
** Test Ethereum JSON-RPC client
** Run against anvil (local) or HPP Sepolia (testnet)
**
** Usage:
**   ./test_eth_client                    # Uses anvil at localhost:8545
**   ./test_eth_client <rpc_url> <chain_id>
*/

#include "eth_client.h"
#include "abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

/* Test private key (anvil default account 0) */
static const uint8_t TEST_PRIVKEY[32] = {
    0xac, 0x09, 0x74, 0xbe, 0xc3, 0x9a, 0x17, 0xe3,
    0x6b, 0xa4, 0xa6, 0xb4, 0xd2, 0x38, 0xff, 0x94,
    0x4b, 0xac, 0xb4, 0x78, 0xcb, 0xed, 0x5e, 0xfa,
    0xcb, 0x29, 0x71, 0x5e, 0x83, 0x01, 0x24, 0xcb
};

static void print_hex(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
}

static void test_create_client(const char *rpc_url, uint64_t chain_id) {
    tests_run++;
    
    EthClient *client = eth_client_create(rpc_url, chain_id);
    if (client) {
        printf("  PASS: create client\n");
        tests_passed++;
        eth_client_destroy(client);
    } else {
        printf("  FAIL: create client\n");
    }
}

static void test_set_privkey(const char *rpc_url, uint64_t chain_id) {
    tests_run++;
    
    EthClient *client = eth_client_create(rpc_url, chain_id);
    if (!client) {
        printf("  FAIL: set_privkey - couldn't create client\n");
        return;
    }
    
    int rc = eth_client_set_privkey(client, TEST_PRIVKEY);
    if (rc == 0) {
        printf("  PASS: set_privkey\n");
        tests_passed++;
    } else {
        printf("  FAIL: set_privkey - %s\n", eth_client_error(client));
    }
    
    eth_client_destroy(client);
}

static void test_get_gas_price(const char *rpc_url, uint64_t chain_id) {
    tests_run++;
    
    EthClient *client = eth_client_create(rpc_url, chain_id);
    if (!client) {
        printf("  FAIL: get_gas_price - couldn't create client\n");
        return;
    }
    
    uint64_t gas_price = eth_client_gas_price(client);
    if (gas_price != (uint64_t)-1) {
        printf("  PASS: get_gas_price = %llu wei\n", (unsigned long long)gas_price);
        tests_passed++;
    } else {
        printf("  FAIL: get_gas_price - %s\n", eth_client_error(client));
    }
    
    eth_client_destroy(client);
}

static void test_get_nonce(const char *rpc_url, uint64_t chain_id) {
    tests_run++;
    
    EthClient *client = eth_client_create(rpc_url, chain_id);
    if (!client) {
        printf("  FAIL: get_nonce - couldn't create client\n");
        return;
    }
    
    eth_client_set_privkey(client, TEST_PRIVKEY);
    
    /* Use a known address (anvil account 0) */
    uint8_t address[20] = {
        0xf3, 0x9f, 0xd6, 0xe5, 0x1a, 0xad, 0x88, 0xf6,
        0xf4, 0xce, 0x6a, 0xb8, 0x82, 0x79, 0x79, 0x55,
        0x36, 0x15, 0x65, 0xb0
    };
    
    uint64_t nonce = eth_client_get_nonce(client, address);
    if (nonce != (uint64_t)-1) {
        printf("  PASS: get_nonce = %llu\n", (unsigned long long)nonce);
        tests_passed++;
    } else {
        printf("  FAIL: get_nonce - %s\n", eth_client_error(client));
    }
    
    eth_client_destroy(client);
}

static void test_hex_encode_decode(void) {
    tests_run++;
    
    uint8_t data[4] = {0xde, 0xad, 0xbe, 0xef};
    char *hex = eth_hex_encode(data, 4);
    
    if (hex && strcmp(hex, "0xdeadbeef") == 0) {
        printf("  PASS: hex_encode\n");
        tests_passed++;
    } else {
        printf("  FAIL: hex_encode - got %s\n", hex ? hex : "NULL");
    }
    free(hex);
    
    tests_run++;
    uint8_t decoded[4];
    int rc = eth_hex_decode("0xdeadbeef", decoded, 4);
    if (rc == 0 && memcmp(decoded, data, 4) == 0) {
        printf("  PASS: hex_decode\n");
        tests_passed++;
    } else {
        printf("  FAIL: hex_decode\n");
    }
}

int main(int argc, char **argv) {
    const char *rpc_url = "http://localhost:8545";
    uint64_t chain_id = 31337; /* anvil default */
    
    if (argc >= 3) {
        rpc_url = argv[1];
        chain_id = strtoull(argv[2], NULL, 10);
    }
    
    printf("test_eth_client: Ethereum JSON-RPC tests\n");
    printf("  RPC URL:  %s\n", rpc_url);
    printf("  Chain ID: %llu\n\n", (unsigned long long)chain_id);
    
    /* Local tests (no network) */
    test_hex_encode_decode();
    
    /* Network tests */
    test_create_client(rpc_url, chain_id);
    test_set_privkey(rpc_url, chain_id);
    test_get_gas_price(rpc_url, chain_id);
    test_get_nonce(rpc_url, chain_id);
    
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
