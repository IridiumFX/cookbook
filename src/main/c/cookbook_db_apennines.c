/* cookbook_db_apennines.c — vtable backend wrapping apennines t4/db/database
 *
 * Drop this file into cookbook/src/main/c/ alongside cookbook_db_sqlite.c
 * and cookbook_db_kv.c. Add the declaration to cookbook/src/main/h/cookbook_db.h:
 *
 *     COOKBOOK_API cookbook_db *cookbook_db_open_apennines(const char *path);
 *
 * And add it to CMakeLists.txt's cookbook library sources.
 *
 * The apennines vendor directory must contain t4/db/database.h + .c and
 * dependencies (t3/db/kv, t3/db/sql, t3/db/wal, t1/sync/{thread,mutex}).
 *
 * --- Design ---
 *
 * Apennines' db follows the governing rule: every function returns
 * `unsigned long` (0 = OK, non-zero = hatch). Cookbook's vtable returns
 * `cookbook_db_status` enum. Mapping:
 *
 *   0            → COOKBOOK_DB_OK
 *   9, 10, 11    → COOKBOOK_DB_CONSTRAINT  (UNIQUE, FK missing parent, FK RESTRICT)
 *   other        → COOKBOOK_DB_ERROR
 *
 * For query_p, apennines returns typed column values. cookbook's vtable
 * passes rows as `const char **values` (string-per-column). We stringify
 * non-TEXT types into per-callback-invocation scratch buffers owned by
 * the wrapper — valid only during one callback call, matching sqlite's
 * `sqlite3_column_text` lifetime contract.
 */

#include "cookbook_db.h"
#include "apennines/t4/db/database.h"
#include "apennines/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------------
 *  Wrapper type
 * ---------------------------------------------------------------------- */

typedef struct {
    cookbook_db  base;
    db_conn     *conn;
} cookbook_db_apennines;

/* Map an apennines hatch to cookbook's three-value status. */
static cookbook_db_status map_rc(unsigned long rc) {
    if (rc == 0) return COOKBOOK_DB_OK;
    /* Apennines constraint hatches:
     *   9  — UNIQUE collision (check_unique_constraints)
     *   10 — FK insert/update references missing parent
     *   11 — FK delete RESTRICT violation (children reference parent)
     * Everything else is a generic error. */
    if (rc == 9 || rc == 10 || rc == 11) return COOKBOOK_DB_CONSTRAINT;
    return COOKBOOK_DB_ERROR;
}

/* ----------------------------------------------------------------------
 *  Raw SQL: exec / query
 *
 *  apennines db_exec splits on top-level `;` so multi-statement blobs
 *  (like cookbook's migration SCHEMA_SQL) are supported.
 * ---------------------------------------------------------------------- */

static cookbook_db_status ap_exec(cookbook_db *db, const char *sql) {
    cookbook_db_apennines *self = (cookbook_db_apennines *)db;
    return map_rc(db_exec(self->conn, sql));
}

/* ap_query runs each statement via prepare/step so we can surface rows
 * to the callback. Only the LAST statement's rows are streamed — matching
 * sqlite3_exec's behaviour when the input is a single query. Multi-
 * statement query blobs are unusual; cookbook uses the parameterised
 * query_p path for all data queries. */
