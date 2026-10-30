/*
** HPPLite row capture (capture engine v2, #19; doc/design-row-capture.md)
**
** A record is what one changed row became, in a compact typed encoding:
**
**   value  := tag payload      tag 0 NULL, 1 integer (zigzag varint),
**                              2 real (8 bytes, IEEE 754, big-endian),
**                              3 text, 4 blob (varint length, bytes)
**   insert := varint n, n values      key (rowid, or the primary key of a
**                                      WITHOUT ROWID table) then every
**                                      stored column
**   update := varint nKey, old key values, byte keyChanged,
**             [new key values], varint nCol, bitmap (nCol bits),
**             the changed columns' new values
**   delete := varint n, key values
**
** Columns are the table's stored columns in order (generated ones are
** computed by the replica); the replica's table has the same columns at that
** point of the log, schema changes being in the same log.
*/
#include "hpplite_sqlite.h"
#include "hpplite.h"
#include "capture_rows.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- encoding */

typedef struct Rec { unsigned char *a; int n, cap, oom; } Rec;

static void recGrow(Rec *r, int n) {
  if (r->oom || r->n + n <= r->cap) return;
  int cap = (r->cap + n) * 2;
  unsigned char *p = sqlite3_realloc(r->a, cap);
  if (!p) { r->oom = 1; return; }
  r->a = p; r->cap = cap;
}
static void recByte(Rec *r, unsigned char b) { recGrow(r, 1); if (!r->oom) r->a[r->n++] = b; }
static void recVarint(Rec *r, sqlite3_uint64 v) {
  recGrow(r, 10);
  if (r->oom) return;
  do { unsigned char b = v & 0x7f; v >>= 7; r->a[r->n++] = b | (v ? 0x80 : 0); } while (v);
}
static void recBytes(Rec *r, const void *p, int n) {
  recVarint(r, (sqlite3_uint64)n);
  recGrow(r, n);
  if (!r->oom && n > 0) { memcpy(r->a + r->n, p, (size_t)n); r->n += n; }
}
static void recValue(Rec *r, sqlite3_value *v) {
  switch (sqlite3_value_type(v)) {
    case SQLITE_INTEGER: {
      sqlite3_int64 x = sqlite3_value_int64(v);
      recByte(r, 1);
      recVarint(r, ((sqlite3_uint64)x << 1) ^ (sqlite3_uint64)(x >> 63));
      break;
    }
    case SQLITE_FLOAT: {
      double d = sqlite3_value_double(v);
      sqlite3_uint64 u;
      int i;
      memcpy(&u, &d, 8);
      recByte(r, 2);
      for (i = 7; i >= 0; i--) recByte(r, (unsigned char)(u >> (i * 8)));
      break;
    }
    case SQLITE_TEXT:
      recByte(r, 3);
      recBytes(r, sqlite3_value_text(v), sqlite3_value_bytes(v));
      break;
    case SQLITE_BLOB:
      recByte(r, 4);
      recBytes(r, sqlite3_value_blob(v), sqlite3_value_bytes(v));
      break;
    default:
      recByte(r, 0);
  }
}
static void recResult(sqlite3_context *c, Rec *r) {
  if (r->oom) { sqlite3_free(r->a); sqlite3_result_error_nomem(c); return; }
  sqlite3_result_blob(c, r->a, r->n, sqlite3_free);
}

/* hpplite_rec(v, ...): an insert or delete record */
static void fnRec(sqlite3_context *c, int n, sqlite3_value **v) {
  Rec r = {0};
  int i;
  recVarint(&r, (sqlite3_uint64)n);
  for (i = 0; i < n; i++) recValue(&r, v[i]);
  recResult(c, &r);
}

/* Same value, the way the row would store it (NULL IS NULL; 1 IS 1.0 is not:
** types are part of the value) */
static int sameValue(sqlite3_value *a, sqlite3_value *b) {
  int t = sqlite3_value_type(a);
  if (t != sqlite3_value_type(b)) return 0;
  switch (t) {
    case SQLITE_NULL: return 1;
    case SQLITE_INTEGER: return sqlite3_value_int64(a) == sqlite3_value_int64(b);
    case SQLITE_FLOAT: { double x = sqlite3_value_double(a), y = sqlite3_value_double(b); return memcmp(&x, &y, 8) == 0; }
    default: {
      int n = sqlite3_value_bytes(a);
      return n == sqlite3_value_bytes(b) && (n == 0 || memcmp(sqlite3_value_blob(a), sqlite3_value_blob(b), (size_t)n) == 0);
    }
  }
}

