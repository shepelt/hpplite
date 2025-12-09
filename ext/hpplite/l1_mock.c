/*
** HPPLite L1 Mock Implementation
**
** In-memory mock for testing singleton sequencer model.
** Implements lease-based sequencer coordination.
*/

#include "l1_interface.h"
#include "crypto.h"
#include "sqlite3.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define MAX_CHECKPOINTS 1000
#define MAX_BATCHES 10000
#define DEFAULT_LEASE_DURATION 300  /* 5 minutes */

/*
** Internal L1 mock structure
*/
struct HppliteL1 {
    /* Owner */
    unsigned char owner[20];

    /* Sequencer with lease */
    int hasSequencer;
    unsigned char sequencerWallet[20];
    unsigned char sequencerInstance[32];
    uint64_t leaseExpiry;

    /* Configuration */
    uint64_t checkpointInterval;
    uint64_t leaseDuration;

    /* Checkpoints */
    int nCheckpoints;
    HppliteCheckpoint *checkpoints[MAX_CHECKPOINTS];
    uint64_t lastCheckpointTime;
    uint64_t lastCheckpointHeight;
    unsigned char lastStateRoot[32];

    /* Mock L1 block simulation */
    uint64_t currentBlock;
    uint64_t currentTimestamp;

    /* Crypto context for signature verification */
    HppliteCrypto *crypto;

    /* Private key for transactions */
    unsigned char privkey[32];
    int hasPrivkey;
    unsigned char myAddress[20];

    /* Callbacks */
    HppliteL1Callback callback;
    void *callbackArg;

    /* Batch DA storage */
    uint64_t nBatches;
    uint8_t *batchData[MAX_BATCHES];
    size_t batchLen[MAX_BATCHES];
};

/*
** Create a new mock L1
*/
HppliteL1 *hpplite_l1_create_mock(void) {
    HppliteL1 *l1 = sqlite3_malloc(sizeof(HppliteL1));
    if (!l1) return NULL;

    memset(l1, 0, sizeof(HppliteL1));

    /* Initialize crypto for signature verification */
    l1->crypto = hpplite_crypto_init();
    if (!l1->crypto) {
        sqlite3_free(l1);
        return NULL;
    }

    /* Default configuration */
    l1->checkpointInterval = 100;
    l1->leaseDuration = DEFAULT_LEASE_DURATION;

    /* Initialize mock block state */
    l1->currentBlock = 1;
    l1->currentTimestamp = (uint64_t)time(NULL);
    l1->lastCheckpointTime = l1->currentTimestamp;

    return l1;
}

/*
** Connect to L1 (for mock, just creates a new instance)
*/
HppliteL1 *hpplite_l1_connect(const char *endpoint, const char *contractAddress) {
    (void)endpoint;
    (void)contractAddress;
    return hpplite_l1_create_mock();
}

/*
** Connect from network config
*/
HppliteL1 *hpplite_l1_connect_config(const HppliteNetworkConfig *config) {
    (void)config;
    return hpplite_l1_create_mock();
}

/*
** Set private key for transactions
*/
int hpplite_l1_set_privkey(HppliteL1 *l1, const uint8_t privkey[32]) {
    if (!l1 || !privkey) return -1;

    memcpy(l1->privkey, privkey, 32);
    l1->hasPrivkey = 1;

    /* Derive address from private key */
    HppliteKeypair kp;
    int rc = hpplite_crypto_keypair_from_privkey(l1->crypto, &kp, privkey);
    if (rc == 0) {
        memcpy(l1->myAddress, kp.address, 20);
    }

    return 0;
}

/*
** Disconnect from L1
*/
void hpplite_l1_disconnect(HppliteL1 *l1) {
    if (!l1) return;

    /* Free checkpoints */
    for (int i = 0; i < l1->nCheckpoints; i++) {
        hpplite_checkpoint_free(l1->checkpoints[i]);
    }

    /* Free batch data */
    for (uint64_t i = 0; i < l1->nBatches; i++) {
        if (l1->batchData[i]) {
            free(l1->batchData[i]);
        }
    }

    /* Free crypto */
    if (l1->crypto) {
        hpplite_crypto_free(l1->crypto);
    }

    sqlite3_free(l1);
}

/*
** Get system configuration
*/
HppliteSystemConfig *hpplite_l1_get_system_config(HppliteL1 *l1) {
    if (!l1) return NULL;

    HppliteSystemConfig *config = sqlite3_malloc(sizeof(HppliteSystemConfig));
    if (!config) return NULL;

    memset(config, 0, sizeof(HppliteSystemConfig));
    config->daScheme = sqlite3_mprintf("hppda");
    config->version = 4;
    config->checkpointInterval = l1->checkpointInterval;
    config->leaseDuration = l1->leaseDuration;
    config->batchSizeLimit = 128 * 1024;

    return config;
}

