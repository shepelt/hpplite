/*
** HPPLite - State Root Tracking for SQLite
**
** Implementation of state tracking via preupdate hooks.
*/

#include "hpplite.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>

/*
** Simple SHA-256 implementation for state root computation.
** In production, use a proper crypto library.
*/

/* SHA-256 constants */
static const uint32_t K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
  0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
  0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
  0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
  0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
  0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z) (((x)&(y))^((~(x))&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x) (ROTR(x,2)^ROTR(x,13)^ROTR(x,22))
#define EP1(x) (ROTR(x,6)^ROTR(x,11)^ROTR(x,25))
#define SIG0(x) (ROTR(x,7)^ROTR(x,18)^((x)>>3))
#define SIG1(x) (ROTR(x,17)^ROTR(x,19)^((x)>>10))

typedef struct {
  uint8_t data[64];
  uint32_t datalen;
  uint64_t bitlen;
  uint32_t state[8];
} SHA256_CTX;

static void sha256_init(SHA256_CTX *ctx) {
  ctx->datalen = 0;
  ctx->bitlen = 0;
  ctx->state[0] = 0x6a09e667;
  ctx->state[1] = 0xbb67ae85;
  ctx->state[2] = 0x3c6ef372;
  ctx->state[3] = 0xa54ff53a;
  ctx->state[4] = 0x510e527f;
  ctx->state[5] = 0x9b05688c;
  ctx->state[6] = 0x1f83d9ab;
  ctx->state[7] = 0x5be0cd19;
}

static void sha256_transform(SHA256_CTX *ctx, const uint8_t *data) {
  uint32_t a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];

  for (i = 0, j = 0; i < 16; ++i, j += 4)
    m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
           ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
  for (; i < 64; ++i)
    m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];

  a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
  e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

  for (i = 0; i < 64; ++i) {
    t1 = h + EP1(e) + CH(e, f, g) + K[i] + m[i];
    t2 = EP0(a) + MAJ(a, b, c);
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }

  ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
  ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len) {
  uint32_t i;
  for (i = 0; i < len; ++i) {
    ctx->data[ctx->datalen] = data[i];
    ctx->datalen++;
    if (ctx->datalen == 64) {
      sha256_transform(ctx, ctx->data);
      ctx->bitlen += 512;
      ctx->datalen = 0;
    }
  }
}

static void sha256_final(SHA256_CTX *ctx, uint8_t *hash) {
  uint32_t i = ctx->datalen;

  if (ctx->datalen < 56) {
    ctx->data[i++] = 0x80;
    while (i < 56) ctx->data[i++] = 0x00;
  } else {
    ctx->data[i++] = 0x80;
    while (i < 64) ctx->data[i++] = 0x00;
    sha256_transform(ctx, ctx->data);
    memset(ctx->data, 0, 56);
  }

  ctx->bitlen += ctx->datalen * 8;
  ctx->data[63] = (uint8_t)(ctx->bitlen);
  ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
  ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
  ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
  ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
  ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
  ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
  ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
  sha256_transform(ctx, ctx->data);

  for (i = 0; i < 8; ++i) {
    hash[i * 4] = (ctx->state[i] >> 24) & 0xff;
    hash[i * 4 + 1] = (ctx->state[i] >> 16) & 0xff;
    hash[i * 4 + 2] = (ctx->state[i] >> 8) & 0xff;
    hash[i * 4 + 3] = ctx->state[i] & 0xff;
  }
}

/*
** Helper: compute SHA256 of data
*/
static void sha256(const uint8_t *data, size_t len, uint8_t *hash) {
  SHA256_CTX ctx;
  sha256_init(&ctx);
  sha256_update(&ctx, data, len);
  sha256_final(&ctx, hash);
}

/* Forward declaration */
static void freeAllPending(HppliteCtx *pCtx);

/*
** Free a change record
*/
static void freeChange(HppliteChange *p) {
  if (p) {
    sqlite3_free(p->zDb);
    sqlite3_free(p->zTable);
    sqlite3_free(p->pOldData);
    sqlite3_free(p->pNewData);
    sqlite3_free(p);
  }
}

/*
** Free all changes in a context
*/
static void freeAllChanges(HppliteCtx *pCtx) {
  HppliteChange *p = pCtx->pChanges;
  while (p) {
    HppliteChange *pNext = p->pNext;
    freeChange(p);
    p = pNext;
  }
  pCtx->pChanges = NULL;
  pCtx->pLast = NULL;
  pCtx->nChanges = 0;
}

