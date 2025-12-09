# HPPLite Architecture

**Singleton Sequencer with L1 Data Availability**

## Overview

HPPLite is an L2/L3 rollup that brings SQLite to the blockchain. It uses a **singleton sequencer** model with full data availability on L1, matching the security model of production L2s like Optimism and Arbitrum.

```
┌─────────────────────────────────────────────────────────────┐
│                         HPPLite                             │
│                                                             │
│   Clients ──────► Sequencer ──────► L1 Contract             │
│                      │               - Batch DA             │
│                      │               - Checkpoints          │
│                      ▼               - State roots          │
│                   SQLite                                    │
│                                                             │
│   Anyone can verify: hpplite verify <contract>              │
└─────────────────────────────────────────────────────────────┘
```

---

## Core Design

### Why Singleton Sequencer?

SQLite requires strong consistency - apps expect read-your-writes semantics:

```c
sqlite3_exec(db, "INSERT INTO users VALUES (1, 'alice')", ...);
sqlite3_exec(db, "SELECT * FROM users WHERE id=1", ...);
// App expects 'alice' immediately
```

Distributed writes break this. Solutions like multi-master (AergoLite) require append-only restrictions. HPPLite keeps full SQL by using a single sequencer.

**This is the same model as major L2s:**
- Optimism: centralized sequencer
- Arbitrum: centralized sequencer
- Base: centralized sequencer
- zkSync: centralized sequencer

### What Makes It Secure?

| Layer | What It Provides |
|-------|------------------|
| **L1 Data Availability** | All batch data on-chain, anyone can reconstruct |
| **Checkpoints** | State roots anchored to L1 |
| **Verifiability** | Anyone can replay and verify |
| **Challenge Period** | Time window to detect fraud |

The sequencer cannot:
- **Corrupt state** - all data is on L1, verifiable
- **Steal funds** - invalid state transitions detectable
- **Hide fraud** - full history on L1

The sequencer can only:
- **Order transactions** (MEV possible)
- **Censor temporarily** (but not permanently with forced inclusion)
- **Go offline** (liveness, not safety)

---

## Security Model: "Trust But Verify"

### The Honest Reality

This is the same "optimistic" security as billion-dollar L2s:

```
Sequencer posts batches to L1
    ↓
Assumed valid (optimistic)
    ↓
Challenge period (anyone can verify)
    ↓
Finalized
```

**If no one verifies during the challenge period, fraud becomes canonical.**

This works because:
1. Sequencer has reputation/bond at stake
2. The *capability* to verify is the deterrent
3. Actual fraud proofs are like nukes - exist to never be used

### Who Watches the Sequencer?

In Optimism/Arbitrum: permissioned validator sets, mostly run by the teams.

In HPPLite: **anyone can verify on-demand**.

```bash
# One-click verification from L1 data
hpplite verify 0x1234...

# Or programmatically
sqlite3_open("file:db.db?hpplite=...&mode=verify")
```

No need for 24/7 witness nodes. The ability to launch verification at any time is the deterrent.

---

## Architecture

### Sequencer Role

The sequencer is the single node that:
1. Receives SQL from clients
2. Executes against local SQLite
3. Batches transactions
4. Submits to L1 (data availability)
5. Creates checkpoints (state commitments)

```c
// Client connects directly to sequencer
sqlite3_open_v2(
    "file:myapp.db?hpplite=on&rpc=https://sepolia.hpp.io"
    "&factory=0x51cD...&privkey=0x...",
    &db, ...);

// All reads and writes go to sequencer
sqlite3_exec(db, "INSERT INTO ...", ...);  // Batched, submitted to L1
sqlite3_exec(db, "SELECT ...", ...);        // Strong consistency
```

### L1 Contract

The L1 contract stores:
- **Batch data** - full DA, enables reconstruction
- **Checkpoints** - state root commitments
- **Configuration** - sequencer address, parameters

```solidity
interface IHPPLite {
    function claimSequencer(bytes32 instanceId) external;
    function renewLease(bytes32 instanceId) external;
    function submitBatch(bytes32 instanceId, uint256 height, bytes data) external;
    function submitCheckpoint(bytes32 instanceId, uint256 from, uint256 to, bytes32 root) external;
    function getBatch(uint256 height) external view returns (bytes);
}
```

### Verification

Anyone can verify the entire chain:

```
1. Fetch all batches from L1
2. Replay SQL in order
3. Compute state roots
4. Compare against checkpointed roots
5. If mismatch → fraud detected
```

This doesn't require running a node 24/7. It's an on-demand audit.

---

## Comparison with Other Systems

| System | Sequencer | Verification | Finality |
|--------|-----------|--------------|----------|
| **HPPLite** | Singleton | On-demand | Checkpoint + challenge |
| **Optimism** | Singleton | Permissioned watchers | 7-day challenge |
| **Arbitrum** | Singleton | Validator whitelist | ~7-day challenge |
| **AergoLite** | None (multi-master) | VRF consensus | Block votes |

HPPLite matches the proven L2 model: centralized sequencer, decentralized verification.

---

## Why Not Witnesses?

We considered active witness nodes that:
- Sync from sequencer in real-time
- Attest to checkpoints
- Serve read traffic

**Problems:**
1. **Write routing** - if witness receives write, must forward to sequencer (complex)
2. **Consistency** - reads from witness may be stale (breaks SQLite semantics)
3. **Infrastructure** - requires 24/7 witness nodes

**Solution:** Skip client-facing witnesses. Verification is on-demand, not continuous.

Future optimization: read replicas (explicit opt-in for stale reads) if needed for scale.

---

## Finality Levels

```
┌─────────────┬────────────────────────────────────────────┐
│ Level       │ Description                                │
├─────────────┼────────────────────────────────────────────┤
│ SEQUENCED   │ SQL executed by sequencer                  │
│ L1          │ Batch submitted to L1 (data available)     │
│ FINALIZED   │ Challenge period passed                    │
└─────────────┴────────────────────────────────────────────┘
```

Typical latencies:
- SEQUENCED: immediate
- L1: seconds to minutes (batch interval)
- FINALIZED: challenge period (configurable, e.g., hours to days)

---

## Configuration

| Parameter | Description | Default |
|-----------|-------------|---------|
| `batchInterval` | Time between batch submissions | 60s |
| `checkpointInterval` | Batches between checkpoints | 100 |
| `challengePeriod` | Time window for fraud detection | 1 hour |

---

## Summary

HPPLite implements a practical L2/L3 for SQLite:

1. **Singleton sequencer** - strong consistency, full SQL support
2. **L1 data availability** - all data on-chain, reconstructable
3. **On-demand verification** - anyone can verify, no 24/7 nodes needed
4. **Same security as major L2s** - if it's good enough for Optimism, it's good enough

```
"Centralized sequencer, decentralized verification"
```

Or more honestly:

```
"Your data on-chain, one server, verify anytime"
```
