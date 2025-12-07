# HPPLite Development Progress

## Milestones

### Milestone 1: Shared-Memory Testing ✓
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

### Milestone 2: ZeroMQ Multi-Process ✓
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

## Milestone 2 Progress

### Completed ✓
- [x] ZeroMQ transport layer (`zmq_transport.c`, `zmq_transport.h`)
- [x] ZeroMQ integration for node-to-node (PUB/SUB, DEALER/ROUTER)
- [x] L1 mock service (`l1_service.c`, `l1_service.h`) - REQ/REP protocol
- [x] L1 client library for multi-process nodes

- [x] Multi-process test harness (`test_m2_multiprocess.c`)

### Remaining
- [x] Sequencer failure / witness promotion test
- [x] Multi-process node test with ZMQ node-to-node communication (`test_m2_nodes.c`)
- [x] L1-anchored replay test (fetch checkpoint from L1, replay batches, verify against L1 commitment)

**M2 Complete** - Full multi-process architecture:
- L1 service accessible via ZMQ REQ/REP
- Sequencer broadcasts batches via ZMQ PUB, receives attestations via ROUTER
- Witnesses receive batches via SUB, send attestations via DEALER
- Checkpoint attestation flow works end-to-end via ZMQ
- L1-anchored state verification (replay validates against L1 checkpoint)
- Benchmark: `make speedtest` (SQLite-style, separate from tests)

### M2 Robustness Notes

**Solid:**
- Cryptographic auth via signatures (batches signed by sequencer, attestations signed by witnesses)
- Signature verification against L1 pubkey registry
- Deterministic replay from genesis
- L1-anchored state verification
- Checkpoint-based finality

**Not implemented (future hardening):**
- ZMQ reconnection on disconnect
- Heartbeat/liveness detection (relies on L1 timeout)
- Out-of-order batch handling (currently rejects, needs sync)
- Adversarial/Byzantine testing

## Milestone 3 Tasks ← CURRENT
- [ ] Web3/Ethereum RPC client
- [ ] Real L1 contract deployment
- [ ] Production configuration
- [ ] Sync protocol for new nodes

---

## File Structure

```
ext/hpplite/
├── hpplite.h/c          # Core state tracking (preupdate hooks)
├── batch.h/c            # Batch & checkpoint serialization
├── crypto.h/c           # secp256k1 signing (Ethereum-compatible)
├── fs_storage.h/c       # File-based storage
├── node.h/c             # Unified sequencer/witness node
├── l1_interface.h       # L1 contract interface (API)
├── l1_mock.c            # L1 mock implementation (M1 shared memory)
├── l1_service.h/c       # L1 mock ZMQ service (M2 multi-process)
├── zmq_transport.h/c    # ZeroMQ transport layer (M2)
├── hpplite_rollup.md    # Rollup model documentation
├── todo.md              # This file
├── test_*.c             # Tests (run via ctest)
│   ├── test_hpplite.c
│   ├── test_batch.c
│   ├── test_sequencer.c
│   ├── test_integrated.c
│   ├── test_node.c
│   ├── test_l1_mock.c
│   ├── test_m1_integration.c
│   ├── test_m2_multiprocess.c
│   └── test_m2_nodes.c
└── bench_*.c            # Benchmarks (run via make speedtest)
    └── bench_replay.c
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
ctest --output-on-failure   # Run tests (9/9 passing)
make speedtest              # Run benchmark (optional)
```

**Dependencies:** CMake, pkg-config, libsecp256k1, libzmq (required for M2+)

**Test status:** 9/9 passing | **Benchmark:** ~1500 batches/sec
