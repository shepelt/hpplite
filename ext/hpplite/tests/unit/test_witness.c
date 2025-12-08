/*
** HPPLite M3 Witness Attestation Test
**
** Tests witness batch verification and attestation signing:
** 1. Sequencer creates and signs batches
** 2. Witness receives, verifies, and signs attestations
** 3. Invalid batches are rejected
** 4. Attestation signatures are valid
*/

#include "node.h"
#include "crypto.h"
#include "batch.h"
#include "l1_interface.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEQ_DIR "/tmp/hpplite_witness_seq"
#define WIT_DIR "/tmp/hpplite_witness_wit"

static int tests_run = 0;
static int tests_passed = 0;

static void print_hex(const unsigned char *data, int len) {
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
}

/* Sequencer key */
static const unsigned char SEQ_PRIVKEY[32] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
};

/* Witness key */
static const unsigned char WIT_PRIVKEY[32] = {
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40
};

static HppliteBatch *load_batch(const char *dataDir, uint64_t height) {
    char path[256];
    snprintf(path, sizeof(path), "%s/batches/%08llu.json",
             dataDir, (unsigned long long)height);

    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *json = malloc(len + 1);
    fread(json, 1, len, f);
    json[len] = '\0';
    fclose(f);

    HppliteBatch *batch = hpplite_batch_from_json(json);
    free(json);
    return batch;
}

/*
** Test 1: Witness successfully verifies valid batch
*/
static int test_valid_attestation(void) {
    printf("Test 1: Witness attests valid batch\n");
    tests_run++;

    char cmd[256];
    char *errMsg = NULL;
    int rc;

    /* Create sequencer */
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", SEQ_DIR, SEQ_DIR);
    system(cmd);

    HppliteNodeConfig seqCfg;
    memset(&seqCfg, 0, sizeof(seqCfg));
    seqCfg.nodeId = "sequencer";
    memcpy(seqCfg.privkey, SEQ_PRIVKEY, 32);
    seqCfg.dataDir = SEQ_DIR;
    snprintf(cmd, sizeof(cmd), "%s/seq.db", SEQ_DIR);
    seqCfg.dbPath = cmd;

    HppliteNode *seqNode = hpplite_node_create(&seqCfg);
    if (!seqNode) {
        printf("  FAIL: Could not create sequencer\n");
        return -1;
    }
    hpplite_node_set_role(seqNode, HPPLITE_ROLE_SEQUENCER);
    hpplite_node_start(seqNode);

    /* Execute some SQL and create batch */
    rc = hpplite_node_exec(seqNode, "CREATE TABLE attest_test(id INT, data TEXT)", &errMsg);
    if (rc != SQLITE_OK) {
        printf("  FAIL: CREATE: %s\n", errMsg);
        sqlite3_free(errMsg);
        hpplite_node_destroy(seqNode);
        return -1;
    }

    rc = hpplite_node_exec(seqNode, "INSERT INTO attest_test VALUES(1, 'hello')", &errMsg);
    if (rc != SQLITE_OK) {
        printf("  FAIL: INSERT: %s\n", errMsg);
        sqlite3_free(errMsg);
        hpplite_node_destroy(seqNode);
        return -1;
    }

    uint64_t height = hpplite_node_flush_batch(seqNode);
    printf("  Sequencer created batch %llu\n", (unsigned long long)height);

    unsigned char seqStateRoot[32];
    hpplite_node_get_state_root(seqNode, seqStateRoot);
    printf("  Sequencer state root: ");
    print_hex(seqStateRoot, 32);
    printf("\n");

    /* Load the batch */
    HppliteBatch *batch = load_batch(SEQ_DIR, height);
    if (!batch) {
        printf("  FAIL: Could not load batch\n");
        hpplite_node_destroy(seqNode);
        return -1;
    }

    /* Create witness node */
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", WIT_DIR, WIT_DIR);
    system(cmd);

    HppliteNodeConfig witCfg;
    memset(&witCfg, 0, sizeof(witCfg));
    witCfg.nodeId = "witness";
    memcpy(witCfg.privkey, WIT_PRIVKEY, 32);
    witCfg.dataDir = WIT_DIR;
    snprintf(cmd, sizeof(cmd), "%s/wit.db", WIT_DIR);
    witCfg.dbPath = cmd;

    HppliteNode *witNode = hpplite_node_create(&witCfg);
    if (!witNode) {
        printf("  FAIL: Could not create witness\n");
        hpplite_batch_free(batch);
        hpplite_node_destroy(seqNode);
        return -1;
    }
    hpplite_node_set_role(witNode, HPPLITE_ROLE_WITNESS);
    hpplite_node_start(witNode);

    /* Witness verifies batch */
    char batchPath[256];
    snprintf(batchPath, sizeof(batchPath), "%s/batches/%08llu.json",
             SEQ_DIR, (unsigned long long)height);

    rc = hpplite_node_verify_batch(witNode, batch, batchPath);
    if (rc != 0) {
        printf("  FAIL: Witness rejected valid batch\n");
        hpplite_batch_free(batch);
        hpplite_node_destroy(witNode);
        hpplite_node_destroy(seqNode);
        return -1;
    }
    printf("  Witness verified batch successfully\n");

    /* Verify witness has same state root */
    unsigned char witStateRoot[32];
    hpplite_node_get_state_root(witNode, witStateRoot);
    printf("  Witness state root:   ");
    print_hex(witStateRoot, 32);
    printf("\n");

    if (memcmp(seqStateRoot, witStateRoot, 32) == 0) {
        printf("  PASS: State roots match!\n");
        tests_passed++;
    } else {
        printf("  FAIL: State roots don't match\n");
    }

    hpplite_batch_free(batch);
    hpplite_node_destroy(witNode);
    hpplite_node_destroy(seqNode);
    return 0;
}

