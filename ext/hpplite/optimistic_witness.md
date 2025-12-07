# HPPLite Optimistic Witness Model

## Overview

HPPLite uses an **optimistic witness** model for validation instead of traditional fraud proofs with bisection games. This approach is practical for SQL-based L2s where instruction-level dispute resolution is impractical.

## Why Not Traditional Fraud Proofs?

| Approach | EVM L2 | SQL L2 (HPPLite) |
|----------|--------|------------------|
| Instruction trace | Opcodes are atomic | No standard trace |
| Bisection game | Narrow to 1 opcode | Can't bisect SQL |
| On-chain proof | Verify single step | Would need full re-execution |
| Determinism | Guaranteed | Query optimizer may vary |

**Conclusion:** Bisection-based fraud proofs are impractical for SQL execution.

## Pre-emptive Validation vs Reactive Fraud Proofs

The optimistic witness model uses **pre-emptive validation** rather than reactive fraud proofs:

```
Traditional Optimistic Rollup (Reactive):
─────────────────────────────────────────────────────────────
Sequencer posts ──► Assumed valid ──► [7 day window] ──► Finalized
                                            │
                                  Challenge only if fraud detected
                                  (reactive, emergency response)


HPPLite Optimistic Witness (Pre-emptive):
─────────────────────────────────────────────────────────────
Sequencer posts ──► Witnesses verify ──► Co-sign ──► Finalized
                          │
                Pre-emptive validation every batch
                (proactive, continuous verification)
```

| Aspect | Reactive (Fraud Proof) | Pre-emptive (Witness) |
|--------|------------------------|----------------------|
| **Philosophy** | "Trust, then verify if suspicious" | "Verify, then trust" |
| **Bad state** | Gets posted, challenged later | Never finalized |
| **Finality** | Delayed (challenge window) | Fast (once signed) |
| **Verification** | Only if challenged | Every batch, always |

**Key insight:** Witnesses continuously validate. Invalid state never gets finalized because it won't get enough signatures.

## Optimistic Witness Model

### Architecture

```
                    ┌─────────────────────────────┐
                    │         SEQUENCER           │
                    │  (executes SQL, produces    │
                    │   batches + state roots)    │
                    └──────────────┬──────────────┘
                                   │
                    ┌──────────────┼──────────────┐
                    │              │              │
                    ▼              ▼              ▼
              ┌──────────┐  ┌──────────┐  ┌──────────┐
              │ Witness 1│  │ Witness 2│  │ Witness 3│
              │ (replay) │  │ (replay) │  │ (replay) │
              └────┬─────┘  └────┬─────┘  └────┬─────┘
                   │             │             │
                   └─────────────┼─────────────┘
                                 │
                    ┌────────────▼────────────┐
                    │     L1 SYSTEM CONTRACT  │
                    │  - Accepts commitments  │
                    │  - Requires N/M sigs    │
                    │  - Stores checkpoints   │
                    └─────────────────────────┘
```

### Roles

#### Sequencer
- Executes SQL transactions
- Produces batches with pre/post state roots
- Posts batches to DA layer
- Proposes commitments to L1

#### Witnesses
- Subscribe to batch stream (DA layer or direct from sequencer)
- Replay each batch independently
- Verify state roots match
- Co-sign valid commitments
- Refuse to sign (and alert) on invalid state

#### L1 System Contract
- Accepts commitments only with sufficient witness signatures
- Stores finalized checkpoints (height + state root)
- Provides canonical state reference for external systems

### Flow

```
1. Sequencer executes SQL batch
   └─► Batch {height: N, txns: [...], pre_state, post_state}

2. Sequencer posts to DA
   └─► Returns: batch_ref (IPFS CID / Arweave TX / etc)

3. Sequencer proposes commitment to L1
   └─► propose(height: N, state_root, batch_ref)

4. Witnesses receive batch (from DA or sequencer)
   └─► Download batch, replay SQL, compute state root

5. Witnesses verify and sign
   └─► IF computed_root == proposed_root:
         sign(height, state_root, batch_ref)
       ELSE:
         alert("Invalid state at height N")
         refuse to sign

6. L1 contract finalizes
   └─► IF signatures >= threshold (e.g., 2/3):
         finalize(height, state_root)
       ELSE:
         commitment stays pending
```

## Witness Implementation

### Witness Node Components

