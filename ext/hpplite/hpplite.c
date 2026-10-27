/*
** HPPLite - Verifiable SQLite
**
** Core: statement capture into _hpplite_pending, batch building, content-hash
** state roots, replay from L1. See hpplite.h for the persistence model.
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

/*
** Public SHA-256 helper
*/
void hpplite_sha256(const uint8_t *data, size_t len, uint8_t *out) {
  sha256(data, len, out);
}

#include "batch.h"
#include <time.h>

/*
** ============================================================================
** Local persistence
** ============================================================================
*/

static const char *HPPLITE_SCHEMA =
  "CREATE TABLE IF NOT EXISTS _hpplite_meta("
  "  id INTEGER PRIMARY KEY CHECK(id=1),"
  "  next_height INTEGER NOT NULL,"
  "  prev_block_hash BLOB NOT NULL,"
  "  state_root BLOB NOT NULL,"
  "  last_checkpoint_height INTEGER NOT NULL DEFAULT 0,"
  "  last_checkpoint_root BLOB"
  ");"
  "INSERT OR IGNORE INTO _hpplite_meta(id, next_height, prev_block_hash, state_root)"
  "  VALUES(1, 1, zeroblob(32), zeroblob(32));"
  "CREATE TABLE IF NOT EXISTS _hpplite_pending("
  "  id INTEGER PRIMARY KEY,"
  "  txn INTEGER NOT NULL,"
  "  sql TEXT NOT NULL"
  ");";

/*
** Run SQL on behalf of HPPLite itself. Statements issued while bInternal is
** set are ignored by the trace and commit hooks.
*/
static int internalExec(HppliteCtx *pCtx, sqlite3 *db, const char *zSql) {
  int saved = pCtx->bInternal;
  int rc;
  pCtx->bInternal = 1;
  rc = sqlite3_exec(db, zSql, NULL, NULL, NULL);
  pCtx->bInternal = saved;
  return rc;
}

static void columnHash(sqlite3_stmt *pStmt, int i, unsigned char *pOut) {
  const void *p = sqlite3_column_blob(pStmt, i);
  int n = sqlite3_column_bytes(pStmt, i);
  if (p && n == HPPLITE_HASH_SIZE) {
    memcpy(pOut, p, HPPLITE_HASH_SIZE);
  } else {
    memset(pOut, 0, HPPLITE_HASH_SIZE);
  }
}

/*
** Load the _hpplite_meta row and the transaction counter into the context.
*/
static int loadMeta(HppliteCtx *pCtx) {
  sqlite3_stmt *pStmt;
  int rc;

  pCtx->bInternal = 1;
  rc = sqlite3_prepare_v2(pCtx->bkDb,
    "SELECT next_height, prev_block_hash, state_root,"
    "       last_checkpoint_height, last_checkpoint_root"
    "  FROM _hpplite_meta WHERE id=1", -1, &pStmt, NULL);
  if (rc == SQLITE_OK) {
    if (sqlite3_step(pStmt) == SQLITE_ROW) {
      pCtx->blockHeight = (uint64_t)sqlite3_column_int64(pStmt, 0);
      columnHash(pStmt, 1, pCtx->prevBlockHash);
      columnHash(pStmt, 2, pCtx->stateRoot);
      pCtx->lastCheckpointHeight = (uint64_t)sqlite3_column_int64(pStmt, 3);
      columnHash(pStmt, 4, pCtx->lastCheckpointRoot);
    }
    sqlite3_finalize(pStmt);
  }

  /* Pending rows carry transaction numbers; keep numbering above them */
  if (sqlite3_prepare_v2(pCtx->bkDb,
        "SELECT COALESCE(MAX(txn), 0) + 1 FROM _hpplite_pending",
        -1, &pStmt, NULL) == SQLITE_OK) {
    if (sqlite3_step(pStmt) == SQLITE_ROW) {
      pCtx->txnSeq = (uint64_t)sqlite3_column_int64(pStmt, 0);
    }
    sqlite3_finalize(pStmt);
  }
  pCtx->bInternal = 0;
  pCtx->bRootDirty = 1;
  return rc;
}

/*
** ============================================================================
** Content hash (state root)
** ============================================================================
*/

static void hashU64(SHA256_CTX *pH, uint64_t v) {
  uint8_t b[8];
  int i;
  for (i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (56 - i * 8));
  sha256_update(pH, b, 8);
}

