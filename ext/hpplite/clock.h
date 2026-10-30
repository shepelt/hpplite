/*
** HPPLite clock: SQLite's "now", pinned for capture and replay (#56). See clock.c.
*/
#ifndef HPPLITE_CLOCK_H
#define HPPLITE_CLOCK_H

/* Register the clock VFS as the process default (once; later calls are no-ops) */
int hpplite_clock_register(void);
/* Does this connection take its time from the clock VFS? */
int hpplite_clock_on(sqlite3 *db);
/* The real current time, Julian day milliseconds (what xCurrentTimeInt64 returns) */
sqlite3_int64 hpplite_clock_read(void);
/* Pin "now" on the calling thread (0 = unpin); resets the consulted count */
void hpplite_clock_pin(sqlite3_int64 t);
sqlite3_int64 hpplite_clock_pinned(void);
/* Clock reads on this thread since the last pin */
unsigned hpplite_clock_consulted(void);
/* Put back a pin and count saved with hpplite_clock_pinned/_consulted */
void hpplite_clock_restore(sqlite3_int64 t, unsigned consulted);

#endif