/*
** Preupdate hook callback - captures row changes
*/
static void hpplitePreupdateHook(
  void *pArg,
  sqlite3 *db,
  int op,
  const char *zDb,
  const char *zTable,
  sqlite3_int64 iKey1,
  sqlite3_int64 iKey2
) {
  HppliteCtx *pCtx = (HppliteCtx*)pArg;
  HppliteChange *pChange;
  int nCol, i, rc;
  sqlite3_value *pVal;

  if (!pCtx->bEnabled) return;

  pChange = (HppliteChange*)sqlite3_malloc(sizeof(HppliteChange));
  if (!pChange) return;
  memset(pChange, 0, sizeof(HppliteChange));

  /* Set operation type */
  switch (op) {
    case SQLITE_INSERT: pChange->op = HPPLITE_INSERT; break;
    case SQLITE_UPDATE: pChange->op = HPPLITE_UPDATE; break;
    case SQLITE_DELETE: pChange->op = HPPLITE_DELETE; break;
    default:
      sqlite3_free(pChange);
      return;
  }

  /* Copy database and table names */
  pChange->zDb = sqlite3_mprintf("%s", zDb);
  pChange->zTable = sqlite3_mprintf("%s", zTable);

  /* Set rowid based on operation */
  pChange->iRowid = (op == SQLITE_DELETE) ? iKey1 : iKey2;

  /* Get column count */
  nCol = sqlite3_preupdate_count(db);

  /* Serialize old values for UPDATE/DELETE */
  if (op == SQLITE_UPDATE || op == SQLITE_DELETE) {
    /* Simple serialization: concatenate column values */
    sqlite3_str *pStr = sqlite3_str_new(db);
    for (i = 0; i < nCol; i++) {
      rc = sqlite3_preupdate_old(db, i, &pVal);
      if (rc == SQLITE_OK && pVal) {
        int type = sqlite3_value_type(pVal);
        sqlite3_str_appendf(pStr, "%d:", type);
        switch (type) {
          case SQLITE_INTEGER:
            sqlite3_str_appendf(pStr, "%lld|", sqlite3_value_int64(pVal));
            break;
          case SQLITE_FLOAT:
            sqlite3_str_appendf(pStr, "%g|", sqlite3_value_double(pVal));
            break;
          case SQLITE_TEXT:
            sqlite3_str_appendf(pStr, "%s|", sqlite3_value_text(pVal));
            break;
          case SQLITE_BLOB: {
            int n = sqlite3_value_bytes(pVal);
            sqlite3_str_appendf(pStr, "[%d bytes]|", n);
            break;
          }
          default:
            sqlite3_str_appendf(pStr, "NULL|");
        }
      }
    }
    pChange->nOldData = sqlite3_str_length(pStr);
    pChange->pOldData = (unsigned char*)sqlite3_str_finish(pStr);
  }

  /* Serialize new values for INSERT/UPDATE */
  if (op == SQLITE_INSERT || op == SQLITE_UPDATE) {
    sqlite3_str *pStr = sqlite3_str_new(db);
    for (i = 0; i < nCol; i++) {
      rc = sqlite3_preupdate_new(db, i, &pVal);
      if (rc == SQLITE_OK && pVal) {
        int type = sqlite3_value_type(pVal);
        sqlite3_str_appendf(pStr, "%d:", type);
        switch (type) {
          case SQLITE_INTEGER:
            sqlite3_str_appendf(pStr, "%lld|", sqlite3_value_int64(pVal));
            break;
          case SQLITE_FLOAT:
            sqlite3_str_appendf(pStr, "%g|", sqlite3_value_double(pVal));
            break;
          case SQLITE_TEXT:
            sqlite3_str_appendf(pStr, "%s|", sqlite3_value_text(pVal));
            break;
          case SQLITE_BLOB: {
            int n = sqlite3_value_bytes(pVal);
            sqlite3_str_appendf(pStr, "[%d bytes]|", n);
            break;
          }
          default:
            sqlite3_str_appendf(pStr, "NULL|");
        }
      }
    }
    pChange->nNewData = sqlite3_str_length(pStr);
    pChange->pNewData = (unsigned char*)sqlite3_str_finish(pStr);
  }

  /* Add to change list */
  pChange->pNext = NULL;
  if (pCtx->pLast) {
    pCtx->pLast->pNext = pChange;
  } else {
    pCtx->pChanges = pChange;
  }
  pCtx->pLast = pChange;
  pCtx->nChanges++;
}

/*
** Commit hook callback - compute new state root
*/
static int hppliteCommitHook(void *pArg) {
  HppliteCtx *pCtx = (HppliteCtx*)pArg;
  unsigned char newRoot[HPPLITE_HASH_SIZE];

  if (!pCtx->bEnabled || pCtx->nChanges == 0) {
    freeAllChanges(pCtx);
    return 0;  /* Allow commit */
  }

  /* Compute new state root */
  hpplite_compute_state_root(pCtx->stateRoot, pCtx->pChanges, newRoot);
  memcpy(pCtx->stateRoot, newRoot, HPPLITE_HASH_SIZE);

  /* Notify callback if registered */
  if (pCtx->xOnCommit) {
    pCtx->xOnCommit(pCtx->pCallbackArg, newRoot, pCtx->nChanges);
  }

  /* Clean up changes */
  freeAllChanges(pCtx);

  return 0;  /* Allow commit */
}

