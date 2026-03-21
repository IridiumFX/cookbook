/*
 * cookbook_db_kv.c — KV store backend for cookbook_db vtable
 *
 * Uses apennines kv (append-only log + hash index) as a lightweight
 * alternative to SQLite. Designed for Nova OS where a full SQL engine
 * is overkill.
 *
 * Data model: each table row is stored as a pasta map, keyed by
 * "{table}:{primary_key}". Queries are mapped from SQL patterns to
 * KV get/put/delete/prefix-iterate operations.
 *
 * Limitations:
 * - No JOINs, aggregates, or complex WHERE clauses
 * - ORDER BY not supported (iteration order is insertion order)
 * - Only supports the specific SQL patterns used by cookbook
 */

#include "cookbook_db.h"
#include <apennines/t3/db/kv.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    cookbook_db base;
    kv_store  *store;
} cookbook_db_kv;

/* ---- helpers ---- */

/* Build a KV key: "table:pk_value" */
static void kv_key(char *out, size_t sz, const char *table, const char *pk) {
    snprintf(out, sz, "%s:%s", table, pk);
}

/* Store a row as "col1=val1\tcol2=val2\t..." */
static char *encode_row(const char **cols, const char **vals, int ncols) {
    size_t total = 0;
    for (int i = 0; i < ncols; i++)
        total += strlen(cols[i]) + 1 + strlen(vals[i]) + 1;
    char *buf = malloc(total + 1);
    if (!buf) return NULL;
    size_t off = 0;
    for (int i = 0; i < ncols; i++) {
        off += (size_t)sprintf(buf + off, "%s=%s\t", cols[i], vals[i]);
    }
    if (off > 0) buf[off - 1] = '\0'; /* strip trailing tab */
    return buf;
}

/* Decode a row from "col1=val1\tcol2=val2\t..." into a callback */
static int decode_row(const char *data, size_t len,
                        cookbook_db_row_cb cb, void *ctx) {
    /* count fields */
    int ncols = 1;
    for (size_t i = 0; i < len; i++)
        if (data[i] == '\t') ncols++;

    const char **cols = calloc((size_t)ncols, sizeof(char *));
    const char **vals = calloc((size_t)ncols, sizeof(char *));
    char *buf = malloc(len + 1);
    if (buf) { memcpy(buf, data, len); buf[len] = '\0'; }
    if (!cols || !vals || !buf) {
        free(cols); free(vals); free(buf);
        return -1;
    }

    /* parse "col=val" pairs */
    int idx = 0;
    char *p = buf;
    while (p && idx < ncols) {
        char *tab = strchr(p, '\t');
        if (tab) *tab = '\0';
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = '\0';
            cols[idx] = p;
            vals[idx] = eq + 1;
            idx++;
        }
        p = tab ? tab + 1 : NULL;
    }

    cookbook_db_row row = { .ncols = idx, .columns = cols, .values = vals };
    int rc = cb(&row, ctx);

    free(buf);
    free(cols);
    free(vals);
    return rc;
}

/* ---- SQL pattern matching ---- */

/* Extract table name from SQL: "... FROM tablename" or "INTO tablename" */
static const char *sql_table(const char *sql, char *out, size_t sz) {
    const char *p = strstr(sql, " FROM ");
    if (!p) p = strstr(sql, " from ");
    if (!p) p = strstr(sql, " INTO ");
    if (!p) p = strstr(sql, " into ");
    if (!p) {
        /* UPDATE tablename SET ... */
        p = strstr(sql, "UPDATE ");
        if (!p) p = strstr(sql, "update ");
        if (p) p += 6; /* skip "UPDATE" */
        else {
            /* DELETE FROM tablename */
            p = strstr(sql, "DELETE FROM ");
            if (!p) p = strstr(sql, "delete from ");
            if (p) p += 11;
        }
    } else {
        p += 6; /* skip " FROM " or " INTO " */
    }
    if (!p) return NULL;
    while (*p == ' ') p++;
    size_t i = 0;
    while (p[i] && p[i] != ' ' && p[i] != '(' && p[i] != ';' && i < sz - 1) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return out;
}

/* ---- vtable implementation ---- */

