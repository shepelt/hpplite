/*
** Test L1 Mock Implementation
*/

#include "l1_interface.h"
#include "crypto.h"
#include "sqlite3.h"
#include <stdio.h>
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

static const unsigned char WIT1_PRIVKEY[32] = {
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40
};

static const unsigned char WIT2_PRIVKEY[32] = {
    0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50,
    0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60
};

/*
** Test 1: Create and destroy L1 mock
*/
static void test_l1_create(void) {
    HppliteL1 *l1;

    TEST("l1_create");

    l1 = hpplite_l1_create_mock();
    if (!l1) {
        FAIL("Failed to create L1 mock");
        return;
    }

    /* Check initial state */
    if (hpplite_l1_mock_get_block(l1) != 1) {
        FAIL("Initial block should be 1");
        hpplite_l1_disconnect(l1);
        return;
    }

    hpplite_l1_disconnect(l1);
    PASS();
}

/*
** Test 2: Witness registration
*/
static void test_witness_registration(void) {
    HppliteL1 *l1;
    HppliteCrypto *crypto;
    HppliteKeypair kp1, kp2;
    int rc;

    TEST("witness_registration");

    l1 = hpplite_l1_create_mock();
    crypto = hpplite_crypto_init();

    /* Get pubkeys */
    hpplite_crypto_keypair_from_privkey(crypto, &kp1, WIT1_PRIVKEY);
    hpplite_crypto_keypair_from_privkey(crypto, &kp2, WIT2_PRIVKEY);

    /* Initially no witnesses */
    if (hpplite_l1_is_witness(l1, kp1.pubkey)) {
        FAIL("Should not be witness before registration");
        goto cleanup;
    }

    /* Register witnesses */
    rc = hpplite_l1_register_witness(l1, WIT1_PRIVKEY);
    if (rc != 0) {
        FAIL("Failed to register witness 1");
        goto cleanup;
    }

    rc = hpplite_l1_register_witness(l1, WIT2_PRIVKEY);
    if (rc != 0) {
        FAIL("Failed to register witness 2");
        goto cleanup;
    }

    /* Check witnesses are registered */
    if (!hpplite_l1_is_witness(l1, kp1.pubkey)) {
        FAIL("Witness 1 should be registered");
        goto cleanup;
    }

    if (!hpplite_l1_is_witness(l1, kp2.pubkey)) {
        FAIL("Witness 2 should be registered");
        goto cleanup;
    }

    /* Check state */
    HppliteL1State *state = hpplite_l1_get_state(l1);
    if (!state || state->nWitnesses != 2) {
        FAIL("State should show 2 witnesses");
        hpplite_l1_state_free(state);
        goto cleanup;
    }
    hpplite_l1_state_free(state);

    PASS();

cleanup:
    hpplite_crypto_free(crypto);
    hpplite_l1_disconnect(l1);
}

/*
** Test 3: Sequencer claim
*/
static void test_sequencer_claim(void) {
    HppliteL1 *l1;
    HppliteCrypto *crypto;
    HppliteKeypair seqKp, witKp;
    int rc;

    TEST("sequencer_claim");

    l1 = hpplite_l1_create_mock();
    crypto = hpplite_crypto_init();

    hpplite_crypto_keypair_from_privkey(crypto, &seqKp, SEQ_PRIVKEY);
    hpplite_crypto_keypair_from_privkey(crypto, &witKp, WIT1_PRIVKEY);

    /* Can't claim sequencer if not a witness */
    rc = hpplite_l1_claim_sequencer(l1, SEQ_PRIVKEY);
    if (rc == 0) {
        FAIL("Should not be able to claim if not a witness");
        goto cleanup;
    }

    /* Register as witness first */
    hpplite_l1_register_witness(l1, SEQ_PRIVKEY);

    /* Now claim should work (no current sequencer) */
    rc = hpplite_l1_claim_sequencer(l1, SEQ_PRIVKEY);
    if (rc != 0) {
        FAIL("Should be able to claim with no sequencer");
        goto cleanup;
    }

    /* Check we are sequencer */
    if (!hpplite_l1_is_sequencer(l1, seqKp.pubkey)) {
        FAIL("Should be sequencer after claim");
        goto cleanup;
    }

    /* Another witness shouldn't be able to claim (no timeout) */
    hpplite_l1_register_witness(l1, WIT1_PRIVKEY);
    rc = hpplite_l1_claim_sequencer(l1, WIT1_PRIVKEY);
    if (rc == 0) {
        FAIL("Should not be able to claim while sequencer active");
        goto cleanup;
    }

    PASS();

cleanup:
    hpplite_crypto_free(crypto);
    hpplite_l1_disconnect(l1);
}

