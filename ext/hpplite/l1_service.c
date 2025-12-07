/*
** HPPLite L1 Mock Service Implementation
**
** ZeroMQ-based L1 mock service for M2 multi-process testing.
*/

#ifdef HPPLITE_ENABLE_ZMQ

#include "l1_service.h"
#include "l1_interface.h"
#include "batch.h"
#include "crypto.h"
#include "sqlite3.h"
#include <zmq.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/*
** Service context
*/
struct HppliteL1Service {
    void *zmqContext;
    void *zmqSocket;    /* REP socket */
    HppliteL1 *l1;      /* Underlying L1 mock */
    int running;
};

/*
** Client context
*/
struct HppliteL1Client {
    void *zmqContext;
    void *zmqSocket;    /* REQ socket */
};

/*
** Remote L1 wrapper (implements HppliteL1 interface via ZMQ)
*/
typedef struct {
    HppliteL1Client *client;
} HppliteL1Remote;

/*
** === Server Implementation ===
*/

HppliteL1Service *hpplite_l1_service_create(const char *endpoint) {
    HppliteL1Service *svc;
    int rc;

    if (!endpoint) return NULL;

    svc = sqlite3_malloc(sizeof(HppliteL1Service));
    if (!svc) return NULL;
    memset(svc, 0, sizeof(HppliteL1Service));

    /* Create ZMQ context and REP socket */
    svc->zmqContext = zmq_ctx_new();
    if (!svc->zmqContext) {
        sqlite3_free(svc);
        return NULL;
    }

    svc->zmqSocket = zmq_socket(svc->zmqContext, ZMQ_REP);
    if (!svc->zmqSocket) {
        zmq_ctx_destroy(svc->zmqContext);
        sqlite3_free(svc);
        return NULL;
    }

    rc = zmq_bind(svc->zmqSocket, endpoint);
    if (rc != 0) {
        fprintf(stderr, "L1 Service: Failed to bind to %s: %s\n",
            endpoint, zmq_strerror(zmq_errno()));
        zmq_close(svc->zmqSocket);
        zmq_ctx_destroy(svc->zmqContext);
        sqlite3_free(svc);
        return NULL;
    }

    /* Create underlying L1 mock */
    svc->l1 = hpplite_l1_create_mock();
    if (!svc->l1) {
        zmq_close(svc->zmqSocket);
        zmq_ctx_destroy(svc->zmqContext);
        sqlite3_free(svc);
        return NULL;
    }

    svc->running = 0;
    return svc;
}

void hpplite_l1_service_destroy(HppliteL1Service *svc) {
    if (!svc) return;

    svc->running = 0;

    if (svc->l1) {
        hpplite_l1_disconnect(svc->l1);
    }
    if (svc->zmqSocket) {
        zmq_close(svc->zmqSocket);
    }
    if (svc->zmqContext) {
        zmq_ctx_destroy(svc->zmqContext);
    }

    sqlite3_free(svc);
}

