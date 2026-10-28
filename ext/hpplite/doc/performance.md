# Performance

Reference run of `tests/bench/bench_hpplite.c` (mock L1, no network).
Build it with the unit tests and run `make bench`, or
`./bench_hpplite [--rows N] [--mb 1,10,100] [--json out.json]`.

Machine: Apple M1 Max, macOS 26.6, SQLite 3.54.1, WAL mode, default
`synchronous`. 2026-10-28.

## Write overhead

Cost of capturing statements into `_hpplite_pending` (trace callback,
`sqlite3_expanded_sql`, one extra INSERT in the same transaction, effect
check on completion).

| Mode | plain SQLite | hpplite=on | overhead |
|------|-------------:|-----------:|---------:|
| autocommit (one fsync per statement) | 14 900 stmt/s | 12 200 stmt/s | 1.2x |
| 1000 statements per transaction | 338 000 stmt/s | 208 000 stmt/s | 1.6x |

Autocommit is fsync-bound, so the extra row barely shows. Inside large
transactions the capture is a real ~40% tax on raw insert throughput: each
user row costs a second row (the SQL text, ~118 bytes here) plus the
bookkeeping lookups. That is the price of "nothing lives only in memory".

## Flush and replay (20 000 statements, one batch)

| Step | Time | Rate |
|------|-----:|-----:|
| build + serialize + submit (mock) + finalize, no root | 102 ms | 196 000 stmt/s |
| same, with state root + checkpoint | 147 ms | |
| batch JSON size | 118 B/stmt | |
| replay into a fresh file (2 batches, 40 000 stmts) | 268 ms | 149 000 stmt/s |
| reopen with 20 000 pending statements | 5 ms | |
| first flush after that reopen | 73 ms | |

## State root (content hash)

`hpplite_db_hash()` walks every user table in rowid order and hashes
schema + typed values with SHA-256. Linear in database size; from the OS
page cache it is CPU-bound.

| Database | Root | Per MB |
|---------:|-----:|-------:|
| 1 MB | 13 ms | 17 ms/MB |
| 10 MB | 129 ms | 17 ms/MB |
| 100 MB (75 MB on disk) | 1.3 s | 17 ms/MB |

The root is computed once per checkpoint interval (default 100 batches),
not per batch, so at the v0.1 size bound (~100 MB) it costs about a second
every few minutes of continuous writing. Issue #9 (incremental per-table
roots) is the plan beyond that.

## What the benchmark has caught

- `hpplite_build_batch_limited` cut over-limit batches by re-serializing
  the whole JSON per dropped statement: O(n^2), minutes for a 20 000-row
  import. Now sized from per-statement lengths, O(n).
- A statement that fails inside an explicit transaction kept its log row
  (only autocommit failures rolled it back). Capture now removes rows of
  statements with no effect; replay skips statements that fail.
