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

### Zero-patch extension (issue #2)
- `sqlite3_close_hook` core patch replaced by the `hpplite_status` eponymous
  virtual table: its `xDisconnect` runs inside `sqlite3_close()` on stock SQLite
- Loadable module `hpplite.dylib` / `.so` (`.load ./hpplite`), entry point
  `sqlite3_hpplite_init`, pinned in memory, auto-extension for later connections
- `HPPLITE_SQLITE_SRC` to build against any amalgamation; `test_loadable`
  links only stock SQLite and loads the module

### Stress test and determinism
- `tests/stress/stress_hpplite.c` (`make stress`): 100 MB through encryption,
  DA chunking, checkpoints with roots, reconstruction compared by root
- Capture counts non-deterministic statements (`random()`, `'now'`, ...);
  row-level capture is #19 (v0.3)
- `checkpoint=` URI / `HPPLITE_CHECKPOINT_INTERVAL`; the integration test now
  anchors a real checkpoint root on HPP Sepolia and compares it with the
  local content root

### Keys in v0.1 (issue #16)
- Key file loader refuses group/world-readable or non-regular files, accepts
  `0x`/bare hex with whitespace; a node without a key never posts (plaintext
  requires explicit `encrypt=off`); `hpplite_status.keyed`
- `test_keys`: URI / env / keyfile on both open paths, precedence, rejections,
  no-key behaviour, encrypt=off
- v0.2: #17 key roles, runtime `hpplite_key()`, keystore V3, KMS signer,
  SQLCipher page-key passthrough; v1.0: #18 shadow tables + defensive mode

### Encrypted batches (issue #14)
- `cipher.c`: ChaCha20-Poly1305 (RFC 8439 vectors in `test_cipher`), HMAC/HKDF-SHA256,
  binary envelope bound to height; key derived from the wallet key; on by default,
  `encrypt=off` / `HPPLITE_ENCRYPT=off`; plaintext history still replays
- Reads go through `hpplite_node_fetch_batch()` (opens the envelope); a node
  without the key sees ciphertext and syncs nothing
- Release build type is now the CMake default

### Microbenchmark
- `tests/bench/bench_hpplite.c` (`make bench`), reference numbers in
  `doc/performance.md`; found the O(n^2) size-limit cut and the
  failed-statement-in-transaction capture hole

### v0.1 wrap-up (issues #10, #13)
- State root at checkpoint cadence: only the interval-closing batch carries
  `state_root`; the flush loop submits the checkpoint automatically (before,
  nothing in production ever called `submit_checkpoint`)
- Examples run in the integration suite; found and fixed `l1=` alias not
  applying the factory on the `sqlite3_open_v2` path (the README quick-start)

### Batch size limit and test coverage (issues #7, #8)
- Batches are cut at the DA `batchSizeLimit` (`partial` series); early flush
  at half the limit; oversized single statements reported, not retried blindly
- `test_hpplite` / `test_batch` have real assertions (they always exited 0)
- Durability suite: adopt after lost confirmation / after crash, divergence
  refused, timer flush vs open transaction, checkpoint persistence, `:memory:`
  node, `hpplite_sync()`, size-limit chunking + reconstruction
- Mock L1 enforces sequential heights and the size limit like the contract
- JSON parser decodes `\uXXXX` (control characters in SQL round-trip)

### Durability (issue #6)
- Pending SQL in `_hpplite_pending`, written inside the user's transaction
- `_hpplite_meta` for height / prev hash / root / checkpoint
- Content-hash state root (`hpplite_db_hash`)
- Retry-safe flush: adopt batches already on L1, keep pending on failure
- Bookkeeping connection + WAL; lease released on close
- Bound parameters expanded in captured SQL

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

## Upstream Tracking

HPPLite needs **no SQLite core patches** (issue #2): it is a plain extension
and also builds as a loadable module against any stock amalgamation >= 3.24
(`cmake -DHPPLITE_SQLITE_SRC=...`, then `.load ./hpplite`). The `hpplite`
branch sits on an upstream SQLite **release tag** (currently
`version-3.54.1`, recorded in `patches/UPSTREAM_BASE`) with only
`ext/hpplite/` commits above it. Move to a new release with:

```bash
ext/hpplite/tools/sync_upstream.sh version-3.55.0
```

Rebase, never merge, so `git log version-X..hpplite` always answers "what did we change".

---

## Versions (GitHub milestones)

- **v0.1 — Small databases, on-chain DA** (current, complete): scope: databases up to ~100 MB, L1 calldata DA.
- **v0.2 — Verification**: #5 `hpplite verify`, per-table hashes for divergence reporting.
- **v0.3 — Large databases, economical DA**: #11 S3 DA with L1 commitments, #9 incremental state root, #12 batch compression. These ship together: cheap DA invites the sizes where a full-scan root is the wall.
- **v1.0 — Stable formats**: frozen batch JSON / `_hpplite_meta` / `hppda://`, bindings.

---

## Future

### Cost Optimization
- [ ] Off-chain DA beyond S3 (IPFS, Arweave) with L1 commitment
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
├── patches/             # The one SQLite core patch + UPSTREAM_BASE
├── tools/               # sync_upstream.sh
└── doc/                 # Documentation
    ├── architecture.md
    └── backlog.md
```
