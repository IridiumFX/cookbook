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

/* Bind parameters to a prepared statement. */
static int bind_params(sqlite3_stmt *stmt,
                        const cookbook_db_param *params, int nparams) {
    int i;
    for (i = 0; i < nparams; i++) {
        int rc;
        switch (params[i].type) {
        case COOKBOOK_DB_PARAM_TEXT:
            rc = sqlite3_bind_text(stmt, i + 1, params[i].text, -1,
                                   SQLITE_TRANSIENT);
            break;
        case COOKBOOK_DB_PARAM_INT:
            rc = sqlite3_bind_int64(stmt, i + 1, params[i].integer);
            break;
        case COOKBOOK_DB_PARAM_NULL:
            rc = sqlite3_bind_null(stmt, i + 1);
            break;
        default:
            return SQLITE_ERROR;
        }
        if (rc != SQLITE_OK) return rc;
    }
    return SQLITE_OK;
}

static cookbook_db_status sqlite_exec_p(cookbook_db *db, const char *sql,
                                        const cookbook_db_param *params,
                                        int nparams) {
    cookbook_db_sqlite *self = (cookbook_db_sqlite *)db;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return COOKBOOK_DB_ERROR;

    if (bind_params(stmt, params, nparams) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return COOKBOOK_DB_ERROR;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_CONSTRAINT)
        return COOKBOOK_DB_CONSTRAINT;
    if (rc == SQLITE_DONE || rc == SQLITE_ROW)
        return COOKBOOK_DB_OK;
    return COOKBOOK_DB_ERROR;
}

static cookbook_db_status sqlite_query_p(cookbook_db *db, const char *sql,
                                         const cookbook_db_param *params,
                                         int nparams,
                                         cookbook_db_row_cb cb, void *ctx) {
    cookbook_db_sqlite *self = (cookbook_db_sqlite *)db;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(self->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return COOKBOOK_DB_ERROR;

    if (bind_params(stmt, params, nparams) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return COOKBOOK_DB_ERROR;
    }

    int ncols = sqlite3_column_count(stmt);
    const char **columns = malloc((size_t)ncols * sizeof(char *));
    const char **values  = malloc((size_t)ncols * sizeof(char *));
    if (!columns || !values) {
        free(columns);
        free(values);
        sqlite3_finalize(stmt);
        return COOKBOOK_DB_ERROR;
    }

    for (int i = 0; i < ncols; i++)
        columns[i] = sqlite3_column_name(stmt, i);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        for (int i = 0; i < ncols; i++)
            values[i] = (const char *)sqlite3_column_text(stmt, i);

        cookbook_db_row row = { ncols, columns, values };
        if (cb(&row, ctx) != 0) break;
    }

    free(columns);
    free(values);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE || rc == SQLITE_ROW)
        return COOKBOOK_DB_OK;
    return COOKBOOK_DB_ERROR;
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

    self->base.exec    = sqlite_exec;
    self->base.query   = sqlite_query;
    self->base.exec_p  = sqlite_exec_p;
    self->base.query_p = sqlite_query_p;
    self->base.close   = sqlite_close;
    return &self->base;
}
