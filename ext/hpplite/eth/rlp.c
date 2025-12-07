/*
** RLP (Recursive Length Prefix) encoding for Ethereum transactions
*/

#include "rlp.h"
#include <string.h>

/* Encode length as big-endian bytes, return number of bytes written */
static size_t encode_length_be(size_t len, uint8_t *out) {
    if (len == 0) return 0;
    
    size_t n = 0;
    size_t temp = len;
    while (temp > 0) {
        n++;
        temp >>= 8;
    }
    
    for (size_t i = 0; i < n; i++) {
        out[n - 1 - i] = (uint8_t)(len & 0xff);
        len >>= 8;
    }
    return n;
}

size_t rlp_encoded_bytes_len(size_t data_len) {
    if (data_len == 1) {
        return 1; /* Could be 1 or 2 depending on value, assume worst case handled elsewhere */
    } else if (data_len <= 55) {
        return 1 + data_len;
    } else {
        size_t len_bytes = 0;
        size_t temp = data_len;
        while (temp > 0) { len_bytes++; temp >>= 8; }
        return 1 + len_bytes + data_len;
    }
}

size_t rlp_list_header_len(size_t payload_len) {
    if (payload_len <= 55) {
        return 1;
    } else {
        size_t len_bytes = 0;
        size_t temp = payload_len;
        while (temp > 0) { len_bytes++; temp >>= 8; }
        return 1 + len_bytes;
    }
}

size_t rlp_encode_bytes(const uint8_t *data, size_t len, uint8_t *out) {
    if (len == 1 && data[0] < 0x80) {
        /* Single byte in [0x00, 0x7f] is its own encoding */
        out[0] = data[0];
        return 1;
    } else if (len <= 55) {
        out[0] = 0x80 + len;
        memcpy(out + 1, data, len);
        return 1 + len;
    } else {
        uint8_t len_buf[8];
        size_t len_bytes = encode_length_be(len, len_buf);
        out[0] = 0xb7 + len_bytes;
        memcpy(out + 1, len_buf, len_bytes);
        memcpy(out + 1 + len_bytes, data, len);
        return 1 + len_bytes + len;
    }
}

size_t rlp_encode_uint64(uint64_t value, uint8_t *out) {
    if (value == 0) {
        out[0] = 0x80; /* Empty byte string */
        return 1;
    } else if (value < 0x80) {
        out[0] = (uint8_t)value;
        return 1;
    } else {
        uint8_t buf[8];
        size_t n = 0;
        uint64_t temp = value;
        while (temp > 0) {
            n++;
            temp >>= 8;
        }
        for (size_t i = 0; i < n; i++) {
            buf[n - 1 - i] = (uint8_t)(value & 0xff);
            value >>= 8;
        }
        return rlp_encode_bytes(buf, n, out);
    }
}

size_t rlp_encode_uint256(const uint8_t value[32], uint8_t *out) {
    /* Find first non-zero byte */
    size_t start = 0;
    while (start < 32 && value[start] == 0) start++;
    
    if (start == 32) {
        /* All zeros - encode as empty string */
        out[0] = 0x80;
        return 1;
    }
    
    return rlp_encode_bytes(value + start, 32 - start, out);
}

size_t rlp_encode_list_header(size_t payload_len, uint8_t *out) {
    if (payload_len <= 55) {
        out[0] = 0xc0 + payload_len;
        return 1;
    } else {
        uint8_t len_buf[8];
        size_t len_bytes = encode_length_be(payload_len, len_buf);
        out[0] = 0xf7 + len_bytes;
        memcpy(out + 1, len_buf, len_bytes);
        return 1 + len_bytes;
    }
}