/*
** Free system config
*/
void hpplite_l1_system_config_free(HppliteSystemConfig *config) {
    if (!config) return;
    sqlite3_free(config->daScheme);
    sqlite3_free(config);
}

/*
** Get current L1 state
*/
HppliteL1State *hpplite_l1_get_state(HppliteL1 *l1) {
    if (!l1) return NULL;

    HppliteL1State *state = sqlite3_malloc(sizeof(HppliteL1State));
    if (!state) return NULL;

    memset(state, 0, sizeof(HppliteL1State));

    /* Copy owner */
    memcpy(state->owner, l1->owner, 20);

    /* Copy sequencer info */
    if (l1->hasSequencer) {
        memcpy(state->sequencerWallet, l1->sequencerWallet, 20);
        memcpy(state->sequencerInstance, l1->sequencerInstance, 32);
        state->leaseExpiry = l1->leaseExpiry;
    }

    /* Copy config */
    state->checkpointInterval = l1->checkpointInterval;
    state->leaseDuration = l1->leaseDuration;

    /* Copy checkpoint info */
    state->lastCheckpointHeight = l1->lastCheckpointHeight;
    memcpy(state->lastStateRoot, l1->lastStateRoot, 32);
    state->lastCheckpointTime = l1->lastCheckpointTime;

    state->currentBlock = l1->currentBlock;
    state->currentTimestamp = l1->currentTimestamp;

    return state;
}

/*
** Free L1 state
*/
void hpplite_l1_state_free(HppliteL1State *state) {
    if (!state) return;
    sqlite3_free(state);
}

/* === Lease Management === */

/*
** Check if lease is currently active
*/
int hpplite_l1_is_lease_active(HppliteL1 *l1) {
    if (!l1 || !l1->hasSequencer) return 0;
    return l1->currentTimestamp < l1->leaseExpiry;
}

/*
** Get remaining lease time
*/
uint64_t hpplite_l1_get_lease_time_remaining(HppliteL1 *l1) {
    if (!l1 || !l1->hasSequencer) return 0;
    if (l1->currentTimestamp >= l1->leaseExpiry) return 0;
    return l1->leaseExpiry - l1->currentTimestamp;
}

/*
** Check if pubkey is current sequencer with active lease
*/
int hpplite_l1_is_sequencer(HppliteL1 *l1, const unsigned char *pubkey) {
    if (!l1 || !pubkey || !l1->hasSequencer) return 0;
    if (!hpplite_l1_is_lease_active(l1)) return 0;

    /* Derive address from pubkey and compare to sequencer wallet */
    /* For mock, we just check if there's an active lease */
    return 1;
}

/*
** Claim sequencer lease with instance ID
*/
char *hpplite_l1_claim_sequencer(
    HppliteL1 *l1,
    const unsigned char instanceId[32]
) {
    if (!l1 || !instanceId) return NULL;
    if (!l1->hasPrivkey) return NULL;

    /* Check if lease is available */
    if (l1->hasSequencer && hpplite_l1_is_lease_active(l1)) {
        return NULL;  /* Lease still active */
    }

    /* Claim the lease */
    l1->hasSequencer = 1;
    memcpy(l1->sequencerWallet, l1->myAddress, 20);
    memcpy(l1->sequencerInstance, instanceId, 32);
    l1->leaseExpiry = l1->currentTimestamp + l1->leaseDuration;

    /* Generate mock transaction hash */
    char *txHash = sqlite3_mprintf("0x%08x%08x",
        (unsigned int)(l1->currentBlock),
        (unsigned int)(l1->currentTimestamp));

    /* Emit event */
    if (l1->callback) {
        l1->callback(l1->callbackArg, "SequencerClaimed", (void *)instanceId);
    }

    return txHash;
}

/*
** Renew sequencer lease
*/
char *hpplite_l1_renew_lease(
    HppliteL1 *l1,
    const unsigned char instanceId[32]
) {
    if (!l1 || !instanceId) return NULL;
    if (!l1->hasPrivkey) return NULL;

    /* Must be current sequencer */
    if (!l1->hasSequencer) return NULL;
    if (memcmp(l1->sequencerWallet, l1->myAddress, 20) != 0) return NULL;
    if (memcmp(l1->sequencerInstance, instanceId, 32) != 0) return NULL;

    /* Lease must not be expired */
    if (!hpplite_l1_is_lease_active(l1)) return NULL;

    /* Renew */
    l1->leaseExpiry = l1->currentTimestamp + l1->leaseDuration;

    /* Generate mock transaction hash */
    char *txHash = sqlite3_mprintf("0x%08xrenew%08x",
        (unsigned int)(l1->currentBlock),
        (unsigned int)(l1->currentTimestamp));

    return txHash;
}

