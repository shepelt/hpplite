/*
** Test L1 Mock Implementation - Lease-Based Sequencer Model
*/

#include "l1_interface.h"
#include "crypto.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Test counters */
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    printf("  Testing: %s... ", name); \
    tests_run++; \
} while(0)

#define PASS() do { \
    printf("PASS\n"); \
    tests_passed++; \
} while(0)

#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
} while(0)

/* Test private keys */
static const unsigned char SEQ_PRIVKEY[32] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
};

static const unsigned char OTHER_PRIVKEY[32] = {
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40
};

/* Test instance IDs */
static const unsigned char INSTANCE1[32] = {
    0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
    0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
    0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
    0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11
};

static const unsigned char INSTANCE2[32] = {
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22
};

/*
** Test 1: Basic L1 mock creation
*/
static void test_create_mock(void) {
    HppliteL1 *l1;

    TEST("create_mock");

    l1 = hpplite_l1_create_mock();
    if (!l1) {
        FAIL("Failed to create mock L1");
        return;
    }

    /* Check initial state */
    HppliteL1State *state = hpplite_l1_get_state(l1);
    if (!state) {
        FAIL("Failed to get state");
        hpplite_l1_disconnect(l1);
        return;
    }

    /* No lease should be active initially */
    if (hpplite_l1_is_lease_active(l1)) {
        FAIL("Lease should not be active initially");
        hpplite_l1_state_free(state);
        hpplite_l1_disconnect(l1);
        return;
    }

    hpplite_l1_state_free(state);
    hpplite_l1_disconnect(l1);

    PASS();
}

/*
** Test 2: Claim sequencer lease
*/
static void test_claim_lease(void) {
    HppliteL1 *l1;
    char *txHash;

    TEST("claim_lease");

    l1 = hpplite_l1_create_mock();
    if (!l1) {
        FAIL("Failed to create mock L1");
        return;
    }

    /* Set private key first */
    hpplite_l1_set_privkey(l1, SEQ_PRIVKEY);

    /* Claim sequencer lease */
    txHash = hpplite_l1_claim_sequencer(l1, INSTANCE1);
    if (!txHash) {
        FAIL("Failed to claim sequencer lease");
        hpplite_l1_disconnect(l1);
        return;
    }
    sqlite3_free(txHash);

    /* Verify lease is active */
    if (!hpplite_l1_is_lease_active(l1)) {
        FAIL("Lease should be active after claim");
        hpplite_l1_disconnect(l1);
        return;
    }

    /* Check remaining time */
    uint64_t remaining = hpplite_l1_get_lease_time_remaining(l1);
    if (remaining == 0) {
        FAIL("Lease time remaining should be > 0");
        hpplite_l1_disconnect(l1);
        return;
    }

    hpplite_l1_disconnect(l1);
    PASS();
}

/*
** Test 3: Cannot claim while lease is active
*/
static void test_lease_conflict(void) {
    HppliteL1 *l1;
    char *txHash;

    TEST("lease_conflict");

    l1 = hpplite_l1_create_mock();
    if (!l1) {
        FAIL("Failed to create mock L1");
        return;
    }

    /* First wallet claims */
    hpplite_l1_set_privkey(l1, SEQ_PRIVKEY);
    txHash = hpplite_l1_claim_sequencer(l1, INSTANCE1);
    if (!txHash) {
        FAIL("First claim should succeed");
        hpplite_l1_disconnect(l1);
        return;
    }
    sqlite3_free(txHash);

    /* Second wallet tries to claim (should fail) */
    hpplite_l1_set_privkey(l1, OTHER_PRIVKEY);
    txHash = hpplite_l1_claim_sequencer(l1, INSTANCE2);
    if (txHash != NULL) {
        FAIL("Second claim should fail while lease active");
        sqlite3_free(txHash);
        hpplite_l1_disconnect(l1);
        return;
    }

    hpplite_l1_disconnect(l1);
    PASS();
}

/*
** Test 4: Lease renewal
*/
static void test_lease_renewal(void) {
    HppliteL1 *l1;
    char *txHash;

    TEST("lease_renewal");

    l1 = hpplite_l1_create_mock();
    if (!l1) {
        FAIL("Failed to create mock L1");
        return;
    }

    hpplite_l1_set_privkey(l1, SEQ_PRIVKEY);

    /* Claim */
    txHash = hpplite_l1_claim_sequencer(l1, INSTANCE1);
    if (!txHash) {
        FAIL("Claim should succeed");
        hpplite_l1_disconnect(l1);
        return;
    }
    sqlite3_free(txHash);

    /* Advance time */
    hpplite_l1_mock_advance_time(l1, 100);

    /* Renew */
    txHash = hpplite_l1_renew_lease(l1, INSTANCE1);
    if (!txHash) {
        FAIL("Renewal should succeed");
        hpplite_l1_disconnect(l1);
        return;
    }
    sqlite3_free(txHash);

    /* Should still be active */
    if (!hpplite_l1_is_lease_active(l1)) {
        FAIL("Lease should still be active after renewal");
        hpplite_l1_disconnect(l1);
        return;
    }

    hpplite_l1_disconnect(l1);
    PASS();
}

