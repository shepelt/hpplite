/*
** Test RLP encoding
*/

#include "rlp.h"
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

static void print_hex(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
}

static void test_single_byte(void) {
    uint8_t out[10];
    size_t len;
    
    tests_run++;
    
    /* Single byte 0x00 should stay as 0x00 */
    uint8_t zero = 0x00;
    len = rlp_encode_bytes(&zero, 1, out);
    if (len == 1 && out[0] == 0x00) {
        printf("  PASS: single byte 0x00\n");
        tests_passed++;
    } else {
        printf("  FAIL: single byte 0x00\n");
    }
    
    tests_run++;
    /* Single byte 0x7f should stay as 0x7f */
    uint8_t x7f = 0x7f;
    len = rlp_encode_bytes(&x7f, 1, out);
    if (len == 1 && out[0] == 0x7f) {
        printf("  PASS: single byte 0x7f\n");
        tests_passed++;
    } else {
        printf("  FAIL: single byte 0x7f\n");
    }
    
    tests_run++;
    /* Single byte 0x80 should become [0x81, 0x80] */
    uint8_t x80 = 0x80;
    len = rlp_encode_bytes(&x80, 1, out);
    if (len == 2 && out[0] == 0x81 && out[1] == 0x80) {
        printf("  PASS: single byte 0x80\n");
        tests_passed++;
    } else {
        printf("  FAIL: single byte 0x80 - got ");
        print_hex(out, len);
        printf("\n");
    }
}

static void test_short_string(void) {
    uint8_t out[100];
    size_t len;
    
    tests_run++;
    
    /* "dog" = [0x83, 'd', 'o', 'g'] */
    len = rlp_encode_bytes((const uint8_t *)"dog", 3, out);
    if (len == 4 && out[0] == 0x83 && memcmp(out + 1, "dog", 3) == 0) {
        printf("  PASS: short string 'dog'\n");
        tests_passed++;
    } else {
        printf("  FAIL: short string 'dog'\n");
    }
}

static void test_empty_string(void) {
    uint8_t out[10];
    size_t len;
    
    tests_run++;
    
    /* Empty string = [0x80] */
    len = rlp_encode_bytes(NULL, 0, out);
    if (len == 1 && out[0] == 0x80) {
        printf("  PASS: empty string\n");
        tests_passed++;
    } else {
        printf("  FAIL: empty string\n");
    }
}

static void test_uint64(void) {
    uint8_t out[10];
    size_t len;
    
    tests_run++;
    /* 0 = [0x80] */
    len = rlp_encode_uint64(0, out);
    if (len == 1 && out[0] == 0x80) {
        printf("  PASS: uint64 0\n");
        tests_passed++;
    } else {
        printf("  FAIL: uint64 0\n");
    }
    
    tests_run++;
    /* 127 (0x7f) = [0x7f] */
    len = rlp_encode_uint64(127, out);
    if (len == 1 && out[0] == 0x7f) {
        printf("  PASS: uint64 127\n");
        tests_passed++;
    } else {
        printf("  FAIL: uint64 127\n");
    }
    
    tests_run++;
    /* 128 (0x80) = [0x81, 0x80] */
    len = rlp_encode_uint64(128, out);
    if (len == 2 && out[0] == 0x81 && out[1] == 0x80) {
        printf("  PASS: uint64 128\n");
        tests_passed++;
    } else {
        printf("  FAIL: uint64 128 - got ");
        print_hex(out, len);
        printf("\n");
    }
    
    tests_run++;
    /* 1024 (0x0400) = [0x82, 0x04, 0x00] */
    len = rlp_encode_uint64(1024, out);
    if (len == 3 && out[0] == 0x82 && out[1] == 0x04 && out[2] == 0x00) {
        printf("  PASS: uint64 1024\n");
        tests_passed++;
    } else {
        printf("  FAIL: uint64 1024 - got ");
        print_hex(out, len);
        printf("\n");
    }
}

static void test_list_header(void) {
    uint8_t out[10];
    size_t len;
    
    tests_run++;
    /* Empty list header (payload len 0) = [0xc0] */
    len = rlp_encode_list_header(0, out);
    if (len == 1 && out[0] == 0xc0) {
        printf("  PASS: empty list header\n");
        tests_passed++;
    } else {
        printf("  FAIL: empty list header\n");
    }
    
    tests_run++;
    /* Short list (payload len 10) = [0xca] */
    len = rlp_encode_list_header(10, out);
    if (len == 1 && out[0] == 0xca) {
        printf("  PASS: short list header (len 10)\n");
        tests_passed++;
    } else {
        printf("  FAIL: short list header (len 10)\n");
    }
    
    tests_run++;
    /* Long list (payload len 56) = [0xf8, 0x38] */
    len = rlp_encode_list_header(56, out);
    if (len == 2 && out[0] == 0xf8 && out[1] == 0x38) {
        printf("  PASS: long list header (len 56)\n");
        tests_passed++;
    } else {
        printf("  FAIL: long list header (len 56) - got ");
        print_hex(out, len);
        printf("\n");
    }
}

int main(void) {
    printf("test_rlp: RLP encoding tests\n");
    
    test_single_byte();
    test_short_string();
    test_empty_string();
    test_uint64();
    test_list_header();
    
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
