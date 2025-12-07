/*
** HPPLite Node - Unified Sequencer/Witness Implementation
**
** Each node can operate as either sequencer or witness.
** Role is determined by L1 contract state.
*/

#include "node.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

/*
** Helper: duplicate a string using sqlite3_malloc
*/
static char *node_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *dup = sqlite3_malloc((int)len);
    if (dup) memcpy(dup, s, len);
    return dup;
}

/*
** Free an attestation
*/
static void free_attestation(HppliteAttestation *att) {
    if (att) {
        sqlite3_free(att->batchRef);
        sqlite3_free(att);
    }
}

/*
** Free all attestations in a list
*/
static void free_attestation_list(HppliteAttestation *head) {
    while (head) {
        HppliteAttestation *next = head->pNext;
        free_attestation(head);
        head = next;
    }
}

/*
** Free a pending commit
*/
static void free_pending_commit(HpplitePendingCommit *pc) {
    if (pc) {
        sqlite3_free(pc->batchRef);
        free_attestation_list(pc->attestations);
        sqlite3_free(pc);
    }
}

/*
** Free all pending commits
*/
static void free_pending_commits(HppliteNode *node) {
    while (node->pendingCommits) {
        HpplitePendingCommit *next = node->pendingCommits->pNext;
        free_pending_commit(node->pendingCommits);
        node->pendingCommits = next;
    }
    node->nPendingCommits = 0;
}

/*
** Initialize a node with configuration.
*/
HppliteNode *hpplite_node_create(const HppliteNodeConfig *config) {
    HppliteNode *node;
    int rc;

    if (!config) return NULL;

    node = sqlite3_malloc(sizeof(HppliteNode));
    if (!node) return NULL;
    memset(node, 0, sizeof(HppliteNode));

    /* Copy configuration */
    node->config.nodeId = node_strdup(config->nodeId);
    memcpy(node->config.privkey, config->privkey, 32);
    node->config.dataDir = node_strdup(config->dataDir);
    node->config.dbPath = node_strdup(config->dbPath);
    node->config.bindAddress = node_strdup(config->bindAddress);
    node->config.sequencerAddress = node_strdup(config->sequencerAddress);
    node->config.requiredAttestations = config->requiredAttestations > 0 ? config->requiredAttestations : 1;
    node->config.checkpointInterval = config->checkpointInterval > 0 ? config->checkpointInterval : 100;
    node->config.batchTimeoutMs = config->batchTimeoutMs > 0 ? config->batchTimeoutMs : 1000;
    node->config.attestationTimeoutMs = config->attestationTimeoutMs > 0 ? config->attestationTimeoutMs : 5000;

    /* Initialize state */
    node->state = HPPLITE_STATE_INIT;
    node->role = HPPLITE_ROLE_UNKNOWN;

    /* Initialize crypto */
    node->crypto = hpplite_crypto_init();
    if (!node->crypto) {
        hpplite_node_destroy(node);
        return NULL;
    }

    /* Initialize keypair from private key */
    rc = hpplite_crypto_keypair_from_privkey(node->crypto, &node->keypair, config->privkey);
    if (rc != 0) {
        hpplite_node_destroy(node);
        return NULL;
    }

    /* Open database */
    rc = sqlite3_open(config->dbPath, &node->db);
    if (rc != SQLITE_OK) {
        hpplite_node_destroy(node);
        return NULL;
    }

    /* Initialize HPPLite context */
    rc = hpplite_init(node->db, &node->ctx);
    if (rc != SQLITE_OK) {
        hpplite_node_destroy(node);
        return NULL;
    }

    /* Initialize storage */
    rc = hpplite_storage_init(config->dataDir, &node->storage);
    if (rc != 0) {
        hpplite_node_destroy(node);
        return NULL;
    }

    return node;
}

/*
** Free a node and all resources.
*/
void hpplite_node_destroy(HppliteNode *node) {
    int i;

    if (!node) return;

    /* Stop if running */
    if (node->state == HPPLITE_STATE_RUNNING) {
        hpplite_node_stop(node);
    }

    /* Free pending commits */
    free_pending_commits(node);

    /* Free pending checkpoint */
    if (node->pendingCheckpoint) {
        hpplite_checkpoint_free(node->pendingCheckpoint);
    }

    /* Free checkpoint attestations */
    if (node->cpAttestations) {
        sqlite3_free(node->cpAttestations);
    }

    /* Free witness pubkeys */
    if (node->witnessPubkeys) {
        for (i = 0; i < node->nWitnesses; i++) {
            sqlite3_free(node->witnessPubkeys[i]);
        }
        sqlite3_free(node->witnessPubkeys);
    }

    /* Note: L1 is NOT freed here - it's a shared pointer for M1 */

    /* Close storage */
    if (node->storage) {
        hpplite_storage_close(node->storage);
    }

    /* Shutdown HPPLite context */
    if (node->ctx) {
        hpplite_shutdown(node->ctx);
    }

    /* Close database */
    if (node->db) {
        sqlite3_close(node->db);
    }

    /* Free crypto */
    if (node->crypto) {
        hpplite_crypto_free(node->crypto);
    }

    /* Free config strings */
    sqlite3_free(node->config.nodeId);
    sqlite3_free(node->config.dataDir);
    sqlite3_free(node->config.dbPath);
    sqlite3_free(node->config.bindAddress);
    sqlite3_free(node->config.sequencerAddress);

    sqlite3_free(node);
}