/* hpplite_upd(nKey, old key..., new key..., old c1, new c1, ...): an update record */
static void fnUpd(sqlite3_context *c, int n, sqlite3_value **v) {
  Rec r = {0};
  int nKey = sqlite3_value_int(v[0]), i, nCol, keyChanged = 0;
  if (nKey < 0 || 1 + 2 * nKey > n || (n - 1 - 2 * nKey) % 2) { sqlite3_result_error(c, "hpplite_upd: arguments", -1); return; }
  nCol = (n - 1 - 2 * nKey) / 2;
  recVarint(&r, (sqlite3_uint64)nKey);
  for (i = 0; i < nKey; i++) recValue(&r, v[1 + i]);
  for (i = 0; i < nKey; i++) if (!sameValue(v[1 + i], v[1 + nKey + i])) keyChanged = 1;
  recByte(&r, (unsigned char)keyChanged);
  if (keyChanged) for (i = 0; i < nKey; i++) recValue(&r, v[1 + nKey + i]);
  recVarint(&r, (sqlite3_uint64)nCol);
  {
    sqlite3_value **pc = v + 1 + 2 * nKey;
    int nb = (nCol + 7) / 8, at;
    recGrow(&r, nb);
    if (!r.oom) {
      at = r.n;
      memset(r.a + at, 0, (size_t)nb);
      r.n += nb;
      for (i = 0; i < nCol; i++) if (!sameValue(pc[2 * i], pc[2 * i + 1])) r.a[at + i / 8] |= (unsigned char)(1 << (i % 8));
      for (i = 0; i < nCol; i++) if (r.a[at + i / 8] & (1 << (i % 8))) recValue(&r, pc[2 * i + 1]);
    }
  }
  recResult(c, &r);
}

/* hpplite_cap(): capture is on (off while a statement is logged as text, and
** while HPPLite runs its own statements - replay among them) */
static void fnCap(sqlite3_context *c, int n, sqlite3_value **v) {
  HppliteCtx *p = sqlite3_user_data(c);
  (void)n; (void)v;
  sqlite3_result_int(c, p->bRows && !p->bInternal && !p->bRowsCapOff);
}
/* hpplite_grp() / hpplite_note(id): transaction groups as v1 names them -
** the first row of a transaction stores 0 (its own id), the rest that id */
static void fnGrp(sqlite3_context *c, int n, sqlite3_value **v) {
  HppliteCtx *p = sqlite3_user_data(c);
  (void)n; (void)v;
  sqlite3_result_int64(c, p->curTxn);
}
static void fnNote(sqlite3_context *c, int n, sqlite3_value **v) {
  HppliteCtx *p = sqlite3_user_data(c);
  (void)n;
  if (p->curTxn == 0) p->curTxn = sqlite3_value_int64(v[0]);
  sqlite3_result_null(c);
}

/* ---------------------------------------------------------------- triggers */

/* A table as capture sees it: its stored columns and its key */
typedef struct Tab {
  char *zName;
  int nCol; char **azCol;       /* stored columns, in order */
  int nKey; char **azKey;       /* key columns (WITHOUT ROWID); 0 = rowid */
  int ipk;                      /* rowid table whose INTEGER PRIMARY KEY is the rowid: its index, else -1 */
} Tab;

static void tabFree(Tab *t) {
  int i;
  for (i = 0; i < t->nCol; i++) sqlite3_free(t->azCol[i]);
  for (i = 0; i < t->nKey; i++) sqlite3_free(t->azKey[i]);
  sqlite3_free(t->azCol); sqlite3_free(t->azKey); sqlite3_free(t->zName);
  memset(t, 0, sizeof(*t));
}