/*
** Rollback hook callback - discard changes
*/
static void hppliteRollbackHook(void *pArg) {
  HppliteCtx *pCtx = (HppliteCtx*)pArg;

  if (pCtx->xOnRollback) {
    pCtx->xOnRollback(pCtx->pCallbackArg);
  }

  freeAllChanges(pCtx);
}

/* Forward declaration */
static int addPendingSql(HppliteCtx *pCtx, const char *zSql, int nRows);

/*
** Trace callback to capture SQL statements for transparent API.
** This allows standard sqlite3_exec() calls to be captured for batching.
*/
static int hppliteTraceCallback(
    unsigned mask,
    void *pCtx,
    void *pStmt,
    void *pSql
) {
    (void)mask;
    HppliteCtx *ctx = (HppliteCtx*)pCtx;
    sqlite3_stmt *stmt = (sqlite3_stmt*)pStmt;
    const char *sql = (const char*)pSql;

    if (!ctx || !ctx->bEnabled || !sql) return 0;

    /* Skip internal SQLite statements, empty SQL, and read-only queries */
    if (sql[0] == '\0') return 0;
    if (strncmp(sql, "PRAGMA", 6) == 0) return 0;
    if (strncmp(sql, "pragma", 6) == 0) return 0;
    if (strncmp(sql, "SELECT", 6) == 0) return 0;
    if (strncmp(sql, "select", 6) == 0) return 0;
    if (strncmp(sql, "EXPLAIN", 7) == 0) return 0;
    if (strncmp(sql, "explain", 7) == 0) return 0;

    /* Save pre-state before first SQL in a batch */
    if (ctx->nPending == 0) {
        memcpy(ctx->preStateRoot, ctx->stateRoot, HPPLITE_HASH_SIZE);
    }

    /* Add SQL to pending list */
    addPendingSql(ctx, sql, -1);  /* -1 = rows affected unknown */

    (void)stmt;
    return 0;
}

/*
** Initialize HPPLite tracking on a database connection.
*/
int hpplite_init(sqlite3 *db, HppliteCtx **ppCtx) {
  HppliteCtx *pCtx;

  pCtx = (HppliteCtx*)sqlite3_malloc(sizeof(HppliteCtx));
  if (!pCtx) return SQLITE_NOMEM;
  memset(pCtx, 0, sizeof(HppliteCtx));

  pCtx->db = db;
  pCtx->bEnabled = 1;

  /* Initialize state root to zeros (genesis state) */
  memset(pCtx->stateRoot, 0, HPPLITE_HASH_SIZE);

  /* Initialize block builder state */
  pCtx->blockHeight = 1;  /* First block is height 1 */
  memset(pCtx->prevBlockHash, 0, HPPLITE_HASH_SIZE);  /* Genesis has zero prev hash */
  memset(pCtx->preStateRoot, 0, HPPLITE_HASH_SIZE);

  /* Register hooks */
#ifdef SQLITE_ENABLE_PREUPDATE_HOOK
  sqlite3_preupdate_hook(db, hpplitePreupdateHook, pCtx);
#endif
  sqlite3_commit_hook(db, hppliteCommitHook, pCtx);
  sqlite3_rollback_hook(db, hppliteRollbackHook, pCtx);

  /* Register trace callback to capture SQL for transparent API */
  sqlite3_trace_v2(db, SQLITE_TRACE_STMT, hppliteTraceCallback, pCtx);

  *ppCtx = pCtx;
  return SQLITE_OK;
}

/*
** Shutdown HPPLite tracking and free resources.
*/
void hpplite_shutdown(HppliteCtx *pCtx) {
  if (!pCtx) return;

  /* Unregister hooks */
#ifdef SQLITE_ENABLE_PREUPDATE_HOOK
  sqlite3_preupdate_hook(pCtx->db, NULL, NULL);
#endif
  sqlite3_commit_hook(pCtx->db, NULL, NULL);
  sqlite3_rollback_hook(pCtx->db, NULL, NULL);
  sqlite3_trace_v2(pCtx->db, 0, NULL, NULL);

  freeAllChanges(pCtx);
  freeAllPending(pCtx);
  sqlite3_free(pCtx);
}

/*
** Enable/disable state tracking.
*/
void hpplite_enable(HppliteCtx *pCtx, int bEnable) {
  if (pCtx) pCtx->bEnabled = bEnable;
}

/*
** Get current state root.
*/
void hpplite_get_state_root(HppliteCtx *pCtx, unsigned char *pOut) {
  if (pCtx && pOut) {
    memcpy(pOut, pCtx->stateRoot, HPPLITE_HASH_SIZE);
  }
}