/*
** Set node role (sequencer or witness).
*/
int hpplite_node_set_role(HppliteNode *node, HppliteNodeRole role) {
    if (!node) return -1;

    HppliteNodeRole oldRole = node->role;
    node->role = role;

    /* Notify callback if role changed */
    if (oldRole != role && node->onRoleChanged) {
        node->onRoleChanged(node->callbackArg, role);
    }

    return 0;
}

/*
** Start the node (begin networking and processing).
*/
int hpplite_node_start(HppliteNode *node) {
    if (!node) return -1;
    if (node->state == HPPLITE_STATE_RUNNING) return 0;

    /* TODO: Initialize ZeroMQ sockets based on role */
    /*
    if (node->role == HPPLITE_ROLE_SEQUENCER) {
        // Create PUB socket for broadcasting batches
        // Create ROUTER socket for receiving attestations
    } else if (node->role == HPPLITE_ROLE_WITNESS) {
        // Create SUB socket for receiving batches
        // Create DEALER socket for sending attestations
    }
    */

    node->state = HPPLITE_STATE_RUNNING;
    return 0;
}

/*
** Stop the node.
*/
void hpplite_node_stop(HppliteNode *node) {
    if (!node) return;
    if (node->state != HPPLITE_STATE_RUNNING) return;

    /* TODO: Close ZeroMQ sockets */
    /*
    if (node->zmqPublisher) zmq_close(node->zmqPublisher);
    if (node->zmqSubscriber) zmq_close(node->zmqSubscriber);
    if (node->zmqDealer) zmq_close(node->zmqDealer);
    if (node->zmqRouter) zmq_close(node->zmqRouter);
    if (node->zmqContext) zmq_ctx_destroy(node->zmqContext);
    */

    node->state = HPPLITE_STATE_STOPPED;
}

/*
** Process events (call in main loop or from timer).
*/
int hpplite_node_process(HppliteNode *node, int timeoutMs) {
    int eventsProcessed = 0;

    if (!node || node->state != HPPLITE_STATE_RUNNING) return -1;

    (void)timeoutMs; /* TODO: use for zmq_poll timeout */

    /* TODO: Poll ZeroMQ sockets and process messages */
    /*
    zmq_pollitem_t items[] = {
        { node->zmqSubscriber, 0, ZMQ_POLLIN, 0 },
        { node->zmqRouter, 0, ZMQ_POLLIN, 0 },
    };
    int rc = zmq_poll(items, 2, timeoutMs);
    if (rc > 0) {
        if (items[0].revents & ZMQ_POLLIN) {
            // Handle incoming batch (witness)
            eventsProcessed++;
        }
        if (items[1].revents & ZMQ_POLLIN) {
            // Handle incoming attestation (sequencer)
            eventsProcessed++;
        }
    }
    */

    /* Check for pending commits that have enough attestations */
    if (node->role == HPPLITE_ROLE_SEQUENCER) {
        HpplitePendingCommit *pc = node->pendingCommits;
        HpplitePendingCommit *prev = NULL;

        while (pc) {
            if (pc->nAttestations >= node->config.requiredAttestations) {
                /* Finality achieved! */
                if (node->onCommitmentFinalized) {
                    node->onCommitmentFinalized(node->callbackArg, pc->height, pc->stateRoot);
                }

                /* Remove from list */
                HpplitePendingCommit *next = pc->pNext;
                if (prev) {
                    prev->pNext = next;
                } else {
                    node->pendingCommits = next;
                }
                node->nPendingCommits--;
                free_pending_commit(pc);
                pc = next;
                eventsProcessed++;
            } else {
                prev = pc;
                pc = pc->pNext;
            }
        }
    }

    return eventsProcessed;
}

/*
** === Sequencer Operations ===
*/

/*
** Execute SQL (sequencer only).
*/
int hpplite_node_exec(HppliteNode *node, const char *sql, char **errMsg) {
    if (!node || !sql) return -1;
    if (node->role != HPPLITE_ROLE_SEQUENCER) {
        if (errMsg) *errMsg = node_strdup("Node is not sequencer");
        return -1;
    }

    return hpplite_exec(node->ctx, sql, errMsg);
}

/*
** Flush pending SQL to create a batch and broadcast to witnesses.
*/
uint64_t hpplite_node_flush_batch(HppliteNode *node) {
    HppliteBatch *batch;
    HpplitePendingCommit *pc;
    char *batchRef;
    int rc;

    if (!node) return 0;
    if (node->role != HPPLITE_ROLE_SEQUENCER) return 0;

    /* Create batch from pending SQL */
    batch = hpplite_flush_block(node->ctx);
    if (!batch) return 0;

    /* Store batch to disk */
    batchRef = hpplite_storage_store_batch(node->storage, batch);
    if (!batchRef) {
        hpplite_batch_free(batch);
        return 0;
    }

    /* Create pending commit to track attestations */
    pc = sqlite3_malloc(sizeof(HpplitePendingCommit));
    if (!pc) {
        sqlite3_free(batchRef);
        hpplite_batch_free(batch);
        return 0;
    }
    memset(pc, 0, sizeof(HpplitePendingCommit));

    pc->height = batch->height;
    memcpy(pc->stateRoot, batch->postStateRoot, HPPLITE_HASH_SIZE);
    pc->batchRef = batchRef;
    pc->proposedAt = (int64_t)time(NULL);

    /* Add to pending commits list */
    pc->pNext = node->pendingCommits;
    node->pendingCommits = pc;
    node->nPendingCommits++;

    /* Track checkpoint progress */
    node->batchesSinceCheckpoint++;

    /* Initialize checkpoint window if first batch */
    if (node->batchesSinceCheckpoint == 1) {
        node->lastCheckpointHeight = batch->height - 1;  /* Start from previous height */
        memcpy(node->lastCheckpointRoot, batch->preStateRoot, HPPLITE_HASH_SIZE);
    }

    /* Update stats */
    node->batchesProduced++;

    /* Notify callback */
    if (node->onBatchProduced) {
        node->onBatchProduced(node->callbackArg, batch);
    }

    /* TODO: Broadcast batch to witnesses via ZeroMQ */
    /*
    int msgSize;
    unsigned char *msg = hpplite_node_serialize_batch_msg(node, batch, batchRef, &msgSize);
    if (msg) {
        zmq_send(node->zmqPublisher, msg, msgSize, 0);
        sqlite3_free(msg);
    }
    */

    uint64_t height = batch->height;
    hpplite_batch_free(batch);
    return height;
}

