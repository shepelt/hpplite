/*
** HPPLite Configuration
**
** Layered configuration system with priority:
**   URI params > Env vars > Defaults
**
** URI format:
**   file:state.db?hpplite=on&role=sequencer&l1=hpp-sepolia&contract=0x...
**
** Environment variables:
**   HPPLITE_ROLE        - sequencer|witness|observer
**   HPPLITE_CHAIN_ID    - Chain ID (e.g., 181228)
**   HPPLITE_RPC_URL     - RPC endpoint
**   HPPLITE_CONTRACT    - L1 contract address
**   HPPLITE_PRIVKEY     - Private key (hex)
**   HPPLITE_PRIVKEY_FILE - Path to private key file
**   HPPLITE_DATA_DIR    - Data directory for batches
**   HPPLITE_NODE_ID     - Node identifier
*/

#ifndef HPPLITE_CONFIG_H
#define HPPLITE_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** Environment variable names
*/
#define HPPLITE_ENV_ROLE          "HPPLITE_ROLE"
#define HPPLITE_ENV_CHAIN_ID      "HPPLITE_CHAIN_ID"
#define HPPLITE_ENV_RPC_URL       "HPPLITE_RPC_URL"
#define HPPLITE_ENV_CONTRACT      "HPPLITE_CONTRACT"
#define HPPLITE_ENV_PRIVKEY       "HPPLITE_PRIVKEY"
#define HPPLITE_ENV_PRIVKEY_FILE  "HPPLITE_PRIVKEY_FILE"
#define HPPLITE_ENV_DATA_DIR      "HPPLITE_DATA_DIR"
#define HPPLITE_ENV_NODE_ID       "HPPLITE_NODE_ID"

/*
** URI parameter names (same as env vars but lowercase, no prefix)
*/
#define HPPLITE_URI_ENABLED       "hpplite"
#define HPPLITE_URI_ROLE          "role"
#define HPPLITE_URI_CHAIN_ID      "chainid"
#define HPPLITE_URI_L1            "l1"          /* Alias for chain (hpp-sepolia, etc) */
#define HPPLITE_URI_RPC_URL       "rpc"
#define HPPLITE_URI_CONTRACT      "contract"
#define HPPLITE_URI_PRIVKEY       "privkey"
#define HPPLITE_URI_PRIVKEY_FILE  "keyfile"
#define HPPLITE_URI_DATA_DIR      "datadir"
#define HPPLITE_URI_NODE_ID       "nodeid"
#define HPPLITE_URI_FACTORY       "factory"
#define HPPLITE_URI_ZMQ_BIND      "zmq_bind"      /* Sequencer: ZMQ bind address */
#define HPPLITE_URI_ZMQ_SEQUENCER "zmq_sequencer" /* Witness: ZMQ sequencer address */

/*
** Default values
*/
#define HPPLITE_DEFAULT_CHAIN_ID   181228
#define HPPLITE_DEFAULT_RPC_URL    "https://sepolia.hpp.io"
#define HPPLITE_DEFAULT_DATA_DIR   "/tmp/hpplite"

/*
** Role enum
*/
typedef enum {
    HPPLITE_CFG_ROLE_UNKNOWN = 0,
    HPPLITE_CFG_ROLE_SEQUENCER,
    HPPLITE_CFG_ROLE_WITNESS,
    HPPLITE_CFG_ROLE_OBSERVER
} HppliteConfigRole;

/*
** Unified configuration struct
*/
typedef struct HppliteConfig {
    /* Node identity */
    char *nodeId;                    /* Node identifier */
    HppliteConfigRole role;          /* sequencer|witness|observer */
    unsigned char privkey[32];       /* Node private key */
    int hasPrivkey;                  /* Whether privkey is set */

    /* L1 connection */
    uint64_t chainId;                /* Chain ID */
    char *rpcUrl;                    /* RPC endpoint */
    unsigned char contract[20];      /* L1 contract address */
    int hasContract;                 /* Whether contract is set */
    unsigned char factory[20];       /* Factory contract address */
    int hasFactory;                  /* Whether factory is set */

    /* Paths */
    char *dataDir;                   /* Data directory */
    char *dbPath;                    /* Database path (from URI) */

    /* Batching */
    int batchIntervalMs;             /* Batch interval in milliseconds */

    /* ZMQ networking */
    char *zmqBind;                   /* Sequencer: ZMQ bind address (e.g. tcp://star:5555) */
    char *zmqSequencer;              /* Witness: ZMQ sequencer address (e.g. tcp://host:5555) */

    /* Source tracking (for debugging) */
    int roleSource;                  /* 0=default, 1=env, 2=uri */
    int chainIdSource;
    int rpcUrlSource;
    int contractSource;
} HppliteConfig;

/*
** Create config with defaults.
** Caller must free with hpplite_config_free().
*/
HppliteConfig *hpplite_config_create(void);

/*
** Free config and all allocated strings.
*/
void hpplite_config_free(HppliteConfig *cfg);

/*
** Load config from environment variables.
** Overrides current values if env vars are set.
*/
int hpplite_config_load_env(HppliteConfig *cfg);

/*
** Load config from SQLite URI parameters.
** Call after sqlite3_open() with the db handle.
** Overrides current values if URI params are set.
*/
int hpplite_config_load_uri(HppliteConfig *cfg, const char *uri);

/*
** Load config from SQLite db filename using sqlite3_uri_parameter().
** This is the correct way to load config from an open database,
** since sqlite3_db_filename() only returns the path, not the URI.
*/
int hpplite_config_load_sqlite_uri(HppliteConfig *cfg, const char *filename);

/*
** Load config from all sources (defaults + env + uri).
** Convenience function that calls the above in order.
*/
HppliteConfig *hpplite_config_load(const char *uri);

/*
** Parse role string to enum.
*/
HppliteConfigRole hpplite_config_parse_role(const char *str);

/*
** Get role string from enum.
*/
const char *hpplite_config_role_str(HppliteConfigRole role);

/*
** Load private key from file.
** Returns 0 on success, -1 on error.
*/
int hpplite_config_load_privkey_file(HppliteConfig *cfg, const char *path);

/*
** Parse hex private key string.
** Returns 0 on success, -1 on error.
*/
int hpplite_config_parse_privkey(HppliteConfig *cfg, const char *hex);

/*
** Parse hex contract address string.
** Returns 0 on success, -1 on error.
*/
int hpplite_config_parse_contract(HppliteConfig *cfg, const char *hex);

/*
** Print config for debugging.
*/
void hpplite_config_print(const HppliteConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_CONFIG_H */