static int tabLoad(sqlite3 *db, const char *zName, Tab *t) {
  sqlite3_stmt *q = NULL;
  char *z;
  int rc, nPk = 0, i, hasRowid;
  memset(t, 0, sizeof(*t));
  t->ipk = -1;
  t->zName = sqlite3_mprintf("%s", zName);
  /* a WITHOUT ROWID table has no rowid to select */
  z = sqlite3_mprintf("SELECT rowid FROM \"%w\" LIMIT 0", zName);
  hasRowid = sqlite3_prepare_v2(db, z, -1, &q, NULL) == SQLITE_OK;
  sqlite3_finalize(q); q = NULL;
  sqlite3_free(z);
  z = sqlite3_mprintf("SELECT name, type, pk, hidden FROM pragma_table_xinfo(%Q) ORDER BY cid", zName);
  rc = sqlite3_prepare_v2(db, z, -1, &q, NULL);
  sqlite3_free(z);
  if (rc != SQLITE_OK) return rc;
  {
    int cap = 0, pkCap = 0;
    char **azPkByOrder = NULL;
    int *aPkOrder = NULL;
    while (sqlite3_step(q) == SQLITE_ROW) {
      int hidden = sqlite3_column_int(q, 3), pk = sqlite3_column_int(q, 2);
      const char *zCol = (const char *)sqlite3_column_text(q, 0);
      const char *zType = (const char *)sqlite3_column_text(q, 1);
      if (hidden == 2 || hidden == 3) continue;           /* generated: the replica computes it */
      if (t->nCol == cap) { cap = cap ? cap * 2 : 8; t->azCol = sqlite3_realloc(t->azCol, cap * (int)sizeof(char *)); }
      t->azCol[t->nCol] = sqlite3_mprintf("%s", zCol);
      if (pk > 0) {
        if (nPk == pkCap) {
          pkCap = pkCap ? pkCap * 2 : 4;
          azPkByOrder = sqlite3_realloc(azPkByOrder, pkCap * (int)sizeof(char *));
          aPkOrder = sqlite3_realloc(aPkOrder, pkCap * (int)sizeof(int));
        }
        azPkByOrder[nPk] = t->azCol[t->nCol]; aPkOrder[nPk] = pk; nPk++;
        if (hasRowid && zType && sqlite3_stricmp(zType, "INTEGER") == 0) t->ipk = t->nCol;
      }
      t->nCol++;
    }
    sqlite3_finalize(q);
    if (hasRowid && nPk != 1) t->ipk = -1;
    if (!hasRowid) {
      /* the key is the primary key, in its declared order */
      t->nKey = nPk;
      t->azKey = sqlite3_malloc((nPk ? nPk : 1) * (int)sizeof(char *));
      for (i = 0; i < nPk; i++) {
        int j, at = 0;
        for (j = 0; j < nPk; j++) if (aPkOrder[j] < aPkOrder[i]) at++;
        t->azKey[at] = sqlite3_mprintf("%s", azPkByOrder[i]);
      }
    }
    sqlite3_free(azPkByOrder); sqlite3_free(aPkOrder);
  }
  return SQLITE_OK;
}

/* "new.k1, new.k2" / "new.rowid" */
static char *keyList(const Tab *t, const char *zRow) {
  char *z = NULL;
  int i;
  if (t->nKey == 0) return sqlite3_mprintf("%s.rowid", zRow);
  for (i = 0; i < t->nKey; i++) {
    char *n = sqlite3_mprintf("%z%s%s.\"%w\"", z, i ? ", " : "", zRow, t->azKey[i]);
    z = n;
  }
  return z;
}

