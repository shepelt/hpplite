/*
** HPPLite Ethereum L1 Client
**
** Real L1 client implementation using eth_client.
** Implements l1_interface.h for production use with HPP Sepolia.
*/

#include "l1_interface.h"
#include "eth_client.h"
#include "abi.h"
#include "keccak256.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

struct HppliteL1 {
    EthClient *eth;
    uint8_t contract[20];      /* Main HPPLite contract */
    uint8_t da_contract[20];   /* HPPLiteDA contract (for on-chain DA) */
    int has_da_contract;
    uint8_t privkey[32];
    int has_privkey;

    /* Network info */
    uint64_t chainId;
    char *rpcUrl;

    /* Cached state */
    HppliteL1State *cached_state;
    HppliteSystemConfig *cached_system_config;

    /* Callback */
    HppliteL1Callback callback;
    void *callback_arg;
};

/* Function selectors - HPPLite contract (coordination) */
static uint8_t SEL_GET_STATE[4];
static uint8_t SEL_CLAIM_SEQUENCER[4];
static uint8_t SEL_RENEW_LEASE[4];
static uint8_t SEL_SUBMIT_CHECKPOINT[4];
static uint8_t SEL_IS_LEASE_ACTIVE[4];
static uint8_t SEL_GET_SYSTEM_CONFIG[4];

/* Function selectors - HPPLiteDA contract (data availability) */
static uint8_t SEL_SUBMIT_BATCH[4];
static uint8_t SEL_GET_BATCH[4];
static uint8_t SEL_GET_BATCH_HASH[4];
static uint8_t SEL_GET_DA_STATE[4];

/* Function selectors - HPPLiteFactory contract */
static uint8_t SEL_GET_ROLLUP[4];
static uint8_t SEL_HAS_ROLLUP[4];
static uint8_t SEL_GET_OR_CREATE_ROLLUP[4];
static uint8_t SEL_CREATE_SIMPLE_ROLLUP[4];

static int selectors_initialized = 0;

/* Default factory address on HPP Sepolia (update after deployment) */
static const char *DEFAULT_FACTORY_ADDRESS = "0x0000000000000000000000000000000000000000";

static void init_selectors(void) {
    if (selectors_initialized) return;

    /* HPPLite selectors (coordination) */
    abi_function_selector("getState()", SEL_GET_STATE);
    abi_function_selector("claimSequencer(bytes32)", SEL_CLAIM_SEQUENCER);
    abi_function_selector("renewLease(bytes32)", SEL_RENEW_LEASE);
    abi_function_selector("submitCheckpoint(bytes32,uint256,uint256,bytes32)", SEL_SUBMIT_CHECKPOINT);
    abi_function_selector("isLeaseActive()", SEL_IS_LEASE_ACTIVE);
    abi_function_selector("getSystemConfig()", SEL_GET_SYSTEM_CONFIG);

    /* HPPLiteDA selectors (data availability) */
    abi_function_selector("submitBatch(bytes32,uint256,bytes)", SEL_SUBMIT_BATCH);
    abi_function_selector("getBatch(uint256)", SEL_GET_BATCH);
    abi_function_selector("getBatchHash(uint256)", SEL_GET_BATCH_HASH);
    abi_function_selector("getDAState()", SEL_GET_DA_STATE);

    /* HPPLiteFactory selectors */
    abi_function_selector("getRollup(address)", SEL_GET_ROLLUP);
    abi_function_selector("hasRollup(address)", SEL_HAS_ROLLUP);
    abi_function_selector("getOrCreateRollup()", SEL_GET_OR_CREATE_ROLLUP);
    abi_function_selector("createSimpleRollup(address)", SEL_CREATE_SIMPLE_ROLLUP);

    selectors_initialized = 1;
}

HppliteL1 *hpplite_l1_connect(const char *endpoint, const char *contractAddress) {
    if (!endpoint || !contractAddress) return NULL;

    init_selectors();

    HppliteL1 *l1 = calloc(1, sizeof(HppliteL1));
    if (!l1) return NULL;

    /* Parse contract address */
    if (eth_hex_decode(contractAddress, l1->contract, 20) != 0) {
        free(l1);
        return NULL;
    }

    /* Default to HPP Sepolia if chain ID not specified */
    l1->chainId = 181228;
    l1->rpcUrl = strdup(endpoint);

    l1->eth = eth_client_create(endpoint, l1->chainId);
    if (!l1->eth) {
        free(l1->rpcUrl);
        free(l1);
        return NULL;
    }

    return l1;
}

