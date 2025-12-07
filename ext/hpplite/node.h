/*
** HPPLite Node - Unified Sequencer/Witness Implementation
**
** Each node can operate as either sequencer or witness.
** Role is determined by L1 contract state.
** Nodes communicate via ZeroMQ pub/sub.
*/

#ifndef HPPLITE_NODE_H
#define HPPLITE_NODE_H

#include "hpplite.h"
#include "batch.h"
#include "crypto.h"
#include "fs_storage.h"
#include "l1_interface.h"
#include <stdint.h>

#ifdef HPPLITE_ENABLE_ZMQ
#include "zmq_transport.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
** Node role
*/
typedef enum {
    HPPLITE_ROLE_UNKNOWN = 0,
    HPPLITE_ROLE_SEQUENCER,
    HPPLITE_ROLE_WITNESS
} HppliteNodeRole;

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
** Attestation from a witness
*/
typedef struct HppliteAttestation {
    uint64_t height;
    unsigned char stateRoot[HPPLITE_HASH_SIZE];
    char *batchRef;
    unsigned char witnessPubkey[HPPLITE_PUBKEY_SIZE];
    HppliteSignature sig;
    struct HppliteAttestation *pNext;
} HppliteAttestation;

/*
** Pending commitment waiting for signatures
*/
typedef struct HpplitePendingCommit {
    uint64_t height;
    unsigned char stateRoot[HPPLITE_HASH_SIZE];
    char *batchRef;
    HppliteAttestation *attestations;
    int nAttestations;
    int64_t proposedAt;  /* Unix timestamp */
    struct HpplitePendingCommit *pNext;
} HpplitePendingCommit;

/*
** Node configuration
*/
typedef struct HppliteNodeConfig {
    /* Identity */
    char *nodeId;                   /* Human-readable node ID */
    unsigned char privkey[32];      /* Node's private key */

    /* Paths */
    char *dataDir;                  /* Data directory */
    char *dbPath;                   /* SQLite database path */

    /* Network */
    char *bindAddress;              /* Address to bind, e.g. tcp on port 5555 */
    char *sequencerAddress;         /* Sequencer address to connect to */

    /* Thresholds */
    int requiredAttestations;       /* Signatures needed for finality */
    int checkpointInterval;         /* Batches between L1 checkpoints */
    int batchTimeoutMs;             /* Max time to wait for batch (ms) */
    int attestationTimeoutMs;       /* Max time to wait for attestations (ms) */
} HppliteNodeConfig;

/*
** Main node structure
*/
typedef struct HppliteNode {
    /* Configuration */
    HppliteNodeConfig config;

    /* State */
    HppliteNodeState state;
    HppliteNodeRole role;

    /* Database and context */
    sqlite3 *db;
    HppliteCtx *ctx;
    HppliteStorage *storage;

    /* L1 connection */
    HppliteL1 *l1;

    /* Crypto */
    HppliteCrypto *crypto;
    HppliteKeypair keypair;

    /* Networking (ZeroMQ) */
#ifdef HPPLITE_ENABLE_ZMQ
    HppliteZmqTransport *zmqTransport;
#endif

    /* Sequencer state */
    HpplitePendingCommit *pendingCommits;
    int nPendingCommits;

    /* Checkpoint tracking (shared) */
    uint64_t lastCheckpointHeight;              /* Height at last L1 checkpoint */
    unsigned char lastCheckpointRoot[HPPLITE_HASH_SIZE];  /* State root at last checkpoint */
    int batchesSinceCheckpoint;                 /* Batches since last checkpoint */

    /* Pending checkpoint (sequencer collects attestations for this) */
    HppliteCheckpoint *pendingCheckpoint;
    HppliteCheckpointAttestation *cpAttestations;
    int nCpAttestations;

    /* Witness state */
    uint64_t lastVerifiedHeight;
    unsigned char lastVerifiedRoot[HPPLITE_HASH_SIZE];

    /* Verified batches for checkpoint (witness accumulates these) */
    uint64_t checkpointFromHeight;              /* Start height of current checkpoint window */
    unsigned char checkpointPreRoot[HPPLITE_HASH_SIZE];   /* Pre-state root at checkpoint start */

    /* Known witnesses (for sequencer - fallback if L1 not available) */
    unsigned char **witnessPubkeys;
    int nWitnesses;

    /* Callbacks */
    void *callbackArg;
    void (*onBatchProduced)(void *arg, HppliteBatch *batch);
    void (*onBatchVerified)(void *arg, HppliteBatch *batch, int valid);
    void (*onCommitmentFinalized)(void *arg, uint64_t height, unsigned char *stateRoot);
    void (*onCheckpointSubmitted)(void *arg, HppliteCheckpoint *cp, const char *txHash);
    void (*onRoleChanged)(void *arg, HppliteNodeRole newRole);

    /* Statistics */
    uint64_t batchesProduced;
    uint64_t batchesVerified;
    uint64_t attestationsSent;
    uint64_t attestationsReceived;
} HppliteNode;

/*
** Initialize a node with configuration.
** Returns NULL on failure.
*/
HppliteNode *hpplite_node_create(const HppliteNodeConfig *config);

/*
** Free a node and all resources.
*/
void hpplite_node_destroy(HppliteNode *node);

/*
** Set node role (sequencer or witness).
** In production, this would be determined by L1 contract.
*/
int hpplite_node_set_role(HppliteNode *node, HppliteNodeRole role);