/*
** Test 4: Sequencer timeout and promotion
*/
static void test_sequencer_timeout(void) {
    HppliteL1 *l1;
    HppliteCrypto *crypto;
    HppliteKeypair seqKp, witKp;
    int rc;

    TEST("sequencer_timeout");

    l1 = hpplite_l1_create_mock();
    crypto = hpplite_crypto_init();

    hpplite_crypto_keypair_from_privkey(crypto, &seqKp, SEQ_PRIVKEY);
    hpplite_crypto_keypair_from_privkey(crypto, &witKp, WIT1_PRIVKEY);

    /* Set short timeout for testing */
    hpplite_l1_mock_set_config(l1, 2, 100, 60);  /* 60 second timeout */

    /* Register and claim */
    hpplite_l1_register_witness(l1, SEQ_PRIVKEY);
    hpplite_l1_register_witness(l1, WIT1_PRIVKEY);
    hpplite_l1_claim_sequencer(l1, SEQ_PRIVKEY);

    /* Timeout should not be expired yet */
    if (hpplite_l1_sequencer_timeout_expired(l1)) {
        FAIL("Timeout should not be expired immediately");
        goto cleanup;
    }

    /* Witness cannot claim */
    rc = hpplite_l1_claim_sequencer(l1, WIT1_PRIVKEY);
    if (rc == 0) {
        FAIL("Should not claim before timeout");
        goto cleanup;
    }

    /* Advance time past timeout */
    hpplite_l1_mock_advance_time(l1, 61);

    /* Timeout should be expired */
    if (!hpplite_l1_sequencer_timeout_expired(l1)) {
        FAIL("Timeout should be expired after 61 seconds");
        goto cleanup;
    }

    /* Now witness can claim */
    rc = hpplite_l1_claim_sequencer(l1, WIT1_PRIVKEY);
    if (rc != 0) {
        FAIL("Should be able to claim after timeout");
        goto cleanup;
    }

    /* Witness is now sequencer */
    if (!hpplite_l1_is_sequencer(l1, witKp.pubkey)) {
        FAIL("Witness should be new sequencer");
        goto cleanup;
    }

    if (hpplite_l1_is_sequencer(l1, seqKp.pubkey)) {
        FAIL("Old sequencer should not be sequencer");
        goto cleanup;
    }

    PASS();

cleanup:
    hpplite_crypto_free(crypto);
    hpplite_l1_disconnect(l1);
}