/*
** Handle a single request and send response
*/
static int handle_request(HppliteL1Service *svc, unsigned char *data, int size) {
    unsigned char response[4096];
    int respLen = 0;

    if (size < 1) {
        response[0] = L1_STATUS_INVALID;
        zmq_send(svc->zmqSocket, response, 1, 0);
        return -1;
    }

    unsigned char msgType = data[0];

    switch (msgType) {
        case L1_MSG_GET_STATE: {
            HppliteL1State *state = hpplite_l1_get_state(svc->l1);
            if (state) {
                response[0] = L1_STATUS_OK;
                respLen = 1;

                /* Pack state: nWitnesses(4) + requiredAtts(4) + sequencerPubkey(33) */
                response[respLen++] = (state->nWitnesses >> 24) & 0xFF;
                response[respLen++] = (state->nWitnesses >> 16) & 0xFF;
                response[respLen++] = (state->nWitnesses >> 8) & 0xFF;
                response[respLen++] = state->nWitnesses & 0xFF;

                response[respLen++] = (state->requiredAttestations >> 24) & 0xFF;
                response[respLen++] = (state->requiredAttestations >> 16) & 0xFF;
                response[respLen++] = (state->requiredAttestations >> 8) & 0xFF;
                response[respLen++] = state->requiredAttestations & 0xFF;

                /* Sequencer pubkey (fixed array in struct) */
                int hasSequencer = 0;
                for (int i = 0; i < HPPLITE_PUBKEY_SIZE; i++) {
                    if (state->sequencerPubkey[i] != 0) {
                        hasSequencer = 1;
                        break;
                    }
                }
                response[respLen++] = hasSequencer ? 1 : 0;
                if (hasSequencer) {
                    memcpy(response + respLen, state->sequencerPubkey, HPPLITE_PUBKEY_SIZE);
                    respLen += HPPLITE_PUBKEY_SIZE;
                }

                /* Witness pubkeys */
                for (int i = 0; i < state->nWitnesses && respLen + HPPLITE_PUBKEY_SIZE < (int)sizeof(response); i++) {
                    memcpy(response + respLen, state->witnessPubkeys[i], HPPLITE_PUBKEY_SIZE);
                    respLen += HPPLITE_PUBKEY_SIZE;
                }

                hpplite_l1_state_free(state);
            } else {
                response[0] = L1_STATUS_ERROR;
                respLen = 1;
            }
            break;
        }

        case L1_MSG_IS_SEQUENCER: {
            if (size < 1 + HPPLITE_PUBKEY_SIZE) {
                response[0] = L1_STATUS_INVALID;
                respLen = 1;
            } else {
                int result = hpplite_l1_is_sequencer(svc->l1, data + 1);
                response[0] = L1_STATUS_OK;
                response[1] = result ? 1 : 0;
                respLen = 2;
            }
            break;
        }

        case L1_MSG_IS_WITNESS: {
            if (size < 1 + HPPLITE_PUBKEY_SIZE) {
                response[0] = L1_STATUS_INVALID;
                respLen = 1;
            } else {
                int result = hpplite_l1_is_witness(svc->l1, data + 1);
                response[0] = L1_STATUS_OK;
                response[1] = result ? 1 : 0;
                respLen = 2;
            }
            break;
        }

        case L1_MSG_GET_SEQUENCER: {
            HppliteL1State *state = hpplite_l1_get_state(svc->l1);
            if (state) {
                int hasSequencer = 0;
                for (int i = 0; i < HPPLITE_PUBKEY_SIZE; i++) {
                    if (state->sequencerPubkey[i] != 0) {
                        hasSequencer = 1;
                        break;
                    }
                }
                if (hasSequencer) {
                    response[0] = L1_STATUS_OK;
                    memcpy(response + 1, state->sequencerPubkey, HPPLITE_PUBKEY_SIZE);
                    respLen = 1 + HPPLITE_PUBKEY_SIZE;
                } else {
                    response[0] = L1_STATUS_NOT_FOUND;
                    respLen = 1;
                }
                hpplite_l1_state_free(state);
            } else {
                response[0] = L1_STATUS_ERROR;
                respLen = 1;
            }
            break;
        }

        case L1_MSG_SET_SEQUENCER: {
            if (size < 1 + HPPLITE_PUBKEY_SIZE) {
                response[0] = L1_STATUS_INVALID;
                respLen = 1;
            } else {
                hpplite_l1_mock_set_sequencer(svc->l1, data + 1);
                response[0] = L1_STATUS_OK;
                respLen = 1;
            }
            break;
        }

        case L1_MSG_REGISTER_WITNESS: {
            if (size < 1 + HPPLITE_PUBKEY_SIZE) {
                response[0] = L1_STATUS_INVALID;
                respLen = 1;
            } else {
                hpplite_l1_mock_add_witness(svc->l1, data + 1);
                response[0] = L1_STATUS_OK;
                respLen = 1;
            }
            break;
        }

        case L1_MSG_UNREGISTER_WITNESS: {
            /* Not implemented in mock yet */
            response[0] = L1_STATUS_ERROR;
            respLen = 1;
            break;
        }

        case L1_MSG_SUBMIT_CHECKPOINT: {
            /* Parse checkpoint from message */
            int offset = 1;

            /* Get JSON length */
            if (size < offset + 4) {
                response[0] = L1_STATUS_INVALID;
                respLen = 1;
                break;
            }
            int jsonLen = ((int)data[offset] << 24) | ((int)data[offset+1] << 16) |
                          ((int)data[offset+2] << 8) | data[offset+3];
            offset += 4;

            if (size < offset + jsonLen + 4) {
                response[0] = L1_STATUS_INVALID;
                respLen = 1;
                break;
            }

            /* Parse checkpoint JSON */
            char *jsonStr = sqlite3_malloc(jsonLen + 1);
            if (!jsonStr) {
                response[0] = L1_STATUS_ERROR;
                respLen = 1;
                break;
            }
            memcpy(jsonStr, data + offset, jsonLen);
            jsonStr[jsonLen] = '\0';
            offset += jsonLen;

            HppliteCheckpoint *cp = hpplite_checkpoint_from_json(jsonStr);
            sqlite3_free(jsonStr);

            if (!cp) {
                response[0] = L1_STATUS_INVALID;
                respLen = 1;
                break;
            }

            /* Get number of attestations */
            int nAtts = ((int)data[offset] << 24) | ((int)data[offset+1] << 16) |
                        ((int)data[offset+2] << 8) | data[offset+3];
            offset += 4;

            /* Parse attestations */
            HppliteCheckpointAttestation *atts = NULL;
            if (nAtts > 0) {
                int attSize = nAtts * (int)sizeof(HppliteCheckpointAttestation);
                if (size < offset + attSize) {
                    hpplite_checkpoint_free(cp);
                    response[0] = L1_STATUS_INVALID;
                    respLen = 1;
                    break;
                }
                atts = sqlite3_malloc(attSize);
                if (!atts) {
                    hpplite_checkpoint_free(cp);
                    response[0] = L1_STATUS_ERROR;
                    respLen = 1;
                    break;
                }
                memcpy(atts, data + offset, attSize);
            }

            /* Submit checkpoint */
            char *txHash = hpplite_l1_submit_checkpoint(svc->l1, cp, atts, nAtts);

            if (atts) sqlite3_free(atts);
            hpplite_checkpoint_free(cp);

            if (txHash) {
                int hashLen = (int)strlen(txHash);
                response[0] = L1_STATUS_OK;
                response[1] = (hashLen >> 8) & 0xFF;
                response[2] = hashLen & 0xFF;
                memcpy(response + 3, txHash, hashLen);
                respLen = 3 + hashLen;
                sqlite3_free(txHash);
            } else {
                response[0] = L1_STATUS_ERROR;
                respLen = 1;
            }
            break;
        }

        case L1_MSG_GET_CHECKPOINT: {
            if (size < 5) {
                response[0] = L1_STATUS_INVALID;
                respLen = 1;
            } else {
                int index = ((int)data[1] << 24) | ((int)data[2] << 16) |
                            ((int)data[3] << 8) | data[4];
                int cpCount = hpplite_l1_mock_get_checkpoint_count(svc->l1);
                if (index < 0 || index >= cpCount) {
                    response[0] = L1_STATUS_NOT_FOUND;
                    respLen = 1;
                } else {
                    const HppliteCheckpoint *cp = hpplite_l1_mock_get_last_checkpoint(svc->l1);
                    /* Note: This only gives last checkpoint, not by index */
                    /* For full implementation, need to add get-by-index to mock */
                    if (cp && index == cpCount - 1) {
                        char *json = hpplite_checkpoint_to_json((HppliteCheckpoint*)cp);
                        if (json) {
                            int jsonLen = (int)strlen(json);
                            response[0] = L1_STATUS_OK;
                            response[1] = (jsonLen >> 24) & 0xFF;
                            response[2] = (jsonLen >> 16) & 0xFF;
                            response[3] = (jsonLen >> 8) & 0xFF;
                            response[4] = jsonLen & 0xFF;
                            memcpy(response + 5, json, jsonLen);
                            respLen = 5 + jsonLen;
                            sqlite3_free(json);
                        } else {
                            response[0] = L1_STATUS_ERROR;
                            respLen = 1;
                        }
                    } else {
                        response[0] = L1_STATUS_NOT_FOUND;
                        respLen = 1;
                    }
                }
            }
            break;
        }

        case L1_MSG_CHECK_TIMEOUT: {
            int expired = hpplite_l1_sequencer_timeout_expired(svc->l1);
            response[0] = L1_STATUS_OK;
            response[1] = expired ? 1 : 0;
            respLen = 2;
            break;
        }

        case L1_MSG_CLAIM_SEQUENCER: {
            if (size < 1 + HPPLITE_PUBKEY_SIZE) {
                response[0] = L1_STATUS_INVALID;
                respLen = 1;
            } else {
                /* For mock, we use the privkey directly (pubkey is passed) */
                /* hpplite_l1_claim_sequencer expects privkey but for testing we just set directly */
                int canClaim = !hpplite_l1_is_sequencer(svc->l1, data + 1) &&
                               (hpplite_l1_sequencer_timeout_expired(svc->l1) ||
                                !hpplite_l1_get_state(svc->l1)->sequencerPubkey[0]);
                if (canClaim) {
                    hpplite_l1_mock_set_sequencer(svc->l1, data + 1);
                    response[0] = L1_STATUS_OK;
                } else {
                    response[0] = L1_STATUS_ERROR;
                }
                respLen = 1;
            }
            break;
        }

        case L1_MSG_ADVANCE_TIME: {
            if (size < 9) {
                response[0] = L1_STATUS_INVALID;
                respLen = 1;
            } else {
                uint64_t seconds = ((uint64_t)data[1] << 56) | ((uint64_t)data[2] << 48) |
                                   ((uint64_t)data[3] << 40) | ((uint64_t)data[4] << 32) |
                                   ((uint64_t)data[5] << 24) | ((uint64_t)data[6] << 16) |
                                   ((uint64_t)data[7] << 8) | data[8];
                hpplite_l1_mock_advance_time(svc->l1, seconds);
                response[0] = L1_STATUS_OK;
                respLen = 1;
            }
            break;
        }

        case L1_MSG_SET_CONFIG: {
            if (size < 21) {  /* 1 + 4 + 8 + 8 = 21 bytes */
                response[0] = L1_STATUS_INVALID;
                respLen = 1;
            } else {
                int offset = 1;
                int reqAtts = ((int)data[offset] << 24) | ((int)data[offset+1] << 16) |
                              ((int)data[offset+2] << 8) | data[offset+3];
                offset += 4;

                uint64_t cpInterval = ((uint64_t)data[offset] << 56) | ((uint64_t)data[offset+1] << 48) |
                                      ((uint64_t)data[offset+2] << 40) | ((uint64_t)data[offset+3] << 32) |
                                      ((uint64_t)data[offset+4] << 24) | ((uint64_t)data[offset+5] << 16) |
                                      ((uint64_t)data[offset+6] << 8) | data[offset+7];
                offset += 8;

                uint64_t seqTimeout = ((uint64_t)data[offset] << 56) | ((uint64_t)data[offset+1] << 48) |
                                      ((uint64_t)data[offset+2] << 40) | ((uint64_t)data[offset+3] << 32) |
                                      ((uint64_t)data[offset+4] << 24) | ((uint64_t)data[offset+5] << 16) |
                                      ((uint64_t)data[offset+6] << 8) | data[offset+7];

                hpplite_l1_mock_set_config(svc->l1, reqAtts, cpInterval, seqTimeout);
                response[0] = L1_STATUS_OK;
                respLen = 1;
            }
            break;
        }

        default:
            response[0] = L1_STATUS_INVALID;
            respLen = 1;
            break;
    }

    zmq_send(svc->zmqSocket, response, respLen, 0);
    return 0;
}

