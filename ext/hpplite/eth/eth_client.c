/*
** Minimal Ethereum JSON-RPC client for HPPLite
*/

#include "eth_client.h"
#include "cJSON.h"
#include "keccak256.h"
#include "rlp.h"
#include <curl/curl.h>
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct EthClient {
    char *rpc_url;
    uint64_t chain_id;
    uint8_t privkey[32];
    uint8_t address[20];
    int has_privkey;
    char error[256];
    CURL *curl;
    secp256k1_context *secp_ctx;
};

/* Response buffer for curl */
struct ResponseBuffer {
    char *data;
    size_t size;
};

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct ResponseBuffer *buf = (struct ResponseBuffer *)userp;
    
    char *ptr = realloc(buf->data, buf->size + realsize + 1);
    if (!ptr) return 0;
    
    buf->data = ptr;
    memcpy(&(buf->data[buf->size]), contents, realsize);
    buf->size += realsize;
    buf->data[buf->size] = 0;
    
    return realsize;
}

/* Hex utilities */
char *eth_hex_encode(const uint8_t *data, size_t len) {
    char *hex = malloc(2 + len * 2 + 1);
    if (!hex) return NULL;
    
    hex[0] = '0';
    hex[1] = 'x';
    for (size_t i = 0; i < len; i++) {
        sprintf(hex + 2 + i * 2, "%02x", data[i]);
    }
    hex[2 + len * 2] = '\0';
    return hex;
}

int eth_hex_decode(const char *hex, uint8_t *out, size_t out_len) {
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
    }
    
    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2) return -1;
    
    for (size_t i = 0; i < out_len; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%02x", &byte) != 1) return -1;
        out[i] = (uint8_t)byte;
    }
    return 0;
}

/* Make JSON-RPC call */
static cJSON *rpc_call(EthClient *client, const char *method, cJSON *params) {
    struct ResponseBuffer response = {0};
    struct curl_slist *headers = NULL;
    
    /* Build request */
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddStringToObject(request, "method", method);
    cJSON_AddItemToObject(request, "params", params ? params : cJSON_CreateArray());
    cJSON_AddNumberToObject(request, "id", 1);
    
    char *request_str = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    
    /* Setup curl */
    curl_easy_reset(client->curl);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    curl_easy_setopt(client->curl, CURLOPT_URL, client->rpc_url);
    curl_easy_setopt(client->curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(client->curl, CURLOPT_POSTFIELDS, request_str);
    curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, &response);
    
    CURLcode res = curl_easy_perform(client->curl);
    curl_slist_free_all(headers);
    free(request_str);
    
    if (res != CURLE_OK) {
        snprintf(client->error, sizeof(client->error), "curl error: %s", 
                 curl_easy_strerror(res));
        free(response.data);
        return NULL;
    }
    
    /* Parse response */
    cJSON *json = cJSON_Parse(response.data);
    free(response.data);
    
    if (!json) {
        snprintf(client->error, sizeof(client->error), "JSON parse error");
        return NULL;
    }
    
    /* Check for error */
    cJSON *error = cJSON_GetObjectItem(json, "error");
    if (error) {
        cJSON *msg = cJSON_GetObjectItem(error, "message");
        snprintf(client->error, sizeof(client->error), "RPC error: %s",
                 msg ? msg->valuestring : "unknown");
        cJSON_Delete(json);
        return NULL;
    }
    
    return json;
}

EthClient *eth_client_create(const char *rpc_url, uint64_t chain_id) {
    EthClient *client = calloc(1, sizeof(EthClient));
    if (!client) return NULL;
    
    client->rpc_url = strdup(rpc_url);
    client->chain_id = chain_id;
    client->curl = curl_easy_init();
    client->secp_ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    
    if (!client->curl || !client->secp_ctx) {
        eth_client_destroy(client);
        return NULL;
    }
    
    return client;
}

void eth_client_destroy(EthClient *client) {
    if (!client) return;
    free(client->rpc_url);
    if (client->curl) curl_easy_cleanup(client->curl);
    if (client->secp_ctx) secp256k1_context_destroy(client->secp_ctx);
    free(client);
}

int eth_client_set_privkey(EthClient *client, const uint8_t privkey[32]) {
    memcpy(client->privkey, privkey, 32);
    
    /* Derive address from private key */
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_create(client->secp_ctx, &pubkey, privkey)) {
        snprintf(client->error, sizeof(client->error), "invalid private key");
        return -1;
    }
    
    uint8_t pubkey_serialized[65];
    size_t pubkey_len = 65;
    secp256k1_ec_pubkey_serialize(client->secp_ctx, pubkey_serialized, &pubkey_len,
                                   &pubkey, SECP256K1_EC_UNCOMPRESSED);
    
    eth_address_from_pubkey(pubkey_serialized, client->address);
    client->has_privkey = 1;
    return 0;
}