/*
** Set callbacks for commit/rollback notifications.
*/
void hpplite_set_callbacks(
  HppliteCtx *pCtx,
  void *pArg,
  void (*xOnCommit)(void*, unsigned char*, int),
  void (*xOnRollback)(void*)
) {
  if (pCtx) {
    pCtx->pCallbackArg = pArg;
    pCtx->xOnCommit = xOnCommit;
    pCtx->xOnRollback = xOnRollback;
  }
}

/*
** Get list of changes in current transaction.
*/
int hpplite_get_changes(HppliteCtx *pCtx, HppliteChange **ppChanges) {
  if (pCtx) {
    if (ppChanges) *ppChanges = pCtx->pChanges;
    return pCtx->nChanges;
  }
  return 0;
}

/*
** Compute state root from a list of changes.
**
** Algorithm: hash(prev_root || hash(change1) || hash(change2) || ...)
** This is a simple incremental Merkle-like structure.
*/
void hpplite_compute_state_root(
  unsigned char *pPrevRoot,
  HppliteChange *pChanges,
  unsigned char *pNewRoot
) {
  SHA256_CTX ctx;
  HppliteChange *p;
  unsigned char changeHash[HPPLITE_HASH_SIZE];

  sha256_init(&ctx);

  /* Include previous state root */
  sha256_update(&ctx, pPrevRoot, HPPLITE_HASH_SIZE);

  /* Hash each change and include in state */
  for (p = pChanges; p; p = p->pNext) {
    SHA256_CTX changeCtx;
    sha256_init(&changeCtx);

    /* Hash: op | db | table | rowid | old_data | new_data */
    sha256_update(&changeCtx, (uint8_t*)&p->op, sizeof(p->op));
    if (p->zDb) sha256_update(&changeCtx, (uint8_t*)p->zDb, strlen(p->zDb));
    if (p->zTable) sha256_update(&changeCtx, (uint8_t*)p->zTable, strlen(p->zTable));
    sha256_update(&changeCtx, (uint8_t*)&p->iRowid, sizeof(p->iRowid));
    if (p->pOldData) sha256_update(&changeCtx, p->pOldData, p->nOldData);
    if (p->pNewData) sha256_update(&changeCtx, p->pNewData, p->nNewData);

    sha256_final(&changeCtx, changeHash);
    sha256_update(&ctx, changeHash, HPPLITE_HASH_SIZE);
  }

  sha256_final(&ctx, pNewRoot);
}

/*
** Serialize the current transaction changes to a buffer.
*/
unsigned char *hpplite_serialize_changes(HppliteCtx *pCtx, int *pnSize) {
  sqlite3_str *pStr;
  HppliteChange *p;

  if (!pCtx || !pCtx->pChanges) {
    if (pnSize) *pnSize = 0;
    return NULL;
  }

  pStr = sqlite3_str_new(pCtx->db);

  /* Simple JSON-like serialization */
  sqlite3_str_appendf(pStr, "[");
  for (p = pCtx->pChanges; p; p = p->pNext) {
    if (p != pCtx->pChanges) sqlite3_str_appendf(pStr, ",");
    sqlite3_str_appendf(pStr,
      "{\"op\":%d,\"db\":\"%s\",\"table\":\"%s\",\"rowid\":%lld",
      p->op, p->zDb ? p->zDb : "", p->zTable ? p->zTable : "", p->iRowid
    );
    if (p->pOldData) {
      sqlite3_str_appendf(pStr, ",\"old\":\"%s\"", p->pOldData);
    }
    if (p->pNewData) {
      sqlite3_str_appendf(pStr, ",\"new\":\"%s\"", p->pNewData);
    }
    sqlite3_str_appendf(pStr, "}");
  }
  sqlite3_str_appendf(pStr, "]");

  if (pnSize) *pnSize = sqlite3_str_length(pStr);
  return (unsigned char*)sqlite3_str_finish(pStr);
}

/*
** ============================================================================
** Block Builder Implementation
** ============================================================================
*/

#include "batch.h"
#include <time.h>

/*
** Free a pending SQL record
*/
static void freePendingSql(HpplitePendingSql *p) {
  if (p) {
    sqlite3_free(p->zSql);
    sqlite3_free(p);
  }
}

/*
** Free all pending SQL in context
*/
static void freeAllPending(HppliteCtx *pCtx) {
  HpplitePendingSql *p = pCtx->pPending;
  while (p) {
    HpplitePendingSql *pNext = p->pNext;
    freePendingSql(p);
    p = pNext;
  }
  pCtx->pPending = NULL;
  pCtx->pPendingLast = NULL;
  pCtx->nPending = 0;
}