int hpplite_l1_service_process(HppliteL1Service *svc, int timeoutMs) {
    zmq_pollitem_t items[1];
    int processed = 0;
    int rc;

    if (!svc) return -1;

    items[0].socket = svc->zmqSocket;
    items[0].events = ZMQ_POLLIN;

    rc = zmq_poll(items, 1, timeoutMs);
    if (rc < 0) return -1;

    if (rc > 0 && (items[0].revents & ZMQ_POLLIN)) {
        zmq_msg_t msg;
        zmq_msg_init(&msg);

        if (zmq_msg_recv(&msg, svc->zmqSocket, 0) >= 0) {
            handle_request(svc, zmq_msg_data(&msg), (int)zmq_msg_size(&msg));
            processed++;
        }

        zmq_msg_close(&msg);
    }

    return processed;
}

void hpplite_l1_service_run(HppliteL1Service *svc) {
    if (!svc) return;

    svc->running = 1;
    while (svc->running) {
        hpplite_l1_service_process(svc, 100);  /* 100ms poll timeout */
    }
}

void hpplite_l1_service_stop(HppliteL1Service *svc) {
    if (svc) {
        svc->running = 0;
    }
}

HppliteL1 *hpplite_l1_service_get_l1(HppliteL1Service *svc) {
    return svc ? svc->l1 : NULL;
}

