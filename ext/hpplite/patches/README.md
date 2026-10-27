# SQLite core patches: none

HPPLite runs on **unmodified SQLite** (any amalgamation >= 3.24). It is an
ordinary extension: statement trace, commit/rollback hooks, an eponymous
virtual table (`hpplite_status`, whose `xDisconnect` runs the shutdown inside
`sqlite3_close()`), `sqlite3_auto_extension`, and URI parameters.

Until issue #2 the branch carried one 60-line core commit adding
`sqlite3_close_hook()`; it was replaced by the virtual-table disconnect and
removed from the branch history. This directory now only records which
upstream release the `hpplite` branch sits on:

```
UPSTREAM_BASE   e.g. version-3.54.1
```

## Branch layout

```
version-3.54.1            <- upstream release tag (current base)
  Add HPPLite extension - Milestone 1 complete
  ...                     <- ext/hpplite/ only; never conflicts with upstream
```

## Moving to a new upstream release

```bash
ext/hpplite/tools/sync_upstream.sh version-3.55.0
```

## Building without this tree at all

```bash
cmake -S ext/hpplite -B build -DHPPLITE_SQLITE_SRC=/path/to/sqlite-amalgamation
cmake --build build          # -> build/hpplite.dylib (or .so)
sqlite3> .load ./build/hpplite
```
