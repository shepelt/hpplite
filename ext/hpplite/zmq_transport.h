/*
** HPPLite ZeroMQ Transport Layer
**
** Handles node-to-node communication for M2+ (multi-process).
** Sequencer: PUB (broadcast batches) + ROUTER (receive attestations)
** Witness: SUB (receive batches) + DEALER (send attestations)
*/

#ifndef HPPLITE_ZMQ_TRANSPORT_H
#define HPPLITE_ZMQ_TRANSPORT_H

#ifdef HPPLITE_ENABLE_ZMQ

#include "batch.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** ZMQ Transport context
*/
typedef struct HppliteZmqTransport HppliteZmqTransport;

/*
** Transport configuration
*/
typedef struct HppliteZmqConfig {
    /* Sequencer endpoints (for sequencer to bind) */
    const char *pubEndpoint;      /* e.g., tcp bind address port 5555 */
    const char *routerEndpoint;   /* e.g., tcp bind address port 5556 */

    /* Witness endpoints (for witness to connect) */
    const char *sequencerPubAddr;    /* e.g., "tcp://localhost:5555" */
    const char *sequencerRouterAddr; /* e.g., "tcp://localhost:5556" */

    /* Identity for DEALER socket */
    const char *identity;

    /* Timeouts (milliseconds) */
    int recvTimeout;
    int sendTimeout;
} HppliteZmqConfig;

/*
** Message types for ZMQ protocol
*/
#define ZMQ_MSG_BATCH           0x01  /* Batch broadcast */
#define ZMQ_MSG_ATTESTATION     0x02  /* Per-batch attestation */
#define ZMQ_MSG_CHECKPOINT      0x03  /* Checkpoint proposal */
#define ZMQ_MSG_CP_ATTESTATION  0x04  /* Checkpoint attestation */
#define ZMQ_MSG_HEARTBEAT       0x05  /* Sequencer heartbeat */

/*
** Callback types
*/
typedef void (*HppliteOnBatchReceived)(
    void *arg,
    HppliteBatch *batch,
    const char *batchRef
);

typedef void (*HppliteOnAttestationReceived)(
    void *arg,
    const unsigned char *data,
    int dataLen
);

typedef void (*HppliteOnCheckpointReceived)(
    void *arg,
    HppliteCheckpoint *cp
);

typedef void (*HppliteOnCpAttestationReceived)(
    void *arg,
    const HppliteCheckpointAttestation *att
);

/*
** Create ZMQ transport for sequencer.
** Binds PUB and ROUTER sockets.
*/
HppliteZmqTransport *hpplite_zmq_create_sequencer(const HppliteZmqConfig *config);

/*
** Create ZMQ transport for witness.
** Connects SUB and DEALER sockets.
*/
HppliteZmqTransport *hpplite_zmq_create_witness(const HppliteZmqConfig *config);

/*
** Destroy transport and free resources.
*/
void hpplite_zmq_destroy(HppliteZmqTransport *transport);

/*
** Set callbacks for received messages.
*/
void hpplite_zmq_set_callbacks(
    HppliteZmqTransport *transport,
    void *arg,
    HppliteOnBatchReceived onBatch,
    HppliteOnAttestationReceived onAttestation,
    HppliteOnCheckpointReceived onCheckpoint,
    HppliteOnCpAttestationReceived onCpAttestation
);

/*
** Poll for incoming messages and dispatch to callbacks.
** Returns number of messages processed, or -1 on error.
*/
int hpplite_zmq_poll(HppliteZmqTransport *transport, int timeoutMs);

/*
** === Sequencer Operations ===
*/

/*
** Broadcast a batch to all witnesses.
** Returns 0 on success, -1 on error.
*/
int hpplite_zmq_broadcast_batch(
    HppliteZmqTransport *transport,
    HppliteBatch *batch,
    const char *batchRef
);

/*
** Broadcast a checkpoint proposal to all witnesses.
** Returns 0 on success, -1 on error.
*/
int hpplite_zmq_broadcast_checkpoint(
    HppliteZmqTransport *transport,
    HppliteCheckpoint *cp
);

/*
** Send a heartbeat to witnesses.
** Returns 0 on success, -1 on error.
*/
int hpplite_zmq_send_heartbeat(HppliteZmqTransport *transport);

/*
** === Witness Operations ===
*/

/*
** Send an attestation to the sequencer.
** Returns 0 on success, -1 on error.
*/
int hpplite_zmq_send_attestation(
    HppliteZmqTransport *transport,
    const unsigned char *attData,
    int attLen
);

/*
** Send a checkpoint attestation to the sequencer.
** Returns 0 on success, -1 on error.
*/
int hpplite_zmq_send_cp_attestation(
    HppliteZmqTransport *transport,
    const HppliteCheckpointAttestation *att
);

/*
** Get the ZMQ context (for advanced usage).
*/
void *hpplite_zmq_get_context(HppliteZmqTransport *transport);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_ENABLE_ZMQ */

#endif /* HPPLITE_ZMQ_TRANSPORT_H */