/*
** === Client Implementation ===
*/

HppliteL1Client *hpplite_l1_client_create(const char *endpoint) {
    HppliteL1Client *client;
    int rc;

    if (!endpoint) return NULL;

    client = sqlite3_malloc(sizeof(HppliteL1Client));
    if (!client) return NULL;
    memset(client, 0, sizeof(HppliteL1Client));

    client->zmqContext = zmq_ctx_new();
    if (!client->zmqContext) {
        sqlite3_free(client);
        return NULL;
    }

    client->zmqSocket = zmq_socket(client->zmqContext, ZMQ_REQ);
    if (!client->zmqSocket) {
        zmq_ctx_destroy(client->zmqContext);
        sqlite3_free(client);
        return NULL;
    }

    /* Set receive timeout */
    int timeout = 5000;  /* 5 second timeout */
    zmq_setsockopt(client->zmqSocket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));

    rc = zmq_connect(client->zmqSocket, endpoint);
    if (rc != 0) {
        fprintf(stderr, "L1 Client: Failed to connect to %s: %s\n",
            endpoint, zmq_strerror(zmq_errno()));
        zmq_close(client->zmqSocket);
        zmq_ctx_destroy(client->zmqContext);
        sqlite3_free(client);
        return NULL;
    }

    return client;
}

