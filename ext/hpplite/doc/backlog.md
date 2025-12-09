# HPPLite Backlog

## Completed

### M1: Single-Process State Tracking
- Core state tracking (`hpplite.c`)
- Batch serialization (`batch.c`)
- File storage (`fs_storage.c`)
- Crypto signing with secp256k1 (`crypto.c`)
- Node module (`node.c`)
- L1 mock implementation (`l1_mock.c`)

### M2: L1 Integration
- HPPLiteDA.sol contract with on-chain DA
- HPPLiteFactory.sol for one-click rollup deployment
- Web3 primitives (`eth/` - RLP, ABI, keccak256)
- Ethereum JSON-RPC client (`l1_eth.c`)
- EIP-191 signature verification
- Batch submission to L1
- State reconstruction from L1 batches

### M3: Transparent SQLite API
- `sqlite3_close_hook` for cleanup on close
- Auto-extension for `?hpplite=on` URI parameter
- L1 auto-connect from URI parameters (`rpc`, `contract`, `factory`)
- Factory auto-deploy on first open
- Timer thread for time-based batch flushing

### Testing
- Unit tests with mock L1
- Integration tests with real L1 (HPP Sepolia)
- State reconstruction test

---

## Architecture Decision: Singleton Sequencer

After evaluating distributed witness models, we settled on **singleton sequencer** for simplicity:

**Why not active witnesses?**
- Write routing from witness to sequencer is complex
- Eventual consistency breaks SQLite app expectations
- 24/7 witness nodes add infrastructure burden

**The chosen model:**
- Single sequencer serves all clients (strong consistency)
- All data on L1 (verifiable, reconstructable)
- On-demand verification (no 24/7 watchers needed)
- Same security model as Optimism/Arbitrum

See [architecture.md](architecture.md) for details.

---

## M4: Verify CLI (Next)

One-click verification from L1 data:

```bash
hpplite verify 0xCONTRACT_ADDRESS --rpc https://sepolia.hpp.io
```

Implementation:
- [ ] CLI argument parsing
- [ ] Fetch all batches from L1
- [ ] Replay SQL in order
- [ ] Compute and compare state roots
- [ ] Report verification result

This is the "ability to verify" that provides security without running 24/7 nodes.

---

## Future

### Cost Optimization
- [ ] Batch compression (gzip/zstd)
- [ ] Off-chain DA options (IPFS, Arweave) with L1 commitment
- [ ] Batch aggregation (multiple txs per batch)

### Security
- [ ] Challenge submission to L1
- [ ] Sequencer bonding/slashing
- [ ] Fraud proof generation

### Light Clients
- [ ] Merkle proofs for state queries
- [ ] Verify specific rows without full replay
- [ ] Mobile-friendly verification

### Scaling (If Needed)
- [ ] Read replicas (opt-in stale reads)
- [ ] Explicit `mode=replica` for read-only access
- [ ] Bounded staleness guarantees

### Developer Experience
- [ ] `@hpplite/js` - JavaScript/TypeScript SDK
- [ ] `@hpplite/next` - Next.js integration
- [ ] Better error messages
- [ ] Dashboard/explorer

---

## Removed from Scope

These were considered but removed for simplicity:

- **Active witness nodes** - replaced with on-demand verification
- **ZMQ P2P networking** - not needed for singleton sequencer
- **Witness attestations** - replaced with challenge-based security
- **Auto-discovery** - not needed without distributed nodes
- **Write forwarding** - clients connect directly to sequencer

The code for these features still exists but is not part of the primary architecture.

---

## File Structure

```
ext/hpplite/
├── hpplite.c/h          # Core state tracking, transparent API
├── batch.c/h            # Batch serialization
├── crypto.c/h           # secp256k1 signing
├── fs_storage.c/h       # File-based storage
├── node.c/h             # Node lifecycle, timer
├── config.c/h           # URI parsing, configuration
├── l1_interface.h       # L1 contract interface
├── l1_mock.c            # L1 mock (testing)
├── l1_eth.c/h           # Real Ethereum client
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
