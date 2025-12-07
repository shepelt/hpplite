/*
** Test Keccak-256 implementation
*/

#include "keccak256.h"
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

static void print_hex(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_len) {
    for (size_t i = 0; i < out_len; i++) {
        unsigned int byte;
        if (sscanf(hex + 2 * i, "%02x", &byte) != 1) return -1;
        out[i] = (uint8_t)byte;
    }
    return 0;
}

static void test_vector(const char *name, const char *input, const char *expected_hex) {
    uint8_t output[32];
    uint8_t expected[32];
    
    tests_run++;
    
    hex_to_bytes(expected_hex, expected, 32);
    keccak256((const uint8_t *)input, strlen(input), output);
    
    if (memcmp(output, expected, 32) == 0) {
        printf("  PASS: %s\n", name);
        tests_passed++;
    } else {
        printf("  FAIL: %s\n", name);
        printf("    expected: %s\n", expected_hex);
        printf("    got:      ");
        print_hex(output, 32);
        printf("\n");
    }
}

static void test_empty(void) {
    /* Keccak-256("") = c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470 */
    test_vector("empty string", "", 
        "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
}

static void test_hello(void) {
    /* Keccak-256("hello") - Ethereum uses Keccak, not SHA3 */
    test_vector("hello",
        "hello",
        "1c8aff950685c2ed4bc3174f3472287b56d9517b9c948127319a09a7a36deac8");
}

static void test_ethereum_address(void) {
    /* Test vector: known private key -> public key -> address */
    /* Private key: 0x0000000000000000000000000000000000000000000000000000000000000001 */
    /* Public key (uncompressed, 65 bytes with 04 prefix): */
    uint8_t pubkey[65] = {
        0x04,
        0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac,
        0x55, 0xa0, 0x62, 0x95, 0xce, 0x87, 0x0b, 0x07,
        0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9,
        0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98,
        0x48, 0x3a, 0xda, 0x77, 0x26, 0xa3, 0xc4, 0x65,
        0x5d, 0xa4, 0xfb, 0xfc, 0x0e, 0x11, 0x08, 0xa8,
        0xfd, 0x17, 0xb4, 0x48, 0xa6, 0x85, 0x54, 0x19,
        0x9c, 0x47, 0xd0, 0x8f, 0xfb, 0x10, 0xd4, 0xb8
    };
    /* Expected address: 0x7E5F4552091A69125d5DfCb7b8C2659029395Bdf */
    uint8_t expected_addr[20] = {
        0x7e, 0x5f, 0x45, 0x52, 0x09, 0x1a, 0x69, 0x12,
        0x5d, 0x5d, 0xfc, 0xb7, 0xb8, 0xc2, 0x65, 0x90,
        0x29, 0x39, 0x5b, 0xdf
    };
    uint8_t address[20];
    
    tests_run++;
    eth_address_from_pubkey(pubkey, address);
    
    if (memcmp(address, expected_addr, 20) == 0) {
        printf("  PASS: ethereum address from pubkey\n");
        tests_passed++;
    } else {
        printf("  FAIL: ethereum address from pubkey\n");
        printf("    expected: ");
        print_hex(expected_addr, 20);
        printf("\n    got:      ");
        print_hex(address, 20);
        printf("\n");
    }
}

static void test_long_input(void) {
    /* Test with input longer than rate (136 bytes) */
    char input[200];
    memset(input, 'a', sizeof(input) - 1);
    input[sizeof(input) - 1] = '\0';
    
    uint8_t output[32];
    keccak256((const uint8_t *)input, strlen(input), output);
    
    tests_run++;
    /* Just verify it doesn't crash and produces output */
    int non_zero = 0;
    for (int i = 0; i < 32; i++) {
        if (output[i] != 0) non_zero++;
    }
    if (non_zero > 0) {
        printf("  PASS: long input (199 bytes)\n");
        tests_passed++;
    } else {
        printf("  FAIL: long input produced all zeros\n");
    }
}

int main(void) {
    printf("test_keccak: Keccak-256 tests\n");
    
    test_empty();
    test_hello();
    test_ethereum_address();
    test_long_input();
    
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
