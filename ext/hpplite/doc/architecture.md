# HPPLite Architecture

**Optimistic Witness Consensus for SQLite**

## Overview

HPPLite is a distributed SQLite system that uses L1 blockchain as a coordination layer. It implements an "Optimistic Witness" model where witnesses verify continuously but only attest at checkpoints.

```
┌─────────────────────────────────────────────────────────────┐
│                         HPPLite                             │
│                                                             │
│   Nodes ◄────────► L1 Contract ◄────────► Nodes            │
│                    - Sequencer election                     │
│                    - Witness registry                       │
│                    - Checkpoint commitments                 │
│                    - Configuration                          │
└─────────────────────────────────────────────────────────────┘
```

---

## Core Insight: L1 as Zookeeper

| Zookeeper          | L1 Contract                              |
|--------------------|------------------------------------------|
| Leader election    | `sequencer`, `claimSequencer()`          |
| Group membership   | `witnesses[]`, `addWitness()`            |
| Distributed lock   | Sequencer role (only one can sequence)   |
| Configuration      | `requiredSigs`, `timeout`, etc.          |
| Consensus          | Ethereum/L1 consensus (already solved)   |

**Benefits:**
- No extra infrastructure (no Zookeeper cluster)
- Battle-tested consensus (L1 finality)
- Transparent (anyone can read contract state)
- Trustless (contract enforces rules)

---

## Roles

### Sequencer

The sequencer is the **single leader** responsible for ordering and producing batches.

**Responsibilities:**
1. Receive SQL transactions from clients
2. Order transactions (determines execution order)
3. Execute SQL against local SQLite
4. Track state root changes
5. Package transactions into batches
6. Broadcast batches to all witnesses
7. At checkpoint interval: collect attestations, submit to L1

**What Sequencer Controls:**
- Transaction ordering (can do MEV)
- Batch timing (when to flush)
- Which transactions to include (can censor temporarily)

