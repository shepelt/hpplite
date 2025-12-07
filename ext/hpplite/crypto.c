/*
** HPPLite Crypto - secp256k1 signing implementation
*/

#include "crypto.h"
#include "sqlite3.h"
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <string.h>
#include <stdlib.h>

/*
** Crypto context structure
*/
struct HppliteCrypto {
    secp256k1_context *ctx;
};

/*
** Keccak256 implementation (simplified - Ethereum's hash)
** In production, use a proper library like tiny_keccak
*/

/* Keccak constants */
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

static const int keccak_rotation[24] = {
    1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
    27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44
};

static const int keccak_pi[24] = {
    10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
    15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1
};

#define ROTL64(x, n) (((x) << (n)) | ((x) >> (64 - (n))))

static void keccak_f1600(uint64_t state[25]) {
    uint64_t t, bc[5];
    int round, i, j;

    for (round = 0; round < 24; round++) {
        /* Theta */
        for (i = 0; i < 5; i++)
            bc[i] = state[i] ^ state[i + 5] ^ state[i + 10] ^ state[i + 15] ^ state[i + 20];
        for (i = 0; i < 5; i++) {
            t = bc[(i + 4) % 5] ^ ROTL64(bc[(i + 1) % 5], 1);
            for (j = 0; j < 25; j += 5)
                state[j + i] ^= t;
        }

        /* Rho and Pi */
        t = state[1];
        for (i = 0; i < 24; i++) {
            j = keccak_pi[i];
            bc[0] = state[j];
            state[j] = ROTL64(t, keccak_rotation[i]);
            t = bc[0];
        }

        /* Chi */
        for (j = 0; j < 25; j += 5) {
            for (i = 0; i < 5; i++)
                bc[i] = state[j + i];
            for (i = 0; i < 5; i++)
                state[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
        }

        /* Iota */
        state[0] ^= keccak_round_constants[round];
    }
}

void hpplite_keccak256(const unsigned char *data, size_t len, unsigned char *hash_out) {
    uint64_t state[25] = {0};
    size_t rate = 136;  /* (1600 - 256*2) / 8 */
    size_t i;
    unsigned char *state_bytes = (unsigned char *)state;

    /* Absorb */
    while (len >= rate) {
        for (i = 0; i < rate; i++)
            state_bytes[i] ^= data[i];
        keccak_f1600(state);
        data += rate;
        len -= rate;
    }

    /* Pad and absorb final block */
    for (i = 0; i < len; i++)
        state_bytes[i] ^= data[i];
    state_bytes[len] ^= 0x01;
    state_bytes[rate - 1] ^= 0x80;
    keccak_f1600(state);

    /* Squeeze */
    memcpy(hash_out, state, 32);
}

/*
** Initialize crypto context
*/
HppliteCrypto *hpplite_crypto_init(void) {
    HppliteCrypto *crypto = sqlite3_malloc(sizeof(HppliteCrypto));
    if (!crypto) return NULL;

    crypto->ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY
    );
    if (!crypto->ctx) {
        sqlite3_free(crypto);
        return NULL;
    }

    return crypto;
}

/*
** Free crypto context
*/
void hpplite_crypto_free(HppliteCrypto *crypto) {
    if (crypto) {
        if (crypto->ctx) {
            secp256k1_context_destroy(crypto->ctx);
        }
        sqlite3_free(crypto);
    }
}

/*
** Generate random keypair
*/
int hpplite_crypto_generate_keypair(HppliteCrypto *crypto, HppliteKeypair *kp) {
    if (!crypto || !kp) return -1;

    /* Generate random private key */
    sqlite3_randomness(HPPLITE_PRIVKEY_SIZE, kp->privkey);

    /* Derive public key and address */
    return hpplite_crypto_keypair_from_privkey(crypto, kp, kp->privkey);
}