HppliteL1 *hpplite_l1_connect_config(const HppliteNetworkConfig *config) {
    if (!config || !config->rpcUrl) return NULL;

    init_selectors();

    HppliteL1 *l1 = calloc(1, sizeof(HppliteL1));
    if (!l1) return NULL;

    /* Copy contract address */
    memcpy(l1->contract, config->contract, 20);

    /* Store network info */
    l1->chainId = config->chainId;
    l1->rpcUrl = strdup(config->rpcUrl);

    l1->eth = eth_client_create(config->rpcUrl, config->chainId);
    if (!l1->eth) {
        free(l1->rpcUrl);
        free(l1);
        return NULL;
    }

    return l1;
}

void hpplite_l1_disconnect(HppliteL1 *l1) {
    if (!l1) return;
    if (l1->cached_state) hpplite_l1_state_free(l1->cached_state);
    if (l1->cached_system_config) hpplite_l1_system_config_free(l1->cached_system_config);
    if (l1->rpcUrl) free(l1->rpcUrl);
    if (l1->eth) eth_client_destroy(l1->eth);
    free(l1);
}

void hpplite_l1_system_config_free(HppliteSystemConfig *config) {
    if (!config) return;
    if (config->daScheme) free(config->daScheme);
    free(config);
}

/* Set private key for signing transactions */
int hpplite_l1_set_privkey(HppliteL1 *l1, const uint8_t privkey[32]) {
    memcpy(l1->privkey, privkey, 32);
    l1->has_privkey = 1;
    return eth_client_set_privkey(l1->eth, privkey);
}

HppliteL1State *hpplite_l1_get_state(HppliteL1 *l1) {
    if (!l1) return NULL;

    /* Call getState() on contract */
    uint8_t calldata[4];
    memcpy(calldata, SEL_GET_STATE, 4);

    size_t result_len;
    uint8_t *result = eth_client_call(l1->eth, l1->contract, calldata, 4, &result_len);
    if (!result) return NULL;

    /* Parse result: (owner, sequencerWallet, sequencerInstance, leaseExpiry, lastCpHeight, lastStateRoot, lastCpTime) */
    if (result_len < 7 * 32) {
        free(result);
        return NULL;
    }

    HppliteL1State *state = calloc(1, sizeof(HppliteL1State));
    if (!state) {
        free(result);
        return NULL;
    }

    /* Parse each field (32 bytes each) */
    uint8_t *p = result;

    /* owner */
    abi_decode_address(p, state->owner);
    p += 32;

    /* sequencerWallet */
    abi_decode_address(p, state->sequencerWallet);
    p += 32;

    /* sequencerInstance (bytes32) */
    abi_decode_bytes32(p, state->sequencerInstance);
    p += 32;

    /* leaseExpiry */
    state->leaseExpiry = abi_decode_uint64(p);
    p += 32;

    /* lastCheckpointHeight */
    state->lastCheckpointHeight = abi_decode_uint64(p);
    p += 32;

    /* lastStateRoot */
    abi_decode_bytes32(p, state->lastStateRoot);
    p += 32;

    /* lastCheckpointTime */
    state->lastCheckpointTime = abi_decode_uint64(p);

    free(result);
    return state;
}

void hpplite_l1_state_free(HppliteL1State *state) {
    if (!state) return;
    free(state);
}

