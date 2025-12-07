/*
** HPPLite Replay Benchmark
**
** Benchmarks L1-anchored replay from genesis.
** Similar to SQLite's speedtest1.c pattern.
**
** Usage:
**   ./bench_replay [OPTIONS]
**
** Options:
**   --batches N      Number of batches to generate (default: 100)
**   --checkpoint N   Batches per checkpoint (default: 10)
**   --stats          Show detailed statistics
**   --help           Show this help
*/

#include "node.h"
#include "l1_interface.h"
#include "crypto.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

/* Configuration */
static int g_nBatches = 100;
static int g_checkpointInterval = 10;
static int g_showStats = 0;

/* Timing */
static double getTime(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

/* Test keys */
static const unsigned char SEQ_PRIVKEY[32] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
};

static const unsigned char WIT_PRIVKEY[32] = {
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40
};

static void showHelp(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("\nOptions:\n");
    printf("  --batches N      Number of batches to generate (default: %d)\n", g_nBatches);
    printf("  --checkpoint N   Batches per checkpoint (default: %d)\n", g_checkpointInterval);
    printf("  --stats          Show detailed statistics\n");
    printf("  --help           Show this help\n");
}

static void parseArgs(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--batches") == 0 && i + 1 < argc) {
            g_nBatches = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--checkpoint") == 0 && i + 1 < argc) {
            g_checkpointInterval = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--stats") == 0) {
            g_showStats = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            showHelp(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            showHelp(argv[0]);
            exit(1);
        }
    }
}

static void cleanup_dirs(void) {
    system("rm -rf /tmp/hpplite_bench_*");
}

static char* create_dir(const char *suffix) {
    char *path = sqlite3_mprintf("/tmp/hpplite_bench_%s", suffix);
    char *cmd = sqlite3_mprintf("mkdir -p %s", path);
    system(cmd);
    sqlite3_free(cmd);
    return path;
}

