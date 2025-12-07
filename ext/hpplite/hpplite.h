/*
** HPPLite - State Root Tracking for SQLite
**
** This extension provides deterministic state root computation
** for SQLite databases, enabling L2 blockchain integration.
*/

#ifndef HPPLITE_H
#define HPPLITE_H

#include "sqlite3.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** State root is a 32-byte hash representing database state
*/
#define HPPLITE_HASH_SIZE 32

/*
** Row change types
*/
#define HPPLITE_INSERT 1
#define HPPLITE_UPDATE 2
#define HPPLITE_DELETE 3

/*
** A single row change tracked during a transaction
*/
typedef struct HppliteChange HppliteChange;
struct HppliteChange {
  HppliteChange *pNext;      /* Next change in list */
  int op;                     /* HPPLITE_INSERT, UPDATE, or DELETE */
  char *zDb;                  /* Database name */
  char *zTable;               /* Table name */
  sqlite3_int64 iRowid;       /* Rowid of affected row */
  unsigned char *pOldData;    /* Serialized old row (UPDATE/DELETE) */
  int nOldData;               /* Size of old data */
  unsigned char *pNewData;    /* Serialized new row (INSERT/UPDATE) */
  int nNewData;               /* Size of new data */
};

/*
** Pending SQL transaction in the mempool
*/
typedef struct HpplitePendingSql HpplitePendingSql;
struct HpplitePendingSql {
  HpplitePendingSql *pNext;   /* Next in list */
  char *zSql;                 /* SQL statement */
  int nRowsAffected;          /* Rows affected by this statement */
};

/*
** HPPLite context attached to a database connection
*/
typedef struct HppliteCtx HppliteCtx;
struct HppliteCtx {
  sqlite3 *db;                /* Database connection */
  HppliteChange *pChanges;    /* List of changes in current transaction */
  HppliteChange *pLast;       /* Last change in list */
  int nChanges;               /* Number of changes */
  unsigned char stateRoot[HPPLITE_HASH_SIZE];  /* Current state root */
  int bEnabled;               /* Tracking enabled flag */

  /* Block building state */
  uint64_t blockHeight;       /* Current block height (next block to produce) */
  unsigned char prevBlockHash[HPPLITE_HASH_SIZE]; /* Hash of previous block */
  unsigned char preStateRoot[HPPLITE_HASH_SIZE];  /* State root at block start */
  HpplitePendingSql *pPending;  /* Pending SQL (mempool) */
  HpplitePendingSql *pPendingLast; /* Last pending SQL */
  int nPending;               /* Number of pending SQL statements */
  int bInBlock;               /* Currently building a block */

  /* Callbacks for external integration */
  void *pCallbackArg;
  void (*xOnCommit)(void*, unsigned char*, int);  /* Called on commit with new state root */
  void (*xOnRollback)(void*);                     /* Called on rollback */
};

/*
** Initialize HPPLite tracking on a database connection.
** Returns SQLITE_OK on success.
*/
int hpplite_init(sqlite3 *db, HppliteCtx **ppCtx);

/*
** Shutdown HPPLite tracking and free resources.
*/
void hpplite_shutdown(HppliteCtx *pCtx);

/*
** Enable/disable state tracking.
*/
void hpplite_enable(HppliteCtx *pCtx, int bEnable);

/*
** Get current state root (32 bytes).
*/
void hpplite_get_state_root(HppliteCtx *pCtx, unsigned char *pOut);

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
** Get list of changes in current transaction.
** Returns number of changes.
*/
int hpplite_get_changes(HppliteCtx *pCtx, HppliteChange **ppChanges);

/*
** Compute state root from a list of changes.
** This is a pure function for testing/verification.
*/
void hpplite_compute_state_root(
  unsigned char *pPrevRoot,    /* Previous state root (32 bytes) */
  HppliteChange *pChanges,     /* List of changes */
  unsigned char *pNewRoot      /* Output: new state root (32 bytes) */
);

/*
** Serialize the current transaction changes to a buffer.
** Caller must free the returned buffer with sqlite3_free().
*/
unsigned char *hpplite_serialize_changes(
  HppliteCtx *pCtx,
  int *pnSize                  /* Output: size of serialized data */
);

/*
** ============================================================================
** Block Builder API (Sequencer-style)
** ============================================================================
*/

/* Forward declaration for batch */
struct HppliteBatch;

/*
** Execute SQL and track it for the current block.
** This is the main entry point for sequencers.
** Returns SQLITE_OK on success, error code on failure.
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
** Flush pending SQL into a batch (creates a block).
** Returns NULL if no pending transactions.
** Caller must free returned batch with hpplite_batch_free().
*/
struct HppliteBatch *hpplite_flush_block(HppliteCtx *pCtx);

/*
** Get number of pending transactions in mempool.
*/
int hpplite_pending_count(HppliteCtx *pCtx);

/*
** Get current block height.
*/
uint64_t hpplite_get_block_height(HppliteCtx *pCtx);

/*
** Get previous block hash.
*/
void hpplite_get_prev_block_hash(HppliteCtx *pCtx, unsigned char *pOut);

/*
** Discard all pending transactions (rollback mempool).
*/
void hpplite_discard_pending(HppliteCtx *pCtx);

/*
** Finalize a verified batch (for witnesses).
** Increments block height and clears pending SQL without creating a batch.
** Used after successfully verifying a batch from the sequencer.
*/
void hpplite_finalize_verified_batch(HppliteCtx *pCtx);

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

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_H */
