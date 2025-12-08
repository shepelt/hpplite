/*
** Test Environment Helper
**
** Loads configuration from .env file for integration tests.
** Place your .env file in the tests/ directory (gitignored).
**
** Usage:
**   #include "test_env.h"
**
**   int main() {
**       test_env_load();  // Load .env from tests/ directory
**
**       const char *privkey = test_env_get("HPPLITE_PRIVATE_KEY");
**       const char *rpc = test_env_get("HPPLITE_L1_RPC");
**       ...
**   }
*/

#ifndef TEST_ENV_H
#define TEST_ENV_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Load .env file and set environment variables */
static int test_env_load(void) {
    /* Try multiple paths */
    const char *paths[] = {
        ".env",
        "tests/.env",
        "../tests/.env",
        "../../tests/.env",
        NULL
    };

    FILE *f = NULL;
    for (int i = 0; paths[i]; i++) {
        f = fopen(paths[i], "r");
        if (f) break;
    }

    if (!f) {
        fprintf(stderr, "Warning: .env file not found. Using environment variables.\n");
        return -1;
    }

    char line[1024];
    int count = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Skip comments and empty lines */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        /* Remove trailing newline */
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';

        /* Find = */
        char *eq = strchr(p, '=');
        if (!eq) continue;

        /* Split key=value */
        *eq = '\0';
        char *key = p;
        char *value = eq + 1;

        /* Trim key */
        char *end = eq - 1;
        while (end > key && (*end == ' ' || *end == '\t')) *end-- = '\0';

        /* Skip leading whitespace/quotes on value */
        while (*value == ' ' || *value == '\t' || *value == '"' || *value == '\'') value++;

        /* Remove trailing quotes */
        size_t vlen = strlen(value);
        if (vlen > 0 && (value[vlen-1] == '"' || value[vlen-1] == '\'')) {
            value[vlen-1] = '\0';
        }

        /* Only set if not already set (env takes precedence) */
        if (!getenv(key)) {
            setenv(key, value, 0);
            count++;
        }
    }

    fclose(f);
    return count;
}

/* Get environment variable (after loading .env) */
static const char *test_env_get(const char *key) {
    return getenv(key);
}

/* Get private key as bytes (32 bytes) */
static int test_env_get_privkey(const char *key, unsigned char *out) {
    const char *hex = getenv(key);
    if (!hex) return -1;

    /* Skip 0x prefix */
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
    }

    /* Parse 64 hex chars -> 32 bytes */
    if (strlen(hex) < 64) return -1;

    for (int i = 0; i < 32; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        out[i] = (unsigned char)byte;
    }

    return 0;
}

/* Check if running in integration test mode (has real keys) */
static int test_env_is_integration(void) {
    const char *key = getenv("HPPLITE_PRIVATE_KEY");
    if (!key) return 0;

    /* Check it's not the placeholder */
    if (strstr(key, "0000000000000000000000000000000000000000")) return 0;

    return 1;
}

/* Print loaded config (for debugging) */
static void test_env_print(void) {
    printf("Test Environment:\n");
    printf("  HPPLITE_PRIVATE_KEY: %s\n",
           getenv("HPPLITE_PRIVATE_KEY") ? "(set)" : "(not set)");
    printf("  HPPLITE_L1_RPC: %s\n",
           getenv("HPPLITE_L1_RPC") ? getenv("HPPLITE_L1_RPC") : "(not set)");
    printf("  HPPLITE_CONTRACT_ADDRESS: %s\n",
           getenv("HPPLITE_CONTRACT_ADDRESS") ? getenv("HPPLITE_CONTRACT_ADDRESS") : "(not set)");
    printf("  Integration mode: %s\n", test_env_is_integration() ? "yes" : "no");
}

#endif /* TEST_ENV_H */