HppliteSystemConfig *hpplite_l1_get_system_config(HppliteL1 *l1) {
    if (!l1) return NULL;

    /* Call getSystemConfig() on contract */
    /* Returns: (string daScheme, address daContract, uint256 version, uint256 chainId) */
    uint8_t calldata[4];
    memcpy(calldata, SEL_GET_SYSTEM_CONFIG, 4);

    size_t result_len;
    uint8_t *result = eth_client_call(l1->eth, l1->contract, calldata, 4, &result_len);
    if (!result) return NULL;

    /* Dynamic string + 3 values
     * Layout:
     * [0-31]   offset to string data
     * [32-63]  daContract (address)
     * [64-95]  version
     * [96-127] chainId
     * [offset] string length
     * [offset+32] string data
     */
    if (result_len < 128) {
        free(result);
        return NULL;
    }

    HppliteSystemConfig *config = calloc(1, sizeof(HppliteSystemConfig));
    if (!config) {
        free(result);
        return NULL;
    }

    /* Parse string offset and decode string */
    uint64_t strOffset = abi_decode_uint64(result);
    if (strOffset < result_len) {
        uint64_t strLen = abi_decode_uint64(result + strOffset);
        if (strLen > 0 && strOffset + 32 + strLen <= result_len) {
            config->daScheme = malloc(strLen + 1);
            memcpy(config->daScheme, result + strOffset + 32, strLen);
            config->daScheme[strLen] = '\0';
        }
    }

    /* Parse address (bytes 32-63, last 20 bytes) */
    abi_decode_address(result + 32, config->daContract);

    /* Parse remaining uint256 values */
    config->version = abi_decode_uint64(result + 64);
    config->chainId = abi_decode_uint64(result + 96);
    config->batchSizeLimit = 128 * 1024;  /* Default, DA contract has actual limit */

    /* Cache DA contract address for batch operations */
    int is_zero = 1;
    for (int i = 0; i < 20; i++) {
        if (config->daContract[i] != 0) {
            is_zero = 0;
            break;
        }
    }
    if (!is_zero) {
        memcpy(l1->da_contract, config->daContract, 20);
        l1->has_da_contract = 1;
    }

    free(result);
    return config;
}

int hpplite_l1_is_sequencer(HppliteL1 *l1, const unsigned char *pubkey) {
    HppliteL1State *state = hpplite_l1_get_state(l1);
    if (!state) return 0;

    /* Derive address from compressed pubkey and compare */
    uint8_t address[20];
    if (eth_address_from_compressed_pubkey(pubkey, address) != 0) {
        hpplite_l1_state_free(state);
        return 0;
    }

    int result = (memcmp(address, state->sequencerWallet, 20) == 0);
    hpplite_l1_state_free(state);
    return result;
}

char *hpplite_l1_submit_checkpoint(
    HppliteL1 *l1,
    const HppliteCheckpoint *checkpoint,
    const unsigned char instanceId[32]
) {
    if (!l1 || !l1->has_privkey || !checkpoint || !instanceId) return NULL;

    /* Build calldata for submitCheckpoint(bytes32,uint256,uint256,bytes32) */
    size_t calldata_len = 4 + 32 + 32 + 32 + 32;

    uint8_t *calldata = calloc(1, calldata_len);
    if (!calldata) return NULL;

    uint8_t *p = calldata;

    /* Function selector */
    memcpy(p, SEL_SUBMIT_CHECKPOINT, 4);
    p += 4;

    /* instanceId */
    memcpy(p, instanceId, 32);
    p += 32;

    /* fromHeight */
    abi_encode_uint64(checkpoint->fromHeight, p);
    p += 32;

    /* toHeight */
    abi_encode_uint64(checkpoint->toHeight, p);
    p += 32;

    /* stateRoot */
    abi_encode_bytes32(checkpoint->postStateRoot, p);

    /* Send transaction */
    uint8_t *tx_hash = eth_client_send_tx(l1->eth, l1->contract, calldata, calldata_len,
                                           500000, 0);
    free(calldata);

    if (!tx_hash) return NULL;

    char *hash_hex = eth_hex_encode(tx_hash, 32);
    free(tx_hash);
    return hash_hex;
}

char *hpplite_l1_claim_sequencer(HppliteL1 *l1, const unsigned char instanceId[32]) {
    if (!l1 || !l1->has_privkey || !instanceId) return NULL;
    init_selectors();

    /* Build calldata: claimSequencer(bytes32) */
    uint8_t calldata[4 + 32];
    memcpy(calldata, SEL_CLAIM_SEQUENCER, 4);
    memcpy(calldata + 4, instanceId, 32);

    /* Send transaction */
    uint8_t *tx_hash = eth_client_send_tx(l1->eth, l1->contract, calldata, sizeof(calldata), 200000, 0);
    if (!tx_hash) return NULL;

    char *hash_hex = eth_hex_encode(tx_hash, 32);
    free(tx_hash);
    return hash_hex;
}

char *hpplite_l1_renew_lease(HppliteL1 *l1, const unsigned char instanceId[32]) {
    if (!l1 || !l1->has_privkey || !instanceId) return NULL;
    init_selectors();

    /* Build calldata: renewLease(bytes32) */
    uint8_t calldata[4 + 32];
    memcpy(calldata, SEL_RENEW_LEASE, 4);
    memcpy(calldata + 4, instanceId, 32);

    /* Send transaction */
    uint8_t *tx_hash = eth_client_send_tx(l1->eth, l1->contract, calldata, sizeof(calldata), 100000, 0);
    if (!tx_hash) return NULL;

    char *hash_hex = eth_hex_encode(tx_hash, 32);
    free(tx_hash);
    return hash_hex;
}

