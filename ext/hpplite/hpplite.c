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
** ============================================================================
** Simple Open API (M3)
** ============================================================================
*/

#include "config.h"
#include "crypto.h"
#include "fs_storage.h"
#include "batch.h"
#include "l1_interface.h"

/*
** Global registry mapping sqlite3* -> HppliteOpenState
** (Simple linked list for now, could use hash table for performance)
*/
typedef struct HppliteOpenState HppliteOpenState;
struct HppliteOpenState {
    sqlite3 *db;
    HppliteCtx *ctx;
    HppliteConfig *config;
    HppliteCrypto *crypto;
    HppliteKeypair keypair;
    HppliteStorage *storage;
    HppliteL1 *l1;              /* L1 connection (auto-connected if config present) */
    HppliteOpenState *next;

    /* Batch timer state */
    int64_t lastFlushTime;

    /* State root at last flush (for detecting changes) */
    unsigned char lastFlushedRoot[HPPLITE_HASH_SIZE];

    /* Background timer thread for automatic flushing */
    pthread_t timerThread;
    pthread_mutex_t flushMutex;
    volatile int timerRunning;  /* Flag to signal thread to stop */
};

static HppliteOpenState *g_openDatabases = NULL;

static HppliteOpenState *findOpenState(sqlite3 *db) {
    HppliteOpenState *s = g_openDatabases;
    while (s) {
        if (s->db == db) return s;
        s = s->next;
    }
    return NULL;
}

static void removeOpenState(sqlite3 *db) {
    HppliteOpenState **pp = &g_openDatabases;
    while (*pp) {
        if ((*pp)->db == db) {
            HppliteOpenState *s = *pp;
            *pp = s->next;
            if (s->config) hpplite_config_free(s->config);
            if (s->crypto) hpplite_crypto_free(s->crypto);
            if (s->storage) hpplite_storage_close(s->storage);
            free(s);
            return;
        }
        pp = &(*pp)->next;
    }
}

/*
** Get current time in milliseconds
*/
static int64_t currentTimeMs(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Forward declaration */
static uint64_t doFlush(HppliteOpenState *state);
static uint64_t doFlushLocked(HppliteOpenState *state);

/*
** Background timer thread - automatically flushes batches on interval
*/
static void *timerThreadFunc(void *arg) {
    HppliteOpenState *state = (HppliteOpenState *)arg;

    if (!state || !state->config) return NULL;

    int intervalMs = state->config->batchIntervalMs;
    if (intervalMs <= 0) return NULL;  /* Auto-flush disabled */

    /* Sleep in small increments to allow quick shutdown */
    int sleepMs = (intervalMs < 100) ? intervalMs : 100;

    while (state->timerRunning) {
        /* Sleep for a bit */
        usleep(sleepMs * 1000);

        if (!state->timerRunning) break;

        /* Check if it's time to flush */
        int64_t now = currentTimeMs();
        int64_t elapsed = now - state->lastFlushTime;

        if (elapsed >= intervalMs) {
            /* Lock and flush */
            pthread_mutex_lock(&state->flushMutex);
            if (state->timerRunning) {  /* Double-check after lock */
                doFlushLocked(state);
            }
            pthread_mutex_unlock(&state->flushMutex);
        }
    }

    return NULL;
}

/*
** Start the background timer thread
*/
static void startTimerThread(HppliteOpenState *state) {
    if (!state || !state->config) return;
    if (state->config->batchIntervalMs <= 0) return;  /* Auto-flush disabled */

    pthread_mutex_init(&state->flushMutex, NULL);
    state->timerRunning = 1;
    pthread_create(&state->timerThread, NULL, timerThreadFunc, state);
}

/*
** Stop the background timer thread
*/
static void stopTimerThread(HppliteOpenState *state) {
    if (!state || !state->timerRunning) return;

    /* Signal thread to stop */
    state->timerRunning = 0;
}

/* Forward declaration */
static uint64_t doFlushLocked(HppliteOpenState *state);

/*
** Close hook callback - called automatically by sqlite3_close()
** This performs all HPPLite cleanup before SQLite tears down the connection.
*/
static void hpplite_close_hook(void *pArg, sqlite3 *db) {
    (void)pArg;  /* Unused - we look up state by db */

    HppliteOpenState *state = findOpenState(db);
    if (!state) return;

    /* Stop background timer thread first */
    if (state->timerRunning) {
        state->timerRunning = 0;
        pthread_join(state->timerThread, NULL);
    }

    /* Flush any pending changes */
    pthread_mutex_lock(&state->flushMutex);
    doFlushLocked(state);
    pthread_mutex_unlock(&state->flushMutex);

    /* Shutdown HPPLite context */
    if (state->ctx) {
        hpplite_shutdown(state->ctx);
        state->ctx = NULL;
    }

    /* Disconnect L1 if connected */
    if (state->l1) {
        hpplite_l1_disconnect(state->l1);
        state->l1 = NULL;
    }

    /* Destroy mutex before freeing state */
    pthread_mutex_destroy(&state->flushMutex);

    /* Remove from registry (frees state) */
    removeOpenState(db);
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

    /* Open SQLite database */
    sqlite3 *db;
    int rc = sqlite3_open_v2(uri, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI,
        NULL);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        hpplite_config_free(config);
        return NULL;
    }

    /* Initialize HPPLite context */
    HppliteCtx *ctx;
    rc = hpplite_init(db, &ctx);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        hpplite_config_free(config);
        return NULL;
    }

    /* Create open state */
    HppliteOpenState *state = calloc(1, sizeof(HppliteOpenState));
    if (!state) {
        hpplite_shutdown(ctx);
        sqlite3_close(db);
        hpplite_config_free(config);
        return NULL;
    }

    state->db = db;
    state->ctx = ctx;
    state->config = config;
    state->lastFlushTime = currentTimeMs();

    /* Initialize lastFlushedRoot to current state (genesis) */
    hpplite_get_state_root(ctx, state->lastFlushedRoot);

    /* Initialize crypto if we have a private key */
    if (config->hasPrivkey) {
        state->crypto = hpplite_crypto_init();
        if (state->crypto) {
            hpplite_crypto_keypair_from_privkey(state->crypto, &state->keypair, config->privkey);
        }
    }

    /* Initialize storage */
    hpplite_storage_init(config->dataDir, &state->storage);

    /* Auto-connect to L1 if rpcUrl and contract are configured */
    if (config->rpcUrl && config->hasContract) {
        char contractHex[43];
        snprintf(contractHex, sizeof(contractHex), "0x");
        for (int i = 0; i < 20; i++) {
            snprintf(contractHex + 2 + i*2, 3, "%02x", config->contract[i]);
        }
        state->l1 = hpplite_l1_connect(config->rpcUrl, contractHex);
        /* Note: L1 connection failure is not fatal - may be offline */
    }

    /* Initialize timer thread state */
    state->timerRunning = 0;

    /* Add to global registry */
    state->next = g_openDatabases;
    g_openDatabases = state;

    /* Register close hook for automatic cleanup when sqlite3_close() is called */
    sqlite3_close_hook(db, hpplite_close_hook, NULL);

    /* Start background timer thread for automatic flushing */
    startTimerThread(state);

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
    HppliteOpenState *state = findOpenState(db);
    return state ? state->ctx : NULL;
}

