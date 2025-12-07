/*
** HPPLite Crypto - secp256k1 signing for L2 attestations
**
** Uses Bitcoin's secp256k1 library for Ethereum-compatible signatures.
** L1 contracts can verify these signatures using ecrecover().
*/

#ifndef HPPLITE_CRYPTO_H
#define HPPLITE_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HPPLITE_PRIVKEY_SIZE 32
#define HPPLITE_PUBKEY_SIZE 33      /* Compressed */
#define HPPLITE_PUBKEY_UNCOMPRESSED 65
#define HPPLITE_SIGNATURE_SIZE 64   /* Compact format (r, s) */
#define HPPLITE_ADDRESS_SIZE 20     /* Ethereum address */
#define HPPLITE_HASH_SIZE 32

/*
** Crypto context (holds secp256k1 context)
*/
typedef struct HppliteCrypto HppliteCrypto;

/*
** Key pair
*/
typedef struct HppliteKeypair {
    unsigned char privkey[HPPLITE_PRIVKEY_SIZE];
    unsigned char pubkey[HPPLITE_PUBKEY_SIZE];
    unsigned char address[HPPLITE_ADDRESS_SIZE];
} HppliteKeypair;

/*
** Signature with recovery id (for ecrecover)
*/
typedef struct HppliteSignature {
    unsigned char sig[HPPLITE_SIGNATURE_SIZE];  /* r (32) + s (32) */
    int recid;                                   /* Recovery id (0-3) */
} HppliteSignature;

/*
** Initialize crypto context.
** Returns NULL on failure.
*/
HppliteCrypto *hpplite_crypto_init(void);

/*
** Free crypto context.
*/
void hpplite_crypto_free(HppliteCrypto *crypto);

/*
** Generate a new random keypair.
** Returns 0 on success, -1 on failure.
*/
int hpplite_crypto_generate_keypair(HppliteCrypto *crypto, HppliteKeypair *kp);

/*
** Initialize keypair from existing private key.
** Derives public key and address from private key.
** Returns 0 on success, -1 on failure.
*/
int hpplite_crypto_keypair_from_privkey(
    HppliteCrypto *crypto,
    HppliteKeypair *kp,
    const unsigned char *privkey
);

/*
** Sign a 32-byte message hash.
** Produces recoverable signature (can be used with ecrecover).
** Returns 0 on success, -1 on failure.
*/
int hpplite_crypto_sign(
    HppliteCrypto *crypto,
    const HppliteKeypair *kp,
    const unsigned char *msg_hash,  /* 32 bytes */
    HppliteSignature *sig_out
);

/*
** Verify a signature.
** Returns 1 if valid, 0 if invalid, -1 on error.
*/
int hpplite_crypto_verify(
    HppliteCrypto *crypto,
    const unsigned char *pubkey,    /* 33 bytes compressed */
    const unsigned char *msg_hash,  /* 32 bytes */
    const HppliteSignature *sig
);

/*
** Recover public key from signature and message hash.
** Uses recovery id from signature.
** Returns 0 on success, -1 on failure.
*/
int hpplite_crypto_recover_pubkey(
    HppliteCrypto *crypto,
    const unsigned char *msg_hash,  /* 32 bytes */
    const HppliteSignature *sig,
    unsigned char *pubkey_out       /* 33 bytes */
);

/*
** Hash data with Keccak256 (Ethereum's hash function).
** Used for creating message hashes and deriving addresses.
*/
void hpplite_keccak256(
    const unsigned char *data,
    size_t len,
    unsigned char *hash_out         /* 32 bytes */
);

/*
** Hash a commitment for signing.
** hash = keccak256(height || state_root || batch_ref)
*/
void hpplite_hash_commitment(
    uint64_t height,
    const unsigned char *state_root,  /* 32 bytes */
    const char *batch_ref,
    unsigned char *hash_out           /* 32 bytes */
);

/*
** Convert signature to Ethereum format (v, r, s).
** v = 27 + recid (or 28 + recid for EIP-155)
*/
void hpplite_sig_to_eth_format(
    const HppliteSignature *sig,
    unsigned char *v,               /* 1 byte: 27 or 28 */
    unsigned char *r,               /* 32 bytes */
    unsigned char *s                /* 32 bytes */
);

/*
** Utility: Convert bytes to hex string.
** Output buffer must be at least (len * 2 + 1) bytes.
*/
void hpplite_bytes_to_hex(const unsigned char *bytes, size_t len, char *hex_out);

/*
** Utility: Convert hex string to bytes.
** Returns number of bytes written, or -1 on error.
*/
int hpplite_hex_to_bytes(const char *hex, unsigned char *bytes_out, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_CRYPTO_H */
