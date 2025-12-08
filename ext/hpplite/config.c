/*
** HPPLite Configuration Implementation
**
** Layered config: Defaults → Env vars → URI params
*/

#include "config.h"
#include "da_uri.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/*
** Helper: duplicate string safely
*/
static char *safe_strdup(const char *s) {
    return s ? strdup(s) : NULL;
}

/*
** Helper: parse hex string to bytes
*/
static int hex_to_bytes(const char *hex, unsigned char *out, int len) {
    if (!hex) return -1;

    /* Skip 0x prefix */
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
    }

    size_t hexLen = strlen(hex);
    if (hexLen != (size_t)(len * 2)) return -1;

    for (int i = 0; i < len; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        out[i] = (unsigned char)byte;
    }

    return 0;
}

/*
** Helper: parse duration string (e.g., "5s", "100ms", "1m")
*/
static int parse_duration_ms(const char *str) {
    if (!str) return -1;

    int value = atoi(str);
    if (value <= 0) return -1;

    /* Find unit suffix */
    const char *p = str;
    while (*p && (isdigit(*p) || *p == '.')) p++;

    if (*p == 's' || *p == 'S') {
        return value * 1000;
    } else if (*p == 'm' && (*(p+1) == 's' || *(p+1) == 'S')) {
        return value;
    } else if (*p == 'm' || *p == 'M') {
        return value * 60 * 1000;
    } else if (*p == '\0') {
        /* No unit = seconds */
        return value * 1000;
    }

    return -1;
}

HppliteConfigRole hpplite_config_parse_role(const char *str) {
    if (!str) return HPPLITE_CFG_ROLE_UNKNOWN;
    if (strcmp(str, "sequencer") == 0) return HPPLITE_CFG_ROLE_SEQUENCER;
    if (strcmp(str, "witness") == 0) return HPPLITE_CFG_ROLE_WITNESS;
    if (strcmp(str, "observer") == 0) return HPPLITE_CFG_ROLE_OBSERVER;
    return HPPLITE_CFG_ROLE_UNKNOWN;
}

const char *hpplite_config_role_str(HppliteConfigRole role) {
    switch (role) {
        case HPPLITE_CFG_ROLE_SEQUENCER: return "sequencer";
        case HPPLITE_CFG_ROLE_WITNESS: return "witness";
        case HPPLITE_CFG_ROLE_OBSERVER: return "observer";
        default: return "unknown";
    }
}

HppliteConfig *hpplite_config_create(void) {
    HppliteConfig *cfg = calloc(1, sizeof(HppliteConfig));
    if (!cfg) return NULL;

    /* Set defaults */
    cfg->chainId = HPPLITE_DEFAULT_CHAIN_ID;
    cfg->rpcUrl = strdup(HPPLITE_DEFAULT_RPC_URL);
    cfg->dataDir = strdup(HPPLITE_DEFAULT_DATA_DIR);
    cfg->role = HPPLITE_CFG_ROLE_OBSERVER;  /* Safe default */
    cfg->batchIntervalMs = 5000;  /* 5 seconds default */

    return cfg;
}

void hpplite_config_free(HppliteConfig *cfg) {
    if (!cfg) return;
    if (cfg->nodeId) free(cfg->nodeId);
    if (cfg->rpcUrl) free(cfg->rpcUrl);
    if (cfg->dataDir) free(cfg->dataDir);
    if (cfg->dbPath) free(cfg->dbPath);
    if (cfg->zmqBind) free(cfg->zmqBind);
    if (cfg->zmqSequencer) free(cfg->zmqSequencer);
    free(cfg);
}

