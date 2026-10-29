/*
** HPPLite Node - Singleton Sequencer Implementation
**
** Single sequencer model - all writes go through sequencer.
** L1 contract handles coordination (lease/lock mechanism).
** No peer-to-peer networking - all data flows through L1.
**
** Durability: the node keeps nothing in memory that is not also in the
** database. Pending SQL lives in _hpplite_pending (written inside the user's
** own transactions), height/roots in _hpplite_meta. Bookkeeping runs on a
** second connection so the background flush never touches the application's
** transaction state.
*/

#include "node.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>
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
    /* Monotonic: a wall-clock step must not delay or double a flush */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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
    size_t got = 0;
    if (urandom) {
        got = fread(out + 12, 1, 20, urandom);
        fclose(urandom);
    }
    if (got < 20) {
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
    node->config.maxBatchBytes = config->maxBatchBytes ? config->maxBatchBytes : 128 * 1024;
    node->config.noEncrypt = config->noEncrypt;
    node->config.determinismWarn = config->determinismWarn;

    /* Generate unique instance ID */
    generate_instance_id(node->instanceId);
    node->leaseDuration = 300;

    /* One flush at a time, timer or not (two hpplite_flush() calls from two
    ** threads used to run concurrently on a timer-less node) */
    {
        pthread_mutex_t *mutex = sqlite3_malloc(sizeof(pthread_mutex_t));
        if (!mutex) return -1;
        pthread_mutex_init(mutex, NULL);
        node->flushMutex = mutex;
    }

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
        /* Batch key: whoever can write the rollup can read it */
        hpplite_cipher_derive_key(config->privkey, node->encKey);
        node->hasEncKey = 1;
        if (!config->noEncrypt) node->cipher = hpplite_cipher_default();
    }

    return 0;
}

/*
** Set up HPPLite tracking on node->db plus the bookkeeping connection.
**
** File databases are switched to WAL so the bookkeeping connection's reads
** never block the application's writes (and vice versa), and both connections
** get a busy timeout so the brief bookkeeping writes wait instead of failing.
*/
/* Tell the capture path how long a statement may be: what fits in one DA
** batch after the envelope and the JSON framing. */
static void node_set_stmt_limit(HppliteNode *node) {
    size_t limit = node->config.maxBatchBytes;
    if (!node->ctx) return;
    if (limit > 0) {
        size_t overhead = hpplite_batch_json_overhead() + 64;
        if (node->cipher) overhead += hpplite_envelope_overhead(node->cipher);
        /* JSON escaping can double a statement full of quotes/backslashes;
        ** the capture-time check is on the raw text, so be conservative */
        limit = limit > overhead ? (limit - overhead) : 1;
    }
    node->ctx->maxStmtBytes = limit;
}

/*
** Is another connection in this process already sequencing this file? Then
** this one only captures: the log lives in the file and is shared, the
** sequencer's flush posts everyone's rows. No L1, lease or timer here.
*/
static void node_detect_follower(HppliteNode *node) {
    const char *zFile = sqlite3_db_filename(node->db, "main");
    char resolved[PATH_MAX];
    if (!zFile || !zFile[0]) return;
    if (!realpath(zFile, resolved)) return;
    node->canonicalPath = node_strdup(resolved);
    if (hpplite_find_sequencer(resolved)) {
        node->follower = 1;
        fprintf(stderr, "hpplite: %s is already sequenced by another connection in this process; "
                        "this connection captures only\n", zFile);
    }
}

