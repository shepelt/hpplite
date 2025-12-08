# HPPLite

A lightweight L2 rollup built on SQLite with L1 anchoring to HPP Network.

HPPLite enables verifiable SQL state transitions with checkpoint finality on Ethereum-compatible L1 chains. It combines SQLite's proven reliability with blockchain's trust guarantees.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         HPP Sepolia (L1)                        │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                    HPPLite.sol                          │    │
│  │  - Checkpoint storage (height, stateRoot)               │    │
│  │  - Witness registry                                     │    │
│  │  - Attestation verification (ecrecover)                 │    │
│  └─────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
                              ▲
                              │ submitCheckpoint(from, to, stateRoot, sigs)
                              │
┌─────────────────────────────────────────────────────────────────┐
│                         HPPLite L2                              │
│                                                                 │
│  ┌──────────────┐    batches    ┌──────────────┐               │
│  │  Sequencer   │ ────────────► │  Witness 1   │               │
│  │              │               │              │               │
│  │  - Executes  │               │  - Verifies  │               │
│  │    SQL       │               │  - Attests   │               │
│  │  - Creates   │               └──────────────┘               │
│  │    batches   │                                               │
│  │  - Submits   │    batches    ┌──────────────┐               │
│  │    to L1     │ ────────────► │  Witness 2   │               │
│  └──────────────┘               │              │               │
│         │                       │  - Verifies  │               │
│         │                       │  - Attests   │               │
│         ▼                       └──────────────┘               │
│  ┌──────────────┐                                               │
│  │   SQLite     │               ┌──────────────┐               │
│  │  + State     │    batches    │  Witness N   │               │
│  │    Tracking  │ ────────────► │      ...     │               │
│  └──────────────┘               └──────────────┘               │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## Features

### Core (M1)
- **SQLite State Tracking** - Pre-update hooks capture all state changes
- **Batch Creation** - SQL operations grouped into signed batches
- **Merkle State Roots** - Cryptographic commitment to database state
- **Deterministic Replay** - Any node can reconstruct state from batches

### Networking (M2)
- **ZeroMQ Transport** - Sequencer-to-witness batch distribution
- **Multi-Process** - Nodes run as separate processes
- **Role-Based** - Sequencer, Witness, and Observer modes

### L1 Integration (M3)
- **HPP Sepolia** - Real Ethereum L1 checkpoint anchoring
- **EIP-191 Signatures** - Standard Ethereum signed messages
- **Witness Attestations** - Multi-sig checkpoint verification
- **State Reconstruction** - Rebuild L2 from L1 checkpoints + batch data

## Components

| Component | Description |
|-----------|-------------|
| `hpplite.c` | Core state tracking and Merkle tree |
| `node.c` | Node lifecycle and role management |
| `batch.c` | Batch creation, serialization, signing |
| `crypto.c` | secp256k1 signing and verification |
| `l1_eth.c` | Real Ethereum L1 client |
| `l1_mock.c` | In-memory L1 mock for testing |
| `eth/` | Web3 primitives (RLP, ABI, keccak256) |

## Building

```bash
# Prerequisites
brew install libsecp256k1 zeromq curl  # macOS
# apt install libsecp256k1-dev libzmq3-dev libcurl4-openssl-dev  # Linux

# Build SQLite first
cd /path/to/sqlite
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
# Unit tests (no network required)
./test_hpplite          # State tracking
./test_batch            # Batch serialization
./test_node             # Node lifecycle
./test_m3_witness       # Witness attestation
./test_m3_reconstruct   # State reconstruction
./test_m3_multinode     # Multi-node E2E

# Integration tests (requires HPP Sepolia)
./test_m3_e2e           # Full L1 integration
./test_m3_checkpoint    # Checkpoint submission

# Benchmarks
make speedtest          # Replay performance
```

## Configuration