/* === Checkpoint Operations === */

/*
** Submit a checkpoint to L1
*/
char *hpplite_l1_submit_checkpoint(
    HppliteL1 *l1,
    const HppliteCheckpoint *checkpoint,
    const unsigned char instanceId[32]
) {
    if (!l1 || !checkpoint || !instanceId) return NULL;

    /* Verify instanceId matches active lease */
    if (memcmp(instanceId, l1->sequencerInstance, 32) != 0) {
        return NULL;  /* Wrong instance */
    }
    if (l1->leaseExpiry <= l1->currentTimestamp) {
        return NULL;  /* Lease expired */
    }

    /* Store checkpoint */
    if (l1->nCheckpoints >= MAX_CHECKPOINTS) {
        return NULL;
    }

    HppliteCheckpoint *cpCopy = hpplite_checkpoint_new(checkpoint->fromHeight, checkpoint->toHeight);
    if (!cpCopy) return NULL;

    memcpy(cpCopy->preStateRoot, checkpoint->preStateRoot, 32);
    memcpy(cpCopy->postStateRoot, checkpoint->postStateRoot, 32);
    memcpy(cpCopy->batchesHash, checkpoint->batchesHash, 32);
    cpCopy->timestamp = checkpoint->timestamp;

    l1->checkpoints[l1->nCheckpoints++] = cpCopy;
    l1->lastCheckpointTime = l1->currentTimestamp;
    l1->lastCheckpointHeight = checkpoint->toHeight;
    memcpy(l1->lastStateRoot, checkpoint->postStateRoot, 32);

    /* Auto-renew lease on checkpoint */
    if (l1->hasSequencer && hpplite_l1_is_lease_active(l1)) {
        l1->leaseExpiry = l1->currentTimestamp + l1->leaseDuration;
    }

    /* Generate mock transaction hash */
    char *txHash = sqlite3_mprintf("0xcp%08x%08x",
        (unsigned int)(l1->currentBlock),
        (unsigned int)(l1->nCheckpoints));

    /* Emit event */
    if (l1->callback) {
        l1->callback(l1->callbackArg, "CheckpointSubmitted", cpCopy);
    }

    return txHash;
}

/*
** Set callback for L1 events
*/
void hpplite_l1_set_callback(HppliteL1 *l1, HppliteL1Callback callback, void *arg) {
    if (!l1) return;
    l1->callback = callback;
    l1->callbackArg = arg;
}

/*
** Poll for L1 updates (mock: advance block)
*/
int hpplite_l1_poll(HppliteL1 *l1) {
    if (!l1) return -1;

    /* Simulate block advancement (12 seconds per block) */
    l1->currentBlock++;
    l1->currentTimestamp += 12;

    return 0;
}

int hpplite_l1_wait_for_tx(HppliteL1 *l1, const char *tx_hash, int timeout_secs) {
    (void)l1;
    (void)tx_hash;
    (void)timeout_secs;
    /* Mock always succeeds immediately */
    return 1;
}

/* === Mock-specific functions for testing === */

void hpplite_l1_mock_set_sequencer(HppliteL1 *l1, const unsigned char *pubkey) {
    if (!l1 || !pubkey) return;

    l1->hasSequencer = 1;
    /* Just use pubkey bytes as address for mock */
    memcpy(l1->sequencerWallet, pubkey, 20);
    memset(l1->sequencerInstance, 0, 32);
    l1->leaseExpiry = l1->currentTimestamp + l1->leaseDuration;
}

void hpplite_l1_mock_advance_blocks(HppliteL1 *l1, uint64_t blocks) {
    if (!l1) return;
    l1->currentBlock += blocks;
    l1->currentTimestamp += blocks * 12;
}

void hpplite_l1_mock_advance_time(HppliteL1 *l1, uint64_t seconds) {
    if (!l1) return;
    l1->currentTimestamp += seconds;
    l1->currentBlock += seconds / 12;
}

void hpplite_l1_mock_set_config(
    HppliteL1 *l1,
    uint64_t checkpointInterval,
    uint64_t leaseDuration
) {
    if (!l1) return;
    l1->checkpointInterval = checkpointInterval;
    l1->leaseDuration = leaseDuration;
}

uint64_t hpplite_l1_mock_get_timestamp(HppliteL1 *l1) {
    return l1 ? l1->currentTimestamp : 0;
}

uint64_t hpplite_l1_mock_get_block(HppliteL1 *l1) {
    return l1 ? l1->currentBlock : 0;
}

const HppliteCheckpoint *hpplite_l1_mock_get_last_checkpoint(HppliteL1 *l1) {
    if (!l1 || l1->nCheckpoints == 0) return NULL;
    return l1->checkpoints[l1->nCheckpoints - 1];
}