struct HppliteConfig *hpplite_get_config(sqlite3 *db) {
    HppliteOpenState *state = findOpenState(db);
    return state ? state->config : NULL;
}

HppliteL1 *hpplite_get_l1(sqlite3 *db) {
    HppliteOpenState *state = findOpenState(db);
    return state ? state->l1 : NULL;
}

/*
** Internal flush implementation - creates batch if state changed
** Called with mutex held (or when timer not running)
*/
static uint64_t doFlushLocked(HppliteOpenState *state) {
    if (!state || !state->ctx) return 0;

    HppliteCtx *ctx = state->ctx;

    /* Check if state root has changed since last flush */
    unsigned char currentRoot[HPPLITE_HASH_SIZE];
    hpplite_get_state_root(ctx, currentRoot);

    if (memcmp(currentRoot, state->lastFlushedRoot, HPPLITE_HASH_SIZE) == 0) {
        /* No state change since last flush */
        return 0;
    }

    /* Create batch with state transition */
    HppliteBatch *batch = hpplite_batch_new(ctx->blockHeight);
    if (!batch) return 0;

    /* Set pre-state (from last flush) */
    hpplite_batch_set_pre_state(batch, state->lastFlushedRoot);

    /* Set post-state (current) */
    hpplite_batch_set_post_state(batch, currentRoot);

    /* Set timestamp */
    batch->timestamp = (uint64_t)time(NULL);

    uint64_t height = batch->height;

    /* Compute block hash */
    SHA256_CTX hashCtx;
    unsigned char blockHash[HPPLITE_HASH_SIZE];
    sha256_init(&hashCtx);
    sha256_update(&hashCtx, (uint8_t*)&batch->height, sizeof(batch->height));
    sha256_update(&hashCtx, ctx->prevBlockHash, HPPLITE_HASH_SIZE);
    sha256_update(&hashCtx, batch->preStateRoot, HPPLITE_HASH_SIZE);
    sha256_update(&hashCtx, batch->postStateRoot, HPPLITE_HASH_SIZE);
    sha256_final(&hashCtx, blockHash);

    /* Update context for next block */
    memcpy(ctx->prevBlockHash, blockHash, HPPLITE_HASH_SIZE);
    ctx->blockHeight++;

    /* TODO: Sign batch when extended batch format is implemented */
    (void)state->crypto;
    (void)state->keypair;

    /* Store batch */
    if (state->storage) {
        hpplite_storage_store_batch(state->storage, batch);
    }

    /* Update last flushed root */
    memcpy(state->lastFlushedRoot, currentRoot, HPPLITE_HASH_SIZE);

    hpplite_batch_free(batch);
    state->lastFlushTime = currentTimeMs();

    return height;
}

