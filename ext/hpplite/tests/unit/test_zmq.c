/*
** HPPLite M2 Multi-Process Test
**
** Tests ZeroMQ-based communication between nodes and L1 service.
** Uses threads to simulate separate processes.
*/

#include "node.h"
#include "l1_service.h"
#include "crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#ifdef HPPLITE_ENABLE_ZMQ

#define TEST_L1_ENDPOINT "tcp://127.0.0.1:15560"
#define TEST_SEQ_PUB     "tcp://127.0.0.1:15561"
#define TEST_SEQ_ROUTER  "tcp://127.0.0.1:15562"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s at %s:%d\n", msg, __FILE__, __LINE__); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_PASS(name) do { \
    printf("PASS: %s\n", name); \
    tests_passed++; \
} while(0)

/* L1 service thread data */
typedef struct {
    HppliteL1Service *svc;
    int running;
} L1ThreadData;

/* L1 service thread function */
static void *l1_service_thread(void *arg) {
    L1ThreadData *data = (L1ThreadData*)arg;

    while (data->running) {
        hpplite_l1_service_process(data->svc, 50);
    }

    return NULL;
}

/*
** Test: L1 service start/stop
*/
static void test_l1_service_lifecycle(void) {
    HppliteL1Service *svc = hpplite_l1_service_create(TEST_L1_ENDPOINT);
    TEST_ASSERT(svc != NULL, "L1 service creation failed");

    HppliteL1 *l1 = hpplite_l1_service_get_l1(svc);
    TEST_ASSERT(l1 != NULL, "L1 mock access failed");

    hpplite_l1_service_destroy(svc);

    TEST_PASS("L1 service lifecycle");
}

/*
** Test: L1 client connect/disconnect
*/
static void test_l1_client_lifecycle(void) {
    /* Start L1 service */
    HppliteL1Service *svc = hpplite_l1_service_create(TEST_L1_ENDPOINT);
    TEST_ASSERT(svc != NULL, "L1 service creation failed");

    L1ThreadData threadData = { svc, 1 };
    pthread_t svcThread;
    pthread_create(&svcThread, NULL, l1_service_thread, &threadData);

    /* Give service time to start */
    usleep(100000);  /* 100ms */

    /* Create client */
    HppliteL1Client *client = hpplite_l1_client_create(TEST_L1_ENDPOINT);
    TEST_ASSERT(client != NULL, "L1 client creation failed");

    /* Get state */
    HppliteL1State *state = hpplite_l1_client_get_state(client);
    TEST_ASSERT(state != NULL, "Failed to get L1 state");
    TEST_ASSERT(state->nWitnesses == 0, "Initial witnesses should be 0");
    hpplite_l1_state_free(state);

    /* Clean up */
    hpplite_l1_client_destroy(client);

    threadData.running = 0;
    pthread_join(svcThread, NULL);
    hpplite_l1_service_destroy(svc);

    TEST_PASS("L1 client lifecycle");
}

/*
** Test: Set sequencer via L1 service
*/
static void test_l1_set_sequencer(void) {
    /* Start L1 service */
    HppliteL1Service *svc = hpplite_l1_service_create(TEST_L1_ENDPOINT);
    TEST_ASSERT(svc != NULL, "L1 service creation failed");

    L1ThreadData threadData = { svc, 1 };
    pthread_t svcThread;
    pthread_create(&svcThread, NULL, l1_service_thread, &threadData);
    usleep(100000);

    /* Create client */
    HppliteL1Client *client = hpplite_l1_client_create(TEST_L1_ENDPOINT);
    TEST_ASSERT(client != NULL, "L1 client creation failed");

    /* Generate test pubkey */
    unsigned char testPubkey[HPPLITE_PUBKEY_SIZE];
    memset(testPubkey, 0xAA, HPPLITE_PUBKEY_SIZE);

    /* Set sequencer */
    int rc = hpplite_l1_client_set_sequencer(client, testPubkey);
    TEST_ASSERT(rc == 0, "Failed to set sequencer");

    /* Verify sequencer is set */
    int isSeq = hpplite_l1_client_is_sequencer(client, testPubkey);
    TEST_ASSERT(isSeq == 1, "Sequencer check failed");

    /* Verify different pubkey is not sequencer */
    unsigned char otherPubkey[HPPLITE_PUBKEY_SIZE];
    memset(otherPubkey, 0xBB, HPPLITE_PUBKEY_SIZE);
    isSeq = hpplite_l1_client_is_sequencer(client, otherPubkey);
    TEST_ASSERT(isSeq == 0, "Non-sequencer should return 0");

    /* Clean up */
    hpplite_l1_client_destroy(client);
    threadData.running = 0;
    pthread_join(svcThread, NULL);
    hpplite_l1_service_destroy(svc);

    TEST_PASS("L1 set sequencer");
}

