# HPPLite Simple Open API Design

**Transparent SQLite Integration with Rollup Semantics**

## Overview

This document describes the architecture for making HPPLite work transparently with standard SQLite APIs. Users should be able to use `sqlite3_open()` with URI parameters and get full rollup functionality without learning new APIs.

```c
// Goal: This should just work
sqlite3 *db;
sqlite3_open_v2("file:app.db?hpplite=on&role=sequencer&l1=hpp-sepolia",
                &db, SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE, NULL);
sqlite3_exec(db, "INSERT INTO users VALUES(1, 'alice')", NULL, NULL, NULL);
sqlite3_close(db);  // Automatically flushes pending batch
```

## Architecture

### Hybrid Model: Minimal Fork + Extension

```
┌─────────────────────────────────────────────────────────────────┐
│                    SQLite Core (minimal fork)                    │
│  + sqlite3_close_hook()    (~20 lines added)                    │
│  + sqlite3_open_hook()     (optional, for auto-init)            │
└─────────────────────────────────────────────────────────────────┘
                              │
┌─────────────────────────────────────────────────────────────────┐
│                      HPPLite Extension                           │
│  - All rollup logic                                              │
│  - State tracking (preupdate_hook)                              │
│  - Batch creation (commit_hook)                                  │
│  - Timer-based flushing                                          │
│  - L1 posting                                                    │
│  - Config parsing                                                │
└─────────────────────────────────────────────────────────────────┘
```

**Benefits:**
- Core diff is tiny (~20-50 lines) - easy to maintain during upstream merges
- All HPPLite logic stays in extension
- Other extensions can also use the new hooks
- Clean separation of concerns

---

## SQLite Core Patch

### New Hook APIs

```c
// sqlite3.h additions

// Close hook - called before connection closes
typedef void (*sqlite3_close_hook_t)(sqlite3 *db, void *pArg);
int sqlite3_close_hook(sqlite3 *db, sqlite3_close_hook_t xCallback, void *pArg);

// Open hook - called after connection opens (global, like auto_extension)
typedef int (*sqlite3_open_hook_t)(sqlite3 *db, void *pArg);
int sqlite3_open_hook_register(sqlite3_open_hook_t xCallback, void *pArg);
int sqlite3_open_hook_unregister(sqlite3_open_hook_t xCallback);
```

### Implementation in sqlite3.c

```c
// struct sqlite3 addition
struct sqlite3 {
    // ... existing fields ...
    sqlite3_close_hook_t xCloseHook;
    void *pCloseHookArg;
};

// Close hook registration
int sqlite3_close_hook(sqlite3 *db, sqlite3_close_hook_t xCallback, void *pArg) {
    db->xCloseHook = xCallback;
    db->pCloseHookArg = pArg;
    return SQLITE_OK;
}

// In sqlite3_close() / sqlite3Close()
static int sqlite3Close(sqlite3 *db, int forceZombie) {
    // NEW: Call close hook before cleanup
    if (db->xCloseHook) {
        db->xCloseHook(db, db->pCloseHookArg);
    }
    // ... existing close logic ...
}
```

**Total diff: ~30 lines**

---

## Extension Architecture

### Initialization Flow

```
sqlite3_open_v2("file:db.sqlite?hpplite=on&role=sequencer", ...)
        │
        ▼
┌─────────────────────────────────────────────────────────────────┐
│  SQLite opens database normally                                  │
└───────────────────────────────┬─────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│  Auto-extension: hpplite_auto_init(db)                          │
│    1. Get URI: sqlite3_db_filename(db, "main")                  │
│    2. Check: sqlite3_uri_parameter(filename, "hpplite")         │
│    3. If "on":                                                  │
│       - Parse all URI params (role, privkey, chainid, etc.)     │
│       - Initialize HPPLite state                                │
│       - Register preupdate_hook (change tracking)               │
│       - Register commit_hook (state root + batch trigger)       │
│       - Register close_hook (cleanup + final flush)             │
│       - Start timer thread (time-based batching)                │
└─────────────────────────────────────────────────────────────────┘
```

### State Structure

