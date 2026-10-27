/*
** HPPLite - Verifiable SQLite
**
** Tracks every write made through a connection, batches the SQL, and posts
** the batches to an L1 contract for data availability. Anyone can rebuild
** the database from L1 alone and check the resulting state root.
**
** The local database file is the only durable state HPPLite keeps:
**
**   _hpplite_pending  SQL captured by the statement trace. The row is written
**                     from inside the user's own statement, so it commits or
**                     rolls back together with the user's transaction. Rows
**                     are deleted once the batch they belong to is confirmed.
**   _hpplite_meta     Next batch height, previous block hash, the state root
**                     after the last confirmed batch, and checkpoint tracking.
**
** The state root is a content hash of all user tables (schema + rows). It is a
** function of the file, not of the path taken to produce it, so it can be
** recomputed at any time and compared against what was posted to L1.
*/

#ifndef HPPLITE_H
#define HPPLITE_H

#include "hpplite_sqlite.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** State root is a 32-byte hash representing database state
*/
#define HPPLITE_HASH_SIZE 32

/* Forward declaration for batch */
struct HppliteBatch;

/*
** HPPLite context attached to a database connection
*/
typedef struct HppliteCtx HppliteCtx;
struct HppliteCtx {
  sqlite3 *db;                /* Application connection: trace + pending log */
  sqlite3 *bkDb;              /* Bookkeeping connection (== db for :memory:) */
  int bEnabled;               /* Tracking enabled flag */
  int bInternal;              /* Set while HPPLite runs its own statements */
  uint64_t txnSeq;            /* Groups pending SQL by transaction */
  sqlite3_stmt *pLogStmt;     /* Cached INSERT INTO _hpplite_pending */

  /* Mirror of _hpplite_meta (the row in the database is authoritative) */
  uint64_t blockHeight;       /* Next batch height to produce */
  unsigned char prevBlockHash[HPPLITE_HASH_SIZE];
  unsigned char stateRoot[HPPLITE_HASH_SIZE];  /* Root after last confirmed batch */
  uint64_t lastCheckpointHeight;
  unsigned char lastCheckpointRoot[HPPLITE_HASH_SIZE];

  /* Cached content hash of the committed state */
  unsigned char curRoot[HPPLITE_HASH_SIZE];
  int bRootDirty;

  uint64_t nLogErrors;        /* Statements that could not be logged */

  /* Callbacks for external integration */
  void *pCallbackArg;
  void (*xOnCommit)(void*, unsigned char*, int);  /* root is NULL; use hpplite_get_state_root() */
  void (*xOnRollback)(void*);
};

/*
** Initialize HPPLite tracking on a database connection.
** Creates _hpplite_meta / _hpplite_pending if missing and installs hooks.
** Returns SQLITE_OK on success.
*/
int hpplite_init(sqlite3 *db, HppliteCtx **ppCtx);

/*
** Shutdown HPPLite tracking and free resources.
** Does not close db or the bookkeeping connection.
*/
void hpplite_shutdown(HppliteCtx *pCtx);

/*
** Use a second connection to the same file for bookkeeping (reading pending
** rows, computing roots, finalizing batches). This keeps the flush path off
** the application's connection, so a background flush never interferes with
** the application's transaction. Pass NULL to fall back to the main connection.
*/
int hpplite_set_bookkeeping_db(HppliteCtx *pCtx, sqlite3 *bkDb);

/*
** Enable/disable tracking.
*/
void hpplite_enable(HppliteCtx *pCtx, int bEnable);

/*
** Current state root (32 bytes): content hash of committed user tables.
** All zeros for a database with no user schema.
*/
void hpplite_get_state_root(HppliteCtx *pCtx, unsigned char *pOut);

/*
** Content hash of any connection's main database, same definition as the
** state root. Excludes sqlite_* and _hpplite_* tables.
*/
int hpplite_db_hash(sqlite3 *db, unsigned char *pOut);

/*
** SHA-256 helper (used for state roots, block hashes, batch identity).
*/
void hpplite_sha256(const uint8_t *data, size_t len, uint8_t *out);