/*
** Test: Register witness via L1 service
*/
static void test_l1_register_witness(void) {
    /* Start L1 service */
    HppliteL1Service *svc = hpplite_l1_service_create(TEST_L1_ENDPOINT);
    TEST_ASSERT(svc != NULL, "L1 service creation failed");

    L1ThreadData threadData = { svc, 1 };
    pthread_t svcThread;
    pthread_create(&svcThread, NULL, l1_service_thread, &threadData);
    usleep(100000);

    /* Create client */
    HppliteL1Client *client = hpplite_l1_client_create(TEST_L1_ENDPOINT);
    TEST_ASSERT(client != NULL, "L1 client creation failed");

    /* Generate test pubkeys */
    unsigned char wit1Pubkey[HPPLITE_PUBKEY_SIZE];
    unsigned char wit2Pubkey[HPPLITE_PUBKEY_SIZE];
    memset(wit1Pubkey, 0xCC, HPPLITE_PUBKEY_SIZE);
    memset(wit2Pubkey, 0xDD, HPPLITE_PUBKEY_SIZE);

    /* Register witnesses */
    int rc = hpplite_l1_client_register_witness(client, wit1Pubkey);
    TEST_ASSERT(rc == 0, "Failed to register witness 1");

    rc = hpplite_l1_client_register_witness(client, wit2Pubkey);
    TEST_ASSERT(rc == 0, "Failed to register witness 2");

    /* Verify witnesses */
    int isWit = hpplite_l1_client_is_witness(client, wit1Pubkey);
    TEST_ASSERT(isWit == 1, "Witness 1 check failed");

    isWit = hpplite_l1_client_is_witness(client, wit2Pubkey);
    TEST_ASSERT(isWit == 1, "Witness 2 check failed");

    /* Verify non-witness */
    unsigned char otherPubkey[HPPLITE_PUBKEY_SIZE];
    memset(otherPubkey, 0xEE, HPPLITE_PUBKEY_SIZE);
    isWit = hpplite_l1_client_is_witness(client, otherPubkey);
    TEST_ASSERT(isWit == 0, "Non-witness should return 0");

    /* Check state */
    HppliteL1State *state = hpplite_l1_client_get_state(client);
    TEST_ASSERT(state != NULL, "Failed to get state");
    TEST_ASSERT(state->nWitnesses == 2, "Should have 2 witnesses");
    hpplite_l1_state_free(state);

    /* Clean up */
    hpplite_l1_client_destroy(client);
    threadData.running = 0;
    pthread_join(svcThread, NULL);
    hpplite_l1_service_destroy(svc);

    TEST_PASS("L1 register witness");
}

/*
** Test: Multiple clients can connect to same L1 service
*/
static void test_l1_multiple_clients(void) {
    /* Start L1 service */
    HppliteL1Service *svc = hpplite_l1_service_create(TEST_L1_ENDPOINT);
    TEST_ASSERT(svc != NULL, "L1 service creation failed");

    L1ThreadData threadData = { svc, 1 };
    pthread_t svcThread;
    pthread_create(&svcThread, NULL, l1_service_thread, &threadData);
    usleep(100000);

    /* Create multiple clients */
    HppliteL1Client *client1 = hpplite_l1_client_create(TEST_L1_ENDPOINT);
    HppliteL1Client *client2 = hpplite_l1_client_create(TEST_L1_ENDPOINT);
    HppliteL1Client *client3 = hpplite_l1_client_create(TEST_L1_ENDPOINT);

    TEST_ASSERT(client1 != NULL, "Client 1 creation failed");
    TEST_ASSERT(client2 != NULL, "Client 2 creation failed");
    TEST_ASSERT(client3 != NULL, "Client 3 creation failed");

    /* Client 1 sets sequencer */
    unsigned char seqPubkey[HPPLITE_PUBKEY_SIZE];
    memset(seqPubkey, 0x11, HPPLITE_PUBKEY_SIZE);
    int rc = hpplite_l1_client_set_sequencer(client1, seqPubkey);
    TEST_ASSERT(rc == 0, "Failed to set sequencer via client 1");

    /* Client 2 registers witness */
    unsigned char witPubkey[HPPLITE_PUBKEY_SIZE];
    memset(witPubkey, 0x22, HPPLITE_PUBKEY_SIZE);
    rc = hpplite_l1_client_register_witness(client2, witPubkey);
    TEST_ASSERT(rc == 0, "Failed to register witness via client 2");

    /* Client 3 verifies state */
    int isSeq = hpplite_l1_client_is_sequencer(client3, seqPubkey);
    TEST_ASSERT(isSeq == 1, "Sequencer check via client 3 failed");

    int isWit = hpplite_l1_client_is_witness(client3, witPubkey);
    TEST_ASSERT(isWit == 1, "Witness check via client 3 failed");

    /* Clean up */
    hpplite_l1_client_destroy(client1);
    hpplite_l1_client_destroy(client2);
    hpplite_l1_client_destroy(client3);
    threadData.running = 0;
    pthread_join(svcThread, NULL);
    hpplite_l1_service_destroy(svc);

    TEST_PASS("L1 multiple clients");
}

