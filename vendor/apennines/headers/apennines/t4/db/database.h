#ifndef APENNINES_T4_DATABASE_H
#define APENNINES_T4_DATABASE_H

#include "apennines/export.h"
#include "apennines/types.h"

/* ================================================================
 *  Embedded Database Engine
 *  Composes: WAL + SQL Parser + B-Tree + Buffer Pool + MVCC
 * ================================================================ */

typedef struct db_conn db_conn;
typedef struct db_stmt db_stmt;

/* Column types */
#define DB_TYPE_NULL    0
#define DB_TYPE_INTEGER 1
#define DB_TYPE_REAL    2
#define DB_TYPE_TEXT    3
#define DB_TYPE_BLOB    4

/* ---- Connection ---- */

APENNINES_API unsigned long db_open(db_conn **out, const char *path);
APENNINES_API unsigned long db_close(db_conn *db);

/* ---- Execute (no result set) ---- */
APENNINES_API unsigned long db_exec(db_conn *db, const char *sql);

/* ---- Prepared statements ---- */
APENNINES_API unsigned long db_prepare(db_stmt **out, db_conn *db, const char *sql);
APENNINES_API unsigned long db_bind_i64(db_stmt *s, u32 idx, i64 val);
APENNINES_API unsigned long db_bind_f64(db_stmt *s, u32 idx, double val);
APENNINES_API unsigned long db_bind_str(db_stmt *s, u32 idx, const char *val, u64 len);
APENNINES_API unsigned long db_bind_bytes(db_stmt *s, u32 idx, const u8 *val, u64 len);
APENNINES_API unsigned long db_bind_null(db_stmt *s, u32 idx);

/* Step returns 0=row available, hatch 100=done, other=error */
APENNINES_API unsigned long db_step(db_stmt *s);

APENNINES_API unsigned long db_column_count(u32 *out, db_stmt *s);
APENNINES_API unsigned long db_column_name(const char **out, db_stmt *s, u32 idx);
APENNINES_API unsigned long db_column_type(int *out, db_stmt *s, u32 idx);
APENNINES_API unsigned long db_column_i64(i64 *out, db_stmt *s, u32 idx);
APENNINES_API unsigned long db_column_f64(double *out, db_stmt *s, u32 idx);
APENNINES_API unsigned long db_column_str(const char **out, u64 *out_len, db_stmt *s, u32 idx);
APENNINES_API unsigned long db_column_bytes(const u8 **out, u64 *out_len, db_stmt *s, u32 idx);
APENNINES_API unsigned long db_column_is_null(int *out, db_stmt *s, u32 idx);

APENNINES_API unsigned long db_finalize(db_stmt *s);
APENNINES_API unsigned long db_reset(db_stmt *s);

/* ---- Transactions ---- */
APENNINES_API unsigned long db_begin(db_conn *db);
APENNINES_API unsigned long db_commit(db_conn *db);
APENNINES_API unsigned long db_rollback(db_conn *db);

/* ---- Utilities ---- */
APENNINES_API unsigned long db_last_insert_id(i64 *out, db_conn *db);
APENNINES_API unsigned long db_changes(u64 *out, db_conn *db);
APENNINES_API unsigned long db_table_exists(int *out, db_conn *db, const char *name);
APENNINES_API unsigned long db_backup(db_conn *db, const char *dest_path);
APENNINES_API unsigned long db_vacuum(db_conn *db);
APENNINES_API unsigned long db_integrity_check(int *out_ok, db_conn *db);

/* ---- Extended Query ---- */
APENNINES_API unsigned long db_query(db_stmt **out, db_conn *db, const char *sql);

/* ---- Extended Transactions ---- */
APENNINES_API unsigned long db_begin_immediate(db_conn *db);
APENNINES_API unsigned long db_savepoint(db_conn *db, const char *name);
APENNINES_API unsigned long db_release_savepoint(db_conn *db, const char *name);
APENNINES_API unsigned long db_rollback_to_savepoint(db_conn *db, const char *name);

/* ---- Decimal binds ---- */
APENNINES_API unsigned long db_bind_decimal(db_stmt *s, u32 idx,
                                             const char *decimal_str);
APENNINES_API unsigned long db_column_decimal(const char **out, db_stmt *s, u32 idx);

/* ---- Pragmas / configuration ---- */
APENNINES_API unsigned long db_set_busy_timeout(db_conn *db, u64 ms);
APENNINES_API unsigned long db_set_journal_mode(db_conn *db, const char *mode);
APENNINES_API unsigned long db_set_page_size(db_conn *db, u32 size);

/* ---- Schema introspection ---- */
APENNINES_API unsigned long db_table_list(const char ***out, u64 *out_count,
                                           db_conn *db);

/* ---- WAL checkpoint ---- */
APENNINES_API unsigned long db_checkpoint(db_conn *db);

#endif