void hpplite_l1_client_destroy(HppliteL1Client *client) {
    if (!client) return;

    if (client->zmqSocket) {
        zmq_close(client->zmqSocket);
    }
    if (client->zmqContext) {
        zmq_ctx_destroy(client->zmqContext);
    }

    sqlite3_free(client);
}

/*
** Helper: send request and receive response
*/
static int client_request(HppliteL1Client *client, unsigned char *req, int reqLen,
                          unsigned char *resp, int maxRespLen) {
    int rc;

    rc = zmq_send(client->zmqSocket, req, reqLen, 0);
    if (rc < 0) return -1;

    rc = zmq_recv(client->zmqSocket, resp, maxRespLen, 0);
    return rc;
}

HppliteL1State *hpplite_l1_client_get_state(HppliteL1Client *client) {
    unsigned char req[1] = { L1_MSG_GET_STATE };
    unsigned char resp[4096];
    HppliteL1State *state;
    int rc, offset;

    if (!client) return NULL;

    rc = client_request(client, req, 1, resp, sizeof(resp));
    if (rc < 1 || resp[0] != L1_STATUS_OK) return NULL;

    state = sqlite3_malloc(sizeof(HppliteL1State));
    if (!state) return NULL;
    memset(state, 0, sizeof(HppliteL1State));

    offset = 1;

    /* nWitnesses */
    state->nWitnesses = ((int)resp[offset] << 24) | ((int)resp[offset+1] << 16) |
                        ((int)resp[offset+2] << 8) | resp[offset+3];
    offset += 4;

    /* requiredAttestations */
    state->requiredAttestations = ((int)resp[offset] << 24) | ((int)resp[offset+1] << 16) |
                                   ((int)resp[offset+2] << 8) | resp[offset+3];
    offset += 4;

    /* Sequencer pubkey */
    if (resp[offset++]) {
        memcpy(state->sequencerPubkey, resp + offset, HPPLITE_PUBKEY_SIZE);
        offset += HPPLITE_PUBKEY_SIZE;
    }

    /* Witness pubkeys */
    if (state->nWitnesses > 0) {
        state->witnessPubkeys = sqlite3_malloc(state->nWitnesses * sizeof(unsigned char[HPPLITE_PUBKEY_SIZE]));
        if (state->witnessPubkeys) {
            for (int i = 0; i < state->nWitnesses; i++) {
                memcpy(state->witnessPubkeys[i], resp + offset, HPPLITE_PUBKEY_SIZE);
                offset += HPPLITE_PUBKEY_SIZE;
            }
        }
    }

    return state;
}