static int buildTriggers(HppliteCtx *pCtx, const Tab *t, int k) {
  char *zNewKey = keyList(t, "new"), *zOldKey = keyList(t, "old");
  char *zIns = NULL, *zUpd = NULL, *zSql;
  int i, rc, nKey = t->nKey ? t->nKey : 1;
  for (i = 0; i < t->nCol; i++) {
    zIns = sqlite3_mprintf("%z, new.\"%w\"", zIns, t->azCol[i]);
    zUpd = sqlite3_mprintf("%z, old.\"%w\", new.\"%w\"", zUpd, t->azCol[i], t->azCol[i]);
  }
  /* the insert record: key, then the stored columns (a WITHOUT ROWID
  ** table's key is among its columns: not repeated) */
  zSql = sqlite3_mprintf(
    "CREATE TEMP TRIGGER \"hpplite_cap_i%d\" AFTER INSERT ON main.\"%w\" WHEN hpplite_cap() BEGIN"
    " INSERT INTO _hpplite_pending(txn, sql, mode, now, tbl, op, rec)"
    "  VALUES(hpplite_grp(), '', 0, 0, %Q, %d, hpplite_rec(%s%s));"
    " SELECT hpplite_note(last_insert_rowid()); END;"
    "CREATE TEMP TRIGGER \"hpplite_cap_u%d\" AFTER UPDATE ON main.\"%w\" WHEN hpplite_cap() BEGIN"
    " INSERT INTO _hpplite_pending(txn, sql, mode, now, tbl, op, rec)"
    "  VALUES(hpplite_grp(), '', 0, 0, %Q, %d, hpplite_upd(%d, %s, %s%s));"
    " SELECT hpplite_note(last_insert_rowid()); END;"
    "CREATE TEMP TRIGGER \"hpplite_cap_d%d\" AFTER DELETE ON main.\"%w\" WHEN hpplite_cap() BEGIN"
    " INSERT INTO _hpplite_pending(txn, sql, mode, now, tbl, op, rec)"
    "  VALUES(hpplite_grp(), '', 0, 0, %Q, %d, hpplite_rec(%s));"
    " SELECT hpplite_note(last_insert_rowid()); END;",
    k, t->zName, t->zName, HPPLITE_ROW_INSERT, t->nKey ? "" : "new.rowid", t->nKey ? (zIns ? zIns + 2 : "") : (zIns ? zIns : ""),
    k, t->zName, t->zName, HPPLITE_ROW_UPDATE, nKey, zOldKey, zNewKey, zUpd ? zUpd : "",
    k, t->zName, t->zName, HPPLITE_ROW_DELETE, zOldKey);
  rc = zSql ? sqlite3_exec(pCtx->db, zSql, NULL, NULL, NULL) : SQLITE_NOMEM;
  sqlite3_free(zSql); sqlite3_free(zIns); sqlite3_free(zUpd); sqlite3_free(zNewKey); sqlite3_free(zOldKey);
  return rc;
}

static int schemaVersionOf(HppliteCtx *pCtx) {
  int v = -1;
  if (!pCtx->pRowsSchemaStmt
      && sqlite3_prepare_v3(pCtx->db, "PRAGMA main.schema_version", -1, SQLITE_PREPARE_PERSISTENT,
                            &pCtx->pRowsSchemaStmt, NULL) != SQLITE_OK) {
    pCtx->pRowsSchemaStmt = NULL;
    return -1;
  }
  if (sqlite3_step(pCtx->pRowsSchemaStmt) == SQLITE_ROW) v = sqlite3_column_int(pCtx->pRowsSchemaStmt, 0);
  sqlite3_reset(pCtx->pRowsSchemaStmt);
  return v;
}

/* Does zSql mention zName as a whole identifier? (case-insensitive; a
** string or comment counts too: a false hit only costs a text record) */
static int mentions(const char *zSql, const char *zName) {
  int n = (int)strlen(zName);
  const char *z;
  if (n == 0) return 0;
  for (z = zSql; *z; z++) {
    if (sqlite3_strnicmp(z, zName, n) != 0) continue;
    if (z > zSql && (isalnum((unsigned char)z[-1]) || z[-1] == '_' || z[-1] == '$' || (unsigned char)z[-1] >= 0x80)) continue;
    if (isalnum((unsigned char)z[n]) || z[n] == '_' || z[n] == '$' || (unsigned char)z[n] >= 0x80) continue;
    return 1;
  }
  return 0;
}

static int textOnlyHas(const char *zList, const char *zName) {
  const char *z;
  for (z = zList; z && *z; z += strlen(z) + 1) if (sqlite3_stricmp(z, zName) == 0) return 1;
  return 0;
}

static int textOnlyMentioned(const char *zList, const char *zSql) {
  const char *z;
  for (z = zList; z && *z; z += strlen(z) + 1) if (mentions(zSql, z)) return 1;
  return 0;
}

