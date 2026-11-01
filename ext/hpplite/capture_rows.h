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
#define HPPLITE_ROW_SEQ    4   /* sqlite_sequence entry of a table, set exactly */

/* Turn row capture on for the connection: functions, log columns, triggers */
int hpplite_rows_enable(struct HppliteCtx *pCtx);
/* Undo it on a connection that lives on after hpplite_shutdown() (#66) */
void hpplite_rows_disable(struct HppliteCtx *pCtx);

/* Do the triggers cover the database's current schema? (a statement on a
** table they do not cover is logged as text instead) */
int hpplite_rows_covered(struct HppliteCtx *pCtx);
/* ...asked as a write starts: takes the write lock first (see the .c) */
/* ...as a write starts, the write lock taken first (a no-op write that waits
** busy_timeout): 1 covered, 0 not (text), HPPLITE_ROWS_BUSY the lock is held
** elsewhere past the timeout - nothing was read, the caller decides (#92) */
#define HPPLITE_ROWS_BUSY (-1)
/* ...HPPLITE_ROWS_STALE: the lock no-op failed SQLITE_SCHEMA - the schema is stale and
** stays so while the statement runs (#108); the statement is stale too */
#define HPPLITE_ROWS_STALE (-2)
int hpplite_rows_covered_for_write(struct HppliteCtx *pCtx);

/* Must this write be logged as text? (it names a virtual table, or a table
** or view whose triggers reach one) */
int hpplite_rows_text_only(struct HppliteCtx *pCtx, const char *zSql);

/* ALTER TABLE x DROP COLUMN: x (else 0); and dropping one table's triggers
** before it, which would make it fail (they are rebuilt after) */
int hpplite_rows_drop_column_target(const char *zSql, char *zOut, int nOut);
int hpplite_rows_drop_triggers_for(struct HppliteCtx *pCtx, const char *zTab);

/* After a statement, at a point where DDL is allowed: rebuild the triggers
** if the schema moved */
void hpplite_rows_after_statement(struct HppliteCtx *pCtx);

/* Replay one record onto db (triggers and foreign keys off, the caller's
** business). Returns SQLITE_OK or an error with *pzErr set (sqlite3_free). */
int hpplite_rows_apply(sqlite3 *db, const char *zTab, int op,
                       const unsigned char *aRec, int nRec, char **pzErr);

/* sqlite_sequence as it was when last looked at, and what moved since: a
** write that changed no row can still create an entry (an INSERT ... SELECT
** of nothing into a new AUTOINCREMENT table) that no record carries.
** hpplite_rows_seq_changed() lists the entries that differ from the
** snapshot ('\0'-separated name, seq pairs as text; sqlite3_free) without
** taking them in; hpplite_rows_seq_take() takes the current state in. */
char *hpplite_rows_seq_changed(struct HppliteCtx *pCtx, sqlite3 *db, int *pn, char **pzNow);
void hpplite_rows_seq_take(struct HppliteCtx *pCtx, sqlite3 *db);
/* ...and takes in the state hpplite_rows_seq_changed() read (*pzNow): the
** one the records were made from, not a later one (#91) */
void hpplite_rows_seq_install(struct HppliteCtx *pCtx, char *zSnap);
/* The record of one entry (sqlite3_free) */
unsigned char *hpplite_rows_seq_rec(sqlite3_int64 seq, int *pnRec);

/* A record as a batch entry's text (sqlite3_free), and back: applied onto
** db the way hpplite_rows_apply does */
char *hpplite_rows_entry(int op, const char *zTab, const unsigned char *aRec, int nRec);
int hpplite_rows_apply_entry(sqlite3 *db, const char *zEntry, char **pzErr);
/* What an entry is: its op and table (*pzTab: sqlite3_free) */
int hpplite_rows_entry_info(const char *zEntry, int *pOp, char **pzTab);

#endif
