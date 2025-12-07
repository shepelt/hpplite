/*
** HPPLite ZeroMQ Transport Layer Implementation
*/

#ifdef HPPLITE_ENABLE_ZMQ

#include "zmq_transport.h"
#include "node.h"
#include "sqlite3.h"
#include <zmq.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/*
** Internal transport structure
*/
struct HppliteZmqTransport {
    void *context;

    /* Sequencer sockets */
    void *pubSocket;      /* PUB - broadcast batches/checkpoints */
    void *routerSocket;   /* ROUTER - receive attestations */

    /* Witness sockets */
    void *subSocket;      /* SUB - receive batches/checkpoints */
    void *dealerSocket;   /* DEALER - send attestations */

    /* Configuration */
    int isSequencer;
    char *identity;

    /* Callbacks */
    void *callbackArg;
    HppliteOnBatchReceived onBatch;
    HppliteOnAttestationReceived onAttestation;
    HppliteOnCheckpointReceived onCheckpoint;
    HppliteOnCpAttestationReceived onCpAttestation;
};

/*
** Helper: duplicate string
*/
static char *zmq_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *dup = sqlite3_malloc((int)len);
    if (dup) memcpy(dup, s, len);
    return dup;
}

/*
** Create ZMQ transport for sequencer
*/
HppliteZmqTransport *hpplite_zmq_create_sequencer(const HppliteZmqConfig *config) {
    HppliteZmqTransport *t;
    int rc;

    if (!config) return NULL;

    t = sqlite3_malloc(sizeof(HppliteZmqTransport));
    if (!t) return NULL;
    memset(t, 0, sizeof(HppliteZmqTransport));

    t->isSequencer = 1;
    t->identity = zmq_strdup(config->identity);

    /* Create ZMQ context */
    t->context = zmq_ctx_new();
    if (!t->context) {
        sqlite3_free(t->identity);
        sqlite3_free(t);
        return NULL;
    }

    /* Create PUB socket for broadcasting */
    t->pubSocket = zmq_socket(t->context, ZMQ_PUB);
    if (!t->pubSocket) {
        zmq_ctx_destroy(t->context);
        sqlite3_free(t->identity);
        sqlite3_free(t);
        return NULL;
    }

    /* Bind PUB socket */
    rc = zmq_bind(t->pubSocket, config->pubEndpoint);
    if (rc != 0) {
        fprintf(stderr, "ZMQ: Failed to bind PUB socket to %s: %s\n",
            config->pubEndpoint, zmq_strerror(zmq_errno()));
        zmq_close(t->pubSocket);
        zmq_ctx_destroy(t->context);
        sqlite3_free(t->identity);
        sqlite3_free(t);
        return NULL;
    }

    /* Create ROUTER socket for receiving attestations */
    t->routerSocket = zmq_socket(t->context, ZMQ_ROUTER);
    if (!t->routerSocket) {
        zmq_close(t->pubSocket);
        zmq_ctx_destroy(t->context);
        sqlite3_free(t->identity);
        sqlite3_free(t);
        return NULL;
    }

    /* Bind ROUTER socket */
    rc = zmq_bind(t->routerSocket, config->routerEndpoint);
    if (rc != 0) {
        fprintf(stderr, "ZMQ: Failed to bind ROUTER socket to %s: %s\n",
            config->routerEndpoint, zmq_strerror(zmq_errno()));
        zmq_close(t->routerSocket);
        zmq_close(t->pubSocket);
        zmq_ctx_destroy(t->context);
        sqlite3_free(t->identity);
        sqlite3_free(t);
        return NULL;
    }

    /* Set receive timeout if specified */
    if (config->recvTimeout > 0) {
        zmq_setsockopt(t->routerSocket, ZMQ_RCVTIMEO, &config->recvTimeout, sizeof(int));
    }

    return t;
}