/*
** Initialize keypair from private key
*/
int hpplite_crypto_keypair_from_privkey(
    HppliteCrypto *crypto,
    HppliteKeypair *kp,
    const unsigned char *privkey
) {
    secp256k1_pubkey pubkey;
    unsigned char pubkey_uncompressed[65];
    size_t pubkey_len;
    unsigned char hash[32];

    if (!crypto || !kp || !privkey) return -1;

    /* Copy private key */
    memcpy(kp->privkey, privkey, HPPLITE_PRIVKEY_SIZE);

    /* Verify private key is valid */
    if (!secp256k1_ec_seckey_verify(crypto->ctx, privkey)) {
        return -1;
    }

    /* Create public key */
    if (!secp256k1_ec_pubkey_create(crypto->ctx, &pubkey, privkey)) {
        return -1;
    }

    /* Serialize compressed public key */
    pubkey_len = HPPLITE_PUBKEY_SIZE;
    if (!secp256k1_ec_pubkey_serialize(crypto->ctx, kp->pubkey, &pubkey_len,
                                        &pubkey, SECP256K1_EC_COMPRESSED)) {
        return -1;
    }

    /* Serialize uncompressed for address derivation */
    pubkey_len = 65;
    if (!secp256k1_ec_pubkey_serialize(crypto->ctx, pubkey_uncompressed, &pubkey_len,
                                        &pubkey, SECP256K1_EC_UNCOMPRESSED)) {
        return -1;
    }

    /* Derive Ethereum address: keccak256(pubkey[1:65])[12:32] */
    hpplite_keccak256(pubkey_uncompressed + 1, 64, hash);
    memcpy(kp->address, hash + 12, HPPLITE_ADDRESS_SIZE);

    return 0;
}

/*
** Sign a message hash
*/
int hpplite_crypto_sign(
    HppliteCrypto *crypto,
    const HppliteKeypair *kp,
    const unsigned char *msg_hash,
    HppliteSignature *sig_out
) {
    secp256k1_ecdsa_recoverable_signature sig;
    int recid;

    if (!crypto || !kp || !msg_hash || !sig_out) return -1;

    /* Create recoverable signature */
    if (!secp256k1_ecdsa_sign_recoverable(crypto->ctx, &sig, msg_hash,
                                           kp->privkey, NULL, NULL)) {
        return -1;
    }

    /* Serialize to compact format */
    if (!secp256k1_ecdsa_recoverable_signature_serialize_compact(
            crypto->ctx, sig_out->sig, &recid, &sig)) {
        return -1;
    }

    sig_out->recid = recid;
    return 0;
}

/*
** Verify a signature
*/
int hpplite_crypto_verify(
    HppliteCrypto *crypto,
    const unsigned char *pubkey_bytes,
    const unsigned char *msg_hash,
    const HppliteSignature *sig
) {
    secp256k1_pubkey pubkey;
    secp256k1_ecdsa_signature sig_normal;
    secp256k1_ecdsa_recoverable_signature sig_rec;

    if (!crypto || !pubkey_bytes || !msg_hash || !sig) return -1;

    /* Parse public key */
    if (!secp256k1_ec_pubkey_parse(crypto->ctx, &pubkey,
                                    pubkey_bytes, HPPLITE_PUBKEY_SIZE)) {
        return -1;
    }

    /* Parse recoverable signature */
    if (!secp256k1_ecdsa_recoverable_signature_parse_compact(
            crypto->ctx, &sig_rec, sig->sig, sig->recid)) {
        return -1;
    }

    /* Convert to normal signature for verification */
    if (!secp256k1_ecdsa_recoverable_signature_convert(crypto->ctx, &sig_normal, &sig_rec)) {
        return -1;
    }

    /* Verify */
    return secp256k1_ecdsa_verify(crypto->ctx, &sig_normal, msg_hash, &pubkey);
}