int hpplite_config_load_env(HppliteConfig *cfg) {
    if (!cfg) return -1;

    const char *val;

    /* Role */
    val = getenv(HPPLITE_ENV_ROLE);
    if (val) {
        cfg->role = hpplite_config_parse_role(val);
        cfg->roleSource = 1;
    }

    /* Chain ID */
    val = getenv(HPPLITE_ENV_CHAIN_ID);
    if (val) {
        cfg->chainId = strtoull(val, NULL, 10);
        cfg->chainIdSource = 1;
    }

    /* RPC URL */
    val = getenv(HPPLITE_ENV_RPC_URL);
    if (val) {
        free(cfg->rpcUrl);
        cfg->rpcUrl = strdup(val);
        cfg->rpcUrlSource = 1;
    }

    /* Contract */
    val = getenv(HPPLITE_ENV_CONTRACT);
    if (val) {
        if (hpplite_config_parse_contract(cfg, val) == 0) {
            cfg->contractSource = 1;
        }
    }

    /* Private key */
    val = getenv(HPPLITE_ENV_PRIVKEY);
    if (val) {
        hpplite_config_parse_privkey(cfg, val);
    }

    /* Private key file */
    val = getenv(HPPLITE_ENV_PRIVKEY_FILE);
    if (val) {
        hpplite_config_load_privkey_file(cfg, val);
    }

    /* Data dir */
    val = getenv(HPPLITE_ENV_DATA_DIR);
    if (val) {
        free(cfg->dataDir);
        cfg->dataDir = strdup(val);
    }

    /* Node ID */
    val = getenv(HPPLITE_ENV_NODE_ID);
    if (val) {
        free(cfg->nodeId);
        cfg->nodeId = strdup(val);
    }

    /* Batch interval */
    val = getenv("HPPLITE_BATCH_INTERVAL");
    if (val) {
        int ms = parse_duration_ms(val);
        if (ms > 0) cfg->batchIntervalMs = ms;
    }

    return 0;
}

/*
** Simple URI parameter extraction
** Parses: file:path?key1=val1&key2=val2
*/
static const char *get_uri_param(const char *uri, const char *key, char *buf, size_t buflen) {
    if (!uri || !key) return NULL;

    /* Find ? */
    const char *query = strchr(uri, '?');
    if (!query) return NULL;
    query++;

    size_t keyLen = strlen(key);
    const char *p = query;

    while (*p) {
        /* Check if this is our key */
        if (strncmp(p, key, keyLen) == 0 && p[keyLen] == '=') {
            const char *valStart = p + keyLen + 1;
            const char *valEnd = valStart;

            /* Find end of value */
            while (*valEnd && *valEnd != '&') valEnd++;

            size_t valLen = valEnd - valStart;
            if (valLen >= buflen) valLen = buflen - 1;

            memcpy(buf, valStart, valLen);
            buf[valLen] = '\0';
            return buf;
        }

        /* Move to next param */
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }

    return NULL;
}

/*
** Extract database path from URI
** file:path?params → path
** file://path?params → path
*/
static char *extract_db_path(const char *uri) {
    if (!uri) return NULL;

    const char *path = uri;

    /* Skip file: prefix */
    if (strncmp(uri, "file:", 5) == 0) {
        path = uri + 5;
        /* Skip optional // */
        if (path[0] == '/' && path[1] == '/') {
            path += 2;
        }
    }

    /* Find end (? or end of string) */
    const char *end = strchr(path, '?');
    if (!end) end = path + strlen(path);

    size_t len = end - path;
    char *result = malloc(len + 1);
    memcpy(result, path, len);
    result[len] = '\0';

    return result;
}