/*
** Create ZMQ transport for witness
*/
HppliteZmqTransport *hpplite_zmq_create_witness(const HppliteZmqConfig *config) {
    HppliteZmqTransport *t;
    int rc;

    if (!config) return NULL;

    t = sqlite3_malloc(sizeof(HppliteZmqTransport));
    if (!t) return NULL;
    memset(t, 0, sizeof(HppliteZmqTransport));

    t->isSequencer = 0;
    t->identity = zmq_strdup(config->identity);

    /* Create ZMQ context */
    t->context = zmq_ctx_new();
    if (!t->context) {
        sqlite3_free(t->identity);
        sqlite3_free(t);
        return NULL;
    }

    /* Create SUB socket for receiving broadcasts */
    t->subSocket = zmq_socket(t->context, ZMQ_SUB);
    if (!t->subSocket) {
        zmq_ctx_destroy(t->context);
        sqlite3_free(t->identity);
        sqlite3_free(t);
        return NULL;
    }

    /* Subscribe to all messages */
    zmq_setsockopt(t->subSocket, ZMQ_SUBSCRIBE, "", 0);

    /* Connect to sequencer PUB */
    rc = zmq_connect(t->subSocket, config->sequencerPubAddr);
    if (rc != 0) {
        fprintf(stderr, "ZMQ: Failed to connect SUB socket to %s: %s\n",
            config->sequencerPubAddr, zmq_strerror(zmq_errno()));
        zmq_close(t->subSocket);
        zmq_ctx_destroy(t->context);
        sqlite3_free(t->identity);
        sqlite3_free(t);
        return NULL;
    }

    /* Create DEALER socket for sending attestations */
    t->dealerSocket = zmq_socket(t->context, ZMQ_DEALER);
    if (!t->dealerSocket) {
        zmq_close(t->subSocket);
        zmq_ctx_destroy(t->context);
        sqlite3_free(t->identity);
        sqlite3_free(t);
        return NULL;
    }

    /* Set identity for DEALER socket */
    if (config->identity) {
        zmq_setsockopt(t->dealerSocket, ZMQ_IDENTITY,
            config->identity, strlen(config->identity));
    }

    /* Connect to sequencer ROUTER */
    rc = zmq_connect(t->dealerSocket, config->sequencerRouterAddr);
    if (rc != 0) {
        fprintf(stderr, "ZMQ: Failed to connect DEALER socket to %s: %s\n",
            config->sequencerRouterAddr, zmq_strerror(zmq_errno()));
        zmq_close(t->dealerSocket);
        zmq_close(t->subSocket);
        zmq_ctx_destroy(t->context);
        sqlite3_free(t->identity);
        sqlite3_free(t);
        return NULL;
    }

    /* Set receive timeout if specified */
    if (config->recvTimeout > 0) {
        zmq_setsockopt(t->subSocket, ZMQ_RCVTIMEO, &config->recvTimeout, sizeof(int));
    }

    return t;
}

/*
** Destroy transport
*/
void hpplite_zmq_destroy(HppliteZmqTransport *t) {
    if (!t) return;

    if (t->pubSocket) zmq_close(t->pubSocket);
    if (t->routerSocket) zmq_close(t->routerSocket);
    if (t->subSocket) zmq_close(t->subSocket);
    if (t->dealerSocket) zmq_close(t->dealerSocket);

    if (t->context) zmq_ctx_destroy(t->context);

    sqlite3_free(t->identity);
    sqlite3_free(t);
}

/*
** Set callbacks
*/
void hpplite_zmq_set_callbacks(
    HppliteZmqTransport *t,
    void *arg,
    HppliteOnBatchReceived onBatch,
    HppliteOnAttestationReceived onAttestation,
    HppliteOnCheckpointReceived onCheckpoint,
    HppliteOnCpAttestationReceived onCpAttestation
) {
    if (!t) return;
    t->callbackArg = arg;
    t->onBatch = onBatch;
    t->onAttestation = onAttestation;
    t->onCheckpoint = onCheckpoint;
    t->onCpAttestation = onCpAttestation;
}

