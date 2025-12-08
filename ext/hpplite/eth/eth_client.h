/*
** Minimal Ethereum JSON-RPC client for HPPLite
** Uses libcurl for HTTP, cJSON for JSON parsing
*/

#ifndef ETH_CLIENT_H
#define ETH_CLIENT_H

#include <stdint.h>
#include <stddef.h>

/* Client handle */
typedef struct EthClient EthClient;

/* Create client connected to RPC endpoint */
EthClient *eth_client_create(const char *rpc_url, uint64_t chain_id);

/* Destroy client */
void eth_client_destroy(EthClient *client);

/* Set private key for signing transactions */
int eth_client_set_privkey(EthClient *client, const uint8_t privkey[32]);

/* Get chain ID */
uint64_t eth_client_chain_id(EthClient *client);

/* === Read operations (eth_call) === */

/* Call contract function, returns allocated result (caller frees) */
uint8_t *eth_client_call(EthClient *client,
                         const uint8_t to[20],
                         const uint8_t *data, size_t data_len,
                         size_t *result_len);

/* === Write operations === */

/* Get nonce for address */
uint64_t eth_client_get_nonce(EthClient *client, const uint8_t address[20]);

/* Get current gas price */
uint64_t eth_client_gas_price(EthClient *client);

/* Send raw transaction, returns tx hash (32 bytes) or NULL on error */
uint8_t *eth_client_send_raw_tx(EthClient *client,
                                const uint8_t *raw_tx, size_t tx_len);

/* Send transaction (builds, signs, sends)
   Returns tx hash (32 bytes, caller frees) or NULL on error */
uint8_t *eth_client_send_tx(EthClient *client,
                            const uint8_t to[20],
                            const uint8_t *data, size_t data_len,
                            uint64_t gas_limit,
                            uint64_t value);

/* Get transaction receipt, returns 1 if mined, 0 if pending, -1 on error
   If mined, fills in status (1 = success, 0 = reverted) */
int eth_client_get_receipt(EthClient *client,
                           const uint8_t tx_hash[32],
                           int *status);

/* === Utility === */

/* Get last error message */
const char *eth_client_error(EthClient *client);

/* Hex encode/decode utilities */
char *eth_hex_encode(const uint8_t *data, size_t len);
int eth_hex_decode(const char *hex, uint8_t *out, size_t out_len);

/* Derive Ethereum address from compressed (33-byte) public key */
int eth_address_from_compressed_pubkey(const uint8_t pubkey[33], uint8_t address[20]);

/* Derive Ethereum address from private key */
int eth_address_from_privkey(const uint8_t privkey[32], uint8_t address[20]);

#endif /* ETH_CLIENT_H */
