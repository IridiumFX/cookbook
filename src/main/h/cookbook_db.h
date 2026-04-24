#ifndef COOKBOOK_DB_H
#define COOKBOOK_DB_H

#include "cookbook.h"
#include <stddef.h>
#include <stdint.h>

typedef struct cookbook_db cookbook_db;

typedef struct cookbook_db_row {
    int            ncols;
    const char   **columns;
    const char   **values;
} cookbook_db_row;

typedef int (*cookbook_db_row_cb)(const cookbook_db_row *row, void *ctx);

typedef enum {
    COOKBOOK_DB_OK = 0,
    COOKBOOK_DB_ERROR,
    COOKBOOK_DB_CONSTRAINT,
    COOKBOOK_DB_NOT_FOUND
} cookbook_db_status;

/* Parameter binding for parameterized queries. */
typedef enum {
    COOKBOOK_DB_PARAM_TEXT,
    COOKBOOK_DB_PARAM_INT,
    COOKBOOK_DB_PARAM_NULL
} cookbook_db_param_type;

typedef struct {
    cookbook_db_param_type type;
    const char           *text;
    int64_t               integer;
} cookbook_db_param;

#define COOKBOOK_P_TEXT(s)  ((cookbook_db_param){COOKBOOK_DB_PARAM_TEXT, (s), 0})
#define COOKBOOK_P_INT(i)  ((cookbook_db_param){COOKBOOK_DB_PARAM_INT, NULL, (i)})
#define COOKBOOK_P_NULL()  ((cookbook_db_param){COOKBOOK_DB_PARAM_NULL, NULL, 0})

struct cookbook_db {
    /* Raw SQL (for DDL, migrations only — NOT for user-supplied data). */
    cookbook_db_status (*exec)(cookbook_db *db, const char *sql);
    cookbook_db_status (*query)(cookbook_db *db, const char *sql,
                               cookbook_db_row_cb cb, void *ctx);

    /* Parameterized queries — use these for all data operations. */
    cookbook_db_status (*exec_p)(cookbook_db *db, const char *sql,
                                const cookbook_db_param *params, int nparams);
    cookbook_db_status (*query_p)(cookbook_db *db, const char *sql,
                                 const cookbook_db_param *params, int nparams,
                                 cookbook_db_row_cb cb, void *ctx);

    void              (*close)(cookbook_db *db);
};

COOKBOOK_API cookbook_db *cookbook_db_open_sqlite(const char *path);

/* KV store backend — apennines kv (append-only log + hash index).
   Lightweight alternative to SQLite for Nova OS.
   path is the KV store file path (e.g. "cookbook.kv"). */
COOKBOOK_API cookbook_db *cookbook_db_open_kv(const char *path);

/* PostgreSQL backend — requires libpq.
   conninfo is a standard libpq connection string, e.g.
   "postgres://user:pass@host:5432/dbname" or keyword=value pairs.
   Returns NULL if libpq is not available or connection fails. */
COOKBOOK_API cookbook_db *cookbook_db_open_postgres(const char *conninfo);

/* Apennines t4/db/database backend — embedded SQL engine with WAL +
   B-tree + MVCC, same family as the rest of apennines. Not sqlite-file
   compatible; dump-and-load migration only. INSERT-heavy workloads
   outperform sqlite 12-20x; SELECT-heavy the reverse. */
COOKBOOK_API cookbook_db *cookbook_db_open_apennines(const char *path);

COOKBOOK_API cookbook_db_status cookbook_db_migrate(cookbook_db *db);

/* Format current UTC time into out[20] as "YYYY-MM-DD HH:MM:SS\0" —
 * the exact wire shape sqlite's datetime('now') produces. Used by
 * cookbook INSERT/UPDATE call sites that store timestamps; keeping it
 * caller-side means the SQL is portable across all four backends
 * (sqlite / apennines / PostgreSQL / kv). out must be ≥ 20 bytes. */
COOKBOOK_API void cookbook_now_iso(char out[20]);

#endif /* COOKBOOK_DB_H */