/*
** Start the node (begin networking and processing).
*/
int hpplite_node_start(HppliteNode *node);

/*
** Stop the node.
*/
void hpplite_node_stop(HppliteNode *node);

/*
** Process events (call in main loop or from timer).
** Returns number of events processed, or -1 on error.
*/
int hpplite_node_process(HppliteNode *node, int timeoutMs);

/*
** === Sequencer Operations ===
*/

/*
** Execute SQL (sequencer only).
** SQL is queued for next batch.
*/
int hpplite_node_exec(HppliteNode *node, const char *sql, char **errMsg);

/*
** Flush pending SQL to create a batch and broadcast to witnesses.
** Returns the batch height, or 0 if no pending SQL.
*/
uint64_t hpplite_node_flush_batch(HppliteNode *node);

/*
** Get number of pending SQL statements.
*/
int hpplite_node_pending_count(HppliteNode *node);

/*
** Add a witness public key (for attestation validation).
*/
int hpplite_node_add_witness(HppliteNode *node, const unsigned char *pubkey);

/*
** === Witness Operations ===
*/

/*
** Connect to sequencer (witness only).
*/
int hpplite_node_connect_sequencer(HppliteNode *node, const char *address);

/*
** Manually submit an attestation (usually done automatically).
*/
int hpplite_node_submit_attestation(
    HppliteNode *node,
    uint64_t height,
    const unsigned char *stateRoot,
    const char *batchRef
);

/*
** Verify a batch (witness side).
** With checkpoint mode: verifies but doesn't attest (waits for checkpoint).
** Returns 0 if valid, -1 if invalid or error.
*/
int hpplite_node_verify_batch(
    HppliteNode *node,
    HppliteBatch *batch,
    const char *batchRef
);

/*
** Receive an attestation from a witness (sequencer side).
** Validates signature and adds to pending commit.
*/
int hpplite_node_receive_attestation(
    HppliteNode *node,
    const HppliteAttestation *att
);

/*
** === Checkpoint Operations ===
*/

/*
** Create a checkpoint proposal (sequencer only).
** Called when checkpoint interval is reached.
** Returns the checkpoint or NULL on error.
*/
HppliteCheckpoint *hpplite_node_create_checkpoint(HppliteNode *node);

/*
** Receive a checkpoint attestation from a witness (sequencer side).
** Returns 0 on success, -1 on error.
*/
int hpplite_node_receive_checkpoint_attestation(
    HppliteNode *node,
    const HppliteCheckpointAttestation *att
);

/*
** Submit pending checkpoint to L1 (sequencer only).
** Called when enough attestations collected.
** Returns transaction hash or NULL on error.
*/
char *hpplite_node_submit_checkpoint(HppliteNode *node);

/*
** Sign and submit checkpoint attestation (witness side).
** Called when checkpoint is requested.
** Returns 0 on success, -1 on error.
*/
int hpplite_node_attest_checkpoint(
    HppliteNode *node,
    const HppliteCheckpoint *cp
);

/*
** Check if node should create a checkpoint.
** Returns 1 if checkpoint interval reached, 0 otherwise.
*/
int hpplite_node_should_checkpoint(HppliteNode *node);

/*
** Set the L1 connection for this node.
** For M1: all nodes share the same L1 mock pointer.
*/
void hpplite_node_set_l1(HppliteNode *node, HppliteL1 *l1);

/*
** Sync role from L1 contract state.
** Updates node role based on current L1 sequencer.
** Returns new role.
*/
HppliteNodeRole hpplite_node_sync_role_from_l1(HppliteNode *node);

/*
** === Query Operations ===
*/

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
** Get node statistics as JSON string.
** Caller must free with sqlite3_free().
*/
char *hpplite_node_get_stats(HppliteNode *node);

/*
** === Message Types (for network protocol) ===
*/

#define HPPLITE_MSG_BATCH       1   /* Sequencer → Witnesses: new batch */
#define HPPLITE_MSG_ATTESTATION 2   /* Witness → Sequencer: signed attestation (per-batch) */
#define HPPLITE_MSG_COMMITMENT  3   /* Announcement: commitment finalized */
#define HPPLITE_MSG_SYNC_REQ    4   /* Request: sync from height */
#define HPPLITE_MSG_SYNC_RESP   5   /* Response: batch data for sync */
#define HPPLITE_MSG_CHECKPOINT  6   /* Sequencer → Witnesses: checkpoint request */
#define HPPLITE_MSG_CP_ATT      7   /* Witness → Sequencer: checkpoint attestation */

/*
** Serialize a batch message for network transmission.
** Caller must free with sqlite3_free().
*/
unsigned char *hpplite_node_serialize_batch_msg(
    HppliteNode *node,
    HppliteBatch *batch,
    const char *batchRef,
    int *outSize
);

/*
** Parse a batch message from network.
** Returns batch and sets batchRef (caller must free both).
*/
HppliteBatch *hpplite_node_parse_batch_msg(
    const unsigned char *data,
    int size,
    char **batchRef
);

/*
** Serialize an attestation message.
*/
unsigned char *hpplite_node_serialize_attestation_msg(
    HppliteNode *node,
    const HppliteAttestation *att,
    int *outSize
);

/*
** Parse an attestation message.
*/
HppliteAttestation *hpplite_node_parse_attestation_msg(
    const unsigned char *data,
    int size
);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_NODE_H */
