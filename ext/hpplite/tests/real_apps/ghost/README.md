# Ghost on HPPLite (#58)

Stock [Ghost](https://ghost.org) 6.73.0 with its SQLite database under HPPLite.
Ghost's code is untouched. Two changes are made around it:

- Ghost's better-sqlite3 is rebuilt against a stock SQLite amalgamation
  (`setup.sh`). Its bundled SQLite is compiled with
  `SQLITE_TRACE_SIZE_LIMIT=32`, which cuts bound strings in expanded SQL, so
  HPPLite refuses it at open.
- A `NODE_OPTIONS=--require` preload loads HPPLite into that SQLite, and
  `HPPLITE_FILES` names Ghost's database (`run.sh`).

```bash
tests/real_apps/ghost/setup.sh /tmp/ghost build          # Node >= 24.20, pnpm >= 11.10
HPPLITE_PARAMS="rpc=...&factory=...&chainid=...&privkey=0x..." \
  tests/real_apps/ghost/run.sh /tmp/ghost build_real/hpplite.dylib &
python3 tests/real_apps/ghost/drive.py                     # the Admin API session
# stop Ghost (SIGINT), then the replica check:
HPPLITE_ANVIL_RPC=... HPPLITE_ANVIL_FACTORY=... HPPLITE_ANVIL_KEY0=0x... \
  HPPLITE_ORM_DIR=/tmp/ghost/content/data python tests/orm/orm_common.py ghost-local.db
```

`drive.py` runs a publisher's first day through the Admin API: owner setup,
staff session, 10 tags, 60 posts (Lexical; drafts, published, scheduled,
featured), 20 edits, 5 deletes, 8 pages, 150 members with labels, a
settings change and a newsletter.

## Results (2026-10-30, anvil)

- First boot, all of Ghost's migrations and fixtures (106 tables): clean,
  three times in a row.
- Admin API session: every call succeeded.
- A replica rebuilt from the chain alone equals Ghost's database: 102 tables,
  2208 rows, 15 batches, same state root.
- Found on the way, fixed in 280a364cf2: a finalize deferred while Ghost had
  a statement open was taken for a fork; a batch built while a no-op
  statement was being unlogged carried a row the log no longer had.
