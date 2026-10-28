# HPPLite

A lightweight L2/L3 rollup for SQLite with L1 data availability.

HPPLite enables verifiable SQL with checkpoint finality on Ethereum-compatible L1 chains. It uses the same security model as Optimism and Arbitrum: singleton sequencer with on-chain data availability.

## Quick Start

```c
// Just use SQLite with a special URI - that's it!
sqlite3 *db;
sqlite3_open_v2(
    "file:myapp.db?hpplite=on&l1=hpp-sepolia"
    "&privkey=0xYOUR_PRIVATE_KEY",
    &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);

// Use SQLite normally - batches auto-submit to L1
sqlite3_exec(db, "CREATE TABLE users(id INT, name TEXT)", NULL, NULL, NULL);
sqlite3_exec(db, "INSERT INTO users VALUES(1, 'alice')", NULL, NULL, NULL);

// Close - state persists on L1
sqlite3_close(db);
```

`l1=hpp-sepolia` selects the RPC endpoint and factory contract for HPP Sepolia (see Contracts). On first open, the factory auto-deploys your rollup contracts (~3s). All writes are batched and submitted to L1 for data availability.

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
- **Stock SQLite** - No core patches; builds as a loadable extension (`.load ./hpplite`) against any SQLite >= 3.24
- **Encrypted by default** - Batches are sealed (ChaCha20-Poly1305) with a key derived from your wallet key before they leave the process; L1 sees ciphertext
- **Nothing to Lose** - Pending SQL lives in the database file itself, written inside your own transactions; a crash or a failed L1 submission never loses or duplicates a batch

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
| HPPLiteFactory | `0x9cfacba505ee281f1f6b0bd5bef8073a21f1519f` | Creates rollups (one per wallet); built into the `hpp-sepolia` alias |
| HPPLite | *(per wallet)* | Your rollup's coordination contract: sequencer lease, checkpoints |
| HPPLiteDA | *(per wallet)* | Your rollup's data-availability contract: batch data |

The factory address lives in one place in code (`da_uri.c`, `HPPLITE_NETWORKS`); `ctest` fails if this README disagrees with it.

## URI Parameters

| Parameter | Required | Description |
|-----------|----------|-------------|
| `hpplite` | Yes | Set to `on` to enable |
| `l1` | Yes* | Network alias (`hpp-sepolia`): sets `rpc` and `factory` |
| `rpc` | Yes* | L1 RPC URL (if no `l1`) |
| `factory` | Yes* | Factory contract address (if no `l1`) |
| `contract` | No | Connect to an existing rollup instead of using the factory |
| `privkey` | Yes | Private key for signing (or `keyfile=/path`) |
| `datadir` | No | Local data directory |
| `interval` | No | Batch flush interval, e.g. `5s` (default) |
| `encrypt` | No | `on` (default when a key is present) or `off` |

*Either `l1`, or both `rpc` and `factory` (or `rpc` and `contract`). The same settings can come from environment variables — see `.env.example`.

## Building

HPPLite is an ordinary SQLite extension: **no patches to SQLite, no special
compile options**, any amalgamation from 3.24 on. Two ways to use it.

**As a loadable extension** (`hpplite.dylib` / `hpplite.so`) against the
SQLite you already have:

```bash
brew install libsecp256k1 curl pkg-config  # macOS
# apt install libsecp256k1-dev libcurl4-openssl-dev  # Linux

cmake -S ext/hpplite -B build -DHPPLITE_USE_REAL_L1=ON \
      -DHPPLITE_SQLITE_SRC=/path/to/sqlite-amalgamation   # dir with sqlite3.c/.h/ext.h
cmake --build build
```

```
sqlite> .load ./build/hpplite
sqlite> .open 'file:app.db?hpplite=on&l1=hpp-sepolia&privkey=0x...'
sqlite> CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT);
sqlite> INSERT INTO users VALUES(1,'alice');
sqlite> SELECT * FROM hpplite_status;      -- height, pending, state_root, ...
sqlite> SELECT hpplite_flush();            -- or just wait for the interval / close
```

