/* PostgreSQL metadata backend for cookbook.
   Implements the cookbook_db vtable using libpq.
   Compiled only when COOKBOOK_HAS_POSTGRES is defined (i.e., libpq found). */

#ifdef COOKBOOK_HAS_POSTGRES

#include "cookbook_db.h"
#include <libpq-fe.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

typedef struct {
    cookbook_db  base;
    PGconn     *conn;
} cookbook_db_postgres;

/* Translate SQLite-style ?N placeholders to PostgreSQL $N placeholders.
   Returns a malloc'd string that the caller must free. */
static char *translate_params(const char *sql) {
    size_t len = strlen(sql);
    /* worst case: each ?N becomes $N (same length for single digit) */
    char *out = malloc(len + 64);
    if (!out) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (sql[i] == '?' && i + 1 < len && sql[i+1] >= '1' && sql[i+1] <= '9') {
            out[j++] = '$';
            /* copy the digits */
            i++;
            while (i < len && sql[i] >= '0' && sql[i] <= '9') {
                out[j++] = sql[i++];
            }
            i--; /* for loop will increment */
        } else {
            out[j++] = sql[i];
        }
    }
    out[j] = '\0';
    return out;
}

/* Translate SQLite-specific SQL to PostgreSQL equivalents. */
static char *translate_sql(const char *sql) {
    char *translated = translate_params(sql);
    if (!translated) return NULL;

    /* Replace "INSERT OR IGNORE" with "INSERT ... ON CONFLICT DO NOTHING"
       This is a simple string replacement. */
    char *oi = strstr(translated, "INSERT OR IGNORE");
    if (oi) {
        /* Replace "INSERT OR IGNORE" with "INSERT             " (same length) */
        /* Then we need to add "ON CONFLICT DO NOTHING" before VALUES or at end.
           Simpler: just replace "OR IGNORE " with empty and append ON CONFLICT. */
    }

    /* Actually, let's do a proper replacement. Find "INSERT OR IGNORE INTO"
       and replace with "INSERT INTO". Then find the closing ')' of the VALUES
       and append " ON CONFLICT DO NOTHING". */
    char *result = translated;
    char *ignore_pos = strstr(result, "INSERT OR IGNORE INTO");
    if (ignore_pos) {
        /* build new string: everything before "OR IGNORE " + everything after */
        size_t prefix_len = (size_t)(ignore_pos - result) + 7; /* "INSERT " */
        const char *after = ignore_pos + 21; /* skip "INSERT OR IGNORE INTO" → "INTO..." */
        /* we want "INSERT INTO..." */
        size_t after_len = strlen(after);
        size_t new_len = prefix_len + 4 + after_len + 30; /* "INTO" + suffix + ON CONFLICT */
        char *new_sql = malloc(new_len + 1);
        if (new_sql) {
            memcpy(new_sql, result, (size_t)(ignore_pos - result));
            size_t pos = (size_t)(ignore_pos - result);
            memcpy(new_sql + pos, "INSERT INTO", 11);
            pos += 11;
            memcpy(new_sql + pos, after, after_len);
            pos += after_len;

            /* strip trailing whitespace/semicolons to append ON CONFLICT */
            while (pos > 0 && (new_sql[pos-1] == ' ' || new_sql[pos-1] == '\n' ||
                               new_sql[pos-1] == ';'))
                pos--;

            memcpy(new_sql + pos, " ON CONFLICT DO NOTHING", 23);
            pos += 23;
            new_sql[pos] = '\0';

            free(result);
            result = new_sql;
        }
    }

    return result;
}

static cookbook_db_status pg_status(const PGresult *res) {
    if (!res) return COOKBOOK_DB_ERROR;
    ExecStatusType st = PQresultStatus(res);
    if (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK)
        return COOKBOOK_DB_OK;
    /* check for unique violation (23505) */
    const char *code = PQresultErrorField(res, PG_DIAG_SQLSTATE);
    if (code && strcmp(code, "23505") == 0)
        return COOKBOOK_DB_CONSTRAINT;
    /* foreign key violation (23503) */
    if (code && strcmp(code, "23503") == 0)
        return COOKBOOK_DB_CONSTRAINT;
    return COOKBOOK_DB_ERROR;
}