```c
typedef struct HppliteState {
    sqlite3 *db;
    HppliteConfig *config;
    HppliteCtx *ctx;              // Core state tracking
    HppliteCrypto *crypto;
    HppliteKeypair keypair;
    HppliteStorage *storage;

    // Batching state
    int64_t lastFlushTime;
    int pendingChanges;
    int pendingBytes;
    unsigned char lastFlushedRoot[32];

    // Timer thread
    pthread_t timerThread;
    pthread_mutex_t flushMutex;
    volatile int timerRunning;

    // Linked list for global tracking
    struct HppliteState *next;
} HppliteState;
```

---

## Batch Triggering

### Three Triggers (Belt and Suspenders)

Batches are flushed when ANY of these conditions are met:

```
┌─────────────────────────────────────────────────────────────────┐
│                        BATCH TRIGGERS                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  1. SIZE TRIGGER (in commit_hook)                               │
│     if (pendingChanges >= maxBatchSize) flush();                │
│                                                                  │
│  2. TIME TRIGGER (in timer_thread)                              │
│     if (elapsed >= batchIntervalMs && hasPending) flush();      │
│                                                                  │
│  3. CLOSE TRIGGER (in close_hook)                               │
│     if (hasPending) flush();  // NEVER lose data                │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Why This Mirrors Rollup Behavior

Real L2 rollups batch transactions the same way:

| Trigger | Rollup Equivalent | Purpose |
|---------|-------------------|---------|
| Size | Block gas limit | Prevent oversized batches |
| Time | Block interval | Bound latency for users |
| Close | Graceful shutdown | Ensure data reaches L1 |

**No activity = No batches** - This is correct, not a limitation.

### Implementation

```c
// Commit hook - size trigger
static int on_commit(void *arg) {
    HppliteState *s = arg;

    s->pendingChanges++;

    // Size trigger
    if (s->pendingChanges >= s->config->maxBatchSize) {
        do_flush(s);
    }
    // Time trigger also checked here for efficiency
    else if (current_time_ms() - s->lastFlushTime >= s->config->batchIntervalMs) {
        do_flush(s);
    }

    return 0;  // Allow commit
}

// Timer thread - backup time trigger
static void *timer_thread_func(void *arg) {
    HppliteState *s = arg;
    int sleepMs = 100;  // Check every 100ms

    while (s->timerRunning) {
        usleep(sleepMs * 1000);
        if (!s->timerRunning) break;

        pthread_mutex_lock(&s->flushMutex);
        int64_t elapsed = current_time_ms() - s->lastFlushTime;
        if (elapsed >= s->config->batchIntervalMs && has_pending(s)) {
            do_flush_locked(s);
        }
        pthread_mutex_unlock(&s->flushMutex);
    }
    return NULL;
}

// Close hook - final flush
static void on_close(sqlite3 *db, void *arg) {
    HppliteState *s = arg;

    // Stop timer
    s->timerRunning = 0;
    pthread_join(s->timerThread, NULL);

    // Final flush
    pthread_mutex_lock(&s->flushMutex);
    if (has_pending(s)) {
        do_flush_locked(s);
    }
    pthread_mutex_unlock(&s->flushMutex);

    // Cleanup
    cleanup_state(s);
}
```

---

## Finality Model

### Finality Levels

```
┌─────────────┬─────────────────────────────────────────────────────┐
│ Level       │ Description                                         │
├─────────────┼─────────────────────────────────────────────────────┤
│ LOCAL       │ SQLite committed (durable on disk)                  │
│ BATCHED     │ Included in pending batch (in memory)               │
│ SOFT        │ Batch posted to L1 (tx pending)                     │
│ FINAL       │ L1 tx confirmed (immutable)                         │
└─────────────┴─────────────────────────────────────────────────────┘
```

### Batch Finality (Default)

Multiple SQL commits are grouped into batches:

```
Commit 1 ─┐
Commit 2 ─┼─► Batch N ─► L1 Post ─► Finality
Commit 3 ─┘                            │
                                       ▼
                              All 3 commits final
```

**Configuration:**
```
file:db.sqlite?hpplite=on&batch_interval=5000&max_batch=100
                          ↑                   ↑
                     5 seconds            100 changes