/*
** Set callbacks for commit/rollback notifications.
*/
void hpplite_set_callbacks(
  HppliteCtx *pCtx,
  void *pArg,
  void (*xOnCommit)(void*, unsigned char*, int),
  void (*xOnRollback)(void*)
);

/*
** ============================================================================
** Block Builder API (Sequencer-style)
** ============================================================================
*/

/*
** Execute SQL (tracked like any other statement on the connection).
*/
int hpplite_exec(
  HppliteCtx *pCtx,
  const char *zSql,
  char **pzErrMsg
);

/*
** Execute SQL with callback (like sqlite3_exec).
*/
int hpplite_exec_cb(
  HppliteCtx *pCtx,
  const char *zSql,
  int (*xCallback)(void*, int, char**, char**),
  void *pArg,
  char **pzErrMsg
);

/*
** Build a batch from the committed pending SQL, with the content-hash state
** root. Has no side effects: the pending rows stay until
** hpplite_finalize_batch(). *pMaxId receives the id of the last pending row
** included. Returns NULL if nothing is pending.
*/
struct HppliteBatch *hpplite_build_batch(HppliteCtx *pCtx, sqlite3_int64 *pMaxId);

/*
** Like hpplite_build_batch() but keeps the serialized batch within maxBytes
** (0 = unlimited), cutting at a transaction boundary when possible, and
** computes the state root only when withRoot is set. Computing the root is
** O(database size), so the node asks for it only on the batch that closes a
** checkpoint interval. When pending SQL remains after the cut the batch is
** marked partial and carries no root (the last batch of the series does). A
** single statement that cannot fit on its own is left pending and reported
** (returns NULL, nLogErrors++).
*/
struct HppliteBatch *hpplite_build_batch_limited(HppliteCtx *pCtx, size_t maxBytes,
                                                 int withRoot, sqlite3_int64 *pMaxId);

/*
** Total bytes of pending SQL text (cheap size estimate for early flushing).
*/
sqlite3_int64 hpplite_pending_bytes(HppliteCtx *pCtx);

/*
** Mark a batch confirmed: delete its pending rows (id <= maxId) and advance
** height / previous block hash in _hpplite_meta, atomically. The recorded
** state root advances only when the batch carries one.
*/
int hpplite_finalize_batch(HppliteCtx *pCtx, const struct HppliteBatch *pBatch, sqlite3_int64 maxId);

/*
** Build and immediately finalize a batch (no L1). Returns NULL if nothing
** is pending. Caller must free returned batch with hpplite_batch_free().
*/
struct HppliteBatch *hpplite_flush_block(HppliteCtx *pCtx);

/*
** Apply a batch fetched from L1 with tracking disabled, then advance meta.
** The batch height must equal the current block height. With useTxn, each
** transaction group in the batch is applied in its own transaction.
** Returns SQLITE_OK, SQLITE_DONE if already applied, or an error code.
*/
int hpplite_replay_batch(HppliteCtx *pCtx, const struct HppliteBatch *pBatch, int useTxn);

/*
** A batch at the current height already exists on L1 and was produced from
** this file's pending rows (e.g. the process died after submitting but before
** finalizing). If the batch's SQL matches the first pending rows, finalize
** them. Returns SQLITE_OK on adoption, SQLITE_MISMATCH if the batch was not
** produced from these pending rows.
*/
int hpplite_adopt_batch(HppliteCtx *pCtx, const struct HppliteBatch *pBatch);

/*
** Number of committed pending SQL statements.
*/
int hpplite_pending_count(HppliteCtx *pCtx);

/*
** Next block height to produce.
*/
uint64_t hpplite_get_block_height(HppliteCtx *pCtx);

/*
** Hash of the previous block.
*/
void hpplite_get_prev_block_hash(HppliteCtx *pCtx, unsigned char *pOut);

/*
** Block hash: sha256(height || prevHash || [stateRoot] || sql...)
*/
void hpplite_block_hash(const struct HppliteBatch *pBatch, const unsigned char *pPrevHash, unsigned char *pOut);

/*
** Persist checkpoint progress.
*/
int hpplite_set_checkpoint(HppliteCtx *pCtx, uint64_t height, const unsigned char *pRoot);

