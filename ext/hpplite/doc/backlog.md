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
- HPPLite.sol contract (deployed to HPP Sepolia)
- HPPLiteDA.sol with on-chain DA + system config
- Web3 primitives (`eth/` - RLP, ABI, keccak256)
- Ethereum JSON-RPC client (`l1_eth.c`)
- EIP-191 signature verification
- Checkpoint submission with witness attestations

### M3.5: Transparent SQLite API
- `sqlite3_close_hook` for cleanup on close
- Auto-extension for `?hpplite=on` URI parameter
- L1 auto-connect from URI parameters

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
- [ ] Timer thread for time-based batch flushing
- [ ] Size trigger in commit_hook (max changes per batch)
- [ ] `_hpplite_pending` table for crash recovery
- [ ] Write-ahead pattern (pending table → flush → clear)
- [ ] Multi-connection handling

### Data Availability
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
├── hpplite.c/h          # Core state tracking
├── batch.c/h            # Batch serialization
├── crypto.c/h           # secp256k1 signing
├── fs_storage.c/h       # File-based storage
├── node.c/h             # Node lifecycle
├── l1_interface.h       # L1 contract interface
├── l1_mock.c            # L1 mock (M1)
├── l1_service.c/h       # L1 mock ZMQ service (M2)
├── l1_eth.c/h           # Real Ethereum client (M3)
├── zmq_transport.c/h    # ZeroMQ networking
├── eth/                 # Web3 primitives
│   ├── keccak256.c/h
│   ├── rlp.c/h
│   └── abi.c/h
├── contracts/           # Solidity (M3)
│   ├── src/HPPLite.sol
│   └── src/HPPLiteDA.sol
├── doc/                 # Documentation
│   ├── architecture.md  # System design
│   └── backlog.md       # This file
└── test_*.c             # Tests
```