```

### Immediate Finality (Optional, Expensive)

Each commit posts immediately to L1:

```
Commit 1 ─► L1 Post ─► Finality
Commit 2 ─► L1 Post ─► Finality
Commit 3 ─► L1 Post ─► Finality
```

**Configuration:**
```
file:db.sqlite?hpplite=on&finality=immediate
```

**Trade-offs:**

| Mode | Latency | L1 Cost | Use Case |
|------|---------|---------|----------|
| Batch (default) | 5s soft, L1 block hard | Low (amortized) | Most apps |
| Immediate | L1 block time (~12s) | High (per commit) | High-value txs |

---

## Crash Recovery

### The Problem

```
Commit 1 → pending
Commit 2 → pending
         💥 CRASH (before flush)
```

Local SQLite has commits, but L1 doesn't know about them.

### Solution: Pending Batch Table

Store pending batch data in SQLite itself:

```sql
-- Internal table (created by HPPLite)
CREATE TABLE _hpplite_pending (
    id INTEGER PRIMARY KEY,
    batch_height INTEGER,
    change_type TEXT,      -- INSERT/UPDATE/DELETE
    table_name TEXT,
    rowid INTEGER,
    old_data BLOB,
    new_data BLOB,
    created_at INTEGER
);

CREATE TABLE _hpplite_meta (
    key TEXT PRIMARY KEY,
    value BLOB
);
-- Stores: last_flush_time, last_flushed_root, batch_height, etc.
```

### Recovery Flow

```c
static int hpplite_auto_init(sqlite3 *db, ...) {
    // ... parse config ...

    // Check for pending data from previous session
    if (has_pending_batch(db)) {
        // Reconstruct batch from _hpplite_pending
        HppliteBatch *batch = recover_pending_batch(db);

        // Post to L1
        post_batch_to_l1(batch);

        // Clear pending table
        clear_pending_table(db);
    }

    // Continue normal initialization
    // ...
}
```

### Write-Ahead Pattern

```c
static int on_preupdate(void *arg, sqlite3 *db, int op, ...) {
    HppliteState *s = arg;

    // 1. Write to pending table FIRST (in same transaction)
    insert_pending_change(db, op, table, rowid, old_data, new_data);

    // 2. Track in memory for state root computation
    track_change(s->ctx, op, table, rowid, old_data, new_data);
}

static void do_flush(HppliteState *s) {
    // 1. Create and post batch
    post_batch_to_l1(s);

    // 2. Clear pending table (batch is now on L1)
    sqlite3_exec(s->db, "DELETE FROM _hpplite_pending", ...);

    // 3. Update meta
    update_meta(s->db, "last_flushed_root", s->currentRoot);
    update_meta(s->db, "last_flush_time", current_time_ms());
}
```

---

## Configuration

### URI Parameters

```
file:path/to/db.sqlite?param1=value1&param2=value2
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `hpplite` | bool | off | Enable HPPLite |
| `role` | string | follower | sequencer, witness, follower |
| `privkey` | hex | - | Private key (32 bytes hex) |
| `l1` | string | - | Network alias or chain ID |
| `rpc` | string | - | L1 RPC URL (overrides alias) |
| `contract` | address | - | L1 contract address |
| `datadir` | path | .hpplite | Batch storage directory |
| `batch_interval` | int | 5000 | Batch interval in ms |
| `max_batch` | int | 100 | Max changes per batch |
| `finality` | string | batch | batch or immediate |

### Network Aliases

```c
// Built-in aliases
"hpp-sepolia"  → chainId=181228, rpc="https://sepolia.hpp.io"
"hpp-mainnet"  → chainId=181227, rpc="https://rpc.hpp.io"
"sepolia"      → chainId=11155111, rpc="https://rpc.sepolia.org"
"ethereum"     → chainId=1, rpc="https://eth.llamarpc.com"
```

### Layered Configuration

Priority (highest to lowest):
1. URI parameters
2. Environment variables
3. Config file (~/.hpplite/config)
4. Built-in defaults

```c
// Example resolution
// URI: file:db.sqlite?hpplite=on&l1=hpp-sepolia
// Env: HPPLITE_PRIVKEY=0x123...
// Default: batch_interval=5000

// Result:
config->chainId = 181228;           // from l1=hpp-sepolia
config->rpcUrl = "https://...";     // from l1=hpp-sepolia
config->privkey = 0x123...;         // from env
config->batchIntervalMs = 5000;     // from default
```

---

## API Reference

### Public API