int hpplite_config_load_uri(HppliteConfig *cfg, const char *uri) {
    if (!cfg || !uri) return -1;

    char buf[256];

    /* Extract database path */
    cfg->dbPath = extract_db_path(uri);

    /* Check if hpplite is enabled */
    if (!get_uri_param(uri, HPPLITE_URI_ENABLED, buf, sizeof(buf))) {
        /* hpplite param not present - not an HPPLite database */
        return 1;
    }

    /* Role */
    if (get_uri_param(uri, HPPLITE_URI_ROLE, buf, sizeof(buf))) {
        cfg->role = hpplite_config_parse_role(buf);
        cfg->roleSource = 2;
    }

    /* Chain ID (numeric) */
    if (get_uri_param(uri, HPPLITE_URI_CHAIN_ID, buf, sizeof(buf))) {
        cfg->chainId = strtoull(buf, NULL, 10);
        cfg->chainIdSource = 2;
    }

    /* L1 network alias */
    if (get_uri_param(uri, HPPLITE_URI_L1, buf, sizeof(buf))) {
        uint64_t chainId = hpplite_da_resolve_alias(buf);
        if (chainId > 0) {
            cfg->chainId = chainId;
            cfg->chainIdSource = 2;

            /* Also set RPC URL from alias */
            const char *rpc = hpplite_da_get_rpc_url(chainId);
            if (rpc) {
                free(cfg->rpcUrl);
                cfg->rpcUrl = strdup(rpc);
                cfg->rpcUrlSource = 2;
            }
        }
    }

    /* RPC URL */
    if (get_uri_param(uri, HPPLITE_URI_RPC_URL, buf, sizeof(buf))) {
        free(cfg->rpcUrl);
        cfg->rpcUrl = strdup(buf);
        cfg->rpcUrlSource = 2;
    }

    /* Contract */
    if (get_uri_param(uri, HPPLITE_URI_CONTRACT, buf, sizeof(buf))) {
        if (hpplite_config_parse_contract(cfg, buf) == 0) {
            cfg->contractSource = 2;
        }
    }

    /* Factory */
    if (get_uri_param(uri, HPPLITE_URI_FACTORY, buf, sizeof(buf))) {
        if (hex_to_bytes(buf, cfg->factory, 20) == 0) {
            cfg->hasFactory = 1;
        }
    }

    /* Private key */
    if (get_uri_param(uri, HPPLITE_URI_PRIVKEY, buf, sizeof(buf))) {
        hpplite_config_parse_privkey(cfg, buf);
    }

    /* Private key file */
    if (get_uri_param(uri, HPPLITE_URI_PRIVKEY_FILE, buf, sizeof(buf))) {
        hpplite_config_load_privkey_file(cfg, buf);
    }

    /* Data dir */
    if (get_uri_param(uri, HPPLITE_URI_DATA_DIR, buf, sizeof(buf))) {
        free(cfg->dataDir);
        cfg->dataDir = strdup(buf);
    }

    /* Node ID */
    if (get_uri_param(uri, HPPLITE_URI_NODE_ID, buf, sizeof(buf))) {
        free(cfg->nodeId);
        cfg->nodeId = strdup(buf);
    }

    /* Batch interval */
    if (get_uri_param(uri, "interval", buf, sizeof(buf))) {
        int ms = parse_duration_ms(buf);
        if (ms > 0) cfg->batchIntervalMs = ms;
    }

    /* ZMQ bind address (sequencer) */
    if (get_uri_param(uri, HPPLITE_URI_ZMQ_BIND, buf, sizeof(buf))) {
        free(cfg->zmqBind);
        cfg->zmqBind = strdup(buf);
    }

    /* ZMQ sequencer address (witness) */
    if (get_uri_param(uri, HPPLITE_URI_ZMQ_SEQUENCER, buf, sizeof(buf))) {
        free(cfg->zmqSequencer);
        cfg->zmqSequencer = strdup(buf);
    }

    return 0;
}