**What Sequencer Cannot Do:**
- Produce invalid state transitions (witnesses won't attest)
- Finalize without witness quorum
- Remain sequencer after timeout without producing checkpoints

### Witness

Witnesses form the **verification committee** that validates sequencer honesty.

**Responsibilities:**
1. Register on L1 contract
2. Subscribe to sequencer's batch stream
3. For each batch: verify, re-execute SQL, check state roots
4. At checkpoint request: sign if valid, refuse if invalid
5. Monitor L1 for sequencer timeout
6. Optionally: claim sequencer role if timeout

**Verification Process:**
```
receive_batch(batch):
    assert batch.height == local_height + 1
    assert batch.preStateRoot == local_state_root

    for txn in batch.transactions:
        execute(txn.sql)

    computed_root = get_state_root()
    assert computed_root == batch.postStateRoot

    store_batch(batch)
```

---

## Optimistic Witness Model

### Why Not Traditional Fraud Proofs?

| Approach | EVM L2 | SQL L2 (HPPLite) |
|----------|--------|------------------|
| Instruction trace | Opcodes are atomic | No standard trace |
| Bisection game | Narrow to 1 opcode | Can't bisect SQL |
| On-chain proof | Verify single step | Would need full re-execution |

**Conclusion:** Bisection-based fraud proofs are impractical for SQL execution.

### Pre-emptive vs Reactive Validation

```
Traditional Optimistic Rollup (Reactive):
Sequencer posts ──► Assumed valid ──► [7 day window] ──► Finalized
                                            │
                              Challenge only if fraud detected

HPPLite Optimistic Witness (Pre-emptive):
Sequencer posts ──► Witnesses verify ──► Co-sign ──► Finalized
                          │
            Pre-emptive validation every batch
```

| Aspect | Reactive (Fraud Proof) | Pre-emptive (Witness) |
|--------|------------------------|----------------------|
| Philosophy | "Trust, then verify if suspicious" | "Verify, then trust" |
| Bad state | Gets posted, challenged later | Never finalized |
| Finality | Delayed (challenge window) | Fast (once signed) |

**Key insight:** Invalid state never gets finalized because it won't get enough signatures.

### Why "Optimistic"?
- Witnesses verify continuously in the background
- Only explicit attestations needed at checkpoints
- Between checkpoints: optimistic trust in sequencer
- No per-batch attestation overhead

---

## Checkpoints

### What is a Checkpoint?

A checkpoint is a **commitment to a range of batches** that gets:
1. Attested by witnesses (M-of-N signatures)
2. Submitted to L1 contract
3. Becomes the new finality anchor

```
┌─────────────────────────────────────────────────────────────┐
│                    CHECKPOINT                               │
├─────────────────────────────────────────────────────────────┤
│  fromHeight: 101         (first batch in range)             │
│  toHeight:   200         (last batch in range)              │
│  preStateRoot:  0xabc... (state before batch 101)           │
│  postStateRoot: 0xdef... (state after batch 200)            │
│  timestamp:     16839... (when created)                     │
├─────────────────────────────────────────────────────────────┤
│  ATTESTATIONS:                                              │
│    Witness 1: signature_1                                   │
│    Witness 2: signature_2                                   │
│    Witness 3: signature_3                                   │
└─────────────────────────────────────────────────────────────┘
```

### Checkpoint Flow

```
Sequencer                              Witnesses
    │                                      │
    │  [batch 101-200]                     │
    │─────────────────────────────────────►│ verify, store
    │                                      │
    │  CHECKPOINT_INTERVAL reached         │
    │                                      │
    │  REQUEST_ATTESTATION ───────────────►│
    │◄─────────────── ATTESTATION ─────────│
    │                                      │
    │  quorum reached (e.g., 2 of 3)       │
    │                                      │
    │  SUBMIT TO L1 ──────────────────────►│ L1 Contract
    │◄─────────── CHECKPOINT_FINALIZED ────│
```

### Checkpoint Benefits

| Aspect | Per-Batch | Checkpoint |
|--------|-----------|------------|
| L1 gas cost | 1 tx per batch | 1 tx per N batches |
| Attestation traffic | High | Low |
| Witness availability | Must be online | Can be intermittent |

### What Checkpoints Enable

1. **Finality**: State after checkpoint cannot be reverted
2. **Pruning**: Old batches can be archived after checkpoint
3. **Sync**: New nodes can start from checkpoint, not genesis
4. **Bridges**: Other chains can trust checkpointed state

---

## Finality Levels

```
┌─────────────┬─────────────────────────────────────────────────┐
│ Level       │ Description                                     │
├─────────────┼─────────────────────────────────────────────────┤
│ SEQUENCED   │ Sequencer produced batch, not yet broadcast     │
│ SOFT        │ Witnesses received and verified locally         │
│ CHECKPOINT  │ Included in attested checkpoint (quorum sigs)   │
│ L1          │ Checkpoint committed to L1 contract             │
└─────────────┴─────────────────────────────────────────────────┘
```

**Typical latencies:**
- SEQUENCED → SOFT: ~100ms (network broadcast)
- SOFT → CHECKPOINT: configurable (every N batches)
- CHECKPOINT → L1: ~12s (Ethereum block time) + confirmations

---

## Role Promotion (Witness → Sequencer)

When the current sequencer fails, a witness can **promote** itself.

### Promotion Timeline

```
T+0      Sequencer posts checkpoint (height 100)
T+30min  Sequencer produces batches 101-150
T+45min  Sequencer crashes
T+1hr    TIMEOUT EXPIRES - witnesses detect via L1
T+1hr    Witness calls claimSequencer()
T+1hr+   New sequencer continues from batch 151
```

### Why L1 Timeout is Canonical

1. **Consistency**: All witnesses see identical L1 state
2. **No split-brain**: Cannot have disagreement on sequencer status
3. **Verifiable**: Anyone can check L1
4. **Objective**: No subjective timeout judgments

### Witnesses as Hot Standbys

Witnesses maintain **full state replicas** and can step in immediately:

| Benefit | Description |
|---------|-------------|
| Zero sync time | Witnesses already have current state |
| Instant failover | Any witness can take over immediately |
| High availability | N-1 failures tolerated |
| Decentralization path | Can rotate sequencer among witnesses |

---

## L1 Contract Interface

```solidity
interface IHPPLite {
    // State
    function sequencer() external view returns (address);
    function witnesses(uint256 index) external view returns (address);
    function isWitness(address addr) external view returns (bool);
    function requiredAttestations() external view returns (uint256);
    function lastCheckpointHeight() external view returns (uint256);
    function lastStateRoot() external view returns (bytes32);

    // Actions
    function addWitness(address _witness) external;
    function removeWitness(address _witness) external;
    function setSequencer(address _sequencer) external;

    function submitCheckpoint(
        uint256 fromHeight,
        uint256 toHeight,
        bytes32 stateRoot,
        bytes calldata signatures
    ) external;

    // Events
    event CheckpointSubmitted(uint256 fromHeight, uint256 toHeight, bytes32 stateRoot);
    event SequencerChanged(address indexed oldSequencer, address indexed newSequencer);
}
```

---

## Security Model

### Trust Assumptions

| Component | Trust Level |
|-----------|-------------|
| L1 | Fully trusted (Ethereum consensus) |
| Sequencer | Trusted for liveness, not correctness |
| Witnesses | Threshold trust (M-of-N must be honest) |
| State roots | Verified by witness re-execution |

### Attack Vectors & Mitigations

| Attack | Mitigation |
|--------|------------|
| Sequencer posts bad state | Witnesses won't attest, no finality |
| Sequencer censors txs | Users can submit directly (future) |
| Sequencer goes offline | L1 timeout → new sequencer election |
| Witness collusion | Requires M-of-N threshold to be broken |
| L1 reorg | Wait for L1 finality before trusting |

---

## Comparison with Other Systems

| System | Consensus Model | Finality | Trust |
|--------|-----------------|----------|-------|
| **HPPLite** | Optimistic Witness | Checkpoint | M-of-N witness |
| Optimistic L2 | Fraud proofs | 7 days | 1-of-N honest |
| ZK Rollup | Validity proofs | ~minutes | Math (ZK) |
| Tendermint | BFT | Immediate | 2/3 validators |

**HPPLite's niche**: Simple, practical, SQL-native consensus without ZK complexity or 7-day delays.

---

## Configuration Parameters

| Parameter | Description | Default |
|-----------|-------------|---------|
| `requiredAttestations` | Signatures needed for checkpoint | 2 of 3 |
| `checkpointInterval` | Batches between checkpoints | 100 |
| `sequencerTimeout` | Time before sequencer can be replaced | 1 hour |

---

## Summary

HPPLite implements a practical distributed SQLite system:

1. **L1 as coordinator** - No Zookeeper needed
2. **Optimistic witnesses** - Verify always, attest at checkpoints
3. **Checkpoint finality** - Efficient, batched L1 commits
4. **Simple security** - Sequencer can't cheat, only delay

```
SQLite + L1 Coordination + Optimistic Witnesses = HPPLite
```