int hpplite_l1_is_lease_active(HppliteL1 *l1) {
    if (!l1) return 0;

    uint8_t calldata[4];
    memcpy(calldata, SEL_IS_LEASE_ACTIVE, 4);
    
    size_t result_len;
    uint8_t *result = eth_client_call(l1->eth, l1->contract, calldata, 4, &result_len);
    if (!result || result_len < 32) {
        free(result);
        return 0;
    }
    
    int expired = (abi_decode_uint64(result) != 0);
    free(result);
    return expired;
}

void hpplite_l1_set_callback(HppliteL1 *l1, HppliteL1Callback callback, void *arg) {
    if (!l1) return;
    l1->callback = callback;
    l1->callback_arg = arg;
}

int hpplite_l1_poll(HppliteL1 *l1) {
    /* For now, just refresh state - no event monitoring yet */
    if (!l1) return -1;
    
    HppliteL1State *new_state = hpplite_l1_get_state(l1);
    if (!new_state) return -1;
    
    /* Check for changes and fire callbacks */
    if (l1->cached_state && l1->callback) {
        /* Check for new checkpoint */
        if (new_state->lastCheckpointHeight > l1->cached_state->lastCheckpointHeight) {
            l1->callback(l1->callback_arg, "checkpoint", new_state);
        }
    }
    
    if (l1->cached_state) hpplite_l1_state_free(l1->cached_state);
    l1->cached_state = new_state;

    return 0;
}

int hpplite_l1_wait_for_tx(HppliteL1 *l1, const char *tx_hash, int timeout_secs) {
    if (!l1 || !tx_hash) return -1;

    /* Parse tx hash */
    uint8_t hash_bytes[32];
    if (eth_hex_decode(tx_hash, hash_bytes, 32) != 0) {
        return -1;
    }

    /* Poll for receipt */
    for (int i = 0; i < timeout_secs; i++) {
        int status = 0;
        int result = eth_client_get_receipt(l1->eth, hash_bytes, &status);

        if (result == 1) {
            /* Mined */
            return status;  /* 1 = success, 0 = reverted */
        }
        if (result < 0) {
            return -1;  /* Error */
        }

        /* Still pending, wait */
        sleep(1);
    }

    return -1;  /* Timeout */
}

/* ============ Factory Functions ============ */

int hpplite_l1_factory_get_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t owner_address[20],
    uint8_t rollup_address_out[20]
) {
    init_selectors();

    /* Parse factory address */
    uint8_t factory[20];
    const char *factory_addr = factory_address ? factory_address : DEFAULT_FACTORY_ADDRESS;
    if (eth_hex_decode(factory_addr, factory, 20) != 0) {
        return -1;
    }

    /* Create temporary eth client */
    EthClient *eth = eth_client_create(rpc_url, 181228);  /* HPP Sepolia */
    if (!eth) return -1;

    /* Build calldata: getRollup(address) */
    uint8_t calldata[4 + 32];
    memcpy(calldata, SEL_GET_ROLLUP, 4);
    abi_encode_address(owner_address, calldata + 4);

    size_t result_len;
    uint8_t *result = eth_client_call(eth, factory, calldata, sizeof(calldata), &result_len);
    eth_client_destroy(eth);

    if (!result || result_len < 32) {
        free(result);
        return -1;
    }

    /* Decode address from result */
    abi_decode_address(result, rollup_address_out);
    free(result);

    /* Check if zero address (no rollup) */
    int is_zero = 1;
    for (int i = 0; i < 20; i++) {
        if (rollup_address_out[i] != 0) {
            is_zero = 0;
            break;
        }
    }

    return is_zero ? 0 : 1;  /* 0 = no rollup, 1 = has rollup */
}

int hpplite_l1_factory_has_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t owner_address[20]
) {
    uint8_t rollup[20];
    return hpplite_l1_factory_get_rollup(rpc_url, factory_address, owner_address, rollup);
}