uint64_t eth_client_chain_id(EthClient *client) {
    return client->chain_id;
}

const char *eth_client_error(EthClient *client) {
    return client->error;
}

uint8_t *eth_client_call(EthClient *client,
                         const uint8_t to[20],
                         const uint8_t *data, size_t data_len,
                         size_t *result_len) {
    cJSON *params = cJSON_CreateArray();
    cJSON *tx = cJSON_CreateObject();
    
    char *to_hex = eth_hex_encode(to, 20);
    char *data_hex = eth_hex_encode(data, data_len);
    
    cJSON_AddStringToObject(tx, "to", to_hex);
    cJSON_AddStringToObject(tx, "data", data_hex);
    cJSON_AddItemToArray(params, tx);
    cJSON_AddItemToArray(params, cJSON_CreateString("latest"));
    
    free(to_hex);
    free(data_hex);
    
    cJSON *response = rpc_call(client, "eth_call", params);
    if (!response) return NULL;
    
    cJSON *result = cJSON_GetObjectItem(response, "result");
    if (!result || !result->valuestring) {
        snprintf(client->error, sizeof(client->error), "no result in response");
        cJSON_Delete(response);
        return NULL;
    }
    
    /* Decode hex result */
    const char *hex = result->valuestring;
    if (hex[0] == '0' && hex[1] == 'x') hex += 2;
    
    size_t len = strlen(hex) / 2;
    uint8_t *out = malloc(len);
    if (!out) {
        cJSON_Delete(response);
        return NULL;
    }
    
    for (size_t i = 0; i < len; i++) {
        unsigned int byte;
        sscanf(hex + i * 2, "%02x", &byte);
        out[i] = (uint8_t)byte;
    }
    
    *result_len = len;
    cJSON_Delete(response);
    return out;
}

uint64_t eth_client_get_nonce(EthClient *client, const uint8_t address[20]) {
    cJSON *params = cJSON_CreateArray();
    char *addr_hex = eth_hex_encode(address, 20);
    cJSON_AddItemToArray(params, cJSON_CreateString(addr_hex));
    cJSON_AddItemToArray(params, cJSON_CreateString("pending"));
    free(addr_hex);
    
    cJSON *response = rpc_call(client, "eth_getTransactionCount", params);
    if (!response) return (uint64_t)-1;
    
    cJSON *result = cJSON_GetObjectItem(response, "result");
    uint64_t nonce = strtoull(result->valuestring, NULL, 16);
    cJSON_Delete(response);
    return nonce;
}

uint64_t eth_client_gas_price(EthClient *client) {
    cJSON *response = rpc_call(client, "eth_gasPrice", NULL);
    if (!response) return (uint64_t)-1;
    
    cJSON *result = cJSON_GetObjectItem(response, "result");
    uint64_t price = strtoull(result->valuestring, NULL, 16);
    cJSON_Delete(response);
    return price;
}

