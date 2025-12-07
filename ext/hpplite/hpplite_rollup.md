# HPPLite Rollup Model

**Optimistic Witness Consensus for SQLite**

## Overview

HPPLite is a distributed SQLite system that uses L1 blockchain as a coordination layer, replacing traditional systems like Zookeeper. It implements an "Optimistic Witness" model where witnesses verify continuously but only attest at checkpoints.

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

## Core Insight: L1 as Zookeeper

| Zookeeper          | L1 Contract                              |
|--------------------|------------------------------------------|
| Leader election    | `sequencer`, `claimSequencer()`          |
| Group membership   | `witnesses[]`, `registerWitness()`       |
| Distributed lock   | Sequencer role (only one can sequence)   |
| Configuration      | `requiredSigs`, `timeout`, etc.          |
| Consensus          | Ethereum/L1 consensus (already solved)   |

**Benefits:**
- No extra infrastructure (no Zookeeper cluster)
- Battle-tested consensus (L1 finality)
- Transparent (anyone can read contract state)
- Trustless (contract enforces rules)
- Already needed (we post commitments anyway)

---

## Roles

### Sequencer

The sequencer is the **single leader** responsible for ordering and producing batches.

**Responsibilities:**
```
┌─────────────────────────────────────────────────────────┐
│                     SEQUENCER                           │
├─────────────────────────────────────────────────────────┤
│  1. Receive SQL transactions from clients               │
│  2. Order transactions (determines execution order)     │
│  3. Execute SQL against local SQLite                    │
│  4. Track state root changes                            │
│  5. Package transactions into batches                   │
│  6. Broadcast batches to all witnesses                  │
│  7. At checkpoint interval:                             │
│     - Request attestations from witnesses               │
│     - Collect signatures until quorum                   │
│     - Submit checkpoint to L1 contract                  │
└─────────────────────────────────────────────────────────┘
```

**State Machine:**
```
              ┌─────────────────┐
              │      INIT       │
              └────────┬────────┘
                       │ claimSequencer() succeeds
                       ▼
              ┌─────────────────┐
         ┌───►│    RUNNING      │◄───┐
         │    └────────┬────────┘    │
         │             │             │
   new   │    timeout/ │  checkpoint │ checkpoint
  client │    failure  │  interval   │ submitted
   txn   │             ▼             │
         │    ┌─────────────────┐    │
         │    │  CHECKPOINTING  │────┘
         │    └─────────────────┘
         │
         └─────────────────────────────
```

**What Sequencer Controls:**
- Transaction ordering (can do MEV)
- Batch timing (when to flush)
- Which transactions to include (can censor temporarily)

