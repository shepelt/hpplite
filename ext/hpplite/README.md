# HPPLite

A lightweight L2/L3 rollup for SQLite with L1 data availability.

HPPLite enables verifiable SQL with checkpoint finality on Ethereum-compatible L1 chains. It uses the same security model as Optimism and Arbitrum: singleton sequencer with on-chain data availability.

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
┌─────────────────────────────────────────────────────────────────┐
│                         HPPLite                                 │
│                                                                 │
│   App ──► sqlite3_open() ──► Sequencer ──► L1 Contract          │
│                                  │          - Batch DA          │
│                                  ▼          - Checkpoints       │
│                              SQLite         - State roots       │
│                                                                 │
│   Verify anytime: hpplite verify <contract>                     │
└─────────────────────────────────────────────────────────────────┘
```

**Security model:** Same as Optimism/Arbitrum - centralized sequencer, decentralized verification. All data is on L1, anyone can verify.

## Features

- **Transparent API** - Just use `sqlite3_open_v2()` with URI parameters
- **Factory Deploy** - One-click rollup creation, no manual contract deployment
- **On-Chain DA** - Full batch data stored on L1 for reconstruction
- **On-Demand Verification** - Anyone can verify state from L1 data
- **State Reconstruction** - Rebuild entire database from L1 alone

## Why Singleton Sequencer?

SQLite apps expect strong consistency (read-your-writes). Distributed writes break this - you'd either need:
- Write forwarding (complex routing)
- Eventual consistency (breaks app expectations)
- Append-only restrictions (limits SQL)

HPPLite keeps full SQL semantics by using a single sequencer. This is the same trade-off made by every major L2.

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

*Either `factory` or `contract` is required.

## Building

```bash
# Prerequisites
brew install libsecp256k1 curl pkg-config  # macOS
# apt install libsecp256k1-dev libcurl4-openssl-dev  # Linux

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
ctest                    # Run all unit tests (~4s)

# Integration tests (real L1, requires funded wallet)
export HPPLITE_PRIVATE_KEY=0x...
./tests/integration/run_integration.sh
```

## Gas Costs (HPP Sepolia)

| Operation | Gas | Cost (ETH) | Cost (USD*) |
|-----------|-----|------------|-------------|
| Factory deploy | 3.5M | 0.000035 | $0.12 |
| Batch submit (~1KB) | 1.3M | 0.000013 | $0.04 |

*At $3,500/ETH, 10 gwei gas price

### Monthly Projections

| Usage | Batches/day | Monthly Cost |
|-------|-------------|--------------|
| Light (personal) | 10 | ~$13 |
| Medium (small app) | 100 | ~$130 |
| Heavy (production) | 1,000 | ~$1,300 |

**Cost driver**: 97% of cost is on-chain DA storage. Consider compression for high-volume apps.

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

## Contract Interface

```solidity
// HPPLiteFactory
function getOrCreateRollup() external returns (address);
function getRollup(address owner) external view returns (address);

// HPPLiteDA (per-rollup)
function claimSequencer(bytes32 instanceId) external;
function renewLease(bytes32 instanceId) external;
function submitBatch(bytes32 instanceId, uint256 height, bytes data) external;
function getBatch(uint256 height) external view returns (bytes);
function submitCheckpoint(bytes32 instanceId, uint256 from, uint256 to, bytes32 root) external;
```

## Verification

Anyone can verify the chain state from L1 data:

```bash
# Verify a rollup (fetches all batches from L1, replays, checks state roots)
hpplite verify 0xYOUR_CONTRACT_ADDRESS --rpc https://sepolia.hpp.io
```

Or programmatically:
```c
sqlite3_open_v2("file:verify.db?hpplite=on&mode=verify&contract=0x...", &db, ...);
```

No need to run 24/7 nodes. The ability to verify is the security guarantee.

## Roadmap

### Completed
- [x] SQLite state tracking with Merkle roots
- [x] Batch creation and signing
- [x] L1 checkpoint anchoring
- [x] Factory-based deployment
- [x] On-chain data availability
- [x] Transparent SQLite API
- [x] State reconstruction from L1

### Planned
- [ ] Verify CLI (`hpplite verify`)
- [ ] Batch compression (reduce DA costs)
- [ ] Challenge/fraud proof submission
- [ ] Light client mode (Merkle proofs only)
- [ ] Read replicas (opt-in stale reads for scale)

## Security Model

| What Sequencer Can Do | What Sequencer Cannot Do |
|-----------------------|--------------------------|
| Order transactions | Corrupt state (verifiable) |
| Censor temporarily | Steal funds (detectable) |
| Go offline | Hide fraud (full DA on L1) |

This is "trust but verify" - the same model securing billions on Optimism and Arbitrum.

See [doc/architecture.md](doc/architecture.md) for details.

## License

See parent SQLite project for license terms.