static int textOnlyAdd(char **pzList, int *pnList, const char *zName) {
  int n = (int)strlen(zName) + 1;
  char *z = sqlite3_realloc(*pzList, *pnList + n + 1);
  if (!z) return SQLITE_NOMEM;
  memcpy(z + *pnList, zName, n);
  *pnList += n;
  z[*pnList] = 0;
  *pzList = z;
  return SQLITE_OK;
}

/* The names whose writes are text: virtual tables, then - to a fixpoint -
** every table or view with a trigger that mentions one already listed */
static int loadTextOnly(HppliteCtx *pCtx) {
  sqlite3_stmt *q = NULL;
  char *zList = NULL;
  int nList = 0, rc, grew;
  rc = sqlite3_prepare_v2(pCtx->db,
    "SELECT name FROM pragma_table_list WHERE schema = 'main' AND type = 'virtual'", -1, &q, NULL);
  while (rc == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) {
    rc = textOnlyAdd(&zList, &nList, (const char *)sqlite3_column_text(q, 0));
  }
  sqlite3_finalize(q);
  q = NULL;
  if (rc == SQLITE_OK && zList) {
    rc = sqlite3_prepare_v2(pCtx->db,
      "SELECT tbl_name, sql FROM main.sqlite_schema WHERE type = 'trigger' AND sql IS NOT NULL", -1, &q, NULL);
    do {
      grew = 0;
      while (rc == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) {
        const char *zName = (const char *)sqlite3_column_text(q, 0);
        const char *zBody = (const char *)sqlite3_column_text(q, 1);
        if (!zName || !zBody || textOnlyHas(zList, zName) || !textOnlyMentioned(zList, zBody)) continue;
        rc = textOnlyAdd(&zList, &nList, zName);
        grew = 1;
      }
      if (q) sqlite3_reset(q);
    } while (rc == SQLITE_OK && grew);
    sqlite3_finalize(q);
  }
  if (rc != SQLITE_OK) { sqlite3_free(zList); return rc; }
  sqlite3_free(pCtx->zRowsTextOnly);
  pCtx->zRowsTextOnly = zList;
  return SQLITE_OK;
}

/* Drop every capture trigger and build them again for the current schema */
static int rebuild(HppliteCtx *pCtx) {
  sqlite3_stmt *q = NULL;
  char **azName = NULL, *zDrop = NULL;
  int n = 0, cap = 0, i, rc, v, savedInternal = pCtx->bInternal;
  pCtx->bInternal = 1;
  v = schemaVersionOf(pCtx);
  rc = sqlite3_prepare_v2(pCtx->db,
    "SELECT name FROM temp.sqlite_schema WHERE type = 'trigger' AND name LIKE 'hpplite_cap_%'", -1, &q, NULL);
  while (rc == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) {
    zDrop = sqlite3_mprintf("%zDROP TRIGGER temp.\"%w\";", zDrop, (const char *)sqlite3_column_text(q, 0));
  }
  sqlite3_finalize(q);
  if (rc == SQLITE_OK && zDrop) rc = sqlite3_exec(pCtx->db, zDrop, NULL, NULL, NULL);
  sqlite3_free(zDrop);
  /* every ordinary table of main but SQLite's and ours. Not virtual tables,
  ** which cannot carry triggers, nor their shadow tables: a trigger there
  ** makes the module's own flush open a savepoint that flushes again,
  ** without end (FTS5). Writes to them are logged as text instead. */
  if (rc == SQLITE_OK) {
    rc = sqlite3_prepare_v2(pCtx->db,
      "SELECT name FROM pragma_table_list WHERE schema = 'main' AND type = 'table'"
      " AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' AND name NOT LIKE '\\_hpplite\\_%' ESCAPE '\\' ORDER BY name",
      -1, &q, NULL);
    while (rc == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) {
      if (n == cap) { cap = cap ? cap * 2 : 16; azName = sqlite3_realloc(azName, cap * (int)sizeof(char *)); }
      azName[n++] = sqlite3_mprintf("%s", (const char *)sqlite3_column_text(q, 0));
    }
    sqlite3_finalize(q);
  }
  for (i = 0; i < n; i++) {
    if (rc == SQLITE_OK) {
      Tab t;
      rc = tabLoad(pCtx->db, azName[i], &t);
      if (rc == SQLITE_OK) rc = buildTriggers(pCtx, &t, i);
      tabFree(&t);
    }
    sqlite3_free(azName[i]);
  }
  sqlite3_free(azName);
  if (rc == SQLITE_OK) rc = loadTextOnly(pCtx);
  pCtx->rowsBuiltFor = rc == SQLITE_OK ? v : -1;
  pCtx->bInternal = savedInternal;
  return rc;
}