/*
** Test 2: Witness rejects tampered batch
*/
static int test_reject_tampered_batch(void) {
    printf("\nTest 2: Witness rejects tampered batch\n");
    tests_run++;

    char cmd[256];
    char *errMsg = NULL;
    int rc;

    /* Create sequencer */
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", SEQ_DIR, SEQ_DIR);
    system(cmd);

    HppliteNodeConfig seqCfg;
    memset(&seqCfg, 0, sizeof(seqCfg));
    seqCfg.nodeId = "sequencer";
    memcpy(seqCfg.privkey, SEQ_PRIVKEY, 32);
    seqCfg.dataDir = SEQ_DIR;
    snprintf(cmd, sizeof(cmd), "%s/seq.db", SEQ_DIR);
    seqCfg.dbPath = cmd;

    HppliteNode *seqNode = hpplite_node_create(&seqCfg);
    if (!seqNode) {
        printf("  FAIL: Could not create sequencer\n");
        return -1;
    }
    hpplite_node_set_role(seqNode, HPPLITE_ROLE_SEQUENCER);
    hpplite_node_start(seqNode);

    /* Execute some SQL */
    rc = hpplite_node_exec(seqNode, "CREATE TABLE tamper_test(x INT)", &errMsg);
    if (rc != SQLITE_OK) {
        printf("  FAIL: CREATE: %s\n", errMsg);
        sqlite3_free(errMsg);
        hpplite_node_destroy(seqNode);
        return -1;
    }

    rc = hpplite_node_exec(seqNode, "INSERT INTO tamper_test VALUES(42)", &errMsg);
    if (rc != SQLITE_OK) {
        printf("  FAIL: INSERT: %s\n", errMsg);
        sqlite3_free(errMsg);
        hpplite_node_destroy(seqNode);
        return -1;
    }

    uint64_t height = hpplite_node_flush_batch(seqNode);
    printf("  Sequencer created batch %llu\n", (unsigned long long)height);

    /* Load and tamper with batch */
    HppliteBatch *batch = load_batch(SEQ_DIR, height);
    if (!batch) {
        printf("  FAIL: Could not load batch\n");
        hpplite_node_destroy(seqNode);
        return -1;
    }

    /* Tamper: flip a bit in the state root */
    batch->postStateRoot[0] ^= 0x01;
    printf("  Tampered with batch state root\n");

    /* Create witness */
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", WIT_DIR, WIT_DIR);
    system(cmd);

    HppliteNodeConfig witCfg;
    memset(&witCfg, 0, sizeof(witCfg));
    witCfg.nodeId = "witness";
    memcpy(witCfg.privkey, WIT_PRIVKEY, 32);
    witCfg.dataDir = WIT_DIR;
    snprintf(cmd, sizeof(cmd), "%s/wit.db", WIT_DIR);
    witCfg.dbPath = cmd;

    HppliteNode *witNode = hpplite_node_create(&witCfg);
    if (!witNode) {
        printf("  FAIL: Could not create witness\n");
        hpplite_batch_free(batch);
        hpplite_node_destroy(seqNode);
        return -1;
    }
    hpplite_node_set_role(witNode, HPPLITE_ROLE_WITNESS);
    hpplite_node_start(witNode);

    /* Witness should reject tampered batch */
    char batchPath[256];
    snprintf(batchPath, sizeof(batchPath), "%s/batches/%08llu.json",
             SEQ_DIR, (unsigned long long)height);

    rc = hpplite_node_verify_batch(witNode, batch, batchPath);
    if (rc == 0) {
        printf("  FAIL: Witness accepted tampered batch!\n");
    } else {
        printf("  PASS: Witness correctly rejected tampered batch\n");
        tests_passed++;
    }

    hpplite_batch_free(batch);
    hpplite_node_destroy(witNode);
    hpplite_node_destroy(seqNode);
    return 0;
}