/*
** Test 5: Lease expiry
*/
static void test_lease_expiry(void) {
    HppliteL1 *l1;
    char *txHash;

    TEST("lease_expiry");

    l1 = hpplite_l1_create_mock();
    if (!l1) {
        FAIL("Failed to create mock L1");
        return;
    }

    hpplite_l1_set_privkey(l1, SEQ_PRIVKEY);

    /* Claim with short lease duration */
    hpplite_l1_mock_set_config(l1, 100, 60);  /* 60 second lease */

    txHash = hpplite_l1_claim_sequencer(l1, INSTANCE1);
    if (!txHash) {
        FAIL("Claim should succeed");
        hpplite_l1_disconnect(l1);
        return;
    }
    sqlite3_free(txHash);

    /* Should be active */
    if (!hpplite_l1_is_lease_active(l1)) {
        FAIL("Lease should be active");
        hpplite_l1_disconnect(l1);
        return;
    }

    /* Advance time past expiry */
    hpplite_l1_mock_advance_time(l1, 120);

    /* Should be expired */
    if (hpplite_l1_is_lease_active(l1)) {
        FAIL("Lease should be expired");
        hpplite_l1_disconnect(l1);
        return;
    }

    /* Another wallet should now be able to claim */
    hpplite_l1_set_privkey(l1, OTHER_PRIVKEY);
    txHash = hpplite_l1_claim_sequencer(l1, INSTANCE2);
    if (!txHash) {
        FAIL("Claim should succeed after expiry");
        hpplite_l1_disconnect(l1);
        return;
    }
    sqlite3_free(txHash);

    hpplite_l1_disconnect(l1);
    PASS();
}

/*
** Test 6: Batch DA operations
*/
static void test_batch_da(void) {
    HppliteL1 *l1;
    char *txHash;

    TEST("batch_da");

    l1 = hpplite_l1_create_mock();
    if (!l1) {
        FAIL("Failed to create mock L1");
        return;
    }

    /* Need to claim lease before batch submission */
    hpplite_l1_set_privkey(l1, SEQ_PRIVKEY);
    unsigned char instanceId[32];
    memset(instanceId, 0x42, 32);
    txHash = hpplite_l1_claim_sequencer(l1, instanceId);
    if (!txHash) {
        FAIL("Failed to claim sequencer for batch test");
        hpplite_l1_disconnect(l1);
        return;
    }
    sqlite3_free(txHash);

    /* Submit batch */
    const char *testData = "{\"ops\":[{\"sql\":\"INSERT INTO t VALUES (1)\"}]}";
    txHash = hpplite_l1_submit_batch(l1, instanceId, 1, (const uint8_t *)testData, strlen(testData));
    if (!txHash) {
        FAIL("Failed to submit batch");
        hpplite_l1_disconnect(l1);
        return;
    }
    sqlite3_free(txHash);

    /* Get batch back */
    size_t dataLen;
    uint8_t *data = hpplite_l1_get_batch(l1, 1, &dataLen);
    if (!data) {
        FAIL("Failed to get batch");
        hpplite_l1_disconnect(l1);
        return;
    }

    if (dataLen != strlen(testData) || memcmp(data, testData, dataLen) != 0) {
        FAIL("Batch data mismatch");
        free(data);
        hpplite_l1_disconnect(l1);
        return;
    }
    free(data);

    /* Check DA state */
    uint64_t lastHeight, total;
    uint8_t hash[32];
    if (hpplite_l1_get_da_state(l1, &lastHeight, &total, hash) != 0) {
        FAIL("Failed to get DA state");
        hpplite_l1_disconnect(l1);
        return;
    }

    if (lastHeight != 1 || total != 1) {
        FAIL("DA state incorrect");
        hpplite_l1_disconnect(l1);
        return;
    }

    hpplite_l1_disconnect(l1);
    PASS();
}

/*
** Test 7: Checkpoint submission
*/
static void test_checkpoint_submit(void) {
    HppliteL1 *l1;
    char *txHash;

    TEST("checkpoint_submit");

    l1 = hpplite_l1_create_mock();
    if (!l1) {
        FAIL("Failed to create mock L1");
        return;
    }

    /* Set private key first */
    hpplite_l1_set_privkey(l1, SEQ_PRIVKEY);

    /* Claim sequencer lease */
    unsigned char instanceId[32];
    memset(instanceId, 0x42, 32);
    txHash = hpplite_l1_claim_sequencer(l1, instanceId);
    if (!txHash) {
        FAIL("Failed to claim sequencer");
        hpplite_l1_disconnect(l1);
        return;
    }
    sqlite3_free(txHash);

    /* Create and submit checkpoint */
    HppliteCheckpoint *cp = hpplite_checkpoint_new(1, 10);
    if (!cp) {
        FAIL("Failed to create checkpoint");
        hpplite_l1_disconnect(l1);
        return;
    }

    memset(cp->preStateRoot, 0xAA, 32);
    memset(cp->postStateRoot, 0xBB, 32);
    cp->timestamp = 12345;

    /* Submit with instance ID */
    txHash = hpplite_l1_submit_checkpoint(l1, cp, instanceId);
    if (!txHash) {
        FAIL("Failed to submit checkpoint");
        hpplite_checkpoint_free(cp);
        hpplite_l1_disconnect(l1);
        return;
    }
    sqlite3_free(txHash);

    /* Verify checkpoint was stored */
    if (hpplite_l1_mock_get_checkpoint_count(l1) != 1) {
        FAIL("Checkpoint not stored");
        hpplite_checkpoint_free(cp);
        hpplite_l1_disconnect(l1);
        return;
    }

    const HppliteCheckpoint *stored = hpplite_l1_mock_get_last_checkpoint(l1);
    if (!stored || stored->toHeight != 10) {
        FAIL("Stored checkpoint mismatch");
        hpplite_checkpoint_free(cp);
        hpplite_l1_disconnect(l1);
        return;
    }

    hpplite_checkpoint_free(cp);
    hpplite_l1_disconnect(l1);
    PASS();
}

int main(void) {
    printf("\n=== L1 Mock Tests (Lease-Based Model) ===\n\n");

    test_create_mock();
    test_claim_lease();
    test_lease_conflict();
    test_lease_renewal();
    test_lease_expiry();
    test_batch_da();
    test_checkpoint_submit();

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
