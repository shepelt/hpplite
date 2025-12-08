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
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** Network configuration (for bootstrapping)
*/
typedef struct HppliteNetworkConfig {
    const char *alias;           /* Network alias: "hpp-sepolia" (optional) */
    uint64_t chainId;            /* Chain ID: 181228 */
    const char *rpcUrl;          /* RPC URL: "https://sepolia.hpp.io" */
    unsigned char contract[20];  /* Contract address */
} HppliteNetworkConfig;

/*
** System configuration (read from L1 contract)
*/
typedef struct HppliteSystemConfig {
    char *daScheme;              /* DA URI scheme: "hppda", "ipfs", "file" */
    unsigned char daContract[20]; /* DA contract address */
    uint64_t batchSizeLimit;     /* Max batch size in bytes */
    uint64_t version;            /* Contract version */
    uint64_t chainId;            /* Chain ID */
} HppliteSystemConfig;

/*
** Built-in network definitions
*/
#define HPPLITE_NETWORK_HPP_SEPOLIA { \
    .alias = "hpp-sepolia", \
    .chainId = 181228, \
    .rpcUrl = "https://sepolia.hpp.io", \
    .contract = {0} \
}

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
** Initialize L1 connection from network config.
** Preferred method - uses structured config.
*/
HppliteL1 *hpplite_l1_connect_config(const HppliteNetworkConfig *config);

/*
** Get system configuration from L1 contract.
** Caller must free with hpplite_l1_system_config_free().
*/
HppliteSystemConfig *hpplite_l1_get_system_config(HppliteL1 *l1);

/*
** Free system config
*/
void hpplite_l1_system_config_free(HppliteSystemConfig *config);

/*
** Set private key for signing L1 transactions.
** Must be called before submitting batches or checkpoints.
*/
int hpplite_l1_set_privkey(HppliteL1 *l1, const uint8_t privkey[32]);

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

/*
** Wait for transaction to be mined
** tx_hash is hex string (0x prefixed)
** Returns 1 if success, 0 if reverted, -1 if pending/error
*/
int hpplite_l1_wait_for_tx(HppliteL1 *l1, const char *tx_hash, int timeout_secs);

/* === Factory Functions (for auto-deploy) === */

/*
** Get rollup address for an owner from factory.
** Returns: 1 if rollup exists (address in rollup_address_out)
**          0 if no rollup for this owner
**         -1 on error
*/
int hpplite_l1_factory_get_rollup(
    const char *rpc_url,
    const char *factory_address,    /* NULL for default HPP Sepolia factory */
    const uint8_t owner_address[20],
    uint8_t rollup_address_out[20]
);

/*
** Check if owner has a rollup.
** Returns: 1 if rollup exists, 0 if no rollup, -1 on error
*/
int hpplite_l1_factory_has_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t owner_address[20]
);

/*
** Get or create rollup for the wallet associated with privkey.
** If no rollup exists, creates one (transaction).
** Returns transaction hash on success, NULL on failure.
** Caller should wait for tx, then call get_rollup to get address.
*/
char *hpplite_l1_factory_get_or_create_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint8_t rollup_address_out[20]
);

/*
** Get rollup address for the wallet associated with privkey.
** Helper that derives address from privkey, then calls get_rollup.
** Returns: 1 if rollup exists, 0 if no rollup, -1 on error
*/
int hpplite_l1_factory_get_my_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint8_t rollup_address_out[20]
);

/*
** Connect to L1 using factory - auto-discovers rollup for this wallet.
** Returns NULL if no rollup exists for this wallet.
** On success, private key is already set for transactions.
*/
HppliteL1 *hpplite_l1_connect_factory(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32]
);

/* === Batch DA Functions (on-chain data availability) === */

/*
** Submit batch data to L1 for data availability.
** Returns transaction hash on success, NULL on failure.
*/
char *hpplite_l1_submit_batch(
    HppliteL1 *l1,
    uint64_t height,
    const uint8_t *data,
    size_t data_len
);

/*
** Get batch data from L1.
** Returns batch data (caller must free), NULL if not found.
*/
uint8_t *hpplite_l1_get_batch(
    HppliteL1 *l1,
    uint64_t height,
    size_t *data_len_out
);

/*
** Get batch hash from L1.
** Returns: 1 if batch exists (hash in hash_out)
**          0 if no batch at this height
**         -1 on error
*/
int hpplite_l1_get_batch_hash(
    HppliteL1 *l1,
    uint64_t height,
    uint8_t hash_out[32]
);

/*
** Get DA state from L1 contract.
** Returns 0 on success, -1 on error.
*/
int hpplite_l1_get_da_state(
    HppliteL1 *l1,
    uint64_t *last_batch_height,
    uint64_t *total_batches,
    uint8_t latest_batch_hash[32]
);

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