| CMake Option | Default | Description |
|--------------|---------|-------------|
| `HPPLITE_USE_REAL_L1` | OFF | Use real Ethereum vs mock |
| `HPPLITE_ENABLE_ZMQ` | ON | Enable ZeroMQ networking |
| `HPPLITE_BUILD_TESTS` | ON | Build test executables |

## L1 System Contracts

Deployed on HPP Sepolia (Chain ID: 181228, RPC: `https://sepolia.hpp.io`):

| Contract | Address | Description |
|----------|---------|-------------|
| HPPLite | `0x2aC7688dFd3f81f189294cd12586776f43F19492` | Checkpoint-only (off-chain DA) |
| HPPLiteDA | `0x2Cd27d02C3b36c8926B849Bc53745Fab661572Dc` | Checkpoint + On-chain DA + System Config (v2) |

### HPPLiteDA - On-Chain Data Availability

The HPPLiteDA contract extends the base contract with on-chain batch storage:

```solidity
// Submit batch data to L1 for data availability
function submitBatch(uint256 height, bytes calldata data) external onlySequencer;

// Submit multiple batches in one transaction
function submitBatches(uint256 startHeight, bytes[] calldata batches) external onlySequencer;

// Retrieve batch data by height
function getBatch(uint256 height) external view returns (bytes memory);

// Get batch hash for verification
function getBatchHash(uint256 height) external view returns (bytes32);

// Get DA state
function getDAState() external view returns (
    uint256 lastBatchHeight,
    uint256 totalBatches,
    bytes32 latestBatchHash
);
```

This enables full L2 reconstruction from L1 alone - no external DA layer needed.

### System Config (Optimism-style)

HPPLiteDA v2 includes on-chain system configuration for client bootstrapping:

```solidity
// Get full system config for client discovery
function getSystemConfig() external view returns (
    string memory daScheme,      // "hppda", "ipfs", "file"
    address daContract,          // DA contract address (address(this) if self-hosted)
    uint256 batchSizeLimit,      // Max batch size in bytes
    uint256 version,             // Contract version
    uint256 chainId              // Chain ID from block.chainid
);

// Admin functions for config updates
function setDAScheme(string calldata _scheme) external onlyOwner;
function setDAContract(address _da) external onlyOwner;
function setBatchSizeLimit(uint256 _limit) external onlyOwner;
```

**DA URI Scheme:**

Batch references use a URI scheme for flexible DA layer switching:
```
hppda://<chainId>/<contract>/<height>
hppda://181228/0x2Cd27d02C3b36c8926B849Bc53745Fab661572Dc/42
```

Clients can bootstrap from just the contract address and discover all configuration on-chain.

### Full Contract Interface

```solidity
// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

contract HPPLite {
    // State
    address public owner;
    address public sequencer;
    address[] public witnesses;

    uint256 public requiredAttestations;    // Quorum (e.g., 2-of-3)
    uint256 public checkpointInterval;      // Batches between checkpoints
    uint256 public sequencerTimeout;        // Seconds before sequencer can be replaced

    uint256 public lastCheckpointHeight;    // Latest L2 height on L1
    bytes32 public lastStateRoot;           // Latest committed state root
    uint256 public lastCheckpointTime;      // Timestamp of last checkpoint

    // Events
    event CheckpointSubmitted(uint256 fromHeight, uint256 toHeight, bytes32 stateRoot);
    event SequencerChanged(address indexed oldSequencer, address indexed newSequencer);
    event WitnessAdded(address indexed witness);
    event WitnessRemoved(address indexed witness);

    // Submit checkpoint with witness attestations
    function submitCheckpoint(
        uint256 fromHeight,
        uint256 toHeight,
        bytes32 stateRoot,
        bytes calldata signatures    // Packed (r, s, v) * nWitnesses
    ) external;

    // Admin functions
    function setSequencer(address _sequencer) external;
    function addWitness(address _witness) external;
    function removeWitness(address _witness) external;
    function setRequiredAttestations(uint256 _required) external;

    // View functions
    function getState() external view returns (
        address owner,
        address sequencer,
        uint256 witnessCount,
        uint256 requiredAttestations,
        uint256 checkpointInterval,
        uint256 sequencerTimeout,
        uint256 lastCheckpointHeight,
        bytes32 lastStateRoot,
        uint256 lastCheckpointTime
    );

    function getWitnesses() external view returns (address[] memory);
    function isSequencerTimedOut() external view returns (bool);
}
```