static cookbook_db_status pg_exec(cookbook_db *db, const char *sql) {
    cookbook_db_postgres *self = (cookbook_db_postgres *)db;

    /* PostgreSQL migration: translate schema SQL.
       The main difference is INTEGER → same, TEXT → same.
       The schema SQL is standard enough to work directly. */
    PGresult *res = PQexec(self->conn, sql);
    cookbook_db_status st = pg_status(res);
    PQclear(res);
    return st;
}

static cookbook_db_status pg_query(cookbook_db *db, const char *sql,
                                   cookbook_db_row_cb cb, void *ctx) {
    cookbook_db_postgres *self = (cookbook_db_postgres *)db;
    PGresult *res = PQexec(self->conn, sql);
    if (!res) return COOKBOOK_DB_ERROR;

    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
        PQclear(res);
        return COOKBOOK_DB_ERROR;
    }

    int nrows = PQntuples(res);
    int ncols = PQnfields(res);

    const char **columns = malloc((size_t)ncols * sizeof(char *));
    const char **values  = malloc((size_t)ncols * sizeof(char *));
    if (!columns || !values) {
        free(columns);
        free(values);
        PQclear(res);
        return COOKBOOK_DB_ERROR;
    }

    for (int c = 0; c < ncols; c++)
        columns[c] = PQfname(res, c);

    for (int r = 0; r < nrows; r++) {
        for (int c = 0; c < ncols; c++)
            values[c] = PQgetisnull(res, r, c) ? NULL : PQgetvalue(res, r, c);

        cookbook_db_row row = { ncols, columns, values };
        if (cb(&row, ctx) != 0) break;
    }

    free(columns);
    free(values);
    PQclear(res);
    return COOKBOOK_DB_OK;
}

static cookbook_db_status pg_exec_p(cookbook_db *db, const char *sql,
                                    const cookbook_db_param *params,
                                    int nparams) {
    cookbook_db_postgres *self = (cookbook_db_postgres *)db;
    char *translated = translate_sql(sql);
    if (!translated) return COOKBOOK_DB_ERROR;

    /* build parameter arrays for PQexecParams */
    const char **values = malloc((size_t)nparams * sizeof(char *));
    int *lengths = calloc((size_t)nparams, sizeof(int));
    char **int_bufs = calloc((size_t)nparams, sizeof(char *));
    if (!values || !lengths || !int_bufs) {
        free(translated);
        free(values);
        free(lengths);
        free(int_bufs);
        return COOKBOOK_DB_ERROR;
    }

    for (int i = 0; i < nparams; i++) {
        switch (params[i].type) {
        case COOKBOOK_DB_PARAM_TEXT:
            values[i] = params[i].text;
            break;
        case COOKBOOK_DB_PARAM_INT:
            int_bufs[i] = malloc(32);
            if (int_bufs[i]) {
                snprintf(int_bufs[i], 32, "%" PRId64, params[i].integer);
                values[i] = int_bufs[i];
            }
            break;
        case COOKBOOK_DB_PARAM_NULL:
            values[i] = NULL;
            break;
        }
    }

    PGresult *res = PQexecParams(self->conn, translated, nparams,
                                  NULL, values, lengths, NULL, 0);
    cookbook_db_status st = pg_status(res);
    PQclear(res);

    for (int i = 0; i < nparams; i++)
        free(int_bufs[i]);
    free(int_bufs);
    free(values);
    free(lengths);
    free(translated);
    return st;
}

