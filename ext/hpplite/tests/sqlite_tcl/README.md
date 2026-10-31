# SQLite's own tests with HPPLite on every connection (#62)

SQLite's TCL suite (`test/*.test` in this tree), run twice: on a stock
`testfixture`, and with HPPLite loaded into it and turned on for every file
a test opens (`HPPLITE_FILES="*"`, row capture, no chain). What fails only
with HPPLite is what HPPLite changes about SQLite's behaviour.

```bash
tests/sqlite_tcl/run.sh /tmp/sqlite-tcl build_mock/hpplite.dylib insert update trigger1 fkey2 ...
```

`run.sh` builds a stock `testfixture` from this tree the first time (Tcl
with `tclConfig.sh`: `TCL_CONFIG_DIR`). `wrap.tcl` loads the HPPLite module
into the test process. `tester.tcl` clears automatic extensions as it
starts, so the wrapper loads HPPLite again right after. It also keeps the
harness's "drop every table" helper off HPPLite's own tables, which HPPLite
refuses to drop. A few tests shut SQLite down mid-file; HPPLite is off from
there on in those.

## Results (2026-10-31; 66 files, the parts row capture touches)

31 of 66 files run with the same result as stock SQLite: `conflict`,
`conflict2`, `conflict3`, `delete2`-`4`, `fkey3`, `fkey4`, `fkey6`,
`lastinsert`, `returning1`, `savepoint2`, `trigger2`-`6`, `trigger8`,
`trigger9`, `triggerB`, `update2`, `upsert1`-`5`, `vacuum4`,
`without_rowid2`, `without_rowid4`-`6`. 29 differ, and 6 stop part way
(`alter`, `alter2`, `altercol`, `insert3`: their own cleanup drops every
table, HPPLite's too, which it refuses; `delete`, `alterdropcol`: a
database deleted by its main file alone, below).

What fails in the rest, by cause:

- **HPPLite's own tables and triggers** (most of them): tests that list the
  schema (`SELECT name FROM sqlite_master`), check a table's root page, or
  count pages and file sizes see `_hpplite_*` tables and the capture
  triggers. `alter3`, `trigger1-6`, `savepoint-10`, `fkey7`,
  `without_rowid1-1.50`, `insert-5.4`, `vacuum*`, `autoinc`.
- **SQLite's transfer optimization is skipped** (`insert4`): `INSERT INTO t
  SELECT * FROM s` copies pages directly only when `t` has no triggers, and
  the capture triggers count. The result is the same, but a whole-table copy
  (a migration's) runs row by row.
- **Refused by design**: ATTACH (the `aux`/`temp` sections of `fkey2` and
  `without_rowid3`); leaving WAL (`changes.test` sets `journal_mode = off`);
  dropping HPPLite's tables; non-deterministic SQL in a statement still
  logged as text (an `ALTER TABLE ADD COLUMN ... DEFAULT CURRENT_TIME`,
  which SQLite rejects anyway, now reported as HPPLite's refusal).
- **Injected allocation failures** (`triggerA-3`, malloc tests): an
  out-of-memory inside HPPLite's own checks refuses the statement - the safe
  outcome (its message used to advise binding values; it no longer does).
- **A database deleted by its main file alone** (`delete`, `alterdropcol`,
  `savepoint`): HPPLite keeps files in WAL, so a `-wal` file (and HPPLite's
  `-hpplite*` sidecars) outlive the main file, and the new database the test
  then creates meets the old WAL. Deleting a database means deleting its
  `-wal`, `-shm` and `-hpplite*` files too.
- **Not reproduced outside the suite**: `fkey-2.14.3.3` (a DROP TABLE that
  must fail a foreign key check succeeded). The same statements with HPPLite
  fail correctly in a fresh database, in both capture modes; in the suite
  they follow refused ATTACH sections.

## Found and fixed by this run

- `PRAGMA count_changes=1` made every write refused (HPPLite's own writes
  returned a row), a v0.1 bug too: `conflict`, `conflict2` and `upsert1` went
  from failing to clean.
- Leaving WAL failed as "database is locked"; it is refused with the reason.
- A read-only connection failed to open; HPPLite stands aside for it.
