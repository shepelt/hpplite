/*
** HPPLite L1 Mock Implementation
**
** In-memory mock for Milestone 1 (shared-memory testing).
** All nodes in same process share this via pointer.
*/

#include "l1_interface.h"
#include "crypto.h"
#include "sqlite3.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define MAX_WITNESSES 32
#define MAX_CHECKPOINTS 1000

/*
** Internal L1 mock structure
*/
struct HppliteL1 {
    /* Sequencer */
    int hasSequencer;
    unsigned char sequencerPubkey[33];
    unsigned char sequencerAddress[20];
    uint64_t sequencerSinceBlock;

    /* Witnesses */
    int nWitnesses;
    unsigned char witnessPubkeys[MAX_WITNESSES][33];
    unsigned char witnessAddresses[MAX_WITNESSES][20];

    /* Configuration */
    int requiredAttestations;
    uint64_t checkpointInterval;
    uint64_t sequencerTimeout;  /* in seconds */

    /* Checkpoints */
    int nCheckpoints;
    HppliteCheckpoint *checkpoints[MAX_CHECKPOINTS];
    uint64_t lastCheckpointTime;

    /* Mock L1 block simulation */
    uint64_t currentBlock;
    uint64_t currentTimestamp;

    /* Crypto context for signature verification */
    HppliteCrypto *crypto;