static cookbook_db_status pg_query_p(cookbook_db *db, const char *sql,
                                     const cookbook_db_param *params,
                                     int nparams,
                                     cookbook_db_row_cb cb, void *ctx) {
    cookbook_db_postgres *self = (cookbook_db_postgres *)db;
    char *translated = translate_sql(sql);
    if (!translated) return COOKBOOK_DB_ERROR;

    const char **param_values = malloc((size_t)nparams * sizeof(char *));
    int *lengths = calloc((size_t)nparams, sizeof(int));
    char **int_bufs = calloc((size_t)nparams, sizeof(char *));
    if (!param_values || !lengths || !int_bufs) {
        free(translated);
        free(param_values);
        free(lengths);
        free(int_bufs);
        return COOKBOOK_DB_ERROR;
    }

    for (int i = 0; i < nparams; i++) {
        switch (params[i].type) {
        case COOKBOOK_DB_PARAM_TEXT:
            param_values[i] = params[i].text;
            break;
        case COOKBOOK_DB_PARAM_INT:
            int_bufs[i] = malloc(32);
            if (int_bufs[i]) {
                snprintf(int_bufs[i], 32, "%" PRId64, params[i].integer);
                param_values[i] = int_bufs[i];
            }
            break;
        case COOKBOOK_DB_PARAM_NULL:
            param_values[i] = NULL;
            break;
        }
    }

    PGresult *res = PQexecParams(self->conn, translated, nparams,
                                  NULL, param_values, lengths, NULL, 0);
    free(translated);

    if (!res || (PQresultStatus(res) != PGRES_TUPLES_OK &&
                  PQresultStatus(res) != PGRES_COMMAND_OK)) {
        for (int i = 0; i < nparams; i++) free(int_bufs[i]);
        free(int_bufs);
        free(param_values);
        free(lengths);
        if (res) PQclear(res);
        return COOKBOOK_DB_ERROR;
    }

    int nrows = PQntuples(res);
    int ncols = PQnfields(res);

    const char **columns = malloc((size_t)ncols * sizeof(char *));
    const char **values  = malloc((size_t)ncols * sizeof(char *));
    if (!columns || !values) {
        free(columns);
        free(values);
        for (int i = 0; i < nparams; i++) free(int_bufs[i]);
        free(int_bufs);
        free(param_values);
        free(lengths);
        PQclear(res);
        return COOKBOOK_DB_ERROR;
    }

    for (int c = 0; c < ncols; c++)
        columns[c] = PQfname(res, c);

    for (int r = 0; r < nrows; r++) {
        for (int c = 0; c < ncols; c++)
            values[c] = PQgetisnull(res, r, c) ? NULL : PQgetvalue(res, r, c);

        cookbook_db_row row = { ncols, columns, values };
        if (cb(&row, ctx) != 0) break;
    }

    free(columns);
    free(values);
    for (int i = 0; i < nparams; i++) free(int_bufs[i]);
    free(int_bufs);
    free(param_values);
    free(lengths);
    PQclear(res);
    return COOKBOOK_DB_OK;
}

static void pg_close(cookbook_db *db) {
    cookbook_db_postgres *self = (cookbook_db_postgres *)db;
    if (self->conn) PQfinish(self->conn);
    free(self);
}

cookbook_db *cookbook_db_open_postgres(const char *conninfo) {
    cookbook_db_postgres *self = calloc(1, sizeof(*self));
    if (!self) return NULL;

    self->conn = PQconnectdb(conninfo);
    if (PQstatus(self->conn) != CONNECTION_OK) {
        fprintf(stderr, "cookbook: PostgreSQL connection failed: %s\n",
                PQerrorMessage(self->conn));
        PQfinish(self->conn);
        free(self);
        return NULL;
    }

    self->base.exec    = pg_exec;
    self->base.query   = pg_query;
    self->base.exec_p  = pg_exec_p;
    self->base.query_p = pg_query_p;
    self->base.close   = pg_close;
    return &self->base;
}

#endif /* COOKBOOK_HAS_POSTGRES */
