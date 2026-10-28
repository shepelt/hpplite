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
    "file:myapp.db?hpplite=on&l1=hpp-sepolia&privkey=0x...",
    &db, ...);

// All reads and writes go to sequencer
sqlite3_exec(db, "INSERT INTO ...", ...);  // Batched, submitted to L1
sqlite3_exec(db, "SELECT ...", ...);        // Strong consistency
```

### Local Durability

The local SQLite file is the only durable state HPPLite keeps. Nothing
lives only in memory, so a crash at any point loses nothing and a restart
needs no special recovery:

| Table | Holds | Written |
|-------|-------|---------|
| `_hpplite_pending` | SQL of every committed write, in order, grouped by transaction | From inside the user's own statement, so the row commits or rolls back **with** the user's transaction |
| `_hpplite_meta` | Next batch height, previous block hash, state root after the last confirmed batch, checkpoint progress | When a batch is confirmed on L1 |

How a statement gets logged: SQLite's statement trace fires at the start of
every statement on the connection. HPPLite inserts the statement text (with
bound parameters expanded) into `_hpplite_pending` through the same
connection. Because the insert runs inside the statement's own transaction
(implicit or explicit), a `ROLLBACK` discards the log row too, and in
autocommit so does a failed statement. No separate journal, no sync
interval, no durability model beyond SQLite's own.

Inside an explicit transaction a failed statement only undoes its own
changes, so when a logged statement completes HPPLite checks whether it had
any effect (rows changed or schema changed) and deletes the log row if not:
failed statements and no-ops are never replayed. SQLite reports a
statement's error only after the trace sees it finish, so effect is the
signal; its one blind spot is a statement whose triggers changed rows
before it aborted. Replay therefore skips any statement that fails
(`hpplite_status.replay_skipped`), which reproduces what happened at the
sequencer, and the checkpoint root remains the arbiter of equivalence.

Flushing a batch:

```
1. If L1 already holds a batch at our next height, it was produced from our
   pending rows by an earlier attempt (crash after submit, lost RPC reply):
   adopt it - delete those rows, advance meta.
2. Read pending rows in one snapshot; if this batch closes a checkpoint
   interval, compute the state root in the same snapshot.
3. Submit to L1 and wait for the transaction.
4. Confirmed  -> delete the rows, advance height / prev hash (and the root,
   when the batch carries one) in one transaction.
   Not confirmed -> leave everything; retry next interval at the same height.
5. If an interval of batches is now complete, submit the checkpoint
   (previous root -> this root) to the coordinator contract.
```

The DA contract caps a batch at `batchSizeLimit` (128 KB by default, read
from the contract's system config on connect). Pending SQL beyond that is
submitted as a series of batches, each cut at a transaction boundary when
possible and marked `"partial": 1` except the last. A partial batch carries
the state root of the end of its series, so verification compares roots at
non-partial batches only. The timer flushes early once pending SQL reaches
half the limit, so series are the exception. A single statement that cannot
fit on its own is left pending and reported (`hpplite_status.log_errors`).

Bookkeeping (steps 1, 2, 4) runs on a second connection to the same file,
so the background flush never touches the application's transaction state.
File databases are switched to WAL and both connections get a 5 s busy
timeout; readers and the writer then never block each other.

Reopening a file continues at the recorded height. If L1 is ahead (a fresh
file, or history written elsewhere), the missing batches are replayed with
tracking disabled and are not logged again. If local pending SQL and the
batch on L1 disagree, the node refuses to continue rather than fork.

Close is handled without touching SQLite core: `hpplite_status` is an
eponymous virtual table whose instance lives for the connection; SQLite
disconnects it at the start of `sqlite3_close()` while the connection is
still usable, and that `xDisconnect` flushes, releases the lease and frees
the node. The same code builds as a loadable extension against stock
SQLite.

On graceful close the sequencer lease is released (`forceExpireLease`, the
wallet owns its rollup). After a crash the old lease blocks submission until
it expires (5 minutes); writes keep accumulating in `_hpplite_pending` and
flush once the lease is free.

### Privacy: encrypted batches

L1 is public, so a batch is sealed before it is posted whenever the node has
a private key:

```
"HPE1" | scheme id | nonce (12) | ChaCha20-Poly1305(key, nonce, ad = scheme || height, batch JSON) || tag (16)
key = HKDF-SHA256(ikm = wallet private key, salt = "hpplite", info = "hpplite/batch-encryption/v1")
```

- The key is derived, not stored: whoever can write the rollup can read it,
  and reconstruction/verification need only the same `privkey=`.
- The height is associated data, so a ciphertext cannot be presented at
  another height. Each batch gets a fresh random nonce.
- Payloads that do not start with `HPE1` are plaintext JSON (history written
  before encryption, or `encrypt=off`); a node reads both, writes sealed.
- The scheme is pluggable (`HppliteCipher` in `cipher.h`); the envelope
  names it, so new schemes can be added without touching batch or replay
  code. The v0.1 scheme is vendored ChaCha20-Poly1305 (RFC 8439): ~400
  lines of constant-time plain C, no platform crypto dependency, which
  matters for the loadable extension.
- The DA size limit applies to the posted bytes, so the batch cut leaves
  room for the 33-byte envelope.

What remains visible on L1: heights, sizes, timing, batch hashes, and the
checkpoint roots (content hashes: they show that content changed, not what).
Consequence for verification: only key holders can replay. Per-reader
access without the wallet key (encrypt to public keys, grant/revoke) is the
v0.2 public-key scheme.

### State Root

The state root is a **content hash** of all user tables: SHA-256 over the
schema entries (type, name, table, SQL) and every row of every table in
rowid order, with typed values. `sqlite_*` and `_hpplite_*` tables are
excluded; a database with no user schema has the zero root.

It is a function of the file, not of the history that produced it. Two
databases with the same content have the same root whatever statements
built them, and anyone holding a reconstructed database can recompute it
(`hpplite_db_hash()`) and compare with what the sequencer posted.

Computing it is O(database size) (~9 ms/MB in a Release build), so it is computed at
**checkpoint cadence**, not per batch: only the batch that closes a
checkpoint interval (`checkpointInterval`, default 100) carries
`"state_root"`, and the checkpoint submitted right after it anchors that
root on L1. Other batches are data only. This is the same split every L2
makes - data is posted often, state is committed rarely - and it does not
touch durability (every COMMIT is durable locally, every batch is on L1);
it only sets how finely a verifier can localize a divergence. Deployments
that want finer auditing lower `checkpointInterval` and pay proportionally.

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
2. Replay SQL in order (each batch's transaction groups as transactions)
3. Compute the content hash after each batch
4. Compare against the batch's posted root and checkpointed roots
5. If mismatch → fraud detected
```

Replay already does step 3-4 and warns on mismatch; `hpplite verify`
packages it as a command.

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
