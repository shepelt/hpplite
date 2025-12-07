/*
** HPPLite - State Root Tracking for SQLite
**
** Implementation of state tracking via preupdate hooks.
*/

#include "hpplite.h"
#include <string.h>
#include <stdlib.h>

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
  int nRows;

  if (!pCtx || !zSql) return SQLITE_MISUSE;

  /* If this is the first SQL in a new block, save pre-state */
  if (pCtx->nPending == 0) {
    memcpy(pCtx->preStateRoot, pCtx->stateRoot, HPPLITE_HASH_SIZE);
  }

  /* Execute the SQL */
  rc = sqlite3_exec(pCtx->db, zSql, xCallback, pArg, pzErrMsg);
  if (rc != SQLITE_OK) {
    return rc;
  }

  /* Get rows affected */
  nRows = sqlite3_changes(pCtx->db);

  /* Track this SQL in the pending list */
  rc = addPendingSql(pCtx, zSql, nRows);

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
** Finalize a verified batch (for witnesses).
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