static cookbook_db_status kv_exec(cookbook_db *db, const char *sql) {
    cookbook_db_kv *self = (cookbook_db_kv *)db;

    /* CREATE TABLE — no-op (schema-less) */
    if (strstr(sql, "CREATE TABLE") || strstr(sql, "CREATE INDEX"))
        return COOKBOOK_DB_OK;

    /* DELETE without WHERE — clear all rows of a table */
    char table[64];
    if (strstr(sql, "DELETE") && !strstr(sql, "WHERE")) {
        if (sql_table(sql, table, sizeof(table))) {
            /* iterate prefix and delete all */
            char prefix[72];
            snprintf(prefix, sizeof(prefix), "%s:", table);
            kv_iter *it = NULL;
            if (kv_iter_create(&it, self->store,
                                (const u8 *)prefix, (u64)strlen(prefix)) == 0) {
                const u8 *ik, *iv;
                u64 ikl, ivl;
                while (kv_iter_next(&ik, &ikl, &iv, &ivl, it) == 0) {
                    kv_delete(self->store, ik, ikl);
                }
                kv_iter_destroy(it);
            }
            return COOKBOOK_DB_OK;
        }
    }

    return COOKBOOK_DB_OK;
}

static cookbook_db_status kv_query(cookbook_db *db, const char *sql,
                                    cookbook_db_row_cb cb, void *ctx) {
    cookbook_db_kv *self = (cookbook_db_kv *)db;
    char table[64];
    if (!sql_table(sql, table, sizeof(table)))
        return COOKBOOK_DB_ERROR;

    /* SELECT ... FROM table — iterate all rows */
    char prefix[72];
    snprintf(prefix, sizeof(prefix), "%s:", table);
    kv_iter *it = NULL;
    if (kv_iter_create(&it, self->store,
                        (const u8 *)prefix, (u64)strlen(prefix)) != 0)
        return COOKBOOK_DB_ERROR;

    const u8 *ik, *iv;
    u64 ikl, ivl;
    while (kv_iter_next(&ik, &ikl, &iv, &ivl, it) == 0) {
        decode_row((const char *)iv, (size_t)ivl, cb, ctx);
    }
    kv_iter_destroy(it);
    return COOKBOOK_DB_OK;
}

