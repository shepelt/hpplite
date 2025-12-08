# HPPLite Examples

Example programs demonstrating HPPLite features.

## Quick Start

HPPLite is built into SQLite - just use `?hpplite=on` in your URI:

```c
#include "hpplite.h"

// Open database with HPPLite enabled - no registration needed!
sqlite3 *db;
sqlite3_open_v2(
    "file:state.db?hpplite=on&role=sequencer&l1=hpp-sepolia",
    &db,
    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI,
    NULL
);

// Use standard SQLite API - changes are tracked automatically!
sqlite3_exec(db, "CREATE TABLE users(id, name)", NULL, NULL, NULL);
sqlite3_exec(db, "INSERT INTO users VALUES(1, 'Alice')", NULL, NULL, NULL);

// State root updates after each commit
unsigned char root[32];
hpplite_state_root(db, root);

// Flush batch when ready
uint64_t height = hpplite_flush(db);

// Close - close hook flushes any pending changes automatically
sqlite3_close(db);
```

## Building

From the `examples/` directory:

```bash
# Ensure HPPLite is built first
cd ../build
cmake .. -DHPPLITE_USE_REAL_L1=ON
make

# Build examples
cd ../examples
make
```

Or build individually:

```bash
gcc -I.. -I../../build -o basic_sequencer basic_sequencer.c \
    ../build/libhpplite.a ../build/libsqlite3.a \
    $(pkg-config --libs libsecp256k1 libcurl) -lzmq
```

## Examples

### basic_sequencer.c

Basic sequencer example demonstrating:
- Opening database with `sqlite3_open_v2()` and `?hpplite=on` URI parameter
- Using standard `sqlite3_exec()` for all SQL operations
- Automatic state root tracking
- Creating batches with `hpplite_flush()`
- Transparent close via `sqlite3_close()` (close hook handles cleanup)

```bash
./basic_sequencer
```

### l1_config.c

L1 configuration example showing:
- Network aliases (`l1=hpp-sepolia` → chainId=181228)
- Reading system config from L1 contract
- Building DA URIs
- Layered configuration (URI params override env vars)

```bash
./l1_config
```

**Note:** Requires network access to HPP Sepolia.

### da_uri_demo.c

DA URI scheme demonstration:
- Parsing `hppda://`, `file://`, `ipfs://` URIs
- Network alias resolution (hpp-sepolia → 181228)
- Building URIs from components

```bash
./da_uri_demo
```

### factory_demo.c

Factory pattern demonstration showing:
- Auto-discovery of rollup from wallet (private key)
- Auto-creation of rollup if none exists
- One rollup per wallet (1:1 wallet:rollup mapping)
- Using factory helper functions

```bash
export HPPLITE_PRIVATE_KEY=0x...
./factory_demo
```

**Note:** Requires network access to HPP Sepolia and the factory contract to be deployed.

## URI Parameters

Open databases with configuration in the URI:

```
file:state.db?hpplite=on&role=sequencer&l1=hpp-sepolia&contract=0x...
```

| Parameter | Description | Example |
|-----------|-------------|---------|
| `hpplite` | Enable HPPLite (required) | `hpplite=on` |
| `role` | Node role | `sequencer`, `witness`, `observer` |
| `l1` | L1 network alias | `hpp-sepolia`, `hpp-mainnet` |
| `chainid` | L1 chain ID (explicit) | `181228` |
| `rpc` | L1 RPC URL | `https://sepolia.hpp.io` |
| `contract` | L1 contract address | `0x2Cd27...` |
| `datadir` | Batch storage directory | `/var/lib/hpplite` |
| `interval` | Batch interval | `5s`, `100ms`, `1m` |
| `privkey` | Private key (hex) | `0x01020304...` |
| `keyfile` | Path to private key file | `/path/to/key` |

## Environment Variables

Configuration can also be set via environment (URI params take priority):

```bash
export HPPLITE_ROLE=sequencer
export HPPLITE_CHAIN_ID=181228
export HPPLITE_RPC_URL=https://sepolia.hpp.io
export HPPLITE_CONTRACT=0x2Cd27d02C3b36c8926B849Bc53745Fab661572Dc
export HPPLITE_DATA_DIR=/var/lib/hpplite
export HPPLITE_PRIVKEY=0x0102030405...
export HPPLITE_BATCH_INTERVAL=5s
```

## DA URI Formats

### hppda:// (On-chain L1 DA)

```
hppda://<chainId>/<contract>/<height>
hppda://<chainId>/<contract>/<from>-<to>
hppda://<alias>/<contract>/<height>
```

Examples:
```
hppda://181228/0x2Cd27d02C3b36c8926B849Bc53745Fab661572Dc/42
hppda://hpp-sepolia/0x2Cd27d02C3b36c8926B849Bc53745Fab661572Dc/1-100
```

### file:// (Local filesystem)

```
file:///path/to/batch.json
```

### ipfs:// (IPFS)

```
ipfs://QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG
```

## Network Aliases

| Alias | Chain ID | RPC URL |
|-------|----------|---------|
| hpp-sepolia | 181228 | https://sepolia.hpp.io |
| hpp-mainnet | 181227 | https://mainnet.hpp.io |
| ethereum | 1 | https://eth.llamarpc.com |
| sepolia | 11155111 | https://sepolia.drpc.org |

## Contract Addresses

HPPLiteDA v2 on HPP Sepolia:
```
0x2Cd27d02C3b36c8926B849Bc53745Fab661572Dc
```
