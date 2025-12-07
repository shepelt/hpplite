/*
** HPPLite L1 Mock Service
**
** ZeroMQ-based L1 mock service for M2 multi-process testing.
** Provides REQ/REP interface for L1 operations.
*/

#ifndef HPPLITE_L1_SERVICE_H
#define HPPLITE_L1_SERVICE_H

#ifdef HPPLITE_ENABLE_ZMQ

#include "l1_interface.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** Message types for L1 service protocol
*/
#define L1_MSG_GET_STATE           0x01
#define L1_MSG_SET_SEQUENCER       0x02
#define L1_MSG_REGISTER_WITNESS    0x03
#define L1_MSG_UNREGISTER_WITNESS  0x04
#define L1_MSG_SUBMIT_CHECKPOINT   0x05
#define L1_MSG_GET_CHECKPOINT      0x06
#define L1_MSG_IS_SEQUENCER        0x07
#define L1_MSG_IS_WITNESS          0x08
#define L1_MSG_GET_SEQUENCER       0x09
#define L1_MSG_CHECK_TIMEOUT       0x0A  /* Check if sequencer timeout expired */
#define L1_MSG_CLAIM_SEQUENCER     0x0B  /* Claim sequencer role */
#define L1_MSG_ADVANCE_TIME        0x0C  /* Advance mock time (testing) */
#define L1_MSG_SET_CONFIG          0x0D  /* Set mock configuration */

/*
** Response status codes
*/
#define L1_STATUS_OK               0x00
#define L1_STATUS_ERROR            0x01
#define L1_STATUS_NOT_FOUND        0x02
#define L1_STATUS_INVALID          0x03

/*
** L1 Service context (server side)
*/
typedef struct HppliteL1Service HppliteL1Service;

/*
** L1 Client context (node side)
*/
typedef struct HppliteL1Client HppliteL1Client;

/*
** === Server API ===
*/

/*
** Create L1 service bound to the given endpoint.
** e.g., endpoint = "tcp://localhost:5560"
** Returns NULL on error.
*/
HppliteL1Service *hpplite_l1_service_create(const char *endpoint);

/*
** Destroy L1 service and free resources.
*/
void hpplite_l1_service_destroy(HppliteL1Service *svc);

/*
** Process incoming requests (blocking up to timeoutMs).
** Returns number of requests processed, or -1 on error.
*/
int hpplite_l1_service_process(HppliteL1Service *svc, int timeoutMs);

/*
** Run service in a loop until stopped.
** Call from dedicated thread or process.
*/
void hpplite_l1_service_run(HppliteL1Service *svc);

/*
** Signal service to stop.
*/
void hpplite_l1_service_stop(HppliteL1Service *svc);

/*
** Get underlying L1 mock (for direct access in tests).
*/
HppliteL1 *hpplite_l1_service_get_l1(HppliteL1Service *svc);

/*
** === Client API ===
*/

/*
** Create L1 client connecting to the given endpoint.
** e.g., endpoint = "tcp://localhost:5560"
** Returns NULL on error.
*/
HppliteL1Client *hpplite_l1_client_create(const char *endpoint);

/*
** Destroy L1 client.
*/
void hpplite_l1_client_destroy(HppliteL1Client *client);

/*
** Get L1 state from service.
** Caller must free with hpplite_l1_state_free().
*/
HppliteL1State *hpplite_l1_client_get_state(HppliteL1Client *client);

/*
** Check if pubkey is the sequencer.
*/
int hpplite_l1_client_is_sequencer(HppliteL1Client *client, const unsigned char *pubkey);

/*
** Check if pubkey is a registered witness.
*/
int hpplite_l1_client_is_witness(HppliteL1Client *client, const unsigned char *pubkey);

/*
** Get current sequencer pubkey.
** Returns allocated buffer (caller frees with sqlite3_free) or NULL.
*/
unsigned char *hpplite_l1_client_get_sequencer(HppliteL1Client *client);

/*
** Register as witness.
** Returns 0 on success, -1 on error.
*/
int hpplite_l1_client_register_witness(HppliteL1Client *client, const unsigned char *pubkey);

/*
** Unregister witness.
** Returns 0 on success, -1 on error.
*/
int hpplite_l1_client_unregister_witness(HppliteL1Client *client, const unsigned char *pubkey);

/*
** Set sequencer.
** Returns 0 on success, -1 on error.
*/
int hpplite_l1_client_set_sequencer(HppliteL1Client *client, const unsigned char *pubkey);

/*
** Submit checkpoint.
** Returns transaction hash (caller frees) or NULL on error.
*/
char *hpplite_l1_client_submit_checkpoint(
    HppliteL1Client *client,
    HppliteCheckpoint *cp,
    HppliteCheckpointAttestation *attestations,
    int nAttestations
);

/*
** Get checkpoint by index.
** Returns checkpoint (caller frees) or NULL if not found.
*/
HppliteCheckpoint *hpplite_l1_client_get_checkpoint(HppliteL1Client *client, int index);

/*
** Check if sequencer timeout has expired.
** Returns 1 if expired, 0 otherwise.
*/
int hpplite_l1_client_check_timeout(HppliteL1Client *client);

/*
** Claim sequencer role (after timeout or if none set).
** Returns 0 on success, -1 on failure.
*/
int hpplite_l1_client_claim_sequencer(HppliteL1Client *client, const unsigned char *pubkey);

/*
** Advance mock time (for testing).
** Returns 0 on success, -1 on failure.
*/
int hpplite_l1_client_advance_time(HppliteL1Client *client, uint64_t seconds);

/*
** Set mock configuration (for testing).
** Returns 0 on success, -1 on failure.
*/
int hpplite_l1_client_set_config(
    HppliteL1Client *client,
    int requiredAttestations,
    uint64_t checkpointInterval,
    uint64_t sequencerTimeout
);

/*
** === L1 Interface Wrapper ===
** These functions create an HppliteL1 that wraps a client connection.
*/

/*
** Create L1 interface backed by ZMQ client.
** The resulting HppliteL1 can be used with existing node code.
*/
HppliteL1 *hpplite_l1_create_remote(const char *endpoint);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_ENABLE_ZMQ */

#endif /* HPPLITE_L1_SERVICE_H */