static cookbook_db_status kv_exec_p(cookbook_db *db, const char *sql,
                                      const cookbook_db_param *params,
                                      int nparams) {
    cookbook_db_kv *self = (cookbook_db_kv *)db;
    char table[64];

    /* CREATE TABLE — no-op */
    if (strstr(sql, "CREATE TABLE") || strstr(sql, "CREATE INDEX"))
        return COOKBOOK_DB_OK;

    if (!sql_table(sql, table, sizeof(table)))
        return COOKBOOK_DB_ERROR;

    if (strstr(sql, "INSERT") || strstr(sql, "insert")) {
        /* INSERT — first param is primary key */
        if (nparams < 1 || params[0].type != COOKBOOK_DB_PARAM_TEXT)
            return COOKBOOK_DB_ERROR;

        char key[256];
        kv_key(key, sizeof(key), table, params[0].text);

        /* check for OR IGNORE — if key exists, return OK */
        if (strstr(sql, "OR IGNORE") || strstr(sql, "or ignore")) {
            u8 *existing = NULL;
            u64 elen = 0;
            if (kv_get(&existing, &elen, self->store,
                        (const u8 *)key, (u64)strlen(key)) == 0) {
                free(existing);
                return COOKBOOK_DB_OK;
            }
        }

        /* check for uniqueness (no OR IGNORE) */
        if (!strstr(sql, "OR IGNORE") && !strstr(sql, "OR REPLACE")) {
            u8 *existing = NULL;
            u64 elen = 0;
            if (kv_get(&existing, &elen, self->store,
                        (const u8 *)key, (u64)strlen(key)) == 0) {
                free(existing);
                return COOKBOOK_DB_CONSTRAINT;
            }
        }

        /* encode params as col=val pairs */
        /* extract column names from SQL: INSERT INTO table (col1, col2, ...) */
        const char *paren = strchr(sql, '(');
        if (!paren) return COOKBOOK_DB_ERROR;
        paren++;
        char colbuf[1024];
        const char *endparen = strchr(paren, ')');
        if (!endparen) return COOKBOOK_DB_ERROR;
        size_t clen = (size_t)(endparen - paren);
        if (clen >= sizeof(colbuf)) clen = sizeof(colbuf) - 1;
        memcpy(colbuf, paren, clen);
        colbuf[clen] = '\0';

        /* parse column names */
        const char *colnames[32];
        int ncols = 0;
        char *cp = colbuf;
        while (cp && ncols < 32) {
            while (*cp == ' ') cp++;
            colnames[ncols++] = cp;
            char *comma = strchr(cp, ',');
            if (comma) { *comma = '\0'; cp = comma + 1; }
            else break;
        }
        /* trim trailing spaces from column names */
        for (int i = 0; i < ncols; i++) {
            char *end = (char *)colnames[i] + strlen(colnames[i]) - 1;
            while (end > colnames[i] && *end == ' ') *end-- = '\0';
        }

        /* build value strings */
        const char *valstrs[32];
        char intbufs[32][32];
        for (int i = 0; i < nparams && i < ncols; i++) {
            if (params[i].type == COOKBOOK_DB_PARAM_TEXT)
                valstrs[i] = params[i].text ? params[i].text : "";
            else if (params[i].type == COOKBOOK_DB_PARAM_INT) {
                snprintf(intbufs[i], sizeof(intbufs[i]), "%d", params[i].integer);
                valstrs[i] = intbufs[i];
            } else {
                valstrs[i] = "";
            }
        }

        char *encoded = encode_row(colnames, valstrs,
                                     nparams < ncols ? nparams : ncols);
        if (!encoded) return COOKBOOK_DB_ERROR;

        unsigned long rc = kv_put(self->store, (const u8 *)key, (u64)strlen(key),
                                    (const u8 *)encoded, (u64)strlen(encoded));
        free(encoded);
        return rc == 0 ? COOKBOOK_DB_OK : COOKBOOK_DB_ERROR;
    }

    if (strstr(sql, "DELETE") || strstr(sql, "delete")) {
        /* DELETE FROM table WHERE pk = ?1 */
        if (nparams < 1 || params[0].type != COOKBOOK_DB_PARAM_TEXT)
            return COOKBOOK_DB_ERROR;
        char key[256];
        kv_key(key, sizeof(key), table, params[0].text);
        kv_delete(self->store, (const u8 *)key, (u64)strlen(key));
        return COOKBOOK_DB_OK;
    }

    if (strstr(sql, "UPDATE") || strstr(sql, "update")) {
        /* UPDATE table SET col = ?1 WHERE pk = ?N
           Pattern: params[0..N-2] are SET values, params[N-1] is WHERE pk.
           Extract column names from "SET col1 = ?1, col2 = ?2" */
        if (nparams < 2) return COOKBOOK_DB_ERROR;
        const char *pk = params[nparams - 1].text;
        if (!pk) return COOKBOOK_DB_ERROR;

        char key[256];
        kv_key(key, sizeof(key), table, pk);

        /* read existing row */
        u8 *existing = NULL;
        u64 elen = 0;
        if (kv_get(&existing, &elen, self->store,
                    (const u8 *)key, (u64)strlen(key)) != 0)
            return COOKBOOK_DB_NOT_FOUND;

        /* decode existing row into a mutable buffer */
        char *row = malloc(elen + 1);
        if (!row) { free(existing); return COOKBOOK_DB_ERROR; }
        memcpy(row, existing, elen);
        row[elen] = '\0';
        free(existing);

        /* parse SET clause: "SET col1 = ?1, col2 = ?2" */
        const char *set = strstr(sql, " SET ");
        if (!set) set = strstr(sql, " set ");
        if (set) {
            set += 5;
            /* for each SET assignment, find the column name and param index */
            int pidx = 0;
            const char *p = set;
            while (*p && pidx < nparams - 1) {
                while (*p == ' ') p++;
                /* column name */
                const char *col_start = p;
                while (*p && *p != ' ' && *p != '=') p++;
                size_t col_len = (size_t)(p - col_start);
                char colname[128] = {0};
                if (col_len >= sizeof(colname)) col_len = sizeof(colname) - 1;
                memcpy(colname, col_start, col_len);

                /* skip " = ?N" */
                while (*p && *p != '?') p++;
                if (*p == '?') p++;
                while (*p >= '0' && *p <= '9') p++;
                if (*p == ',') p++;

                /* get the new value */
                const char *newval = "";
                char intbuf[32];
                if (params[pidx].type == COOKBOOK_DB_PARAM_TEXT)
                    newval = params[pidx].text ? params[pidx].text : "";
                else if (params[pidx].type == COOKBOOK_DB_PARAM_INT) {
                    snprintf(intbuf, sizeof(intbuf), "%d", (int)params[pidx].integer);
                    newval = intbuf;
                }

                /* find and replace "colname=oldval" in row */
                char search[136];
                snprintf(search, sizeof(search), "%s=", colname);
                char *found = strstr(row, search);
                if (found) {
                    /* rebuild row with new value */
                    size_t prefix_len = (size_t)(found - row);
                    char *tab = strchr(found, '\t');
                    size_t suffix_start = tab ? (size_t)(tab - row) : strlen(row);
                    size_t new_entry_len = strlen(colname) + 1 + strlen(newval);
                    size_t new_row_len = prefix_len + new_entry_len + (strlen(row) - suffix_start);
                    char *newrow = malloc(new_row_len + 1);
                    if (newrow) {
                        memcpy(newrow, row, prefix_len);
                        size_t off = prefix_len;
                        off += (size_t)sprintf(newrow + off, "%s=%s", colname, newval);
                        memcpy(newrow + off, row + suffix_start, strlen(row) - suffix_start);
                        newrow[off + strlen(row) - suffix_start] = '\0';
                        free(row);
                        row = newrow;
                    }
                }
                pidx++;
                /* skip to WHERE */
                if (strstr(p, "WHERE") || strstr(p, "where")) break;
            }
        }

        /* write back */
        unsigned long rc = kv_put(self->store, (const u8 *)key, (u64)strlen(key),
                                    (const u8 *)row, (u64)strlen(row));
        free(row);
        return rc == 0 ? COOKBOOK_DB_OK : COOKBOOK_DB_ERROR;
    }

    return COOKBOOK_DB_OK;
}

