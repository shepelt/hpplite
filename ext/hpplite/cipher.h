/*
** HPPLite batch encryption
**
** Everything posted to L1 is public, so batch payloads are sealed before
** they leave the process. The cipher is pluggable (HppliteCipher): the
** envelope names the scheme, so algorithms can be added without touching
** batch, flush or replay code.
**
** v0.1 scheme "chacha20poly1305" (RFC 8439 AEAD, 256-bit key, 96-bit
** nonce, 128-bit tag), vendored, constant-time plain C. The key is derived
** from the wallet private key with HKDF-SHA256, so whoever can write the
** rollup can read it and no extra secret exists.
**
** Envelope (binary, posted as the batch bytes):
**   "HPE1" | scheme id (1) | nonce (12) | ciphertext || tag (16)
** Associated data: scheme id | height (8 bytes big-endian), so a ciphertext
** cannot be moved to another height. A payload that does not start with
** "HPE1" is treated as plaintext JSON (batches written before encryption).
*/

#ifndef HPPLITE_CIPHER_H
#define HPPLITE_CIPHER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HPPLITE_CIPHER_KEY_SIZE 32
#define HPPLITE_ENVELOPE_MAGIC "HPE1"
#define HPPLITE_ENVELOPE_MAGIC_LEN 4

typedef struct HppliteCipher {
    const char *name;
    uint8_t id;             /* scheme id in the envelope */
    size_t nonceLen;
    size_t tagLen;
    /* out must hold ptLen + tagLen; returns 0 on success */
    int (*seal)(const uint8_t key[32], const uint8_t *nonce,
                const uint8_t *ad, size_t adLen,
                const uint8_t *pt, size_t ptLen, uint8_t *out);
    /* out must hold ctLen - tagLen; returns 0 on success, -1 if authentication fails */
    int (*open)(const uint8_t key[32], const uint8_t *nonce,
                const uint8_t *ad, size_t adLen,
                const uint8_t *ct, size_t ctLen, uint8_t *out);
} HppliteCipher;

/* Built-in schemes */
extern const HppliteCipher HPPLITE_CIPHER_CHACHA20POLY1305;

/* Default scheme for new batches */
const HppliteCipher *hpplite_cipher_default(void);

/* Look up a scheme by envelope id or by name; NULL if unknown */
const HppliteCipher *hpplite_cipher_by_id(uint8_t id);
const HppliteCipher *hpplite_cipher_by_name(const char *name);

/*
** Derive the batch key from the wallet private key:
**   HKDF-SHA256(ikm = privkey, salt = "hpplite", info = "hpplite/batch-encryption/v1")
*/
void hpplite_cipher_derive_key(const uint8_t privkey[32], uint8_t key[32]);

/*
** Seal a batch payload into an envelope. Returns a malloc'd buffer (free())
** and its length, or NULL on failure.
*/
uint8_t *hpplite_envelope_seal(const HppliteCipher *cipher, const uint8_t key[32],
                               uint64_t height, const uint8_t *payload, size_t payloadLen,
                               size_t *pOutLen);

/*
** Is this an envelope (vs plaintext JSON)?
*/
int hpplite_envelope_is_sealed(const uint8_t *data, size_t len);

/*
** Open an envelope. Returns a malloc'd, NUL-terminated payload and its
** length, or NULL if the scheme is unknown, the key is wrong (authentication
** failed) or the data is malformed. *pScheme receives the scheme used.
*/
uint8_t *hpplite_envelope_open(const uint8_t key[32], uint64_t height,
                               const uint8_t *data, size_t len, size_t *pOutLen,
                               const HppliteCipher **pScheme);

/* Primitives, exposed for tests */
void hpplite_hmac_sha256(const uint8_t *key, size_t keyLen, const uint8_t *msg, size_t msgLen, uint8_t out[32]);
void hpplite_hkdf_sha256(const uint8_t *salt, size_t saltLen, const uint8_t *ikm, size_t ikmLen,
                         const uint8_t *info, size_t infoLen, uint8_t *okm, size_t okmLen);
int hpplite_random_bytes(uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_CIPHER_H */
