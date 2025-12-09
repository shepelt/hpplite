/*
** HPPLite Node - Singleton Sequencer Implementation
**
** Single sequencer model - all writes go through sequencer.
** L1 contract handles coordination (lease/lock mechanism).
** No peer-to-peer networking - all data flows through L1.
*/

#include "node.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>

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
** Helper: get current time in milliseconds
*/
static int64_t node_current_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/*
** Generate a unique instance ID for this process.
** Combines: timestamp + pid + random bytes
*/
static void generate_instance_id(unsigned char *out) {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    /* First 8 bytes: timestamp */
    uint64_t ts = (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    for (int i = 0; i < 8; i++) {
        out[i] = (ts >> (56 - i * 8)) & 0xFF;
    }

    /* Next 4 bytes: PID */
    pid_t pid = getpid();
    out[8] = (pid >> 24) & 0xFF;
    out[9] = (pid >> 16) & 0xFF;
    out[10] = (pid >> 8) & 0xFF;
    out[11] = pid & 0xFF;

    /* Remaining 20 bytes: random */
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (urandom) {
        fread(out + 12, 1, 20, urandom);
        fclose(urandom);
    } else {
        /* Fallback: use more timestamp bits */
        for (int i = 12; i < 32; i++) {
            out[i] = (unsigned char)(rand() ^ (tv.tv_usec >> (i % 20)));
        }
    }
}

/*
** Initialize common node fields from config.
*/
static int node_init_common(HppliteNode *node, const HppliteNodeConfig *config) {
    int rc;

    /* Copy configuration */
    node->config.nodeId = node_strdup(config->nodeId);
    memcpy(node->config.privkey, config->privkey, 32);
    node->config.hasPrivkey = config->hasPrivkey;
    node->config.dataDir = node_strdup(config->dataDir);
    node->config.dbPath = node_strdup(config->dbPath);
    node->config.rpcUrl = node_strdup(config->rpcUrl);
    memcpy(node->config.factory, config->factory, 20);
    node->config.hasFactory = config->hasFactory;
    memcpy(node->config.contract, config->contract, 20);
    node->config.hasContract = config->hasContract;
    node->config.checkpointInterval = config->checkpointInterval > 0 ? config->checkpointInterval : 100;
    node->config.batchIntervalMs = config->batchIntervalMs;

    /* Generate unique instance ID */
    generate_instance_id(node->instanceId);

    /* Initialize state */
    node->state = HPPLITE_STATE_INIT;

    /* Initialize crypto */
    node->crypto = hpplite_crypto_init();
    if (!node->crypto) {
        return -1;
    }

    /* Initialize keypair from private key */
    if (config->hasPrivkey) {
        rc = hpplite_crypto_keypair_from_privkey(node->crypto, &node->keypair, config->privkey);
        if (rc != 0) {
            return -1;
        }
    }

    return 0;
}

/*
** Auto-connect to L1 and sync state.
*/
static void node_auto_connect_l1(HppliteNode *node, const HppliteNodeConfig *config) {
    if (!config->rpcUrl) return;
    if (!config->hasContract && !config->hasFactory) return;

    if (config->hasFactory && config->hasPrivkey) {
        /* Use factory to find/create rollup */
        char factoryHex[43];
        snprintf(factoryHex, sizeof(factoryHex), "0x");
        for (int i = 0; i < 20; i++) {
            snprintf(factoryHex + 2 + i*2, 3, "%02x", config->factory[i]);
        }

        /* Check if rollup exists */
        unsigned char rollup[20];
        int result = hpplite_l1_factory_get_my_rollup(
            config->rpcUrl, factoryHex, config->privkey, rollup);

        if (result == 0) {
            /* No rollup exists - create one */
            char *txHash = hpplite_l1_factory_get_or_create_rollup(
                config->rpcUrl, factoryHex, config->privkey, rollup);

            if (txHash) {
                /* Wait for transaction to be mined */
                HppliteL1 *tempL1 = hpplite_l1_connect(config->rpcUrl, factoryHex);
                if (tempL1) {
                    hpplite_l1_wait_for_tx(tempL1, txHash, 60);
                    hpplite_l1_disconnect(tempL1);
                }
                free(txHash);

                /* Try again to get rollup address */
                result = hpplite_l1_factory_get_my_rollup(
                    config->rpcUrl, factoryHex, config->privkey, rollup);
            }
        }

        if (result == 1) {
            /* Connect to the rollup */
            node->l1 = hpplite_l1_connect_factory(
                config->rpcUrl, factoryHex, config->privkey);
            node->ownsL1 = (node->l1 != NULL);
        }
    } else if (config->hasContract) {
        /* Direct contract connection */
        char contractHex[43];
        snprintf(contractHex, sizeof(contractHex), "0x");
        for (int i = 0; i < 20; i++) {
            snprintf(contractHex + 2 + i*2, 3, "%02x", config->contract[i]);
        }
        node->l1 = hpplite_l1_connect(config->rpcUrl, contractHex);
        node->ownsL1 = (node->l1 != NULL);

        if (node->l1 && config->hasPrivkey) {
            hpplite_l1_set_privkey(node->l1, config->privkey);
        }
    }

    /* Sync existing batches from L1 */
    if (node->l1) {
        uint64_t lastBatch = 0, totalBatches = 0;
        unsigned char latestHash[32];
        if (hpplite_l1_get_da_state(node->l1, &lastBatch, &totalBatches, latestHash) == 0
            && totalBatches > 0) {
            /* Replay batches from L1 */
            for (uint64_t h = 1; h <= lastBatch; h++) {
                size_t dataLen = 0;
                uint8_t *data = hpplite_l1_get_batch(node->l1, h, &dataLen);
                if (data && dataLen > 0) {
                    HppliteBatch *batch = hpplite_batch_from_json((const char *)data);
                    if (batch) {
                        for (int i = 0; i < batch->nTxns; i++) {
                            if (batch->aTxns[i].zSql) {
                                sqlite3_exec(node->db, batch->aTxns[i].zSql, NULL, NULL, NULL);
                            }
                        }
                        node->ctx->blockHeight = batch->height + 1;
                        node->lastCheckpointHeight = batch->height;
                        memcpy(node->lastCheckpointRoot, batch->postStateRoot, HPPLITE_HASH_SIZE);
                        memcpy(node->lastFlushedRoot, batch->postStateRoot, HPPLITE_HASH_SIZE);
                        hpplite_batch_free(batch);
                    }
                    free(data);
                }
            }
        }
    }
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

    /* Initialize common fields */
    if (node_init_common(node, config) != 0) {
        hpplite_node_destroy(node);
        return NULL;
    }

    /* Open database */
    rc = sqlite3_open(config->dbPath, &node->db);
    if (rc != SQLITE_OK) {
        hpplite_node_destroy(node);
        return NULL;
    }
    node->ownsDb = 1;

    /* Initialize HPPLite context */
    rc = hpplite_init(node->db, &node->ctx);
    if (rc != SQLITE_OK) {
        hpplite_node_destroy(node);
        return NULL;
    }

    /* Auto-connect to L1 */
    node_auto_connect_l1(node, config);

    /* Register node with global registry */
    hpplite_register_node(node->db, node);

    return node;
}

/*
** Initialize a node using an existing database connection.
*/
HppliteNode *hpplite_node_create_with_db(sqlite3 *db, const HppliteNodeConfig *config) {
    HppliteNode *node;
    int rc;

    if (!db || !config) return NULL;

    node = sqlite3_malloc(sizeof(HppliteNode));
    if (!node) return NULL;
    memset(node, 0, sizeof(HppliteNode));

    /* Use existing db, don't own it */
    node->db = db;
    node->ownsDb = 0;

    /* Initialize common fields */
    if (node_init_common(node, config) != 0) {
        hpplite_node_destroy(node);
        return NULL;
    }

    /* Initialize HPPLite context */
    rc = hpplite_init(db, &node->ctx);
    if (rc != SQLITE_OK) {
        hpplite_node_destroy(node);
        return NULL;
    }

    /* Initialize timer fields */
    node->lastFlushTime = node_current_time_ms();
    hpplite_get_state_root(node->ctx, node->lastFlushedRoot);

    /* Auto-connect to L1 */
    node_auto_connect_l1(node, config);

    /* Register node with global registry */
    hpplite_register_node(node->db, node);

    return node;
}

/*
** Free a node and all resources.
*/
void hpplite_node_destroy(HppliteNode *node) {
    if (!node) return;

    /* Stop timer thread first */
    hpplite_node_stop_timer(node);

    /* Flush any pending data before cleanup */
    if (node->ctx) {
        int pending = hpplite_pending_count(node->ctx);
        if (pending > 0) {
            hpplite_node_flush_batch(node);
        }
    }

    /* Stop if running */
    if (node->state == HPPLITE_STATE_RUNNING) {
        hpplite_node_stop(node);
    }

    /* Disconnect L1 only if we own it */
    if (node->l1 && node->ownsL1) {
        hpplite_l1_disconnect(node->l1);
    }

    /* Shutdown HPPLite context */
    if (node->ctx) {
        hpplite_shutdown(node->ctx);
    }

    /* Close database only if we own it */
    if (node->db && node->ownsDb) {
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
    sqlite3_free(node->config.rpcUrl);

    sqlite3_free(node);
}

/*
** Internal flush for timer thread - creates batch and posts to L1.
** Must be called with mutex held.
*/
static uint64_t node_do_flush_locked(HppliteNode *node) {
    if (!node || !node->ctx) return 0;

    /* Create batch from pending SQL */
    HppliteBatch *batch = hpplite_flush_block(node->ctx);
    if (!batch) return 0;

    uint64_t height = batch->height;

    /* Post batch to L1 for data availability */
    if (node->l1) {
        /* Ensure we have an active lease before submitting */
        if (!hpplite_l1_is_lease_active(node->l1)) {
            hpplite_node_claim_lease(node);
        }

        char *json = hpplite_batch_to_json(batch);
        if (json) {
            char *txHash = hpplite_l1_submit_batch(node->l1, node->instanceId, height,
                                    (const uint8_t *)json, strlen(json));
            if (txHash) {
                hpplite_l1_wait_for_tx(node->l1, txHash, 30);

                /* Notify callback */
                if (node->onBatchPosted) {
                    node->onBatchPosted(node->callbackArg, height, txHash);
                }
                free(txHash);
            }
            sqlite3_free(json);
        }
    }

    /* Update state */
    memcpy(node->lastFlushedRoot, batch->postStateRoot, HPPLITE_HASH_SIZE);
    node->lastFlushTime = node_current_time_ms();
    node->batchesProduced++;
    node->batchesSinceCheckpoint++;

    hpplite_batch_free(batch);
    return height;
}

/*
** Timer thread function - auto-flush batches on interval.
*/
static void *node_timer_thread_func(void *arg) {
    HppliteNode *node = (HppliteNode *)arg;

    if (!node) return NULL;

    int intervalMs = node->config.batchIntervalMs;
    if (intervalMs <= 0) return NULL;

    /* Sleep in small increments for quick shutdown */
    int sleepMs = (intervalMs < 100) ? intervalMs : 100;

    while (node->timerRunning) {
        usleep(sleepMs * 1000);

        if (!node->timerRunning) break;

        /* Check if time to flush */
        int64_t now = node_current_time_ms();
        int64_t elapsed = now - node->lastFlushTime;

        if (elapsed >= intervalMs) {
            pthread_mutex_t *mutex = (pthread_mutex_t *)node->flushMutex;
            if (mutex) {
                pthread_mutex_lock(mutex);
                if (node->timerRunning) {
                    node_do_flush_locked(node);
                }
                pthread_mutex_unlock(mutex);
            }
        }
    }

    return NULL;
}

/*
** Start auto-flush timer thread.
*/
void hpplite_node_start_timer(HppliteNode *node) {
    if (!node) return;
    if (node->config.batchIntervalMs <= 0) return;
    if (node->timerRunning) return;

    /* Allocate mutex */
    pthread_mutex_t *mutex = sqlite3_malloc(sizeof(pthread_mutex_t));
    if (!mutex) return;
    pthread_mutex_init(mutex, NULL);
    node->flushMutex = mutex;

    /* Start thread */
    pthread_t *thread = sqlite3_malloc(sizeof(pthread_t));
    if (!thread) {
        pthread_mutex_destroy(mutex);
        sqlite3_free(mutex);
        node->flushMutex = NULL;
        return;
    }

    node->timerRunning = 1;
    pthread_create(thread, NULL, node_timer_thread_func, node);
    node->timerThread = thread;
}

/*
** Stop auto-flush timer thread.
*/
void hpplite_node_stop_timer(HppliteNode *node) {
    if (!node || !node->timerRunning) return;

    /* Signal thread to stop */
    node->timerRunning = 0;

    /* Wait for thread to finish */
    pthread_t *thread = (pthread_t *)node->timerThread;
    if (thread) {
        pthread_join(*thread, NULL);
        sqlite3_free(thread);
        node->timerThread = NULL;
    }

    /* Destroy mutex */
    pthread_mutex_t *mutex = (pthread_mutex_t *)node->flushMutex;
    if (mutex) {
        pthread_mutex_destroy(mutex);
        sqlite3_free(mutex);
        node->flushMutex = NULL;
    }
}

/*
** Claim sequencer lease on L1.
** Returns 0 on success, -1 if lease unavailable.
*/
int hpplite_node_claim_lease(HppliteNode *node) {
    if (!node || !node->l1) return -1;

    /* Claim sequencer lease with our instance ID */
    char *txHash = hpplite_l1_claim_sequencer(node->l1, node->instanceId);
    if (!txHash) {
        return -1;  /* Lease not available */
    }
    sqlite3_free(txHash);
    return 0;
}

/*
** Renew sequencer lease on L1.
*/
int hpplite_node_renew_lease(HppliteNode *node) {
    if (!node || !node->l1) return -1;

    /* Renew lease with our instance ID */
    char *txHash = hpplite_l1_renew_lease(node->l1, node->instanceId);
    if (!txHash) {
        return -1;  /* Renewal failed */
    }
    sqlite3_free(txHash);
    return 0;
}

/*
** Check if our lease is still active.
*/
int hpplite_node_has_lease(HppliteNode *node) {
    if (!node || !node->l1) return 0;

    /* Check if any lease is active on L1 */
    return hpplite_l1_is_lease_active(node->l1);
}

/*
** Start the node.
*/
int hpplite_node_start(HppliteNode *node) {
    if (!node) return -1;
    if (node->state == HPPLITE_STATE_RUNNING) return 0;

    /* Try to claim sequencer lease */
    if (node->l1) {
        if (hpplite_node_claim_lease(node) != 0) {
            return -1;  /* Failed to claim lease */
        }
    }

    node->state = HPPLITE_STATE_RUNNING;
    return 0;
}

/*
** Stop the node.
*/
void hpplite_node_stop(HppliteNode *node) {
    if (!node) return;
    if (node->state != HPPLITE_STATE_RUNNING) return;

    node->state = HPPLITE_STATE_STOPPED;
}

/*
** Execute SQL (queued for next batch).
*/
int hpplite_node_exec(HppliteNode *node, const char *sql, char **errMsg) {
    if (!node || !sql) return -1;
    return hpplite_exec(node->ctx, sql, errMsg);
}

/*
** Flush pending SQL to create a batch and post to L1.
*/
uint64_t hpplite_node_flush_batch(HppliteNode *node) {
    HppliteBatch *batch;

    if (!node) return 0;

    /* Create batch from pending SQL */
    batch = hpplite_flush_block(node->ctx);
    if (!batch) return 0;

    uint64_t height = batch->height;

    /* Post batch to L1 for data availability */
    if (node->l1) {
        /* Ensure we have an active lease before submitting */
        if (!hpplite_l1_is_lease_active(node->l1)) {
            hpplite_node_claim_lease(node);
        }

        char *json = hpplite_batch_to_json(batch);
        if (json) {
            char *txHash = hpplite_l1_submit_batch(node->l1, node->instanceId, height,
                                    (const uint8_t *)json, strlen(json));
            if (txHash) {
                hpplite_l1_wait_for_tx(node->l1, txHash, 30);

                /* Notify callback */
                if (node->onBatchPosted) {
                    node->onBatchPosted(node->callbackArg, height, txHash);
                }
                free(txHash);
            }
            sqlite3_free(json);
        }
    }

    /* Update state */
    node->batchesProduced++;
    node->batchesSinceCheckpoint++;

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
** Check if node should create a checkpoint.
*/
int hpplite_node_should_checkpoint(HppliteNode *node) {
    if (!node) return 0;
    return (node->batchesSinceCheckpoint >= node->config.checkpointInterval);
}

/*
** Set the L1 connection for this node.
*/
void hpplite_node_set_l1(HppliteNode *node, HppliteL1 *l1) {
    if (!node) return;
    node->l1 = l1;
    node->ownsL1 = 0;  /* Externally provided - we don't own it */

    /* Pass our private key to L1 for signing transactions */
    if (node->config.hasPrivkey) {
        hpplite_l1_set_privkey(l1, node->config.privkey);
    }
}

/*
** Create and submit checkpoint to L1.
*/
char *hpplite_node_submit_checkpoint(HppliteNode *node) {
    if (!node || !node->l1) return NULL;
    if (node->batchesSinceCheckpoint == 0) return NULL;

    /* Ensure we have an active lease */
    if (!hpplite_l1_is_lease_active(node->l1)) {
        if (hpplite_node_claim_lease(node) != 0) {
            return NULL;  /* Could not claim lease */
        }
    }

    /* Get current state */
    unsigned char currentRoot[HPPLITE_HASH_SIZE];
    hpplite_get_state_root(node->ctx, currentRoot);
    uint64_t currentHeight = hpplite_get_block_height(node->ctx) - 1;

    /* Create checkpoint */
    HppliteCheckpoint *cp = hpplite_checkpoint_new(
        node->lastCheckpointHeight + 1,
        currentHeight
    );
    if (!cp) return NULL;

    memcpy(cp->preStateRoot, node->lastCheckpointRoot, HPPLITE_HASH_SIZE);
    memcpy(cp->postStateRoot, currentRoot, HPPLITE_HASH_SIZE);
    cp->timestamp = (uint64_t)time(NULL);

    /* Submit to L1 with our instance ID */
    char *txHash = hpplite_l1_submit_checkpoint(node->l1, cp, node->instanceId);

    if (txHash) {
        /* Update checkpoint tracking */
        node->lastCheckpointHeight = cp->toHeight;
        memcpy(node->lastCheckpointRoot, cp->postStateRoot, HPPLITE_HASH_SIZE);
        node->batchesSinceCheckpoint = 0;

        /* Notify callback */
        if (node->onCheckpointSubmitted) {
            node->onCheckpointSubmitted(node->callbackArg, cp->fromHeight, cp->toHeight, txHash);
        }
    }

    hpplite_checkpoint_free(cp);
    return txHash;
}

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
** Get node's instance ID.
*/
void hpplite_node_get_instance_id(HppliteNode *node, unsigned char *out) {
    if (!node || !out) return;
    memcpy(out, node->instanceId, 32);
}

/*
** Get node statistics as JSON string.
*/
char *hpplite_node_get_stats(HppliteNode *node) {
    char pubkeyHex[HPPLITE_PUBKEY_SIZE * 2 + 1];
    char addressHex[HPPLITE_ADDRESS_SIZE * 2 + 1];
    char stateRootHex[HPPLITE_HASH_SIZE * 2 + 1];
    char instanceIdHex[32 * 2 + 1];
    unsigned char stateRoot[HPPLITE_HASH_SIZE];
    const char *stateStr;

    if (!node) return NULL;

    hpplite_bytes_to_hex(node->keypair.pubkey, HPPLITE_PUBKEY_SIZE, pubkeyHex);
    hpplite_bytes_to_hex(node->keypair.address, HPPLITE_ADDRESS_SIZE, addressHex);
    hpplite_bytes_to_hex(node->instanceId, 32, instanceIdHex);

    hpplite_node_get_state_root(node, stateRoot);
    hpplite_bytes_to_hex(stateRoot, HPPLITE_HASH_SIZE, stateRootHex);

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
        "\"state\":\"%s\","
        "\"instanceId\":\"0x%s\","
        "\"pubkey\":\"0x%s\","
        "\"address\":\"0x%s\","
        "\"height\":%llu,"
        "\"stateRoot\":\"0x%s\","
        "\"batchesProduced\":%llu,"
        "\"batchesSinceCheckpoint\":%d,"
        "\"lastCheckpointHeight\":%llu"
        "}",
        node->config.nodeId ? node->config.nodeId : "",
        stateStr,
        instanceIdHex,
        pubkeyHex,
        addressHex,
        (unsigned long long)hpplite_node_get_height(node),
        stateRootHex,
        (unsigned long long)node->batchesProduced,
        node->batchesSinceCheckpoint,
        (unsigned long long)node->lastCheckpointHeight
    );
}
