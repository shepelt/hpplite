# Performance

Reference run of `tests/bench/bench_hpplite.c` (mock L1, no network).
Build with the unit tests and run `make bench`, or
`./bench_hpplite [--rows N] [--mb 1,10,100] [--json out.json]`.

Machine: Apple M1 Max, macOS 26.6, SQLite 3.54.1, WAL mode, default
`synchronous`, Release build (the CMake default since this run; a Debug
build is roughly 2x slower across the board). 2026-10-28.

## Write overhead

Cost of capturing statements into `_hpplite_pending`: trace callback,
`sqlite3_expanded_sql`, one extra INSERT in the same transaction, effect
check on completion.

| Mode | plain SQLite | hpplite=on | overhead |
|------|-------------:|-----------:|---------:|
| autocommit (one fsync per statement) | 15 900 stmt/s | 13 800 stmt/s | 1.2x |
| 1000 statements per transaction | 784 000 stmt/s | 442 000 stmt/s | 1.8x |

Autocommit is fsync-bound, so the extra row barely shows. Inside large
transactions capture costs ~45% of raw insert throughput: each user row
carries a second row (the SQL text, ~118 bytes here) plus the effect check.
That is the price of "nothing lives only in memory"; 440k captured
statements per second is still far above what any single-writer app does.

## Flush, encryption, replay (20 000 statements, one batch, 2.3 MB JSON)

| Step | Time | Rate |
|------|-----:|-----:|
| build + serialize + seal + submit (mock) + finalize, no root | 61 ms | 330 000 stmt/s |
| same, with state root + checkpoint | 81 ms | |
| posted batch size | 118 B/stmt | |
| seal (ChaCha20-Poly1305, vendored C) | 5.2 ms | 455 MB/s |
| open | 5.0 ms | |
| replay into a fresh file (2 batches, 40 000 stmts, incl. open) | 136 ms | 295 000 stmt/s |
| reopen with 20 000 pending statements | 3 ms | |
| first flush after that reopen | 39 ms | |

Encryption is ~8% of a flush and ~4% of a replay; it is not where time goes.

## State root (content hash)

`hpplite_db_hash()` walks every user table in rowid order and hashes
schema + typed values with SHA-256. Linear in database size; from the OS
page cache it is CPU-bound.

| Database | Root | Per MB |
|---------:|-----:|-------:|
| 1 MB | 6 ms | 8.5 ms/MB |
| 10 MB | 64 ms | 8.6 ms/MB |
| 100 MB (75 MB on disk) | 0.65 s | 8.6 ms/MB |

The root is computed once per checkpoint interval (default 100 batches),
not per batch, so at the v0.1 size bound (~100 MB) it costs under a second
every few minutes of continuous writing. Issue #9 (incremental per-table
roots) is the plan beyond that.

## Stress test: 100 MB end to end

`tests/stress/stress_hpplite.c` (`make stress`): writes through HPPLite with
encryption on, the 128 KB DA limit (so batches are cut into series), a
checkpoint every 20 batches (roots computed and checkpoints submitted), then
rebuilds a fresh database from the mock L1 and compares content roots.

| | |
|---|---:|
| rows / size | 1 729 000 rows, 100 MB on disk, ~500 rows per transaction |
| batches | 2 075 (1 729 partial, largest 112 805 B sealed), 86 with a root |
| checkpoints | 86, last root == live content root |
| write | 5.2 s (333 000 rows/s) |
| flush total | 50 s, of which ~45 s is the 86 root computations (one root at 100 MB: 1.04 s) |
| reconstruct from L1 | 50 s for 2 075 batches; dominated by verifying the root at each of the 86 checkpoints |
| result | row count, height and content root identical; 0 statements skipped |

Two things this run established: the root is the cost at this size (with the
default interval of 100 batches it would be ~10 s of the flush total instead
of ~45 s), and replay verification costs the same hashes again. Both are the
#9 incremental-root item; nothing else in the pipeline is size-sensitive.

It also caught two defects before the numbers above: a series of chunks
computed and discarded the root on every partial chunk once an interval was
reached (flush 232 s instead of 50 s), and a generator using `randomblob()`
showed that SQL-text replay cannot reproduce non-deterministic statements
(now detected and counted, #19).

## What the benchmark has caught

- `hpplite_build_batch_limited` cut over-limit batches by re-serializing
  the whole JSON per dropped statement: O(n^2), minutes for a 20 000-row
  import. Now sized from per-statement lengths, O(n).
- A statement that fails inside an explicit transaction kept its log row
  (only autocommit failures rolled it back). Capture now removes rows of
  statements with no effect; replay skips statements that fail.
- The default build was unoptimized; CMake now defaults to Release.