static void hashBytes(SHA256_CTX *pH, const void *p, int n) {
  hashU64(pH, (uint64_t)n);
  if (n > 0) sha256_update(pH, (const uint8_t*)p, (size_t)n);
}

static void hashText(SHA256_CTX *pH, const unsigned char *z) {
  hashBytes(pH, z, z ? (int)strlen((const char*)z) : 0);
}

static void hashValue(SHA256_CTX *pH, sqlite3_stmt *pStmt, int i) {
  switch (sqlite3_column_type(pStmt, i)) {
    case SQLITE_INTEGER: {
      sha256_update(pH, (const uint8_t*)"I", 1);
      hashU64(pH, (uint64_t)sqlite3_column_int64(pStmt, i));
      break;
    }
    case SQLITE_FLOAT: {
      double d = sqlite3_column_double(pStmt, i);
      uint64_t bits;
      memcpy(&bits, &d, 8);
      sha256_update(pH, (const uint8_t*)"F", 1);
      hashU64(pH, bits);
      break;
    }
    case SQLITE_TEXT:
      sha256_update(pH, (const uint8_t*)"T", 1);
      hashBytes(pH, sqlite3_column_text(pStmt, i), sqlite3_column_bytes(pStmt, i));
      break;
    case SQLITE_BLOB:
      sha256_update(pH, (const uint8_t*)"B", 1);
      hashBytes(pH, sqlite3_column_blob(pStmt, i), sqlite3_column_bytes(pStmt, i));
      break;
    default:
      sha256_update(pH, (const uint8_t*)"N", 1);
      break;
  }
}

/*
** Hash every row of one table in a deterministic order.
*/
static int hashTableRows(sqlite3 *db, SHA256_CTX *pH, const char *zName, const char *zSql) {
  sqlite3_stmt *pStmt;
  char *zQuery;
  int rc, nCol, i;
  int withoutRowid = (zSql && strstr(zSql, "WITHOUT ROWID") != NULL);

  if (withoutRowid) {
    /* No rowid: order by every column (the primary key is among them) */
    sqlite3_str *pStr;
    zQuery = sqlite3_mprintf("SELECT * FROM \"%w\"", zName);
    rc = sqlite3_prepare_v2(db, zQuery, -1, &pStmt, NULL);
    sqlite3_free(zQuery);
    if (rc != SQLITE_OK) return rc;
    nCol = sqlite3_column_count(pStmt);
    sqlite3_finalize(pStmt);
    pStr = sqlite3_str_new(db);
    sqlite3_str_appendf(pStr, "SELECT * FROM \"%w\" ORDER BY 1", zName);
    for (i = 2; i <= nCol; i++) sqlite3_str_appendf(pStr, ",%d", i);
    zQuery = sqlite3_str_finish(pStr);
  } else {
    zQuery = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY rowid", zName);
  }
  rc = sqlite3_prepare_v2(db, zQuery, -1, &pStmt, NULL);
  sqlite3_free(zQuery);
  if (rc != SQLITE_OK) return rc;

  nCol = sqlite3_column_count(pStmt);
  while ((rc = sqlite3_step(pStmt)) == SQLITE_ROW) {
    sha256_update(pH, (const uint8_t*)"R", 1);
    for (i = 0; i < nCol; i++) hashValue(pH, pStmt, i);
  }
  sqlite3_finalize(pStmt);
  return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

int hpplite_db_hash(sqlite3 *db, unsigned char *pOut) {
  SHA256_CTX h;
  sqlite3_stmt *pStmt;
  int rc, nEntries = 0;

  sha256_init(&h);
  rc = sqlite3_prepare_v2(db,
    "SELECT type, name, tbl_name, sql FROM sqlite_schema"
    " WHERE name NOT LIKE 'sqlite\\_%' ESCAPE '\\'"
    "   AND name NOT LIKE '\\_hpplite\\_%' ESCAPE '\\'"
    " ORDER BY type, name", -1, &pStmt, NULL);
  if (rc != SQLITE_OK) return rc;

  while ((rc = sqlite3_step(pStmt)) == SQLITE_ROW) {
    const unsigned char *zType = sqlite3_column_text(pStmt, 0);
    const unsigned char *zName = sqlite3_column_text(pStmt, 1);
    const unsigned char *zSql = sqlite3_column_text(pStmt, 3);
    nEntries++;
    sha256_update(&h, (const uint8_t*)"S", 1);
    hashText(&h, zType);
    hashText(&h, zName);
    hashText(&h, sqlite3_column_text(pStmt, 2));
    hashText(&h, zSql);
    if (zType && strcmp((const char*)zType, "table") == 0
        && zSql && sqlite3_strnicmp((const char*)zSql, "CREATE VIRTUAL", 14) != 0) {
      rc = hashTableRows(db, &h, (const char*)zName, (const char*)zSql);
      if (rc != SQLITE_OK) break;
    }
  }
  sqlite3_finalize(pStmt);
  if (rc != SQLITE_DONE && rc != SQLITE_OK) return rc;

  if (nEntries == 0) {
    memset(pOut, 0, HPPLITE_HASH_SIZE);  /* Genesis: no user schema */
  } else {
    sha256_final(&h, pOut);
  }
  return SQLITE_OK;
}

/*
** ============================================================================
** Hooks
** ============================================================================
*/

/*
** Decide whether a statement is a write worth logging, by its first keyword.
*/
static int isTrackedSql(const char *z) {
  static const char *aSkip[] = {
    "SELECT", "EXPLAIN", "PRAGMA", "BEGIN", "COMMIT", "END", "ROLLBACK",
    "SAVEPOINT", "RELEASE", "VACUUM", "ANALYZE", "REINDEX", "ATTACH", "DETACH",
    NULL
  };
  int i;

  /* Skip whitespace and comments. Trigger sub-programs are traced as a bare
  ** "-- TRIGGER name" comment; they are reproduced by replaying the statement
  ** that fired them. */
  for (;;) {
    while (*z == ' ' || *z == '\t' || *z == '\n' || *z == '\r') z++;
    if (z[0] == '-' && z[1] == '-') {
      while (*z && *z != '\n') z++;
    } else if (z[0] == '/' && z[1] == '*') {
      const char *zEnd = strstr(z + 2, "*/");
      if (!zEnd) return 0;
      z = zEnd + 2;
    } else {
      break;
    }
  }
  if (*z == '\0') return 0;
  for (i = 0; aSkip[i]; i++) {
    int n = (int)strlen(aSkip[i]);
    if (sqlite3_strnicmp(z, aSkip[i], n) == 0) {
      char c = z[n];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
        return 0;
      }
    }
  }
  return 1;
}

