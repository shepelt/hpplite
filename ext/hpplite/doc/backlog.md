# HPPLite Backlog

## Completed

### M1: Single-Process State Tracking
- Core state tracking (`hpplite.c`)
- Batch serialization (`batch.c`)
- File storage (`fs_storage.c`)
- Crypto signing with secp256k1 (`crypto.c`)
- Node module (`node.c`)
- L1 mock implementation (`l1_mock.c`)

### M2: Multi-Process Networking
- ZeroMQ transport layer (`zmq_transport.c`)
- L1 mock service (`l1_service.c`)
- Sequencer → Witness batch distribution (PUB/SUB)
- Witness → Sequencer attestations (DEALER/ROUTER)
- L1-anchored replay verification

### M3: L1 Integration
- HPPLiteDA.sol contract with on-chain DA
- HPPLiteFactory.sol for one-click rollup deployment
- Web3 primitives (`eth/` - RLP, ABI, keccak256)
- Ethereum JSON-RPC client (`l1_eth.c`)
- EIP-191 signature verification
- Batch submission to L1
- State reconstruction from L1 batches

### M3.5: Transparent SQLite API
- `sqlite3_close_hook` for cleanup on close
- Auto-extension for `?hpplite=on` URI parameter
- L1 auto-connect from URI parameters (`rpc`, `contract`, `factory`)
- Factory auto-deploy on first open
- Timer thread for time-based batch flushing
- ZMQ in transparent API (`zmq_bind`, `zmq_sequencer` URI params)

### Testing
- Unit tests with mock L1 (15 tests)
- Integration tests with real L1 (HPP Sepolia)
- Full cluster test: sequencer + witness + ZMQ sync + L1 reconstruction
- Full attestation flow test: checkpoint creation → ZMQ broadcast → witness attestation → L1 submission

### Attestation & Checkpoint Flow
- Witness checkpoint window tracking during L1 sync
- Sequencer checkpoint creation and ZMQ broadcast
- Witness attestation signing and ZMQ send
- Sequencer attestation verification (L1 witness registry check)
- Checkpoint submission to L1 with attestations

---

## In Progress / Known Issues

### Bugs (Fixed)
- [x] **Flush on close**: `hpplite_node_destroy()` now flushes pending data before cleanup
- [x] **hpplite_sync()**: SQL function implemented - triggers L1 sync for witness/replica nodes
- [x] **hpplite_flush()**: SQL function implemented - triggers batch flush for sequencer nodes

---

## M4: L1 System Views (Planned)

Virtual tables exposing L1 state via SQL:

| Table | Source | Description |
|-------|--------|-------------|
| `hpplite_system` | L1 contract | System config (DA scheme, limits) |
| `hpplite_checkpoints` | L1 contract | Finalized checkpoints |
| `hpplite_batches` | DA layer | Batch metadata |
| `hpplite_witnesses` | L1 contract | Registered witnesses |
| `hpplite_status` | Local | Node status, sync state |

Example queries:
```sql
SELECT * FROM hpplite_system;
SELECT height, state_root FROM hpplite_checkpoints ORDER BY height DESC LIMIT 5;
SELECT u.name, c.state_root FROM users u, hpplite_checkpoints c WHERE c.height = 100;
```

---

## Future

### Transparent API Enhancements
- [ ] Size trigger in commit_hook (max changes per batch)
- [ ] `_hpplite_pending` table for crash recovery
- [ ] Write-ahead pattern (pending table → flush → clear)
- [ ] Multi-connection handling

### Data Availability
- [ ] Batch compression (reduce DA costs)
- [ ] IPFS batch storage
- [ ] Arweave batch storage
- [ ] Batch pruning after L1 finality

### Light Clients
- [ ] Merkle proofs for state queries
- [ ] Verify state without full replay

### Robustness
- [ ] ZMQ reconnection on disconnect
- [ ] Heartbeat/liveness detection
- [ ] Out-of-order batch handling (sync)
- [ ] L1 polling for new batches (witness/replica)
- [ ] Adversarial/Byzantine testing

### Advanced
- [ ] Fraud proofs (challenge invalid transitions)
- [ ] Cross-L2 messaging
- [ ] SQL subset restrictions (determinism)
- [ ] WAL mode support
- [ ] IoT optimizations (reduced footprint)

---

## File Structure

```
ext/hpplite/
├── hpplite.c/h          # Core state tracking, transparent API
├── batch.c/h            # Batch serialization
├── crypto.c/h           # secp256k1 signing
├── fs_storage.c/h       # File-based storage
├── node.c/h             # Node lifecycle, ZMQ, timer
├── config.c/h           # URI parsing, configuration
├── da_uri.c/h           # DA URI resolution
├── l1_interface.h       # L1 contract interface
├── l1_mock.c            # L1 mock (testing)
├── l1_service.c/h       # L1 mock ZMQ service
├── l1_eth.c/h           # Real Ethereum client
├── zmq_transport.c/h    # ZeroMQ networking
├── eth/                 # Web3 primitives
│   ├── eth_client.c/h   # JSON-RPC client
│   ├── keccak256.c/h
│   ├── rlp.c/h
│   ├── abi.c/h
│   └── cJSON.c/h
├── contracts/           # Solidity
│   └── src/
│       ├── HPPLiteDA.sol
│       └── HPPLiteFactory.sol
├── tests/
│   ├── unit/            # Mock L1 tests
│   └── integration/     # Real L1 tests
├── examples/            # Usage examples
└── doc/                 # Documentation
    ├── architecture.md
    └── backlog.md
```