### Checkpoint Signature Verification

The contract verifies attestations using `ecrecover`:

```solidity
// Message hash computation (must match client-side)
bytes32 message = keccak256(abi.encodePacked(fromHeight, toHeight, stateRoot));

// EIP-191 prefix for personal_sign compatibility
bytes32 ethSignedHash = keccak256(abi.encodePacked(
    "\x19Ethereum Signed Message:\n32",
    message
));

// Recover signer from signature
address signer = ecrecover(ethSignedHash, v, r, s);
require(isWitness[signer], "Invalid witness");
```

### Calldata Encoding

Checkpoint submission calldata format:

```
┌─────────────────────────────────────────────────────────────────┐
│ Bytes 0-3:   Function selector (4 bytes)                        │
│              keccak256("submitCheckpoint(uint256,uint256,       │
│                         bytes32,bytes)")[:4]                    │
│              = 0x????????                                       │
├─────────────────────────────────────────────────────────────────┤
│ Bytes 4-35:  fromHeight (uint256, 32 bytes, big-endian)         │
├─────────────────────────────────────────────────────────────────┤
│ Bytes 36-67: toHeight (uint256, 32 bytes, big-endian)           │
├─────────────────────────────────────────────────────────────────┤
│ Bytes 68-99: stateRoot (bytes32, 32 bytes)                      │
├─────────────────────────────────────────────────────────────────┤
│ Bytes 100-131: offset to signatures (uint256 = 128)             │
├─────────────────────────────────────────────────────────────────┤
│ Bytes 132-163: signatures length (uint256 = nSigs * 65)         │
├─────────────────────────────────────────────────────────────────┤
│ Bytes 164+:  signatures data                                    │
│              ┌─────────────────────────────────────────────┐    │
│              │ Sig 1: r (32) + s (32) + v (1) = 65 bytes   │    │
│              │ Sig 2: r (32) + s (32) + v (1) = 65 bytes   │    │
│              │ ...                                          │    │
│              └─────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
```

## Batch Storage Format

Batches are stored locally as JSON files and referenced by height.

### File Structure
```
{dataDir}/
├── batches/
│   ├── 00000001.json
│   ├── 00000002.json
│   └── ...
└── db.sqlite
```

### Batch JSON Schema
```json
{
  "height": 1,
  "prevStateRoot": "0x0000000000000000000000000000000000000000000000000000000000000000",
  "postStateRoot": "0xabc123...",
  "timestamp": 1702000000,
  "sequencerPubkey": "02abc123...",
  "signature": "abc123...",
  "operations": [
    {
      "type": "INSERT",
      "table": "users",
      "rowid": 1,
      "values": {
        "id": 1,
        "name": "alice"
      }
    },
    {
      "type": "UPDATE",
      "table": "users",
      "rowid": 1,
      "oldValues": { "name": "alice" },
      "newValues": { "name": "bob" }
    },
    {
      "type": "DELETE",
      "table": "users",
      "rowid": 1,
      "values": { "id": 1, "name": "bob" }
    }
  ]
}
```

### Operation Types

| Type | Description | Captured Data |
|------|-------------|---------------|
| `INSERT` | New row added | table, rowid, all column values |
| `UPDATE` | Row modified | table, rowid, old values, new values |
| `DELETE` | Row removed | table, rowid, deleted values |

### State Root Computation

The state root is a Merkle root over all table data:

