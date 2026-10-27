/*
** HPPLite - SQLite API include
**
** Statically linked builds use sqlite3.h directly. The loadable-extension
** build (HPPLITE_EXTENSION) routes every sqlite3_* call through the
** sqlite3_api_routines table supplied by the host, as any SQLite extension
** must. Include this header first in every HPPLite source file.
*/
#ifndef HPPLITE_SQLITE_H
#define HPPLITE_SQLITE_H

#ifdef HPPLITE_EXTENSION
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT3
#else
#include "sqlite3.h"
#endif

#endif /* HPPLITE_SQLITE_H */