/*
** Thread-safe flush - acquires mutex if timer thread is running
*/
static uint64_t doFlush(HppliteOpenState *state) {
    if (!state) return 0;

    uint64_t height;

    if (state->timerRunning) {
        pthread_mutex_lock(&state->flushMutex);
        height = doFlushLocked(state);
        pthread_mutex_unlock(&state->flushMutex);
    } else {
        height = doFlushLocked(state);
    }

    return height;
}

uint64_t hpplite_flush(sqlite3 *db) {
    HppliteOpenState *state = findOpenState(db);
    return doFlush(state);
}

/*
** Check if auto-flush is needed and perform it if so.
** Returns batch height if flushed, 0 otherwise.
*/
uint64_t hpplite_check_flush(sqlite3 *db) {
    HppliteOpenState *state = findOpenState(db);
    if (!state) return 0;

    /* Check if interval passed */
    if (state->config && state->config->batchIntervalMs > 0) {
        int64_t now = currentTimeMs();
        int64_t elapsed = now - state->lastFlushTime;
        if (elapsed >= state->config->batchIntervalMs) {
            return doFlush(state);
        }
    }
    return 0;
}

void hpplite_state_root(sqlite3 *db, unsigned char *out) {
    HppliteOpenState *state = findOpenState(db);
    if (state && state->ctx) {
        hpplite_get_state_root(state->ctx, out);
    } else {
        memset(out, 0, HPPLITE_HASH_SIZE);
    }
}

/*
** Auto-extension callback - called automatically on every sqlite3_open()
** Checks URI parameters and initializes HPPLite if enabled.
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

    /* Load configuration from URI */
    HppliteConfig *config = hpplite_config_load(filename);
    if (!config) return SQLITE_OK;

    /* Initialize HPPLite context */
    HppliteCtx *ctx;
    int rc = hpplite_init(db, &ctx);
    if (rc != SQLITE_OK) {
        hpplite_config_free(config);
        return SQLITE_OK;  /* Don't fail the open, just skip HPPLite */
    }

    /* Create open state */
    HppliteOpenState *state = calloc(1, sizeof(HppliteOpenState));
    if (!state) {
        hpplite_shutdown(ctx);
        hpplite_config_free(config);
        return SQLITE_OK;
    }

    state->db = db;
    state->ctx = ctx;
    state->config = config;
    state->lastFlushTime = currentTimeMs();

    /* Initialize lastFlushedRoot to current state (genesis) */
    hpplite_get_state_root(ctx, state->lastFlushedRoot);

    /* Initialize crypto if we have a private key */
    if (config->hasPrivkey) {
        state->crypto = hpplite_crypto_init();
        if (state->crypto) {
            hpplite_crypto_keypair_from_privkey(state->crypto, &state->keypair, config->privkey);
        }
    }

    /* Initialize storage */
    hpplite_storage_init(config->dataDir, &state->storage);

    /* Auto-connect to L1 if rpcUrl and contract are configured */
    if (config->rpcUrl && config->hasContract) {
        char contractHex[43];
        snprintf(contractHex, sizeof(contractHex), "0x");
        for (int i = 0; i < 20; i++) {
            snprintf(contractHex + 2 + i*2, 3, "%02x", config->contract[i]);
        }
        state->l1 = hpplite_l1_connect(config->rpcUrl, contractHex);
        /* Note: L1 connection failure is not fatal - may be offline */
    }

    /* Initialize timer thread state */
    state->timerRunning = 0;

    /* Add to global registry */
    state->next = g_openDatabases;
    g_openDatabases = state;

    /* Register close hook for automatic cleanup */
    sqlite3_close_hook(db, hpplite_close_hook, NULL);

    /* Start background timer thread for automatic flushing */
    startTimerThread(state);

    return SQLITE_OK;
}

/*
** Register HPPLite as an auto-extension.
** Call this once at application startup to enable transparent HPPLite support.
** After registration, any sqlite3_open() with ?hpplite=on will automatically
** initialize HPPLite.
**
** Example:
**   hpplite_register();
**   sqlite3_open_v2("file:db.sqlite?hpplite=on&role=sequencer", &db, flags, NULL);
*/
void hpplite_register(void) {
    sqlite3_auto_extension((void(*)(void))hpplite_auto_init);
}

/*
** Unregister HPPLite auto-extension.
*/
void hpplite_unregister(void) {
    sqlite3_cancel_auto_extension((void(*)(void))hpplite_auto_init);
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