**What Sequencer Cannot Do:**
- Produce invalid state transitions (witnesses won't attest)
- Finalize without witness quorum
- Remain sequencer after timeout without producing checkpoints

---

### Witness

Witnesses form the **verification committee** that validates sequencer honesty.

**Responsibilities:**
```
┌─────────────────────────────────────────────────────────┐
│                      WITNESS                            │
├─────────────────────────────────────────────────────────┤
│  1. Register on L1 contract                             │
│  2. Subscribe to sequencer's batch stream               │
│  3. For each batch received:                            │
│     - Verify batch height is sequential                 │
│     - Verify pre-state matches local state              │
│     - Re-execute all SQL transactions                   │
│     - Verify post-state matches batch claim             │
│     - Store batch locally                               │
│  4. At checkpoint request:                              │
│     - Verify checkpoint covers correct range            │
│     - Sign checkpoint hash                              │
│     - Return attestation to sequencer                   │
│  5. Monitor L1 for sequencer timeout                    │
│  6. Optionally: claim sequencer role if timeout         │
└─────────────────────────────────────────────────────────┘
```

**State Machine:**
```
              ┌─────────────────┐
              │      INIT       │
              └────────┬────────┘
                       │ registerWitness()
                       ▼
              ┌─────────────────┐
              │   REGISTERED    │
              └────────┬────────┘
                       │ connect to sequencer
                       ▼
              ┌─────────────────┐
         ┌───►│   FOLLOWING     │◄───┐
         │    └────────┬────────┘    │
         │             │             │
  batch  │   sequencer │  attest     │ attestation
received │   timeout   │  request    │ sent
         │             ▼             │
         │    ┌─────────────────┐    │
         │    │   ATTESTING     │────┘
         │    └────────┬────────┘
         │             │
         │             │ timeout expired
         │             ▼
         │    ┌─────────────────┐
         │    │  PROMOTE_READY  │───► claimSequencer()
         │    └─────────────────┘
         │
         └─────────────────────────────
```

**Witness Verification Process:**
```
receive_batch(batch):
    # 1. Check sequence
    assert batch.height == local_height + 1

    # 2. Check pre-state
    assert batch.preStateRoot == local_state_root

    # 3. Re-execute all transactions
    for txn in batch.transactions:
        execute(txn.sql)

    # 4. Check post-state
    computed_root = get_state_root()
    assert computed_root == batch.postStateRoot

    # 5. Update local state
    local_height = batch.height
    local_state_root = computed_root
    store_batch(batch)
```

---

## Role Promotion (Witness → Sequencer)

When the current sequencer fails, a witness can **promote** itself to become the new sequencer.

### Promotion Trigger: L1 Timeout

```
┌─────────────────────────────────────────────────────────┐
│                  PROMOTION TIMELINE                     │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  T+0      Sequencer posts checkpoint (height 100)       │
│           lastCheckpointTime = T                        │
│                                                         │
│  T+30min  Sequencer produces batches 101-150            │
│                                                         │
│  T+45min  Sequencer crashes                             │
│           (no more batches, no checkpoint)              │
│                                                         │
│  T+1hr    TIMEOUT EXPIRES                               │
│           Witnesses detect: block.timestamp > T + 1hr   │
│           No checkpoint submitted for batches 101+      │
│                                                         │
│  T+1hr    Witness calls claimSequencer()                │
│           L1 contract verifies timeout                  │
│           Witness becomes new sequencer                 │
│                                                         │
│  T+1hr+   New sequencer:                                │
│           - May need to sync to batch 150               │
│           - Continues from where old sequencer left     │
│           - Produces checkpoint for 101-150+            │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

### Promotion Process

```solidity
// L1 Contract
function claimSequencer() external {
    // Must be registered witness
    require(isWitness[msg.sender], "Not a witness");

    // Timeout must have expired
    require(
        block.timestamp > lastCheckpointTime + sequencerTimeout,
        "Sequencer still active"
    );

    // Promote witness to sequencer
    address oldSequencer = sequencer;
    sequencer = msg.sender;
    sequencerSince = block.number;

    emit SequencerChanged(oldSequencer, msg.sender);
}
```

### Promotion Strategies

| Strategy          | How It Works                                    | Pros/Cons                    |
|-------------------|-------------------------------------------------|------------------------------|
| **Race**          | First witness to call wins                      | Simple, may cause gas wars   |
| **Round-robin**   | Next witness in list gets priority              | Fair, predictable            |
| **Stake-weighted**| Highest staked witness has priority             | Economic security            |
| **Random**        | VRF selects winner                              | Fair, needs randomness       |

### Post-Promotion Sync

New sequencer may be behind if old sequencer produced batches that weren't checkpointed:

```
Old sequencer state:  [checkpoint@100] → 101 → 102 → ... → 150 (crash)
Witnesses have:       [checkpoint@100] → 101 → 102 → ... → 150
New sequencer:        [checkpoint@100] → ??? (needs sync)

Sync process:
1. Query witnesses for batches 101-150
2. Verify and apply each batch
3. Resume sequencing from 151
4. Produce checkpoint covering 101-150+
```

---

## Checkpoint Deep Dive

### What is a Checkpoint?

A checkpoint is a **commitment to a range of batches** that gets:
1. Attested by witnesses (M-of-N signatures)
2. Submitted to L1 contract
3. Becomes the new finality anchor

```
┌─────────────────────────────────────────────────────────┐
│                    CHECKPOINT                           │
├─────────────────────────────────────────────────────────┤
│  fromHeight: 101         (first batch in range)         │
│  toHeight:   200         (last batch in range)          │
│  preStateRoot:  0xabc... (state before batch 101)       │
│  postStateRoot: 0xdef... (state after batch 200)        │
│  batchesHash:   0x123... (merkle root of batch refs)    │
│  timestamp:     16839... (when created)                 │
├─────────────────────────────────────────────────────────┤
│  ATTESTATIONS:                                          │
│    Witness 1: signature_1 (signs hash of above)         │
│    Witness 2: signature_2                               │
│    Witness 3: signature_3                               │
└─────────────────────────────────────────────────────────┘
```

### Checkpoint Creation Flow

```
Sequencer                              Witnesses
    │                                      │
    │  [batch 101]                         │
    │─────────────────────────────────────►│ verify, store
    │  [batch 102]                         │
    │─────────────────────────────────────►│ verify, store
    │  ...                                 │
    │  [batch 200]                         │
    │─────────────────────────────────────►│ verify, store
    │                                      │
    │  CHECKPOINT_INTERVAL reached         │
    │                                      │
    │  create checkpoint {101-200}         │
    │                                      │
    │  REQUEST_ATTESTATION ───────────────►│
    │                                      │ verify checkpoint
    │                                      │ sign if valid
    │◄─────────────── ATTESTATION ─────────│
    │◄─────────────── ATTESTATION ─────────│
    │◄─────────────── ATTESTATION ─────────│
    │                                      │
    │  quorum reached (e.g., 2 of 3)       │
    │                                      │
    │  SUBMIT TO L1 ──────────────────────►│ L1 Contract
    │                                      │
    │◄─────────── CHECKPOINT_FINALIZED ────│
    │                                      │
```

### Checkpoint Verification (Witness Side)

```python
def verify_checkpoint(checkpoint, local_state):
    # 1. Range must be sequential from last checkpoint
    assert checkpoint.fromHeight == last_checkpoint.toHeight + 1

    # 2. We must have all batches in range
    for height in range(checkpoint.fromHeight, checkpoint.toHeight + 1):
        assert have_batch(height)

    # 3. Pre-state must match our state before this range
    assert checkpoint.preStateRoot == state_root_at(checkpoint.fromHeight - 1)

    # 4. Post-state must match our state after this range
    assert checkpoint.postStateRoot == state_root_at(checkpoint.toHeight)

    # 5. Batches hash must match
    batch_refs = [get_batch_ref(h) for h in range(checkpoint.fromHeight, checkpoint.toHeight + 1)]
    assert checkpoint.batchesHash == merkle_root(batch_refs)

    # All good - sign it
    return sign(checkpoint_hash(checkpoint))
```

### Checkpoint Interval Strategies

| Interval    | Batches/Checkpoint | L1 Cost      | Finality Delay    |
|-------------|-------------------|--------------|-------------------|
| Aggressive  | 10                | High         | ~10 batches       |
| Balanced    | 100               | Medium       | ~100 batches      |
| Economic    | 1000              | Low          | ~1000 batches     |

**Dynamic adjustment:**
- Increase interval when L1 gas is high
- Decrease interval when fast finality needed
- Could be governance-controlled parameter

### What Checkpoints Enable

1. **Finality**: State after checkpoint cannot be reverted
2. **Pruning**: Old batches can be archived after checkpoint
3. **Sync**: New nodes can start from checkpoint, not genesis
4. **Bridges**: Other chains can trust checkpointed state
5. **Withdrawals**: Users can prove state for L1 withdrawals

---

## Optimistic Witness Model

Unlike fraud-proof systems where provers challenge invalid state, HPPLite uses an **attestation model**:

```
Fraud Proof Model:          Optimistic Witness Model:
─────────────────           ────────────────────────
Sequencer posts state  →    Sequencer posts batch
Challenger finds fraud →    Witnesses verify
Challenger submits proof    Witnesses attest to checkpoint
Sequencer slashed           Finality achieved
```

### Why "Optimistic"?
- Witnesses verify continuously in the background
- Only explicit attestations needed at checkpoints
- Between checkpoints: optimistic trust in sequencer
- No per-batch attestation overhead

### Why No Censorship Concern?
In fraud-proof systems, sequencers have incentive to censor challengers. In attestation systems:
- Sequencer **needs** attestations for finality
- Censoring witnesses = no finality = self-harm
- Incentives are aligned, not adversarial

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

## Checkpoint-Based Finality

Instead of attesting every batch, witnesses attest to **checkpoints** covering ranges of batches:

```
Batch 1 ─► Batch 2 ─► Batch 3 ─► Batch 4 ─► Batch 5
                                              │
                                        CHECKPOINT
                                     (attestations)
                                              │
                                      L1 Commitment
                              "state root after batch 5"
```

### Checkpoint Structure

```c
struct HppliteCheckpoint {
    uint64_t fromHeight;           // First batch (e.g., 1)
    uint64_t toHeight;             // Last batch (e.g., 5)
    bytes32 preStateRoot;          // State before batch 1
    bytes32 postStateRoot;         // State after batch 5
    bytes32 batchesHash;           // Merkle root of batch refs
    uint64_t timestamp;
};
```

### Checkpoint Attestation

```c
struct HppliteCheckpointAttestation {
    uint64_t fromHeight;
    uint64_t toHeight;
    bytes32 postStateRoot;
    bytes33 witnessPubkey;         // Compressed secp256k1
    bytes64 signature;             // ECDSA signature
    uint8_t recid;                 // Recovery ID for ecrecover
};
```

### Benefits of Checkpoints

| Aspect              | Per-Batch         | Checkpoint        |
|---------------------|-------------------|-------------------|
| L1 gas cost         | 1 tx per batch    | 1 tx per N batches|
| Attestation traffic | High              | Low               |
| Witness availability| Must be online    | Can be intermittent|
| Soft finality       | ~100ms            | ~100ms            |
| Hard finality       | ~1 batch          | ~N batches        |

---

## Sequencer Election

Handled entirely by L1 contract:

```solidity
contract HPPLite {
    address public sequencer;
    uint256 public sequencerSince;
    uint256 public constant SEQUENCER_TIMEOUT = 1 hours;

    // Claim sequencer role (if none or timeout expired)
    function claimSequencer() external onlyWitness {
        require(
            sequencer == address(0) ||
            block.timestamp > lastCheckpointTime + SEQUENCER_TIMEOUT,
            "Sequencer still active"
        );
        sequencer = msg.sender;
        sequencerSince = block.number;
        emit SequencerChanged(msg.sender);
    }
}
```

### Election Strategies

| Strategy        | Description                                      |
|-----------------|--------------------------------------------------|
| First-claim     | First witness to call `claimSequencer()` wins    |
| Round-robin     | Next witness in list becomes sequencer           |
| Stake-weighted  | Highest staked witness gets priority             |
| Governance      | Multisig/DAO appoints sequencer                  |

---

## Sequencer Failure Detection

Witnesses detect sequencer failure via **L1 timeout** (canonical signal):

```
Witness monitors L1:
  "Last checkpoint at block B.
   If current block > B + TIMEOUT and no new checkpoint,
   sequencer has failed."
```

### Why L1 is Canonical

1. **Consistency**: All witnesses see identical L1 state
2. **No split-brain**: Cannot have disagreement on sequencer status
3. **Verifiable**: Anyone can check L1
4. **Objective**: No subjective timeout judgments

### Failure Timeline

```
T+0:      Sequencer posts checkpoint for batch 100
T+10min:  Sequencer produces batches 101-110
T+20min:  Sequencer crashes
T+30min:  Witnesses notice no new batches (local timeout - warning)
T+1hr:    L1 timeout expires, no checkpoint 101+
T+1hr:    Any witness can call claimSequencer()
T+1hr+:   New sequencer takes over from batch 101
```

---

## Data Flow

### Normal Operation

```
┌──────────────┐     SQL      ┌──────────────┐
│   Client     │─────────────►│  Sequencer   │
└──────────────┘              └──────┬───────┘
                                     │
                              ┌──────▼───────┐
                              │ Execute SQL  │
                              │ Track state  │
                              │ Create batch │
                              └──────┬───────┘
                                     │ broadcast
         ┌───────────────────────────┼───────────────────────────┐
         │                           │                           │
         ▼                           ▼                           ▼
┌──────────────┐            ┌──────────────┐            ┌──────────────┐
│  Witness 1   │            │  Witness 2   │            │  Witness 3   │
│  - verify    │            │  - verify    │            │  - verify    │
│  - track     │            │  - track     │            │  - track     │
└──────────────┘            └──────────────┘            └──────────────┘
```

### Checkpoint Flow

```
┌──────────────┐
│  Sequencer   │  "Checkpoint interval reached"
└──────┬───────┘
       │ request attestations
       ▼
┌──────────────────────────────────────────┐
│              Witnesses                    │
│  - Sign checkpoint hash                   │
│  - Return attestations                    │
└──────────────────────────────────────────┘
       │
       ▼
┌──────────────┐
│  Sequencer   │  "Quorum reached"
│  - Aggregate │
│  - Submit    │
└──────┬───────┘
       │
       ▼
┌──────────────┐
│ L1 Contract  │  "Checkpoint committed"
│  - Verify    │
│  - Store     │
└──────────────┘
```

---

## L1 Contract Interface

```solidity
interface IHPPLite {
    // === State ===
    function sequencer() external view returns (address);
    function witnesses(uint256 index) external view returns (address);
    function witnessCount() external view returns (uint256);
    function isWitness(address addr) external view returns (bool);

    function requiredAttestations() external view returns (uint256);
    function checkpointInterval() external view returns (uint256);
    function sequencerTimeout() external view returns (uint256);

    function lastCheckpointHeight() external view returns (uint256);
    function lastCheckpointStateRoot() external view returns (bytes32);
    function lastCheckpointTime() external view returns (uint256);

    // === Actions ===
    function registerWitness() external;
    function unregisterWitness() external;

    function claimSequencer() external;

    function submitCheckpoint(
        uint64 fromHeight,
        uint64 toHeight,
        bytes32 preStateRoot,
        bytes32 postStateRoot,
        bytes32 batchesHash,
        bytes[] calldata signatures
    ) external;

    // === Events ===
    event WitnessRegistered(address indexed witness);
    event WitnessUnregistered(address indexed witness);
    event SequencerChanged(address indexed newSequencer);
    event CheckpointSubmitted(
        uint64 indexed fromHeight,
        uint64 indexed toHeight,
        bytes32 stateRoot
    );
}
```

---

## Security Model

### Trust Assumptions

| Component   | Trust Level                                        |
|-------------|---------------------------------------------------|
| L1          | Fully trusted (Ethereum consensus)                |
| Sequencer   | Trusted for liveness, not for correctness         |
| Witnesses   | Threshold trust (M-of-N must be honest)           |
| State roots | Verified by witness re-execution                  |

### Attack Vectors & Mitigations

| Attack                    | Mitigation                                |
|---------------------------|-------------------------------------------|
| Sequencer posts bad state | Witnesses won't attest, no finality       |
| Sequencer censors txs     | Users can submit directly (future)        |
| Sequencer goes offline    | L1 timeout → new sequencer election       |
| Witness collusion         | Requires M-of-N threshold to be broken    |
| L1 reorg                  | Wait for L1 finality before trusting      |

### What Sequencer CAN Do
- Order transactions (MEV potential)
- Delay transactions
- Temporarily censor (until timeout)

### What Sequencer CANNOT Do
- Produce invalid state transitions
- Steal funds
- Finalize without witness attestations

---

## Configuration Parameters

| Parameter              | Description                        | Default     |
|------------------------|------------------------------------|-------------|
| `requiredAttestations` | Signatures needed for checkpoint   | 2 of 3      |
| `checkpointInterval`   | Batches between checkpoints        | 100         |
| `sequencerTimeout`     | Blocks before sequencer failure    | 1 hour      |
| `batchTimeout`         | Max time to wait for batch         | 1000ms      |

---

## Comparison with Other Systems

| System          | Consensus Model           | Finality      | Trust          |
|-----------------|---------------------------|---------------|----------------|
| **HPPLite**     | Optimistic Witness        | Checkpoint    | M-of-N witness |
| Optimistic L2   | Fraud proofs              | 7 days        | 1-of-N honest  |
| ZK Rollup       | Validity proofs           | ~minutes      | Math (ZK)      |
| Tendermint      | BFT                       | Immediate     | 2/3 validators |
| Raft            | Leader-based              | Immediate     | Majority       |

**HPPLite's niche**: Simple, practical, SQL-native consensus without ZK complexity or 7-day delays.

---

## Future Extensions

### Planned
- [ ] ZeroMQ networking for real multi-process
- [ ] Sync protocol for new node catch-up
- [ ] Direct client submission (bypass sequencer)

### Possible
- [ ] Decentralized sequencer (rotating)
- [ ] Economic incentives (staking, fees)
- [ ] Cross-chain checkpoints
- [ ] ZK state proofs (optional upgrade)

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
