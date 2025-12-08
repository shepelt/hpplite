/*
** HPPLite Example: DA URI Parsing
**
** Demonstrates:
** - Parsing various DA URI formats
** - Network alias resolution
** - Building URIs from components
**
** Build:
**   gcc -I.. -I../../build -o da_uri_demo da_uri_demo.c \
**       ../build/libhpplite.a ../build/libsqlite3.a \
**       -L/opt/homebrew/lib -lsecp256k1 -lcurl -lzmq
*/

#include "da_uri.h"
#include <stdio.h>
#include <stdlib.h>

static void print_hex(const unsigned char *data, int len) {
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
}

static void parse_and_print(const char *uri) {
    printf("URI: %s\n", uri);

    HppliteDAUri *parsed = hpplite_da_uri_parse(uri);
    if (!parsed) {
        printf("  ERROR: Failed to parse\n\n");
        return;
    }

    printf("  Scheme:   %s\n", hpplite_da_scheme_str(parsed->scheme));

    switch (parsed->scheme) {
        case HPPLITE_DA_HPPDA:
            printf("  Chain ID: %llu\n", (unsigned long long)parsed->chainId);
            printf("  Contract: 0x");
            print_hex(parsed->contract, 20);
            printf("\n");
            if (parsed->heightFrom == parsed->heightTo) {
                printf("  Height:   %llu\n", (unsigned long long)parsed->heightFrom);
            } else {
                printf("  Range:    %llu-%llu\n",
                       (unsigned long long)parsed->heightFrom,
                       (unsigned long long)parsed->heightTo);
            }

            /* Show RPC URL for this chain */
            const char *rpc = hpplite_da_get_rpc_url(parsed->chainId);
            if (rpc) {
                printf("  RPC URL:  %s\n", rpc);
            }
            break;

        case HPPLITE_DA_FILE:
        case HPPLITE_DA_IPFS:
        case HPPLITE_DA_HTTP:
            printf("  Path:     %s\n", parsed->path);
            break;

        default:
            break;
    }

    printf("\n");
    hpplite_da_uri_free(parsed);
}

int main(void) {
    printf("HPPLite DA URI Demo\n");
    printf("===================\n\n");

    /* Parse various URI formats */
    printf("=== Parsing URIs ===\n\n");

    /* hppda:// with chain ID */
    parse_and_print("hppda://181228/0x2Cd27d02C3b36c8926B849Bc53745Fab661572Dc/42");

    /* hppda:// with network alias */
    parse_and_print("hppda://hpp-sepolia/0x2Cd27d02C3b36c8926B849Bc53745Fab661572Dc/100");

    /* hppda:// with height range */
    parse_and_print("hppda://181228/0x2Cd27d02C3b36c8926B849Bc53745Fab661572Dc/1-50");

    /* file:// URI */
    parse_and_print("file:///var/hpplite/batches/00000001.json");

    /* ipfs:// URI */
    parse_and_print("ipfs://QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG");

    /* Building URIs */
    printf("=== Building URIs ===\n\n");

    unsigned char contract[20] = {
        0x2c, 0xd2, 0x7d, 0x02, 0xc3, 0xb3, 0x6c, 0x89, 0x26, 0xb8,
        0x49, 0xbc, 0x53, 0x74, 0x5f, 0xab, 0x66, 0x15, 0x72, 0xdc
    };

    char *uri1 = hpplite_da_uri_build(HPPLITE_DA_HPPDA, 181228, contract, 1, 1);
    printf("Single batch: %s\n", uri1);
    free(uri1);

    char *uri2 = hpplite_da_uri_build(HPPLITE_DA_HPPDA, 181228, contract, 100, 200);
    printf("Batch range:  %s\n", uri2);
    free(uri2);

    /* Network aliases */
    printf("\n=== Network Aliases ===\n\n");

    printf("Built-in networks:\n");
    for (int i = 0; i < HPPLITE_NETWORKS_COUNT; i++) {
        printf("  %-12s -> chain %llu (%s)\n",
               HPPLITE_NETWORKS[i].alias,
               (unsigned long long)HPPLITE_NETWORKS[i].chainId,
               HPPLITE_NETWORKS[i].rpcUrl);
    }

    printf("\nDone!\n");
    return 0;
}