```
stateRoot = keccak256(
    tableRoot("table1") ||
    tableRoot("table2") ||
    ...
)

tableRoot(name) = keccak256(
    rowHash(rowid1) ||
    rowHash(rowid2) ||
    ...
)

rowHash(rowid) = keccak256(
    columnValue1 ||
    columnValue2 ||
    ...
)
```

### Batch Signing

Batches are signed by the sequencer using secp256k1:

```
batchHash = keccak256(
    height ||
    prevStateRoot ||
    postStateRoot ||
    operationsHash
)

signature = secp256k1_sign(sequencerPrivkey, batchHash)
```

### Data Availability

Current implementation stores batches locally on each node. Future options:

| Storage | Pros | Cons |
|---------|------|------|
| Local FS | Fast, simple | Node must stay online |
| IPFS | Decentralized, content-addressed | Pinning required |
| Arweave | Permanent, pay once | Cost per byte |
| L1 calldata | Maximum security | Expensive |

For checkpoints, only the state root goes to L1. Full batch data is stored off-chain with the hash committed on-chain for verification.

## Usage Example

```c
#include "node.h"

// Create sequencer node
HppliteNodeConfig cfg = {
    .nodeId = "seq1",
    .dataDir = "/data/hpplite",
    .dbPath = "/data/hpplite/state.db"
};
memcpy(cfg.privkey, my_privkey, 32);

HppliteNode *node = hpplite_node_create(&cfg);
hpplite_node_set_role(node, HPPLITE_ROLE_SEQUENCER);
hpplite_node_start(node);

// Execute SQL
hpplite_node_exec(node, "CREATE TABLE users(id INT, name TEXT)", NULL);
hpplite_node_exec(node, "INSERT INTO users VALUES(1, 'alice')", NULL);

// Flush batch
uint64_t height = hpplite_node_flush_batch(node);

// Get state root
unsigned char root[32];
hpplite_node_get_state_root(node, root);
```

## Roadmap

### Completed
- [x] M1: Single-process state tracking and batching
- [x] M2: Multi-process with ZeroMQ networking
- [x] M3: L1 checkpoint anchoring on HPP Sepolia

### M4: L1 System Views (Planned)
- [ ] **Virtual Tables** - SQL access to L1 state
  - `hpplite_system` - System config from L1 contract
  - `hpplite_checkpoints` - Finalized checkpoints on L1
  - `hpplite_batches` - Batch metadata from DA layer
  - `hpplite_witnesses` - Registered witnesses
  - `hpplite_status` - Local node status, sync state
- [ ] **Cross-layer Queries** - Join L1 state with local data

### Future
- [ ] **Data Availability** - IPFS/Arweave batch storage
- [ ] **Batch Pruning** - Delete batches after L1 finality
- [ ] **Light Clients** - Verify state with Merkle proofs only
- [ ] **IoT Optimizations** - Reduced footprint for embedded devices
- [ ] **Fraud Proofs** - Challenge invalid state transitions
- [ ] **Cross-L2 Messaging** - Communication between HPPLite instances
- [ ] **SQL Subset Restrictions** - Determinism guarantees
- [ ] **WAL Mode Support** - Better concurrent read performance

## Design Decisions

### Why SQLite?
- Battle-tested, billions of deployments
- Single-file database, easy backup/restore
- Pre-update hooks enable state tracking
- Embedded, no external dependencies
- Perfect for edge/IoT scenarios

### Why HPP Network?
- EVM-compatible, standard tooling
- Low fees for checkpoint transactions
- Fast finality for L2 confirmation
- Testnet available for development

### Batch vs Transaction Model
HPPLite uses a batch model where multiple SQL operations are grouped:
- Reduces L1 costs (one checkpoint per N batches)
- Better throughput than per-TX commits
- Witnesses verify batch outcomes, not individual ops

## License

See parent SQLite project for license terms.