static int addColumn(sqlite3 *db, const char *zCol, const char *zDecl) {
  sqlite3_stmt *q = NULL;
  int has = 0, rc = SQLITE_OK;
  char *z = sqlite3_mprintf("SELECT 1 FROM pragma_table_info('_hpplite_pending') WHERE name = %Q", zCol);
  if (sqlite3_prepare_v2(db, z, -1, &q, NULL) == SQLITE_OK) has = sqlite3_step(q) == SQLITE_ROW;
  sqlite3_finalize(q);
  sqlite3_free(z);
  if (!has) {
    z = sqlite3_mprintf("ALTER TABLE _hpplite_pending ADD COLUMN %s %s", zCol, zDecl);
    rc = sqlite3_exec(db, z, NULL, NULL, NULL);
    sqlite3_free(z);
  }
  return rc;
}

int hpplite_rows_enable(HppliteCtx *pCtx) {
  int rc, savedInternal = pCtx->bInternal;
  sqlite3 *db = pCtx->db;
  pCtx->bInternal = 1;
  rc = addColumn(db, "tbl", "TEXT");
  if (rc == SQLITE_OK) rc = addColumn(db, "op", "INTEGER NOT NULL DEFAULT 0");
  if (rc == SQLITE_OK) rc = addColumn(db, "rec", "BLOB");
  pCtx->bInternal = savedInternal;
  if (rc != SQLITE_OK) return rc;
  sqlite3_create_function(db, "hpplite_rec", -1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, fnRec, NULL, NULL);
  sqlite3_create_function(db, "hpplite_upd", -1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, fnUpd, NULL, NULL);
  sqlite3_create_function(db, "hpplite_cap", 0, SQLITE_UTF8, pCtx, fnCap, NULL, NULL);
  sqlite3_create_function(db, "hpplite_grp", 0, SQLITE_UTF8, pCtx, fnGrp, NULL, NULL);
  sqlite3_create_function(db, "hpplite_note", 1, SQLITE_UTF8, pCtx, fnNote, NULL, NULL);
  pCtx->bRows = 1;
  return rebuild(pCtx);
}

int hpplite_rows_covered(HppliteCtx *pCtx) {
  return pCtx->rowsBuiltFor >= 0 && schemaVersionOf(pCtx) == pCtx->rowsBuiltFor;
}

int hpplite_rows_text_only(HppliteCtx *pCtx, const char *zSql) {
  return pCtx->zRowsTextOnly && textOnlyMentioned(pCtx->zRowsTextOnly, zSql);
}

void hpplite_rows_after_statement(HppliteCtx *pCtx) {
  if (!pCtx->bRows || hpplite_rows_covered(pCtx)) return;
  rebuild(pCtx);     /* on failure (a statement still holding the schema) the next one is text */
}

/* ------------------------------------------------------------------ replay */

