/*
** HPPLite clock: a pass-through VFS that can pin SQLite's idea of "now" (#56)
**
** SQLite takes the current time from the connection's VFS
** (xCurrentTimeInt64) - CURRENT_TIMESTAMP, datetime('now'), unixepoch(),
** DEFAULT CURRENT_TIMESTAMP all go through it. This VFS forwards every call
** to the VFS that was the default when it was registered, except the clock:
** while a time is pinned on the calling thread, that time is returned.
**
** The writer pins the real time when it captures a write and records it
** with the statement if the statement consulted the clock; replay pins the
** recorded time. Both sides then compute the same values.
**
** Registered once per process as the default VFS (static library: at load;
** loadable module: when it is loaded), so connections opened afterwards use
** it. A connection that names another VFS is not covered: HPPLite keeps
** refusing clock SQL there.
*/
#include "hpplite_sqlite.h"
#include "clock.h"
#include <pthread.h>
#include <string.h>

static sqlite3_vfs gClock;               /* the shim */
static sqlite3_vfs *gOrig;               /* the VFS it forwards to */
static pthread_once_t gOnce = PTHREAD_ONCE_INIT;
static int gRegistered;

static _Thread_local sqlite3_int64 tlPinned;    /* Julian day ms; 0 = not pinned */
static _Thread_local unsigned tlConsulted;      /* clock reads while pinned */

static int clockCurrentTimeInt64(sqlite3_vfs *pVfs, sqlite3_int64 *piNow) {
  (void)pVfs;
  if (tlPinned) {
    tlConsulted++;
    *piNow = tlPinned;
    return SQLITE_OK;
  }
  if (gOrig->iVersion >= 2 && gOrig->xCurrentTimeInt64) return gOrig->xCurrentTimeInt64(gOrig, piNow);
  {
    double r = 0;
    int rc = gOrig->xCurrentTime(gOrig, &r);
    *piNow = (sqlite3_int64)(r * 86400000.0);
    return rc;
  }
}
static int clockCurrentTime(sqlite3_vfs *pVfs, double *prNow) {
  sqlite3_int64 i = 0;
  int rc = clockCurrentTimeInt64(pVfs, &i);
  *prNow = i / 86400000.0;
  return rc;
}

/* everything else is the original VFS's */
static int clockOpen(sqlite3_vfs *p, const char *z, sqlite3_file *f, int fl, int *pOut) {
  (void)p; return gOrig->xOpen(gOrig, z, f, fl, pOut);
}
static int clockDelete(sqlite3_vfs *p, const char *z, int sync) { (void)p; return gOrig->xDelete(gOrig, z, sync); }
static int clockAccess(sqlite3_vfs *p, const char *z, int fl, int *pRes) { (void)p; return gOrig->xAccess(gOrig, z, fl, pRes); }
static int clockFullPathname(sqlite3_vfs *p, const char *z, int n, char *out) {
  (void)p; return gOrig->xFullPathname(gOrig, z, n, out);
}
static void *clockDlOpen(sqlite3_vfs *p, const char *z) { (void)p; return gOrig->xDlOpen(gOrig, z); }
static void clockDlError(sqlite3_vfs *p, int n, char *z) { (void)p; gOrig->xDlError(gOrig, n, z); }
static void (*clockDlSym(sqlite3_vfs *p, void *h, const char *z))(void) { (void)p; return gOrig->xDlSym(gOrig, h, z); }
static void clockDlClose(sqlite3_vfs *p, void *h) { (void)p; gOrig->xDlClose(gOrig, h); }
static int clockRandomness(sqlite3_vfs *p, int n, char *z) { (void)p; return gOrig->xRandomness(gOrig, n, z); }
static int clockSleep(sqlite3_vfs *p, int us) { (void)p; return gOrig->xSleep(gOrig, us); }
static int clockGetLastError(sqlite3_vfs *p, int n, char *z) {
  (void)p; return gOrig->xGetLastError ? gOrig->xGetLastError(gOrig, n, z) : 0;
}
static int clockSetSystemCall(sqlite3_vfs *p, const char *z, sqlite3_syscall_ptr f) {
  (void)p; return gOrig->xSetSystemCall(gOrig, z, f);
}
static sqlite3_syscall_ptr clockGetSystemCall(sqlite3_vfs *p, const char *z) { (void)p; return gOrig->xGetSystemCall(gOrig, z); }
static const char *clockNextSystemCall(sqlite3_vfs *p, const char *z) { (void)p; return gOrig->xNextSystemCall(gOrig, z); }