```c
// Registration (call once at app startup)
void hpplite_register(void);

// Then use standard SQLite API:
sqlite3_open_v2("file:db.sqlite?hpplite=on&...", &db, flags, NULL);
sqlite3_exec(db, sql, ...);
sqlite3_close(db);

// Optional: Access HPPLite internals
HppliteState *hpplite_get_state(sqlite3 *db);
HppliteConfig *hpplite_get_config(sqlite3 *db);
void hpplite_state_root(sqlite3 *db, unsigned char *out);
uint64_t hpplite_flush(sqlite3 *db);       // Force flush
uint64_t hpplite_batch_height(sqlite3 *db); // Current height
```

### Usage Example

```c
#include <sqlite3.h>
#include "hpplite.h"

int main() {
    // Register HPPLite (once)
    hpplite_register();

    // Open with HPPLite enabled
    sqlite3 *db;
    int rc = sqlite3_open_v2(
        "file:myapp.db?hpplite=on&role=sequencer&l1=hpp-sepolia",
        &db,
        SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        NULL
    );
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to open: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    // Use standard SQLite API
    sqlite3_exec(db, "CREATE TABLE users(id INT, name TEXT)", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO users VALUES(1, 'alice')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO users VALUES(2, 'bob')", NULL, NULL, NULL);

    // Batches are created automatically based on time/size triggers
    // Or force a flush:
    uint64_t height = hpplite_flush(db);
    printf("Flushed batch %llu\n", height);

    // Close - automatically flushes any pending batch
    sqlite3_close(db);

    return 0;
}
```

---

## Comparison with AergoLite

| Aspect | AergoLite | HPPLite |
|--------|-----------|---------|
| SQLite modification | Heavy (custom sqlite3.c) | Minimal (close hook only) |
| Consensus | VRF-based leader election | Single sequencer + witnesses |
| Finality | Block-level (distributed) | Batch-level (L1-anchored) |
| Network | P2P mesh | Sequencer → Witnesses |
| L1 dependency | None (self-contained) | Required (Ethereum-compatible) |
| Merge with upstream | Difficult | Easy (~30 line diff) |

### When to Use Which

**AergoLite:**
- Fully decentralized operation
- No L1 dependency
- IoT/edge scenarios with unreliable connectivity

**HPPLite:**
- L1 security guarantees needed
- Simpler operational model
- Integration with Ethereum ecosystem

---

## Implementation Checklist

### Phase 1: Core Hooks
- [ ] Add `sqlite3_close_hook()` to sqlite3.c
- [ ] Add `sqlite3_open_hook_register()` to sqlite3.c
- [ ] Test hooks work correctly

### Phase 2: Auto-Extension
- [ ] Implement `hpplite_auto_init()`
- [ ] Parse URI parameters
- [ ] Initialize HPPLite state
- [ ] Register all hooks

### Phase 3: Batching
- [ ] Implement timer thread
- [ ] Implement size trigger in commit_hook
- [ ] Implement close trigger in close_hook
- [ ] Thread-safe flush with mutex

### Phase 4: Crash Recovery
- [ ] Create `_hpplite_pending` table
- [ ] Write changes to pending table in preupdate_hook
- [ ] Recover pending batch on open
- [ ] Clear pending table after flush

### Phase 5: Testing
- [ ] Unit tests for each trigger
- [ ] Crash recovery test
- [ ] Multi-connection test
- [ ] Integration test with L1

---

## Appendix: Full Configuration Example

```bash
# Environment variables
export HPPLITE_PRIVKEY="0x1234567890abcdef..."
export HPPLITE_DATADIR="/var/lib/hpplite"

# Open database
sqlite3_open_v2(
    "file:/var/lib/myapp/data.db"
    "?hpplite=on"
    "&role=sequencer"
    "&l1=hpp-sepolia"
    "&contract=0x2Cd27d02C3b36c8926B849Bc53745Fab661572Dc"
    "&batch_interval=5000"
    "&max_batch=100"
    "&finality=batch",
    &db, flags, NULL
);
```

Equivalent to:
```c
HppliteConfig config = {
    .role = HPPLITE_ROLE_SEQUENCER,
    .chainId = 181228,
    .rpcUrl = "https://sepolia.hpp.io",
    .contract = 0x2Cd27...,
    .privkey = 0x1234...,  // from env
    .dataDir = "/var/lib/hpplite",  // from env
    .batchIntervalMs = 5000,
    .maxBatchSize = 100,
    .finality = HPPLITE_FINALITY_BATCH,
};
```