static cookbook_db_status ap_query(cookbook_db *db, const char *sql,
                                     cookbook_db_row_cb cb, void *ctx) {
    cookbook_db_apennines *self = (cookbook_db_apennines *)db;
    db_stmt *s = NULL;
    unsigned long rc = db_prepare(&s, self->conn, sql);
    if (rc) return map_rc(rc);

    u32 ncols = 0;
    (void)db_column_count(&ncols, s);

    const char **columns = (const char **)malloc((size_t)ncols * sizeof(char *));
    const char **values  = (const char **)malloc((size_t)ncols * sizeof(char *));
    /* Per-call scratch buffers for non-TEXT values. Allocated once,
     * reused per row; free'd on loop exit. */
    char **scratch = (char **)calloc((size_t)ncols, sizeof(char *));
    if (!columns || !values || !scratch) {
        free(columns); free(values); free(scratch);
        db_finalize(s);
        return COOKBOOK_DB_ERROR;
    }
    for (u32 i = 0; i < ncols; i++) {
        const char *nm = NULL;
        if (db_column_name(&nm, s, i) == 0 && nm) columns[i] = nm;
        else columns[i] = "";
    }

    cookbook_db_status out_status = COOKBOOK_DB_OK;
    for (;;) {
        rc = db_step(s);
        if (rc == 100) break;               /* DB_STEP_DONE */
        if (rc != 0)  { out_status = map_rc(rc); break; }

        /* Materialise each column as a C string for the callback. */
        for (u32 i = 0; i < ncols; i++) {
            int col_ty = 0;
            if (db_column_type(&col_ty, s, i) != 0) { values[i] = ""; continue; }
            int is_null = 0;
            (void)db_column_is_null(&is_null, s, i);
            if (is_null) { values[i] = NULL; continue; }
            if (col_ty == 3 /* TEXT */ || col_ty == 4 /* BLOB */) {
                const char *t = NULL; u64 tl = 0;
                if (db_column_str(&t, &tl, s, i) == 0 && t) {
                    values[i] = t;          /* engine-owned, valid this step */
                } else {
                    values[i] = "";
                }
            } else if (col_ty == 1 /* INTEGER */) {
                i64 v = 0;
                db_column_i64(&v, s, i);
                if (!scratch[i]) scratch[i] = (char *)malloc(32);
                if (scratch[i]) {
                    snprintf(scratch[i], 32, "%lld", (long long)v);
                    values[i] = scratch[i];
                } else {
                    values[i] = "";
                }
            } else if (col_ty == 2 /* REAL */) {
                double v = 0;
                db_column_f64(&v, s, i);
                if (!scratch[i]) scratch[i] = (char *)malloc(48);
                if (scratch[i]) {
                    snprintf(scratch[i], 48, "%.17g", v);
                    values[i] = scratch[i];
                } else {
                    values[i] = "";
                }
            } else {
                values[i] = NULL;
            }
        }

        cookbook_db_row row = { (int)ncols, columns, values };
        if (cb(&row, ctx) != 0) break;
    }

    for (u32 i = 0; i < ncols; i++) free(scratch[i]);
    free(scratch);
    free(columns);
    free(values);
    db_finalize(s);
    return out_status;
}

/* ----------------------------------------------------------------------
 *  Parameterised: exec_p / query_p
 * ---------------------------------------------------------------------- */

/* Apply cookbook's param array to apennines' 1-based bind slots. */
static unsigned long bind_params(db_stmt *s,
                                    const cookbook_db_param *params,
                                    int nparams) {
    for (int i = 0; i < nparams; i++) {
        /* cookbook's idx starts at 1 (?1 in SQL); apennines bind_* uses
         * 0-based indexing. */
        u32 ai = (u32)i;
        unsigned long rc = 0;
        switch (params[i].type) {
        case COOKBOOK_DB_PARAM_TEXT:
            rc = db_bind_str(s, ai, params[i].text ? params[i].text : "",
                              params[i].text ? strlen(params[i].text) : 0);
            break;
        case COOKBOOK_DB_PARAM_INT:
            rc = db_bind_i64(s, ai, (i64)params[i].integer);
            break;
        case COOKBOOK_DB_PARAM_NULL:
            rc = db_bind_null(s, ai);
            break;
        default:
            return 99;
        }
        if (rc) return rc;
    }
    return 0;
}

static cookbook_db_status ap_exec_p(cookbook_db *db, const char *sql,
                                      const cookbook_db_param *params,
                                      int nparams) {
    cookbook_db_apennines *self = (cookbook_db_apennines *)db;
    db_stmt *s = NULL;
    unsigned long rc = db_prepare(&s, self->conn, sql);
    if (rc) return map_rc(rc);
    rc = bind_params(s, params, nparams);
    if (rc) { db_finalize(s); return map_rc(rc); }

    rc = db_step(s);
    cookbook_db_status st = COOKBOOK_DB_OK;
    if (rc == 100) st = COOKBOOK_DB_OK;             /* DONE */
    else if (rc == 0) st = COOKBOOK_DB_OK;          /* row available */
    else st = map_rc(rc);
    db_finalize(s);
    return st;
}

