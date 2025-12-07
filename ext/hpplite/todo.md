# HPPLite Development Progress

## Milestones

### Milestone 1: Shared-Memory Testing ← CURRENT
Single process, nodes share L1 mock pointer
```
┌─────────────────────────────────────┐
│           Single Process            │
│  ┌───────┐ ┌───────┐ ┌───────┐     │
│  │  Seq  │ │ Wit1  │ │ Wit2  │     │
│  └───┬───┘ └───┬───┘ └───┬───┘     │
│      │         │         │          │
│      └─────────┼─────────┘          │
│                ▼                    │
│         [L1 Mock Ptr]               │
│        (shared memory)              │
└─────────────────────────────────────┘
```

### Milestone 2: ZeroMQ Multi-Process
Separate processes, ZeroMQ communication, L1 mock as dedicated process
```
┌─────────┐   ┌─────────┐   ┌─────────┐
│   Seq   │   │  Wit1   │   │  Wit2   │
│ Process │   │ Process │   │ Process │
└────┬────┘   └────┬────┘   └────┬────┘
     │   ZeroMQ    │             │
     │◄────────────┼─────────────┤  (batch/attestation)
     │             │             │
     └─────────────┼─────────────┘
                   │ ZeroMQ
                   ▼
          ┌───────────────┐
          │  L1 Mock Svc  │
          │   (process)   │
          └───────────────┘
```

### Milestone 3: Web3 Integration
Real Ethereum L1, production deployment
```
┌─────────┐   ┌─────────┐   ┌─────────┐
│   Seq   │   │  Wit1   │   │  Wit2   │
└────┬────┘   └────┬────┘   └────┬────┘
     │             │             │
     └─────────────┼─────────────┘
                   │ JSON-RPC
                   ▼
          ┌───────────────┐
          │   Ethereum    │
          │   (L1 chain)  │
          └───────────────┘
```

---

## Milestone 1 Progress

### Completed ✓
- [x] CMake build system
- [x] Core state tracking (`hpplite.c`, `hpplite.h`)
- [x] Batch serialization (`batch.c`, `batch.h`)
- [x] File storage (`fs_storage.c`, `fs_storage.h`)
- [x] Crypto signing with secp256k1 (`crypto.c`, `crypto.h`)
- [x] Node module (`node.c`, `node.h`) - sequencer/witness unified logic
- [x] Checkpoint structures (`HppliteCheckpoint`, `HppliteCheckpointAttestation`)
- [x] L1 interface header (`l1_interface.h`)
- [x] Rollup model documentation (`hpplite_rollup.md`)
- [x] Tests: test_hpplite, test_batch, test_sequencer, test_integrated, test_node
- [x] L1 mock implementation (`l1_mock.c`) - in-memory, shared pointer
- [x] Update node.c for checkpoint-based attestations
- [x] Integrate L1 mock into node (`hpplite_node_set_l1`, `hpplite_node_sync_role_from_l1`)
- [x] Checkpoint operations in node (`hpplite_node_create_checkpoint`, `hpplite_node_submit_checkpoint`)
- [x] Full M1 integration test (`test_m1_integration.c`)

### M1 Test Status: 7/7 passing
- test_hpplite
- test_batch
- test_sequencer
- test_integrated
- test_node
- test_l1_mock
- test_m1_integration

---

## Milestone 2 Tasks
- [ ] ZeroMQ integration for node-to-node (PUB/SUB, DEALER/ROUTER)
- [ ] L1 mock as separate process with ZeroMQ interface
- [ ] Multi-process test harness
- [ ] Sequencer failure / witness promotion test

## Milestone 3 Tasks
- [ ] Web3/Ethereum RPC client
- [ ] Real L1 contract deployment
- [ ] Production configuration
- [ ] Sync protocol for new nodes

---

## File Structure

```
ext/hpplite/
├── hpplite.h/c        # Core state tracking (preupdate hooks)
├── batch.h/c          # Batch & checkpoint serialization
├── crypto.h/c         # secp256k1 signing (Ethereum-compatible)
├── fs_storage.h/c     # File-based storage (mock IPFS/L1)
├── node.h/c           # Unified sequencer/witness node
├── l1_interface.h     # L1 contract interface (API)
├── l1_interface.c     # L1 mock implementation (TODO)
├── hpplite_rollup.md  # Rollup model documentation
├── todo.md            # This file
└── tests/
    ├── test_hpplite.c
    ├── test_batch.c
    ├── test_sequencer.c
    ├── test_integrated.c
    └── test_node.c
```

## Key Concepts

See `hpplite_rollup.md` for full documentation.

| Concept | Description |
|---------|-------------|
| **L1 as Zookeeper** | L1 contract handles election, registry, coordination |
| **Sequencer** | Single leader, orders txns, produces batches |
| **Witness** | Verification committee, re-executes SQL, attests |
| **Checkpoint** | Batched finality (N batches → 1 L1 commit) |
| **Promotion** | Witness → Sequencer after L1 timeout |
| **Optimistic Witness** | Verify always, attest at checkpoints only |

## Build & Test

```bash
cd ext/hpplite/build
cmake ..
make -j4
ctest --output-on-failure
```

**Dependencies:** CMake, pkg-config, libsecp256k1

**Current test status:** 7/7 passing (M1 complete)