static int node_init_tracking(HppliteNode *node) {
    const char *zFile = sqlite3_db_filename(node->db, "main");
    int rc;

    if (zFile && zFile[0]) {
        sqlite3_exec(node->db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
        sqlite3_busy_timeout(node->db, 5000);
    }

    rc = hpplite_init(node->db, &node->ctx);
    if (rc != SQLITE_OK) return rc;
    node->ctx->bDeterminismWarn = node->config.determinismWarn;

    if (zFile && zFile[0]) {
        rc = sqlite3_open_v2(zFile, &node->bkDb, SQLITE_OPEN_READWRITE, NULL);
        if (rc != SQLITE_OK) {
            if (node->bkDb) sqlite3_close(node->bkDb);
            node->bkDb = NULL;
        } else {
            sqlite3_busy_timeout(node->bkDb, 5000);
            hpplite_set_bookkeeping_db(node->ctx, node->bkDb);
        }
    }

    node->lastCheckpointHeight = node->ctx->lastCheckpointHeight;
    memcpy(node->lastCheckpointRoot, node->ctx->lastCheckpointRoot, HPPLITE_HASH_SIZE);
    /* Batches confirmed since the last checkpoint are in _hpplite_meta
    ** (heights), so the cadence survives a restart: a process that opens,
    ** writes a few batches and exits still reaches a checkpoint eventually */
    if (node->ctx->blockHeight > node->lastCheckpointHeight + 1)
        node->batchesSinceCheckpoint = (int)(node->ctx->blockHeight - 1 - node->lastCheckpointHeight);
    node->lastFlushTime = node_current_time_ms();
    node_set_stmt_limit(node);
    node_detect_follower(node);
    return SQLITE_OK;
}

static void node_learn_batch_limit(HppliteNode *node);

/*
** Auto-connect to L1 and sync state.
*/
static int node_ensure_lease(HppliteNode *node);

static void node_auto_connect_l1(HppliteNode *node, const HppliteNodeConfig *config) {
    if (node->follower) return;
    if (!config->rpcUrl) return;
    if (config->chainId) hpplite_l1_set_default_chain_id(config->chainId);
    if (!config->hasContract && !config->hasFactory) return;
    node->wantsL1 = 1;

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
                sqlite3_free(txHash);

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

    /* Catch up with whatever is already on L1, then take the lease now
    ** rather than at the first write: a restart has no window in which the
    ** rollup is unclaimed, and lease trouble shows at open */
    if (node->l1) {
        node_learn_batch_limit(node);
        if (hpplite_node_sync_from_l1(node, 1) < 0) {
            node->state = HPPLITE_STATE_ERROR;
        } else if (!node->follower && (node->cipher || node->config.noEncrypt)) {
            node_ensure_lease(node);
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

    if (node_init_tracking(node) != SQLITE_OK) {
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

    if (node_init_tracking(node) != SQLITE_OK) {
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
** Free a node and all resources.
*/
void hpplite_node_destroy(HppliteNode *node) {
    if (!node) return;

    /* Stop timer thread first */
    hpplite_node_stop_timer(node);

    /* Flush any pending data before cleanup (not when bookkeeping shared a
    ** connection that is already closed: nothing can run there any more) */
    if (node->ctx && !(node->ctx->bDbClosed && node->ctx->bkDb == node->ctx->db)
        && hpplite_pending_count(node->ctx) > 0) {
        hpplite_node_flush_batch(node);
    }

    /* Stop if running */
    if (node->state == HPPLITE_STATE_RUNNING) {
        hpplite_node_stop(node);
    }

    /* Let go of the sequencer lease so a restart need not wait it out */
    if (node->l1 && node->hasLease) {
        hpplite_node_release_lease(node);
    }

    /* Disconnect L1 only if we own it */
    if (node->l1 && node->ownsL1) {
        hpplite_l1_disconnect(node->l1);
    }

    /* Shutdown HPPLite context */
    if (node->ctx) {
        hpplite_shutdown(node->ctx);
    }

    if (node->bkDb) {
        sqlite3_close(node->bkDb);
    }

    /* Close database only if we own it. Unregister first so the close
    ** (which disconnects hpplite_status) does not tear this node down again. */
    if (node->db && node->ownsDb) {
        hpplite_unregister_node(node->db);
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
    sqlite3_free(node->canonicalPath);
    if (node->flushMutex) {
        pthread_mutex_destroy((pthread_mutex_t *)node->flushMutex);
        sqlite3_free(node->flushMutex);
        node->flushMutex = NULL;
    }
    /* Key material does not outlive the node */
    memset(node->config.privkey, 0, sizeof(node->config.privkey));
    memset(node->encKey, 0, sizeof(node->encKey));

    sqlite3_free(node);
}

/*
** ============================================================================
** Batches
** ============================================================================
*/

/*
** Take the DA size limit from the contract's system config, if it has one.
*/
static void node_learn_batch_limit(HppliteNode *node) {
    HppliteSystemConfig *cfg;
    if (!node || !node->l1) return;
    cfg = hpplite_l1_get_system_config(node->l1);
    if (cfg) {
        if (cfg->batchSizeLimit > 0) node->config.maxBatchBytes = (size_t)cfg->batchSizeLimit;
        if (cfg->leaseDuration > 0) node->leaseDuration = cfg->leaseDuration;
        node_set_stmt_limit(node);
        hpplite_l1_system_config_free(cfg);
    }
}

/*
** Decode batch bytes from L1: open the envelope if sealed, else plaintext JSON.
*/
static HppliteBatch *node_decode_batch(HppliteNode *node, uint64_t height,
                                       const uint8_t *data, size_t dataLen) {
    HppliteBatch *batch = NULL;
    node->lastFetchForeign = 0;
    if (!data || dataLen == 0) return NULL;
    if (hpplite_envelope_is_sealed(data, dataLen)) {
        const HppliteCipher *scheme = NULL;
        uint8_t *plain;
        if (!node->hasEncKey) {
            fprintf(stderr, "hpplite: batch %llu is encrypted and this node has no key\n",
                    (unsigned long long)height);
            return NULL;
        }
        plain = hpplite_envelope_open(node->encKey, height, data, dataLen, NULL, &scheme);
        if (!plain) {
            /* A known scheme that fails our tag was sealed with another key:
            ** posted under a lease we did not hold. Provably not ours. */
            node->lastFetchForeign = (scheme != NULL);
            fprintf(stderr, "hpplite: batch %llu: %s\n", (unsigned long long)height,
                    scheme ? "not sealed with our key (posted by another sequencer)" : "unknown encryption scheme");
            return NULL;
        }
        batch = hpplite_batch_from_json((const char *)plain);
        free(plain);
    } else {
        batch = hpplite_batch_from_json((const char *)data);
    }
    if (!batch) {
        fprintf(stderr, "hpplite: batch %llu on L1 is not a batch this version can decode (%zu bytes)\n",
                (unsigned long long)height, dataLen);
    }
    return batch;
}

/*
** The batch at our next height is not ours (fails our authentication): it was
** posted by whoever held the lease. Nobody with our key can read it, so every
** key holder skips the height the same way and reconstruction goes on. The
** raw bytes are hashed into the chain of block hashes so the skip is part of
** the history, not an omission.
*/
static int node_skip_foreign_height(HppliteNode *node, uint64_t height) {
    size_t len = 0;
    uint8_t *raw;
    unsigned char h[32];
    int rc;
    /* Every batch failing our tag from height 1 on is not an intruder, it is
    ** the wrong key on our side (or someone else's rollup): never skip that. */
    if (height <= 1) {
        fprintf(stderr, "hpplite: batch 1 is not sealed with our key: wrong key or wrong rollup\n");
        return -1;
    }
    raw = hpplite_l1_get_batch(node->l1, height, &len);
    if (!raw) return -1;
    hpplite_sha256(raw, len, h);
    free(raw);
    rc = hpplite_skip_height(node->ctx, height, h);
    if (rc == SQLITE_OK) {
        fprintf(stderr, "hpplite: batch %llu was posted by another sequencer; skipped (%llu so far)\n",
                (unsigned long long)height, (unsigned long long)node->ctx->nForeignBatches);
    }
    return rc == SQLITE_OK ? 0 : -1;
}

HppliteBatch *hpplite_node_fetch_batch(HppliteNode *node, uint64_t height) {
    size_t dataLen = 0;
    uint8_t *data;
    HppliteBatch *batch;
    if (!node || !node->l1) return NULL;
    data = hpplite_l1_get_batch(node->l1, height, &dataLen);
    batch = node_decode_batch(node, height, data, dataLen);
    free(data);
    return batch;
}

static HppliteBatch *node_fetch_batch(HppliteNode *node, uint64_t height) {
    return hpplite_node_fetch_batch(node, height);
}

/*
** If L1 already holds a batch at our next height, it was produced from this
** file's pending rows by an earlier attempt (process died after submitting,
** or the RPC reply was lost). Adopt it so we neither resubmit nor skip it.
** Returns 1 if adopted, 0 if L1 has nothing at that height, -1 on divergence.
*/
static int node_adopt_existing(HppliteNode *node) {
    uint64_t lastBatch = 0, totalBatches = 0;
    unsigned char latestHash[32];
    HppliteBatch *batch;
    int rc;

    if (hpplite_l1_get_da_state(node->l1, &lastBatch, &totalBatches, latestHash) != 0) return 0;
    if (lastBatch < node->ctx->blockHeight) return 0;

    batch = node_fetch_batch(node, node->ctx->blockHeight);
    if (!batch) {
        /* someone else's batch sits at our height: step over it and post after it */
        if (node->lastFetchForeign && node_skip_foreign_height(node, node->ctx->blockHeight) == 0) return 1;
        return 0;
    }
    rc = hpplite_adopt_batch(node->ctx, batch);
    hpplite_batch_free(batch);
    if (rc == SQLITE_OK) {
        node->batchesProduced++;
        node->batchesSinceCheckpoint++;
        return 1;
    }
    fprintf(stderr, "hpplite: L1 batch %llu does not match local pending SQL\n",
            (unsigned long long)node->ctx->blockHeight);
    return -1;
}

/*
** Build, submit and finalize one batch. *pMore is set when pending SQL
** remains because the batch was cut at the size limit.
** Returns the confirmed height, or 0.
*/
/*
** Make sure this instance holds the sequencer lease before it posts.
**
** The contract lets anyone claim an expired lease, so "a lease is active"
** is not "our lease is active": a batch submitted under a foreign lease
** reverts (and costs gas every interval). The lease is read from L1, renewed
** when ours is past half its life, claimed when free, and left alone when
** someone else holds it. Returns 1 when posting is allowed.
*/
static int node_ensure_lease(HppliteNode *node) {
    int64_t now = node_current_time_ms();
    HppliteL1State *st;
    uint64_t chainNow, remaining;
    int ours;

    if (now < node->leaseRecheckAt) return node->hasLease;

    st = hpplite_l1_get_state(node->l1);
    if (!st) {
        fprintf(stderr, "hpplite: cannot read the sequencer lease from L1; not posting\n");
        node->leaseRecheckAt = now + 5000;
        return 0;
    }
    chainNow = st->currentTimestamp ? st->currentTimestamp : (uint64_t)time(NULL);
    remaining = st->leaseExpiry > chainNow ? st->leaseExpiry - chainNow : 0;
    ours = remaining > 0 && memcmp(st->sequencerInstance, node->instanceId, 32) == 0;
    if (st->leaseDuration > 0) node->leaseDuration = st->leaseDuration;
    {
        /* A stranger's lease on a rollup we own (possible on rollups from the
        ** pre-VERSION-5 factory): the owner expires it and takes it back. A
        ** sibling instance of our own wallet is left alone - no takeover. */
        unsigned char me[20];
        int stranger = remaining > 0 && !ours && hpplite_l1_get_my_address(node->l1, me) == 0
                       && memcmp(st->sequencerWallet, me, 20) != 0;
        int weOwn = stranger && memcmp(st->owner, me, 20) == 0;
        hpplite_l1_state_free(st);
        if (stranger && weOwn) {
            char *tx = hpplite_l1_release_lease(node->l1, NULL);   /* forceExpireLease: owner only */
            fprintf(stderr, "hpplite: another wallet holds the lease of our rollup; %s\n",
                    tx ? "expiring it as owner" : "could not expire it");
            if (tx) {
                hpplite_l1_wait_for_tx(node->l1, tx, 30);
                sqlite3_free(tx);
                remaining = 0;                                     /* free now: claim below */
            }
        } else if (stranger) {
            fprintf(stderr, "hpplite: another wallet holds the lease and we are not the owner\n");
        }
    }

    if (remaining > 0 && !ours) {
        if (!node->warnedForeignLease) {
            fprintf(stderr, "hpplite: another sequencer instance holds the lease (%llu s left); "
                            "batches are kept pending until it expires\n", (unsigned long long)remaining);
            node->warnedForeignLease = 1;
        }
        node->hasLease = 0;
        node->leaseRecheckAt = now + (int64_t)(remaining < 30 ? remaining : 30) * 1000;
        return 0;
    }
    node->warnedForeignLease = 0;

    if (ours && remaining * 2 < node->leaseDuration) {
        if (hpplite_node_renew_lease(node) == 0) remaining = node->leaseDuration;
    } else if (!ours) {
        if (hpplite_node_claim_lease(node) != 0) {
            node->leaseRecheckAt = now + 5000;
            return 0;
        }
        remaining = node->leaseDuration;
    }
    node->hasLease = 1;
    /* Look again well before the renewal point, never later than a minute */
    {
        uint64_t s = remaining / 4;
        if (s < 1) s = 1;
        if (s > 60) s = 60;
        node->leaseRecheckAt = now + (int64_t)s * 1000;
    }
    return 1;
}

/*
** Deterministic crash points for the crash harness (tests/stress/crash_hpplite.c):
** HPPLITE_TEST_CRASH_AT=after_submit|after_finalize|mid_series makes the
** process die (like kill -9) at that point of a flush. Compiled only into
** mock-L1 builds.
*/
#ifdef HPPLITE_TEST_HOOKS
static void test_crash_point(const char *zName) {
    const char *z = getenv("HPPLITE_TEST_CRASH_AT");
    if (z && strcmp(z, zName) == 0) _exit(137);
}
#else
#define test_crash_point(zName) ((void)0)
#endif

static uint64_t node_flush_one(HppliteNode *node, int *pMore) {
    HppliteCtx *ctx = node->ctx;
    HppliteBatch *batch;
    sqlite3_int64 maxId = 0;
    uint64_t height;
    int confirmed = 0;

    *pMore = 0;
    /* The root is O(database size): compute it only on the batch that
    ** closes a checkpoint interval (the checkpoint is submitted from it) */
    int withRoot = node->batchesSinceCheckpoint + 1 >= node->config.checkpointInterval;
    /* The DA limit applies to the posted bytes: leave room for the envelope */
    size_t maxBytes = node->config.maxBatchBytes;
    if (node->cipher && maxBytes > 0) {
        size_t overhead = hpplite_envelope_overhead(node->cipher);
        maxBytes = maxBytes > overhead ? maxBytes - overhead : 1;
    }
    if (!node->l1 && node->wantsL1) {
        /* l1=/contract=/factory= was given but no connection came of it
        ** (no key, bad RPC, ...): finalizing locally would advance the
        ** height past batches L1 never saw. Keep everything pending. */
        if (!node->warnedNoL1) {
            fprintf(stderr, "hpplite: L1 was configured but is not connected; batches are kept pending "
                            "(check the key and the RPC endpoint)\n");
            node->warnedNoL1 = 1;
        }
        return 0;
    }

    batch = hpplite_build_batch_limited(ctx, maxBytes, withRoot, &maxId);
    if (!batch) return 0;
    height = batch->height;
    *pMore = batch->partial;

    if (node->l1) {
        /* No key means no encryption key: never post plaintext by accident.
        ** encrypt=off is the only way to post in the clear, and it is explicit. */
        if (!node->cipher && !node->config.noEncrypt) {
            if (!node->warnedNoKey) {
                fprintf(stderr, "hpplite: no private key: batches are kept pending and not posted "
                                "(set privkey=/keyfile=/HPPLITE_PRIVATE_KEY, or encrypt=off to post plaintext)\n");
                node->warnedNoKey = 1;
            }
            hpplite_batch_free(batch);
            *pMore = 0;
            return 0;
        }

        /* Only the lease holder may post; never submit into someone else's lease */
        if (!node_ensure_lease(node)) {
            hpplite_batch_free(batch);
            *pMore = 0;
            return 0;
        }

        char *json = hpplite_batch_to_json(batch);
        if (json) {
            const uint8_t *payload = (const uint8_t *)json;
            size_t payloadLen = strlen(json);
            uint8_t *sealed = NULL;
            if (node->cipher) {
                sealed = hpplite_envelope_seal(node->cipher, node->encKey, height,
                                               HPPLITE_ENCODING_NONE, payload, payloadLen, &payloadLen);
                payload = sealed;
            }
            if (payload) {
                char *txHash = hpplite_l1_submit_batch(node->l1, node->instanceId, height,
                                        payload, payloadLen);
                if (txHash) {
                    confirmed = (hpplite_l1_wait_for_tx(node->l1, txHash, 30) == 1);
                    if (confirmed) snprintf(node->lastBatchTx, sizeof(node->lastBatchTx), "%s", txHash);
                    if (confirmed && node->onBatchPosted) {
                        node->onBatchPosted(node->callbackArg, height, txHash);
                    }
                    sqlite3_free(txHash);
                }
            }
            free(sealed);
            sqlite3_free(json);
        }
        if (confirmed) test_crash_point("after_submit");
        if (!confirmed) {
            /* The transaction may have landed despite the error */
            confirmed = (node_adopt_existing(node) == 1);
            if (!confirmed) {
                int64_t wait = (int64_t)node->config.batchIntervalMs;
                if (wait <= 0) wait = 5000;
                if (node->failStreak < 6) node->failStreak++;
                wait <<= node->failStreak;                 /* 2x per failure */
                if (wait > 300000) wait = 300000;          /* ... up to the lease length */
                node->retryAt = node_current_time_ms() + wait;
                fprintf(stderr, "hpplite: batch %llu not confirmed on L1, kept pending (retry in %llds)\n",
                        (unsigned long long)height, (long long)(wait / 1000));
                *pMore = 0;
            }
            hpplite_batch_free(batch);
            return confirmed ? height : 0;
        }
    }

    if (hpplite_finalize_batch(ctx, batch, maxId) != SQLITE_OK) {
        /* Typically: an application statement is mid-step, so bookkeeping
        ** must not commit now (see bkBegin). The batch is on L1 (when there
        ** is one) and the next flush adopts it; locally nothing changed. */
        node->retryAt = 0;        /* not an L1 failure: try again next tick */
        if (ctx->nAppStmtActive == 0) {
            /* not the mid-step deferral: something is actually wrong */
            fprintf(stderr, "hpplite: could not finalize batch %llu locally\n",
                    (unsigned long long)height);
        }
        height = 0;
        *pMore = 0;
    } else {
        node->batchesProduced++;
        node->batchesSinceCheckpoint++;
        node->failStreak = 0;
        node->retryAt = 0;
        test_crash_point(*pMore ? "mid_series" : "after_finalize");
    }

    hpplite_batch_free(batch);
    return height;
}

static uint64_t node_flush_unlocked(HppliteNode *node) {
    uint64_t height = 0, h;
    int more;

    if (!node->ctx) return 0;
    if (node->follower) return 0;          /* the sequencer connection posts */
    node->lastFlushTime = node_current_time_ms();

    /* Finish anything a previous attempt left on L1. With pending SQL, a
    ** batch at our height must be ours (adopt) or the chain has forked
    ** (refuse). With nothing pending it is simply history we have not seen
    ** yet - another process of this wallet posted while we were idle (a
    ** rebuild running beside the live sequencer, a resumed machine): replay
    ** it, as open does. */
    if (node->l1) {
        int rc;
        if (hpplite_pending_count(node->ctx) == 0) {
            if (hpplite_node_sync_from_l1(node, 1) < 0) {
                node->state = HPPLITE_STATE_ERROR;
                return 0;
            }
        }
        while ((rc = node_adopt_existing(node)) == 1) { }
        if (rc < 0) {
            node->state = HPPLITE_STATE_ERROR;
            return 0;
        }
    }

    /* One batch per call normally; several when the size limit splits them */
    do {
        h = node_flush_one(node, &more);
        if (h == 0) break;
        height = h;
    } while (more);

    /* Anchor the state root on L1 once an interval of batches is complete */
    if (height && node->l1 && hpplite_node_should_checkpoint(node)) {
        char *txHash = hpplite_node_submit_checkpoint(node);
        if (txHash) {
            sqlite3_free(txHash);
        } else {
            fprintf(stderr, "hpplite: checkpoint after batch %llu not submitted; will retry\n",
                    (unsigned long long)height);
        }
    }
    return height;
}

/*
** Flush pending SQL to create a batch and post to L1.
*/
uint64_t hpplite_node_flush_batch(HppliteNode *node) {
    pthread_mutex_t *mutex;
    uint64_t height;

    if (!node) return 0;
    mutex = (pthread_mutex_t *)node->flushMutex;
    if (mutex) pthread_mutex_lock(mutex);
    /* An explicit flush reads the lease afresh; only the timer uses the cache */
    node->leaseRecheckAt = 0;
    node->retryAt = 0;
    height = node_flush_unlocked(node);
    if (mutex) pthread_mutex_unlock(mutex);
    return height;
}

/*
** Bring the local database up to date with L1.
*/
int hpplite_node_sync_from_l1(HppliteNode *node, int useTxn) {
    uint64_t lastBatch = 0, totalBatches = 0;
    unsigned char latestHash[32];
    int synced = 0;

    if (!node || !node->l1 || !node->ctx) return 0;
    if (hpplite_l1_get_da_state(node->l1, &lastBatch, &totalBatches, latestHash) != 0) return 0;

    while (node->ctx->blockHeight <= lastBatch) {
        HppliteBatch *batch = node_fetch_batch(node, node->ctx->blockHeight);
        int rc;
        if (!batch) {
            if (node->lastFetchForeign && node_skip_foreign_height(node, node->ctx->blockHeight) == 0) {
                synced++;
                continue;
            }
            fprintf(stderr, "hpplite: sync stopped at height %llu of %llu on L1\n",
                    (unsigned long long)node->ctx->blockHeight, (unsigned long long)lastBatch);
            node->state = HPPLITE_STATE_ERROR;
            return synced ? synced : -1;
        }
        if (hpplite_pending_count(node->ctx) > 0) {
            /* Local writes exist: the L1 batch must be ours */
            rc = hpplite_adopt_batch(node->ctx, batch);
            if (rc != SQLITE_OK) {
                fprintf(stderr, "hpplite: local pending SQL diverges from L1 batch %llu\n",
                        (unsigned long long)batch->height);
                hpplite_batch_free(batch);
                return -1;
            }
        } else {
            rc = hpplite_replay_batch(node->ctx, batch, useTxn);
            if (rc != SQLITE_OK && rc != SQLITE_DONE) {
                hpplite_batch_free(batch);
                return -1;
            }
        }
        node->lastCheckpointHeight = node->ctx->lastCheckpointHeight;
        hpplite_batch_free(batch);
        synced++;
    }
    return synced;
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

        /* Flush on the interval, or early once pending SQL nears the DA limit */
        int64_t now = node_current_time_ms();
        int64_t elapsed = now - node->lastFlushTime;
        int sizeTrigger = node->config.maxBatchBytes > 0 &&
            hpplite_pending_bytes(node->ctx) >= (sqlite3_int64)(node->config.maxBatchBytes / 2);

        if (now < node->retryAt) continue;           /* backing off after L1 failures */
        if (elapsed >= intervalMs || sizeTrigger) {
            pthread_mutex_t *mutex = (pthread_mutex_t *)node->flushMutex;
            if (mutex) {
                pthread_mutex_lock(mutex);
                if (node->timerRunning) {
                    node_flush_unlocked(node);
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
    if (node->follower) return;
    if (!node->bkDb) {
        /* In-memory databases have no bookkeeping connection: a timer would
        ** run BEGIN/COMMIT on the application's connection from another
        ** thread, inside its transactions. Flush on close or hpplite_flush(). */
        return;
    }
    if (!node->flushMutex) return;

    /* Start thread */
    pthread_t *thread = sqlite3_malloc(sizeof(pthread_t));
    if (!thread) return;

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

}

/*
** ============================================================================
** Lease
** ============================================================================
*/

/*
** Claim sequencer lease on L1.
** Returns 0 on success, -1 if lease unavailable.
*/
/* Does the chain say the lease is held by this instance right now? */
static int node_lease_is_ours(HppliteNode *node) {
    HppliteL1State *st = hpplite_l1_get_state(node->l1);
    int ours;
    if (!st) return -1;
    {
        uint64_t now = st->currentTimestamp ? st->currentTimestamp : (uint64_t)time(NULL);
        ours = st->leaseExpiry > now && memcmp(st->sequencerInstance, node->instanceId, 32) == 0;
    }
    hpplite_l1_state_free(st);
    return ours;
}

int hpplite_node_claim_lease(HppliteNode *node) {
    if (!node || !node->l1) return -1;

    /* A claim is a transaction: it can be mined and still have reverted
    ** (another instance holds the lease). Believing the hash alone made a
    ** second instance of the same wallet think it held the lease and then
    ** expire the live sequencer's on close (issue #40). Wait for it and
    ** ask the chain who holds the lease. */
    char *txHash = hpplite_l1_claim_sequencer(node->l1, node->instanceId);
    if (!txHash) {
        return -1;  /* Lease not available */
    }
    hpplite_l1_wait_for_tx(node->l1, txHash, 30);
    sqlite3_free(txHash);
    if (node_lease_is_ours(node) != 1) {
        node->hasLease = 0;
        return -1;
    }
    node->hasLease = 1;
    node->leaseRecheckAt = 0;
    return 0;
}

/*
** Release our sequencer lease on L1.
*/
int hpplite_node_release_lease(HppliteNode *node) {
    if (!node || !node->l1 || !node->hasLease) return -1;

    /* Never expire a lease that is not ours: forceExpireLease is owner-only
    ** and the owner wallet may be running the real sequencer elsewhere */
    {
        int ours = node_lease_is_ours(node);
        if (ours != 1) {
            fprintf(stderr, "hpplite: not releasing the lease: %s\n",
                    ours < 0 ? "cannot read the lease from L1" : "it is held by another instance");
            node->hasLease = 0;
            return -1;
        }
    }
    char *txHash = hpplite_l1_release_lease(node->l1, node->instanceId);
    if (!txHash) {
        fprintf(stderr, "hpplite: lease release transaction could not be sent\n");
        return -1;
    }
    /* Wait so the next instance (possibly this process restarting) can claim */
    if (hpplite_l1_wait_for_tx(node->l1, txHash, 30) != 1) {
        fprintf(stderr, "hpplite: lease release %s not confirmed\n", txHash);
    }
    sqlite3_free(txHash);
    node->hasLease = 0;
    node->leaseRecheckAt = 0;
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

    /* Take the lease if it is free or ours; a foreign holder (another
    ** instance of this wallet, or a stranger) is left alone - we capture
    ** and keep pending until it expires. Starting is not a failure then. */
    if (node->l1 && !node->follower) {
        node->leaseRecheckAt = 0;
        node_ensure_lease(node);
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
** Get number of pending SQL statements.
*/
int hpplite_node_pending_count(HppliteNode *node) {
    if (!node || !node->ctx) return 0;
    return hpplite_pending_count(node->ctx);
}

/*
** ============================================================================
** Checkpoints
** ============================================================================
*/

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
    node_learn_batch_limit(node);
}

/*
** Create and submit checkpoint to L1.
*/
char *hpplite_node_submit_checkpoint(HppliteNode *node) {
    if (!node || !node->l1) return NULL;
    if (node->batchesSinceCheckpoint == 0) return NULL;

    if (!node_ensure_lease(node)) return NULL;

    /* Current state: root after the last confirmed batch */
    uint64_t currentHeight = hpplite_get_block_height(node->ctx) - 1;

    /* Create checkpoint */
    HppliteCheckpoint *cp = hpplite_checkpoint_new(
        node->lastCheckpointHeight + 1,
        currentHeight
    );
    if (!cp) return NULL;

    memcpy(cp->preStateRoot, node->lastCheckpointRoot, HPPLITE_HASH_SIZE);
    memcpy(cp->postStateRoot, node->ctx->stateRoot, HPPLITE_HASH_SIZE);
    cp->timestamp = (uint64_t)time(NULL);

    /* Submit to L1 with our instance ID */
    char *txHash = hpplite_l1_submit_checkpoint(node->l1, cp, node->instanceId);

    if (txHash) {
        /* Update checkpoint tracking (persisted in _hpplite_meta) */
        hpplite_set_checkpoint(node->ctx, cp->toHeight, cp->postStateRoot);
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
** ============================================================================
** Accessors
** ============================================================================
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
        "\"pending\":%d,"
        "\"logErrors\":%llu,"
        "\"batchesProduced\":%llu,"
        "\"batchesSinceCheckpoint\":%d,"
        "\"lastCheckpointHeight\":%llu,"
        "\"encryption\":\"%s\""
        "}",
        node->config.nodeId ? node->config.nodeId : "",
        stateStr,
        instanceIdHex,
        pubkeyHex,
        addressHex,
        (unsigned long long)hpplite_node_get_height(node),
        stateRootHex,
        hpplite_node_pending_count(node),
        (unsigned long long)(node->ctx ? node->ctx->nLogErrors : 0),
        (unsigned long long)node->batchesProduced,
        node->batchesSinceCheckpoint,
        (unsigned long long)node->lastCheckpointHeight,
        node->cipher ? node->cipher->name : "none"
    );
}