    /* Callbacks */
    HppliteL1Callback callback;
    void *callbackArg;
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
    l1->requiredAttestations = 2;
    l1->checkpointInterval = 100;
    l1->sequencerTimeout = 3600;  /* 1 hour */

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
** Disconnect from L1
*/
void hpplite_l1_disconnect(HppliteL1 *l1) {
    if (!l1) return;

    /* Free checkpoints */
    for (int i = 0; i < l1->nCheckpoints; i++) {
        hpplite_checkpoint_free(l1->checkpoints[i]);
    }

    /* Free crypto */
    if (l1->crypto) {
        hpplite_crypto_free(l1->crypto);
    }

    sqlite3_free(l1);
}

/*
** Get current L1 state
*/
HppliteL1State *hpplite_l1_get_state(HppliteL1 *l1) {
    HppliteL1State *state;

    if (!l1) return NULL;

    state = sqlite3_malloc(sizeof(HppliteL1State));
    if (!state) return NULL;

    memset(state, 0, sizeof(HppliteL1State));

    /* Copy sequencer info */
    if (l1->hasSequencer) {
        memcpy(state->sequencerPubkey, l1->sequencerPubkey, 33);
        memcpy(state->sequencerAddress, l1->sequencerAddress, 20);
        state->sequencerSince = l1->sequencerSinceBlock;
    }

    /* Copy witness info */
    state->nWitnesses = l1->nWitnesses;
    if (l1->nWitnesses > 0) {
        state->witnessPubkeys = sqlite3_malloc(l1->nWitnesses * 33);
        state->witnessAddresses = sqlite3_malloc(l1->nWitnesses * 20);
        if (state->witnessPubkeys && state->witnessAddresses) {
            for (int i = 0; i < l1->nWitnesses; i++) {
                memcpy(state->witnessPubkeys[i], l1->witnessPubkeys[i], 33);
                memcpy(state->witnessAddresses[i], l1->witnessAddresses[i], 20);
            }
        }
    }

    /* Copy config */
    state->requiredAttestations = l1->requiredAttestations;
    state->checkpointInterval = l1->checkpointInterval;
    state->sequencerTimeout = l1->sequencerTimeout;

    /* Copy checkpoint info */
    if (l1->nCheckpoints > 0) {
        HppliteCheckpoint *latest = l1->checkpoints[l1->nCheckpoints - 1];
        state->lastCheckpointHeight = latest->toHeight;
        memcpy(state->lastStateRoot, latest->postStateRoot, 32);
        state->lastCheckpointBlock = l1->currentBlock;  /* Simplified */
    }

    state->currentBlock = l1->currentBlock;

    return state;
}

/*
** Free L1 state
*/
void hpplite_l1_state_free(HppliteL1State *state) {
    if (!state) return;
    sqlite3_free(state->witnessPubkeys);
    sqlite3_free(state->witnessAddresses);
    sqlite3_free(state);
}

/*
** Check if pubkey is current sequencer
*/
int hpplite_l1_is_sequencer(HppliteL1 *l1, const unsigned char *pubkey) {
    if (!l1 || !pubkey || !l1->hasSequencer) return 0;
    return memcmp(l1->sequencerPubkey, pubkey, 33) == 0;
}

/*
** Check if pubkey is a registered witness
*/
int hpplite_l1_is_witness(HppliteL1 *l1, const unsigned char *pubkey) {
    if (!l1 || !pubkey) return 0;

    for (int i = 0; i < l1->nWitnesses; i++) {
        if (memcmp(l1->witnessPubkeys[i], pubkey, 33) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
** Check if sequencer timeout has expired
*/
int hpplite_l1_sequencer_timeout_expired(HppliteL1 *l1) {
    if (!l1) return 0;
    if (!l1->hasSequencer) return 1;  /* No sequencer = can claim */

    return (l1->currentTimestamp > l1->lastCheckpointTime + l1->sequencerTimeout);
}

/*
** Submit a checkpoint to L1
*/
char *hpplite_l1_submit_checkpoint(
    HppliteL1 *l1,
    const HppliteCheckpoint *checkpoint,
    const HppliteCheckpointAttestation *attestations,
    int nAttestations
) {
    unsigned char checkpointHash[32];
    int validSigs = 0;
    HppliteCheckpoint *cpCopy;
    char *txHash;

    if (!l1 || !checkpoint || !attestations) return NULL;

    /* Verify caller is sequencer */
    /* (In real impl, would check msg.sender) */

    /* Verify enough attestations */
    if (nAttestations < l1->requiredAttestations) {
        return NULL;
    }

    /* Compute checkpoint hash */
    hpplite_checkpoint_hash(checkpoint, checkpointHash);

    /* Verify each attestation */
    for (int i = 0; i < nAttestations; i++) {
        const HppliteCheckpointAttestation *att = &attestations[i];

        /* Must be registered witness */
        if (!hpplite_l1_is_witness(l1, att->witnessPubkey)) {
            continue;
        }

        /* Verify signature */
        HppliteSignature sig;
        memcpy(sig.sig, att->signature, 64);
        sig.recid = att->recid;

        int valid = hpplite_crypto_verify(l1->crypto, att->witnessPubkey, checkpointHash, &sig);
        if (valid == 1) {
            validSigs++;
        }
    }

    /* Check quorum */
    if (validSigs < l1->requiredAttestations) {
        return NULL;
    }

    /* Store checkpoint */
    if (l1->nCheckpoints >= MAX_CHECKPOINTS) {
        return NULL;  /* Full */
    }

    cpCopy = hpplite_checkpoint_new(checkpoint->fromHeight, checkpoint->toHeight);
    if (!cpCopy) return NULL;

    memcpy(cpCopy->preStateRoot, checkpoint->preStateRoot, 32);
    memcpy(cpCopy->postStateRoot, checkpoint->postStateRoot, 32);
    memcpy(cpCopy->batchesHash, checkpoint->batchesHash, 32);
    cpCopy->timestamp = checkpoint->timestamp;

    l1->checkpoints[l1->nCheckpoints++] = cpCopy;
    l1->lastCheckpointTime = l1->currentTimestamp;

    /* Generate mock transaction hash */
    txHash = sqlite3_mprintf("0x%08x%08x",
        (unsigned int)(l1->currentBlock),
        (unsigned int)(l1->nCheckpoints));

    /* Emit event via callback */
    if (l1->callback) {
        l1->callback(l1->callbackArg, "CheckpointSubmitted", cpCopy);
    }

    return txHash;
}

/*
** Claim sequencer role
*/
int hpplite_l1_claim_sequencer(HppliteL1 *l1, const unsigned char *privkey) {
    HppliteKeypair kp;
    int rc;

    if (!l1 || !privkey) return -1;

    /* Derive pubkey from privkey */
    rc = hpplite_crypto_keypair_from_privkey(l1->crypto, &kp, privkey);
    if (rc != 0) return -1;

    /* Must be a registered witness */
    if (!hpplite_l1_is_witness(l1, kp.pubkey)) {
        return -1;
    }

    /* Check if can claim (no sequencer or timeout expired) */
    if (l1->hasSequencer && !hpplite_l1_sequencer_timeout_expired(l1)) {
        return -1;  /* Current sequencer still active */
    }

    /* Claim sequencer role */
    l1->hasSequencer = 1;
    memcpy(l1->sequencerPubkey, kp.pubkey, 33);
    memcpy(l1->sequencerAddress, kp.address, 20);
    l1->sequencerSinceBlock = l1->currentBlock;

    /* Reset checkpoint timer */
    l1->lastCheckpointTime = l1->currentTimestamp;

    /* Emit event */
    if (l1->callback) {
        l1->callback(l1->callbackArg, "SequencerChanged", kp.pubkey);
    }

    return 0;
}

/*
** Register as a witness
*/
int hpplite_l1_register_witness(HppliteL1 *l1, const unsigned char *privkey) {
    HppliteKeypair kp;
    int rc;

    if (!l1 || !privkey) return -1;
    if (l1->nWitnesses >= MAX_WITNESSES) return -1;

    /* Derive pubkey from privkey */
    rc = hpplite_crypto_keypair_from_privkey(l1->crypto, &kp, privkey);
    if (rc != 0) return -1;

    /* Check not already registered */
    if (hpplite_l1_is_witness(l1, kp.pubkey)) {
        return 0;  /* Already registered, ok */
    }

    /* Add to witness list */
    memcpy(l1->witnessPubkeys[l1->nWitnesses], kp.pubkey, 33);
    memcpy(l1->witnessAddresses[l1->nWitnesses], kp.address, 20);
    l1->nWitnesses++;

    /* Emit event */
    if (l1->callback) {
        l1->callback(l1->callbackArg, "WitnessRegistered", kp.pubkey);
    }

    return 0;
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

/* === Mock-specific functions for testing === */

/*
** Set mock sequencer directly (for testing)
*/
void hpplite_l1_mock_set_sequencer(HppliteL1 *l1, const unsigned char *pubkey) {
    if (!l1 || !pubkey) return;

    l1->hasSequencer = 1;
    memcpy(l1->sequencerPubkey, pubkey, 33);
    /* Address not set - would need full keypair */
    l1->sequencerSinceBlock = l1->currentBlock;
    l1->lastCheckpointTime = l1->currentTimestamp;
}

/*
** Add mock witness directly (for testing)
*/
void hpplite_l1_mock_add_witness(HppliteL1 *l1, const unsigned char *pubkey) {
    if (!l1 || !pubkey) return;
    if (l1->nWitnesses >= MAX_WITNESSES) return;
    if (hpplite_l1_is_witness(l1, pubkey)) return;  /* Already registered */

    memcpy(l1->witnessPubkeys[l1->nWitnesses], pubkey, 33);
    l1->nWitnesses++;
}

/*
** Advance mock block number
*/
void hpplite_l1_mock_advance_blocks(HppliteL1 *l1, uint64_t blocks) {
    if (!l1) return;

    l1->currentBlock += blocks;
    l1->currentTimestamp += blocks * 12;  /* 12 sec per block */
}

/*
** Advance mock time directly (in seconds)
*/
void hpplite_l1_mock_advance_time(HppliteL1 *l1, uint64_t seconds) {
    if (!l1) return;

    l1->currentTimestamp += seconds;
    l1->currentBlock += seconds / 12;
}

/*
** Set mock configuration
*/
void hpplite_l1_mock_set_config(
    HppliteL1 *l1,
    int requiredAttestations,
    uint64_t checkpointInterval,
    uint64_t sequencerTimeout
) {
    if (!l1) return;

    l1->requiredAttestations = requiredAttestations;
    l1->checkpointInterval = checkpointInterval;
    l1->sequencerTimeout = sequencerTimeout;
}

/*
** Get mock current timestamp
*/
uint64_t hpplite_l1_mock_get_timestamp(HppliteL1 *l1) {
    return l1 ? l1->currentTimestamp : 0;
}

/*
** Get mock current block
*/
uint64_t hpplite_l1_mock_get_block(HppliteL1 *l1) {
    return l1 ? l1->currentBlock : 0;
}

/*
** Get last checkpoint (for testing)
*/
const HppliteCheckpoint *hpplite_l1_mock_get_last_checkpoint(HppliteL1 *l1) {
    if (!l1 || l1->nCheckpoints == 0) return NULL;
    return l1->checkpoints[l1->nCheckpoints - 1];
}

/*
** Get checkpoint count (for testing)
*/
int hpplite_l1_mock_get_checkpoint_count(HppliteL1 *l1) {
    return l1 ? l1->nCheckpoints : 0;
}