/*
** Get number of pending SQL statements.
*/
int hpplite_node_pending_count(HppliteNode *node) {
    if (!node || !node->ctx) return 0;
    return hpplite_pending_count(node->ctx);
}

/*
** Add a witness public key (for attestation validation).
*/
int hpplite_node_add_witness(HppliteNode *node, const unsigned char *pubkey) {
    unsigned char **newList;
    unsigned char *pubkeyCopy;

    if (!node || !pubkey) return -1;

    /* Allocate copy of pubkey */
    pubkeyCopy = sqlite3_malloc(HPPLITE_PUBKEY_SIZE);
    if (!pubkeyCopy) return -1;
    memcpy(pubkeyCopy, pubkey, HPPLITE_PUBKEY_SIZE);

    /* Expand witness list */
    newList = sqlite3_realloc(node->witnessPubkeys, (node->nWitnesses + 1) * sizeof(unsigned char*));
    if (!newList) {
        sqlite3_free(pubkeyCopy);
        return -1;
    }

    node->witnessPubkeys = newList;
    node->witnessPubkeys[node->nWitnesses] = pubkeyCopy;
    node->nWitnesses++;

    return 0;
}

/*
** Check if a pubkey is in the witness list (uses L1 if available)
*/
static int is_valid_witness(HppliteNode *node, const unsigned char *pubkey) {
    int i;

    /* Prefer L1 for witness registry */
    if (node->l1) {
        return hpplite_l1_is_witness(node->l1, pubkey);
    }

    /* Fallback to local list */
    for (i = 0; i < node->nWitnesses; i++) {
        if (memcmp(node->witnessPubkeys[i], pubkey, HPPLITE_PUBKEY_SIZE) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
** Receive an attestation (sequencer side)
*/
int hpplite_node_receive_attestation(HppliteNode *node, const HppliteAttestation *att) {
    HpplitePendingCommit *pc;
    HppliteAttestation *attCopy;
    unsigned char msgHash[HPPLITE_HASH_SIZE];
    int valid;

    if (!node || !att) return -1;
    if (node->role != HPPLITE_ROLE_SEQUENCER) return -1;

    /* Verify witness is in our list */
    if (!is_valid_witness(node, att->witnessPubkey)) {
        return -1; /* Unknown witness */
    }

    /* Find matching pending commit */
    for (pc = node->pendingCommits; pc; pc = pc->pNext) {
        if (pc->height == att->height &&
            memcmp(pc->stateRoot, att->stateRoot, HPPLITE_HASH_SIZE) == 0) {
            break;
        }
    }
    if (!pc) return -1; /* No matching pending commit */

    /* Verify signature */
    hpplite_hash_commitment(att->height, att->stateRoot, att->batchRef, msgHash);
    valid = hpplite_crypto_verify(node->crypto, att->witnessPubkey, msgHash, &att->sig);
    if (valid != 1) return -1; /* Invalid signature */

    /* Check for duplicate attestation from same witness */
    HppliteAttestation *existing;
    for (existing = pc->attestations; existing; existing = existing->pNext) {
        if (memcmp(existing->witnessPubkey, att->witnessPubkey, HPPLITE_PUBKEY_SIZE) == 0) {
            return 0; /* Already have attestation from this witness */
        }
    }

    /* Copy attestation and add to list */
    attCopy = sqlite3_malloc(sizeof(HppliteAttestation));
    if (!attCopy) return -1;
    memcpy(attCopy, att, sizeof(HppliteAttestation));
    attCopy->batchRef = node_strdup(att->batchRef);
    attCopy->pNext = pc->attestations;
    pc->attestations = attCopy;
    pc->nAttestations++;

    node->attestationsReceived++;

    return 0;
}

/*
** === Witness Operations ===
*/

/*
** Connect to sequencer (witness only).
*/
int hpplite_node_connect_sequencer(HppliteNode *node, const char *address) {
    if (!node) return -1;
    if (node->role != HPPLITE_ROLE_WITNESS) return -1;

    /* Store sequencer address */
    sqlite3_free(node->config.sequencerAddress);
    node->config.sequencerAddress = node_strdup(address);

    /* TODO: Connect ZeroMQ SUB socket to sequencer PUB socket */
    /*
    int rc = zmq_connect(node->zmqSubscriber, address);
    if (rc != 0) return -1;
    zmq_setsockopt(node->zmqSubscriber, ZMQ_SUBSCRIBE, "", 0);
    */

    return 0;
}

/*
** Verify a batch (witness side)
** In checkpoint mode: verifies but does NOT auto-attest.
** Witness attests only at checkpoint boundaries.
*/
int hpplite_node_verify_batch(HppliteNode *node, HppliteBatch *batch, const char *batchRef) {
    unsigned char computedRoot[HPPLITE_HASH_SIZE];
    int i;
    int rc;
    char *errMsg = NULL;

    (void)batchRef; /* Not used for per-batch attestation in checkpoint mode */

    if (!node || !batch) return -1;
    if (node->role != HPPLITE_ROLE_WITNESS) return -1;

    /* Verify batch height is sequential */
    uint64_t expectedHeight = hpplite_get_block_height(node->ctx);
    if (batch->height != expectedHeight) {
        /* Out of order - need sync */
        node->state = HPPLITE_STATE_SYNCING;
        return -1;
    }

    /* Verify pre-state matches our current state */
    unsigned char currentRoot[HPPLITE_HASH_SIZE];
    hpplite_get_state_root(node->ctx, currentRoot);
    if (memcmp(currentRoot, batch->preStateRoot, HPPLITE_HASH_SIZE) != 0) {
        /* State mismatch - need sync */
        node->state = HPPLITE_STATE_SYNCING;
        return -1;
    }

    /* Track checkpoint window start if this is first batch in window */
    if (node->checkpointFromHeight == 0) {
        node->checkpointFromHeight = batch->height;
        memcpy(node->checkpointPreRoot, batch->preStateRoot, HPPLITE_HASH_SIZE);
    }

    /* Re-execute all transactions in the batch */
    for (i = 0; i < batch->nTxns; i++) {
        rc = hpplite_exec(node->ctx, batch->aTxns[i].zSql, &errMsg);
        if (rc != SQLITE_OK) {
            if (errMsg) {
                sqlite3_free(errMsg);
            }
            /* Execution failed - batch is invalid */
            if (node->onBatchVerified) {
                node->onBatchVerified(node->callbackArg, batch, 0);
            }
            return -1;
        }
    }

    /* Get computed state root */
    hpplite_get_state_root(node->ctx, computedRoot);

    /* Verify computed root matches batch's post-state */
    int valid = (memcmp(computedRoot, batch->postStateRoot, HPPLITE_HASH_SIZE) == 0);

    node->batchesVerified++;

    if (node->onBatchVerified) {
        node->onBatchVerified(node->callbackArg, batch, valid);
    }

    if (valid) {
        /* Update verified state */
        node->lastVerifiedHeight = batch->height;
        memcpy(node->lastVerifiedRoot, computedRoot, HPPLITE_HASH_SIZE);

        /* Finalize the batch verification - increments height and clears pending */
        hpplite_finalize_verified_batch(node->ctx);

        /* NOTE: In checkpoint mode, we do NOT send per-batch attestations.
         * Witness waits for checkpoint request from sequencer, then attests
         * the entire checkpoint range at once. This reduces L1 gas costs.
         */
    }

    return valid ? 0 : -1;
}

/*
** Submit an attestation.
*/
int hpplite_node_submit_attestation(
    HppliteNode *node,
    uint64_t height,
    const unsigned char *stateRoot,
    const char *batchRef
) {
    HppliteAttestation att;
    unsigned char msgHash[HPPLITE_HASH_SIZE];
    int rc;

    if (!node || !stateRoot || !batchRef) return -1;

    /* Build attestation */
    memset(&att, 0, sizeof(att));
    att.height = height;
    memcpy(att.stateRoot, stateRoot, HPPLITE_HASH_SIZE);
    att.batchRef = (char*)batchRef; /* Not owned */
    memcpy(att.witnessPubkey, node->keypair.pubkey, HPPLITE_PUBKEY_SIZE);

    /* Sign commitment */
    hpplite_hash_commitment(height, stateRoot, batchRef, msgHash);
    rc = hpplite_crypto_sign(node->crypto, &node->keypair, msgHash, &att.sig);
    if (rc != 0) return -1;

    node->attestationsSent++;

    /* TODO: Send attestation to sequencer via ZeroMQ */
    /*
    int msgSize;
    unsigned char *msg = hpplite_node_serialize_attestation_msg(node, &att, &msgSize);
    if (msg) {
        zmq_send(node->zmqDealer, msg, msgSize, 0);
        sqlite3_free(msg);
    }
    */

    return 0;
}

/*
** === Query Operations ===
*/

/*
** Get current block height.
*/
uint64_t hpplite_node_get_height(HppliteNode *node) {
    if (!node || !node->ctx) return 0;
    return hpplite_get_block_height(node->ctx);
}

/*
** Get current state root.
*/
void hpplite_node_get_state_root(HppliteNode *node, unsigned char *out) {
    if (!node || !node->ctx || !out) return;
    hpplite_get_state_root(node->ctx, out);
}

/*
** Get node's public key.
*/
void hpplite_node_get_pubkey(HppliteNode *node, unsigned char *out) {
    if (!node || !out) return;
    memcpy(out, node->keypair.pubkey, HPPLITE_PUBKEY_SIZE);
}

/*
** Get node's Ethereum address.
*/
void hpplite_node_get_address(HppliteNode *node, unsigned char *out) {
    if (!node || !out) return;
    memcpy(out, node->keypair.address, HPPLITE_ADDRESS_SIZE);
}

/*
** Get node statistics as JSON string.
*/
char *hpplite_node_get_stats(HppliteNode *node) {
    char pubkeyHex[HPPLITE_PUBKEY_SIZE * 2 + 1];
    char addressHex[HPPLITE_ADDRESS_SIZE * 2 + 1];
    char stateRootHex[HPPLITE_HASH_SIZE * 2 + 1];
    unsigned char stateRoot[HPPLITE_HASH_SIZE];
    const char *roleStr;
    const char *stateStr;

    if (!node) return NULL;

    hpplite_bytes_to_hex(node->keypair.pubkey, HPPLITE_PUBKEY_SIZE, pubkeyHex);
    hpplite_bytes_to_hex(node->keypair.address, HPPLITE_ADDRESS_SIZE, addressHex);

    hpplite_node_get_state_root(node, stateRoot);
    hpplite_bytes_to_hex(stateRoot, HPPLITE_HASH_SIZE, stateRootHex);

    switch (node->role) {
        case HPPLITE_ROLE_SEQUENCER: roleStr = "sequencer"; break;
        case HPPLITE_ROLE_WITNESS: roleStr = "witness"; break;
        default: roleStr = "unknown"; break;
    }

    switch (node->state) {
        case HPPLITE_STATE_INIT: stateStr = "init"; break;
        case HPPLITE_STATE_RUNNING: stateStr = "running"; break;
        case HPPLITE_STATE_SYNCING: stateStr = "syncing"; break;
        case HPPLITE_STATE_STOPPED: stateStr = "stopped"; break;
        case HPPLITE_STATE_ERROR: stateStr = "error"; break;
        default: stateStr = "unknown"; break;
    }

    return sqlite3_mprintf(
        "{"
        "\"nodeId\":\"%s\","
        "\"role\":\"%s\","
        "\"state\":\"%s\","
        "\"pubkey\":\"0x%s\","
        "\"address\":\"0x%s\","
        "\"height\":%llu,"
        "\"stateRoot\":\"0x%s\","
        "\"pendingCommits\":%d,"
        "\"nWitnesses\":%d,"
        "\"batchesProduced\":%llu,"
        "\"batchesVerified\":%llu,"
        "\"attestationsSent\":%llu,"
        "\"attestationsReceived\":%llu"
        "}",
        node->config.nodeId ? node->config.nodeId : "",
        roleStr,
        stateStr,
        pubkeyHex,
        addressHex,
        (unsigned long long)hpplite_node_get_height(node),
        stateRootHex,
        node->nPendingCommits,
        node->nWitnesses,
        (unsigned long long)node->batchesProduced,
        (unsigned long long)node->batchesVerified,
        (unsigned long long)node->attestationsSent,
        (unsigned long long)node->attestationsReceived
    );
}

/*
** === Message Serialization ===
*/

/*
** Serialize a batch message for network transmission.
** Format: [1 byte type][8 bytes height][batch JSON][batchRef]
*/
unsigned char *hpplite_node_serialize_batch_msg(
    HppliteNode *node,
    HppliteBatch *batch,
    const char *batchRef,
    int *outSize
) {
    char *batchJson;
    int batchJsonLen, batchRefLen, totalSize;
    unsigned char *msg;
    int offset = 0;

    (void)node; /* Not used for now */

    if (!batch || !batchRef || !outSize) return NULL;

    batchJson = hpplite_batch_to_json(batch);
    if (!batchJson) return NULL;

    batchJsonLen = (int)strlen(batchJson);
    batchRefLen = (int)strlen(batchRef);

    /* Format: type(1) + height(8) + jsonLen(4) + json + refLen(4) + ref */
    totalSize = 1 + 8 + 4 + batchJsonLen + 4 + batchRefLen;

    msg = sqlite3_malloc(totalSize);
    if (!msg) {
        sqlite3_free(batchJson);
        return NULL;
    }

    /* Type */
    msg[offset++] = HPPLITE_MSG_BATCH;

    /* Height (big-endian) */
    msg[offset++] = (batch->height >> 56) & 0xFF;
    msg[offset++] = (batch->height >> 48) & 0xFF;
    msg[offset++] = (batch->height >> 40) & 0xFF;
    msg[offset++] = (batch->height >> 32) & 0xFF;
    msg[offset++] = (batch->height >> 24) & 0xFF;
    msg[offset++] = (batch->height >> 16) & 0xFF;
    msg[offset++] = (batch->height >> 8) & 0xFF;
    msg[offset++] = batch->height & 0xFF;

    /* JSON length (big-endian) */
    msg[offset++] = (batchJsonLen >> 24) & 0xFF;
    msg[offset++] = (batchJsonLen >> 16) & 0xFF;
    msg[offset++] = (batchJsonLen >> 8) & 0xFF;
    msg[offset++] = batchJsonLen & 0xFF;

    /* JSON */
    memcpy(msg + offset, batchJson, batchJsonLen);
    offset += batchJsonLen;

    /* Ref length (big-endian) */
    msg[offset++] = (batchRefLen >> 24) & 0xFF;
    msg[offset++] = (batchRefLen >> 16) & 0xFF;
    msg[offset++] = (batchRefLen >> 8) & 0xFF;
    msg[offset++] = batchRefLen & 0xFF;

    /* Ref */
    memcpy(msg + offset, batchRef, batchRefLen);

    sqlite3_free(batchJson);
    *outSize = totalSize;
    return msg;
}

/*
** Parse a batch message from network.
*/
HppliteBatch *hpplite_node_parse_batch_msg(
    const unsigned char *data,
    int size,
    char **batchRef
) {
    int offset = 0;
    uint64_t height;
    int jsonLen, refLen;
    char *jsonStr;
    HppliteBatch *batch;

    if (!data || size < 17 || !batchRef) return NULL;

    /* Check type */
    if (data[offset++] != HPPLITE_MSG_BATCH) return NULL;

    /* Parse height */
    height = ((uint64_t)data[offset] << 56) | ((uint64_t)data[offset+1] << 48) |
             ((uint64_t)data[offset+2] << 40) | ((uint64_t)data[offset+3] << 32) |
             ((uint64_t)data[offset+4] << 24) | ((uint64_t)data[offset+5] << 16) |
             ((uint64_t)data[offset+6] << 8) | data[offset+7];
    offset += 8;

    /* Parse JSON length */
    jsonLen = ((int)data[offset] << 24) | ((int)data[offset+1] << 16) |
              ((int)data[offset+2] << 8) | data[offset+3];
    offset += 4;

    if (offset + jsonLen + 4 > size) return NULL;

    /* Parse JSON */
    jsonStr = sqlite3_malloc(jsonLen + 1);
    if (!jsonStr) return NULL;
    memcpy(jsonStr, data + offset, jsonLen);
    jsonStr[jsonLen] = '\0';
    offset += jsonLen;

    batch = hpplite_batch_from_json(jsonStr);
    sqlite3_free(jsonStr);

    if (!batch) return NULL;

    /* Verify height matches */
    if (batch->height != height) {
        hpplite_batch_free(batch);
        return NULL;
    }

    /* Parse ref length */
    refLen = ((int)data[offset] << 24) | ((int)data[offset+1] << 16) |
             ((int)data[offset+2] << 8) | data[offset+3];
    offset += 4;

    if (offset + refLen > size) {
        hpplite_batch_free(batch);
        return NULL;
    }

    /* Parse ref */
    *batchRef = sqlite3_malloc(refLen + 1);
    if (!*batchRef) {
        hpplite_batch_free(batch);
        return NULL;
    }
    memcpy(*batchRef, data + offset, refLen);
    (*batchRef)[refLen] = '\0';

    return batch;
}

/*
** Serialize an attestation message.
** Format: type(1) + height(8) + stateRoot(32) + pubkey(33) + sig(64) + recid(1) + refLen(4) + ref
*/
unsigned char *hpplite_node_serialize_attestation_msg(
    HppliteNode *node,
    const HppliteAttestation *att,
    int *outSize
) {
    int refLen, totalSize;
    unsigned char *msg;
    int offset = 0;

    (void)node; /* Not used for now */

    if (!att || !att->batchRef || !outSize) return NULL;

    refLen = (int)strlen(att->batchRef);
    totalSize = 1 + 8 + 32 + 33 + 64 + 1 + 4 + refLen;

    msg = sqlite3_malloc(totalSize);
    if (!msg) return NULL;

    /* Type */
    msg[offset++] = HPPLITE_MSG_ATTESTATION;

    /* Height (big-endian) */
    msg[offset++] = (att->height >> 56) & 0xFF;
    msg[offset++] = (att->height >> 48) & 0xFF;
    msg[offset++] = (att->height >> 40) & 0xFF;
    msg[offset++] = (att->height >> 32) & 0xFF;
    msg[offset++] = (att->height >> 24) & 0xFF;
    msg[offset++] = (att->height >> 16) & 0xFF;
    msg[offset++] = (att->height >> 8) & 0xFF;
    msg[offset++] = att->height & 0xFF;

    /* State root */
    memcpy(msg + offset, att->stateRoot, 32);
    offset += 32;

    /* Witness pubkey */
    memcpy(msg + offset, att->witnessPubkey, 33);
    offset += 33;

    /* Signature */
    memcpy(msg + offset, att->sig.sig, 64);
    offset += 64;

    /* Recovery ID */
    msg[offset++] = (unsigned char)att->sig.recid;

    /* Ref length */
    msg[offset++] = (refLen >> 24) & 0xFF;
    msg[offset++] = (refLen >> 16) & 0xFF;
    msg[offset++] = (refLen >> 8) & 0xFF;
    msg[offset++] = refLen & 0xFF;

    /* Ref */
    memcpy(msg + offset, att->batchRef, refLen);

    *outSize = totalSize;
    return msg;
}

/*
** Parse an attestation message.
*/
HppliteAttestation *hpplite_node_parse_attestation_msg(
    const unsigned char *data,
    int size
) {
    HppliteAttestation *att;
    int offset = 0;
    int refLen;

    if (!data || size < 143) return NULL; /* Minimum: 1+8+32+33+64+1+4 = 143 */

    /* Check type */
    if (data[offset++] != HPPLITE_MSG_ATTESTATION) return NULL;

    att = sqlite3_malloc(sizeof(HppliteAttestation));
    if (!att) return NULL;
    memset(att, 0, sizeof(HppliteAttestation));

    /* Height */
    att->height = ((uint64_t)data[offset] << 56) | ((uint64_t)data[offset+1] << 48) |
                  ((uint64_t)data[offset+2] << 40) | ((uint64_t)data[offset+3] << 32) |
                  ((uint64_t)data[offset+4] << 24) | ((uint64_t)data[offset+5] << 16) |
                  ((uint64_t)data[offset+6] << 8) | data[offset+7];
    offset += 8;

    /* State root */
    memcpy(att->stateRoot, data + offset, 32);
    offset += 32;

    /* Witness pubkey */
    memcpy(att->witnessPubkey, data + offset, 33);
    offset += 33;

    /* Signature */
    memcpy(att->sig.sig, data + offset, 64);
    offset += 64;

    /* Recovery ID */
    att->sig.recid = data[offset++];

    /* Ref length */
    refLen = ((int)data[offset] << 24) | ((int)data[offset+1] << 16) |
             ((int)data[offset+2] << 8) | data[offset+3];
    offset += 4;

    if (offset + refLen > size) {
        sqlite3_free(att);
        return NULL;
    }

    /* Ref */
    att->batchRef = sqlite3_malloc(refLen + 1);
    if (!att->batchRef) {
        sqlite3_free(att);
        return NULL;
    }
    memcpy(att->batchRef, data + offset, refLen);
    att->batchRef[refLen] = '\0';

    return att;
}

/*
** === Checkpoint Operations ===
*/

/*
** Set the L1 connection for this node.
** For M1: all nodes share the same L1 mock pointer.
*/
void hpplite_node_set_l1(HppliteNode *node, HppliteL1 *l1) {
    if (!node) return;
    node->l1 = l1;
}

/*
** Sync role from L1 contract state.
** Updates node role based on current L1 sequencer.
*/
HppliteNodeRole hpplite_node_sync_role_from_l1(HppliteNode *node) {
    if (!node || !node->l1) return node ? node->role : HPPLITE_ROLE_UNKNOWN;

    /* Check if we are the sequencer */
    if (hpplite_l1_is_sequencer(node->l1, node->keypair.pubkey)) {
        if (node->role != HPPLITE_ROLE_SEQUENCER) {
            hpplite_node_set_role(node, HPPLITE_ROLE_SEQUENCER);
        }
        return HPPLITE_ROLE_SEQUENCER;
    }

    /* Check if we are a registered witness */
    if (hpplite_l1_is_witness(node->l1, node->keypair.pubkey)) {
        if (node->role != HPPLITE_ROLE_WITNESS) {
            hpplite_node_set_role(node, HPPLITE_ROLE_WITNESS);
        }
        return HPPLITE_ROLE_WITNESS;
    }

    return HPPLITE_ROLE_UNKNOWN;
}

/*
** Check if node should create a checkpoint.
*/
int hpplite_node_should_checkpoint(HppliteNode *node) {
    if (!node) return 0;
    if (node->role != HPPLITE_ROLE_SEQUENCER) return 0;

    return (node->batchesSinceCheckpoint >= node->config.checkpointInterval);
}

/*
** Create a checkpoint proposal (sequencer only).
*/
HppliteCheckpoint *hpplite_node_create_checkpoint(HppliteNode *node) {
    HppliteCheckpoint *cp;
    unsigned char currentRoot[HPPLITE_HASH_SIZE];

    if (!node) return NULL;
    if (node->role != HPPLITE_ROLE_SEQUENCER) return NULL;
    if (node->batchesSinceCheckpoint == 0) return NULL;

    /* Get current state root */
    hpplite_get_state_root(node->ctx, currentRoot);

    /* Create checkpoint covering all batches since last checkpoint */
    cp = hpplite_checkpoint_new(
        node->lastCheckpointHeight + 1,  /* fromHeight */
        hpplite_get_block_height(node->ctx) - 1  /* toHeight (current height - 1) */
    );
    if (!cp) return NULL;

    /* Fill in checkpoint data */
    memcpy(cp->preStateRoot, node->lastCheckpointRoot, HPPLITE_HASH_SIZE);
    memcpy(cp->postStateRoot, currentRoot, HPPLITE_HASH_SIZE);
    /* batchesHash would be computed from all batches in range - simplified for now */
    memset(cp->batchesHash, 0, HPPLITE_HASH_SIZE);
    cp->timestamp = (uint64_t)time(NULL);

    /* Store as pending checkpoint */
    if (node->pendingCheckpoint) {
        hpplite_checkpoint_free(node->pendingCheckpoint);
    }
    node->pendingCheckpoint = cp;

    /* Clear existing attestations */
    if (node->cpAttestations) {
        sqlite3_free(node->cpAttestations);
        node->cpAttestations = NULL;
    }
    node->nCpAttestations = 0;

    return cp;
}

/*
** Free checkpoint attestations helper
*/
static void free_cp_attestations(HppliteNode *node) {
    if (node->cpAttestations) {
        sqlite3_free(node->cpAttestations);
        node->cpAttestations = NULL;
    }
    node->nCpAttestations = 0;
}

/*
** Receive a checkpoint attestation from a witness (sequencer side).
*/
int hpplite_node_receive_checkpoint_attestation(
    HppliteNode *node,
    const HppliteCheckpointAttestation *att
) {
    unsigned char cpHash[HPPLITE_HASH_SIZE];
    HppliteSignature sig;
    HppliteCheckpointAttestation *newList;
    int i;

    if (!node || !att) return -1;
    if (node->role != HPPLITE_ROLE_SEQUENCER) return -1;
    if (!node->pendingCheckpoint) return -1;

    /* Verify witness is registered */
    if (!is_valid_witness(node, att->witnessPubkey)) {
        return -1;  /* Unknown witness */
    }

    /* Verify attestation matches pending checkpoint */
    if (att->fromHeight != node->pendingCheckpoint->fromHeight ||
        att->toHeight != node->pendingCheckpoint->toHeight) {
        return -1;  /* Height mismatch */
    }

    if (memcmp(att->postStateRoot, node->pendingCheckpoint->postStateRoot, HPPLITE_HASH_SIZE) != 0) {
        return -1;  /* State root mismatch */
    }

    /* Verify signature */
    hpplite_checkpoint_hash(node->pendingCheckpoint, cpHash);
    memcpy(sig.sig, att->signature, 64);
    sig.recid = att->recid;

    if (hpplite_crypto_verify(node->crypto, att->witnessPubkey, cpHash, &sig) != 1) {
        return -1;  /* Invalid signature */
    }

    /* Check for duplicate */
    for (i = 0; i < node->nCpAttestations; i++) {
        if (memcmp(node->cpAttestations[i].witnessPubkey, att->witnessPubkey, HPPLITE_PUBKEY_SIZE) == 0) {
            return 0;  /* Already have attestation from this witness */
        }
    }

    /* Add to attestations list */
    newList = sqlite3_realloc(node->cpAttestations,
        (node->nCpAttestations + 1) * sizeof(HppliteCheckpointAttestation));
    if (!newList) return -1;

    node->cpAttestations = newList;
    memcpy(&node->cpAttestations[node->nCpAttestations], att, sizeof(HppliteCheckpointAttestation));
    node->nCpAttestations++;

    node->attestationsReceived++;

    return 0;
}

/*
** Submit pending checkpoint to L1 (sequencer only).
*/
char *hpplite_node_submit_checkpoint(HppliteNode *node) {
    char *txHash;
    int requiredAtts;

    if (!node || !node->l1) return NULL;
    if (node->role != HPPLITE_ROLE_SEQUENCER) return NULL;
    if (!node->pendingCheckpoint) return NULL;

    /* Get required attestations from L1 state or config */
    HppliteL1State *l1State = hpplite_l1_get_state(node->l1);
    if (l1State) {
        requiredAtts = l1State->requiredAttestations;
        hpplite_l1_state_free(l1State);
    } else {
        requiredAtts = node->config.requiredAttestations;
    }

    /* Check if we have enough attestations */
    if (node->nCpAttestations < requiredAtts) {
        return NULL;  /* Not enough attestations */
    }

    /* Submit to L1 */
    txHash = hpplite_l1_submit_checkpoint(
        node->l1,
        node->pendingCheckpoint,
        node->cpAttestations,
        node->nCpAttestations
    );

    if (txHash) {
        /* Update checkpoint tracking */
        node->lastCheckpointHeight = node->pendingCheckpoint->toHeight;
        memcpy(node->lastCheckpointRoot, node->pendingCheckpoint->postStateRoot, HPPLITE_HASH_SIZE);
        node->batchesSinceCheckpoint = 0;

        /* Notify callback */
        if (node->onCheckpointSubmitted) {
            node->onCheckpointSubmitted(node->callbackArg, node->pendingCheckpoint, txHash);
        }

        /* Clean up pending checkpoint */
        hpplite_checkpoint_free(node->pendingCheckpoint);
        node->pendingCheckpoint = NULL;
        free_cp_attestations(node);
    }

    return txHash;
}

/*
** Sign and submit checkpoint attestation (witness side).
*/
int hpplite_node_attest_checkpoint(
    HppliteNode *node,
    const HppliteCheckpoint *cp
) {
    HppliteCheckpointAttestation att;
    unsigned char cpHash[HPPLITE_HASH_SIZE];
    HppliteSignature sig;
    unsigned char myRoot[HPPLITE_HASH_SIZE];
    int rc;

    if (!node || !cp) return -1;
    if (node->role != HPPLITE_ROLE_WITNESS) return -1;

    /* Verify checkpoint matches our verified state */
    if (cp->fromHeight != node->checkpointFromHeight) {
        return -1;  /* Checkpoint start doesn't match our window */
    }

    if (memcmp(cp->preStateRoot, node->checkpointPreRoot, HPPLITE_HASH_SIZE) != 0) {
        return -1;  /* Pre-state doesn't match */
    }

    /* Verify we've verified up to the checkpoint end */
    if (node->lastVerifiedHeight < cp->toHeight) {
        return -1;  /* Haven't verified all batches yet */
    }

    /* Verify post-state matches our computed state */
    hpplite_get_state_root(node->ctx, myRoot);
    if (memcmp(cp->postStateRoot, myRoot, HPPLITE_HASH_SIZE) != 0) {
        return -1;  /* State root mismatch - disagree with checkpoint */
    }

    /* Build attestation */
    memset(&att, 0, sizeof(att));
    att.fromHeight = cp->fromHeight;
    att.toHeight = cp->toHeight;
    memcpy(att.postStateRoot, cp->postStateRoot, HPPLITE_HASH_SIZE);
    memcpy(att.witnessPubkey, node->keypair.pubkey, HPPLITE_PUBKEY_SIZE);

    /* Sign checkpoint hash */
    hpplite_checkpoint_hash(cp, cpHash);
    rc = hpplite_crypto_sign(node->crypto, &node->keypair, cpHash, &sig);
    if (rc != 0) return -1;

    memcpy(att.signature, sig.sig, 64);
    att.recid = sig.recid;

    node->attestationsSent++;

    /* Reset checkpoint window for next checkpoint */
    node->checkpointFromHeight = 0;
    memset(node->checkpointPreRoot, 0, HPPLITE_HASH_SIZE);

    /* TODO: Send attestation to sequencer via ZeroMQ */
    /*
    int msgSize;
    unsigned char *msg = hpplite_node_serialize_cp_attestation_msg(node, &att, &msgSize);
    if (msg) {
        zmq_send(node->zmqDealer, msg, msgSize, 0);
        sqlite3_free(msg);
    }
    */

    return 0;
}
