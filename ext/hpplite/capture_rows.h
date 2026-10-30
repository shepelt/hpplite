/*
** HPPLite row capture (capture engine v2, #19; doc/design-row-capture.md)
**
** Each connection keeps TEMP triggers on every table of the database; they
** append one record per changed row to _hpplite_pending, inside the
** application's transaction. Replay applies the records with triggers and
** foreign keys off.
*/
#ifndef HPPLITE_CAPTURE_ROWS_H
#define HPPLITE_CAPTURE_ROWS_H

#include "hpplite_sqlite.h"

struct HppliteCtx;

#define HPPLITE_ROW_INSERT 1
#define HPPLITE_ROW_UPDATE 2
#define HPPLITE_ROW_DELETE 3

/* Turn row capture on for the connection: functions, log columns, triggers */
int hpplite_rows_enable(struct HppliteCtx *pCtx);

/* Do the triggers cover the database's current schema? (a statement on a
** table they do not cover is logged as text instead) */
int hpplite_rows_covered(struct HppliteCtx *pCtx);

/* Must this write be logged as text? (it names a virtual table, or a table
** or view whose triggers reach one) */
int hpplite_rows_text_only(struct HppliteCtx *pCtx, const char *zSql);

/* After a statement, at a point where DDL is allowed: rebuild the triggers
** if the schema moved */
void hpplite_rows_after_statement(struct HppliteCtx *pCtx);

/* Replay one record onto db (triggers and foreign keys off, the caller's
** business). Returns SQLITE_OK or an error with *pzErr set (sqlite3_free). */
int hpplite_rows_apply(sqlite3 *db, const char *zTab, int op,
                       const unsigned char *aRec, int nRec, char **pzErr);

#endif