/*
** Test: Sequencer timeout and witness promotion
*/
static void test_sequencer_failover(void) {
    /* Start L1 service */
    HppliteL1Service *svc = hpplite_l1_service_create(TEST_L1_ENDPOINT);
    TEST_ASSERT(svc != NULL, "L1 service creation failed");

    L1ThreadData threadData = { svc, 1 };
    pthread_t svcThread;
    pthread_create(&svcThread, NULL, l1_service_thread, &threadData);
    usleep(100000);

    /* Create clients for sequencer and witness */
    HppliteL1Client *seqClient = hpplite_l1_client_create(TEST_L1_ENDPOINT);
    HppliteL1Client *witClient = hpplite_l1_client_create(TEST_L1_ENDPOINT);
    TEST_ASSERT(seqClient != NULL, "Sequencer client creation failed");
    TEST_ASSERT(witClient != NULL, "Witness client creation failed");

    /* Configure short timeout (10 seconds) */
    int rc = hpplite_l1_client_set_config(seqClient, 1, 10, 10);
    TEST_ASSERT(rc == 0, "Failed to set config");

    /* Generate pubkeys */
    unsigned char seqPubkey[HPPLITE_PUBKEY_SIZE];
    unsigned char witPubkey[HPPLITE_PUBKEY_SIZE];
    memset(seqPubkey, 0x11, HPPLITE_PUBKEY_SIZE);
    memset(witPubkey, 0x22, HPPLITE_PUBKEY_SIZE);

    /* Set initial sequencer */
    rc = hpplite_l1_client_set_sequencer(seqClient, seqPubkey);
    TEST_ASSERT(rc == 0, "Failed to set sequencer");

    /* Register witness */
    rc = hpplite_l1_client_register_witness(witClient, witPubkey);
    TEST_ASSERT(rc == 0, "Failed to register witness");

    /* Verify sequencer is set */
    int isSeq = hpplite_l1_client_is_sequencer(witClient, seqPubkey);
    TEST_ASSERT(isSeq == 1, "Sequencer should be set");

    /* Timeout should not be expired yet */
    int expired = hpplite_l1_client_check_timeout(witClient);
    TEST_ASSERT(expired == 0, "Timeout should not be expired initially");

    /* Witness should not be able to claim yet */
    rc = hpplite_l1_client_claim_sequencer(witClient, witPubkey);
    TEST_ASSERT(rc == -1, "Witness should not claim before timeout");

    /* Advance time past timeout */
    rc = hpplite_l1_client_advance_time(witClient, 15);
    TEST_ASSERT(rc == 0, "Failed to advance time");

    /* Timeout should now be expired */
    expired = hpplite_l1_client_check_timeout(witClient);
    TEST_ASSERT(expired == 1, "Timeout should be expired after advancing time");

    /* Witness can now claim sequencer role */
    rc = hpplite_l1_client_claim_sequencer(witClient, witPubkey);
    TEST_ASSERT(rc == 0, "Witness should be able to claim after timeout");

    /* Verify witness is now sequencer */
    isSeq = hpplite_l1_client_is_sequencer(seqClient, witPubkey);
    TEST_ASSERT(isSeq == 1, "Witness should now be sequencer");

    /* Old sequencer should no longer be sequencer */
    isSeq = hpplite_l1_client_is_sequencer(seqClient, seqPubkey);
    TEST_ASSERT(isSeq == 0, "Old sequencer should no longer be sequencer");

    /* Clean up */
    hpplite_l1_client_destroy(seqClient);
    hpplite_l1_client_destroy(witClient);
    threadData.running = 0;
    pthread_join(svcThread, NULL);
    hpplite_l1_service_destroy(svc);

    TEST_PASS("Sequencer failover");
}

#endif /* HPPLITE_ENABLE_ZMQ */

int main(void) {
    printf("=== HPPLite M2 Multi-Process Tests ===\n\n");

#ifdef HPPLITE_ENABLE_ZMQ
    test_l1_service_lifecycle();
    test_l1_client_lifecycle();
    test_l1_set_sequencer();
    test_l1_register_witness();
    test_l1_multiple_clients();
    test_sequencer_failover();

    printf("\n=== Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);

    return tests_failed > 0 ? 1 : 0;
#else
    printf("ZeroMQ not enabled, skipping M2 tests\n");
    return 0;
#endif
}
