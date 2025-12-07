/*
** ABI encoding/decoding for Ethereum contract calls
*/

#include "abi.h"
#include "keccak256.h"
#include <string.h>

void abi_function_selector(const char *signature, uint8_t out[4]) {
    uint8_t hash[32];
    keccak256((const uint8_t *)signature, strlen(signature), hash);
    memcpy(out, hash, 4);
}

void abi_encode_uint256(const uint8_t value[32], uint8_t out[32]) {
    memcpy(out, value, 32);
}

void abi_encode_uint64(uint64_t value, uint8_t out[32]) {
    memset(out, 0, 32);
    /* Big-endian in last 8 bytes */
    for (int i = 0; i < 8; i++) {
        out[31 - i] = (uint8_t)(value & 0xff);
        value >>= 8;
    }
}

void abi_encode_address(const uint8_t address[20], uint8_t out[32]) {
    memset(out, 0, 32);
    memcpy(out + 12, address, 20);
}

void abi_encode_bytes32(const uint8_t value[32], uint8_t out[32]) {
    memcpy(out, value, 32);
}

size_t abi_encode_bytes(const uint8_t *data, size_t len,
                        uint8_t *offset_slot, size_t offset_value,
                        uint8_t *data_slot) {
    /* Write offset to offset_slot */
    abi_encode_uint64(offset_value, offset_slot);
    
    /* Write length */
    abi_encode_uint64(len, data_slot);
    
    /* Write data (padded to 32-byte boundary) */
    size_t padded_len = ((len + 31) / 32) * 32;
    memset(data_slot + 32, 0, padded_len);
    memcpy(data_slot + 32, data, len);
    
    return 32 + padded_len;
}

uint64_t abi_decode_uint64(const uint8_t data[32]) {
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
        value = (value << 8) | data[24 + i];
    }
    return value;
}

void abi_decode_address(const uint8_t data[32], uint8_t address[20]) {
    memcpy(address, data + 12, 20);
}

void abi_decode_bytes32(const uint8_t data[32], uint8_t value[32]) {
    memcpy(value, data, 32);
}
