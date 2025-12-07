/*
** HPPLite L1 Interface
**
** Abstract interface for L1 blockchain interaction.
** Provides mock implementation for testing, can be swapped
** for real Ethereum/L1 client in production.
**
** L1 responsibilities (replaces Zookeeper):
** - Sequencer election
** - Witness registry
** - Checkpoint commitments
** - Configuration (required sigs, timeouts)
*/

#ifndef HPPLITE_L1_INTERFACE_H
#define HPPLITE_L1_INTERFACE_H

#include "batch.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** L1 connection context
*/
typedef struct HppliteL1 HppliteL1;

/*
** L1 contract state (read from chain)
*/
typedef struct HppliteL1State {
    /* Current sequencer */
    unsigned char sequencerPubkey[33];
    unsigned char sequencerAddress[20];
    uint64_t sequencerSince;           /* Block number when elected */

    /* Witnesses */
    int nWitnesses;
    unsigned char (*witnessPubkeys)[33];
    unsigned char (*witnessAddresses)[20];

    /* Configuration */
    int requiredAttestations;          /* Signatures needed for finality */
    uint64_t checkpointInterval;       /* Batches between checkpoints */
    uint64_t sequencerTimeout;         /* Blocks before sequencer considered failed */

    /* Latest checkpoint */
    uint64_t lastCheckpointHeight;
    unsigned char lastStateRoot[32];
    uint64_t lastCheckpointBlock;      /* L1 block of last checkpoint */

    /* Current L1 state */
    uint64_t currentBlock;
} HppliteL1State;

/*
** Callback for L1 events
*/
typedef void (*HppliteL1Callback)(void *arg, const char *event, void *data);

/*
** Initialize L1 connection.
** For mock: pass NULL or "mock" as endpoint
** For real: pass RPC endpoint like "https://mainnet.infura.io/..."
*/
HppliteL1 *hpplite_l1_connect(const char *endpoint, const char *contractAddress);

/*
** Disconnect from L1
*/
void hpplite_l1_disconnect(HppliteL1 *l1);

/*
** Get current L1 state
** Caller must free with hpplite_l1_state_free()
*/
HppliteL1State *hpplite_l1_get_state(HppliteL1 *l1);

/*
** Free L1 state
*/
void hpplite_l1_state_free(HppliteL1State *state);

/*
** Check if we are the current sequencer
*/
int hpplite_l1_is_sequencer(HppliteL1 *l1, const unsigned char *pubkey);

/*
** Check if a pubkey is a registered witness
*/
int hpplite_l1_is_witness(HppliteL1 *l1, const unsigned char *pubkey);

/*
** Submit a checkpoint to L1 (sequencer only)
** Returns transaction hash on success, NULL on failure
*/
char *hpplite_l1_submit_checkpoint(
    HppliteL1 *l1,
    const HppliteCheckpoint *checkpoint,
    const HppliteCheckpointAttestation *attestations,
    int nAttestations
);

/*
** Claim sequencer role (after timeout or initial setup)
** Returns 0 on success, -1 on failure
*/
int hpplite_l1_claim_sequencer(HppliteL1 *l1, const unsigned char *privkey);

/*
** Register as a witness
** Returns 0 on success, -1 on failure
*/
int hpplite_l1_register_witness(HppliteL1 *l1, const unsigned char *privkey);

/*
** Check if sequencer timeout has expired
** Returns 1 if timeout expired (sequencer failed), 0 otherwise
*/
int hpplite_l1_sequencer_timeout_expired(HppliteL1 *l1);

/*
** Set callback for L1 events (new checkpoint, sequencer change, etc.)
*/
void hpplite_l1_set_callback(HppliteL1 *l1, HppliteL1Callback callback, void *arg);

/*
** Poll for L1 updates (call periodically)
** Returns number of new events, or -1 on error
*/
int hpplite_l1_poll(HppliteL1 *l1);

/* === Mock-specific functions for testing === */

/*
** Create a mock L1 with custom initial state
*/
HppliteL1 *hpplite_l1_create_mock(void);

/*
** Set mock sequencer (for testing)
*/
void hpplite_l1_mock_set_sequencer(HppliteL1 *l1, const unsigned char *pubkey);

/*
** Add mock witness (for testing)
*/
void hpplite_l1_mock_add_witness(HppliteL1 *l1, const unsigned char *pubkey);

/*
** Advance mock block number (for testing timeouts)
*/
void hpplite_l1_mock_advance_blocks(HppliteL1 *l1, uint64_t blocks);

/*
** Set mock configuration
*/
void hpplite_l1_mock_set_config(
    HppliteL1 *l1,
    int requiredAttestations,
    uint64_t checkpointInterval,
    uint64_t sequencerTimeout
);

/*
** Advance mock time directly (in seconds)
*/
void hpplite_l1_mock_advance_time(HppliteL1 *l1, uint64_t seconds);

/*
** Get mock current timestamp
*/
uint64_t hpplite_l1_mock_get_timestamp(HppliteL1 *l1);

/*
** Get mock current block
*/
uint64_t hpplite_l1_mock_get_block(HppliteL1 *l1);

/*
** Get last checkpoint (for testing)
*/
const HppliteCheckpoint *hpplite_l1_mock_get_last_checkpoint(HppliteL1 *l1);

/*
** Get checkpoint count (for testing)
*/
int hpplite_l1_mock_get_checkpoint_count(HppliteL1 *l1);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_L1_INTERFACE_H */