/*
** Register HPPLite as a loadable extension.
*/
#ifdef HPPLITE_EXTENSION
int sqlite3_hpplite_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
);
#endif

/*
** ============================================================================
** Simple Open API (M3 - URI-based configuration)
** ============================================================================
**
** Usage:
**   sqlite3 *db = hpplite_open("file:state.db?hpplite=on&l1=hpp-sepolia&privkey=0x...");
**
** URI Parameters:
**   hpplite=on        Enable HPPLite (required)
**   l1=hpp-sepolia    L1 network alias (sets rpc + factory)
**   rpc=https://...   L1 RPC endpoint
**   factory=0x...     Factory contract (auto-creates a rollup per wallet)
**   contract=0x...    Existing rollup contract
**   privkey=0x...     Private key (or keyfile=/path)
**   interval=5s       Batch interval (default: 5s)
**   datadir=/path     Data directory
**
** Environment variables (lower priority than URI):
**   HPPLITE_PRIVATE_KEY, HPPLITE_L1_RPC, HPPLITE_FACTORY_ADDRESS, etc. (see config.h)
**
** On open HPPLite switches a file database to WAL mode and sets a 5 s busy
** timeout, so bookkeeping on the second connection never blocks the
** application and vice versa.
*/

/* Forward declaration */
struct HppliteConfig;

/*
** Open database with HPPLite enabled via URI parameters.
** Returns sqlite3* handle or NULL on error.
**
** The returned handle works like normal sqlite3* - just use sqlite3_exec(), etc.
** Batches are created automatically based on the interval setting.
**
** When done, either sqlite3_close(db) or hpplite_close(db) will work.
** The close hook automatically flushes pending batches and cleans up.
*/
sqlite3 *hpplite_open(const char *uri);

/*
** Close HPPLite database.
** This is equivalent to sqlite3_close() when HPPLite close hook is registered.
** Kept for API compatibility.
*/
int hpplite_close(sqlite3 *db);

/*
** Get HPPLite context from an open database.
** Returns NULL if HPPLite is not active on the connection.
*/
HppliteCtx *hpplite_context(sqlite3 *db);

/*
** Get the configuration for an open database.
** Returns NULL if database was not opened with hpplite_open().
*/
struct HppliteConfig *hpplite_get_config(sqlite3 *db);

/* Forward declaration for L1 interface */
struct HppliteL1;
typedef struct HppliteL1 HppliteL1;

/*
** Get the L1 connection for an open database.
** Returns NULL if L1 was not configured or the connection failed.
*/
HppliteL1 *hpplite_get_l1(sqlite3 *db);

/*
** Get the node for an open database.
** Returns NULL if db is not an HPPLite database.
*/
struct HppliteNode *hpplite_get_node(sqlite3 *db);

/*
** Register a node with the global registry and set up SQL functions.
** Called automatically by hpplite_node_create() and hpplite_node_create_with_db().
*/
void hpplite_register_node(sqlite3 *db, struct HppliteNode *node);

/*
** Remove a connection from the registry (no-op if not registered).
** hpplite_node_destroy() calls this before closing a database it owns, so
** the close does not re-enter the node teardown.
*/
void hpplite_unregister_node(sqlite3 *db);

/*
** Force flush pending changes to a batch immediately.
** Returns batch height, or 0 if nothing was pending or the batch could not
** be confirmed on L1 (it stays pending and is retried).
*/
uint64_t hpplite_flush(sqlite3 *db);

/*
** Same as hpplite_flush(); kept for API compatibility.
*/
uint64_t hpplite_check_flush(sqlite3 *db);

/*
** Get current state root.
*/
void hpplite_state_root(sqlite3 *db, unsigned char *out);

/*
** DEPRECATED: HPPLite auto-registers via a constructor when the library is
** loaded. These functions are retained for backwards compatibility and do
** nothing.
*/
void hpplite_register(void);
void hpplite_unregister(void);

/*
** Pubkey size for signatures
*/
#define HPPLITE_PUBKEY_SIZE 33

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_H */