/*
** Trace callback: runs at the start of every statement on the application
** connection. Writes the statement text into _hpplite_pending through the
** same connection, so the row is part of whatever transaction the statement
** runs in - it commits with the statement and rolls back with it.
*/
static int hppliteTraceCallback(unsigned mask, void *pArg, void *pStmt, void *pX) {
  HppliteCtx *pCtx = (HppliteCtx*)pArg;
  sqlite3_stmt *pS = (sqlite3_stmt*)pStmt;
  const char *zSql = (const char*)pX;
  char *zExpanded;
  int rc;
  (void)mask;

  if (!pCtx || !pCtx->bEnabled || pCtx->bInternal || !zSql) return 0;
  if (!isTrackedSql(zSql)) return 0;
  if (strstr(zSql, "_hpplite_")) return 0;  /* never log our own tables */

  /* Bound parameters are substituted so the statement can be replayed */
  zExpanded = sqlite3_expanded_sql(pS);

  pCtx->bInternal = 1;
  if (!pCtx->pLogStmt) {
    rc = sqlite3_prepare_v3(pCtx->db,
      "INSERT INTO _hpplite_pending(txn, sql) VALUES(?1, ?2)",
      -1, SQLITE_PREPARE_PERSISTENT, &pCtx->pLogStmt, NULL);
    if (rc != SQLITE_OK) pCtx->pLogStmt = NULL;
  }
  if (pCtx->pLogStmt) {
    sqlite3_bind_int64(pCtx->pLogStmt, 1, (sqlite3_int64)pCtx->txnSeq);
    sqlite3_bind_text(pCtx->pLogStmt, 2, zExpanded ? zExpanded : zSql, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(pCtx->pLogStmt);
    sqlite3_reset(pCtx->pLogStmt);
    sqlite3_clear_bindings(pCtx->pLogStmt);
  } else {
    rc = SQLITE_ERROR;
  }
  if (rc != SQLITE_DONE) {
    pCtx->nLogErrors++;
    fprintf(stderr, "hpplite: could not log statement: %s\n", sqlite3_errmsg(pCtx->db));
  }
  pCtx->bInternal = 0;
  sqlite3_free(zExpanded);
  return 0;
}

/*
** Commit hook: close the current transaction group.
*/
static int hppliteCommitHook(void *pArg) {
  HppliteCtx *pCtx = (HppliteCtx*)pArg;
  if (pCtx->bInternal) return 0;
  pCtx->txnSeq++;
  pCtx->bRootDirty = 1;
  if (pCtx->xOnCommit) {
    pCtx->xOnCommit(pCtx->pCallbackArg, NULL, 0);
  }
  return 0;  /* Allow commit */
}

/*
** Rollback hook: pending rows written in the transaction are gone with it.
*/
static void hppliteRollbackHook(void *pArg) {
  HppliteCtx *pCtx = (HppliteCtx*)pArg;
  if (pCtx->bInternal) return;
  if (pCtx->xOnRollback) {
    pCtx->xOnRollback(pCtx->pCallbackArg);
  }
}

/*
** ============================================================================
** Lifecycle
** ============================================================================
*/

int hpplite_init(sqlite3 *db, HppliteCtx **ppCtx) {
  HppliteCtx *pCtx;
  int rc;

  pCtx = (HppliteCtx*)sqlite3_malloc(sizeof(HppliteCtx));
  if (!pCtx) return SQLITE_NOMEM;
  memset(pCtx, 0, sizeof(HppliteCtx));

  pCtx->db = db;
  pCtx->bkDb = db;
  pCtx->bEnabled = 1;

  rc = internalExec(pCtx, db, HPPLITE_SCHEMA);
  if (rc != SQLITE_OK) {
    sqlite3_free(pCtx);
    return rc;
  }
  loadMeta(pCtx);

  sqlite3_commit_hook(db, hppliteCommitHook, pCtx);
  sqlite3_rollback_hook(db, hppliteRollbackHook, pCtx);
  sqlite3_trace_v2(db, SQLITE_TRACE_STMT, hppliteTraceCallback, pCtx);

  *ppCtx = pCtx;
  return SQLITE_OK;
}

void hpplite_shutdown(HppliteCtx *pCtx) {
  if (!pCtx) return;

  sqlite3_commit_hook(pCtx->db, NULL, NULL);
  sqlite3_rollback_hook(pCtx->db, NULL, NULL);
  sqlite3_trace_v2(pCtx->db, 0, NULL, NULL);
  if (pCtx->pLogStmt) sqlite3_finalize(pCtx->pLogStmt);
  sqlite3_free(pCtx);
}

int hpplite_set_bookkeeping_db(HppliteCtx *pCtx, sqlite3 *bkDb) {
  if (!pCtx) return SQLITE_MISUSE;
  pCtx->bkDb = bkDb ? bkDb : pCtx->db;
  return loadMeta(pCtx);
}

void hpplite_enable(HppliteCtx *pCtx, int bEnable) {
  if (pCtx) pCtx->bEnabled = bEnable;
}

void hpplite_get_state_root(HppliteCtx *pCtx, unsigned char *pOut) {
  if (!pCtx || !pOut) return;
  if (pCtx->bRootDirty) {
    int saved = pCtx->bInternal;
    pCtx->bInternal = 1;
    if (hpplite_db_hash(pCtx->bkDb, pCtx->curRoot) == SQLITE_OK) {
      pCtx->bRootDirty = 0;
    }
    pCtx->bInternal = saved;
  }
  memcpy(pOut, pCtx->curRoot, HPPLITE_HASH_SIZE);
}

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
** ============================================================================
** Block Builder Implementation
** ============================================================================
*/

int hpplite_exec(HppliteCtx *pCtx, const char *zSql, char **pzErrMsg) {
  return hpplite_exec_cb(pCtx, zSql, NULL, NULL, pzErrMsg);
}

int hpplite_exec_cb(
  HppliteCtx *pCtx,
  const char *zSql,
  int (*xCallback)(void*, int, char**, char**),
  void *pArg,
  char **pzErrMsg
) {
  if (!pCtx || !zSql) return SQLITE_MISUSE;
  /* The trace callback logs the statement; nothing else to do here */
  return sqlite3_exec(pCtx->db, zSql, xCallback, pArg, pzErrMsg);
}

void hpplite_block_hash(const HppliteBatch *pBatch, const unsigned char *pPrevHash, unsigned char *pOut) {
  SHA256_CTX h;
  int i;
  sha256_init(&h);
  hashU64(&h, pBatch->height);
  sha256_update(&h, pPrevHash, HPPLITE_HASH_SIZE);
  sha256_update(&h, pBatch->preStateRoot, HPPLITE_HASH_SIZE);
  sha256_update(&h, pBatch->postStateRoot, HPPLITE_HASH_SIZE);
  for (i = 0; i < pBatch->nTxns; i++) {
    hashText(&h, (const unsigned char*)pBatch->aTxns[i].zSql);
  }
  sha256_final(&h, pOut);
}

/*
** Bookkeeping transactions. With a dedicated bookkeeping connection these
** never touch the application's transaction. On the shared (:memory:)
** connection they are only possible while the application is in autocommit.
*/
static int bkBegin(HppliteCtx *pCtx, int bWrite) {
  if (pCtx->bkDb == pCtx->db && !sqlite3_get_autocommit(pCtx->db)) {
    return SQLITE_BUSY;
  }
  return internalExec(pCtx, pCtx->bkDb, bWrite ? "BEGIN IMMEDIATE" : "BEGIN");
}

static int bkEnd(HppliteCtx *pCtx, int rc) {
  if (rc == SQLITE_OK) {
    rc = internalExec(pCtx, pCtx->bkDb, "COMMIT");
  }
  if (rc != SQLITE_OK) {
    internalExec(pCtx, pCtx->bkDb, "ROLLBACK");
  }
  return rc;
}

struct HppliteBatch *hpplite_build_batch(HppliteCtx *pCtx, sqlite3_int64 *pMaxId) {
  HppliteBatch *pBatch = NULL;
  sqlite3_stmt *pStmt = NULL;
  sqlite3_int64 maxId = 0, lastTxn = -1, txnRel = -1;
  int rc;

  if (!pCtx) return NULL;
  if (bkBegin(pCtx, 0) != SQLITE_OK) return NULL;
  pCtx->bInternal = 1;

  rc = sqlite3_prepare_v2(pCtx->bkDb,
    "SELECT id, txn, sql FROM _hpplite_pending ORDER BY id", -1, &pStmt, NULL);
  if (rc == SQLITE_OK) {
    while (sqlite3_step(pStmt) == SQLITE_ROW) {
      sqlite3_int64 txn = sqlite3_column_int64(pStmt, 1);
      if (!pBatch) {
        pBatch = hpplite_batch_new(pCtx->blockHeight);
        if (!pBatch) break;
        hpplite_batch_set_pre_state(pBatch, pCtx->stateRoot);
      }
      if (txn != lastTxn) { txnRel++; lastTxn = txn; }
      hpplite_batch_add_stmt(pBatch, (const char*)sqlite3_column_text(pStmt, 2), -1, txnRel);
      maxId = sqlite3_column_int64(pStmt, 0);
    }
    sqlite3_finalize(pStmt);
  }

  if (pBatch) {
    /* Post-state root is taken in the same snapshot as the pending rows */
    unsigned char root[HPPLITE_HASH_SIZE];
    if (hpplite_db_hash(pCtx->bkDb, root) == SQLITE_OK) {
      hpplite_batch_set_post_state(pBatch, root);
      pBatch->timestamp = (uint64_t)time(NULL);
    } else {
      hpplite_batch_free(pBatch);
      pBatch = NULL;
    }
  }
  pCtx->bInternal = 0;
  bkEnd(pCtx, SQLITE_OK);

  if (pMaxId) *pMaxId = maxId;
  return pBatch;
}

/*
** Advance _hpplite_meta past pBatch and optionally delete pending rows.
** Caller holds a bookkeeping write transaction.
*/
static int advanceMeta(HppliteCtx *pCtx, const HppliteBatch *pBatch, sqlite3_int64 maxId) {
  unsigned char blockHash[HPPLITE_HASH_SIZE];
  sqlite3_stmt *pStmt;
  int rc;

  hpplite_block_hash(pBatch, pCtx->prevBlockHash, blockHash);

  if (maxId > 0) {
    rc = sqlite3_prepare_v2(pCtx->bkDb,
      "DELETE FROM _hpplite_pending WHERE id <= ?1", -1, &pStmt, NULL);
    if (rc != SQLITE_OK) return rc;
    sqlite3_bind_int64(pStmt, 1, maxId);
    rc = sqlite3_step(pStmt);
    sqlite3_finalize(pStmt);
    if (rc != SQLITE_DONE) return rc;
  }

  rc = sqlite3_prepare_v2(pCtx->bkDb,
    "UPDATE _hpplite_meta SET next_height=?1, prev_block_hash=?2, state_root=?3 WHERE id=1",
    -1, &pStmt, NULL);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)(pBatch->height + 1));
  sqlite3_bind_blob(pStmt, 2, blockHash, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
  sqlite3_bind_blob(pStmt, 3, pBatch->postStateRoot, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
  rc = sqlite3_step(pStmt);
  sqlite3_finalize(pStmt);
  if (rc != SQLITE_DONE) return rc;

  pCtx->blockHeight = pBatch->height + 1;
  memcpy(pCtx->prevBlockHash, blockHash, HPPLITE_HASH_SIZE);
  memcpy(pCtx->stateRoot, pBatch->postStateRoot, HPPLITE_HASH_SIZE);
  return SQLITE_OK;
}

int hpplite_finalize_batch(HppliteCtx *pCtx, const HppliteBatch *pBatch, sqlite3_int64 maxId) {
  int rc;
  if (!pCtx || !pBatch) return SQLITE_MISUSE;
  if (pBatch->height != pCtx->blockHeight) return SQLITE_MISMATCH;
  rc = bkBegin(pCtx, 1);
  if (rc != SQLITE_OK) return rc;
  pCtx->bInternal = 1;
  rc = advanceMeta(pCtx, pBatch, maxId);
  pCtx->bInternal = 0;
  return bkEnd(pCtx, rc);
}

struct HppliteBatch *hpplite_flush_block(HppliteCtx *pCtx) {
  sqlite3_int64 maxId = 0;
  HppliteBatch *pBatch = hpplite_build_batch(pCtx, &maxId);
  if (!pBatch) return NULL;
  if (hpplite_finalize_batch(pCtx, pBatch, maxId) != SQLITE_OK) {
    hpplite_batch_free(pBatch);
    return NULL;
  }
  return pBatch;
}

int hpplite_replay_batch(HppliteCtx *pCtx, const HppliteBatch *pBatch, int useTxn) {
  int savedEnabled, rc = SQLITE_OK, i, open = 0;
  sqlite3_int64 lastTxn = -1;
  unsigned char root[HPPLITE_HASH_SIZE];

  if (!pCtx || !pBatch) return SQLITE_MISUSE;
  if (pBatch->height < pCtx->blockHeight) return SQLITE_DONE;   /* already applied */
  if (pBatch->height > pCtx->blockHeight) return SQLITE_MISMATCH; /* gap */

  savedEnabled = pCtx->bEnabled;
  pCtx->bEnabled = 0;
  pCtx->bInternal = 1;
  for (i = 0; i < pBatch->nTxns && rc == SQLITE_OK; i++) {
    const char *zSql = pBatch->aTxns[i].zSql;
    if (!zSql) continue;
    if (useTxn && pBatch->aTxns[i].txn != lastTxn) {
      if (open) rc = sqlite3_exec(pCtx->db, "COMMIT", NULL, NULL, NULL);
      if (rc == SQLITE_OK) rc = sqlite3_exec(pCtx->db, "BEGIN", NULL, NULL, NULL);
      open = (rc == SQLITE_OK);
      lastTxn = pBatch->aTxns[i].txn;
    }
    if (rc == SQLITE_OK) rc = sqlite3_exec(pCtx->db, zSql, NULL, NULL, NULL);
  }
  if (open) {
    if (rc == SQLITE_OK) rc = sqlite3_exec(pCtx->db, "COMMIT", NULL, NULL, NULL);
    else sqlite3_exec(pCtx->db, "ROLLBACK", NULL, NULL, NULL);
  }
  pCtx->bInternal = 0;
  pCtx->bEnabled = savedEnabled;
  pCtx->bRootDirty = 1;
  if (rc != SQLITE_OK) {
    fprintf(stderr, "hpplite: replay of batch %llu failed: %s\n",
            (unsigned long long)pBatch->height, sqlite3_errmsg(pCtx->db));
    return rc;
  }

  rc = bkBegin(pCtx, 1);
  if (rc != SQLITE_OK) return rc;
  pCtx->bInternal = 1;
  rc = advanceMeta(pCtx, pBatch, 0);
  pCtx->bInternal = 0;
  rc = bkEnd(pCtx, rc);
  if (rc != SQLITE_OK) return rc;

  /* The seed of `hpplite verify`: does the content match what was posted? */
  hpplite_get_state_root(pCtx, root);
  if (memcmp(root, pBatch->postStateRoot, HPPLITE_HASH_SIZE) != 0) {
    fprintf(stderr, "hpplite: state root mismatch after replaying batch %llu\n",
            (unsigned long long)pBatch->height);
  }
  return SQLITE_OK;
}

int hpplite_adopt_batch(HppliteCtx *pCtx, const HppliteBatch *pBatch) {
  sqlite3_stmt *pStmt;
  sqlite3_int64 maxId = 0;
  int rc, i = 0, ok = 1;

  if (!pCtx || !pBatch) return SQLITE_MISUSE;
  if (pBatch->height != pCtx->blockHeight) return SQLITE_MISMATCH;

  pCtx->bInternal = 1;
  rc = sqlite3_prepare_v2(pCtx->bkDb,
    "SELECT id, sql FROM _hpplite_pending ORDER BY id LIMIT ?1", -1, &pStmt, NULL);
  if (rc != SQLITE_OK) { pCtx->bInternal = 0; return rc; }
  sqlite3_bind_int(pStmt, 1, pBatch->nTxns);
  while (sqlite3_step(pStmt) == SQLITE_ROW && i < pBatch->nTxns) {
    const char *zSql = (const char*)sqlite3_column_text(pStmt, 1);
    if (!zSql || !pBatch->aTxns[i].zSql || strcmp(zSql, pBatch->aTxns[i].zSql) != 0) {
      ok = 0;
      break;
    }
    maxId = sqlite3_column_int64(pStmt, 0);
    i++;
  }
  sqlite3_finalize(pStmt);
  pCtx->bInternal = 0;
  if (!ok || i != pBatch->nTxns) return SQLITE_MISMATCH;

  return hpplite_finalize_batch(pCtx, pBatch, maxId);
}

int hpplite_pending_count(HppliteCtx *pCtx) {
  sqlite3_stmt *pStmt;
  int n = 0;
  if (!pCtx) return 0;
  pCtx->bInternal = 1;
  if (sqlite3_prepare_v2(pCtx->bkDb, "SELECT count(*) FROM _hpplite_pending",
                         -1, &pStmt, NULL) == SQLITE_OK) {
    if (sqlite3_step(pStmt) == SQLITE_ROW) n = sqlite3_column_int(pStmt, 0);
    sqlite3_finalize(pStmt);
  }
  pCtx->bInternal = 0;
  return n;
}

uint64_t hpplite_get_block_height(HppliteCtx *pCtx) {
  return pCtx ? pCtx->blockHeight : 0;
}

void hpplite_get_prev_block_hash(HppliteCtx *pCtx, unsigned char *pOut) {
  if (pCtx && pOut) {
    memcpy(pOut, pCtx->prevBlockHash, HPPLITE_HASH_SIZE);
  }
}

int hpplite_set_checkpoint(HppliteCtx *pCtx, uint64_t height, const unsigned char *pRoot) {
  sqlite3_stmt *pStmt;
  int rc;
  if (!pCtx || !pRoot) return SQLITE_MISUSE;
  pCtx->bInternal = 1;
  rc = sqlite3_prepare_v2(pCtx->bkDb,
    "UPDATE _hpplite_meta SET last_checkpoint_height=?1, last_checkpoint_root=?2 WHERE id=1",
    -1, &pStmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)height);
    sqlite3_bind_blob(pStmt, 2, pRoot, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
    rc = sqlite3_step(pStmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
    sqlite3_finalize(pStmt);
  }
  pCtx->bInternal = 0;
  if (rc == SQLITE_OK) {
    pCtx->lastCheckpointHeight = height;
    memcpy(pCtx->lastCheckpointRoot, pRoot, HPPLITE_HASH_SIZE);
  }
  return rc;
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

    if (!node || !node->l1) {
        sqlite3_result_int(ctx, 0);
        return;
    }

    /* Running inside a statement on this connection: replay without
    ** wrapping each batch transaction in BEGIN/COMMIT. */
    int synced = hpplite_node_sync_from_l1(node, 0);
    sqlite3_result_int(ctx, synced < 0 ? 0 : synced);
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
