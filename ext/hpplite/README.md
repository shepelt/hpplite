# HPPLite

A lightweight L2 rollup built on SQLite with L1 anchoring to HPP Network.

HPPLite enables verifiable SQL state transitions with checkpoint finality on Ethereum-compatible L1 chains. It combines SQLite's proven reliability with blockchain's trust guarantees.

## Quick Start

```c
// Just use SQLite with a special URI - that's it!
sqlite3 *db;
sqlite3_open_v2(
    "file:myapp.db?hpplite=on&rpc=https://sepolia.hpp.io"
    "&factory=0x51cD96b8F0BE5bD920326709D39b62130291CaDe"
    "&privkey=0xYOUR_PRIVATE_KEY",
    &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);

// Use SQLite normally - batches auto-submit to L1
sqlite3_exec(db, "CREATE TABLE users(id INT, name TEXT)", NULL, NULL, NULL);
sqlite3_exec(db, "INSERT INTO users VALUES(1, 'alice')", NULL, NULL, NULL);

// Close - state persists on L1
sqlite3_close(db);
```

On first open, the factory auto-deploys your rollup contract (~3s). All writes are batched and submitted to L1 for data availability.

## Architecture

```
HPPLiteFactory (singleton on L1)
    │
    └── getOrCreateRollup() ──► HPPLiteDA (your rollup)
                                    │
                                    ├── submitBatch() - store batch data
                                    ├── getBatch() - retrieve for reconstruction
                                    └── submitCheckpoint() - finalize with attestations

┌─────────────────────────────────────────────────────────────────┐
│                         HPPLite L2 Nodes                        │
│                                                                 │
│  ┌──────────────┐              ┌──────────────┐                │
│  │  Sequencer   │   batches    │  Witness(es) │                │
│  │              │ ───────────► │              │                │
│  │  - Execute   │              │  - Verify    │                │
│  │    SQL       │              │  - Attest    │                │
│  │  - Submit    │              │  - Sync      │                │
│  │    to L1     │              └──────────────┘                │
│  └──────────────┘                                              │
│         │                      ┌──────────────┐                │
│         ▼                      │   Replica    │                │
│  ┌──────────────┐              │              │                │
│  │   SQLite     │   L1 sync    │  - Read-only │                │
│  │  Database    │ ◄─────────── │  - Reconstruct│               │
│  └──────────────┘              └──────────────┘                │
└─────────────────────────────────────────────────────────────────┘
```

## Features

- **Transparent API** - Just use `sqlite3_open_v2()` with URI parameters
- **Factory Deploy** - One-click rollup creation, no manual contract deployment
- **On-Chain DA** - Full batch data stored on L1 for reconstruction
- **Witness Attestation** - Multi-sig checkpoint verification
- **State Reconstruction** - Any node can rebuild from L1 batches alone

## Contracts

Deployed on **HPP Sepolia** (Chain ID: 181228, RPC: `https://sepolia.hpp.io`):

| Contract | Address | Description |
|----------|---------|-------------|
| HPPLiteFactory | `0x51cD96b8F0BE5bD920326709D39b62130291CaDe` | Creates rollups (one per wallet) |
| HPPLiteDA | *(per-user)* | Your rollup contract |

## URI Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `hpplite` | Yes | Set to `on` to enable |
| `rpc` | Yes | L1 RPC URL |
| `factory` | Yes* | Factory contract address |
| `contract` | Yes* | Direct rollup address (if known) |
| `privkey` | Yes | Private key for signing |
| `datadir` | No | Local data directory |
| `role` | No | `sequencer`, `witness`, or `replica` |

*Either `factory` or `contract` is required.

## Building

```bash
# Prerequisites
brew install libsecp256k1 zeromq curl pkg-config  # macOS
# apt install libsecp256k1-dev libzmq3-dev libcurl4-openssl-dev  # Linux

# Build SQLite first (from repo root)
mkdir build && cd build
../configure && make sqlite3.c

# Build HPPLite
cd ext/hpplite
mkdir build && cd build
cmake .. -DHPPLITE_USE_REAL_L1=ON
make
```

## Testing

