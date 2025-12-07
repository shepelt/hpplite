/*
** ABI encoding/decoding for Ethereum contract calls
*/

#ifndef ABI_H
#define ABI_H

#include <stdint.h>
#include <stddef.h>

/* Function selector (first 4 bytes of keccak256(signature)) */
void abi_function_selector(const char *signature, uint8_t out[4]);

/* Encode uint256 (padded to 32 bytes) */
void abi_encode_uint256(const uint8_t value[32], uint8_t out[32]);

/* Encode uint64 as uint256 */
void abi_encode_uint64(uint64_t value, uint8_t out[32]);

/* Encode address (padded to 32 bytes) */
void abi_encode_address(const uint8_t address[20], uint8_t out[32]);

/* Encode bytes32 */
void abi_encode_bytes32(const uint8_t value[32], uint8_t out[32]);

/* Encode dynamic bytes - returns total length written
   offset_slot: where to write the offset (32 bytes)
   data_slot: where to write length + data
   Returns length of data written to data_slot */
size_t abi_encode_bytes(const uint8_t *data, size_t len, 
                        uint8_t *offset_slot, size_t offset_value,
                        uint8_t *data_slot);

/* Decode uint256 to uint64 (assumes value fits) */
uint64_t abi_decode_uint64(const uint8_t data[32]);

/* Decode address */
void abi_decode_address(const uint8_t data[32], uint8_t address[20]);

/* Decode bytes32 */
void abi_decode_bytes32(const uint8_t data[32], uint8_t value[32]);

#endif /* ABI_H */