int hpplite_l1_client_is_sequencer(HppliteL1Client *client, const unsigned char *pubkey) {
    unsigned char req[1 + HPPLITE_PUBKEY_SIZE];
    unsigned char resp[2];
    int rc;

    if (!client || !pubkey) return 0;

    req[0] = L1_MSG_IS_SEQUENCER;
    memcpy(req + 1, pubkey, HPPLITE_PUBKEY_SIZE);

    rc = client_request(client, req, sizeof(req), resp, sizeof(resp));
    if (rc < 2 || resp[0] != L1_STATUS_OK) return 0;

    return resp[1] ? 1 : 0;
}

int hpplite_l1_client_is_witness(HppliteL1Client *client, const unsigned char *pubkey) {
    unsigned char req[1 + HPPLITE_PUBKEY_SIZE];
    unsigned char resp[2];
    int rc;

    if (!client || !pubkey) return 0;

    req[0] = L1_MSG_IS_WITNESS;
    memcpy(req + 1, pubkey, HPPLITE_PUBKEY_SIZE);

    rc = client_request(client, req, sizeof(req), resp, sizeof(resp));
    if (rc < 2 || resp[0] != L1_STATUS_OK) return 0;

    return resp[1] ? 1 : 0;
}

unsigned char *hpplite_l1_client_get_sequencer(HppliteL1Client *client) {
    unsigned char req[1] = { L1_MSG_GET_SEQUENCER };
    unsigned char resp[1 + HPPLITE_PUBKEY_SIZE];
    unsigned char *result;
    int rc;

    if (!client) return NULL;

    rc = client_request(client, req, 1, resp, sizeof(resp));
    if (rc < 1 + HPPLITE_PUBKEY_SIZE || resp[0] != L1_STATUS_OK) return NULL;

    result = sqlite3_malloc(HPPLITE_PUBKEY_SIZE);
    if (result) {
        memcpy(result, resp + 1, HPPLITE_PUBKEY_SIZE);
    }
    return result;
}

int hpplite_l1_client_register_witness(HppliteL1Client *client, const unsigned char *pubkey) {
    unsigned char req[1 + HPPLITE_PUBKEY_SIZE];
    unsigned char resp[1];
    int rc;

    if (!client || !pubkey) return -1;

    req[0] = L1_MSG_REGISTER_WITNESS;
    memcpy(req + 1, pubkey, HPPLITE_PUBKEY_SIZE);

    rc = client_request(client, req, sizeof(req), resp, sizeof(resp));
    if (rc < 1 || resp[0] != L1_STATUS_OK) return -1;

    return 0;
}

int hpplite_l1_client_unregister_witness(HppliteL1Client *client, const unsigned char *pubkey) {
    unsigned char req[1 + HPPLITE_PUBKEY_SIZE];
    unsigned char resp[1];
    int rc;

    if (!client || !pubkey) return -1;

    req[0] = L1_MSG_UNREGISTER_WITNESS;
    memcpy(req + 1, pubkey, HPPLITE_PUBKEY_SIZE);

    rc = client_request(client, req, sizeof(req), resp, sizeof(resp));
    if (rc < 1 || resp[0] != L1_STATUS_OK) return -1;

    return 0;
}

int hpplite_l1_client_set_sequencer(HppliteL1Client *client, const unsigned char *pubkey) {
    unsigned char req[1 + HPPLITE_PUBKEY_SIZE];
    unsigned char resp[1];
    int rc;

    if (!client || !pubkey) return -1;

    req[0] = L1_MSG_SET_SEQUENCER;
    memcpy(req + 1, pubkey, HPPLITE_PUBKEY_SIZE);

    rc = client_request(client, req, sizeof(req), resp, sizeof(resp));
    if (rc < 1 || resp[0] != L1_STATUS_OK) return -1;

    return 0;
}