```bash
# Unit tests (mock L1, fast, no network)
ctest                    # Run all 15 unit tests (~4s)

# Individual unit tests
./test_hpplite           # Core state tracking
./test_batch             # Batch serialization
./test_node              # Node lifecycle
./test_l1_mock           # Mock L1 operations
./test_checkpoint_flow   # Checkpoint logic
./test_witness           # Attestation flow
./test_reconstruct       # State reconstruction
./test_multinode         # Multi-node coordination

# Integration tests (real L1, requires funded wallet)
export HPPLITE_PRIVATE_KEY=0x...
./test_l1_full_e2e       # Simple transparent API test
./test_full_cluster      # Full sequencer/witness/reconstruction

# Benchmarks
make speedtest           # Replay performance
```

## Gas Costs (HPP Sepolia)

| Operation | Gas | Cost (ETH) | Cost (USD*) |
|-----------|-----|------------|-------------|
| Factory deploy | 3.5M | 0.000035 | $0.12 |
| Batch submit (~1KB) | 1.3M | 0.000013 | $0.04 |
| Witness registration | 46K | 0.0000005 | $0.002 |

*At $3,500/ETH, 10 gwei gas price

### Monthly Projections

| Usage | Batches/day | Monthly Cost |
|-------|-------------|--------------|
| Light (personal) | 10 | ~$13 |
| Medium (small app) | 100 | ~$264 |
| Heavy (production) | 1,000 | ~$6,600 |

**Cost driver**: 97% of cost is on-chain DA storage. Consider compression (4x savings) or off-chain DA (14x savings) for high-volume apps.

## Components

| File | Description |
|------|-------------|
| `hpplite.c` | Core state tracking, transparent API |
| `node.c` | Node lifecycle, L1 sync, batch management |
| `batch.c` | Batch creation and serialization |
| `crypto.c` | secp256k1 signing |
| `config.c` | URI parsing, configuration |
| `l1_eth.c` | Real Ethereum L1 client |
| `l1_mock.c` | Mock L1 for testing |
| `eth/` | Web3 primitives (RLP, ABI, keccak256) |

## Test Structure

```
tests/
├── unit/              # Mock L1, no network required
│   ├── test_hpplite.c
│   ├── test_batch.c
│   ├── test_node.c
│   ├── test_l1_mock.c
│   ├── test_checkpoint_flow.c
│   ├── test_witness.c
│   ├── test_reconstruct.c
│   ├── test_multinode.c
│   └── test_zmq*.c
│
└── integration/       # Real L1 (HPP Sepolia)
    ├── test_l1_full_e2e.c      # Transparent API
    ├── test_full_cluster.c     # Sequencer + witness + reconstruction
    ├── test_factory.c
    └── test_l1_*.c
```

## Example: Full Cluster Setup

```c
// Sequencer node
sqlite3_open_v2(
    "file:seq.db?hpplite=on&role=sequencer"
    "&rpc=https://sepolia.hpp.io"
    "&contract=0xYOUR_ROLLUP"
    "&privkey=0xSEQ_KEY",
    &seq_db, ...);

// Witness node (different machine)
sqlite3_open_v2(
    "file:wit.db?hpplite=on&role=witness"
    "&rpc=https://sepolia.hpp.io"
    "&contract=0xYOUR_ROLLUP"
    "&privkey=0xWIT_KEY",
    &wit_db, ...);

// Read-only replica (reconstructs from L1)
sqlite3_open_v2(
    "file:replica.db?hpplite=on&role=replica"
    "&rpc=https://sepolia.hpp.io"
    "&contract=0xYOUR_ROLLUP",
    &replica_db, ...);
```

## Contract Interface

```solidity
// HPPLiteFactory
function getOrCreateRollup() external returns (address);
function getRollup(address owner) external view returns (address);

// HPPLiteDA (per-rollup)
function submitBatch(uint256 height, bytes data) external;
function getBatch(uint256 height) external view returns (bytes);
function submitCheckpoint(uint256 from, uint256 to, bytes32 root, bytes sigs) external;
function addWitness(address witness) external;
function setSequencer(address seq) external;
```

## Roadmap

### Completed
- [x] SQLite state tracking with Merkle roots
- [x] Batch creation and signing
- [x] ZeroMQ networking (sequencer ↔ witness)
- [x] L1 checkpoint anchoring
- [x] Factory-based deployment
- [x] On-chain data availability
- [x] Transparent SQLite API
- [x] State reconstruction from L1

### Planned
- [ ] Batch compression (reduce DA costs)
- [ ] Off-chain DA (IPFS/Arweave)
- [ ] Virtual tables for L1 state queries
- [ ] Light client mode (Merkle proofs only)
- [ ] Fraud proofs

## License

See parent SQLite project for license terms.
