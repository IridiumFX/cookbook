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

struct cookbook_db {
    cookbook_db_status (*exec)(cookbook_db *db, const char *sql);
    cookbook_db_status (*query)(cookbook_db *db, const char *sql,
                               cookbook_db_row_cb cb, void *ctx);
    void              (*close)(cookbook_db *db);
};

COOKBOOK_API cookbook_db *cookbook_db_open_sqlite(const char *path);

COOKBOOK_API cookbook_db_status cookbook_db_migrate(cookbook_db *db);

#endif /* COOKBOOK_DB_H */