int hpplite_config_load_sqlite_uri(HppliteConfig *cfg, const char *filename) {
    if (!cfg || !filename) return -1;

    const char *val;

    /* Set database path */
    cfg->dbPath = strdup(filename);

    /* Role */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_ROLE);
    if (val) {
        cfg->role = hpplite_config_parse_role(val);
        cfg->roleSource = 2;
    }

    /* Chain ID (numeric) */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_CHAIN_ID);
    if (val) {
        cfg->chainId = strtoull(val, NULL, 10);
        cfg->chainIdSource = 2;
    }

    /* L1 network alias */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_L1);
    if (val) {
        uint64_t chainId = hpplite_da_resolve_alias(val);
        if (chainId > 0) {
            cfg->chainId = chainId;
            cfg->chainIdSource = 2;

            /* Also set RPC URL from alias */
            const char *rpc = hpplite_da_get_rpc_url(chainId);
            if (rpc) {
                free(cfg->rpcUrl);
                cfg->rpcUrl = strdup(rpc);
                cfg->rpcUrlSource = 2;
            }
        }
    }

    /* RPC URL */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_RPC_URL);
    if (val) {
        free(cfg->rpcUrl);
        cfg->rpcUrl = strdup(val);
        cfg->rpcUrlSource = 2;
    }

    /* Contract */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_CONTRACT);
    if (val) {
        if (hpplite_config_parse_contract(cfg, val) == 0) {
            cfg->contractSource = 2;
        }
    }

    /* Factory */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_FACTORY);
    if (val) {
        if (hex_to_bytes(val, cfg->factory, 20) == 0) {
            cfg->hasFactory = 1;
        }
    }

    /* Private key */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_PRIVKEY);
    if (val) {
        hpplite_config_parse_privkey(cfg, val);
    }

    /* Private key file */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_PRIVKEY_FILE);
    if (val) {
        hpplite_config_load_privkey_file(cfg, val);
    }

    /* Data dir */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_DATA_DIR);
    if (val) {
        free(cfg->dataDir);
        cfg->dataDir = strdup(val);
    }

    /* Node ID */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_NODE_ID);
    if (val) {
        free(cfg->nodeId);
        cfg->nodeId = strdup(val);
    }

    /* Batch interval */
    val = sqlite3_uri_parameter(filename, "interval");
    if (val) {
        int ms = parse_duration_ms(val);
        if (ms > 0) cfg->batchIntervalMs = ms;
    }

    /* ZMQ bind address (sequencer) */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_ZMQ_BIND);
    if (val) {
        free(cfg->zmqBind);
        cfg->zmqBind = strdup(val);
    }

    /* ZMQ sequencer address (witness) */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_ZMQ_SEQUENCER);
    if (val) {
        free(cfg->zmqSequencer);
        cfg->zmqSequencer = strdup(val);
    }

    return 0;
}

HppliteConfig *hpplite_config_load(const char *uri) {
    HppliteConfig *cfg = hpplite_config_create();
    if (!cfg) return NULL;

    /* Load from env */
    hpplite_config_load_env(cfg);

    /* Load from URI (overrides env) */
    if (uri) {
        hpplite_config_load_uri(cfg, uri);
    }

    return cfg;
}

int hpplite_config_load_privkey_file(HppliteConfig *cfg, const char *path) {
    if (!cfg || !path) return -1;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char buf[128];
    if (fgets(buf, sizeof(buf), f)) {
        /* Remove newline */
        buf[strcspn(buf, "\r\n")] = '\0';
        fclose(f);
        return hpplite_config_parse_privkey(cfg, buf);
    }

    fclose(f);
    return -1;
}

int hpplite_config_parse_privkey(HppliteConfig *cfg, const char *hex) {
    if (!cfg || !hex) return -1;

    if (hex_to_bytes(hex, cfg->privkey, 32) == 0) {
        cfg->hasPrivkey = 1;
        return 0;
    }
    return -1;
}

int hpplite_config_parse_contract(HppliteConfig *cfg, const char *hex) {
    if (!cfg || !hex) return -1;

    if (hex_to_bytes(hex, cfg->contract, 20) == 0) {
        cfg->hasContract = 1;
        return 0;
    }
    return -1;
}

static const char *source_str(int source) {
    switch (source) {
        case 1: return "env";
        case 2: return "uri";
        default: return "default";
    }
}

void hpplite_config_print(const HppliteConfig *cfg) {
    if (!cfg) return;

    printf("HPPLite Configuration:\n");
    printf("  nodeId:     %s\n", cfg->nodeId ? cfg->nodeId : "(auto)");
    printf("  role:       %s (%s)\n", hpplite_config_role_str(cfg->role), source_str(cfg->roleSource));
    printf("  chainId:    %llu (%s)\n", (unsigned long long)cfg->chainId, source_str(cfg->chainIdSource));
    printf("  rpcUrl:     %s (%s)\n", cfg->rpcUrl, source_str(cfg->rpcUrlSource));
    printf("  contract:   %s", cfg->hasContract ? "0x" : "(not set)");
    if (cfg->hasContract) {
        for (int i = 0; i < 20; i++) printf("%02x", cfg->contract[i]);
        printf(" (%s)", source_str(cfg->contractSource));
    }
    printf("\n");
    printf("  dataDir:    %s\n", cfg->dataDir);
    printf("  dbPath:     %s\n", cfg->dbPath ? cfg->dbPath : "(not set)");
    printf("  privkey:    %s\n", cfg->hasPrivkey ? "(set)" : "(not set)");
    printf("  interval:   %dms\n", cfg->batchIntervalMs);
}
