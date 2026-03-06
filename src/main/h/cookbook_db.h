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

/* PostgreSQL backend — requires libpq.
   conninfo is a standard libpq connection string, e.g.
   "postgres://user:pass@host:5432/dbname" or keyword=value pairs.
   Returns NULL if libpq is not available or connection fails. */
COOKBOOK_API cookbook_db *cookbook_db_open_postgres(const char *conninfo);

COOKBOOK_API cookbook_db_status cookbook_db_migrate(cookbook_db *db);

#endif /* COOKBOOK_DB_H */