/*
** Add SQL to pending list
*/
static int addPendingSql(HppliteCtx *pCtx, const char *zSql, int nRows) {
  HpplitePendingSql *p;

  p = (HpplitePendingSql*)sqlite3_malloc(sizeof(HpplitePendingSql));
  if (!p) return SQLITE_NOMEM;

  memset(p, 0, sizeof(HpplitePendingSql));
  p->zSql = sqlite3_mprintf("%s", zSql);
  if (!p->zSql) {
    sqlite3_free(p);
    return SQLITE_NOMEM;
  }
  p->nRowsAffected = nRows;

  /* Add to list */
  p->pNext = NULL;
  if (pCtx->pPendingLast) {
    pCtx->pPendingLast->pNext = p;
  } else {
    pCtx->pPending = p;
  }
  pCtx->pPendingLast = p;
  pCtx->nPending++;

  return SQLITE_OK;
}

/*
** Execute SQL and track it for the current block.
*/
int hpplite_exec(
  HppliteCtx *pCtx,
  const char *zSql,
  char **pzErrMsg
) {
  return hpplite_exec_cb(pCtx, zSql, NULL, NULL, pzErrMsg);
}

/*
** Execute SQL with callback and track it.
*/
int hpplite_exec_cb(
  HppliteCtx *pCtx,
  const char *zSql,
  int (*xCallback)(void*, int, char**, char**),
  void *pArg,
  char **pzErrMsg
) {
  int rc;

  if (!pCtx || !zSql) return SQLITE_MISUSE;

  /*
  ** Execute the SQL.
  ** The trace callback (hppliteTraceCallback) automatically tracks
  ** write operations in the pending list for batching.
  ** No need to call addPendingSql() here - trace callback handles it.
  */
  rc = sqlite3_exec(pCtx->db, zSql, xCallback, pArg, pzErrMsg);

  return rc;
}

/*
** Flush pending SQL into a batch (creates a block).
*/
struct HppliteBatch *hpplite_flush_block(HppliteCtx *pCtx) {
  HppliteBatch *pBatch;
  HpplitePendingSql *p;
  unsigned char blockHash[HPPLITE_HASH_SIZE];
  SHA256_CTX hashCtx;

  if (!pCtx || pCtx->nPending == 0) {
    return NULL;
  }

  /* Create new batch */
  pBatch = hpplite_batch_new(pCtx->blockHeight);
  if (!pBatch) return NULL;

  /* Set pre-state (saved when first SQL was executed) */
  hpplite_batch_set_pre_state(pBatch, pCtx->preStateRoot);

  /* Set post-state (current state after all SQL executed) */
  hpplite_batch_set_post_state(pBatch, pCtx->stateRoot);

  /* Set timestamp */
  pBatch->timestamp = (uint64_t)time(NULL);

  /* Add all pending SQL to the batch */
  for (p = pCtx->pPending; p; p = p->pNext) {
    hpplite_batch_add_txn(pBatch, p->zSql, p->nRowsAffected);
  }

  /* Compute block hash: hash(height || prev_hash || pre_state || post_state || txns...) */
  sha256_init(&hashCtx);
  sha256_update(&hashCtx, (uint8_t*)&pBatch->height, sizeof(pBatch->height));
  sha256_update(&hashCtx, pCtx->prevBlockHash, HPPLITE_HASH_SIZE);
  sha256_update(&hashCtx, pBatch->preStateRoot, HPPLITE_HASH_SIZE);
  sha256_update(&hashCtx, pBatch->postStateRoot, HPPLITE_HASH_SIZE);
  for (p = pCtx->pPending; p; p = p->pNext) {
    sha256_update(&hashCtx, (uint8_t*)p->zSql, strlen(p->zSql));
  }
  sha256_final(&hashCtx, blockHash);

  /* Update context for next block */
  memcpy(pCtx->prevBlockHash, blockHash, HPPLITE_HASH_SIZE);
  pCtx->blockHeight++;

  /* Clear pending list */
  freeAllPending(pCtx);

  return pBatch;
}

/*
** Get number of pending transactions.
*/
int hpplite_pending_count(HppliteCtx *pCtx) {
  return pCtx ? pCtx->nPending : 0;
}

/*
** Get current block height.
*/
uint64_t hpplite_get_block_height(HppliteCtx *pCtx) {
  return pCtx ? pCtx->blockHeight : 0;
}

/*
** Get previous block hash.
*/
void hpplite_get_prev_block_hash(HppliteCtx *pCtx, unsigned char *pOut) {
  if (pCtx && pOut) {
    memcpy(pOut, pCtx->prevBlockHash, HPPLITE_HASH_SIZE);
  }
}

/*
** Discard all pending transactions.
*/
void hpplite_discard_pending(HppliteCtx *pCtx) {
  if (pCtx) {
    freeAllPending(pCtx);
  }
}

