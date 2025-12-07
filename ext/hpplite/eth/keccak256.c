/*
** Keccak-256 implementation for HPPLite
** Based on the Keccak reference implementation (public domain)
*/

#include "keccak256.h"
#include <string.h>

#define KECCAK_ROUNDS 24
#define ROTL64(x, y) (((x) << (y)) | ((x) >> (64 - (y))))

static const uint64_t keccak_round_constants[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL
};

static const int keccak_rotations[24] = {
    1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
    27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44
};

static const int keccak_piln[24] = {
    10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
    15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1
};

static void keccak_f(uint64_t state[25]) {
    uint64_t t, bc[5];
    
    for (int round = 0; round < KECCAK_ROUNDS; round++) {
        /* Theta */
        for (int i = 0; i < 5; i++) {
            bc[i] = state[i] ^ state[i + 5] ^ state[i + 10] ^ state[i + 15] ^ state[i + 20];
        }
        for (int i = 0; i < 5; i++) {
            t = bc[(i + 4) % 5] ^ ROTL64(bc[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5) {
                state[j + i] ^= t;
            }
        }
        
        /* Rho and Pi */
        t = state[1];
        for (int i = 0; i < 24; i++) {
            int j = keccak_piln[i];
            bc[0] = state[j];
            state[j] = ROTL64(t, keccak_rotations[i]);
            t = bc[0];
        }
        
        /* Chi */
        for (int j = 0; j < 25; j += 5) {
            for (int i = 0; i < 5; i++) {
                bc[i] = state[j + i];
            }
            for (int i = 0; i < 5; i++) {
                state[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
            }
        }
        
        /* Iota */
        state[0] ^= keccak_round_constants[round];
    }
}

void keccak256(const uint8_t *input, size_t len, uint8_t output[32]) {
    uint64_t state[25];
    uint8_t temp[136]; /* rate = 1088 bits = 136 bytes for Keccak-256 */
    size_t rate = 136;
    
    memset(state, 0, sizeof(state));
    
    /* Absorb */
    while (len >= rate) {
        for (size_t i = 0; i < rate / 8; i++) {
            state[i] ^= ((uint64_t *)input)[i];
        }
        keccak_f(state);
        input += rate;
        len -= rate;
    }
    
    /* Pad and absorb final block */
    memset(temp, 0, rate);
    memcpy(temp, input, len);
    temp[len] = 0x01;       /* Keccak padding (not SHA-3!) */
    temp[rate - 1] |= 0x80;
    
    for (size_t i = 0; i < rate / 8; i++) {
        state[i] ^= ((uint64_t *)temp)[i];
    }
    keccak_f(state);
    
    /* Squeeze */
    memcpy(output, state, 32);
}

void eth_address_from_pubkey(const uint8_t pubkey[65], uint8_t address[20]) {
    uint8_t hash[32];
    /* Skip the 0x04 prefix, hash the 64-byte public key */
    keccak256(pubkey + 1, 64, hash);
    /* Take last 20 bytes */
    memcpy(address, hash + 12, 20);
}