int main(int argc, char **argv) {
    HppliteNodeConfig seqCfg, replayCfg;
    HppliteNode *seqNode, *replayNode;
    HppliteL1 *l1;
    char *seqDir, *replayDir;
    char seqDb[256], replayDb[256];
    double tStart, tEnd;
    int i;

    parseArgs(argc, argv);

    printf("\n");
    printf("===========================================\n");
    printf("HPPLite Replay Benchmark\n");
    printf("===========================================\n");
    printf("Batches:            %d\n", g_nBatches);
    printf("Checkpoint interval: %d\n", g_checkpointInterval);
    printf("\n");

    cleanup_dirs();

    /* Create L1 mock */
    l1 = hpplite_l1_create_mock();
    hpplite_l1_mock_set_config(l1, 1, g_checkpointInterval, 3600);

    /* Setup sequencer */
    seqDir = create_dir("seq");
    snprintf(seqDb, sizeof(seqDb), "%s/db.sqlite", seqDir);

    memset(&seqCfg, 0, sizeof(seqCfg));
    seqCfg.nodeId = "sequencer";
    memcpy(seqCfg.privkey, SEQ_PRIVKEY, 32);
    seqCfg.dataDir = seqDir;
    seqCfg.dbPath = seqDb;
    seqCfg.checkpointInterval = g_checkpointInterval;
    seqCfg.requiredAttestations = 1;

    seqNode = hpplite_node_create(&seqCfg);
    if (!seqNode) {
        fprintf(stderr, "Failed to create sequencer\n");
        return 1;
    }

    hpplite_node_set_l1(seqNode, l1);

    /* Register and claim sequencer */
    unsigned char seqPubkey[33];
    hpplite_node_get_pubkey(seqNode, seqPubkey);
    hpplite_l1_mock_set_sequencer(l1, seqPubkey);
    hpplite_node_sync_role_from_l1(seqNode);
    hpplite_node_start(seqNode);

    /* ===== Phase 1: Generate batches ===== */
    printf("Phase 1: Generating %d batches...\n", g_nBatches);
    tStart = getTime();

    /* Create initial table */
    char *errMsg = NULL;
    int rc = hpplite_node_exec(seqNode,
        "CREATE TABLE bench(id INTEGER PRIMARY KEY, data TEXT, value REAL)", &errMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to create table: %s\n", errMsg);
        sqlite3_free(errMsg);
        return 1;
    }
    hpplite_node_flush_batch(seqNode);

    /* Generate batches with inserts */
    for (i = 2; i <= g_nBatches; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
            "INSERT INTO bench(id, data, value) VALUES(%d, 'data_%d', %d.%d)",
            i, i, i * 100, i % 100);

        rc = hpplite_node_exec(seqNode, sql, &errMsg);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "Batch %d failed: %s\n", i, errMsg);
            sqlite3_free(errMsg);
            return 1;
        }
        hpplite_node_flush_batch(seqNode);

        /* Create checkpoint if needed */
        if (hpplite_node_should_checkpoint(seqNode)) {
            HppliteCheckpoint *cp = hpplite_node_create_checkpoint(seqNode);
            if (cp) {
                /* Self-attest (single node benchmark) */
                unsigned char cpHash[32];
                hpplite_checkpoint_hash(cp, cpHash);

                HppliteCheckpointAttestation att;
                memset(&att, 0, sizeof(att));
                att.fromHeight = cp->fromHeight;
                att.toHeight = cp->toHeight;
                memcpy(att.postStateRoot, cp->postStateRoot, 32);
                memcpy(att.witnessPubkey, seqPubkey, 33);

                HppliteSignature sig;
                hpplite_crypto_sign(seqNode->crypto, &seqNode->keypair, cpHash, &sig);
                memcpy(att.signature, sig.sig, 64);
                att.recid = sig.recid;

                hpplite_node_receive_checkpoint_attestation(seqNode, &att);
                char *txHash = hpplite_node_submit_checkpoint(seqNode);
                if (txHash) sqlite3_free(txHash);
            }
        }

        if (g_showStats && i % 100 == 0) {
            printf("  Generated %d/%d batches...\n", i, g_nBatches);
        }
    }

    tEnd = getTime();
    double genTime = tEnd - tStart;
    printf("  Done: %.3f seconds (%.1f batches/sec)\n\n", genTime, g_nBatches / genTime);

    /* Get final state */
    unsigned char seqRoot[32];
    hpplite_node_get_state_root(seqNode, seqRoot);
    uint64_t seqHeight = hpplite_node_get_height(seqNode);

    /* Get L1 checkpoint info */
    int nCheckpoints = hpplite_l1_mock_get_checkpoint_count(l1);
    const HppliteCheckpoint *lastCp = hpplite_l1_mock_get_last_checkpoint(l1);

    printf("State:\n");
    printf("  Height:      %llu\n", (unsigned long long)seqHeight);
    printf("  Checkpoints: %d\n", nCheckpoints);
    if (lastCp) {
        printf("  Last CP:     %llu -> %llu\n",
               (unsigned long long)lastCp->fromHeight,
               (unsigned long long)lastCp->toHeight);
    }
    printf("\n");

    /* ===== Phase 2: Replay from genesis ===== */
    printf("Phase 2: Replaying from genesis...\n");

    replayDir = create_dir("replay");
    snprintf(replayDb, sizeof(replayDb), "%s/db.sqlite", replayDir);

    memset(&replayCfg, 0, sizeof(replayCfg));
    replayCfg.nodeId = "replay";
    memcpy(replayCfg.privkey, WIT_PRIVKEY, 32);
    replayCfg.dataDir = replayDir;
    replayCfg.dbPath = replayDb;

    replayNode = hpplite_node_create(&replayCfg);
    if (!replayNode) {
        fprintf(stderr, "Failed to create replay node\n");
        return 1;
    }
    hpplite_node_set_role(replayNode, HPPLITE_ROLE_WITNESS);

    tStart = getTime();

    int batchesReplayed = 0;
    for (i = 1; i <= g_nBatches; i++) {
        char batchPath[64];
        snprintf(batchPath, sizeof(batchPath), "batches/%08llu.json", (unsigned long long)i);

        HppliteBatch *batch = hpplite_storage_load_batch(seqNode->storage, batchPath);
        if (!batch) {
            fprintf(stderr, "Failed to load batch %d\n", i);
            break;
        }

        rc = hpplite_node_verify_batch(replayNode, batch, batchPath);
        hpplite_batch_free(batch);

        if (rc != 0) {
            fprintf(stderr, "Failed to replay batch %d\n", i);
            return 1;
        }

        batchesReplayed++;

        if (g_showStats && i % 100 == 0) {
            printf("  Replayed %d/%d batches...\n", i, g_nBatches);
        }
    }

    tEnd = getTime();
    double replayTime = tEnd - tStart;

    printf("  Done: %.3f seconds (%.1f batches/sec)\n\n", replayTime, batchesReplayed / replayTime);

    /* Verify state */
    unsigned char replayRoot[32];
    hpplite_node_get_state_root(replayNode, replayRoot);

    int stateMatch = (memcmp(seqRoot, replayRoot, 32) == 0);

    printf("Verification:\n");
    printf("  Batches replayed: %d\n", batchesReplayed);
    printf("  State match:      %s\n", stateMatch ? "YES" : "NO");

    if (lastCp) {
        int cpMatch = (memcmp(lastCp->postStateRoot, replayRoot, 32) == 0);
        printf("  L1 CP match:      %s\n", cpMatch ? "YES" : "NO");
    }

    printf("\n");
    printf("===========================================\n");
    printf("Summary\n");
    printf("===========================================\n");
    printf("Generation:  %.3f sec (%.1f batches/sec)\n", genTime, g_nBatches / genTime);
    printf("Replay:      %.3f sec (%.1f batches/sec)\n", replayTime, batchesReplayed / replayTime);
    printf("Result:      %s\n", stateMatch ? "PASS" : "FAIL");
    printf("\n");

    /* Cleanup */
    hpplite_node_destroy(seqNode);
    hpplite_node_destroy(replayNode);
    hpplite_l1_disconnect(l1);
    cleanup_dirs();

    return stateMatch ? 0 : 1;
}