/*
** Recover public key from signature
*/
int hpplite_crypto_recover_pubkey(
    HppliteCrypto *crypto,
    const unsigned char *msg_hash,
    const HppliteSignature *sig,
    unsigned char *pubkey_out
) {
    secp256k1_ecdsa_recoverable_signature sig_rec;
    secp256k1_pubkey pubkey;
    size_t pubkey_len = HPPLITE_PUBKEY_SIZE;

    if (!crypto || !msg_hash || !sig || !pubkey_out) return -1;

    /* Parse recoverable signature */
    if (!secp256k1_ecdsa_recoverable_signature_parse_compact(
            crypto->ctx, &sig_rec, sig->sig, sig->recid)) {
        return -1;
    }

    /* Recover public key */
    if (!secp256k1_ecdsa_recover(crypto->ctx, &pubkey, &sig_rec, msg_hash)) {
        return -1;
    }

    /* Serialize */
    if (!secp256k1_ec_pubkey_serialize(crypto->ctx, pubkey_out, &pubkey_len,
                                        &pubkey, SECP256K1_EC_COMPRESSED)) {
        return -1;
    }

    return 0;
}

/*
** Hash a commitment for signing
*/
void hpplite_hash_commitment(
    uint64_t height,
    const unsigned char *state_root,
    const char *batch_ref,
    unsigned char *hash_out
) {
    /* Concatenate: height (8 bytes) || state_root (32 bytes) || batch_ref */
    size_t ref_len = batch_ref ? strlen(batch_ref) : 0;
    size_t total_len = 8 + 32 + ref_len;
    unsigned char *buf = sqlite3_malloc(total_len);

    if (!buf) {
        memset(hash_out, 0, 32);
        return;
    }

    /* Little-endian height */
    memcpy(buf, &height, 8);
    memcpy(buf + 8, state_root, 32);
    if (ref_len > 0) {
        memcpy(buf + 40, batch_ref, ref_len);
    }

    hpplite_keccak256(buf, total_len, hash_out);
    sqlite3_free(buf);
}

/*
** Convert signature to Ethereum format
*/
void hpplite_sig_to_eth_format(
    const HppliteSignature *sig,
    unsigned char *v,
    unsigned char *r,
    unsigned char *s
) {
    if (!sig) return;

    /* r = first 32 bytes */
    if (r) memcpy(r, sig->sig, 32);

    /* s = next 32 bytes */
    if (s) memcpy(s, sig->sig + 32, 32);

    /* v = 27 + recid (pre-EIP155) */
    if (v) *v = 27 + sig->recid;
}

/*
** Bytes to hex string
*/
void hpplite_bytes_to_hex(const unsigned char *bytes, size_t len, char *hex_out) {
    static const char hex_chars[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < len; i++) {
        hex_out[i * 2] = hex_chars[(bytes[i] >> 4) & 0xf];
        hex_out[i * 2 + 1] = hex_chars[bytes[i] & 0xf];
    }
    hex_out[len * 2] = '\0';
}

/*
** Hex string to bytes
*/
int hpplite_hex_to_bytes(const char *hex, unsigned char *bytes_out, size_t max_len) {
    size_t hex_len = strlen(hex);
    size_t i;
    int hi, lo;

    /* Skip 0x prefix */
    if (hex_len >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
        hex_len -= 2;
    }

    if (hex_len % 2 != 0) return -1;
    if (hex_len / 2 > max_len) return -1;

    for (i = 0; i < hex_len / 2; i++) {
        hi = hex[i * 2];
        lo = hex[i * 2 + 1];

        if (hi >= '0' && hi <= '9') hi = hi - '0';
        else if (hi >= 'a' && hi <= 'f') hi = hi - 'a' + 10;
        else if (hi >= 'A' && hi <= 'F') hi = hi - 'A' + 10;
        else return -1;

        if (lo >= '0' && lo <= '9') lo = lo - '0';
        else if (lo >= 'a' && lo <= 'f') lo = lo - 'a' + 10;
        else if (lo >= 'A' && lo <= 'F') lo = lo - 'A' + 10;
        else return -1;

        bytes_out[i] = (hi << 4) | lo;
    }

    return hex_len / 2;
}