typedef struct Dec { const unsigned char *a; int n, i, bad; } Dec;
static sqlite3_uint64 decVarint(Dec *d) {
  sqlite3_uint64 v = 0;
  int shift = 0;
  while (d->i < d->n && shift < 64) {
    unsigned char b = d->a[d->i++];
    v |= (sqlite3_uint64)(b & 0x7f) << shift;
    if (!(b & 0x80)) return v;
    shift += 7;
  }
  d->bad = 1;
  return 0;
}
static void decBind(Dec *d, sqlite3_stmt *st, int iParam) {
  unsigned char tag;
  if (d->i >= d->n) { d->bad = 1; return; }
  tag = d->a[d->i++];
  switch (tag) {
    case 0: sqlite3_bind_null(st, iParam); break;
    case 1: {
      sqlite3_uint64 u = decVarint(d);
      sqlite3_bind_int64(st, iParam, (sqlite3_int64)((u >> 1) ^ (~(u & 1) + 1)));
      break;
    }
    case 2: {
      sqlite3_uint64 u = 0;
      double x;
      int k;
      if (d->i + 8 > d->n) { d->bad = 1; return; }
      for (k = 0; k < 8; k++) u = (u << 8) | d->a[d->i++];
      memcpy(&x, &u, 8);
      sqlite3_bind_double(st, iParam, x);
      break;
    }
    case 3: case 4: {
      sqlite3_uint64 len = decVarint(d);
      if (d->bad || len > (sqlite3_uint64)(d->n - d->i)) { d->bad = 1; return; }
      if (tag == 3) sqlite3_bind_text(st, iParam, (const char *)d->a + d->i, (int)len, SQLITE_TRANSIENT);
      else sqlite3_bind_blob(st, iParam, d->a + d->i, (int)len, SQLITE_TRANSIENT);
      d->i += (int)len;
      break;
    }
    default: d->bad = 1;
  }
}
/* skip one value (the key of an insert into a rowid table with an INTEGER PRIMARY KEY is that column) */
static void decSkip(Dec *d) {
  unsigned char tag;
  if (d->i >= d->n) { d->bad = 1; return; }
  tag = d->a[d->i++];
  if (tag == 1) decVarint(d);
  else if (tag == 2) d->i += 8;
  else if (tag == 3 || tag == 4) d->i += (int)decVarint(d);
  if (d->i > d->n) d->bad = 1;
}

static char *whereKey(const Tab *t, int firstParam) {
  char *z = NULL;
  int i;
  if (t->nKey == 0) return sqlite3_mprintf("rowid = ?%d", firstParam);
  for (i = 0; i < t->nKey; i++) z = sqlite3_mprintf("%z%s\"%w\" = ?%d", z, i ? " AND " : "", t->azKey[i], firstParam + i);
  return z;
}