char *hpplite_l1_client_submit_checkpoint(
    HppliteL1Client *client,
    HppliteCheckpoint *cp,
    HppliteCheckpointAttestation *attestations,
    int nAttestations
) {
    unsigned char *req;
    unsigned char resp[256];
    char *json;
    int jsonLen, reqLen;
    int rc;

    if (!client || !cp) return NULL;

    json = hpplite_checkpoint_to_json(cp);
    if (!json) return NULL;
    jsonLen = (int)strlen(json);

    /* Build request: type(1) + jsonLen(4) + json + nAtts(4) + attestations */
    int attSize = nAttestations * (int)sizeof(HppliteCheckpointAttestation);
    reqLen = 1 + 4 + jsonLen + 4 + attSize;

    req = sqlite3_malloc(reqLen);
    if (!req) {
        sqlite3_free(json);
        return NULL;
    }

    int offset = 0;
    req[offset++] = L1_MSG_SUBMIT_CHECKPOINT;

    req[offset++] = (jsonLen >> 24) & 0xFF;
    req[offset++] = (jsonLen >> 16) & 0xFF;
    req[offset++] = (jsonLen >> 8) & 0xFF;
    req[offset++] = jsonLen & 0xFF;

    memcpy(req + offset, json, jsonLen);
    offset += jsonLen;
    sqlite3_free(json);

    req[offset++] = (nAttestations >> 24) & 0xFF;
    req[offset++] = (nAttestations >> 16) & 0xFF;
    req[offset++] = (nAttestations >> 8) & 0xFF;
    req[offset++] = nAttestations & 0xFF;

    if (attestations && nAttestations > 0) {
        memcpy(req + offset, attestations, attSize);
    }

    rc = client_request(client, req, reqLen, resp, sizeof(resp));
    sqlite3_free(req);

    if (rc < 1 || resp[0] != L1_STATUS_OK) return NULL;

    /* Parse tx hash */
    int hashLen = ((int)resp[1] << 8) | resp[2];
    char *txHash = sqlite3_malloc(hashLen + 1);
    if (txHash) {
        memcpy(txHash, resp + 3, hashLen);
        txHash[hashLen] = '\0';
    }
    return txHash;
}

HppliteCheckpoint *hpplite_l1_client_get_checkpoint(HppliteL1Client *client, int index) {
    unsigned char req[5];
    unsigned char resp[4096];
    int rc;

    if (!client) return NULL;

    req[0] = L1_MSG_GET_CHECKPOINT;
    req[1] = (index >> 24) & 0xFF;
    req[2] = (index >> 16) & 0xFF;
    req[3] = (index >> 8) & 0xFF;
    req[4] = index & 0xFF;

    rc = client_request(client, req, sizeof(req), resp, sizeof(resp));
    if (rc < 1 || resp[0] != L1_STATUS_OK) return NULL;

    int jsonLen = ((int)resp[1] << 24) | ((int)resp[2] << 16) |
                  ((int)resp[3] << 8) | resp[4];

    char *jsonStr = sqlite3_malloc(jsonLen + 1);
    if (!jsonStr) return NULL;
    memcpy(jsonStr, resp + 5, jsonLen);
    jsonStr[jsonLen] = '\0';

    HppliteCheckpoint *cpResult = hpplite_checkpoint_from_json(jsonStr);
    sqlite3_free(jsonStr);

    return cpResult;
}

int hpplite_l1_client_check_timeout(HppliteL1Client *client) {
    unsigned char req[1];
    unsigned char resp[2];
    int rc;

    if (!client) return 0;

    req[0] = L1_MSG_CHECK_TIMEOUT;
    rc = client_request(client, req, sizeof(req), resp, sizeof(resp));
    if (rc < 2 || resp[0] != L1_STATUS_OK) return 0;

    return resp[1] ? 1 : 0;
}

int hpplite_l1_client_claim_sequencer(HppliteL1Client *client, const unsigned char *pubkey) {
    unsigned char req[1 + HPPLITE_PUBKEY_SIZE];
    unsigned char resp[1];
    int rc;

    if (!client || !pubkey) return -1;

    req[0] = L1_MSG_CLAIM_SEQUENCER;
    memcpy(req + 1, pubkey, HPPLITE_PUBKEY_SIZE);

    rc = client_request(client, req, sizeof(req), resp, sizeof(resp));
    if (rc < 1 || resp[0] != L1_STATUS_OK) return -1;

    return 0;
}