Programs call `sqlite3_load_extension()` once; every later connection in the
process that opens with `?hpplite=on` is handled automatically.

**Statically linked** with the SQLite built in this tree (what the tests use):

```bash
mkdir build && cd build && ../configure && make sqlite3.c   # from repo root
cmake -S ext/hpplite -B ext/hpplite/build_real -DHPPLITE_USE_REAL_L1=ON
cmake --build ext/hpplite/build_real
```

## Testing

```bash
# Unit tests (mock L1, fast, no network)
ctest                    # 13 suites, ~6s: core, batch, node, open API,
                         # durability (16 cases), loadable extension, docs check

# Integration tests (real L1, requires funded wallet); also runs the examples
export HPPLITE_PRIVATE_KEY=0x...
./tests/integration/run_integration.sh

# Microbenchmark (write overhead, flush, replay, root hash); see doc/performance.md
make bench
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
| `hpplite.c` | Statement capture into `_hpplite_pending`, content-hash state root, batch build/finalize/replay, `hpplite_status` table, transparent API, extension entry point |
| `cipher.c` | Batch encryption: ChaCha20-Poly1305, HKDF-SHA256 key derivation, envelope format, pluggable schemes |
| `node.c` | Node lifecycle, bookkeeping connection, L1 sync, flush with retry, lease |
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

## Versions

Scope is tied to versions. The current branch is **v0.1 in progress**.

| Version | Scope | Database size | DA |
|---------|-------|---------------|----|
| **v0.1** | Library + loadable extension on stock SQLite, crash-safe local file, reconstruction from L1, batches cut at the DA limit, root at checkpoint cadence, encrypted batches | per-tenant / single-tenant, up to ~100 MB | L1 calldata |
| v0.2 | `hpplite verify` CLI, per-table divergence reporting | same | same |
| v0.3 | S3-backed DA with L1 commitments, incremental (Merkle) state root, batch compression | GB range | S3 + L1 commitments |
| v1.0 | Frozen batch / meta / URI formats, language bindings | — | — |

Why the size bound in v0.1: the state root is a content hash, O(database size) per
computation (~9 ms/MB), computed once per checkpoint interval (default 100 batches);
see [doc/architecture.md](doc/architecture.md#state-root).
Beyond that, shard per tenant (the factory can create several rollups per wallet)
until v0.3.

Issues are grouped by [milestone](https://github.com/shepelt/hpplite/milestones).

## Privacy

Everything posted to L1 is public, so batch payloads are **encrypted before
posting** whenever a private key is present (`encrypt=off` or
`HPPLITE_ENCRYPT=off` to disable). The scheme is ChaCha20-Poly1305 with a key
derived from the wallet private key (`HKDF-SHA256`), so whoever can write the
rollup can read it and there is no extra secret to manage; reconstruction and
verification work from the same `privkey=`. Each batch is sealed with a fresh
nonce and bound to its height. `SELECT encryption FROM hpplite_status` shows
the active scheme; schemes are pluggable (`cipher.h`).

What stays public on L1: heights, batch sizes and timing, batch hashes, and
the state roots in checkpoints (content hashes that reveal *when* content
changed, not what). Only key holders can replay and verify; sharing read
access without the wallet key is the public-key scheme planned for v0.2.

## Durability

HPPLite keeps no state outside your database file. Captured SQL goes into
`_hpplite_pending` from inside your own transaction (rolled back with it,
committed with it); height and roots go into `_hpplite_meta` once a batch is
confirmed on L1. Kill the process at any point and reopen: pending SQL
flushes, height continues, nothing is replayed twice. A batch that L1 does
not confirm stays pending and is retried at the same height.

File databases are switched to WAL mode on open (bookkeeping runs on a
second connection) and get a 5 s busy timeout. In-memory databases work
too, with bookkeeping on the same connection.

See [doc/architecture.md](doc/architecture.md#local-durability).

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