int hpplite_rows_apply(sqlite3 *db, const char *zTab, int op, const unsigned char *aRec, int nRec, char **pzErr) {
  Tab t;
  Dec d = { aRec, nRec, 0, 0 };
  sqlite3_stmt *st = NULL;
  char *zSql = NULL;
  int rc, i, n;
  *pzErr = NULL;
  rc = tabLoad(db, zTab, &t);
  if (rc != SQLITE_OK || t.nCol == 0) {
    *pzErr = sqlite3_mprintf("record for table %s, which the replica does not have", zTab);
    tabFree(&t);
    return SQLITE_ERROR;
  }
  n = (int)decVarint(&d);
  if (op == HPPLITE_ROW_INSERT) {
    /* INSERT OR REPLACE with the writer's key: a REPLACE conflict on the
    ** writer replaces the same rows here */
    char *zCols = NULL, *zVals = NULL;
    int nWant = (t.nKey ? 0 : 1) + t.nCol, p = 1;
    if (n != nWant) { *pzErr = sqlite3_mprintf("insert record for %s: %d values, table has %d", zTab, n, nWant); tabFree(&t); return SQLITE_ERROR; }
    if (!t.nKey && t.ipk < 0) { zCols = sqlite3_mprintf("rowid"); zVals = sqlite3_mprintf("?%d", p++); }
    for (i = 0; i < t.nCol; i++) {
      zCols = sqlite3_mprintf("%z%s\"%w\"", zCols, zCols ? ", " : "", t.azCol[i]);
      zVals = sqlite3_mprintf("%z%s?%d", zVals, zVals ? ", " : "", p++);
    }
    zSql = sqlite3_mprintf("INSERT OR REPLACE INTO \"%w\"(%s) VALUES(%s)", zTab, zCols, zVals);
    sqlite3_free(zCols); sqlite3_free(zVals);
    rc = sqlite3_prepare_v2(db, zSql, -1, &st, NULL);
    if (rc == SQLITE_OK) {
      p = 1;
      if (!t.nKey) { if (t.ipk < 0) decBind(&d, st, p++); else decSkip(&d); }
      for (i = 0; i < t.nCol && !d.bad; i++) decBind(&d, st, p++);
    }
  } else if (op == HPPLITE_ROW_DELETE) {
    int nKey = t.nKey ? t.nKey : 1;
    char *zWhere = whereKey(&t, 1);
    if (n != nKey) { *pzErr = sqlite3_mprintf("delete record for %s: key of %d values, table's has %d", zTab, n, nKey); sqlite3_free(zWhere); tabFree(&t); return SQLITE_ERROR; }
    zSql = sqlite3_mprintf("DELETE FROM \"%w\" WHERE %s", zTab, zWhere);
    sqlite3_free(zWhere);
    rc = sqlite3_prepare_v2(db, zSql, -1, &st, NULL);
    for (i = 0; rc == SQLITE_OK && i < nKey && !d.bad; i++) decBind(&d, st, 1 + i);
  } else if (op == HPPLITE_ROW_UPDATE) {
    /* old key (?1..), then the new rowid if it moved, then the changed columns */
    int nKey = t.nKey ? t.nKey : 1, keyChanged, nCol, at, p;
    char *zSet = NULL, *zWhere;
    const unsigned char *bitmap;
    if (n != nKey) { *pzErr = sqlite3_mprintf("update record for %s: key of %d values, table's has %d", zTab, n, nKey); tabFree(&t); return SQLITE_ERROR; }
    at = d.i;                                         /* old key values: bound after the statement exists */
    for (i = 0; i < nKey; i++) decSkip(&d);
    keyChanged = d.i < d.n ? d.a[d.i++] : (d.bad = 1, 0);
    {
      int newKeyAt = d.i;
      if (keyChanged) for (i = 0; i < nKey; i++) decSkip(&d);
      nCol = (int)decVarint(&d);
      if (d.bad || nCol != t.nCol || d.i + (nCol + 7) / 8 > d.n) {
        *pzErr = sqlite3_mprintf("update record for %s: %d columns, table has %d", zTab, nCol, t.nCol);
        tabFree(&t);
        return SQLITE_ERROR;
      }
      bitmap = d.a + d.i;
      d.i += (nCol + 7) / 8;
      p = nKey + 1;
      /* a rowid table's key moving is a column (its INTEGER PRIMARY KEY, in
      ** the bitmap) or the bare rowid */
      if (keyChanged && !t.nKey && t.ipk < 0) zSet = sqlite3_mprintf("rowid = ?%d", p++);
      for (i = 0; i < nCol; i++) if (bitmap[i / 8] & (1 << (i % 8))) zSet = sqlite3_mprintf("%z%s\"%w\" = ?%d", zSet, zSet ? ", " : "", t.azCol[i], p++);
      if (!zSet) { tabFree(&t); return SQLITE_OK; }   /* nothing changed */
      zWhere = whereKey(&t, 1);
      zSql = sqlite3_mprintf("UPDATE \"%w\" SET %s WHERE %s", zTab, zSet, zWhere);
      sqlite3_free(zSet); sqlite3_free(zWhere);
      rc = sqlite3_prepare_v2(db, zSql, -1, &st, NULL);
      if (rc == SQLITE_OK) {
        Dec k = { aRec, nRec, at, 0 };
        for (i = 0; i < nKey; i++) decBind(&k, st, 1 + i);
        p = nKey + 1;
        k.i = newKeyAt;
        if (keyChanged && !t.nKey && t.ipk < 0) decBind(&k, st, p++);
        for (i = 0; i < nCol && !d.bad; i++) if (bitmap[i / 8] & (1 << (i % 8))) decBind(&d, st, p++);
        if (k.bad) d.bad = 1;
      }
    }
  } else {
    *pzErr = sqlite3_mprintf("unknown record op %d", op);
    tabFree(&t);
    return SQLITE_ERROR;
  }
  if (rc == SQLITE_OK && d.bad) { *pzErr = sqlite3_mprintf("malformed record for %s", zTab); rc = SQLITE_CORRUPT; }
  if (rc == SQLITE_OK) {
    rc = sqlite3_step(st);
    rc = rc == SQLITE_DONE ? SQLITE_OK : rc;
    if (rc != SQLITE_OK) *pzErr = sqlite3_mprintf("%s: %s", zSql, sqlite3_errmsg(db));
  } else if (!*pzErr) {
    *pzErr = sqlite3_mprintf("%s: %s", zSql ? zSql : zTab, sqlite3_errmsg(db));
  }
  sqlite3_finalize(st);
  sqlite3_free(zSql);
  tabFree(&t);
  return rc;
}