int hpplite_l1_client_advance_time(HppliteL1Client *client, uint64_t seconds) {
    unsigned char req[9];
    unsigned char resp[1];
    int rc;

    if (!client) return -1;

    req[0] = L1_MSG_ADVANCE_TIME;
    req[1] = (seconds >> 56) & 0xFF;
    req[2] = (seconds >> 48) & 0xFF;
    req[3] = (seconds >> 40) & 0xFF;
    req[4] = (seconds >> 32) & 0xFF;
    req[5] = (seconds >> 24) & 0xFF;
    req[6] = (seconds >> 16) & 0xFF;
    req[7] = (seconds >> 8) & 0xFF;
    req[8] = seconds & 0xFF;

    rc = client_request(client, req, sizeof(req), resp, sizeof(resp));
    if (rc < 1 || resp[0] != L1_STATUS_OK) return -1;

    return 0;
}

int hpplite_l1_client_set_config(
    HppliteL1Client *client,
    int requiredAttestations,
    uint64_t checkpointInterval,
    uint64_t sequencerTimeout
) {
    unsigned char req[21];  /* 1 + 4 + 8 + 8 */
    unsigned char resp[1];
    int rc;
    int offset = 0;

    if (!client) return -1;

    req[offset++] = L1_MSG_SET_CONFIG;

    req[offset++] = (requiredAttestations >> 24) & 0xFF;
    req[offset++] = (requiredAttestations >> 16) & 0xFF;
    req[offset++] = (requiredAttestations >> 8) & 0xFF;
    req[offset++] = requiredAttestations & 0xFF;

    req[offset++] = (checkpointInterval >> 56) & 0xFF;
    req[offset++] = (checkpointInterval >> 48) & 0xFF;
    req[offset++] = (checkpointInterval >> 40) & 0xFF;
    req[offset++] = (checkpointInterval >> 32) & 0xFF;
    req[offset++] = (checkpointInterval >> 24) & 0xFF;
    req[offset++] = (checkpointInterval >> 16) & 0xFF;
    req[offset++] = (checkpointInterval >> 8) & 0xFF;
    req[offset++] = checkpointInterval & 0xFF;

    req[offset++] = (sequencerTimeout >> 56) & 0xFF;
    req[offset++] = (sequencerTimeout >> 48) & 0xFF;
    req[offset++] = (sequencerTimeout >> 40) & 0xFF;
    req[offset++] = (sequencerTimeout >> 32) & 0xFF;
    req[offset++] = (sequencerTimeout >> 24) & 0xFF;
    req[offset++] = (sequencerTimeout >> 16) & 0xFF;
    req[offset++] = (sequencerTimeout >> 8) & 0xFF;
    req[offset++] = sequencerTimeout & 0xFF;

    rc = client_request(client, req, sizeof(req), resp, sizeof(resp));
    if (rc < 1 || resp[0] != L1_STATUS_OK) return -1;

    return 0;
}

/*
** === Remote L1 Implementation ===
** Creates HppliteL1 that wraps ZMQ client for use with existing node code
*/

/* Forward declare wrapper functions */
static HppliteL1State *wrapper_get_state(HppliteL1 *l1);
static int wrapper_is_sequencer(HppliteL1 *l1, const unsigned char *pubkey);
static int wrapper_is_witness(HppliteL1 *l1, const unsigned char *pubkey);
static char *wrapper_submit_checkpoint(HppliteL1 *l1, const HppliteCheckpoint *cp,
    const HppliteCheckpointAttestation *att, int nAtt);
static void wrapper_disconnect(HppliteL1 *l1);

/* L1 vtable for remote client */
typedef struct {
    HppliteL1Client *client;
} RemoteL1Data;

static HppliteL1State *wrapper_get_state(HppliteL1 *l1) {
    RemoteL1Data *data = (RemoteL1Data*)l1;
    /* Offset by size of function pointers to get to user data */
    data = (RemoteL1Data*)((char*)l1 + sizeof(void*) * 10);  /* Approximate */
    return hpplite_l1_client_get_state(data->client);
}

HppliteL1 *hpplite_l1_create_remote(const char *endpoint) {
    HppliteL1Client *client = hpplite_l1_client_create(endpoint);
    if (!client) return NULL;

    /* For now, return NULL - full implementation would create
       an HppliteL1 structure with vtable pointing to wrapper functions.
       This requires modifying the L1 interface to use function pointers. */

    /* Clean up for now since we can't properly implement this without
       changes to l1_interface.h */
    hpplite_l1_client_destroy(client);
    return NULL;
}

#endif /* HPPLITE_ENABLE_ZMQ */