/*
** Test 3: Sign attestation for checkpoint
*/
static int test_sign_attestation(void) {
    printf("\nTest 3: Sign checkpoint attestation\n");
    tests_run++;

    /* Initialize crypto */
    HppliteCrypto *crypto = hpplite_crypto_init();
    if (!crypto) {
        printf("  FAIL: Could not init crypto\n");
        return -1;
    }

    /* Generate witness keypair */
    HppliteKeypair kp;
    int rc = hpplite_crypto_keypair_from_privkey(crypto, &kp, WIT_PRIVKEY);
    if (rc != 0) {
        printf("  FAIL: Could not generate keypair\n");
        hpplite_crypto_free(crypto);
        return -1;
    }

    printf("  Witness pubkey: ");
    print_hex(kp.pubkey, 33);
    printf("\n");

    /* Create a checkpoint to sign */
    HppliteCheckpoint checkpoint;
    memset(&checkpoint, 0, sizeof(checkpoint));
    checkpoint.fromHeight = 1;
    checkpoint.toHeight = 10;
    /* Some fake state root */
    for (int i = 0; i < 32; i++) checkpoint.postStateRoot[i] = i + 0xaa;

    /* Compute checkpoint hash */
    unsigned char cpHash[32];
    hpplite_checkpoint_hash(&checkpoint, cpHash);
    printf("  Checkpoint hash: ");
    print_hex(cpHash, 32);
    printf("\n");

    /* Sign the checkpoint */
    HppliteSignature sig;
    rc = hpplite_crypto_sign(crypto, &kp, cpHash, &sig);
    if (rc != 0) {
        printf("  FAIL: Could not sign checkpoint\n");
        hpplite_crypto_free(crypto);
        return -1;
    }

    printf("  Signature r: ");
    print_hex(sig.sig, 32);
    printf("\n  Signature s: ");
    print_hex(sig.sig + 32, 32);
    printf("\n  Signature v: %d\n", sig.recid + 27);

    /* Verify the signature */
    rc = hpplite_crypto_verify(crypto, kp.pubkey, cpHash, &sig);
    if (rc != 1) {
        printf("  FAIL: Signature verification failed\n");
        hpplite_crypto_free(crypto);
        return -1;
    }

    printf("  PASS: Signature verified successfully\n");
    tests_passed++;

    hpplite_crypto_free(crypto);
    return 0;
}

/*
** Test 4: Multiple witnesses attest same batch
*/
static int test_multiple_witnesses(void) {
    printf("\nTest 4: Multiple witnesses attest same batch\n");
    tests_run++;

    HppliteCrypto *crypto = hpplite_crypto_init();
    if (!crypto) {
        printf("  FAIL: Could not init crypto\n");
        return -1;
    }

    /* Two witness keys */
    unsigned char wit1_key[32], wit2_key[32];
    for (int i = 0; i < 32; i++) {
        wit1_key[i] = i + 0x10;
        wit2_key[i] = i + 0x50;
    }

    HppliteKeypair kp1, kp2;
    hpplite_crypto_keypair_from_privkey(crypto, &kp1, wit1_key);
    hpplite_crypto_keypair_from_privkey(crypto, &kp2, wit2_key);

    printf("  Witness 1 pubkey: ");
    print_hex(kp1.pubkey, 33);
    printf("\n  Witness 2 pubkey: ");
    print_hex(kp2.pubkey, 33);
    printf("\n");

    /* Create checkpoint */
    HppliteCheckpoint checkpoint;
    memset(&checkpoint, 0, sizeof(checkpoint));
    checkpoint.fromHeight = 1;
    checkpoint.toHeight = 100;
    for (int i = 0; i < 32; i++) checkpoint.postStateRoot[i] = i * 3;

    unsigned char cpHash[32];
    hpplite_checkpoint_hash(&checkpoint, cpHash);

    /* Both witnesses sign */
    HppliteSignature sig1, sig2;
    int rc1 = hpplite_crypto_sign(crypto, &kp1, cpHash, &sig1);
    int rc2 = hpplite_crypto_sign(crypto, &kp2, cpHash, &sig2);

    if (rc1 != 0 || rc2 != 0) {
        printf("  FAIL: Signing failed\n");
        hpplite_crypto_free(crypto);
        return -1;
    }

    printf("  Both witnesses signed checkpoint\n");

    /* Verify both signatures */
    int v1 = hpplite_crypto_verify(crypto, kp1.pubkey, cpHash, &sig1);
    int v2 = hpplite_crypto_verify(crypto, kp2.pubkey, cpHash, &sig2);

    if (v1 != 1 || v2 != 1) {
        printf("  FAIL: Verification failed\n");
        hpplite_crypto_free(crypto);
        return -1;
    }

    /* Verify signatures are different (different keys) */
    if (memcmp(sig1.sig, sig2.sig, 64) == 0) {
        printf("  FAIL: Signatures should be different\n");
        hpplite_crypto_free(crypto);
        return -1;
    }

    printf("  PASS: Both witness signatures verified\n");
    tests_passed++;

    hpplite_crypto_free(crypto);
    return 0;
}

int main(void) {
    printf("test_m3_witness: Witness attestation tests\n\n");

    test_valid_attestation();
    test_reject_tampered_batch();
    test_sign_attestation();
    test_multiple_witnesses();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