/*
** Finalize a verified batch (for observers).
** Increments block height and clears pending SQL without creating a batch.
** Used after successfully verifying a batch from the sequencer.
*/
void hpplite_finalize_verified_batch(HppliteCtx *pCtx) {
  if (!pCtx) return;

  /* Increment block height */
  pCtx->blockHeight++;

  /* Clear pending SQL (executed during verification) */
  freeAllPending(pCtx);

  /* Note: prevBlockHash would ideally be updated from the batch,
   * but for verification purposes, only the state root matters */
}

/*
** ============================================================================
** Simple Open API (M3)
** ============================================================================
*/

#include "config.h"
#include "crypto.h"
#include "batch.h"
#include "l1_interface.h"
#include "node.h"

/*
** Global registry mapping sqlite3* -> HppliteNode
** (Simple linked list for now, could use hash table for performance)
*/
typedef struct NodeEntry NodeEntry;
struct NodeEntry {
    sqlite3 *db;
    HppliteNode *node;
    HppliteConfig *config;  /* Stored for hpplite_get_config() */
    NodeEntry *next;
};

static NodeEntry *g_nodeRegistry = NULL;

static NodeEntry *findEntry(sqlite3 *db) {
    NodeEntry *e = g_nodeRegistry;
    while (e) {
        if (e->db == db) return e;
        e = e->next;
    }
    return NULL;
}

static HppliteNode *findNode(sqlite3 *db) {
    NodeEntry *e = findEntry(db);
    return e ? e->node : NULL;
}

