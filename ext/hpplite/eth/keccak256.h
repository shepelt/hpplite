/*
** Keccak-256 implementation for HPPLite
** Based on the Keccak reference implementation (public domain)
*/

#ifndef KECCAK256_H
#define KECCAK256_H

#include <stdint.h>
#include <stddef.h>

/* Compute Keccak-256 hash */
void keccak256(const uint8_t *input, size_t len, uint8_t output[32]);

/* Ethereum address from public key (last 20 bytes of keccak256(pubkey[1:])) */
void eth_address_from_pubkey(const uint8_t pubkey[65], uint8_t address[20]);

#endif /* KECCAK256_H */
