/*
** RLP (Recursive Length Prefix) encoding for Ethereum transactions
*/

#ifndef RLP_H
#define RLP_H

#include <stdint.h>
#include <stddef.h>

/* RLP encode a byte string */
size_t rlp_encode_bytes(const uint8_t *data, size_t len, uint8_t *out);

/* RLP encode a uint64 (big-endian, no leading zeros) */
size_t rlp_encode_uint64(uint64_t value, uint8_t *out);

/* RLP encode a uint256 (32 bytes, strips leading zeros) */
size_t rlp_encode_uint256(const uint8_t value[32], uint8_t *out);

/* RLP encode a list header (call before encoding list items) */
size_t rlp_encode_list_header(size_t payload_len, uint8_t *out);

/* Calculate length of RLP-encoded bytes */
size_t rlp_encoded_bytes_len(size_t data_len);

/* Calculate length of list header */
size_t rlp_list_header_len(size_t payload_len);

#endif /* RLP_H */
