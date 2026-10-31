# Agent-built apps on HPPLite

The question HPPLite has to answer for its main use: does it hold up as the
persistence layer of an app a coding agent writes, with the frameworks and
ORMs agents reach for, from the documentation alone? And can anyone check,
from the chain alone, that the app's database is what the chain says?

## Protocol

1. Six agents (Claude, one per app, in parallel) each got the common brief
   (`BRIEF_COMMON.md`), one app brief (`apps.md`), a working directory and a
   development environment on a local chain. Their only documentation was
   HPPLite's README; HPPLite's source and tests were off limits. Each built
   the app, a `run.sh`, a `scenario.sh` that drives it over HTTP like a user
   session (phase 1; a real schema migration on the populated database;
   phase 2 using the new schema and re-checking phase 1), and `NOTES.md`
   recording every HPPLite error it met and every README gap.
2. Every app is then rerun from scratch by `verify_app.sh`: a fresh funded
   wallet (one wallet is one database on chain), a fresh data directory, the
   app's own scenario. After the last server stop, a replica is rebuilt into
   an empty file from the chain alone and must equal the app's database:
   height, state root, schema, and every row of every table, sqlite_sequence
   included.

```bash
# local chain (anvil + the factory deployed with forge), per app:
tests/agent_apps/verify_app.sh <app dir> http://127.0.0.1:8545 <factory> 31337 9201 anvil
# HPP Sepolia (wallet funded with 0.01 ETH from the key in ext/hpplite/.env):
tests/agent_apps/verify_app.sh <app dir> https://sepolia.hpp.io 0xc0fa50eb3076ed0e1ce73639097e40784950b8d8 181228 9201 sepolia
```

## What the agents found (2026-10-30)

All six apps worked on the first build, and no HPPLite statement was refused
in any of them. Their notes and the first verification rerun found six
HPPLite bugs, all fixed in c5d140bf58 with tests written first:

| Found by | Bug | Effect |
|----------|-----|--------|
| inventory (Knex) | `changes()` after an UPDATE/DELETE matching nothing said 1 | an order for 9 units with 3 in stock was accepted |
| habits (node:sqlite) | `last_insert_rowid()` after INSERT OR IGNORE of a duplicate was HPPLite's log row id | wrong ids handed back to the app |
| habits (node:sqlite) | a `RETURNING` write stepped once and reset (`StatementSync.get()`) was applied but never posted | rows silently missing from the chain |
| inventory (Knex) | "database is locked" in read-then-write transactions when a flush committed in between | about one write in a thousand refused |
| booking (Django) rerun | a zero-row INSERT into an AUTOINCREMENT table (Django rebuilding an empty table) was dropped from the log | the replica lacked a sqlite_sequence entry |
| habits (node:sqlite) | `PRAGMA user_version` was not replicated | a rebuilt database would re-run its migrations |

Plus the README gaps they listed (how to load the module per driver, which
connection is the sequencer with a pool, what close does, short histories
having no checkpoint root yet, the status columns), now in the README.

## Results

Rerun from scratch after the fixes; every replica equal to its writer
(height, root, schema, every row). HPP Sepolia: 2026-10-30, fresh wallet per
app, ~0.004 ETH of gas per app.

| App | Stack | Local chain (anvil) | HPP Sepolia |
|-----|-------|---------------------|-------------|
| tasks | FastAPI + SQLAlchemy + Alembic | equal | equal (26 batches) |
| booking | Django | equal | equal (9) |
| shortener | Flask + sqlite3 | equal | equal (22) |
| inventory | Express + Knex | equal | equal (45; its 72 KB import statement in pieces, #59) |
| forum | Drizzle on node:sqlite | equal | equal (28) |
| habits | node:http + node:sqlite | equal | equal (21) |

## What HPP Sepolia found

The first run there failed for all six, and found what anvil could not: HPP
Sepolia caps a transaction at 32M gas (its block limit is 2^50), so any
batch over ~44 KB of contract storage ran out of gas, reverted and was
retried forever, each attempt spending the whole 32M. HPPLite now cuts
batches at 40 KB there and never sends one that cannot fit (a2883a157b).
That made a single statement larger than ~39 KB impossible - inventory's
500-row Knex insert is one 72 KB statement - so statements now span batches
too (#59). forum's own script waited 30 s for its 3000-post seed to post
(tuned on anvil; HPP confirms a batch every ~1.6 s); its wait alone was
lengthened to 3 minutes, nothing else in the app or its checks changed.

Funding: `newkey_sepolia.sh` funds each wallet (0.003 ETH, HPPLITE_STIPEND
overrides) and records its key in `.keys` (git-ignored); `sweep_sepolia.sh`
returns what is left afterwards.