/*
** Test 5: Checkpoint submission
*/
static void test_checkpoint_submission(void) {
    HppliteL1 *l1;
    HppliteCrypto *crypto;
    HppliteKeypair seqKp, wit1Kp, wit2Kp;
    HppliteCheckpoint *cp;
    HppliteCheckpointAttestation attestations[2];
    unsigned char cpHash[32];
    char *txHash;

    TEST("checkpoint_submission");

    l1 = hpplite_l1_create_mock();
    crypto = hpplite_crypto_init();

    hpplite_crypto_keypair_from_privkey(crypto, &seqKp, SEQ_PRIVKEY);
    hpplite_crypto_keypair_from_privkey(crypto, &wit1Kp, WIT1_PRIVKEY);
    hpplite_crypto_keypair_from_privkey(crypto, &wit2Kp, WIT2_PRIVKEY);

    /* Setup: register witnesses, claim sequencer */
    hpplite_l1_register_witness(l1, SEQ_PRIVKEY);
    hpplite_l1_register_witness(l1, WIT1_PRIVKEY);
    hpplite_l1_register_witness(l1, WIT2_PRIVKEY);
    hpplite_l1_claim_sequencer(l1, SEQ_PRIVKEY);

    /* Create checkpoint */
    cp = hpplite_checkpoint_new(1, 100);
    memset(cp->preStateRoot, 0xAA, 32);
    memset(cp->postStateRoot, 0xBB, 32);
    memset(cp->batchesHash, 0xCC, 32);

    /* Compute checkpoint hash for signing */
    hpplite_checkpoint_hash(cp, cpHash);

    /* Create attestations */
    memset(&attestations[0], 0, sizeof(HppliteCheckpointAttestation));
    attestations[0].fromHeight = 1;
    attestations[0].toHeight = 100;
    memcpy(attestations[0].postStateRoot, cp->postStateRoot, 32);
    memcpy(attestations[0].witnessPubkey, wit1Kp.pubkey, 33);

    HppliteSignature sig1;
    hpplite_crypto_sign(crypto, &wit1Kp, cpHash, &sig1);
    memcpy(attestations[0].signature, sig1.sig, 64);
    attestations[0].recid = sig1.recid;

    memset(&attestations[1], 0, sizeof(HppliteCheckpointAttestation));
    attestations[1].fromHeight = 1;
    attestations[1].toHeight = 100;
    memcpy(attestations[1].postStateRoot, cp->postStateRoot, 32);
    memcpy(attestations[1].witnessPubkey, wit2Kp.pubkey, 33);

    HppliteSignature sig2;
    hpplite_crypto_sign(crypto, &wit2Kp, cpHash, &sig2);
    memcpy(attestations[1].signature, sig2.sig, 64);
    attestations[1].recid = sig2.recid;

    /* Submit with only 1 attestation (should fail, need 2) */
    txHash = hpplite_l1_submit_checkpoint(l1, cp, attestations, 1);
    if (txHash != NULL) {
        FAIL("Should fail with insufficient attestations");
        sqlite3_free(txHash);
        goto cleanup;
    }

    /* Submit with 2 attestations (should succeed) */
    txHash = hpplite_l1_submit_checkpoint(l1, cp, attestations, 2);
    if (txHash == NULL) {
        FAIL("Should succeed with 2 attestations");
        goto cleanup;
    }
    sqlite3_free(txHash);

    /* Verify checkpoint was stored */
    if (hpplite_l1_mock_get_checkpoint_count(l1) != 1) {
        FAIL("Should have 1 checkpoint");
        goto cleanup;
    }

    const HppliteCheckpoint *storedCp = hpplite_l1_mock_get_last_checkpoint(l1);
    if (!storedCp || storedCp->toHeight != 100) {
        FAIL("Stored checkpoint mismatch");
        goto cleanup;
    }

    PASS();

cleanup:
    hpplite_checkpoint_free(cp);
    hpplite_crypto_free(crypto);
    hpplite_l1_disconnect(l1);
}

/*
** Test 6: Block advancement
*/
static void test_block_advancement(void) {
    HppliteL1 *l1;
    uint64_t initialBlock, initialTime;

    TEST("block_advancement");

    l1 = hpplite_l1_create_mock();

    initialBlock = hpplite_l1_mock_get_block(l1);
    initialTime = hpplite_l1_mock_get_timestamp(l1);

    /* Advance 10 blocks */
    hpplite_l1_mock_advance_blocks(l1, 10);

    if (hpplite_l1_mock_get_block(l1) != initialBlock + 10) {
        FAIL("Block should advance by 10");
        hpplite_l1_disconnect(l1);
        return;
    }

    /* Time should advance by 120 seconds (12 sec/block) */
    if (hpplite_l1_mock_get_timestamp(l1) != initialTime + 120) {
        FAIL("Time should advance by 120 seconds");
        hpplite_l1_disconnect(l1);
        return;
    }

    /* Poll should also advance */
    hpplite_l1_poll(l1);
    if (hpplite_l1_mock_get_block(l1) != initialBlock + 11) {
        FAIL("Poll should advance block by 1");
        hpplite_l1_disconnect(l1);
        return;
    }

    hpplite_l1_disconnect(l1);
    PASS();
}

int main(void) {
    printf("\n=== L1 Mock Tests ===\n\n");

    test_l1_create();
    test_witness_registration();
    test_sequencer_claim();
    test_sequencer_timeout();
    test_checkpoint_submission();
    test_block_advancement();

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