char *hpplite_l1_factory_get_or_create_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint8_t rollup_address_out[20]
) {
    init_selectors();

    /* Parse factory address */
    uint8_t factory[20];
    const char *factory_addr = factory_address ? factory_address : DEFAULT_FACTORY_ADDRESS;
    if (eth_hex_decode(factory_addr, factory, 20) != 0) {
        return NULL;
    }

    /* Create eth client with private key */
    EthClient *eth = eth_client_create(rpc_url, 181228);
    if (!eth) return NULL;

    if (eth_client_set_privkey(eth, privkey) != 0) {
        eth_client_destroy(eth);
        return NULL;
    }

    /* Build calldata: getOrCreateRollup() - no parameters */
    uint8_t calldata[4];
    memcpy(calldata, SEL_GET_OR_CREATE_ROLLUP, 4);

    /* Send transaction - needs high gas for deploying child HPPLiteDA contract */
    uint8_t *tx_hash = eth_client_send_tx(eth, factory, calldata, 4, 15000000, 0);

    if (!tx_hash) {
        eth_client_destroy(eth);
        return NULL;
    }
    eth_client_destroy(eth);

    char *hash_hex = eth_hex_encode(tx_hash, 32);
    free(tx_hash);

    /* Caller should wait for tx and then call get_rollup to get the address */
    return hash_hex;
}

/* Helper: Get rollup address from private key (derive address, then lookup) */
int hpplite_l1_factory_get_my_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint8_t rollup_address_out[20]
) {
    /* Derive address from private key */
    uint8_t my_address[20];
    if (eth_address_from_privkey(privkey, my_address) != 0) {
        return -1;
    }

    return hpplite_l1_factory_get_rollup(rpc_url, factory_address, my_address, rollup_address_out);
}

/* Connect to L1 using factory - auto-discovers rollup for this wallet */
HppliteL1 *hpplite_l1_connect_factory(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32]
) {
    /* Get rollup address for this wallet */
    uint8_t rollup[20];
    int result = hpplite_l1_factory_get_my_rollup(rpc_url, factory_address, privkey, rollup);

    if (result != 1) {
        /* No rollup exists for this wallet */
        return NULL;
    }

    /* Convert rollup address to hex */
    char *rollup_hex = eth_hex_encode(rollup, 20);
    if (!rollup_hex) return NULL;

    /* Connect to the rollup contract */
    HppliteL1 *l1 = hpplite_l1_connect(rpc_url, rollup_hex);
    free(rollup_hex);

    if (l1) {
        /* Set private key for transactions */
        hpplite_l1_set_privkey(l1, privkey);
    }

    return l1;
}

/* ============ Batch DA Functions ============ */

/* Helper to ensure DA contract is cached */
static int ensure_da_contract(HppliteL1 *l1) {
    if (l1->has_da_contract) return 1;

    /* Try to get system config which caches DA contract */
    HppliteSystemConfig *config = hpplite_l1_get_system_config(l1);
    if (config) {
        hpplite_l1_system_config_free(config);
    }

    return l1->has_da_contract;
}

char *hpplite_l1_submit_batch(
    HppliteL1 *l1,
    const uint8_t instanceId[32],
    uint64_t height,
    const uint8_t *data,
    size_t data_len
) {
    if (!l1 || !l1->has_privkey || !instanceId || !data || data_len == 0) return NULL;
    if (!ensure_da_contract(l1)) return NULL;  /* Need DA contract */

    /* Build calldata: submitBatch(bytes32 instanceId, uint256 height, bytes data) */
    /* Static: 4 + 32 + 32 + 32 (offset) = 100 */
    /* Dynamic: 32 (length) + padded data */
    size_t padded_len = ((data_len + 31) / 32) * 32;
    size_t calldata_len = 4 + 32 + 32 + 32 + 32 + padded_len;

    uint8_t *calldata = calloc(1, calldata_len);
    if (!calldata) return NULL;

    uint8_t *p = calldata;

    /* Function selector */
    memcpy(p, SEL_SUBMIT_BATCH, 4);
    p += 4;

    /* instanceId (bytes32) */
    memcpy(p, instanceId, 32);
    p += 32;

    /* height */
    abi_encode_uint64(height, p);
    p += 32;

    /* bytes offset (96 from start of params = 3 * 32) */
    abi_encode_uint64(96, p);
    p += 32;

    /* bytes length */
    abi_encode_uint64(data_len, p);
    p += 32;

    /* bytes data */
    memcpy(p, data, data_len);

    /* Send transaction to DA contract
     * Gas calculation:
     * - Base tx cost: 21000
     * - Per non-zero byte: 16 (calldata can be large for batch JSON)
     * - Contract storage: ~20000 per 32-byte slot (expensive!)
     * - On L2s like HPP: additional L1 data posting costs
     * For batch storage, we need significant gas for SSTORE operations.
     */
    uint64_t gas_estimate = 200000 + data_len * 700;  /* ~700 gas per byte for storage */
    if (gas_estimate < 3000000) gas_estimate = 3000000;  /* Min 3M gas for batches */
    uint8_t *tx_hash = eth_client_send_tx(l1->eth, l1->da_contract, calldata, calldata_len,
                                           gas_estimate, 0);
    free(calldata);

    if (!tx_hash) return NULL;

    char *hash_hex = eth_hex_encode(tx_hash, 32);
    free(tx_hash);
    return hash_hex;
}

