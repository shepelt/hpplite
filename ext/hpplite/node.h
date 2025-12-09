/*
** HPPLite Node - Singleton Sequencer Implementation
**
** Single sequencer model - all writes go through sequencer.
** L1 contract handles coordination (lease/lock mechanism).
** No peer-to-peer networking - all data flows through L1.
*/

#ifndef HPPLITE_NODE_H
#define HPPLITE_NODE_H

#include "hpplite.h"
#include "batch.h"
#include "crypto.h"
#include "l1_interface.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** Node state
*/
typedef enum {
    HPPLITE_STATE_INIT = 0,
    HPPLITE_STATE_RUNNING,
    HPPLITE_STATE_SYNCING,
    HPPLITE_STATE_STOPPED,
    HPPLITE_STATE_ERROR
} HppliteNodeState;

/*
** Node configuration
*/
typedef struct HppliteNodeConfig {
    /* Identity */
    char *nodeId;                   /* Human-readable node ID */
    unsigned char privkey[32];      /* Node's private key */
    int hasPrivkey;                 /* Whether privkey is set */

    /* Paths */
    char *dataDir;                  /* Data directory */
    char *dbPath;                   /* SQLite database path */

    /* L1 Connection */
    char *rpcUrl;                   /* L1 RPC endpoint */
    unsigned char factory[20];      /* Factory contract address */
    int hasFactory;                 /* Whether factory is set */
    unsigned char contract[20];     /* Direct contract address */
    int hasContract;                /* Whether contract is set */

    /* Sequencer Settings */
    int checkpointInterval;         /* Batches between L1 checkpoints */
    int batchIntervalMs;            /* Auto-flush interval (0 = disabled) */
} HppliteNodeConfig;

/*
** Main node structure (simplified - sequencer only)
*/
typedef struct HppliteNode {
    /* Configuration */
    HppliteNodeConfig config;

    /* State */
    HppliteNodeState state;

    /* Instance ID for lease (unique per process) */
    unsigned char instanceId[32];

    /* Database and context */
    sqlite3 *db;
    HppliteCtx *ctx;

    /* L1 connection */
    HppliteL1 *l1;

    /* Crypto */
    HppliteCrypto *crypto;
    HppliteKeypair keypair;

    /* Checkpoint tracking */
    uint64_t lastCheckpointHeight;
    unsigned char lastCheckpointRoot[HPPLITE_HASH_SIZE];
    int batchesSinceCheckpoint;

    /* Callbacks */
    void *callbackArg;
    void (*onBatchPosted)(void *arg, uint64_t height, const char *txHash);
    void (*onCheckpointSubmitted)(void *arg, uint64_t fromHeight, uint64_t toHeight, const char *txHash);

    /* Statistics */
    uint64_t batchesProduced;

    /* Auto-flush timer thread */
    void *timerThread;              /* pthread_t (opaque) */
    void *flushMutex;               /* pthread_mutex_t */
    volatile int timerRunning;
    int64_t lastFlushTime;
    unsigned char lastFlushedRoot[HPPLITE_HASH_SIZE];

    /* Ownership flags */
    int ownsDb;
    int ownsL1;
} HppliteNode;

/*
** Create a node with configuration.
** Returns NULL on failure.
*/
HppliteNode *hpplite_node_create(const HppliteNodeConfig *config);

/*
** Create a node using an existing database connection.
** The node does NOT own the db (won't close it on destroy).
*/
HppliteNode *hpplite_node_create_with_db(sqlite3 *db, const HppliteNodeConfig *config);

/*
** Free a node and all resources.
*/
void hpplite_node_destroy(HppliteNode *node);

/*
** Start auto-flush timer thread.
** Periodically flushes pending SQL based on config.batchIntervalMs.
*/
void hpplite_node_start_timer(HppliteNode *node);

/*
** Stop auto-flush timer thread.
*/
void hpplite_node_stop_timer(HppliteNode *node);

/*
** Claim sequencer lease on L1.
** Returns 0 on success, -1 if lease unavailable.
*/
int hpplite_node_claim_lease(HppliteNode *node);

/*
** Renew sequencer lease on L1.
** Returns 0 on success, -1 on failure.
*/
int hpplite_node_renew_lease(HppliteNode *node);

/*
** Check if our lease is still active.
** Returns 1 if active, 0 if expired/not held.
*/
int hpplite_node_has_lease(HppliteNode *node);

/*
** Start the node (claim lease and begin operations).
*/
int hpplite_node_start(HppliteNode *node);

/*
** Stop the node.
*/
void hpplite_node_stop(HppliteNode *node);

/*
** Execute SQL (queued for next batch).
*/
int hpplite_node_exec(HppliteNode *node, const char *sql, char **errMsg);

/*
** Flush pending SQL to create a batch and post to L1.
** Returns the batch height, or 0 if no pending SQL.
*/
uint64_t hpplite_node_flush_batch(HppliteNode *node);

/*
** Get number of pending SQL statements.
*/
int hpplite_node_pending_count(HppliteNode *node);

/*
** Check if node should create a checkpoint.
** Returns 1 if checkpoint interval reached.
*/
int hpplite_node_should_checkpoint(HppliteNode *node);

/*
** Create and submit checkpoint to L1.
** Returns transaction hash or NULL on error.
*/
char *hpplite_node_submit_checkpoint(HppliteNode *node);

/*
** Set the L1 connection for this node.
*/
void hpplite_node_set_l1(HppliteNode *node, HppliteL1 *l1);

/*
** Get current block height.
*/
uint64_t hpplite_node_get_height(HppliteNode *node);

/*
** Get current state root.
*/
void hpplite_node_get_state_root(HppliteNode *node, unsigned char *out);

/*
** Get node's public key.
*/
void hpplite_node_get_pubkey(HppliteNode *node, unsigned char *out);

/*
** Get node's Ethereum address.
*/
void hpplite_node_get_address(HppliteNode *node, unsigned char *out);

/*
** Get node's instance ID (32 bytes).
*/
void hpplite_node_get_instance_id(HppliteNode *node, unsigned char *out);

/*
** Get node statistics as JSON string.
** Caller must free with sqlite3_free().
*/
char *hpplite_node_get_stats(HppliteNode *node);

/*
** Register node with global registry for SQL functions.
** Called internally during node creation.
*/
void hpplite_register_node(sqlite3 *db, HppliteNode *node);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_NODE_H */