static void removeNode(sqlite3 *db) {
    NodeEntry **pp = &g_nodeRegistry;
    while (*pp) {
        if ((*pp)->db == db) {
            NodeEntry *e = *pp;
            *pp = e->next;
            /* Note: node is destroyed separately via hpplite_node_destroy() */
            if (e->config) hpplite_config_free(e->config);
            free(e);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* Forward declarations for SQL functions */
static void hpplite_sync_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void hpplite_flush_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);

/*
** Register a node with the global registry and set up SQL functions.
** Called by hpplite_node_create() and hpplite_node_create_with_db().
*/
void hpplite_register_node(sqlite3 *db, HppliteNode *node) {
    if (!db || !node) return;

    /* Check if already registered */
    if (findEntry(db)) return;

    /* Add to registry */
    NodeEntry *entry = calloc(1, sizeof(NodeEntry));
    if (!entry) return;

    entry->db = db;
    entry->node = node;
    entry->config = NULL;  /* No config for direct node creation */
    entry->next = g_nodeRegistry;
    g_nodeRegistry = entry;

    /* Register SQL functions */
    sqlite3_create_function(db, "hpplite_sync", 0, SQLITE_UTF8, NULL,
                            hpplite_sync_func, NULL, NULL);
    sqlite3_create_function(db, "hpplite_flush", 0, SQLITE_UTF8, NULL,
                            hpplite_flush_func, NULL, NULL);
}

/*
** Close hook callback - called automatically by sqlite3_close()
** This performs all HPPLite cleanup before SQLite tears down the connection.
*/
static void hpplite_close_hook(void *pArg, sqlite3 *db) {
    (void)pArg;  /* Unused - we look up node by db */

    HppliteNode *node = findNode(db);
    if (!node) return;

    /*
    ** IMPORTANT: Set ownsDb to 0 because sqlite3_close() is already closing
    ** the database. We don't want hpplite_node_destroy() to try to close it
    ** again, which would cause a double-free/double-close bug.
    */
    node->ownsDb = 0;

    /* Destroy the node (stops timer, flushes pending, frees resources) */
    hpplite_node_destroy(node);

    /* Remove from registry */
    removeNode(db);
}

/*
** Helper: convert HppliteConfig to HppliteNodeConfig
*/
static void config_to_node_config(const HppliteConfig *cfg, HppliteNodeConfig *nc) {
    memset(nc, 0, sizeof(HppliteNodeConfig));
    nc->dataDir = cfg->dataDir ? strdup(cfg->dataDir) : NULL;
    nc->dbPath = cfg->dbPath ? strdup(cfg->dbPath) : NULL;
    nc->rpcUrl = cfg->rpcUrl ? strdup(cfg->rpcUrl) : NULL;
    memcpy(nc->privkey, cfg->privkey, 32);
    nc->hasPrivkey = cfg->hasPrivkey;
    memcpy(nc->factory, cfg->factory, 20);
    nc->hasFactory = cfg->hasFactory;
    memcpy(nc->contract, cfg->contract, 20);
    nc->hasContract = cfg->hasContract;
    nc->batchIntervalMs = cfg->batchIntervalMs;

    /* Role removed - always sequencer in simplified model */
}

/*
** Helper: free node config strings
*/
static void free_node_config(HppliteNodeConfig *nc) {
    free(nc->dataDir);
    free(nc->dbPath);
    free(nc->rpcUrl);
    free(nc->nodeId);
}

sqlite3 *hpplite_open(const char *uri) {
    if (!uri) return NULL;

    /* Load configuration */
    HppliteConfig *config = hpplite_config_load(uri);
    if (!config) return NULL;

    /* Check if hpplite is enabled */
    if (!config->dbPath) {
        hpplite_config_free(config);
        return NULL;
    }

    /* Convert to node config */
    HppliteNodeConfig nodeConfig;
    config_to_node_config(config, &nodeConfig);

    /* Create node (opens db internally) */
    HppliteNode *node = hpplite_node_create(&nodeConfig);
    free_node_config(&nodeConfig);

    if (!node) {
        hpplite_config_free(config);
        return NULL;
    }

    sqlite3 *db = node->db;

    /* Add to registry */
    NodeEntry *entry = calloc(1, sizeof(NodeEntry));
    if (!entry) {
        hpplite_config_free(config);
        hpplite_node_destroy(node);
        return NULL;
    }
    entry->db = db;
    entry->node = node;
    entry->config = config;  /* Store config for hpplite_get_config() */
    entry->next = g_nodeRegistry;
    g_nodeRegistry = entry;

    /* Register SQL functions */
    sqlite3_create_function(db, "hpplite_sync", 0, SQLITE_UTF8, NULL,
                            hpplite_sync_func, NULL, NULL);
    sqlite3_create_function(db, "hpplite_flush", 0, SQLITE_UTF8, NULL,
                            hpplite_flush_func, NULL, NULL);

    /* Register close hook for automatic cleanup */
    sqlite3_close_hook(db, hpplite_close_hook, NULL);

    /* Start auto-flush timer thread */
    hpplite_node_start_timer(node);

    /* Start node (claim lease if L1 connected) */
    hpplite_node_start(node);

    return db;
}

int hpplite_close(sqlite3 *db) {
    /*
    ** With the close hook registered, sqlite3_close() will automatically
    ** trigger hpplite_close_hook() which handles all cleanup.
    ** This function is kept for API compatibility.
    */
    return sqlite3_close(db);
}

HppliteCtx *hpplite_context(sqlite3 *db) {
    HppliteNode *node = findNode(db);
    return node ? node->ctx : NULL;
}

struct HppliteConfig *hpplite_get_config(sqlite3 *db) {
    NodeEntry *e = findEntry(db);
    return e ? e->config : NULL;
}

HppliteL1 *hpplite_get_l1(sqlite3 *db) {
    HppliteNode *node = findNode(db);
    return node ? node->l1 : NULL;
}

HppliteNode *hpplite_get_node(sqlite3 *db) {
    return findNode(db);
}

uint64_t hpplite_flush(sqlite3 *db) {
    HppliteNode *node = findNode(db);
    if (!node) return 0;
    /* Use the node's flush (sequencer path) */
    return hpplite_node_flush_batch(node);
}

/*
** Check if auto-flush is needed and perform it if so.
** Returns batch height if flushed, 0 otherwise.
** Note: With timer thread, this is less necessary but kept for compatibility.
*/
uint64_t hpplite_check_flush(sqlite3 *db) {
    HppliteNode *node = findNode(db);
    if (!node) return 0;
    /* The timer thread handles auto-flush; this is a manual trigger */
    return hpplite_node_flush_batch(node);
}

void hpplite_state_root(sqlite3 *db, unsigned char *out) {
    HppliteNode *node = findNode(db);
    if (node && node->ctx) {
        hpplite_get_state_root(node->ctx, out);
    } else {
        memset(out, 0, HPPLITE_HASH_SIZE);
    }
}

/*
** SQL function: hpplite_sync()
** Triggers sync from L1 for observer/replica nodes.
*/
static void hpplite_sync_func(
    sqlite3_context *ctx,
    int argc,
    sqlite3_value **argv
) {
    (void)argc;
    (void)argv;

    sqlite3 *db = sqlite3_context_db_handle(ctx);
    HppliteNode *node = findNode(db);

    if (!node) {
        sqlite3_result_int(ctx, 0);
        return;
    }

    if (!node->l1) {
        /* No L1 connection, nothing to sync */
        sqlite3_result_int(ctx, 0);
        return;
    }

    /* Get current L1 state */
    uint64_t lastBatch = 0, totalBatches = 0;
    unsigned char latestHash[32];
    if (hpplite_l1_get_da_state(node->l1, &lastBatch, &totalBatches, latestHash) != 0) {
        sqlite3_result_int(ctx, 0);
        return;
    }

    /* Replay batches we haven't seen yet */
    uint64_t currentHeight = node->ctx->blockHeight - 1;
    int synced = 0;

    for (uint64_t h = currentHeight + 1; h <= lastBatch; h++) {
        size_t dataLen = 0;
        uint8_t *data = hpplite_l1_get_batch(node->l1, h, &dataLen);
        if (data && dataLen > 0) {
            HppliteBatch *batch = hpplite_batch_from_json((const char *)data);
            if (batch) {
                for (int i = 0; i < batch->nTxns; i++) {
                    if (batch->aTxns[i].zSql) {
                        sqlite3_exec(node->db, batch->aTxns[i].zSql, NULL, NULL, NULL);
                    }
                }
                node->ctx->blockHeight = batch->height + 1;
                node->lastCheckpointHeight = batch->height;
                memcpy(node->lastCheckpointRoot, batch->postStateRoot, HPPLITE_HASH_SIZE);
                memcpy(node->lastFlushedRoot, batch->postStateRoot, HPPLITE_HASH_SIZE);
                hpplite_batch_free(batch);
                synced++;
            }
            free(data);
        }
    }

    sqlite3_result_int(ctx, synced);
}

/*
** SQL function: hpplite_flush()
** Triggers a batch flush for sequencer nodes.
*/
static void hpplite_flush_func(
    sqlite3_context *ctx,
    int argc,
    sqlite3_value **argv
) {
    (void)argc;
    (void)argv;

    sqlite3 *db = sqlite3_context_db_handle(ctx);
    HppliteNode *node = findNode(db);

    if (!node) {
        sqlite3_result_int64(ctx, 0);
        return;
    }

    uint64_t height = hpplite_node_flush_batch(node);
    sqlite3_result_int64(ctx, (sqlite3_int64)height);
}

/*
** Auto-extension callback - called automatically on every sqlite3_open()
** Checks URI parameters and initializes HPPLite if enabled.
** Uses HppliteNode for unified implementation.
*/
static int hpplite_auto_init(
    sqlite3 *db,
    char **pzErrMsg,
    const sqlite3_api_routines *pApi
) {
    (void)pzErrMsg;
    (void)pApi;

    /* Get the URI used to open this database */
    const char *filename = sqlite3_db_filename(db, "main");
    if (!filename) return SQLITE_OK;

    /* Check if HPPLite is enabled via URI parameter */
    const char *hppliteParam = sqlite3_uri_parameter(filename, "hpplite");
    if (!hppliteParam || strcmp(hppliteParam, "on") != 0) {
        /* HPPLite not enabled for this connection */
        return SQLITE_OK;
    }

    /* Load configuration from SQLite URI parameters */
    HppliteConfig *config = hpplite_config_create();
    if (!config) return SQLITE_OK;

    /* Load from env first */
    hpplite_config_load_env(config);

    /* Then override with SQLite URI parameters */
    hpplite_config_load_sqlite_uri(config, filename);

    /* Convert to node config */
    HppliteNodeConfig nodeConfig;
    config_to_node_config(config, &nodeConfig);

    /* Create node using existing db connection */
    HppliteNode *node = hpplite_node_create_with_db(db, &nodeConfig);
    free_node_config(&nodeConfig);

    if (!node) {
        hpplite_config_free(config);
        return SQLITE_OK;  /* Don't fail the open, just skip HPPLite */
    }

    /* Add to registry */
    NodeEntry *entry = calloc(1, sizeof(NodeEntry));
    if (!entry) {
        hpplite_config_free(config);
        hpplite_node_destroy(node);
        return SQLITE_OK;
    }
    entry->db = db;
    entry->node = node;
    entry->config = config;  /* Store config for hpplite_get_config() */
    entry->next = g_nodeRegistry;
    g_nodeRegistry = entry;

    /* Register SQL functions */
    sqlite3_create_function(db, "hpplite_sync", 0, SQLITE_UTF8, NULL,
                            hpplite_sync_func, NULL, NULL);
    sqlite3_create_function(db, "hpplite_flush", 0, SQLITE_UTF8, NULL,
                            hpplite_flush_func, NULL, NULL);

    /* Register close hook for automatic cleanup */
    sqlite3_close_hook(db, hpplite_close_hook, NULL);

    /* Start auto-flush timer thread */
    hpplite_node_start_timer(node);

    return SQLITE_OK;
}

/*
** Auto-register HPPLite when the library is loaded.
** This uses a constructor attribute so registration happens automatically
** before main() is called.
*/
__attribute__((constructor))
static void hpplite_auto_register(void) {
    sqlite3_auto_extension((void(*)(void))hpplite_auto_init);
}

/*
** Register HPPLite as an auto-extension.
**
** DEPRECATED: No longer needed. HPPLite auto-registers when the library
** is loaded via constructor. This function is retained for backwards
** compatibility but does nothing.
*/
void hpplite_register(void) {
    /* No-op: auto-registered via constructor */
}

/*
** Unregister HPPLite auto-extension.
**
** DEPRECATED: Retained for backwards compatibility but does nothing.
*/
void hpplite_unregister(void) {
    /* No-op */
}

/*
** Loadable extension entry point
*/
#ifdef HPPLITE_EXTENSION
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

int sqlite3_hpplite_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
) {
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;

  /* For loadable extension, user must call hpplite_init() manually */
  return SQLITE_OK;
}
#endif