static cookbook_db_status kv_query_p(cookbook_db *db, const char *sql,
                                       const cookbook_db_param *params,
                                       int nparams,
                                       cookbook_db_row_cb cb, void *ctx) {
    cookbook_db_kv *self = (cookbook_db_kv *)db;
    char table[64];
    if (!sql_table(sql, table, sizeof(table)))
        return COOKBOOK_DB_ERROR;

    /* SELECT ... WHERE primary_key = ?1 */
    if (nparams >= 1 && params[0].type == COOKBOOK_DB_PARAM_TEXT &&
        strstr(sql, "WHERE") && strstr(sql, "= ?1")) {
        char key[256];
        kv_key(key, sizeof(key), table, params[0].text);

        u8 *val = NULL;
        u64 vlen = 0;
        if (kv_get(&val, &vlen, self->store,
                    (const u8 *)key, (u64)strlen(key)) == 0) {
            decode_row((const char *)val, (size_t)vlen, cb, ctx);
            free(val);
        }
        return COOKBOOK_DB_OK;
    }

    /* SELECT ... WHERE col > ?1 (range query — iterate and filter) */
    /* For simplicity, iterate all and let the callback handle filtering */
    char prefix[72];
    snprintf(prefix, sizeof(prefix), "%s:", table);
    kv_iter *it = NULL;
    if (kv_iter_create(&it, self->store,
                        (const u8 *)prefix, (u64)strlen(prefix)) != 0)
        return COOKBOOK_DB_ERROR;

    const u8 *ik, *iv;
    u64 ikl, ivl;
    while (kv_iter_next(&ik, &ikl, &iv, &ivl, it) == 0) {
        decode_row((const char *)iv, (size_t)ivl, cb, ctx);
    }
    kv_iter_destroy(it);
    return COOKBOOK_DB_OK;
}

static void kv_db_close(cookbook_db *db) {
    cookbook_db_kv *self = (cookbook_db_kv *)db;
    if (self->store) kv_close(self->store);
    free(self);
}

/* ---- Public API ---- */

cookbook_db *cookbook_db_open_kv(const char *path) {
    cookbook_db_kv *self = calloc(1, sizeof(*self));
    if (!self) return NULL;

    if (kv_open(&self->store, path) != 0) {
        free(self);
        return NULL;
    }

    self->base.exec    = kv_exec;
    self->base.query   = kv_query;
    self->base.exec_p  = kv_exec_p;
    self->base.query_p = kv_query_p;
    self->base.close   = kv_db_close;

    return &self->base;
}