/*
** Handle received message based on type
*/
static void handle_message(HppliteZmqTransport *t, unsigned char *data, int size) {
    if (size < 1) return;

    unsigned char msgType = data[0];

    switch (msgType) {
        case ZMQ_MSG_BATCH: {
            if (t->onBatch) {
                char *batchRef = NULL;
                HppliteBatch *batch = hpplite_node_parse_batch_msg(data, size, &batchRef);
                if (batch) {
                    t->onBatch(t->callbackArg, batch, batchRef);
                    hpplite_batch_free(batch);
                    sqlite3_free(batchRef);
                }
            }
            break;
        }
        case ZMQ_MSG_ATTESTATION: {
            if (t->onAttestation) {
                t->onAttestation(t->callbackArg, data, size);
            }
            break;
        }
        case ZMQ_MSG_CHECKPOINT: {
            if (t->onCheckpoint) {
                /* Parse checkpoint from message */
                char *json = (char*)(data + 1);
                HppliteCheckpoint *cp = hpplite_checkpoint_from_json(json);
                if (cp) {
                    t->onCheckpoint(t->callbackArg, cp);
                    /* Note: callback takes ownership or frees */
                }
            }
            break;
        }
        case ZMQ_MSG_CP_ATTESTATION: {
            if (t->onCpAttestation && size >= sizeof(HppliteCheckpointAttestation) + 1) {
                HppliteCheckpointAttestation *att =
                    (HppliteCheckpointAttestation*)(data + 1);
                t->onCpAttestation(t->callbackArg, att);
            }
            break;
        }
        case ZMQ_MSG_HEARTBEAT: {
            /* Heartbeat - just update last seen time */
            break;
        }
    }
}

/*
** Poll for incoming messages
*/
int hpplite_zmq_poll(HppliteZmqTransport *t, int timeoutMs) {
    zmq_pollitem_t items[2];
    int nItems = 0;
    int processed = 0;
    int rc;

    if (!t) return -1;

    if (t->isSequencer) {
        /* Sequencer polls ROUTER socket for attestations */
        items[0].socket = t->routerSocket;
        items[0].events = ZMQ_POLLIN;
        nItems = 1;
    } else {
        /* Witness polls SUB socket for batches */
        items[0].socket = t->subSocket;
        items[0].events = ZMQ_POLLIN;
        nItems = 1;
    }

    rc = zmq_poll(items, nItems, timeoutMs);
    if (rc < 0) {
        return -1;
    }

    if (rc > 0) {
        if (items[0].revents & ZMQ_POLLIN) {
            zmq_msg_t msg;
            zmq_msg_init(&msg);

            if (t->isSequencer) {
                /* ROUTER: first frame is identity, second is empty, third is data */
                zmq_msg_t identity, empty;
                zmq_msg_init(&identity);
                zmq_msg_init(&empty);

                if (zmq_msg_recv(&identity, t->routerSocket, 0) >= 0 &&
                    zmq_msg_recv(&empty, t->routerSocket, 0) >= 0 &&
                    zmq_msg_recv(&msg, t->routerSocket, 0) >= 0) {

                    handle_message(t, zmq_msg_data(&msg), (int)zmq_msg_size(&msg));
                    processed++;
                }

                zmq_msg_close(&identity);
                zmq_msg_close(&empty);
            } else {
                /* SUB: data is the message */
                if (zmq_msg_recv(&msg, t->subSocket, 0) >= 0) {
                    handle_message(t, zmq_msg_data(&msg), (int)zmq_msg_size(&msg));
                    processed++;
                }
            }

            zmq_msg_close(&msg);
        }
    }

    return processed;
}