/* Sign transaction using EIP-155 */
static size_t sign_transaction(EthClient *client,
                               uint64_t nonce, uint64_t gas_price, uint64_t gas_limit,
                               const uint8_t to[20], uint64_t value,
                               const uint8_t *data, size_t data_len,
                               uint8_t *out) {
    /* Build unsigned tx for signing (EIP-155) */
    uint8_t items[1024];
    size_t items_len = 0;
    
    items_len += rlp_encode_uint64(nonce, items + items_len);
    items_len += rlp_encode_uint64(gas_price, items + items_len);
    items_len += rlp_encode_uint64(gas_limit, items + items_len);
    items_len += rlp_encode_bytes(to, 20, items + items_len);
    items_len += rlp_encode_uint64(value, items + items_len);
    items_len += rlp_encode_bytes(data, data_len, items + items_len);
    /* EIP-155: append chain_id, 0, 0 */
    items_len += rlp_encode_uint64(client->chain_id, items + items_len);
    items_len += rlp_encode_uint64(0, items + items_len);
    items_len += rlp_encode_uint64(0, items + items_len);
    
    /* RLP encode as list */
    uint8_t unsigned_tx[1200];
    size_t header_len = rlp_encode_list_header(items_len, unsigned_tx);
    memcpy(unsigned_tx + header_len, items, items_len);
    size_t unsigned_tx_len = header_len + items_len;
    
    /* Hash for signing */
    uint8_t hash[32];
    keccak256(unsigned_tx, unsigned_tx_len, hash);
    
    /* Sign */
    secp256k1_ecdsa_recoverable_signature sig;
    if (!secp256k1_ecdsa_sign_recoverable(client->secp_ctx, &sig, hash, 
                                           client->privkey, NULL, NULL)) {
        return 0;
    }
    
    uint8_t sig_bytes[64];
    int recid;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(client->secp_ctx,
                                                             sig_bytes, &recid, &sig);
    
    /* Build signed tx */
    items_len = 0;
    items_len += rlp_encode_uint64(nonce, items + items_len);
    items_len += rlp_encode_uint64(gas_price, items + items_len);
    items_len += rlp_encode_uint64(gas_limit, items + items_len);
    items_len += rlp_encode_bytes(to, 20, items + items_len);
    items_len += rlp_encode_uint64(value, items + items_len);
    items_len += rlp_encode_bytes(data, data_len, items + items_len);
    
    /* EIP-155 v = chain_id * 2 + 35 + recid */
    uint64_t v = client->chain_id * 2 + 35 + recid;
    items_len += rlp_encode_uint64(v, items + items_len);
    items_len += rlp_encode_bytes(sig_bytes, 32, items + items_len);      /* r */
    items_len += rlp_encode_bytes(sig_bytes + 32, 32, items + items_len); /* s */
    
    header_len = rlp_encode_list_header(items_len, out);
    memcpy(out + header_len, items, items_len);
    return header_len + items_len;
}

uint8_t *eth_client_send_raw_tx(EthClient *client,
                                const uint8_t *raw_tx, size_t tx_len) {
    cJSON *params = cJSON_CreateArray();
    char *tx_hex = eth_hex_encode(raw_tx, tx_len);
    cJSON_AddItemToArray(params, cJSON_CreateString(tx_hex));
    free(tx_hex);
    
    cJSON *response = rpc_call(client, "eth_sendRawTransaction", params);
    if (!response) return NULL;
    
    cJSON *result = cJSON_GetObjectItem(response, "result");
    if (!result || !result->valuestring) {
        snprintf(client->error, sizeof(client->error), "no tx hash in response");
        cJSON_Delete(response);
        return NULL;
    }
    
    uint8_t *tx_hash = malloc(32);
    eth_hex_decode(result->valuestring, tx_hash, 32);
    cJSON_Delete(response);
    return tx_hash;
}

uint8_t *eth_client_send_tx(EthClient *client,
                            const uint8_t to[20],
                            const uint8_t *data, size_t data_len,
                            uint64_t gas_limit,
                            uint64_t value) {
    if (!client->has_privkey) {
        snprintf(client->error, sizeof(client->error), "no private key set");
        return NULL;
    }
    
    uint64_t nonce = eth_client_get_nonce(client, client->address);
    if (nonce == (uint64_t)-1) return NULL;
    
    uint64_t gas_price = eth_client_gas_price(client);
    if (gas_price == (uint64_t)-1) return NULL;
    
    /* Add 10% to gas price */
    gas_price = gas_price + gas_price / 10;
    
    uint8_t raw_tx[2048];
    size_t raw_tx_len = sign_transaction(client, nonce, gas_price, gas_limit,
                                          to, value, data, data_len, raw_tx);
    if (raw_tx_len == 0) {
        snprintf(client->error, sizeof(client->error), "failed to sign transaction");
        return NULL;
    }
    
    return eth_client_send_raw_tx(client, raw_tx, raw_tx_len);
}

int eth_client_get_receipt(EthClient *client,
                           const uint8_t tx_hash[32],
                           int *status) {
    cJSON *params = cJSON_CreateArray();
    char *hash_hex = eth_hex_encode(tx_hash, 32);
    cJSON_AddItemToArray(params, cJSON_CreateString(hash_hex));
    free(hash_hex);
    
    cJSON *response = rpc_call(client, "eth_getTransactionReceipt", params);
    if (!response) return -1;
    
    cJSON *result = cJSON_GetObjectItem(response, "result");
    if (!result || cJSON_IsNull(result)) {
        cJSON_Delete(response);
        return 0; /* pending */
    }
    
    cJSON *status_json = cJSON_GetObjectItem(result, "status");
    if (status_json && status_json->valuestring) {
        *status = (strcmp(status_json->valuestring, "0x1") == 0) ? 1 : 0;
    }
    
    cJSON_Delete(response);
    return 1; /* mined */
}
