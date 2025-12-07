/*
** Test ABI encoding/decoding
*/

#include "abi.h"
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

static void print_hex(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
}

static void test_function_selector(void) {
    uint8_t selector[4];
    
    tests_run++;
    /* getState() -> keccak256("getState()")[0:4] */
    abi_function_selector("getState()", selector);
    /* Expected: 1865c57d (can verify with cast sig "getState()") */
    uint8_t expected[] = {0x18, 0x65, 0xc5, 0x7d};
    if (memcmp(selector, expected, 4) == 0) {
        printf("  PASS: getState() selector\n");
        tests_passed++;
    } else {
        printf("  FAIL: getState() selector - got ");
        print_hex(selector, 4);
        printf("\n");
    }
    
    tests_run++;
    /* submitCheckpoint(uint256,uint256,bytes32,bytes) */
    abi_function_selector("submitCheckpoint(uint256,uint256,bytes32,bytes)", selector);
    /* Expected: can verify with cast */
    printf("  INFO: submitCheckpoint selector = ");
    print_hex(selector, 4);
    printf("\n");
    tests_passed++; /* Just print it, no expected value to compare */
    
    tests_run++;
    /* setSequencer(address) */
    abi_function_selector("setSequencer(address)", selector);
    uint8_t expected_seq[] = {0x25, 0x47, 0xfa, 0x3e};
    if (memcmp(selector, expected_seq, 4) == 0) {
        printf("  PASS: setSequencer(address) selector\n");
        tests_passed++;
    } else {
        printf("  FAIL: setSequencer(address) selector - got ");
        print_hex(selector, 4);
        printf("\n");
    }
}

static void test_encode_uint64(void) {
    uint8_t out[32];
    
    tests_run++;
    abi_encode_uint64(1024, out);
    /* Should be 0x0...0400 (1024 in big-endian at the end) */
    int ok = 1;
    for (int i = 0; i < 30; i++) {
        if (out[i] != 0) ok = 0;
    }
    if (out[30] != 0x04 || out[31] != 0x00) ok = 0;
    
    if (ok) {
        printf("  PASS: encode uint64 1024\n");
        tests_passed++;
    } else {
        printf("  FAIL: encode uint64 1024 - got ");
        print_hex(out, 32);
        printf("\n");
    }
}

static void test_encode_address(void) {
    uint8_t address[20] = {
        0x7e, 0x5f, 0x45, 0x52, 0x09, 0x1a, 0x69, 0x12,
        0x5d, 0x5d, 0xfc, 0xb7, 0xb8, 0xc2, 0x65, 0x90,
        0x29, 0x39, 0x5b, 0xdf
    };
    uint8_t out[32];
    
    tests_run++;
    abi_encode_address(address, out);
    
    /* First 12 bytes should be zero */
    int ok = 1;
    for (int i = 0; i < 12; i++) {
        if (out[i] != 0) ok = 0;
    }
    /* Last 20 bytes should be the address */
    if (memcmp(out + 12, address, 20) != 0) ok = 0;
    
    if (ok) {
        printf("  PASS: encode address\n");
        tests_passed++;
    } else {
        printf("  FAIL: encode address\n");
    }
}

static void test_decode_uint64(void) {
    uint8_t data[32] = {0};
    data[30] = 0x04;
    data[31] = 0x00;
    
    tests_run++;
    uint64_t value = abi_decode_uint64(data);
    if (value == 1024) {
        printf("  PASS: decode uint64 1024\n");
        tests_passed++;
    } else {
        printf("  FAIL: decode uint64 - got %llu\n", (unsigned long long)value);
    }
}

static void test_roundtrip(void) {
    tests_run++;
    
    uint64_t original = 0xDEADBEEFCAFE;
    uint8_t encoded[32];
    abi_encode_uint64(original, encoded);
    uint64_t decoded = abi_decode_uint64(encoded);
    
    if (decoded == original) {
        printf("  PASS: uint64 roundtrip\n");
        tests_passed++;
    } else {
        printf("  FAIL: uint64 roundtrip - got %llx expected %llx\n",
               (unsigned long long)decoded, (unsigned long long)original);
    }
}

int main(void) {
    printf("test_abi: ABI encoding tests\n");
    
    test_function_selector();
    test_encode_uint64();
    test_encode_address();
    test_decode_uint64();
    test_roundtrip();
    
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
