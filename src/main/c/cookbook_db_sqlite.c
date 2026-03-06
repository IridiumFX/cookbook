#include "cookbook_db.h"
#include "sqlite3.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    cookbook_db  base;
    sqlite3    *handle;
} cookbook_db_sqlite;

static cookbook_db_status sqlite_exec(cookbook_db *db, const char *sql) {
    cookbook_db_sqlite *self = (cookbook_db_sqlite *)db;
    char *err = NULL;
    int rc = sqlite3_exec(self->handle, sql, NULL, NULL, &err);
    if (err) sqlite3_free(err);
    if (rc == SQLITE_CONSTRAINT) return COOKBOOK_DB_CONSTRAINT;
    return rc == SQLITE_OK ? COOKBOOK_DB_OK : COOKBOOK_DB_ERROR;
}

static int sqlite_query_cb(void *ctx_pair, int ncols,
                            char **values, char **columns) {
    void **pair = (void **)ctx_pair;
    cookbook_db_row_cb cb = (cookbook_db_row_cb)pair[0];
    void *user_ctx = pair[1];
    cookbook_db_row row;
    row.ncols   = ncols;
    row.columns = (const char **)columns;
    row.values  = (const char **)values;
    return cb(&row, user_ctx);
}

static cookbook_db_status sqlite_query(cookbook_db *db, const char *sql,
                                       cookbook_db_row_cb cb, void *ctx) {
    cookbook_db_sqlite *self = (cookbook_db_sqlite *)db;
    char *err = NULL;
    void *pair[2] = { (void *)cb, ctx };
    int rc = sqlite3_exec(self->handle, sql, sqlite_query_cb, pair, &err);
    if (err) sqlite3_free(err);
    return rc == SQLITE_OK ? COOKBOOK_DB_OK : COOKBOOK_DB_ERROR;
}

static void sqlite_close(cookbook_db *db) {
    cookbook_db_sqlite *self = (cookbook_db_sqlite *)db;
    if (self->handle) sqlite3_close(self->handle);
    free(self);
}

cookbook_db *cookbook_db_open_sqlite(const char *path) {
    cookbook_db_sqlite *self = calloc(1, sizeof(*self));
    if (!self) return NULL;

    const char *db_path = (path && *path) ? path : ":memory:";
    int rc = sqlite3_open(db_path, &self->handle);
    if (rc != SQLITE_OK) {
        if (self->handle) sqlite3_close(self->handle);
        free(self);
        return NULL;
    }

    sqlite3_exec(self->handle, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(self->handle, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);

    self->base.exec  = sqlite_exec;
    self->base.query = sqlite_query;
    self->base.close = sqlite_close;
    return &self->base;
}
