# Capture engine v2: row-based capture

**Status: shipped in v0.2 (#19), the default (`capture=rows`). Record what a write did, not what it said.**

## Why

v0.1 captures the text of every write and replays it. That is compact and
readable, and it is correct only when replaying the text reproduces the
effect - which needs the replica to have the same functions, the same
clock, the same connection state, and the same order. Each of these has
had to be handled one by one:

| Problem with replaying text | v0.1 answer |
|---|---|
| `random()`, `randomblob()` in a write | refused (#53) |
| `CURRENT_TIMESTAMP`, `datetime('now')`, column DEFAULTs | clock pinned per statement, time recorded (#56) |
| application-defined functions (WordPress's REGEXP) | refused (#53, #19) |
| failed and no-op statements | effect check after each statement, rows removed (in a transaction) or kept (autocommit) |
| foreign-key and recursive-trigger settings | connection mode recorded per statement (#52) |
| a zero-row INSERT creating a `sqlite_sequence` entry | fingerprint check per statement |

Every row of that table is gone if the log holds the *effect*: which rows
changed, to which values. MySQL went the same way (statement-based, then
row-based and mixed replication).

## Mechanism: per-connection TEMP triggers

Each HPPLite connection creates, in its own TEMP schema, `AFTER INSERT`,
`AFTER UPDATE` and `AFTER DELETE` triggers on every table of the database.
The triggers append one record per changed row to the log table in the
database file.

- **Inside the application's transaction.** A record commits or rolls back
  with the change it describes (crash safety as in v0.1), and records from
  every connection and process are in commit order. A statement that fails,
  or changes nothing, writes nothing.
- **Any SQLite build.** The preupdate hook and the session extension would
  avoid triggers, but they are compile-time options: present in Homebrew's
  and macOS's SQLite and in Node's, absent in the static PHP that runs
  WordPress and in some Python builds. A loadable extension cannot count on
  them. They also cannot write inside the transaction, so a crash between
  commit and persisting the changes would lose them.
- **Invisible to the application's schema.** TEMP triggers are not in
  `sqlite_schema`; migration tools that compare schemas see nothing.
- **Effects of everything.** The application's own triggers, foreign-key
  cascades and `REPLACE` conflict deletes all arrive as row changes
  (checked on SQLite 3.51: cascades and app triggers captured in order).
  Virtual tables (FTS5, R*Tree) are the exception: they cannot carry
  triggers, and their internal (shadow) tables must not. FTS5 accepts a
  trigger there, but its own flush then opens a statement savepoint that
  flushes again, and the recursion overflows the stack. A write that names a
  virtual table, or a table or view whose triggers reach one (the usual
  external-content FTS sync), is logged as text, the v1 way, and replays
  with triggers on.

### Records

| Op | Contents |
|---|---|
| Insert | table, key, every stored column's value |
| Update | table, old key, new key if it changed, the changed columns only |
| Delete | table, key |

The key is the rowid, or the primary key of a WITHOUT ROWID table.
Generated columns are left out (the replica computes them). Values are
encoded by an HPPLite function the triggers call - type tag and exact
payload - so integers above 2^53, REALs and blobs round-trip bit for bit.

### Schema and the rest

Schema changes (CREATE, ALTER, DROP), `PRAGMA user_version` and
`application_id` stay statement text, in the same ordered log. After any
schema change a connection rebuilds its triggers before its next write, so
a migration that rebuilds a table (create new, copy rows, drop old, rename)
is captured row by row. `sqlite_sequence` cannot carry a trigger: replaying
inserts with their original rowids moves it as on the writer, and the one
case that does not - an entry created without a row - is recorded as a
small fix-up record, as today.

## Replay

Records are applied with triggers disabled
(`SQLITE_DBCONFIG_ENABLE_TRIGGER`; the application's TEMP triggers, which
that leaves on, are set aside for them, #76) - their effects are already in
the log; firing them again would double them. Foreign keys are the
writer's for each record (its mode bit), checked at the end of the group
(`defer_foreign_keys`), so a cascade's records, which come before their
cause, pass. Insert is `INSERT OR REPLACE` with the original key (which
also reproduces conflict replacement), update is `UPDATE OR REPLACE` for
the same reason (#64); update and delete go by key. Statement text (schema)
is replayed as in v0.1.

## Format and compatibility

Each entry carries its kind in its mode bits (`HPPLITE_MODE_ROW`, 4): a
batch mixes records and statements. A replica keeps replaying v0.1
(statement) entries, so existing histories continue; a v0.1 database
upgrades in place, its next entries being records. v0.1 refuses a mode
bit it does not know and stops.

## Tampering

TEMP triggers belong to the connection that made them: no other
connection or process can see or change them. The application could, on
its own connection - `DROP TRIGGER`, by mistake more likely than not. The
authorizer HPPLite already installs refuses dropping or replacing its
triggers, and lets writes to the log through only from inside them (the
authorizer is told which trigger is running). Before a write, a connection
checks the TEMP schema version and rebuilds its triggers if it changed.

Anything that goes around capture - a connection without HPPLite, the file
edited directly, an application set on defeating the authorizer - cannot be
prevented from inside the process, but it is not silent: checkpoint roots
are computed from the database file, a replica's from replaying the chain,
and an uncaptured change makes them differ (`root_mismatches` for any
verifier). The trust model is v0.1's: the writer is trusted, divergence is
detected.

## Limits and risks

- **VACUUM** renumbers the rowids of tables without an `INTEGER PRIMARY
  KEY`, and no trigger sees it. Logged as a statement once it has run; a
  replica's VACUUM renumbers the same rows the same way (shown in
  `test_rows_e2e` and `test_abuse`: updates and deletes by rowid after it
  land on the same rows; without the logged VACUUM they do not).
- **Size.** Inserts cost about what their SQL did; updates carry changed
  columns only; bulk statements (`UPDATE` of every row, `INSERT ... SELECT`)
  cost far more than their text. Reducing it is v0.3 (compression, DA,
  #33).
- **Write overhead**: one log row per changed row instead of per statement.
  Measured in phase 1 through the real engine (sysbench-style OLTP, 100k
  rows, WAL, mean µs per transaction):

  | | v1 text | rows |
  |---|---|---|
  | writes (4 statements) | 19.4 | 13.0–14.7 |
  | commit | 140 | 138 |
  | log bytes, same work | 10.2 MB | 7.6 MB |

  Each cached write statement carries its tables' trigger programs: about
  5.5 KB more per statement. On a connection with few statements that is
  enough to fill SQLite's lookaside memory (default about 124 slots), and the
  connection's small allocations fall through to malloc: a sort-heavy read
  (`SELECT DISTINCT ... ORDER BY`) went from 29 to 41 µs. With 300 slots
  (`SQLITE_DBCONFIG_LOOKASIDE`) reads are equal to v1. HPPLite does not
  size the lookaside of the connections it attaches to; an application
  that measures the difference can.
- **Readable history**: v0.1 batches are SQL, an audit trail of what the
  application did; records are not. Rows only in v0.2; statement text kept
  alongside for audit (not replayed) is an option later.
- Not captured, as before: connections without HPPLite (#37), TEMP tables.

## Phases

1. Prototype and benchmark: trigger manager, record encoding, replay of
   inserts, updates, deletes; against v1 on the same workloads - equality,
   size, overhead.
2. Engine: schema changes in the log, trigger rebuild, WITHOUT ROWID,
   `sqlite_sequence`, VACUUM, the authorizer guarding the triggers.
   Done: rebuild, WITHOUT ROWID, virtual tables (by the table a write
   targets, closed over triggers and foreign key actions),
   `sqlite_sequence` (sequence records the flusher logs from the
   bookkeeping connection: a zero-row INSERT creates an entry no row
   record carries), VACUUM (logged once it has run, replayed outside a
   transaction: a replica renumbers rowids the same way; it was refused
   before, its internal ATTACH taken for the application's), the guard (the
   application cannot create or drop `hpplite_cap_` triggers; a table's own
   drop takes them, rebuilt after).
3. Batch format v2; v1 history still replayed. Done: a record is a batch
   entry whose text is base64 of (op, table, record), marked by mode bit 4,
   so pieces (#59) and open transactions (#57) work unchanged and v0.1
   refuses such a batch. Replay applies records with triggers off, under the
   writer's foreign keys, deferred to the end of the group. The default
   since 2026-10-31; `capture=text` keeps statement capture, and
   every node-level suite runs in both modes (the `_text` tests).
   `test_rows_e2e` covers what only records make possible.
4. Validation: all current suites, agent apps, Ghost, WordPress concurrent;
   then SQLite's TCL suite with HPPLite on every connection, Rails 8,
   Home Assistant.