int hpplite_l1_mock_get_checkpoint_count(HppliteL1 *l1) {
    return l1 ? l1->nCheckpoints : 0;
}

/* === Factory Functions (stubs for mock) === */

int hpplite_l1_factory_get_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t owner_address[20],
    uint8_t rollup_address_out[20]
) {
    (void)rpc_url;
    (void)factory_address;
    (void)owner_address;
    memset(rollup_address_out, 0, 20);
    return 0;
}

int hpplite_l1_factory_has_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t owner_address[20]
) {
    (void)rpc_url;
    (void)factory_address;
    (void)owner_address;
    return 0;
}

char *hpplite_l1_factory_get_or_create_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint8_t rollup_address_out[20]
) {
    (void)rpc_url;
    (void)factory_address;
    (void)privkey;
    memset(rollup_address_out, 0, 20);
    return NULL;
}

int hpplite_l1_factory_get_my_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint8_t rollup_address_out[20]
) {
    (void)rpc_url;
    (void)factory_address;
    (void)privkey;
    memset(rollup_address_out, 0, 20);
    return 0;
}

HppliteL1 *hpplite_l1_connect_factory(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32]
) {
    (void)rpc_url;
    (void)factory_address;
    (void)privkey;
    return NULL;
}

/* === Batch DA Functions === */

char *hpplite_l1_submit_batch(
    HppliteL1 *l1,
    const uint8_t instanceId[32],
    uint64_t height,
    const uint8_t *data,
    size_t data_len
) {
    if (!l1 || !instanceId || !data || height == 0) return NULL;
    if (height > MAX_BATCHES) return NULL;

    /* Verify instanceId matches active lease */
    if (memcmp(instanceId, l1->sequencerInstance, 32) != 0) {
        return NULL;  /* Wrong instance */
    }
    if (l1->leaseExpiry <= l1->currentTimestamp) {
        return NULL;  /* Lease expired */
    }

    size_t idx = height - 1;

    /* Free existing data if overwriting */
    if (l1->batchData[idx]) {
        free(l1->batchData[idx]);
    }

    l1->batchData[idx] = malloc(data_len);
    if (!l1->batchData[idx]) return NULL;

    memcpy(l1->batchData[idx], data, data_len);
    l1->batchLen[idx] = data_len;

    if (height > l1->nBatches) {
        l1->nBatches = height;
    }

    return sqlite3_mprintf("0x%016llx", (unsigned long long)height);
}

uint8_t *hpplite_l1_get_batch(
    HppliteL1 *l1,
    uint64_t height,
    size_t *data_len_out
) {
    if (!l1 || height == 0 || height > MAX_BATCHES) {
        if (data_len_out) *data_len_out = 0;
        return NULL;
    }

    size_t idx = height - 1;
    if (!l1->batchData[idx]) {
        if (data_len_out) *data_len_out = 0;
        return NULL;
    }

    uint8_t *copy = malloc(l1->batchLen[idx]);
    if (!copy) {
        if (data_len_out) *data_len_out = 0;
        return NULL;
    }

    memcpy(copy, l1->batchData[idx], l1->batchLen[idx]);
    if (data_len_out) *data_len_out = l1->batchLen[idx];
    return copy;
}

int hpplite_l1_get_batch_hash(
    HppliteL1 *l1,
    uint64_t height,
    uint8_t hash_out[32]
) {
    if (!l1 || height == 0 || height > MAX_BATCHES) {
        memset(hash_out, 0, 32);
        return 0;
    }

    size_t idx = height - 1;
    if (!l1->batchData[idx]) {
        memset(hash_out, 0, 32);
        return 0;
    }

    memset(hash_out, 0, 32);
    for (size_t i = 0; i < l1->batchLen[idx] && i < 32; i++) {
        hash_out[i] = l1->batchData[idx][i];
    }
    return 1;
}

int hpplite_l1_get_da_state(
    HppliteL1 *l1,
    uint64_t *last_batch_height,
    uint64_t *total_batches,
    uint8_t latest_batch_hash[32]
) {
    if (!l1) {
        if (last_batch_height) *last_batch_height = 0;
        if (total_batches) *total_batches = 0;
        if (latest_batch_hash) memset(latest_batch_hash, 0, 32);
        return -1;
    }

    if (last_batch_height) *last_batch_height = l1->nBatches;
    if (total_batches) *total_batches = l1->nBatches;

    if (latest_batch_hash) {
        if (l1->nBatches > 0) {
            hpplite_l1_get_batch_hash(l1, l1->nBatches, latest_batch_hash);
        } else {
            memset(latest_batch_hash, 0, 32);
        }
    }
    return 0;
}
