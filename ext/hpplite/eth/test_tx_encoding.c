/*
** Signed transactions must be valid RLP: r and s are integers and need the
** minimal big-endian encoding. Encoding them as fixed 32-byte strings made
** every signature whose r or s begins with a zero byte (about 1 in 128) a
** transaction the node rejects with "Failed to decode transaction" - seen
** as sporadic unconfirmed batches on HPP Sepolia and found by the anvil
** harness. This signs enough transactions to hit the case many times and
** walks the RLP to check every integer item.
*/
#include "eth_client.h"
#include <stdio.h>
#include <string.h>

static const uint8_t KEY[32] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20 };

/* Decode one RLP item at p; returns header length, sets *len and *isList. */
static size_t item(const uint8_t *p, size_t *len, int *isList) {
    uint8_t b = p[0];
    *isList = 0;
    if (b < 0x80) { *len = 1; return 0; }
    if (b <= 0xb7) { *len = b - 0x80; return 1; }
    if (b <= 0xbf) { size_t n = b - 0xb7, l = 0; for (size_t i = 0; i < n; i++) l = (l << 8) | p[1 + i]; *len = l; return 1 + n; }
    *isList = 1;
    if (b <= 0xf7) { *len = b - 0xc0; return 1; }
    { size_t n = b - 0xf7, l = 0; for (size_t i = 0; i < n; i++) l = (l << 8) | p[1 + i]; *len = l; return 1 + n; }
}

int main(void) {
    EthClient *c = eth_client_create("http://127.0.0.1:1", 31337);
    uint8_t to[20] = {0xa1,0x6e,0x02,0xe8,0x7b,0x74,0x54,0x12,0x6e,0x5e,0x10,0xd9,0x57,0xa9,0x27,0xa7,0xf5,0xb5,0xd2,0xbe};
    uint8_t data[4] = {0xeb,0xc3,0x82,0x74};
    uint8_t raw[512];
    int bad = 0, shortRS = 0, total = 0;
    if (!c || eth_client_set_privkey(c, KEY) != 0) { printf("client\n"); return 1; }
    for (uint64_t nonce = 0; nonce < 3000; nonce++) {
        size_t n = eth_client_sign_tx(c, nonce, 1000000000 + nonce * 7919, 60000, to, 0, data, sizeof(data), raw, sizeof(raw));
        size_t len, hdr; int isList;
        if (n == 0) { printf("sign failed at nonce %llu\n", (unsigned long long)nonce); return 1; }
        hdr = item(raw, &len, &isList);
        if (!isList || hdr + len != n) { printf("not one RLP list (nonce %llu)\n", (unsigned long long)nonce); return 1; }
        /* walk the 9 items: nonce, gasPrice, gas, to, value, data, v, r, s */
        const uint8_t *p = raw + hdr; const uint8_t *end = raw + n; int idx = 0;
        while (p < end) {
            size_t ilen; int il;
            size_t ih = item(p, &ilen, &il);
            const uint8_t *body = p + ih;
            int isInteger = (idx == 0 || idx == 1 || idx == 2 || idx == 4 || idx == 6 || idx == 7 || idx == 8);
            if (isInteger && ilen > 0 && body[0] == 0x00) { bad++; }
            if ((idx == 7 || idx == 8) && ilen < 32) shortRS++;
            p = body + ilen; idx++;
        }
        if (idx != 9) { printf("expected 9 items, got %d (nonce %llu)\n", idx, (unsigned long long)nonce); return 1; }
        total++;
    }
    eth_client_destroy(c);
    printf("%d transactions signed, %d with a leading-zero integer (must be 0), %d with a short r/s (expected ~%d)\n",
           total, bad, shortRS, total * 2 / 128);
    if (bad) return 1;
    if (shortRS == 0) { printf("no short r/s in %d signatures: the minimal-encoding path was never exercised\n", total); return 1; }
    printf("tx encoding: ok\n");
    return 0;
}