```c
struct HppliteWitness {
    sqlite3 *db;              // Local replica database
    HppliteCtx *ctx;          // State tracking context
    uint64_t lastVerified;    // Last verified height
    unsigned char stateRoot[32]; // Current verified state

    // Signing key (for L1 attestations)
    unsigned char privkey[32];
    unsigned char pubkey[33];
};
```

### Verification Loop

```c
void witness_verify_batch(HppliteWitness *w, HppliteBatch *batch) {
    // 1. Check continuity
    assert(batch->height == w->lastVerified + 1);
    assert(memcmp(batch->preStateRoot, w->stateRoot, 32) == 0);

    // 2. Replay all transactions
    for (int i = 0; i < batch->nTxns; i++) {
        sqlite3_exec(w->db, batch->aTxns[i].zSql, NULL, NULL, NULL);
    }

    // 3. Compute state root
    unsigned char computed[32];
    hpplite_get_state_root(w->ctx, computed);

    // 4. Verify matches claimed post-state
    if (memcmp(computed, batch->postStateRoot, 32) != 0) {
        witness_alert_invalid(w, batch);
        return;  // DO NOT SIGN
    }

    // 5. Update local state
    memcpy(w->stateRoot, computed, 32);
    w->lastVerified = batch->height;

    // 6. Sign attestation
    witness_sign_commitment(w, batch->height, computed);
}
```

### Attestation Message

```json
{
    "type": "witness_attestation",
    "height": 1234,
    "state_root": "0xabc...",
    "batch_ref": "ipfs://Qm...",
    "witness_pubkey": "0x02...",
    "signature": "0x..."
}
```

## L1 System Contract (Pseudocode)

```solidity
contract HPPLiteCheckpoint {
    struct Commitment {
        uint64 height;
        bytes32 stateRoot;
        string batchRef;
        uint256 proposedAt;
        mapping(address => bool) signatures;
        uint8 signatureCount;
        bool finalized;
    }

    mapping(uint64 => Commitment) public commitments;
    mapping(address => bool) public isWitness;
    uint8 public requiredSignatures;  // e.g., 2 of 3
    uint64 public latestFinalized;

    function propose(uint64 height, bytes32 stateRoot, string batchRef) external {
        require(height == latestFinalized + 1, "Must be sequential");
        commitments[height] = Commitment(height, stateRoot, batchRef, block.timestamp);
    }

    function attest(uint64 height) external {
        require(isWitness[msg.sender], "Not a witness");
        Commitment storage c = commitments[height];
        require(!c.signatures[msg.sender], "Already signed");

        c.signatures[msg.sender] = true;
        c.signatureCount++;

        if (c.signatureCount >= requiredSignatures) {
            c.finalized = true;
            latestFinalized = height;
            emit Finalized(height, c.stateRoot);
        }
    }
}
```

## Security Properties

### What This Model Provides

| Property | Guarantee |
|----------|-----------|
| **Liveness** | Sequencer + any 1 witness can make progress |
| **Safety** | Invalid state requires corrupting N/M witnesses |
| **Detectability** | Any replica can detect sequencer fraud |
| **Accountability** | Witnesses stake reputation (optionally tokens) |

### Trust Assumptions

1. **Sequencer:** Trusted for liveness, not for correctness
2. **Witnesses:** At least `threshold` are honest and online
3. **DA Layer:** Batches remain available for replay
4. **L1:** Provides finality and ordering

### Attack Scenarios

| Attack | Mitigation |
|--------|------------|
| Sequencer posts bad state | Witnesses detect and refuse to sign |
| Sequencer censors txns | Witnesses see different batch, alert |
| Witness collusion | Need M witnesses to collude (set M appropriately) |
| DA unavailable | Witnesses can't verify, won't sign → no finality |
| Sequencer goes offline | Witness promoted to sequencer (see below) |

## Witnesses as Backup Sequencers

A key advantage of the witness model: witnesses maintain **full state replicas** and can step in as sequencers if the primary fails.

### Hot Standby Architecture

```
Normal operation:
┌────────────────┐      batches       ┌────────────────┐
│   SEQUENCER    │ ─────────────────► │   WITNESSES    │
│   (primary)    │                    │   (replicas)   │
└────────────────┘                    └────────────────┘
        │                                     │
        │ executes SQL                        │ replay SQL
        │ produces batches                    │ verify state
        ▼                                     ▼
   [full state]                          [full state]
                                         (identical)


Sequencer failure:
┌────────────────┐                    ┌────────────────┐
│   SEQUENCER    │  ✗ offline         │   WITNESS 1    │
│   (down)       │                    │ ──────────────►│ Promoted!
└────────────────┘                    │   Now primary  │
                                      └────────────────┘
                                              │
                                              ▼
                                      Starts accepting
                                      new transactions
```