static void clockRegisterOnce(void) {
  gOrig = sqlite3_vfs_find(NULL);
  if (!gOrig || gOrig == &gClock) return;
  memset(&gClock, 0, sizeof(gClock));
  gClock.iVersion = gOrig->iVersion < 3 ? gOrig->iVersion : 3;
  if (gClock.iVersion < 2) gClock.iVersion = 2;      /* our xCurrentTimeInt64 is always there */
  gClock.szOsFile = gOrig->szOsFile;
  gClock.mxPathname = gOrig->mxPathname;
  gClock.zName = "hpplite";
  gClock.xOpen = clockOpen;
  gClock.xDelete = clockDelete;
  gClock.xAccess = clockAccess;
  gClock.xFullPathname = clockFullPathname;
  gClock.xDlOpen = gOrig->xDlOpen ? clockDlOpen : NULL;
  gClock.xDlError = gOrig->xDlError ? clockDlError : NULL;
  gClock.xDlSym = gOrig->xDlSym ? clockDlSym : NULL;
  gClock.xDlClose = gOrig->xDlClose ? clockDlClose : NULL;
  gClock.xRandomness = clockRandomness;
  gClock.xSleep = clockSleep;
  gClock.xCurrentTime = clockCurrentTime;
  gClock.xGetLastError = clockGetLastError;
  gClock.xCurrentTimeInt64 = clockCurrentTimeInt64;
  if (gClock.iVersion >= 3 && gOrig->iVersion >= 3) {
    gClock.xSetSystemCall = gOrig->xSetSystemCall ? clockSetSystemCall : NULL;
    gClock.xGetSystemCall = gOrig->xGetSystemCall ? clockGetSystemCall : NULL;
    gClock.xNextSystemCall = gOrig->xNextSystemCall ? clockNextSystemCall : NULL;
  } else {
    gClock.iVersion = 2;
  }
  if (sqlite3_vfs_register(&gClock, 1) == SQLITE_OK) gRegistered = 1;
}

int hpplite_clock_register(void) {
  pthread_once(&gOnce, clockRegisterOnce);
  return gRegistered ? SQLITE_OK : SQLITE_ERROR;
}

int hpplite_clock_on(sqlite3 *db) {
  sqlite3_vfs *p = NULL;
  if (!gRegistered || !db) return 0;
  if (sqlite3_file_control(db, "main", SQLITE_FCNTL_VFS_POINTER, &p) != SQLITE_OK) return 0;
  return p == &gClock;
}

sqlite3_int64 hpplite_clock_read(void) {
  sqlite3_int64 t = 0;
  sqlite3_int64 saved = tlPinned;
  if (!gOrig) return 0;
  tlPinned = 0;
  clockCurrentTimeInt64(&gClock, &t);
  tlPinned = saved;
  return t;
}

void hpplite_clock_pin(sqlite3_int64 t) {
  tlPinned = t;
  tlConsulted = 0;
}

sqlite3_int64 hpplite_clock_pinned(void) {
  return tlPinned;
}

unsigned hpplite_clock_consulted(void) {
  return tlConsulted;
}

void hpplite_clock_restore(sqlite3_int64 t, unsigned consulted) {
  tlPinned = t;
  tlConsulted = consulted;
}
