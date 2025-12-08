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
    uint8_t contract[20];
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

/* Function selectors */
static uint8_t SEL_GET_STATE[4];
static uint8_t SEL_SET_SEQUENCER[4];
static uint8_t SEL_ADD_WITNESS[4];
static uint8_t SEL_SUBMIT_CHECKPOINT[4];
static uint8_t SEL_IS_SEQUENCER_TIMED_OUT[4];
static uint8_t SEL_GET_WITNESSES[4];
static uint8_t SEL_GET_SYSTEM_CONFIG[4];
static int selectors_initialized = 0;

static void init_selectors(void) {
    if (selectors_initialized) return;
    abi_function_selector("getState()", SEL_GET_STATE);
    abi_function_selector("setSequencer(address)", SEL_SET_SEQUENCER);
    abi_function_selector("addWitness(address)", SEL_ADD_WITNESS);
    abi_function_selector("submitCheckpoint(uint256,uint256,bytes32,bytes)", SEL_SUBMIT_CHECKPOINT);
    abi_function_selector("isSequencerTimedOut()", SEL_IS_SEQUENCER_TIMED_OUT);
    abi_function_selector("getWitnesses()", SEL_GET_WITNESSES);
    abi_function_selector("getSystemConfig()", SEL_GET_SYSTEM_CONFIG);
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
    
    /* Parse result: (owner, sequencer, witnessCount, requiredAtt, cpInterval, seqTimeout, lastCpHeight, lastStateRoot, lastCpTime) */
    if (result_len < 9 * 32) {
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
    
    /* owner - skip for now */
    p += 32;
    
    /* sequencer address */
    abi_decode_address(p, state->sequencerAddress);
    p += 32;
    
    /* witnessCount */
    state->nWitnesses = (int)abi_decode_uint64(p);
    p += 32;
    
    /* requiredAttestations */
    state->requiredAttestations = (int)abi_decode_uint64(p);
    p += 32;
    
    /* checkpointInterval */
    state->checkpointInterval = abi_decode_uint64(p);
    p += 32;
    
    /* sequencerTimeout */
    state->sequencerTimeout = abi_decode_uint64(p);
    p += 32;
    
    /* lastCheckpointHeight */
    state->lastCheckpointHeight = abi_decode_uint64(p);
    p += 32;
    
    /* lastStateRoot */
    abi_decode_bytes32(p, state->lastStateRoot);
    p += 32;
    
    /* lastCheckpointTime - using as lastCheckpointBlock for now */
    state->lastCheckpointBlock = abi_decode_uint64(p);
    
    free(result);
    
    /* Get witnesses if any */
    if (state->nWitnesses > 0) {
        memcpy(calldata, SEL_GET_WITNESSES, 4);
        result = eth_client_call(l1->eth, l1->contract, calldata, 4, &result_len);
        if (result && result_len >= 64) {
            /* Dynamic array: offset (32) + length (32) + data */
            uint64_t offset = abi_decode_uint64(result);
            if (offset < result_len) {
                uint64_t count = abi_decode_uint64(result + offset);
                if (count > 0 && count == (uint64_t)state->nWitnesses) {
                    state->witnessAddresses = calloc(count, 20);
                    for (uint64_t i = 0; i < count; i++) {
                        abi_decode_address(result + offset + 32 + i * 32, state->witnessAddresses[i]);
                    }
                }
            }
        }
        free(result);
    }
    
    return state;
}

void hpplite_l1_state_free(HppliteL1State *state) {
    if (!state) return;
    if (state->witnessPubkeys) free(state->witnessPubkeys);
    if (state->witnessAddresses) free(state->witnessAddresses);
    free(state);
}

HppliteSystemConfig *hpplite_l1_get_system_config(HppliteL1 *l1) {
    if (!l1) return NULL;

    /* Call getSystemConfig() on contract */
    /* Returns: (string daScheme, address daContract, uint256 batchSizeLimit, uint256 version, uint256 chainId) */
    uint8_t calldata[4];
    memcpy(calldata, SEL_GET_SYSTEM_CONFIG, 4);

    size_t result_len;
    uint8_t *result = eth_client_call(l1->eth, l1->contract, calldata, 4, &result_len);
    if (!result) return NULL;

    /* Dynamic string + 4 uint256 values
     * Layout:
     * [0-31]   offset to string data
     * [32-63]  daContract (address, right-padded)
     * [64-95]  batchSizeLimit
     * [96-127] version
     * [128-159] chainId
     * [offset] string length
     * [offset+32] string data
     */
    if (result_len < 160) {
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
    config->batchSizeLimit = abi_decode_uint64(result + 64);
    config->version = abi_decode_uint64(result + 96);
    config->chainId = abi_decode_uint64(result + 128);

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

    int result = (memcmp(address, state->sequencerAddress, 20) == 0);
    hpplite_l1_state_free(state);
    return result;
}

int hpplite_l1_is_witness(HppliteL1 *l1, const unsigned char *pubkey) {
    HppliteL1State *state = hpplite_l1_get_state(l1);
    if (!state) return 0;

    /* Derive address from compressed pubkey */
    uint8_t address[20];
    if (eth_address_from_compressed_pubkey(pubkey, address) != 0) {
        hpplite_l1_state_free(state);
        return 0;
    }
    
    int result = 0;
    if (state->witnessAddresses) {
        for (int i = 0; i < state->nWitnesses; i++) {
            if (memcmp(address, state->witnessAddresses[i], 20) == 0) {
                result = 1;
                break;
            }
        }
    }
    
    hpplite_l1_state_free(state);
    return result;
}

char *hpplite_l1_submit_checkpoint(
    HppliteL1 *l1,
    const HppliteCheckpoint *checkpoint,
    const HppliteCheckpointAttestation *attestations,
    int nAttestations
) {
    if (!l1 || !l1->has_privkey || !checkpoint || !attestations) return NULL;
    
    /* Build calldata for submitCheckpoint(uint256,uint256,bytes32,bytes) */
    /* Static params: 4 + 32 + 32 + 32 + 32 (offset) = 132 bytes */
    /* Dynamic bytes: 32 (length) + nAttestations * 65 (padded to 32) */
    
    size_t sig_data_len = nAttestations * 65;
    size_t sig_padded_len = ((sig_data_len + 31) / 32) * 32;
    size_t calldata_len = 4 + 32 + 32 + 32 + 32 + 32 + sig_padded_len;
    
    uint8_t *calldata = calloc(1, calldata_len);
    if (!calldata) return NULL;
    
    uint8_t *p = calldata;
    
    /* Function selector */
    memcpy(p, SEL_SUBMIT_CHECKPOINT, 4);
    p += 4;
    
    /* fromHeight */
    abi_encode_uint64(checkpoint->fromHeight, p);
    p += 32;
    
    /* toHeight */
    abi_encode_uint64(checkpoint->toHeight, p);
    p += 32;
    
    /* stateRoot */
    abi_encode_bytes32(checkpoint->postStateRoot, p);
    p += 32;
    
    /* bytes offset (4 * 32 = 128 from start of params) */
    abi_encode_uint64(128, p);
    p += 32;
    
    /* bytes length */
    abi_encode_uint64(sig_data_len, p);
    p += 32;
    
    /* attestation signatures (r, s, v for each) */
    for (int i = 0; i < nAttestations; i++) {
        memcpy(p + i * 65, attestations[i].signature, 64);
        p[i * 65 + 64] = attestations[i].recid + 27;
    }
    
    /* Send transaction (needs ~530k gas for checkpoint with 2 sigs) */
    uint8_t *tx_hash = eth_client_send_tx(l1->eth, l1->contract, calldata, calldata_len,
                                           1000000, 0);
    free(calldata);
    
    if (!tx_hash) return NULL;
    
    char *hash_hex = eth_hex_encode(tx_hash, 32);
    free(tx_hash);
    return hash_hex;
}

int hpplite_l1_claim_sequencer(HppliteL1 *l1, const unsigned char *privkey) {
    /* Note: In current contract, only owner can setSequencer */
    /* This would need to be the owner's privkey */
    if (!l1) return -1;
    
    /* For now, this is a no-op - owner manages sequencer externally */
    return -1;
}

int hpplite_l1_register_witness(HppliteL1 *l1, const unsigned char *privkey) {
    /* Note: In current contract, only owner can addWitness */
    if (!l1) return -1;
    
    /* For now, this is a no-op - owner manages witnesses externally */
    return -1;
}

int hpplite_l1_sequencer_timeout_expired(HppliteL1 *l1) {
    if (!l1) return 0;
    
    uint8_t calldata[4];
    memcpy(calldata, SEL_IS_SEQUENCER_TIMED_OUT, 4);
    
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