### Benefits

| Benefit | Description |
|---------|-------------|
| **Zero sync time** | Witnesses already have current state |
| **No cold start** | No need to replay from genesis |
| **Instant failover** | Any witness can take over immediately |
| **High availability** | N-1 failures tolerated |
| **Decentralization path** | Can rotate sequencer among witnesses |
| **Censorship resistance** | Can't censor if any witness can sequence |

### Failover Implementation

```c
// Witness monitors sequencer liveness
void witness_monitor_loop(HppliteWitness *w) {
    while (1) {
        // Wait for next batch with timeout
        HppliteBatch *batch = fetch_batch_with_timeout(LIVENESS_TIMEOUT);

        if (batch == NULL) {
            // Sequencer appears down
            if (should_i_become_sequencer(w)) {
                witness_promote_to_sequencer(w);
                return;
            }
        } else {
            // Normal verification
            witness_verify_batch(w, batch);
        }
    }
}

// Promotion to sequencer
void witness_promote_to_sequencer(HppliteWitness *w) {
    // Already have full state - just start sequencing
    printf("Promoting witness to sequencer at height %llu\n",
           w->lastVerified + 1);

    // Begin accepting transactions
    sequencer_start(w->db, w->ctx, w->lastVerified);
}
```

### Sequencer Election on Failover

When sequencer fails, witnesses can elect new sequencer:

1. **Simple rotation** - Pre-defined order (witness 1 → 2 → 3)
2. **Leader election** - Witnesses vote on who takes over
3. **First to propose** - First witness to produce valid batch wins
4. **L1 contract decides** - Contract designates backup order

```solidity
// L1 contract can track sequencer assignment
contract HPPLiteCheckpoint {
    address public currentSequencer;
    address[] public witnessBackups;
    uint256 public lastBatchTime;

    function claimSequencer() external {
        require(isWitness[msg.sender], "Not a witness");
        require(block.timestamp > lastBatchTime + LIVENESS_TIMEOUT,
                "Current sequencer still active");

        currentSequencer = msg.sender;
        emit SequencerChanged(msg.sender);
    }
}
```

### Decentralized Sequencing

The witness model naturally extends to **rotating sequencer** for further decentralization:

```
Round 1: Witness A sequences → B, C verify → checkpoint
Round 2: Witness B sequences → A, C verify → checkpoint
Round 3: Witness C sequences → A, B verify → checkpoint
...
```

This eliminates single-sequencer trust entirely while maintaining the same simple verification model.

## Comparison to Other Models

| Model | Trust | Finality | Complexity |
|-------|-------|----------|------------|
| **Full fraud proofs** | 1-of-N verifiers | ~7 days | High |
| **ZK proofs** | Math only | Instant | Very high |
| **Optimistic witness** | M-of-N witnesses | Minutes | Low |
| **Pure sequencer** | Sequencer only | Instant | Minimal |

## Configuration Parameters

```c
#define HPPLITE_WITNESS_THRESHOLD  2    // Signatures needed
#define HPPLITE_WITNESS_COUNT      3    // Total witnesses
#define HPPLITE_CHECKPOINT_INTERVAL 100 // Blocks between L1 checkpoints
#define HPPLITE_ATTESTATION_TIMEOUT 300 // Seconds to collect signatures
```

## Checkpoint Frequency

Not every batch needs L1 attestation. Options:

1. **Every N batches** - Checkpoint at height 100, 200, 300...
2. **Time-based** - Checkpoint every 10 minutes
3. **On-demand** - Checkpoint when requested (e.g., for withdrawals)

```
Batch 1 ─► Batch 2 ─► ... ─► Batch 100 ─► CHECKPOINT ─► L1
                                              │
                              Witnesses sign this one
```

## Future Extensions

1. **Economic staking** - Witnesses bond tokens, slashable for signing invalid state
2. **Permissionless witnesses** - Anyone can run a witness, weighted by stake
3. **Light client proofs** - Merkle proofs for specific state queries
4. **Cross-chain bridges** - Use checkpoints to prove state to other chains

## Summary

The optimistic witness model provides:
- **Practical verification** for SQL-based L2
- **Lower complexity** than fraud proofs
- **Faster finality** than challenge periods
- **Configurable trust** via witness threshold

It's a pragmatic middle ground between "trust sequencer completely" and "trustless fraud proofs" - suitable for HPPLite's SQL execution model.