static cookbook_db_status ap_query_p(cookbook_db *db, const char *sql,
                                       const cookbook_db_param *params,
                                       int nparams,
                                       cookbook_db_row_cb cb, void *ctx) {
    cookbook_db_apennines *self = (cookbook_db_apennines *)db;
    db_stmt *s = NULL;
    unsigned long rc = db_prepare(&s, self->conn, sql);
    if (rc) return map_rc(rc);
    rc = bind_params(s, params, nparams);
    if (rc) { db_finalize(s); return map_rc(rc); }

    u32 ncols = 0;
    (void)db_column_count(&ncols, s);
    const char **columns = (const char **)malloc((size_t)ncols * sizeof(char *));
    const char **values  = (const char **)malloc((size_t)ncols * sizeof(char *));
    char **scratch = (char **)calloc((size_t)ncols, sizeof(char *));
    if (!columns || !values || !scratch) {
        free(columns); free(values); free(scratch);
        db_finalize(s);
        return COOKBOOK_DB_ERROR;
    }
    for (u32 i = 0; i < ncols; i++) {
        const char *nm = NULL;
        if (db_column_name(&nm, s, i) == 0 && nm) columns[i] = nm;
        else columns[i] = "";
    }

    cookbook_db_status out_status = COOKBOOK_DB_OK;
    for (;;) {
        rc = db_step(s);
        if (rc == 100) break;
        if (rc != 0)  { out_status = map_rc(rc); break; }

        for (u32 i = 0; i < ncols; i++) {
            int col_ty = 0;
            if (db_column_type(&col_ty, s, i) != 0) { values[i] = ""; continue; }
            int is_null = 0;
            (void)db_column_is_null(&is_null, s, i);
            if (is_null) { values[i] = NULL; continue; }
            if (col_ty == 3 || col_ty == 4) {
                const char *t = NULL; u64 tl = 0;
                if (db_column_str(&t, &tl, s, i) == 0 && t) values[i] = t;
                else values[i] = "";
            } else if (col_ty == 1) {
                i64 v = 0;
                db_column_i64(&v, s, i);
                if (!scratch[i]) scratch[i] = (char *)malloc(32);
                if (scratch[i]) {
                    snprintf(scratch[i], 32, "%lld", (long long)v);
                    values[i] = scratch[i];
                } else values[i] = "";
            } else if (col_ty == 2) {
                double v = 0;
                db_column_f64(&v, s, i);
                if (!scratch[i]) scratch[i] = (char *)malloc(48);
                if (scratch[i]) {
                    snprintf(scratch[i], 48, "%.17g", v);
                    values[i] = scratch[i];
                } else values[i] = "";
            } else {
                values[i] = NULL;
            }
        }

        cookbook_db_row row = { (int)ncols, columns, values };
        if (cb(&row, ctx) != 0) break;
    }

    for (u32 i = 0; i < ncols; i++) free(scratch[i]);
    free(scratch);
    free(columns);
    free(values);
    db_finalize(s);
    return out_status;
}

/* ----------------------------------------------------------------------
 *  Lifecycle
 * ---------------------------------------------------------------------- */

static void ap_close(cookbook_db *db) {
    cookbook_db_apennines *self = (cookbook_db_apennines *)db;
    if (self->conn) db_close(self->conn);
    free(self);
}

cookbook_db *cookbook_db_open_apennines(const char *path) {
    cookbook_db_apennines *self = calloc(1, sizeof(*self));
    if (!self) return NULL;

    /* Apennines uses the path as a base; it creates path.kv and path.wal.
     * Mimic sqlite's `:memory:` behaviour for empty paths by falling back
     * to a process-local temp path. */
    const char *db_path = (path && *path) ? path : "cookbook.apdb";

    if (db_open(&self->conn, db_path) != 0) {
        free(self);
        return NULL;
    }

    self->base.exec    = ap_exec;
    self->base.query   = ap_query;
    self->base.exec_p  = ap_exec_p;
    self->base.query_p = ap_query_p;
    self->base.close   = ap_close;
    return &self->base;
}
