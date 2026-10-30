# HPPLite

**A crash-safe, encrypted, replayable write log for SQLite, mirrored to a chain.**

HPPLite is a SQLite extension. It captures every write statement inside the
transaction that runs it, cuts the log into batches, seals them with a key
derived from your wallet key and posts them to an Ethereum-compatible chain
for data availability. Every N batches it anchors a SHA-256 of the whole
database (the *state root*) on the same chain. Open an empty file with the
same key and the database rebuilds itself from the chain; replay compares
every posted root with what it computes and says so when they differ.

Stock SQLite, loadable extension, no patches. One sequencer posts; the chain
keeps the history; whoever holds the key can rebuild and check it.

What v0.1 is **not**: not a multi-writer database, not a public verifier
(batches are encrypted, so only key holders can replay; viewing keys are
v0.2), not cheap DA (batches live in contract storage on a testnet today),
and not for databases much beyond 100 MB. See [Versions](#versions).

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

The same works from the stock `sqlite3` shell after `.load ./hpplite`, and
from any language whose SQLite binding can load extensions.

## How it works

```
┌──────────────────────────────────────────────────────────────────────┐
│  your process                                                        │
│                                                                      │
│  app ──► SQLite ──► _hpplite_pending (SQL text, inside your txn)     │
│                           │                                          │
│            timer / close  ▼                                          │
│                     batch = JSON ──► ChaCha20-Poly1305 ──► L1 DA     │
│                     every N batches: SHA-256(database) ──► L1 root   │
└──────────────────────────────────────────────────────────────────────┘
      rebuild: open empty file + same key ──► fetch, open, replay, compare roots
```

- **Transparent** - `sqlite3_open_v2()` with URI parameters, or `.load`
- **Stock SQLite** - no core patches; loadable extension against any SQLite >= 3.24
- **Nothing to lose** - the log row is written inside your own transaction;
  it commits and rolls back with your statement. A crash or a failed
  submission never loses or duplicates a batch. A statement that *cannot*
  be logged is refused (`SQLITE_INTERRUPT`) rather than applied unlogged
- **Encrypted by default** - batches are sealed before they leave the
  process with a key derived from your wallet key; the chain sees ciphertext
- **Factory deploy** - one wallet, one rollup, created on first open
- **Rebuild and check** - an empty file with the same key reconstructs from
  the chain and compares every posted state root along the way

## Why one writer?

SQLite apps expect read-your-writes. Distributed writes would need write
forwarding, eventual consistency or append-only restrictions; HPPLite keeps
full SQL by having one sequencer process per database. The chain is the
history, not a consensus layer: it makes the log durable, ordered and
checkable, and it is what a second machine rebuilds from.

## What is HPP?

HPP is the Ethereum-compatible network HPPLite is built for; its Sepolia
testnet is chain id **181228**, RPC `https://sepolia.hpp.io`, explorer
`https://sepolia-explorer.hpp.io`. The `hpp-sepolia` alias bakes those in
together with the factory below. Any EVM chain works with `rpc=` plus
`factory=` (or `contract=`) once the contracts in `contracts/` are deployed
there. You need a funded wallet for gas; the integration runner derives a
fresh test wallet from the key in `.env` for each run.

## Contracts

Deployed on **HPP Sepolia** (Chain ID: 181228, RPC: `https://sepolia.hpp.io`):

| Contract | Address | Description |
|----------|---------|-------------|
| HPPLiteFactory | `0xd1aaa8d10d53b9ea103fbb22d866cac3849c3b2d` | Creates rollups (one per wallet); built into the `hpp-sepolia` alias. Contract VERSION 6 (sequencer-only lease claim; rotating the sequencer ends its lease); rollups from earlier factories keep working with the rules they were created with (`0x9c6abd03…`: VERSION 5, `0x9cfacba5…`: permissionless claim) |
| HPPLite | *(per wallet)* | Your rollup's coordination contract: sequencer lease (only the owner-set sequencer wallet can claim it), checkpoints |
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
| `privkey` | Yes | Private key for signing (or `keyfile=/path`; see Keys) |
| `chainid` | With `rpc` | Chain id of the RPC (set by `l1`) |
| `datadir` | No | Reserved; v0.1 keeps all its state in the database file itself (`_hpplite_*` tables) |
| `interval` | No | Batch flush interval: `500ms`, `5s` (default), `1m`, `1h`; `0` = no timer (flush on `hpplite_flush()` or close) |
| `encrypt` | No | `on` (default when a key is present) or `off` |
| `checkpoint` | No | Batches per checkpoint, the batch that also carries a state root (default 100). A history shorter than that has no root on chain to verify against yet (see Verification) |
| `determinism` | No | `strict` (default): refuse SQL that replay cannot reproduce; `warn`: warn and count |

*Either `l1`, or both `rpc` and `factory` (or `rpc` and `contract`). The same settings can come from environment variables — see `.env.example`.

## Building

HPPLite is an ordinary SQLite extension: **no patches to SQLite, no special
compile options**, any amalgamation from 3.24 on. Two ways to use it.

**As a loadable extension** (`hpplite.dylib` / `hpplite.so`) against the
SQLite you already have:

```bash
brew install secp256k1 curl pkg-config  # macOS
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
sqlite> SELECT * FROM hpplite_status;      -- height, pending, state_root, ..., contract, last_batch_tx
sqlite> SELECT hpplite_flush();            -- or just wait for the interval / close
```

Programs call `sqlite3_load_extension()` once; every later connection in the
process that opens with `?hpplite=on` is handled automatically.

**Drivers.** HPPLite is turned on per connection, by the URI
(`file:app.db?hpplite=on&...`) or - for code that opens a plain path - by
the environment: `HPPLITE_FILES` names the files (`:`-separated globs,
matched against the full path and the file name) and `HPPLITE_PARAMS`
carries what the URI would (`l1=hpp-sepolia&interval=5s`). Load the module
once per process before opening. Checked by `tests/orm/run_orm.sh`:

| Driver / ORM | How | Status |
|---|---|---|
| Python `sqlite3`: SQLAlchemy, Alembic, Django | `sqlite3.connect(uri, uri=True)`; Django `NAME` = the URI | works |
| Node `node:sqlite`: Drizzle (`sqlite-proxy`) | URI, or a plain path with `HPPLITE_FILES` | works |
| npm `sqlite3`: Knex | `connection: { filename: uri, flags: ['OPEN_URI', 'OPEN_READWRITE', 'OPEN_CREATE'] }` | works |
| better-sqlite3 (Drizzle's and Knex's default) | - | **refused at open**: its bundled SQLite is built with `SQLITE_TRACE_SIZE_LIMIT=32`, which cuts bound strings in `sqlite3_expanded_sql()` - writes would replay truncated |
| Prisma | - | not supported: its engine has its own SQLite and loads no extensions |

One SQLite library per process: loading the module into a second one
(better-sqlite3's copy and Node's own) is refused.

### Using it from an application

Load the module once per process, through any connection (a throwaway
`:memory:` one is fine), before opening the database; then open the
database the HPPLite way. Connections opened afterwards in the process are
covered automatically.

```python
# Python sqlite3 (SQLAlchemy: pass a creator; Django: NAME = uri)
boot = sqlite3.connect(":memory:"); boot.enable_load_extension(True)
boot.load_extension("/path/to/hpplite"); boot.close()      # no suffix needed
uri = "file:app.db?hpplite=on&l1=hpp-sepolia&keyfile=/etc/app/key"
db = sqlite3.connect(uri, uri=True)
engine = create_engine("sqlite://", creator=lambda: sqlite3.connect(uri, uri=True, check_same_thread=False),
                       poolclass=QueuePool)
```

```js
// Node node:sqlite (Drizzle: drizzle-orm/sqlite-proxy over it)
const boot = new DatabaseSync(":memory:", { allowExtension: true });
boot.loadExtension("/path/to/hpplite.dylib"); boot.close();
const db = new DatabaseSync("file:app.db?hpplite=on&l1=hpp-sepolia");
// npm sqlite3 (Knex): new sqlite3.Database(":memory:").loadExtension(path, cb) first
```

- **The first connection to a file in a process is the sequencer** (it
  posts); later ones capture and leave posting to it. Open it at startup
  and keep it open for the life of the process; close it last. With a
  connection pool, open one connection yourself at startup and hold it.
  A migration tool run as its own process while the server is stopped is
  fine: it becomes the sequencer for its run.
- **Close flushes**: it posts what is pending, waits for L1 and releases
  the sequencer lease. Bounded: an unreachable RPC costs one timeout (15 s,
  `HPPLITE_RPC_TIMEOUT`) and the rows stay in the file for the next open -
  but the lease could not be released then, so the next process posts only
  once it expires (the lease length, minutes). To be sure before exiting,
  `SELECT hpplite_flush()` and check `pending` is 0.
- **Everything that writes is replayed**, DDL included: `CREATE`, `ALTER`,
  `DROP`, the table rebuilds migration tools do, `PRAGMA user_version` and
  `application_id`. `changes()`, `last_insert_rowid()` and `RETURNING`
  report the application's statements as in plain SQLite.
- **One wallet is one database on chain.** A new, empty file opened with the
  same key rebuilds that database from L1 (that is how you verify, or move
  to another machine). In the same process as the live writer it says
  `another sequencer instance holds the lease`: expected - it is a reader.

**Every connection that writes must open with `hpplite=on`** - a plain
connection's writes are not captured. The first `hpplite=on` connection to a
file in a process is the sequencer (L1, lease, timer); later ones are
*followers*: they capture into the same log in the file and post nothing,
the sequencer's flush posts everyone's rows (transaction groups stay correct
across connections). Close the sequencer last; rows a follower captures
after it is gone wait in the file for the next sequencer. Several
*processes* on one file work the same way, with the lease deciding who
posts. Loading HPPLite twice into one connection (static library plus
module) is refused.

**Statically linked** with the SQLite built in this tree (what the tests use):

```bash
mkdir build && cd build && ../configure && make sqlite3.c   # from repo root
cmake -S ext/hpplite -B ext/hpplite/build_real -DHPPLITE_USE_REAL_L1=ON
cmake --build ext/hpplite/build_real
```

## Testing

```bash
# Unit tests (mock L1, fast, no network)
ctest                    # 15 suites, ~5s: core, batch, node, open API, cipher, keys,
                         # durability (26 cases), loadable extension, crash harness,
                         # Python/CLI smoke, docs check

# Real contracts on a local chain (foundry: anvil + forge + cast; no network, no funds).
# Lease claim/renew/release transactions, a second instance of the same wallet, a
# stranger claiming after expiry, sleeping for days (evm_increaseTime). Built with
# the real-L1 backend; skipped when foundry is missing.
cmake -S ext/hpplite -B build_real -DHPPLITE_USE_REAL_L1=ON -DHPPLITE_SQLITE_SRC=... && cmake --build build_real
(cd build_real && ctest -R test_anvil --output-on-failure)

# The same on Linux, in a container (Docker or Podman; needs nothing else installed)
ext/hpplite/tools/linux_test.sh          # Ubuntu 24.04, Release, ctest
ext/hpplite/tools/linux_test.sh asan     # AddressSanitizer + UBSan
ext/hpplite/tools/linux_test.sh stress   # + the 100 MB stress test

# ORM smoke tests on anvil: SQLAlchemy + Alembic, Django (migrate, ORM), Drizzle, Knex;
# a replica rebuilt from the chain must match each writer. First run pip/npm installs
# into $TMPDIR/hpplite-orm (network), so not in ctest or CI
tests/orm/run_orm.sh build_real/hpplite.dylib [sqlalchemy|django|drizzle|knex ...]

# Integration tests (real L1, requires funded wallet); also runs the examples
export HPPLITE_PRIVATE_KEY=0x...
./tests/integration/run_integration.sh

# Microbenchmark (write overhead, flush, replay, root hash); see doc/performance.md
make bench

# Stress: 100 MB through encryption, DA chunking, checkpoints with roots, reconstruction
make stress            # ./stress_hpplite [--mb N] [--checkpoint N] [--limit BYTES]

# Crash injection (also in ctest): kill -9 at deterministic flush points and at random
# moments while the timer flushes; reopen, check nothing is lost or duplicated
./crash_hpplite --rounds 20
```

## Demo

`examples/demo/` is a public timeline page shared by visitors and a cast of
resident AI agents (written by a local model via LM Studio/Ollama, else
Claude, else templates): stock Python `sqlite3` loads the module, every entry
becomes an encrypted batch on HPP Sepolia with explorer links, a panel shows the ciphertext the chain
holds, and **Verify** rebuilds the database from the chain alone in a
separate process and compares roots. `examples/demo/run_demo.sh` serves it
and opens a Cloudflare quick tunnel (needs `cloudflared`, a real-L1 build
and `HPPLITE_PRIVATE_KEY`).

## Gas Costs (HPP Sepolia)

Measured on HPP Sepolia on 2026-10-28, where gas is 0.01 gwei (the chain's
base fee; it is a testnet, so there is no dollar price). The share by
contract, from the demo wallet's transactions: `submitBatch` 94% of all
gas, `submitCheckpoint` 3%, lease claim/renew/release 3%.

| Operation | Gas | Cost (ETH @ 0.01 gwei) |
|-----------|-----|------------------------|
| Factory deploy (VERSION 6, optimizer on) | 2.8M | 0.000028 |
| Rollup creation (`getOrCreateRollup`) | ~2.5M | 0.000025 |
| Batch submit (~250 B sealed; ~0.7k gas per further byte) | 323k | 0.0000032 |
| Batch submit (29 KB sealed, as cut on HPP Sepolia) | 20.7M | 0.00023 |
| Checkpoint | 50k | 0.0000005 |
| Lease claim / renew / release | 28-37k | 0.0000003 |

**Batch size on HPP Sepolia is set by the chain, not the contract.** The
contract allows 128 KB, but a transaction may use at most 32M gas there
(measured 2026-10-30; the block gas limit is 2^50, the per-transaction cap
binds), and contract storage costs ~0.7k gas per byte: ~44 KB is the most a
batch can carry. Its RPC also refuses more than ~84.9 KB of calldata
("oversized data"). HPPLite knows both for HPP Sepolia (by chain id) and
cuts batches at 40 KB; elsewhere set `maxtx=` (bytes) and `maxtxgas=`, or
let it learn: a batch the RPC refuses as oversized, or whose gas estimate
is over the cap, is never sent, and later batches are cut smaller. Bulk
data costs ~0.008 ETH per MB of SQL at 0.01 gwei.

| Usage | Batches/day | ETH/day on HPP Sepolia |
|-------|-------------|------------------------|
| Light (personal) | 10 | 0.00003 |
| Medium (small app) | 100 | 0.0003 |
| Heavy (production) | 1,000 | 0.003 |

On a chain priced like Ethereum mainnet (10 gwei) every figure is 1000x,
which is why on-chain storage DA is a testnet design: see #11/#33.

**Cost driver**: `HPPLiteDA` keeps batch bytes in contract storage
(`batchData[height]`), the most expensive place to put them. Posting them as
calldata or blobs behind an event, plus compression (#12), is the v0.2/v0.3
cost work; the timer only flushes when something is pending, so an idle
database costs nothing.

## Components

| File | Description |
|------|-------------|
| `hpplite.c` | Statement capture into `_hpplite_pending`, content-hash state root, batch build/finalize/replay, `hpplite_status` table, transparent API, extension entry point |
| `cipher.c` | Batch encryption: ChaCha20-Poly1305, HKDF-SHA256 key derivation, envelope format, pluggable schemes |
| `node.c` | Node lifecycle, bookkeeping connection, L1 sync, flush with retry, lease |
| `batch.c` | Batch creation and serialization |
| `clock.c` | Pass-through VFS that records and pins SQLite's "now" so clock SQL replays (#56) |
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

A replica rebuilt from L1 compares its content hash with the root posted at
each checkpoint (`roots_compared`, `root_mismatches`). Until the first
checkpoint (`checkpoint=`, default every 100 batches) there is no posted
root yet: compare `SELECT state_root FROM hpplite_status` of the writer and
the rebuilt file yourself, or use a smaller `checkpoint` in development.

`hpplite_status` columns: `height` (next batch number), `pending` (statements
logged, not yet posted), `state_root`, `prev_block_hash`,
`last_checkpoint_height`, `lease` (1 = this instance may post), `log_errors`,
`replay_skipped`, `encryption`, `keyed`, `nondeterministic`, `contract`,
`da_contract`, `last_batch_tx`, `foreign_batches`, `hooks_replaced`,
`roots_compared`, `root_mismatches`, `txns_dropped`, `last_refusal`.


What exists in v0.1 is **rebuild and compare**: open an empty file with the
same key and `l1=`/`contract=`; every batch is fetched, opened and replayed,
and after each batch that carries a state root the content hash of the
rebuilt database is compared with the posted one (`hpplite: state root
mismatch after replaying batch N` on stderr when they differ). The live root
of any database is `SELECT state_root FROM hpplite_status` (or
`hpplite_db_hash()` from C).

```bash
sqlite3> .load ./hpplite
sqlite3> .open 'file:rebuilt.db?hpplite=on&l1=hpp-sepolia&keyfile=/path/to/key'
sqlite3> SELECT height, state_root, roots_compared, root_mismatches, replay_skipped FROM hpplite_status;
```

A `hpplite verify` command that does this without writing a file, reports
per-table differences, and works with a viewing key instead of the wallet key
is v0.2 (#5, #15). There is no fraud proof or challenge period: the chain
holds the log and the roots, the key holder is the one who checks them.

## Versions

Scope is tied to versions. The current branch is **v0.1 in progress**.

| Version | Scope | Database size | DA |
|---------|-------|---------------|----|
| **v0.1** | Library + loadable extension on stock SQLite, crash-safe local file, reconstruction from L1, batches cut at the DA limit, root at checkpoint cadence, encrypted batches, one writer process | per-tenant / single-tenant, up to ~100 MB | on-chain (contract storage) |
| v0.2 | `hpplite verify` CLI, per-table divergence reporting, key roles and viewing keys (#17, #15), batch compression (#12) | same | same |
| v0.3 | S3-backed DA with L1 commitments, incremental (Merkle) state root, row-level capture (#19) | GB range | S3 + L1 commitments |
| v1.0 | Frozen batch / meta / URI formats, language bindings | — | — |

Why the size bound in v0.1: the state root is a content hash, O(database size) per
computation (~9 ms/MB), computed once per checkpoint interval (default 100 batches);
see [doc/architecture.md](doc/architecture.md#state-root).
Beyond that, shard per tenant (the factory can create several rollups per wallet)
until v0.3.

Issues are grouped by [milestone](https://github.com/shepelt/hpplite/milestones).

## Unsupported in v0.1

HPPLite v0.1 replays SQL *text*, so a statement must produce the same rows
wherever it runs. What cannot be reproduced is **refused at the write**
(`SQLITE_INTERRUPT`, a line on stderr, counted in
`hpplite_status.nondeterministic`) rather than discovered as a root mismatch
a hundred batches later:

| Refused | Why | Do this instead |
|---------|-----|-----------------|
| `random()`, `randomblob()` in a write | different value on replay | generate in the application, bind it; reads (`ORDER BY random()`) are fine |
| `'localtime'` / `'utc'` modifiers in a write (`datetime('now', 'localtime')`) | the machine's time zone differs between replicas | store UTC, convert when reading (reads may use them) |
| `last_insert_rowid()`, `changes()`, `total_changes()`, `sqlite_version()`, `sqlite_source_id()`, `sqlite_compileoption_*()`, `load_extension()` in a write | connection or build state | read the value (`SELECT last_insert_rowid()` is fine), then bind it |
| a write that calls a function the application registered (`sqlite3_create_function`, Python `create_function`; Django's `django_date_extract` behind `filter(due__month=3).update(...)`), directly or through a view, trigger, generated column or expression index | a replica has SQLite's functions only, so the statement could not run there (#53) | compute the value in the application and bind it, or filter with a built-in (`due__year`, a range); reads may call it |
| `ATTACH` | writes to another file are not captured | one database per file |
| writes that name `_hpplite_*` tables (`INSERT INTO _hpplite_pending`, a trigger on it, `RENAME TO _hpplite_x`, `INSERT INTO t SELECT ... FROM _hpplite_pending`) | the log and meta are per-replica, not data (#18) | reads are fine (`SELECT`), writes go through HPPLite |

**What a refusal looks like.** The driver reports only SQLite's code:
`interrupted` (`SQLITE_INTERRUPT`) or `not authorized` (`SQLITE_AUTH`) - in
Python an `OperationalError`, in Node an `Error` with that text - and the
transaction it was in is rolled back. If a write fails that way, the reason
and the statement are on stderr (`hpplite: ... statement refused`) and one
query away:

```sql
SELECT last_refusal FROM hpplite_status;
-- non-deterministic function random() in a write; replay cannot reproduce it; ... : INSERT INTO t VALUES(random())
```

`determinism=warn` (URI) or `HPPLITE_DETERMINISM=warn` restores warn-and-count
for people who know what they are doing (the `_hpplite_*`, size and
application-function rules stay). Detection is the parser's
authorizer (each write's functions, through views and triggers, checked
against what a fresh connection has) plus a scan of the text outside string
literals. Statements that cannot write (`SELECT`, `WITH ... SELECT`) are
never refused: they are not logged (#54).
`tests/unit/test_abuse.c` is the corpus of spellings it must catch and of
look-alikes it must let through.

**The clock is supported.** `CURRENT_TIMESTAMP`, `datetime('now')`,
`unixepoch()`, `strftime('%f', 'now')` and columns with `DEFAULT
CURRENT_TIMESTAMP` - the timestamp defaults every ORM template uses - work in
writes. SQLite takes "now" from the VFS; HPPLite registers a pass-through VFS
as the default that records the time each write read and pins that same time
when the write is replayed, so a replica computes identical values (#56).
Reads see the real clock. The one condition: the connection must be on that
VFS, which it is unless the application names another one (`vfs=` in the URI,
or a VFS registered as default after HPPLite was loaded); there, clock SQL in
writes is still refused.

**Transactions and statements of any size are supported.** One larger than
a DA batch (a seed script, `bulk_create` of thousands of rows, a data
migration, a multi-row `INSERT` an ORM builds from a list - one statement
can be larger than a batch, #59) is posted across several batches: the batch it starts in is marked open, the ones it
continues in say so, and a replica applies it only when its last part has
arrived - a reader never sees part of it. If the writer dies with the rest
in a log nobody else has, replicas drop the held part whole and count it in
`hpplite_status.txns_dropped`, as if it had never been posted (#57).

**Foreign keys and recursive triggers are supported.** `PRAGMA foreign_keys`
and `PRAGMA recursive_triggers` are connection state: they change what a
statement does to other rows (`ON DELETE CASCADE`, `SET NULL`, `SET DEFAULT`,
`ON UPDATE CASCADE`, a trigger that fires itself) without showing in its
text. HPPLite records the state each statement ran under and replays it in
that state, so an application that turns foreign keys on at connect (as
Django and Rails do) needs no change for it, and switching the pragmas
mid-session is fine (#52).

Not detectable, so not refused - know these:

- **Bound `REAL` values** are expanded with 15 significant digits
  (`sqlite3_expanded_sql`), so `0.1 + 0.2` replays as `0.3`. Store exact
  decimals as `INTEGER` or `TEXT`, or write the literal yourself. (Node's
  `node:sqlite` binds every JS number as `REAL`, so its integers appear as
  `1.0` in the captured text; the writer stored the same value, so replay
  agrees - bind `BigInt` if you want integers stored as integers.)
- **`TEXT` with an embedded NUL** is cut at the NUL in the captured text.
- **`INSERT ... SELECT` without `ORDER BY`** into a rowid table takes its row
  order from the query planner.

All of this is lifted in v0.2 by capturing row changes instead of statement
text (#19), which is why it is "unsupported", not "won't fix".

## Keys

One wallet key does everything in v0.1: it owns the rollup contracts, posts
batches (sequencer), and the batch encryption key is derived from it. It
reaches HPPLite before open, in order of precedence:

| How | Use for |
|-----|---------|
| `keyfile=/path` or `HPPLITE_PRIVATE_KEY_FILE` | **production**: a file with the hex key, mode `0600`; group/world-readable files are refused (like ssh) |
| `HPPLITE_PRIVATE_KEY` | containers / process managers |
| `privkey=0x...` in the URI | **development only**: SQLite keeps the query string next to the filename, so anyone with the `sqlite3*` can read it back via `sqlite3_uri_parameter()`, and URIs end up in logs |

Without a key the database works and keeps capturing, but nothing is posted
(`hpplite_status.keyed = 0`): with an L1 configured the batches stay pending
until a key arrives; with no L1 configured at all (`hpplite=on` alone) batches
are finalized locally, which is a development mode, not a backup. `encrypt=off`
is the only way to post plaintext and must be explicit. A URI with
`hpplite=on` that cannot be honoured (unreadable key file, malformed key)
makes `sqlite3_open_v2()` fail rather than open without capture. v0.2 splits the roles (owner / sequencer / data /
viewing keys), accepts keys at runtime (`hpplite_key()`), Ethereum keystore V3
files and KMS-held signers - see issues #17 and #15.

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
through the C API (`hpplite_node_create` with `:memory:`), with bookkeeping
on the same connection and no timer: flush explicitly or on close.

Three things a statement can be refused for, all with `SQLITE_INTERRUPT`
and a line on stderr, so the log never silently disagrees with the data:
its log row could not be written (disk full), its text is longer than one
DA batch can carry (a 64 KB blob literal is ~128 KB of hex), or its bound
parameters could not be expanded. Inside an explicit transaction SQLite
rolls the transaction back, as it would for a disk-full error.

See [doc/architecture.md](doc/architecture.md#local-durability).

## Trust model

The sequencer is your process; it is trusted to execute SQL. What the chain
adds is a history it cannot quietly rewrite: batches are posted in order
under a lease, roots are anchored, and anyone holding the key can rebuild
from the chain alone and compare. Nothing in v0.1 punishes a divergent
sequencer - there are no funds, bonds or fraud proofs here - it makes the
divergence detectable, and a lost disk recoverable. The lease keeps two
copies of your process from posting at once, and only the rollup's
sequencer wallet (the owner, unless set apart) can take it - a rollup can
sleep for days and nobody else can claim it. Rollups from the previous
factory keep the old permissionless claim; there the node evicts a
stranger's lease as owner, and a batch posted by someone else (it fails our
authentication, so it is provably not ours) is stepped over by every key
holder the same way instead of stopping reconstruction.

See [doc/architecture.md](doc/architecture.md) for details.

## License

See parent SQLite project for license terms.
