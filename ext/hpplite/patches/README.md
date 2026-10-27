# SQLite core patches

HPPLite is an SQLite extension (`ext/hpplite/`) plus **one** change to SQLite core:

| Patch | Files | Purpose |
|-------|-------|---------|
| `0001-core-add-sqlite3_close_hook.patch` | `src/main.c`, `src/sqlite.h.in`, `src/sqliteInt.h` | `sqlite3_close_hook()` — callback at the start of `sqlite3Close()`, so the pending batch can be flushed to L1 while the connection is still usable |

Everything else HPPLite needs (`SQLITE_ENABLE_PREUPDATE_HOOK`, `sqlite3_auto_extension`, URI parameters) is stock SQLite.

## Branch layout

```
version-3.54.1            <- upstream release tag (current base)
  core: add sqlite3_close_hook()
  Add HPPLite extension - Milestone 1 complete
  ...                     <- ext/hpplite/ only, never conflicts
```

The core change is kept as the **bottom commit** so a rebase onto the next release has exactly one place that can conflict.

## Applying to a stock SQLite checkout

```bash
git clone https://github.com/sqlite/sqlite.git && cd sqlite
git checkout version-3.54.1
git am /path/to/hpplite/ext/hpplite/patches/0001-core-add-sqlite3_close_hook.patch
```

## Moving to a new upstream release

```bash
ext/hpplite/tools/sync_upstream.sh version-3.55.0
```

See `tools/sync_upstream.sh` for what it does. Goal (issue #2): remove this patch entirely so HPPLite builds against unmodified SQLite as a loadable extension.