/*
** Broadcast batch (sequencer)
*/
int hpplite_zmq_broadcast_batch(
    HppliteZmqTransport *t,
    HppliteBatch *batch,
    const char *batchRef
) {
    int msgSize;
    unsigned char *msg;
    int rc;

    if (!t || !t->pubSocket || !batch || !batchRef) return -1;

    /* Serialize batch message (already includes type byte) */
    msg = hpplite_node_serialize_batch_msg(NULL, batch, batchRef, &msgSize);
    if (!msg) return -1;

    /* Send on PUB socket */
    rc = zmq_send(t->pubSocket, msg, msgSize, 0);
    sqlite3_free(msg);

    return (rc >= 0) ? 0 : -1;
}

/*
** Broadcast checkpoint (sequencer)
*/
int hpplite_zmq_broadcast_checkpoint(
    HppliteZmqTransport *t,
    HppliteCheckpoint *cp
) {
    char *json;
    int jsonLen;
    unsigned char *msg;
    int rc;

    if (!t || !t->pubSocket || !cp) return -1;

    /* Serialize checkpoint to JSON */
    json = hpplite_checkpoint_to_json(cp);
    if (!json) return -1;

    jsonLen = (int)strlen(json);

    /* Build message: type + json */
    msg = sqlite3_malloc(1 + jsonLen + 1);
    if (!msg) {
        sqlite3_free(json);
        return -1;
    }

    msg[0] = ZMQ_MSG_CHECKPOINT;
    memcpy(msg + 1, json, jsonLen + 1);  /* Include null terminator */
    sqlite3_free(json);

    /* Send on PUB socket */
    rc = zmq_send(t->pubSocket, msg, 1 + jsonLen + 1, 0);
    sqlite3_free(msg);

    return (rc >= 0) ? 0 : -1;
}

/*
** Send heartbeat (sequencer)
*/
int hpplite_zmq_send_heartbeat(HppliteZmqTransport *t) {
    unsigned char msg[1] = { ZMQ_MSG_HEARTBEAT };

    if (!t || !t->pubSocket) return -1;

    return (zmq_send(t->pubSocket, msg, 1, 0) >= 0) ? 0 : -1;
}

/*
** Send attestation (witness)
*/
int hpplite_zmq_send_attestation(
    HppliteZmqTransport *t,
    const unsigned char *attData,
    int attLen
) {
    int rc;

    if (!t || !t->dealerSocket || !attData) return -1;

    /* DEALER: send empty frame then data */
    rc = zmq_send(t->dealerSocket, "", 0, ZMQ_SNDMORE);
    if (rc < 0) return -1;

    rc = zmq_send(t->dealerSocket, attData, attLen, 0);
    return (rc >= 0) ? 0 : -1;
}

/*
** Send checkpoint attestation (witness)
*/
int hpplite_zmq_send_cp_attestation(
    HppliteZmqTransport *t,
    const HppliteCheckpointAttestation *att
) {
    unsigned char *msg;
    int msgLen;
    int rc;

    if (!t || !t->dealerSocket || !att) return -1;

    /* Build message: type + attestation struct */
    msgLen = 1 + sizeof(HppliteCheckpointAttestation);
    msg = sqlite3_malloc(msgLen);
    if (!msg) return -1;

    msg[0] = ZMQ_MSG_CP_ATTESTATION;
    memcpy(msg + 1, att, sizeof(HppliteCheckpointAttestation));

    /* DEALER: send empty frame then data */
    rc = zmq_send(t->dealerSocket, "", 0, ZMQ_SNDMORE);
    if (rc < 0) {
        sqlite3_free(msg);
        return -1;
    }

    rc = zmq_send(t->dealerSocket, msg, msgLen, 0);
    sqlite3_free(msg);

    return (rc >= 0) ? 0 : -1;
}

/*
** Get ZMQ context
*/
void *hpplite_zmq_get_context(HppliteZmqTransport *t) {
    return t ? t->context : NULL;
}

#endif /* HPPLITE_ENABLE_ZMQ */