uint8_t *hpplite_l1_get_batch(
    HppliteL1 *l1,
    uint64_t height,
    size_t *data_len_out
) {
    if (!l1 || !data_len_out) return NULL;
    if (!ensure_da_contract(l1)) {
        *data_len_out = 0;
        return NULL;
    }

    /* Build calldata: getBatch(uint256 height) */
    uint8_t calldata[4 + 32];
    memcpy(calldata, SEL_GET_BATCH, 4);
    abi_encode_uint64(height, calldata + 4);

    size_t result_len;
    uint8_t *result = eth_client_call(l1->eth, l1->da_contract, calldata, sizeof(calldata), &result_len);
    if (!result || result_len < 64) {
        free(result);
        *data_len_out = 0;
        return NULL;
    }

    /* Dynamic bytes: offset (32) + length (32) + data */
    uint64_t offset = abi_decode_uint64(result);
    if (offset >= result_len) {
        free(result);
        *data_len_out = 0;
        return NULL;
    }

    uint64_t len = abi_decode_uint64(result + offset);
    if (len == 0 || offset + 32 + len > result_len) {
        free(result);
        *data_len_out = 0;
        return NULL;
    }

    /* Copy data */
    uint8_t *data = malloc(len);
    if (!data) {
        free(result);
        *data_len_out = 0;
        return NULL;
    }

    memcpy(data, result + offset + 32, len);
    *data_len_out = len;

    free(result);
    return data;
}

int hpplite_l1_get_batch_hash(
    HppliteL1 *l1,
    uint64_t height,
    uint8_t hash_out[32]
) {
    if (!l1) return -1;
    if (!ensure_da_contract(l1)) return -1;

    /* Build calldata: getBatchHash(uint256 height) */
    uint8_t calldata[4 + 32];
    memcpy(calldata, SEL_GET_BATCH_HASH, 4);
    abi_encode_uint64(height, calldata + 4);

    size_t result_len;
    uint8_t *result = eth_client_call(l1->eth, l1->da_contract, calldata, sizeof(calldata), &result_len);
    if (!result || result_len < 32) {
        free(result);
        return -1;
    }

    memcpy(hash_out, result, 32);
    free(result);

    /* Check if zero (no batch at this height) */
    int is_zero = 1;
    for (int i = 0; i < 32; i++) {
        if (hash_out[i] != 0) {
            is_zero = 0;
            break;
        }
    }

    return is_zero ? 0 : 1;  /* 0 = no batch, 1 = has batch */
}

int hpplite_l1_get_da_state(
    HppliteL1 *l1,
    uint64_t *last_batch_height,
    uint64_t *total_batches,
    uint8_t latest_batch_hash[32]
) {
    if (!l1) return -1;
    if (!ensure_da_contract(l1)) return -1;

    /* Build calldata: getDAState() */
    uint8_t calldata[4];
    memcpy(calldata, SEL_GET_DA_STATE, 4);

    size_t result_len;
    uint8_t *result = eth_client_call(l1->eth, l1->da_contract, calldata, 4, &result_len);
    if (!result || result_len < 96) {
        free(result);
        return -1;
    }

    /* Parse: (uint256 lastBatchHeight, uint256 totalBatches, bytes32 latestBatchHash) */
    if (last_batch_height) *last_batch_height = abi_decode_uint64(result);
    if (total_batches) *total_batches = abi_decode_uint64(result + 32);
    if (latest_batch_hash) memcpy(latest_batch_hash, result + 64, 32);

    free(result);
    return 0;
}

/* ============ Peer Discovery Functions ============ */

/* Endpoint functions removed in singleton model - no peer discovery needed */
