#include "apennines/t4/db/database.h"
#include "apennines/t4/db/db_storage.h"
#include "apennines/t3/db/kv.h"
#include "apennines/t3/db/wal.h"
#include "apennines/t3/db/sql.h"
#include "apennines/t1/sync/mutex/mutex.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define DB_META_PREFIX      "__meta__"
#define DB_META_PREFIX_LEN  8
/* DB-wide header key. First open of a fresh KV writes one; subsequent opens
 * verify magic and version-gate. Existing KV stores without this key are
 * treated as legacy v1 (current format) and have a header written on open. */
#define DB_HDR_KEY          "__apdb_hdr__"
#define DB_HDR_KEY_LEN      12
#define DB_HDR_MAGIC        0x42444150u   /* 'APDB' little-endian */
#define DB_HDR_VERSION_CUR  1u            /* current on-disk format version */
#define DB_HDR_SIZE         16            /* magic(4) + version(2) + reserved(2) + flags(8) */
#define DB_MAX_COLUMNS      64
#define DB_MAX_TABLES       256
#define DB_MAX_KEY_LEN      512
#define DB_MAX_ROW_SIZE     65536
#define DB_STEP_DONE        100
#define DB_BIND_MAX         64
#define DB_WAL_OP_PUT       1
#define DB_WAL_OP_DELETE    2

/* ------------------------------------------------------------------ */
/*  Row serialization format                                           */
/*                                                                     */
/*  [u16 col_count]                                                    */
/*  for each column:                                                   */
/*    [u8 type]                                                        */
/*    if INTEGER: [i64 value]          (8 bytes LE)                    */
/*    if REAL:    [double value]       (8 bytes LE)                    */
/*    if TEXT:    [u32 len][len bytes]                                  */
/*    if BLOB:    [u32 len][len bytes]                                 */
/*    if NULL:    (nothing)                                            */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Table metadata                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char  name[128];
    int   type;         /* DB_TYPE_* */
    int   primary_key;  /* 1 if PRIMARY KEY */
    int   not_null;     /* 1 if NOT NULL */
    int   unique;       /* 1 if column-level UNIQUE */
} db_col_def;

/* Composite UNIQUE constraint. Up to DB_MAX_UNIQUE_SETS per table,
 * each spanning up to DB_MAX_COLUMNS columns (by index). Single-column
 * UNIQUE (declared inline as "col TYPE UNIQUE") is captured both as
 * the per-col flag AND as a 1-column unique_set, so all enforcement
 * runs through the same path. */
#define DB_MAX_UNIQUE_SETS 8
#define DB_MAX_FKS         16
#define DB_MAX_INDEXES     16
#define DB_IDX_PREFIX      "__idx__/"
#define DB_IDX_PREFIX_LEN  8

typedef struct {
    u32 col_count;
    u32 cols[DB_MAX_COLUMNS];  /* column indices into db_table.cols */
} db_unique_set;

/* FK on_delete action codes. Stored per-FK and dispatched by
 * exec_delete when a parent row's removal would violate the reference. */
#define DB_FK_NOACTION    0
#define DB_FK_RESTRICT    1
#define DB_FK_CASCADE     2
#define DB_FK_SETNULL     3
#define DB_FK_SETDEFAULT  4

#define DB_FK_MAX_COLS 8

/* FK reference. Supports composite keys up to DB_FK_MAX_COLS columns;
 * single-column FKs use col_count=1. */
typedef struct {
    u32  col_count;
    u32  local_cols[DB_FK_MAX_COLS];            /* indices into table.cols */
    char target_table[128];
    char target_cols[DB_FK_MAX_COLS][128];       /* col names in parent */
    u8   on_delete;  /* DB_FK_* — defaults to NOACTION = reject if child refs */
} db_fk;

#define DB_IDX_MAX_COLS 8

/* Secondary index. Supports composite indexes up to DB_IDX_MAX_COLS
 * columns; the planner uses the index for any equality hit on the
 * first column (longer prefix matches get tighter iteration). */
typedef struct {
    char name[128];
    u32  col_count;
    u32  col_idx[DB_IDX_MAX_COLS];
} db_index;

typedef struct {
    char          name[128];
    db_col_def    cols[DB_MAX_COLUMNS];
    u32           col_count;
    i64           next_rowid;
    db_unique_set unique_sets[DB_MAX_UNIQUE_SETS];
    u32           unique_set_count;
    db_fk         fks[DB_MAX_FKS];
    u32           fk_count;
    db_index      indexes[DB_MAX_INDEXES];
    u32           index_count;
} db_table;

/* ------------------------------------------------------------------ */
/*  Connection                                                         */
/* ------------------------------------------------------------------ */

/* Undo-log entry for txn rollback. Each DML op that touches the KV
 * store during a transaction pushes one of these; db_rollback replays
 * them in reverse. `prev_val == NULL` means the key didn't exist before
 * (rollback should delete). */
typedef struct {
    u8  *key;
    u64  klen;
    u8  *prev_val;
    u64  prev_vlen;
    int  existed_before;  /* 1 = prev_val valid; 0 = key was absent */
} db_undo_entry;

struct db_conn {
    const db_storage_vt *vt;
    void      *storage;
    wal       *log;
    char       path[512];
    db_table   tables[DB_MAX_TABLES];
    u32        table_count;
    int        in_txn;
    i64        last_insert_id;
    u64        changes;
    /* configuration */
    u64        busy_timeout_ms;
    char       journal_mode[32];
    u32        page_size;
    /* Reader/writer lock. SELECTs + getters take the read side and
     * parallelise; INSERT/UPDATE/DELETE/DDL/begin/commit/rollback take
     * the write side and exclude everything. Two threads on the *same*
     * db_stmt remain a programmer error (the statement's iter/cur_row
     * would clobber each other) — document in the API. */
    rwlock     conn_lock;
    int        conn_lock_initialised;
    /* Undo log. Only populated when in_txn=1; cleared on commit,
     * replayed in reverse on rollback. */
    db_undo_entry *undo;
    u64            undo_count;
    u64            undo_cap;
};

/* ------------------------------------------------------------------ */
/*  Bind value                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    int     type;       /* DB_TYPE_* */
    i64     ival;
    double  fval;
    u8     *bval;       /* allocated copy for TEXT/BLOB */
    u64     blen;
} db_bind_val;

/* ------------------------------------------------------------------ */
/*  Deserialized row                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    u32  col_count;
    int  types[DB_MAX_COLUMNS];
    i64  ivals[DB_MAX_COLUMNS];
    double fvals[DB_MAX_COLUMNS];
    u8  *bvals[DB_MAX_COLUMNS];
    u64  blens[DB_MAX_COLUMNS];
} db_row;

/* ------------------------------------------------------------------ */
/*  Statement                                                          */
/* ------------------------------------------------------------------ */

/* Pointer into the AST at a literal node whose text is a "?" placeholder.
 * Resolved on each db_step by rewriting node->text from the bind at `index`. */
typedef struct {
    sql_ast *node;   /* literal node inside stmt->ast (owned by AST) */
    u32      index;  /* 0-based bind slot */
} db_placeholder;

typedef struct {
    u32 col_idx;
    int descending;
} db_order_spec;

/* Aggregate function descriptor populated at db_prepare time for every
 * SELECT column whose text starts with "__AGG__". */
typedef enum {
    DB_AGG_COUNT = 0,
    DB_AGG_SUM,
    DB_AGG_MIN,
    DB_AGG_MAX,
    DB_AGG_AVG
} db_agg_fn;

typedef struct {
    db_agg_fn  fn;
    int        star;        /* 1 if arg is *; only meaningful for COUNT */
    int        col_idx;     /* column index in table, -1 if star */
    /* running state, accumulated during aggregate_and_build_row */
    i64        count_nonnull;
    i64        isum;
    double     dsum;
    int        saw_real;    /* 1 once a REAL value joins the sum */
    i64        imin, imax;
    double     fmin, fmax;
    int        have_any;    /* 1 once MIN/MAX has seen at least one value */
} db_agg_spec;

struct db_stmt {
    db_conn      *db;
    sql_ast      *ast;
    sql_token_list tokens;

    /* bind slots */
    db_bind_val   binds[DB_BIND_MAX];
    u32           bind_count;

    /* placeholder nodes in the AST that need bind substitution each step */
    db_placeholder placeholders[DB_BIND_MAX];
    u32           placeholder_count;

    /* iteration state for SELECT */
    db_storage_iter *iter;
    int           done;         /* 1 when iteration finished */
    db_table     *table;        /* target table for SELECT */
    db_row        cur_row;      /* current row from last db_step */
    int           have_row;     /* 1 if cur_row is valid */

    /* parsed column list for SELECT */
    char         *sel_cols[DB_MAX_COLUMNS];
    u32           sel_col_count;
    int           sel_star;     /* 1 if SELECT * */

    /* LIMIT / OFFSET. has_limit=0 means unlimited. */
    int           has_limit;
    u64           limit_n;
    u64           offset_n;
    u64           rows_returned;   /* toward LIMIT */
    u64           rows_skipped;    /* toward OFFSET (streaming path only) */

    /* ORDER BY path. If order_count > 0, the first db_step materialises
     * every matching row, sorts by (spec[0], spec[1], ...), applies
     * OFFSET+LIMIT, then serves rows from `sorted_rows` sequentially. */
    db_order_spec order_specs[DB_MAX_COLUMNS];
    u32           order_count;
    int           materialised;
    db_row       *sorted_rows;
    u64           sorted_count;
    u64           sorted_pos;

    /* Aggregate SELECT. If agg_count > 0, the stmt has at least one
     * aggregate column (COUNT/SUM/MIN/MAX/AVG). The first db_step
     * walks every matching row, accumulates each spec, builds a
     * single synthetic row in cur_row, and returns it; the second
     * step returns DB_STEP_DONE. */
    db_agg_spec   agg_specs[DB_MAX_COLUMNS];
    u32           agg_count;
    int           agg_produced;

    /* GROUP BY. group_col_count == 0 means no GROUP BY. For each
     * group column, group_col_indices holds the source-table col
     * index. Bucket keys concatenate all group col values; the
     * emitted row lays out group cols first (0..N-1), then each
     * aggregate value (N..N+agg_count-1). */
#define DB_MAX_GROUP_COLS 8
    u32           group_col_indices[DB_MAX_GROUP_COLS];
    u32           group_col_count;
    const sql_ast *having_node;  /* non-NULL if HAVING present */

    /* Synthetic table that mirrors the layout of a group-row so that
     * eval_where can resolve HAVING predicates against it. col 0 is
     * the group column (real name from source table); cols 1..N are
     * aggregates, named "__agg_FN_arg__" — HAVING's SQL_AST_AGG_REF
     * nodes are rewritten to COLUMN_REFs with those names at prepare
     * time. */
    db_table      synth_group_table;
    int           synth_group_table_built;

    /* Planner state — when using_index is set, s->iter yields index
     * entries, and each k needs to be translated to a row_key. For
     * IDX_USE_RANGE iteration, we also filter each entry's first-col
     * value against idx_plan.lower/upper. */
    int           using_index;

    /* Index plan, populated when planner_build returns non-zero. */
    struct {
        int          kind;          /* IDX_USE_NONE / _EQ / _RANGE */
        int          col_type;      /* DB_TYPE_* of indexed first col */
        int          has_lower;
        int          lower_inclusive;
        char         lower[256];
        int          has_upper;
        int          upper_inclusive;
        char         upper[256];
    }             idx_plan;
    u64           idx_prefix_len;  /* prefix length used for kv_iter */
};

/* Index-plan discriminator values for db_stmt.idx_plan.kind. */
#define IDX_USE_NONE  0
#define IDX_USE_EQ    1
#define IDX_USE_RANGE 2

/* ------------------------------------------------------------------ */
/*  LE helpers                                                         */
/* ------------------------------------------------------------------ */

static void write_u16_le(u8 *dst, u16 v) {
    dst[0] = (u8)(v);
    dst[1] = (u8)(v >> 8);
}

static u16 read_u16_le(const u8 *src) {
    return (u16)src[0] | ((u16)src[1] << 8);
}

static void write_u32_le(u8 *dst, u32 v) {
    dst[0] = (u8)(v);
    dst[1] = (u8)(v >> 8);
    dst[2] = (u8)(v >> 16);
    dst[3] = (u8)(v >> 24);
}

static u32 read_u32_le(const u8 *src) {
    return (u32)src[0]
         | ((u32)src[1] << 8)
         | ((u32)src[2] << 16)
         | ((u32)src[3] << 24);
}

static void write_i64_le(u8 *dst, i64 v) {
    u64 u = (u64)v;
    for (int i = 0; i < 8; ++i) { dst[i] = (u8)(u >> (i * 8)); }
}

static i64 read_i64_le(const u8 *src) {
    u64 u = 0;
    for (int i = 0; i < 8; ++i) { u |= (u64)src[i] << (i * 8); }
    return (i64)u;
}

static void write_f64_le(u8 *dst, double v) {
    u64 u;
    memcpy(&u, &v, 8);
    for (int i = 0; i < 8; ++i) { dst[i] = (u8)(u >> (i * 8)); }
}

static double read_f64_le(const u8 *src) {
    u64 u = 0;
    for (int i = 0; i < 8; ++i) { u |= (u64)src[i] << (i * 8); }
    double v;
    memcpy(&v, &u, 8);
    return v;
}

/* ------------------------------------------------------------------ */
/*  String helpers                                                     */
/* ------------------------------------------------------------------ */

static int strcasecmp_a(const char *a, const char *b) {
    while (*a && *b) {
        int d = toupper((unsigned char)*a) - toupper((unsigned char)*b);
        if (d != 0) return d;
        a++; b++;
    }
    return toupper((unsigned char)*a) - toupper((unsigned char)*b);
}

static int strncasecmp_a(const char *a, const char *b, u64 n) {
    for (u64 i = 0; i < n; ++i) {
        if (!a[i] && !b[i]) return 0;
        int d = toupper((unsigned char)a[i]) - toupper((unsigned char)b[i]);
        if (d != 0) return d;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  DB-wide header                                                     */
/*                                                                     */
/*  Layout (16 bytes):                                                 */
/*    [u32 magic='APDB'][u16 version][u16 reserved][u64 flags]         */
/*  Stored at key DB_HDR_KEY. Ensures we can detect when a KV store    */
/*  was written by a future format and refuse to open it (rather than  */
/*  silently corrupt), and gives us a version to gate future           */
/*  migrations on.                                                     */
/* ------------------------------------------------------------------ */

static void hdr_serialize(u8 *buf) {
    write_u32_le(buf, DB_HDR_MAGIC);
    write_u16_le(buf + 4, (u16)DB_HDR_VERSION_CUR);
    write_u16_le(buf + 6, 0);
    for (int i = 0; i < 8; ++i) buf[8 + i] = 0;
}

/* Parse header bytes. Returns 0 on success (fills *out_version),
 * -1 on bad magic, -2 on unsupported version, -3 on short buffer. */
static int hdr_parse(const u8 *buf, u64 len, u16 *out_version) {
    if (len < DB_HDR_SIZE) return -3;
    u32 m = read_u32_le(buf);
    if (m != DB_HDR_MAGIC) return -1;
    u16 v = read_u16_le(buf + 4);
    if (v == 0 || v > DB_HDR_VERSION_CUR) return -2;
    *out_version = v;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Metadata serialization                                             */
/*                                                                     */
/*  Format: [u16 col_count][i64 next_rowid]                            */
/*  per column: [u8 type][u8 pk][u8 notnull][u16 name_len][name]      */
/* ------------------------------------------------------------------ */

static u64 meta_serialize(const db_table *t, u8 *buf, u64 cap) {
    u64 pos = 0;
    if (pos + 2 > cap) return 0;
    write_u16_le(buf + pos, (u16)t->col_count); pos += 2;
    if (pos + 8 > cap) return 0;
    write_i64_le(buf + pos, t->next_rowid); pos += 8;
    for (u32 i = 0; i < t->col_count; ++i) {
        const db_col_def *c = &t->cols[i];
        u16 nlen = (u16)strlen(c->name);
        /* 4 bytes of per-col flags (type, pk, not_null, unique) + len + name */
        if (pos + 4 + 2 + nlen > cap) return 0;
        buf[pos++] = (u8)c->type;
        buf[pos++] = (u8)c->primary_key;
        buf[pos++] = (u8)c->not_null;
        buf[pos++] = (u8)c->unique;
        write_u16_le(buf + pos, nlen); pos += 2;
        memcpy(buf + pos, c->name, nlen); pos += nlen;
    }
    /* Trailing v2+ payload: unique_set_count then each set. Older
     * v1 meta rows end here; deserialise checks pos < len before
     * reading and tolerates absence. */
    if (pos + 1 > cap) return 0;
    buf[pos++] = (u8)t->unique_set_count;
    for (u32 i = 0; i < t->unique_set_count; ++i) {
        const db_unique_set *us = &t->unique_sets[i];
        if (pos + 1 + 2 * us->col_count > cap) return 0;
        buf[pos++] = (u8)us->col_count;
        for (u32 j = 0; j < us->col_count; ++j) {
            write_u16_le(buf + pos, (u16)us->cols[j]);
            pos += 2;
        }
    }
    /* v3 trailer: fk_count then each FK. Older v2 meta rows end here;
     * deserialise gates on pos < len. */
    if (pos + 1 > cap) return 0;
    buf[pos++] = (u8)t->fk_count;
    for (u32 i = 0; i < t->fk_count; ++i) {
        const db_fk *fk = &t->fks[i];
        u16 tt_len = (u16)strlen(fk->target_table);
        /* Format: [u8 col_count][col_count × u16 local_col]
         *         [u16 tt_len][target_table]
         *         [per-col: u16 tc_len][target_col_name]
         *         [u8 on_delete] */
        u64 needed = 1 + 2 * fk->col_count + 2 + tt_len;
        for (u32 j = 0; j < fk->col_count; ++j) {
            needed += 2 + strlen(fk->target_cols[j]);
        }
        needed += 1;
        if (pos + needed > cap) return 0;
        buf[pos++] = (u8)fk->col_count;
        for (u32 j = 0; j < fk->col_count; ++j) {
            write_u16_le(buf + pos, (u16)fk->local_cols[j]);
            pos += 2;
        }
        write_u16_le(buf + pos, tt_len); pos += 2;
        memcpy(buf + pos, fk->target_table, tt_len); pos += tt_len;
        for (u32 j = 0; j < fk->col_count; ++j) {
            u16 tc_len = (u16)strlen(fk->target_cols[j]);
            write_u16_le(buf + pos, tc_len); pos += 2;
            memcpy(buf + pos, fk->target_cols[j], tc_len); pos += tc_len;
        }
        buf[pos++] = fk->on_delete;
    }
    /* v4 trailer: index_count then each index. Composite-capable:
     * each index writes col_count, col_count × u16 col_idx, name_len,
     * name. */
    if (pos + 1 > cap) return 0;
    buf[pos++] = (u8)t->index_count;
    for (u32 i = 0; i < t->index_count; ++i) {
        const db_index *ix = &t->indexes[i];
        u16 nl = (u16)strlen(ix->name);
        if (pos + 1 + 2 * ix->col_count + 2 + nl > cap) return 0;
        buf[pos++] = (u8)ix->col_count;
        for (u32 j = 0; j < ix->col_count; ++j) {
            write_u16_le(buf + pos, (u16)ix->col_idx[j]);
            pos += 2;
        }
        write_u16_le(buf + pos, nl); pos += 2;
        memcpy(buf + pos, ix->name, nl); pos += nl;
    }
    return pos;
}

static int meta_deserialize(db_table *t, const u8 *buf, u64 len) {
    u64 pos = 0;
    if (pos + 2 > len) return -1;
    t->col_count = read_u16_le(buf + pos); pos += 2;
    if (t->col_count > DB_MAX_COLUMNS) return -1;
    if (pos + 8 > len) return -1;
    t->next_rowid = read_i64_le(buf + pos); pos += 8;

    /* v1 per-column format had 3 flag bytes (type, pk, not_null); v2
     * adds a 4th (unique). We detect which format we're reading by
     * pre-computing what each layout would need and seeing which fits
     * the actual remaining buffer. If both fit (possible when name
     * lengths make it ambiguous), prefer v2 — the code that wrote v1
     * never adds the trailing unique_set_count byte, so meta_deserialize
     * for v1 leaves unique_set_count=0 regardless. */

    /* Try v2 first (4 flag bytes per column). */
    u64 saved_pos = pos;
    int ok_v2 = 1;
    for (u32 i = 0; i < t->col_count && ok_v2; ++i) {
        if (pos + 6 > len) { ok_v2 = 0; break; }
        /* skip type,pk,not_null,unique */
        pos += 4;
        u16 nlen = read_u16_le(buf + pos); pos += 2;
        if (pos + nlen > len || nlen >= 128) { ok_v2 = 0; break; }
        pos += nlen;
    }
    int v2 = ok_v2;
    pos = saved_pos;

    /* Read columns in the detected layout. */
    for (u32 i = 0; i < t->col_count; ++i) {
        db_col_def *c = &t->cols[i];
        if (pos + (v2 ? 6 : 5) > len) return -1;
        c->type = buf[pos++];
        c->primary_key = buf[pos++];
        c->not_null = buf[pos++];
        c->unique = v2 ? buf[pos++] : 0;
        u16 nlen = read_u16_le(buf + pos); pos += 2;
        if (pos + nlen > len || nlen >= 128) return -1;
        memcpy(c->name, buf + pos, nlen);
        c->name[nlen] = '\0';
        pos += nlen;
    }

    /* Optional trailing unique_set block. Absent in v1 rows — that
     * produces unique_set_count=0 (already zero from calloc). */
    t->unique_set_count = 0;
    if (pos < len) {
        u32 nsets = buf[pos++];
        if (nsets > DB_MAX_UNIQUE_SETS) return -1;
        for (u32 i = 0; i < nsets; ++i) {
            if (pos + 1 > len) return -1;
            u32 n = buf[pos++];
            if (n > DB_MAX_COLUMNS) return -1;
            if (pos + 2 * n > len) return -1;
            db_unique_set *us = &t->unique_sets[i];
            us->col_count = n;
            for (u32 j = 0; j < n; ++j) {
                us->cols[j] = read_u16_le(buf + pos);
                pos += 2;
            }
        }
        t->unique_set_count = nsets;
    }

    /* Optional trailing FK block (v3). */
    t->fk_count = 0;
    if (pos < len) {
        u32 nfk = buf[pos++];
        if (nfk > DB_MAX_FKS) return -1;
        for (u32 i = 0; i < nfk; ++i) {
            if (pos + 1 > len) return -1;
            db_fk *fk = &t->fks[i];
            u32 cc = buf[pos++];
            if (cc == 0 || cc > DB_FK_MAX_COLS) return -1;
            fk->col_count = cc;
            if (pos + 2 * cc > len) return -1;
            for (u32 j = 0; j < cc; ++j) {
                fk->local_cols[j] = read_u16_le(buf + pos);
                pos += 2;
            }
            if (pos + 2 > len) return -1;
            u16 tt_len = read_u16_le(buf + pos); pos += 2;
            if (pos + tt_len > len || tt_len >= sizeof(fk->target_table)) return -1;
            memcpy(fk->target_table, buf + pos, tt_len);
            fk->target_table[tt_len] = '\0';
            pos += tt_len;
            for (u32 j = 0; j < cc; ++j) {
                if (pos + 2 > len) return -1;
                u16 tc_len = read_u16_le(buf + pos); pos += 2;
                if (pos + tc_len > len || tc_len >= sizeof(fk->target_cols[j])) return -1;
                memcpy(fk->target_cols[j], buf + pos, tc_len);
                fk->target_cols[j][tc_len] = '\0';
                pos += tc_len;
            }
            if (pos < len) {
                fk->on_delete = buf[pos++];
                if (fk->on_delete > DB_FK_SETDEFAULT) fk->on_delete = DB_FK_NOACTION;
            } else {
                fk->on_delete = DB_FK_NOACTION;
            }
        }
        t->fk_count = nfk;
    }
    /* Optional trailing index block (v4). Composite-capable:
     * [u8 col_count][col_count × u16 col_idx][u16 nl][name]. */
    t->index_count = 0;
    if (pos < len) {
        u32 nix = buf[pos++];
        if (nix > DB_MAX_INDEXES) return -1;
        for (u32 i = 0; i < nix; ++i) {
            if (pos + 1 > len) return -1;
            db_index *ix = &t->indexes[i];
            u32 cc = buf[pos++];
            if (cc == 0 || cc > DB_IDX_MAX_COLS) return -1;
            if (pos + 2 * cc > len) return -1;
            ix->col_count = cc;
            for (u32 j = 0; j < cc; ++j) {
                ix->col_idx[j] = read_u16_le(buf + pos);
                pos += 2;
            }
            if (pos + 2 > len) return -1;
            u16 nl = read_u16_le(buf + pos); pos += 2;
            if (pos + nl > len || nl >= sizeof(ix->name)) return -1;
            memcpy(ix->name, buf + pos, nl);
            ix->name[nl] = '\0';
            pos += nl;
        }
        t->index_count = nix;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Row serialization                                                  */
/* ------------------------------------------------------------------ */

static u64 row_serialize(const db_row *r, u8 *buf, u64 cap) {
    u64 pos = 0;
    if (pos + 2 > cap) return 0;
    write_u16_le(buf + pos, (u16)r->col_count); pos += 2;
    for (u32 i = 0; i < r->col_count; ++i) {
        if (pos + 1 > cap) return 0;
        buf[pos++] = (u8)r->types[i];
        switch (r->types[i]) {
        case DB_TYPE_INTEGER:
            if (pos + 8 > cap) return 0;
            write_i64_le(buf + pos, r->ivals[i]); pos += 8;
            break;
        case DB_TYPE_REAL:
            if (pos + 8 > cap) return 0;
            write_f64_le(buf + pos, r->fvals[i]); pos += 8;
            break;
        case DB_TYPE_TEXT:
        case DB_TYPE_BLOB: {
            u32 blen = (u32)r->blens[i];
            if (pos + 4 + blen > cap) return 0;
            write_u32_le(buf + pos, blen); pos += 4;
            if (blen > 0 && r->bvals[i]) {
                memcpy(buf + pos, r->bvals[i], blen);
            }
            pos += blen;
            break;
        }
        default: /* NULL */
            break;
        }
    }
    return pos;
}

static int row_deserialize(db_row *r, const u8 *buf, u64 len) {
    memset(r, 0, sizeof(*r));
    u64 pos = 0;
    if (pos + 2 > len) return -1;
    r->col_count = read_u16_le(buf + pos); pos += 2;
    if (r->col_count > DB_MAX_COLUMNS) return -1;
    for (u32 i = 0; i < r->col_count; ++i) {
        if (pos + 1 > len) return -1;
        r->types[i] = buf[pos++];
        switch (r->types[i]) {
        case DB_TYPE_INTEGER:
            if (pos + 8 > len) return -1;
            r->ivals[i] = read_i64_le(buf + pos); pos += 8;
            break;
        case DB_TYPE_REAL:
            if (pos + 8 > len) return -1;
            r->fvals[i] = read_f64_le(buf + pos); pos += 8;
            break;
        case DB_TYPE_TEXT:
        case DB_TYPE_BLOB: {
            if (pos + 4 > len) return -1;
            u32 blen = read_u32_le(buf + pos); pos += 4;
            if (pos + blen > len) return -1;
            r->bvals[i] = malloc(blen + 1);
            if (!r->bvals[i]) return -1;
            memcpy(r->bvals[i], buf + pos, blen);
            r->bvals[i][blen] = '\0';
            r->blens[i] = blen;
            pos += blen;
            break;
        }
        default: /* NULL */
            break;
        }
    }
    return 0;
}

static void row_free(db_row *r) {
    for (u32 i = 0; i < r->col_count; ++i) {
        if (r->bvals[i]) { free(r->bvals[i]); r->bvals[i] = NULL; }
    }
}

/* ------------------------------------------------------------------ */
/*  KV key builders                                                    */
/* ------------------------------------------------------------------ */

static u64 build_meta_key(u8 *buf, u64 cap, const char *table) {
    int n = snprintf((char *)buf, cap, "%s%s", DB_META_PREFIX, table);
    return (n > 0 && (u64)n < cap) ? (u64)n : 0;
}

static u64 build_row_key(u8 *buf, u64 cap, const char *table, i64 rowid) {
    int n = snprintf((char *)buf, cap, "%s/%lld", table, (long long)rowid);
    return (n > 0 && (u64)n < cap) ? (u64)n : 0;
}

static u64 build_prefix_key(u8 *buf, u64 cap, const char *table) {
    int n = snprintf((char *)buf, cap, "%s/", table);
    return (n > 0 && (u64)n < cap) ? (u64)n : 0;
}

/* Index-entry key schema:
 *   __idx__/<table>/<idx_name>/<val_text>/<rowid_decimal>
 * This lets us do exact-match lookups via prefix iteration
 * ("__idx__/<table>/<idx_name>/<val_text>/") and derive the
 * matching rowid from the key's trailing segment.
 *
 * Values are stored as their printable form (for TEXT/INT/REAL);
 * BLOBs with embedded nulls are indexed only on their length-
 * terminated text projection. Good enough for cookbook's string-
 * and-integer indexed columns. */

static int col_val_to_text(char *buf, u64 cap, const db_row *row, u32 ci) {
    if (ci >= row->col_count) return -1;
    switch (row->types[ci]) {
    case DB_TYPE_NULL:
        snprintf(buf, cap, "%s", "__NULL__");
        return 0;
    case DB_TYPE_INTEGER:
        snprintf(buf, cap, "%lld", (long long)row->ivals[ci]);
        return 0;
    case DB_TYPE_REAL:
        snprintf(buf, cap, "%.17g", row->fvals[ci]);
        return 0;
    case DB_TYPE_TEXT:
    case DB_TYPE_BLOB: {
        u64 l = row->blens[ci];
        if (l >= cap) l = cap - 1;
        if (l && row->bvals[ci]) memcpy(buf, row->bvals[ci], l);
        buf[l] = '\0';
        return 0;
    }
    }
    return -1;
}

/* Build a composite-index entry key of the form
 *   __idx__/<table>/<idx>/<v1>/<v2>/.../<vN>/<rowid>
 * row + col_idx[] provides the values. Returns 0 on buffer overflow. */
static u64 build_idx_entry_key_composite(u8 *buf, u64 cap,
                                           const char *table,
                                           const char *idx_name,
                                           const db_row *row,
                                           const u32 *cols, u32 nc,
                                           i64 rowid) {
    int n = snprintf((char *)buf, cap, "%s%s/%s/", DB_IDX_PREFIX, table, idx_name);
    if (n < 0 || (u64)n >= cap) return 0;
    u64 pos = (u64)n;
    for (u32 i = 0; i < nc; ++i) {
        char valtext[256];
        if (col_val_to_text(valtext, sizeof(valtext), row, cols[i]) != 0) return 0;
        int nn = snprintf((char *)buf + pos, cap - pos, "%s/", valtext);
        if (nn < 0 || (u64)nn >= cap - pos) return 0;
        pos += (u64)nn;
    }
    int nn = snprintf((char *)buf + pos, cap - pos, "%lld", (long long)rowid);
    if (nn < 0 || (u64)nn >= cap - pos) return 0;
    pos += (u64)nn;
    return pos;
}

/* Prefix for "first K column values" — useful for the planner. */
static u64 build_idx_val_prefix_composite(u8 *buf, u64 cap,
                                             const char *table,
                                             const char *idx_name,
                                             const char **val_texts, u32 nv) {
    int n = snprintf((char *)buf, cap, "%s%s/%s/", DB_IDX_PREFIX, table, idx_name);
    if (n < 0 || (u64)n >= cap) return 0;
    u64 pos = (u64)n;
    for (u32 i = 0; i < nv; ++i) {
        int nn = snprintf((char *)buf + pos, cap - pos, "%s/", val_texts[i]);
        if (nn < 0 || (u64)nn >= cap - pos) return 0;
        pos += (u64)nn;
    }
    return pos;
}

static u64 build_idx_all_prefix(u8 *buf, u64 cap, const char *table,
                                  const char *idx_name) {
    int n = snprintf((char *)buf, cap, "%s%s/%s/",
                     DB_IDX_PREFIX, table, idx_name);
    return (n > 0 && (u64)n < cap) ? (u64)n : 0;
}

/* Parse rowid from the tail of an index-entry key. Returns -1 if
 * not well-formed. */
static i64 parse_rowid_from_idx_key(const u8 *k, u64 klen) {
    if (klen == 0) return -1;
    /* Find last '/'. */
    i64 last_slash = -1;
    for (i64 i = (i64)klen - 1; i >= 0; --i) {
        if (k[i] == '/') { last_slash = i; break; }
    }
    if (last_slash < 0 || (u64)(last_slash + 1) >= klen) return -1;
    char tmp[32]; tmp[0] = '\0';
    u64 n = klen - (u64)last_slash - 1;
    if (n >= sizeof(tmp)) return -1;
    memcpy(tmp, k + last_slash + 1, n);
    tmp[n] = '\0';
    return (i64)strtoll(tmp, NULL, 10);
}

/* ------------------------------------------------------------------ */
/*  Table lookup                                                       */
/* ------------------------------------------------------------------ */

static db_table *find_table(db_conn *db, const char *name) {
    for (u32 i = 0; i < db->table_count; ++i) {
        if (strcasecmp_a(db->tables[i].name, name) == 0) {
            return &db->tables[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Flush table metadata to KV                                         */
/* ------------------------------------------------------------------ */

/* Forward declarations — flush_meta now hooks into the undo log for
 * DDL-in-txn rollback, and wal_put/wal_del are defined further down. */
static void push_undo(db_conn *db, const u8 *key, u64 klen);
static unsigned long wal_put(db_conn *db, const u8 *key, u64 klen,
                               const u8 *val, u64 vlen);
static unsigned long wal_del(db_conn *db, const u8 *key, u64 klen);

static unsigned long flush_meta(db_conn *db, db_table *t) {
    u8 key_buf[DB_MAX_KEY_LEN];
    u64 klen = build_meta_key(key_buf, sizeof(key_buf), t->name);
    if (klen == 0) return 1;

    u8 val_buf[DB_MAX_ROW_SIZE];
    u64 vlen = meta_serialize(t, val_buf, sizeof(val_buf));
    if (vlen == 0) return 2;

    /* Meta goes straight to KV (which has its own durability), plus a
     * push_undo entry when in_txn so DDL rollback can undo it. We
     * skip the outer WAL append — meta changes are frequent (every
     * INSERT bumps next_rowid) and the double-log was the regression
     * culprit observed after 000205. kv's own append-only log is the
     * durability path of record. */
    push_undo(db, key_buf, klen);
    return db->vt->put(db->storage, key_buf, klen, val_buf, vlen);
}

/* ------------------------------------------------------------------ */
/*  Parse column type from SQL string                                  */
/* ------------------------------------------------------------------ */

static int parse_col_type(const char *type_str) {
    if (!type_str) return DB_TYPE_TEXT;
    if (strncasecmp_a(type_str, "INT", 3) == 0) return DB_TYPE_INTEGER;
    if (strncasecmp_a(type_str, "REAL", 4) == 0) return DB_TYPE_REAL;
    if (strncasecmp_a(type_str, "BLOB", 4) == 0) return DB_TYPE_BLOB;
    return DB_TYPE_TEXT;
}

/* ------------------------------------------------------------------ */
/*  WAL-logged put/delete                                              */
/* ------------------------------------------------------------------ */

/* Push an undo record for the key as it exists *right now*. Called
 * right before wal_put or wal_del so we can revert on rollback.
 * No-op outside a transaction. */
static void push_undo(db_conn *db, const u8 *key, u64 klen) {
    if (!db->in_txn) return;
    if (db->undo_count >= db->undo_cap) {
        u64 nc = db->undo_cap ? db->undo_cap * 2 : 32;
        db_undo_entry *tmp = (db_undo_entry *)realloc(db->undo,
                                                      nc * sizeof(db_undo_entry));
        if (!tmp) return;  /* silent drop — rollback will be incomplete;
                              callers should keep transactions small */
        db->undo = tmp;
        db->undo_cap = nc;
    }
    db_undo_entry *e = &db->undo[db->undo_count];
    memset(e, 0, sizeof(*e));
    e->key = (u8 *)malloc(klen);
    if (!e->key) return;
    memcpy(e->key, key, klen);
    e->klen = klen;

    u8 *prev = NULL; u64 plen = 0;
    unsigned long rc = db->vt->get(&prev, &plen, db->storage, key, klen);
    if (rc == 0 && prev) {
        e->prev_val = prev;   /* allocated by kv_get, we own it */
        e->prev_vlen = plen;
        e->existed_before = 1;
    } else {
        e->existed_before = 0;
    }
    db->undo_count++;
}

static unsigned long wal_put(db_conn *db, const u8 *key, u64 klen,
                              const u8 *val, u64 vlen) {
    push_undo(db, key, klen);
    /* WAL entry: [u8 op][u32 klen][key][u32 vlen][val] */
    if (db->log) {
        u64 elen = 1 + 4 + klen + 4 + vlen;
        u8 *entry = malloc(elen);
        if (!entry) return 1;
        u64 p = 0;
        entry[p++] = DB_WAL_OP_PUT;
        write_u32_le(entry + p, (u32)klen); p += 4;
        memcpy(entry + p, key, klen); p += klen;
        write_u32_le(entry + p, (u32)vlen); p += 4;
        memcpy(entry + p, val, vlen);
        u64 seq;
        unsigned long rc = wal_append(&seq, db->log, entry, elen);
        free(entry);
        if (rc) return 2;
    }
    return db->vt->put(db->storage, key, klen, val, vlen);
}

static unsigned long wal_del(db_conn *db, const u8 *key, u64 klen) {
    push_undo(db, key, klen);
    if (db->log) {
        u64 elen = 1 + 4 + klen;
        u8 *entry = malloc(elen);
        if (!entry) return 1;
        u64 p = 0;
        entry[p++] = DB_WAL_OP_DELETE;
        write_u32_le(entry + p, (u32)klen); p += 4;
        memcpy(entry + p, key, klen);
        u64 seq;
        unsigned long rc = wal_append(&seq, db->log, entry, elen);
        free(entry);
        if (rc) return 2;
    }
    return db->vt->del(db->storage, key, klen);
}

/* Free undo buffers (commit OR after rollback replay). */
static void undo_clear(db_conn *db) {
    for (u64 i = 0; i < db->undo_count; ++i) {
        free(db->undo[i].key);
        free(db->undo[i].prev_val);
    }
    db->undo_count = 0;
}

/* ------------------------------------------------------------------ */
/*  Evaluate WHERE clause against a row                                */
/* ------------------------------------------------------------------ */

static int col_index(const db_table *t, const char *name) {
    for (u32 i = 0; i < t->col_count; ++i) {
        if (strcasecmp_a(t->cols[i].name, name) == 0) return (int)i;
    }
    /* check for special "rowid" pseudo-column — not stored, handled separately */
    return -1;
}

/* Compare a column value against a literal string.
 * Returns: -1 less, 0 equal, 1 greater, -2 on type mismatch / null */
static int compare_val(const db_row *row, int ci, const char *lit, const db_table *t) {
    if (ci < 0 || (u32)ci >= row->col_count) return -2;
    int ty = row->types[ci];
    if (ty == DB_TYPE_NULL) return -2;

    if (ty == DB_TYPE_INTEGER) {
        i64 rv = row->ivals[ci];
        char *end;
        i64 lv = strtoll(lit, &end, 10);
        if (*end != '\0' && *end != '.') return -2;
        if (rv < lv) return -1;
        if (rv > lv) return 1;
        return 0;
    }
    if (ty == DB_TYPE_REAL) {
        double rv = row->fvals[ci];
        char *end;
        double lv = strtod(lit, &end);
        if (rv < lv) return -1;
        if (rv > lv) return 1;
        return 0;
    }
    if (ty == DB_TYPE_TEXT || ty == DB_TYPE_BLOB) {
        const char *rv = (const char *)row->bvals[ci];
        if (!rv) rv = "";
        return strcmp(rv, lit);
    }
    return -2;
}

static int eval_where_multival(const sql_ast *node, const db_row *row,
                                 const db_table *t);

/* Evaluate an AST condition node against a row. Returns 1 if match, 0 if not. */
static int eval_where(const sql_ast *node, const db_row *row, const db_table *t) {
    if (!node) return 1; /* no WHERE = match all */

    switch (node->type) {
    case SQL_AST_AND:
        return eval_where(node->left, row, t) && eval_where(node->right, row, t);
    case SQL_AST_OR:
        return eval_where(node->left, row, t) || eval_where(node->right, row, t);
    case SQL_AST_BINARY_OP: {
        if (!node->text) return 0;

        /* Multi-value ops (BETWEEN / IN / NOT IN) keep their operands
         * in children[], not left/right. Dispatched separately. */
        if (strcmp(node->text, "BETWEEN") == 0
            || strcmp(node->text, "IN") == 0
            || strcmp(node->text, "NOT IN") == 0) {
            int r = eval_where_multival(node, row, t);
            return r < 0 ? 0 : r;
        }

        /* IS NULL / IS NOT NULL */
        if (strcmp(node->text, "IS") == 0) {
            if (!node->left || node->left->type != SQL_AST_COLUMN_REF) return 0;
            int ci = col_index(t, node->left->text);
            if (ci < 0 || (u32)ci >= row->col_count) return 0;
            return row->types[ci] == DB_TYPE_NULL;
        }
        if (strcmp(node->text, "IS NOT") == 0) {
            if (!node->left || node->left->type != SQL_AST_COLUMN_REF) return 0;
            int ci = col_index(t, node->left->text);
            if (ci < 0 || (u32)ci >= row->col_count) return 0;
            return row->types[ci] != DB_TYPE_NULL;
        }

        /* NOT operator (unary) */
        if (strcmp(node->text, "NOT") == 0) {
            return !eval_where(node->left, row, t);
        }

        /* Standard comparison: left must be column_ref, right must be literal */
        if (!node->left || !node->right) return 0;
        const char *col_name = NULL;
        const char *lit_val = NULL;

        if (node->left->type == SQL_AST_COLUMN_REF && node->right->type == SQL_AST_LITERAL) {
            col_name = node->left->text;
            lit_val  = node->right->text;
        } else if (node->left->type == SQL_AST_LITERAL && node->right->type == SQL_AST_COLUMN_REF) {
            col_name = node->right->text;
            lit_val  = node->left->text;
        } else {
            return 0;
        }

        int ci = col_index(t, col_name);
        if (ci < 0) return 0;

        /* NULL literal check */
        if (strcasecmp_a(lit_val, "NULL") == 0) {
            if (strcmp(node->text, "=") == 0) return row->types[ci] == DB_TYPE_NULL;
            if (strcmp(node->text, "!=") == 0 || strcmp(node->text, "<>") == 0)
                return row->types[ci] != DB_TYPE_NULL;
            return 0;
        }

        int cmp = compare_val(row, ci, lit_val, t);
        if (cmp == -2) return 0; /* null or type mismatch */

        const char *op = node->text;
        if (strcmp(op, "=") == 0)  return cmp == 0;
        if (strcmp(op, "!=") == 0 || strcmp(op, "<>") == 0) return cmp != 0;
        if (strcmp(op, "<") == 0)  return cmp < 0;
        if (strcmp(op, "<=") == 0) return cmp <= 0;
        if (strcmp(op, ">") == 0)  return cmp > 0;
        if (strcmp(op, ">=") == 0) return cmp >= 0;

        if (strcmp(op, "LIKE") == 0) {
            /* Simple LIKE: only % wildcard at start/end */
            if (ci < 0 || (u32)ci >= row->col_count) return 0;
            if (row->types[ci] != DB_TYPE_TEXT) return 0;
            const char *rv = (const char *)row->bvals[ci];
            if (!rv) return 0;
            u64 plen = strlen(lit_val);
            if (plen == 0) return strlen(rv) == 0;
            int starts_pct = (lit_val[0] == '%');
            int ends_pct = (plen > 0 && lit_val[plen - 1] == '%');
            const char *pat = lit_val + (starts_pct ? 1 : 0);
            u64 pat_len = plen - (starts_pct ? 1 : 0) - (ends_pct ? 1 : 0);
            if (starts_pct && ends_pct) {
                /* contains */
                u64 rl = strlen(rv);
                for (u64 i = 0; i + pat_len <= rl; ++i) {
                    if (strncasecmp_a(rv + i, pat, pat_len) == 0) return 1;
                }
                return 0;
            }
            if (starts_pct) {
                /* ends with */
                u64 rl = strlen(rv);
                if (rl < pat_len) return 0;
                return strncasecmp_a(rv + rl - pat_len, pat, pat_len) == 0;
            }
            if (ends_pct) {
                /* starts with */
                return strncasecmp_a(rv, pat, pat_len) == 0;
            }
            /* exact match */
            return strcasecmp_a(rv, lit_val) == 0;
        }
        if (strcmp(op, "NOT LIKE") == 0) {
            /* reuse LIKE logic by building a temp node */
            sql_ast tmp = *node;
            char like_text[] = "LIKE";
            tmp.text = like_text;
            return !eval_where(&tmp, row, t);
        }
        return 0;
    }
    default:
        return 0;
    }
}

/* Handle BETWEEN / IN / NOT IN — these have child_count > 0 (the
 * list of values) rather than a single `right`. Called before the
 * left/right binop path. */
static int eval_where_multival(const sql_ast *node, const db_row *row,
                                 const db_table *t) {
    if (!node || node->type != SQL_AST_BINARY_OP || !node->text) return -1;
    if (!node->left || node->left->type != SQL_AST_COLUMN_REF) return -1;
    int ci = col_index(t, node->left->text);
    if (ci < 0) return 0;

    if (strcmp(node->text, "BETWEEN") == 0) {
        if (node->child_count < 2) return 0;
        const char *lo = node->children[0].text;
        const char *hi = node->children[1].text;
        if (!lo || !hi) return 0;
        int c_lo = compare_val(row, (u32)ci, lo, t);
        int c_hi = compare_val(row, (u32)ci, hi, t);
        if (c_lo == -2 || c_hi == -2) return 0;
        return (c_lo >= 0) && (c_hi <= 0);
    }
    if (strcmp(node->text, "IN") == 0 || strcmp(node->text, "NOT IN") == 0) {
        int found = 0;
        for (u64 i = 0; i < node->child_count; ++i) {
            const char *lit = node->children[i].text;
            if (!lit) continue;
            if (compare_val(row, (u32)ci, lit, t) == 0) { found = 1; break; }
        }
        return (strcmp(node->text, "IN") == 0) ? found : !found;
    }
    return -1;  /* not a multival op; caller falls through */
}

/* ------------------------------------------------------------------ */
/*  Find the WHERE condition child in an AST node                      */
/* ------------------------------------------------------------------ */

static const sql_ast *find_where(const sql_ast *node) {
    if (!node) return NULL;
    for (u64 i = 0; i < node->child_count; ++i) {
        int ty = node->children[i].type;
        if (ty == SQL_AST_BINARY_OP || ty == SQL_AST_AND || ty == SQL_AST_OR) {
            return &node->children[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Parse a literal value into a row column                            */
/* ------------------------------------------------------------------ */

static void literal_to_col(db_row *row, u32 ci, const char *lit, int col_type) {
    if (!lit || strcasecmp_a(lit, "NULL") == 0) {
        row->types[ci] = DB_TYPE_NULL;
        return;
    }

    /* Check if it's a bind placeholder — starts with ? */
    /* Bind placeholders should already be resolved before calling this */

    switch (col_type) {
    case DB_TYPE_INTEGER: {
        row->types[ci] = DB_TYPE_INTEGER;
        row->ivals[ci] = strtoll(lit, NULL, 10);
        break;
    }
    case DB_TYPE_REAL: {
        row->types[ci] = DB_TYPE_REAL;
        row->fvals[ci] = strtod(lit, NULL);
        break;
    }
    case DB_TYPE_BLOB:
    case DB_TYPE_TEXT:
    default: {
        row->types[ci] = DB_TYPE_TEXT;
        u64 slen = strlen(lit);
        row->bvals[ci] = malloc(slen + 1);
        if (row->bvals[ci]) {
            memcpy(row->bvals[ci], lit, slen + 1);
            row->blens[ci] = slen;
        }
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/*  Extract the real table name from AST text                          */
/*  (handles "IF NOT EXISTS tablename" / "IF EXISTS tablename")        */
/* ------------------------------------------------------------------ */

static const char *extract_table_name(const char *text) {
    if (!text) return NULL;
    if (strncasecmp_a(text, "IF NOT EXISTS ", 14) == 0) return text + 14;
    if (strncasecmp_a(text, "IF EXISTS ", 10) == 0) return text + 10;
    /* DISTINCT prefix for SELECT */
    if (strncasecmp_a(text, "DISTINCT ", 9) == 0) return text + 9;
    return text;
}

/* ------------------------------------------------------------------ */
/*  Execute CREATE TABLE                                               */
/* ------------------------------------------------------------------ */

static unsigned long exec_create_table(db_conn *db, const sql_ast *ast) {
    const char *raw_name = ast->text;
    int if_not_exists = 0;
    if (raw_name && strncasecmp_a(raw_name, "IF NOT EXISTS ", 14) == 0) {
        if_not_exists = 1;
    }
    const char *tname = extract_table_name(raw_name);
    if (!tname) return 1;

    db_table *existing = find_table(db, tname);
    if (existing) {
        return if_not_exists ? 0 : 2; /* already exists */
    }
    if (db->table_count >= DB_MAX_TABLES) return 3;

    db_table *t = &db->tables[db->table_count];
    memset(t, 0, sizeof(*t));
    snprintf(t->name, sizeof(t->name), "%s", tname);
    t->next_rowid = 1;

    /* Two-pass: first pass handles column definitions (gives every col
     * an index before we can refer to it by name); second pass handles
     * table-level UNIQUE(...) constraints emitted by the parser as
     * "__UNIQUE__ c1,c2,..." text children. */
    for (u64 i = 0; i < ast->child_count && t->col_count < DB_MAX_COLUMNS; ++i) {
        const sql_ast *cd = &ast->children[i];
        if (cd->type != SQL_AST_COLUMN_DEF) continue;
        if (!cd->text) continue;
        /* Skip table-level markers in this pass. */
        if (strncmp(cd->text, "__UNIQUE__ ", 11) == 0) continue;
        if (strncmp(cd->text, "__FK__ ", 7) == 0) continue;

        db_col_def *col = &t->cols[t->col_count];
        memset(col, 0, sizeof(*col));

        /* Parse "name TYPE [PRIMARY KEY] [NOT NULL] [UNIQUE]" from cd->text */
        const char *src = cd->text;
        u64 slen = strlen(src);
        u64 sp = 0;

        /* Extract column name (first word) */
        while (sp < slen && src[sp] == ' ') sp++;
        u64 ws = sp;
        while (sp < slen && src[sp] != ' ') sp++;
        if (sp == ws) continue;
        u64 nlen_c = sp - ws;
        if (nlen_c >= sizeof(col->name)) nlen_c = sizeof(col->name) - 1;
        memcpy(col->name, src + ws, nlen_c);
        col->name[nlen_c] = '\0';

        /* Extract type (second word) */
        while (sp < slen && src[sp] == ' ') sp++;
        ws = sp;
        while (sp < slen && src[sp] != ' ' && src[sp] != '(') sp++;
        /* skip optional (N) */
        if (sp < slen && src[sp] == '(') {
            while (sp < slen && src[sp] != ')') sp++;
            if (sp < slen) sp++;
        }
        {
            char type_buf[64];
            u64 tl = (sp > ws && sp - ws < sizeof(type_buf)) ? sp - ws : 0;
            if (tl > 0) { memcpy(type_buf, src + ws, tl); type_buf[tl] = '\0'; }
            else { type_buf[0] = '\0'; }
            col->type = parse_col_type(tl > 0 ? type_buf : NULL);
        }

        /* Check remaining text for PRIMARY KEY / NOT NULL / UNIQUE */
        const char *rest = src + sp;
        if (rest[0]) {
            if (strstr(rest, "PRIMARY KEY") || strstr(rest, "PRIMARY")) {
                col->primary_key = 1;
            }
            if (strstr(rest, "NOT NULL")) {
                col->not_null = 1;
            }
            if (strstr(rest, "UNIQUE")) {
                col->unique = 1;
                /* Register a 1-column unique set so all enforcement flows
                 * through the same code path as composite constraints. */
                if (t->unique_set_count < DB_MAX_UNIQUE_SETS) {
                    db_unique_set *us = &t->unique_sets[t->unique_set_count++];
                    us->col_count = 1;
                    us->cols[0] = t->col_count;
                }
            }
        }

        t->col_count++;
    }

    /* Second pass: table-level UNIQUE(c1, c2, ...) markers. Resolve
     * named columns to indices and append unique sets. */
    for (u64 i = 0; i < ast->child_count; ++i) {
        const sql_ast *cd = &ast->children[i];
        if (cd->type != SQL_AST_COLUMN_DEF || !cd->text) continue;
        if (strncmp(cd->text, "__UNIQUE__ ", 11) != 0) continue;
        if (t->unique_set_count >= DB_MAX_UNIQUE_SETS) continue;

        db_unique_set *us = &t->unique_sets[t->unique_set_count];
        us->col_count = 0;

        const char *p = cd->text + 11;
        while (*p) {
            /* Skip leading spaces. */
            while (*p == ' ') p++;
            const char *w = p;
            while (*p && *p != ',' && *p != ' ') p++;
            if (p == w) { if (*p) p++; continue; }
            char colname[128];
            u64 nl = (u64)(p - w);
            if (nl >= sizeof(colname)) nl = sizeof(colname) - 1;
            memcpy(colname, w, nl); colname[nl] = '\0';
            int ci = col_index(t, colname);
            if (ci >= 0 && us->col_count < DB_MAX_COLUMNS) {
                us->cols[us->col_count++] = (u32)ci;
            }
            while (*p == ' ' || *p == ',') p++;
        }
        if (us->col_count > 0) t->unique_set_count++;
    }

    /* Third pass: __FK__ local_csv target_tbl target_csv [action] markers.
     * local_csv / target_csv are comma-joined col name lists — parse
     * into arrays so composite FKs work end-to-end. */
    for (u64 i = 0; i < ast->child_count; ++i) {
        const sql_ast *cd = &ast->children[i];
        if (cd->type != SQL_AST_COLUMN_DEF || !cd->text) continue;
        if (strncmp(cd->text, "__FK__ ", 7) != 0) continue;
        if (t->fk_count >= DB_MAX_FKS) continue;
        char lc_csv[256], tt[128], tc_csv[256], act[32];
        lc_csv[0] = tt[0] = tc_csv[0] = act[0] = '\0';
        int n = sscanf(cd->text + 7, "%255s %127s %255s %31s", lc_csv, tt, tc_csv, act);
        if (n < 3) continue;

        db_fk *fk = &t->fks[t->fk_count];
        memset(fk, 0, sizeof(*fk));
        snprintf(fk->target_table, sizeof(fk->target_table), "%s", tt);

        /* Parse local col names, resolve each to an index. */
        u32 lcount = 0;
        const char *p1 = lc_csv;
        while (*p1 && lcount < DB_FK_MAX_COLS) {
            const char *start = p1;
            while (*p1 && *p1 != ',') p1++;
            u64 nl = (u64)(p1 - start);
            if (nl == 0 || nl >= 128) { if (*p1) p1++; continue; }
            char cn[128];
            memcpy(cn, start, nl); cn[nl] = '\0';
            int ci = col_index(t, cn);
            if (ci < 0) { lcount = 0; break; }
            fk->local_cols[lcount++] = (u32)ci;
            if (*p1 == ',') p1++;
        }

        /* Parse target col names, store as strings (parent table may
         * not exist yet at CREATE time — we resolve on use). */
        u32 tcount = 0;
        const char *p2 = tc_csv;
        while (*p2 && tcount < DB_FK_MAX_COLS) {
            const char *start = p2;
            while (*p2 && *p2 != ',') p2++;
            u64 nl = (u64)(p2 - start);
            if (nl == 0 || nl >= sizeof(fk->target_cols[tcount])) { if (*p2) p2++; continue; }
            memcpy(fk->target_cols[tcount], start, nl);
            fk->target_cols[tcount][nl] = '\0';
            tcount++;
            if (*p2 == ',') p2++;
        }

        if (lcount == 0 || tcount == 0 || lcount != tcount) continue;
        fk->col_count = lcount;
        fk->on_delete = DB_FK_NOACTION;
        if (n >= 4) {
            if      (strcasecmp_a(act, "RESTRICT")   == 0) fk->on_delete = DB_FK_RESTRICT;
            else if (strcasecmp_a(act, "CASCADE")    == 0) fk->on_delete = DB_FK_CASCADE;
            else if (strcasecmp_a(act, "SETNULL")    == 0) fk->on_delete = DB_FK_SETNULL;
            else if (strcasecmp_a(act, "SETDEFAULT") == 0) fk->on_delete = DB_FK_SETDEFAULT;
            else fk->on_delete = DB_FK_NOACTION;
        }
        t->fk_count++;
    }

    db->table_count++;
    return flush_meta(db, t);
}

/* ------------------------------------------------------------------ */
/*  Execute DROP TABLE                                                 */
/* ------------------------------------------------------------------ */

static unsigned long exec_drop_table(db_conn *db, const sql_ast *ast) {
    const char *raw_name = ast->text;
    int if_exists = 0;
    if (raw_name && strncasecmp_a(raw_name, "IF EXISTS ", 10) == 0) {
        if_exists = 1;
    }
    const char *tname = extract_table_name(raw_name);
    if (!tname) return 1;

    db_table *t = find_table(db, tname);
    if (!t) return if_exists ? 0 : 2;

    /* Delete all row entries */
    u8 prefix[DB_MAX_KEY_LEN];
    u64 plen = build_prefix_key(prefix, sizeof(prefix), t->name);
    if (plen > 0) {
        db_storage_iter *it = NULL;
        if (db->vt->iter_create(&it, db->storage, prefix, plen) == 0) {
            const u8 *k; u64 kl;
            const u8 *v; u64 vl;
            /* collect keys first to avoid modifying during iteration */
            u8 *keys[4096];
            u64 klens[4096];
            u64 key_count = 0;
            while (db_storage_iter_next(&k, &kl, &v, &vl, it) == 0) {
                if (key_count < 4096) {
                    keys[key_count] = malloc(kl);
                    if (keys[key_count]) {
                        memcpy(keys[key_count], k, kl);
                        klens[key_count] = kl;
                        key_count++;
                    }
                }
            }
            db_storage_iter_destroy(it);
            for (u64 i = 0; i < key_count; ++i) {
                /* Row-data deletes still route through wal_del —
                 * wal_append + push_undo match how INSERT records are
                 * written, so WAL ordering stays consistent. */
                wal_del(db, keys[i], klens[i]);
                free(keys[i]);
            }
        }
    }

    /* Meta deletion: push_undo + kv_delete, no WAL double-log (same
     * reasoning as flush_meta). */
    u8 mkey[DB_MAX_KEY_LEN];
    u64 mlen = build_meta_key(mkey, sizeof(mkey), t->name);
    if (mlen > 0) {
        push_undo(db, mkey, mlen);
        db->vt->del(db->storage, mkey, mlen);
    }

    /* Remove from table array */
    u32 idx = (u32)(t - db->tables);
    if (idx < db->table_count - 1) {
        memmove(&db->tables[idx], &db->tables[idx + 1],
                (db->table_count - idx - 1) * sizeof(db_table));
    }
    db->table_count--;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  UNIQUE constraint enforcement                                      */
/*                                                                     */
/*  O(n) scan per check — acceptable for cookbook-scale (low thousands */
/*  of rows per table). Real secondary indexes come in step 3.         */
/*                                                                     */
/*  NULL semantics match SQLite: NULL != NULL for UNIQUE purposes, so  */
/*  two rows with NULL in a UNIQUE column are both legal. A match is   */
/*  only declared when every column in the set compares non-NULL-equal */
/*  between the candidate and the existing row.                        */
/* ------------------------------------------------------------------ */

static int values_equal(const db_row *a, const db_row *b, u32 ci) {
    if (ci >= a->col_count || ci >= b->col_count) return 0;
    int ta = a->types[ci], tb = b->types[ci];
    if (ta == DB_TYPE_NULL || tb == DB_TYPE_NULL) return 0;
    if (ta != tb) {
        /* Different types never compare equal for UNIQUE purposes. */
        return 0;
    }
    if (ta == DB_TYPE_INTEGER) return a->ivals[ci] == b->ivals[ci];
    if (ta == DB_TYPE_REAL)    return a->fvals[ci] == b->fvals[ci];
    if (ta == DB_TYPE_TEXT || ta == DB_TYPE_BLOB) {
        if (a->blens[ci] != b->blens[ci]) return 0;
        if (a->blens[ci] == 0) return 1;
        return memcmp(a->bvals[ci], b->bvals[ci], a->blens[ci]) == 0;
    }
    return 0;
}

static int rows_collide_on_set(const db_unique_set *us,
                                 const db_row *a, const db_row *b) {
    if (us->col_count == 0) return 0;
    for (u32 i = 0; i < us->col_count; ++i) {
        if (!values_equal(a, b, us->cols[i])) return 0;
    }
    return 1;
}

/* Scan every row for the given table. If skip_key is non-NULL, the
 * row with that exact key bytes is ignored (used for UPDATE's
 * exclude-self semantics). Returns 0 if no collision, 9 if any
 * unique_set collision is found. */
static unsigned long check_unique_constraints(db_conn *db, const db_table *t,
                                                const db_row *new_row,
                                                const u8 *skip_key,
                                                u64 skip_klen) {
    if (t->unique_set_count == 0) return 0;

    u8 prefix[DB_MAX_KEY_LEN];
    u64 plen = build_prefix_key(prefix, sizeof(prefix), t->name);
    if (plen == 0) return 0;

    db_storage_iter *it = NULL;
    if (db->vt->iter_create(&it, db->storage, prefix, plen) != 0) return 0;

    const u8 *k; u64 kl;
    const u8 *v; u64 vl;
    unsigned long rc = 0;
    while (db_storage_iter_next(&k, &kl, &v, &vl, it) == 0) {
        if (skip_key && kl == skip_klen && memcmp(k, skip_key, kl) == 0) continue;
        db_row other;
        if (row_deserialize(&other, v, vl) != 0) continue;
        for (u32 i = 0; i < t->unique_set_count; ++i) {
            if (rows_collide_on_set(&t->unique_sets[i], new_row, &other)) {
                rc = 9;
                break;
            }
        }
        row_free(&other);
        if (rc) break;
    }
    db_storage_iter_destroy(it);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  FOREIGN KEY constraint enforcement                                 */
/*                                                                     */
/*  Always on by default (unlike SQLite, which ships with fk off).     */
/*  A NULL FK column is always allowed — referential integrity applies */
/*  only to non-NULL values. Composite FKs are not yet supported:      */
/*  each db_fk references a single column.                             */
/* ------------------------------------------------------------------ */

/* Compare one row's cols[ref_col_idx[i]] against another row's
 * cols[target_col_idx[i]] for every i in [0, n). Returns 1 only if
 * every pair compares equal (non-NULL-equal) under values_equal. */
static int rows_match_on_cols(const db_row *a, const u32 *a_cols,
                                const db_row *b, const u32 *b_cols,
                                u32 n) {
    for (u32 i = 0; i < n; ++i) {
        u32 ai = a_cols[i], bi = b_cols[i];
        if (ai >= a->col_count || bi >= b->col_count) return 0;
        int ta = a->types[ai], tb = b->types[bi];
        if (ta == DB_TYPE_NULL || tb == DB_TYPE_NULL) return 0;
        if (ta != tb) return 0;
        if (ta == DB_TYPE_INTEGER) {
            if (a->ivals[ai] != b->ivals[bi]) return 0;
        } else if (ta == DB_TYPE_REAL) {
            if (a->fvals[ai] != b->fvals[bi]) return 0;
        } else if (ta == DB_TYPE_TEXT || ta == DB_TYPE_BLOB) {
            if (a->blens[ai] != b->blens[bi]) return 0;
            if (a->blens[ai] > 0 &&
                memcmp(a->bvals[ai], b->bvals[bi], a->blens[ai]) != 0) return 0;
        }
    }
    return 1;
}

/* Scan `parent` for any row whose target columns match `child`'s local
 * columns. Used by check_fk_constraints_for_row (lookup side). */
static int parent_has_matching_row(db_conn *db, const db_table *parent,
                                      const u32 *target_col_indices,
                                      const db_row *child,
                                      const u32 *local_col_indices,
                                      u32 n_cols) {
    u8 prefix[DB_MAX_KEY_LEN];
    u64 plen = build_prefix_key(prefix, sizeof(prefix), parent->name);
    if (plen == 0) return 0;
    db_storage_iter *it = NULL;
    if (db->vt->iter_create(&it, db->storage, prefix, plen) != 0) return 0;

    const u8 *k; u64 kl;
    const u8 *v; u64 vl;
    int found = 0;
    while (db_storage_iter_next(&k, &kl, &v, &vl, it) == 0) {
        db_row other;
        if (row_deserialize(&other, v, vl) != 0) continue;
        if (rows_match_on_cols(&other, target_col_indices,
                                 child,  local_col_indices, n_cols)) {
            found = 1;
        }
        row_free(&other);
        if (found) break;
    }
    db_storage_iter_destroy(it);
    return found;
}

/* For each FK on t, verify the row's local values have a matching
 * parent row. Returns 0 on pass, 10 on violation. If ANY local col is
 * NULL, the FK check is skipped (MATCH SIMPLE, SQL default). */
static unsigned long check_fk_constraints_for_row(db_conn *db,
                                                     const db_table *t,
                                                     const db_row *row) {
    for (u32 i = 0; i < t->fk_count; ++i) {
        const db_fk *fk = &t->fks[i];
        int any_null = 0;
        for (u32 j = 0; j < fk->col_count; ++j) {
            u32 ci = fk->local_cols[j];
            if (ci >= row->col_count || row->types[ci] == DB_TYPE_NULL) {
                any_null = 1; break;
            }
        }
        if (any_null) continue;

        db_table *parent = find_table(db, fk->target_table);
        if (!parent) return 10;

        /* Resolve target_cols (by name) to indices in the parent. */
        u32 target_idx[DB_FK_MAX_COLS];
        int ok = 1;
        for (u32 j = 0; j < fk->col_count; ++j) {
            int ti = col_index(parent, fk->target_cols[j]);
            if (ti < 0) { ok = 0; break; }
            target_idx[j] = (u32)ti;
        }
        if (!ok) return 10;

        if (!parent_has_matching_row(db, parent, target_idx,
                                        row, fk->local_cols, fk->col_count)) {
            return 10;
        }
    }
    return 0;
}

/* Forward declarations for index maintenance helpers used below by
 * apply_fk_on_delete (CASCADE / SETNULL paths that touch child rows). */
static void write_row_indexes(db_conn *db, const db_table *t,
                                const db_row *row, i64 rowid);
static void delete_row_indexes(db_conn *db, const db_table *t,
                                 const db_row *row, i64 rowid);

/* For a row about to be deleted from `parent`, resolve every FK in
 * every other table targeting `parent` according to its on_delete
 * action:
 *
 *   NOACTION / RESTRICT  — fail with hatch 11 if any child references
 *   CASCADE              — delete matching child rows (and recurse)
 *   SETNULL              — update matching child rows, FK col = NULL
 *   SETDEFAULT           — not implemented; falls back to SETNULL
 *
 * Returns 0 on success, 11 on RESTRICT violation, other non-zero on
 * infrastructure failure. Recursion depth is bounded by the FK graph
 * depth (cookbook's is 3 levels max). */
static unsigned long apply_fk_on_delete(db_conn *db,
                                           const db_table *parent,
                                           const db_row *parent_row) {
    for (u32 ti = 0; ti < db->table_count; ++ti) {
        db_table *child = &db->tables[ti];
        if (child == parent) continue;
        for (u32 fi = 0; fi < child->fk_count; ++fi) {
            const db_fk *fk = &child->fks[fi];
            if (strcmp(fk->target_table, parent->name) != 0) continue;

            /* Resolve target col names to indices in `parent`. Skip
             * the FK entirely if any target is unresolvable (shouldn't
             * happen after a valid CREATE). */
            u32 target_idx[DB_FK_MAX_COLS];
            int ok = 1;
            for (u32 j = 0; j < fk->col_count; ++j) {
                int ci = col_index(parent, fk->target_cols[j]);
                if (ci < 0) { ok = 0; break; }
                target_idx[j] = (u32)ci;
            }
            if (!ok) continue;

            /* If any of the parent's target cols is NULL, MATCH SIMPLE
             * says no child row can reference this — skip. */
            int any_null = 0;
            for (u32 j = 0; j < fk->col_count; ++j) {
                if (parent_row->types[target_idx[j]] == DB_TYPE_NULL) {
                    any_null = 1; break;
                }
            }
            if (any_null) continue;

            /* Collect every child row whose local_cols match the
             * parent's target_cols values. Collect first, mutate
             * after, so kv_iter isn't disturbed by concurrent
             * wal_del/put. */
            u8 prefix[DB_MAX_KEY_LEN];
            u64 plen = build_prefix_key(prefix, sizeof(prefix), child->name);
            if (plen == 0) continue;
            db_storage_iter *it = NULL;
            if (db->vt->iter_create(&it, db->storage, prefix, plen) != 0) continue;

            typedef struct { u8 *k; u64 kl; u8 *v; u64 vl; i64 rowid; } child_match;
            child_match *matches = NULL;
            u64 mn = 0, mc = 0;

            const u8 *k; u64 kl;
            const u8 *v; u64 vl;
            while (db_storage_iter_next(&k, &kl, &v, &vl, it) == 0) {
                db_row row;
                if (row_deserialize(&row, v, vl) != 0) continue;
                int match = rows_match_on_cols(&row,        fk->local_cols,
                                                 parent_row, target_idx,
                                                 fk->col_count);
                if (!match) { row_free(&row); continue; }

                if (fk->on_delete == DB_FK_NOACTION
                    || fk->on_delete == DB_FK_RESTRICT) {
                    row_free(&row);
                    for (u64 i = 0; i < mn; ++i) {
                        free(matches[i].k); free(matches[i].v);
                    }
                    free(matches);
                    db_storage_iter_destroy(it);
                    return 11;
                }

                /* CASCADE or SETNULL: remember the key + raw value. */
                if (mn == mc) {
                    u64 nc = mc ? mc * 2 : 16;
                    child_match *nm = (child_match *)realloc(matches,
                                                              nc * sizeof(*matches));
                    if (!nm) { row_free(&row); break; }
                    matches = nm; mc = nc;
                }
                matches[mn].k = (u8 *)malloc(kl); if (!matches[mn].k) { row_free(&row); break; }
                memcpy(matches[mn].k, k, kl); matches[mn].kl = kl;
                matches[mn].v = (u8 *)malloc(vl); if (!matches[mn].v) { free(matches[mn].k); row_free(&row); break; }
                memcpy(matches[mn].v, v, vl); matches[mn].vl = vl;
                matches[mn].rowid = 0;
                for (i64 j = (i64)kl - 1; j >= 0; --j) {
                    if (k[j] == '/') {
                        char tmp[32];
                        u64 n = kl - (u64)j - 1;
                        if (n < sizeof(tmp)) {
                            memcpy(tmp, k + j + 1, n); tmp[n] = '\0';
                            matches[mn].rowid = strtoll(tmp, NULL, 10);
                        }
                        break;
                    }
                }
                mn++;
                row_free(&row);
            }
            db_storage_iter_destroy(it);

            /* Apply action for each collected match. */
            for (u64 i = 0; i < mn; ++i) {
                db_row row;
                if (row_deserialize(&row, matches[i].v, matches[i].vl) != 0) {
                    free(matches[i].k); free(matches[i].v);
                    continue;
                }
                if (fk->on_delete == DB_FK_CASCADE) {
                    unsigned long rrc = apply_fk_on_delete(db, child, &row);
                    if (rrc) {
                        row_free(&row);
                        for (u64 j = i; j < mn; ++j) {
                            free(matches[j].k); free(matches[j].v);
                        }
                        free(matches);
                        return rrc;
                    }
                    delete_row_indexes(db, child, &row, matches[i].rowid);
                    wal_del(db, matches[i].k, matches[i].kl);
                    db->changes++;
                } else if (fk->on_delete == DB_FK_SETNULL
                           || fk->on_delete == DB_FK_SETDEFAULT) {
                    /* Set every local col to NULL. */
                    delete_row_indexes(db, child, &row, matches[i].rowid);
                    for (u32 j = 0; j < fk->col_count; ++j) {
                        u32 ci = fk->local_cols[j];
                        if (ci >= row.col_count) continue;
                        if (row.bvals[ci]) {
                            free(row.bvals[ci]);
                            row.bvals[ci] = NULL;
                        }
                        row.blens[ci] = 0;
                        row.ivals[ci] = 0;
                        row.fvals[ci] = 0;
                        row.types[ci] = DB_TYPE_NULL;
                    }
                    u8 rb[DB_MAX_ROW_SIZE];
                    u64 rlen = row_serialize(&row, rb, sizeof(rb));
                    if (rlen > 0) {
                        wal_put(db, matches[i].k, matches[i].kl, rb, rlen);
                        write_row_indexes(db, child, &row, matches[i].rowid);
                    }
                    db->changes++;
                }
                row_free(&row);
                free(matches[i].k); free(matches[i].v);
            }
            free(matches);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Index maintenance                                                  */
/* ------------------------------------------------------------------ */

/* Write an entry for every index on `t` using the current row values. */
static void write_row_indexes(db_conn *db, const db_table *t,
                                const db_row *row, i64 rowid) {
    for (u32 i = 0; i < t->index_count; ++i) {
        const db_index *ix = &t->indexes[i];
        u8 key[DB_MAX_KEY_LEN];
        u64 kl = build_idx_entry_key_composite(key, sizeof(key),
                                                 t->name, ix->name,
                                                 row, ix->col_idx,
                                                 ix->col_count, rowid);
        if (kl == 0) continue;
        wal_put(db, key, kl, (const u8 *)"", 0);
    }
}

/* Delete every index entry for a row being removed or superseded. */
static void delete_row_indexes(db_conn *db, const db_table *t,
                                 const db_row *row, i64 rowid) {
    for (u32 i = 0; i < t->index_count; ++i) {
        const db_index *ix = &t->indexes[i];
        u8 key[DB_MAX_KEY_LEN];
        u64 kl = build_idx_entry_key_composite(key, sizeof(key),
                                                 t->name, ix->name,
                                                 row, ix->col_idx,
                                                 ix->col_count, rowid);
        if (kl == 0) continue;
        wal_del(db, key, kl);
    }
}

/* ------------------------------------------------------------------ */
/*  Execute CREATE INDEX                                               */
/* ------------------------------------------------------------------ */

static unsigned long exec_create_index(db_conn *db, const sql_ast *ast) {
    if (!ast->text) return 1;
    int ine = 0;
    const char *src = ast->text;
    if (strncasecmp_a(src, "IF NOT EXISTS ", 14) == 0) {
        ine = 1;
        src += 14;
    }
    /* src is now "<idx_name> <table> <col1,col2,...>" */
    char idx_name[128], tblname[128], cols_csv[256];
    idx_name[0] = tblname[0] = cols_csv[0] = '\0';
    if (sscanf(src, "%127s %127s %255s", idx_name, tblname, cols_csv) != 3) return 2;
    db_table *t = find_table(db, tblname);
    if (!t) return 3;

    /* Already exists (by name) in this table? */
    for (u32 i = 0; i < t->index_count; ++i) {
        if (strcasecmp_a(t->indexes[i].name, idx_name) == 0) {
            return ine ? 0 : 5;
        }
    }
    if (t->index_count >= DB_MAX_INDEXES) return 6;

    db_index *ix = &t->indexes[t->index_count];
    memset(ix, 0, sizeof(*ix));
    snprintf(ix->name, sizeof(ix->name), "%s", idx_name);

    /* Parse comma-joined column list. */
    const char *p = cols_csv;
    while (*p && ix->col_count < DB_IDX_MAX_COLS) {
        const char *start = p;
        while (*p && *p != ',') p++;
        u64 nl = (u64)(p - start);
        if (nl > 0 && nl < 128) {
            char cn[128];
            memcpy(cn, start, nl); cn[nl] = '\0';
            int ci = col_index(t, cn);
            if (ci < 0) return 4;
            ix->col_idx[ix->col_count++] = (u32)ci;
        }
        if (*p == ',') p++;
    }
    if (ix->col_count == 0) return 4;
    t->index_count++;

    /* Scan every existing row and write index entries. */
    u8 prefix[DB_MAX_KEY_LEN];
    u64 plen = build_prefix_key(prefix, sizeof(prefix), t->name);
    if (plen > 0) {
        db_storage_iter *it = NULL;
        if (db->vt->iter_create(&it, db->storage, prefix, plen) == 0) {
            const u8 *k; u64 kl;
            const u8 *v; u64 vl;
            while (db_storage_iter_next(&k, &kl, &v, &vl, it) == 0) {
                db_row row;
                if (row_deserialize(&row, v, vl) != 0) continue;
                /* Parse rowid from row key tail. */
                i64 rowid = 0;
                for (i64 j = (i64)kl - 1; j >= 0; --j) {
                    if (k[j] == '/') {
                        char tmp[32]; tmp[0] = '\0';
                        u64 n = kl - (u64)j - 1;
                        if (n < sizeof(tmp)) {
                            memcpy(tmp, k + j + 1, n); tmp[n] = '\0';
                            rowid = strtoll(tmp, NULL, 10);
                        }
                        break;
                    }
                }
                u8 key[DB_MAX_KEY_LEN];
                u64 eklen = build_idx_entry_key_composite(key, sizeof(key),
                                                            t->name, ix->name,
                                                            &row, ix->col_idx,
                                                            ix->col_count, rowid);
                if (eklen > 0) wal_put(db, key, eklen, (const u8 *)"", 0);
                row_free(&row);
            }
            db_storage_iter_destroy(it);
        }
    }

    return flush_meta(db, t);
}

/* ------------------------------------------------------------------ */
/*  Execute DROP INDEX                                                 */
/* ------------------------------------------------------------------ */

static unsigned long exec_drop_index(db_conn *db, const sql_ast *ast) {
    if (!ast->text) return 1;
    const char *src = ast->text;
    int ie = 0;
    if (strncasecmp_a(src, "IF EXISTS ", 10) == 0) { ie = 1; src += 10; }
    const char *idx_name = src;

    /* Locate the index in any table. */
    db_table *owner = NULL;
    u32 ix_pos = 0;
    for (u32 ti = 0; ti < db->table_count && !owner; ++ti) {
        db_table *t = &db->tables[ti];
        for (u32 i = 0; i < t->index_count; ++i) {
            if (strcasecmp_a(t->indexes[i].name, idx_name) == 0) {
                owner = t; ix_pos = i; break;
            }
        }
    }
    if (!owner) return ie ? 0 : 2;

    /* Delete every index entry matching the prefix `__idx__/<tbl>/<idx>/`. */
    u8 prefix[DB_MAX_KEY_LEN];
    int np = snprintf((char *)prefix, sizeof(prefix),
                       "%s%s/%s/", DB_IDX_PREFIX, owner->name, idx_name);
    if (np > 0 && (u64)np < sizeof(prefix)) {
        db_storage_iter *it = NULL;
        if (db->vt->iter_create(&it, db->storage, prefix, (u64)np) == 0) {
            /* Collect keys first to avoid modifying during iteration. */
            u8 *keys[4096]; u64 klens[4096]; u64 n = 0;
            const u8 *k; u64 kl;
            const u8 *v; u64 vl;
            while (n < 4096 && db_storage_iter_next(&k, &kl, &v, &vl, it) == 0) {
                keys[n] = (u8 *)malloc(kl);
                if (!keys[n]) break;
                memcpy(keys[n], k, kl);
                klens[n] = kl;
                n++;
            }
            db_storage_iter_destroy(it);
            for (u64 i = 0; i < n; ++i) {
                wal_del(db, keys[i], klens[i]);
                free(keys[i]);
            }
        }
    }

    /* Remove from the owner's indexes[] and shift the tail down. */
    for (u32 i = ix_pos; i + 1 < owner->index_count; ++i) {
        owner->indexes[i] = owner->indexes[i + 1];
    }
    owner->index_count--;
    return flush_meta(db, owner);
}

/* ------------------------------------------------------------------ */
/*  Execute INSERT                                                     */
/* ------------------------------------------------------------------ */

/* Evaluate a SQL_AST_FUNC_CALL node into a caller-owned buffer. Only a
 * small set of datetime helpers is supported — enough to cover
 * cookbook's `datetime('now')` / `date('now')` / `time('now')` /
 * `current_timestamp` usage. Other function names leave the buffer
 * empty and return non-zero so the caller can surface the gap. */
static int eval_func_call(char *buf, u64 buf_cap, const sql_ast *call) {
    if (!buf || buf_cap < 20) return 1;
    const char *fn = call->text;
    if (!fn) return 2;

    const char *fmt = NULL;
    if (strcasecmp_a(fn, "datetime") == 0 ||
        strcasecmp_a(fn, "current_timestamp") == 0) {
        fmt = "%Y-%m-%d %H:%M:%S";
    } else if (strcasecmp_a(fn, "date") == 0) {
        fmt = "%Y-%m-%d";
    } else if (strcasecmp_a(fn, "time") == 0) {
        fmt = "%H:%M:%S";
    } else {
        buf[0] = '\0';
        return 3;  /* unsupported function */
    }

    /* If an arg is present it must be 'now' (other modifiers not yet
     * supported — e.g. julianday, weekday, start-of-day). */
    if (call->child_count > 0) {
        const char *a = call->children[0].text;
        if (!a || strcasecmp_a(a, "now") != 0) { buf[0] = '\0'; return 4; }
    }

    time_t now = time(NULL);
    struct tm *tm_g = gmtime(&now);
    if (!tm_g) { buf[0] = '\0'; return 5; }
    size_t n = strftime(buf, buf_cap, fmt, tm_g);
    if (n == 0) { buf[0] = '\0'; return 6; }
    return 0;
}

/* Drop the first row that collides with `new_row` on any UNIQUE/PK set.
 * Used by INSERT OR REPLACE. Walks all rows, finds the colliding one,
 * deletes it (row + indexes). Returns 0 if no collision found or a row
 * was deleted; non-zero on storage error. */
static unsigned long delete_conflicting_rows(db_conn *db, const db_table *t,
                                              const db_row *new_row) {
    if (t->unique_set_count == 0) return 0;

    u8 prefix[DB_MAX_KEY_LEN];
    u64 plen = build_prefix_key(prefix, sizeof(prefix), t->name);
    if (plen == 0) return 0;

    db_storage_iter *it = NULL;
    if (db->vt->iter_create(&it, db->storage, prefix, plen) != 0) return 0;

    /* Collect conflicting keys first (can't delete while iterating the
     * engine's snapshot-backed iter, but for safety we collect-then-delete). */
    u8   *del_keys[DB_MAX_TABLES];
    u64   del_klens[DB_MAX_TABLES];
    i64   del_rowids[DB_MAX_TABLES];
    db_row del_rows[DB_MAX_TABLES];
    u32   dcount = 0;

    const u8 *k; u64 kl;
    const u8 *v; u64 vl;
    while (db_storage_iter_next(&k, &kl, &v, &vl, it) == 0
           && dcount < DB_MAX_TABLES) {
        db_row other;
        if (row_deserialize(&other, v, vl) != 0) continue;
        int collide = 0;
        for (u32 i = 0; i < t->unique_set_count; ++i) {
            if (rows_collide_on_set(&t->unique_sets[i], new_row, &other)) {
                collide = 1; break;
            }
        }
        if (!collide) { row_free(&other); continue; }
        /* Save for delete */
        u8 *kcp = (u8 *)malloc(kl);
        if (!kcp) { row_free(&other); break; }
        memcpy(kcp, k, kl);
        del_keys[dcount] = kcp;
        del_klens[dcount] = kl;
        /* Parse rowid from key tail for index cleanup */
        i64 rid = 0;
        for (i64 j = (i64)kl - 1; j >= 0; --j) {
            if (k[j] == '/') {
                char tmp[32]; tmp[0] = '\0';
                u64 nn = kl - (u64)j - 1;
                if (nn < sizeof(tmp)) {
                    memcpy(tmp, k + j + 1, nn); tmp[nn] = '\0';
                    rid = strtoll(tmp, NULL, 10);
                }
                break;
            }
        }
        del_rowids[dcount] = rid;
        del_rows[dcount] = other;   /* transferred: we free below */
        dcount++;
    }
    db_storage_iter_destroy(it);

    unsigned long rc = 0;
    for (u32 i = 0; i < dcount; i++) {
        /* Remove index entries for this row first (same pattern as
         * exec_delete: drop every index entry tied to this rowid). */
        delete_row_indexes(db, t, &del_rows[i], del_rowids[i]);
        /* Then the row itself */
        if (wal_del(db, del_keys[i], del_klens[i]) != 0) rc = 1;
        row_free(&del_rows[i]);
        free(del_keys[i]);
    }
    return rc;
}

static unsigned long exec_insert(db_conn *db, const sql_ast *ast) {
    const char *tname = ast->text;
    if (!tname) return 1;

    db_table *t = find_table(db, tname);
    if (!t) return 2;

    int is_or_replace = (ast->type == SQL_AST_INSERT_OR_REPLACE);

    /* Separate column refs and literal values from children. FUNC_CALL
     * children are evaluated into per-slot scratch buffers. */
    u32 col_refs[DB_MAX_COLUMNS];
    const char *vals[DB_MAX_COLUMNS];
    char func_scratch[DB_MAX_COLUMNS][64];
    u32 ref_count = 0, val_count = 0;

    for (u64 i = 0; i < ast->child_count; ++i) {
        const sql_ast *child = &ast->children[i];
        if (child->type == SQL_AST_COLUMN_REF) {
            if (ref_count < DB_MAX_COLUMNS) {
                int ci = col_index(t, child->text);
                if (ci < 0) return 3; /* unknown column */
                col_refs[ref_count++] = (u32)ci;
            }
        } else if (child->type == SQL_AST_LITERAL) {
            if (val_count < DB_MAX_COLUMNS) {
                vals[val_count++] = child->text;
            }
        } else if (child->type == SQL_AST_FUNC_CALL) {
            if (val_count < DB_MAX_COLUMNS) {
                if (eval_func_call(func_scratch[val_count],
                                    sizeof(func_scratch[val_count]),
                                    child) != 0) {
                    return 10;  /* unsupported or bad function call */
                }
                vals[val_count] = func_scratch[val_count];
                val_count++;
            }
        }
    }

    /* If no column refs, values map to columns in order */
    if (ref_count == 0) {
        ref_count = t->col_count;
        for (u32 i = 0; i < ref_count; ++i) col_refs[i] = i;
    }

    if (val_count != ref_count) return 4;

    db_row row;
    memset(&row, 0, sizeof(row));
    row.col_count = t->col_count;

    /* Initialize all columns to NULL */
    for (u32 i = 0; i < row.col_count; ++i) {
        row.types[i] = DB_TYPE_NULL;
    }

    /* Fill in provided values */
    for (u32 i = 0; i < val_count; ++i) {
        literal_to_col(&row, col_refs[i], vals[i], t->cols[col_refs[i]].type);
    }

    /* INSERT OR REPLACE: silently drop any row that would collide with
     * the new one on a UNIQUE/PK set. Runs before the unique check so
     * that check sees a clean slate for `row`. */
    if (is_or_replace) {
        unsigned long drc = delete_conflicting_rows(db, t, &row);
        if (drc) { row_free(&row); return drc; }
    }

    /* UNIQUE constraint check — all single-column UNIQUE columns and
     * table-level UNIQUE(...) sets are enforced as one path. Any
     * collision against an existing row aborts with hatch 9
     * (COOKBOOK_DB_CONSTRAINT in vtable mappings). */
    unsigned long urc = check_unique_constraints(db, t, &row, NULL, 0);
    if (urc) { row_free(&row); return urc; }

    /* FOREIGN KEY check — for each FK column, the local value (if not
     * NULL) must exist in the referenced parent table. Hatch 10 on
     * violation. */
    unsigned long frc = check_fk_constraints_for_row(db, t, &row);
    if (frc) { row_free(&row); return frc; }

    /* Serialize and store */
    u8 row_buf[DB_MAX_ROW_SIZE];
    u64 rlen = row_serialize(&row, row_buf, sizeof(row_buf));
    if (rlen == 0) { row_free(&row); return 5; }

    i64 rowid = t->next_rowid++;
    u8 key_buf[DB_MAX_KEY_LEN];
    u64 klen = build_row_key(key_buf, sizeof(key_buf), t->name, rowid);
    if (klen == 0) { row_free(&row); return 6; }

    unsigned long rc = wal_put(db, key_buf, klen, row_buf, rlen);
    if (rc) { row_free(&row); return 7; }

    /* Maintain secondary indexes. Happens AFTER the row is written so
     * the index entries' rowids always point to a live row. */
    write_row_indexes(db, t, &row, rowid);
    row_free(&row);

    /* Update metadata with new next_rowid */
    rc = flush_meta(db, t);
    if (rc) return 8;

    db->last_insert_id = rowid;
    db->changes++;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Execute UPDATE                                                     */
/* ------------------------------------------------------------------ */

static unsigned long exec_update(db_conn *db, const sql_ast *ast) {
    const char *tname = ast->text;
    if (!tname) return 1;

    db_table *t = find_table(db, tname);
    if (!t) return 2;

    /* Gather assignments and WHERE */
    typedef struct { u32 ci; const char *val; } assign_t;
    assign_t assigns[DB_MAX_COLUMNS];
    u32 assign_count = 0;
    const sql_ast *where_node = NULL;

    for (u64 i = 0; i < ast->child_count; ++i) {
        const sql_ast *child = &ast->children[i];
        if (child->type == SQL_AST_ASSIGNMENT) {
            if (assign_count >= DB_MAX_COLUMNS) continue;
            int ci = col_index(t, child->text);
            if (ci < 0) return 3;
            assigns[assign_count].ci = (u32)ci;
            assigns[assign_count].val = child->left ? child->left->text : NULL;
            assign_count++;
        } else if (child->type == SQL_AST_BINARY_OP ||
                   child->type == SQL_AST_AND ||
                   child->type == SQL_AST_OR) {
            where_node = child;
        }
    }

    /* Iterate rows */
    u8 prefix[DB_MAX_KEY_LEN];
    u64 plen = build_prefix_key(prefix, sizeof(prefix), t->name);
    if (plen == 0) return 4;

    db_storage_iter *it = NULL;
    unsigned long rc = db->vt->iter_create(&it, db->storage, prefix, plen);
    if (rc) return 5;

    /* Collect matching keys and updated values */
    typedef struct { u8 *key; u64 klen; u8 *val; u64 vlen; } pending_t;
    pending_t *pending = NULL;
    u64 pend_count = 0, pend_cap = 0;

    const u8 *k; u64 kl;
    const u8 *v; u64 vl;
    while (db_storage_iter_next(&k, &kl, &v, &vl, it) == 0) {
        db_row row;
        if (row_deserialize(&row, v, vl) != 0) continue;

        if (!eval_where(where_node, &row, t)) {
            row_free(&row);
            continue;
        }

        /* Apply assignments */
        for (u32 a = 0; a < assign_count; ++a) {
            u32 ci = assigns[a].ci;
            /* Free old blob data if present */
            if (row.bvals[ci]) { free(row.bvals[ci]); row.bvals[ci] = NULL; }
            row.blens[ci] = 0;
            literal_to_col(&row, ci, assigns[a].val, t->cols[ci].type);
        }

        /* UNIQUE check on the updated row, excluding itself. If any
         * other row already holds the new values for a unique set,
         * bail the whole UPDATE with hatch 9 (transactionally, every
         * pending change is discarded because we haven't applied them
         * yet). */
        if (check_unique_constraints(db, t, &row, k, kl) != 0) {
            row_free(&row);
            for (u64 p = 0; p < pend_count; ++p) {
                free(pending[p].key);
                free(pending[p].val);
            }
            free(pending);
            db_storage_iter_destroy(it);
            return 9;
        }

        /* FK check on updated row. */
        if (check_fk_constraints_for_row(db, t, &row) != 0) {
            row_free(&row);
            for (u64 p = 0; p < pend_count; ++p) {
                free(pending[p].key);
                free(pending[p].val);
            }
            free(pending);
            db_storage_iter_destroy(it);
            return 10;
        }

        /* Serialize updated row */
        u8 row_buf[DB_MAX_ROW_SIZE];
        u64 rlen = row_serialize(&row, row_buf, sizeof(row_buf));
        row_free(&row);
        if (rlen == 0) continue;

        /* Add to pending list */
        if (pend_count >= pend_cap) {
            u64 nc = pend_cap ? pend_cap * 2 : 32;
            pending_t *tmp = realloc(pending, nc * sizeof(pending_t));
            if (!tmp) { free(pending); db_storage_iter_destroy(it); return 6; }
            pending = tmp;
            pend_cap = nc;
        }
        pending[pend_count].key = malloc(kl);
        pending[pend_count].val = malloc(rlen);
        if (pending[pend_count].key && pending[pend_count].val) {
            memcpy(pending[pend_count].key, k, kl);
            pending[pend_count].klen = kl;
            memcpy(pending[pend_count].val, row_buf, rlen);
            pending[pend_count].vlen = rlen;
            pend_count++;
        }
    }
    db_storage_iter_destroy(it);

    /* Apply pending updates. For each row being updated, also sync
     * its secondary-index entries: delete old, write new. */
    for (u64 i = 0; i < pend_count; ++i) {
        /* Parse rowid from row key for index ops. */
        i64 rowid = 0;
        for (i64 j = (i64)pending[i].klen - 1; j >= 0; --j) {
            if (pending[i].key[j] == '/') {
                char tmp[32]; tmp[0] = '\0';
                u64 n = pending[i].klen - (u64)j - 1;
                if (n < sizeof(tmp)) {
                    memcpy(tmp, pending[i].key + j + 1, n); tmp[n] = '\0';
                    rowid = strtoll(tmp, NULL, 10);
                }
                break;
            }
        }
        /* Read current old value, delete its index entries. */
        if (t->index_count > 0) {
            u8 *ov = NULL; u64 ovl = 0;
            if (db->vt->get(&ov, &ovl, db->storage, pending[i].key, pending[i].klen) == 0
                && ov) {
                db_row oldr;
                if (row_deserialize(&oldr, ov, ovl) == 0) {
                    delete_row_indexes(db, t, &oldr, rowid);
                    row_free(&oldr);
                }
                free(ov);
            }
        }
        wal_put(db, pending[i].key, pending[i].klen,
                pending[i].val, pending[i].vlen);
        /* Write new index entries. */
        if (t->index_count > 0) {
            db_row newr;
            if (row_deserialize(&newr, pending[i].val, pending[i].vlen) == 0) {
                write_row_indexes(db, t, &newr, rowid);
                row_free(&newr);
            }
        }
        free(pending[i].key);
        free(pending[i].val);
        db->changes++;
    }
    free(pending);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Execute DELETE                                                     */
/* ------------------------------------------------------------------ */

static unsigned long exec_delete(db_conn *db, const sql_ast *ast) {
    const char *tname = ast->text;
    if (!tname) return 1;

    db_table *t = find_table(db, tname);
    if (!t) return 2;

    const sql_ast *where_node = find_where(ast);

    u8 prefix[DB_MAX_KEY_LEN];
    u64 plen = build_prefix_key(prefix, sizeof(prefix), t->name);
    if (plen == 0) return 3;

    db_storage_iter *it = NULL;
    unsigned long rc = db->vt->iter_create(&it, db->storage, prefix, plen);
    if (rc) return 4;

    /* Collect keys to delete. Also verify no child FK references any
     * matching row — if yes, abort the whole DELETE with hatch 11
     * (RESTRICT). ON DELETE CASCADE is not yet implemented; row
     * removal only proceeds when no children reference it. */
    u8 *del_keys[4096];
    u64 del_klens[4096];
    u64 del_count = 0;

    const u8 *k; u64 kl;
    const u8 *v; u64 vl;
    unsigned long fk_rc = 0;
    while (db_storage_iter_next(&k, &kl, &v, &vl, it) == 0) {
        db_row row;
        if (row_deserialize(&row, v, vl) != 0) continue;
        int match = eval_where(where_node, &row, t);
        if (match) {
            unsigned long fk_apply = apply_fk_on_delete(db, t, &row);
            if (fk_apply) {
                fk_rc = fk_apply;
                row_free(&row);
                break;
            }
        }
        row_free(&row);
        if (!match) continue;

        if (del_count < 4096) {
            del_keys[del_count] = malloc(kl);
            if (del_keys[del_count]) {
                memcpy(del_keys[del_count], k, kl);
                del_klens[del_count] = kl;
                del_count++;
            }
        }
    }
    db_storage_iter_destroy(it);

    if (fk_rc) {
        for (u64 i = 0; i < del_count; ++i) free(del_keys[i]);
        return fk_rc;
    }

    for (u64 i = 0; i < del_count; ++i) {
        /* Sync indexes: read the row so we know its indexed-column
         * values, then delete index entries, then delete the row. */
        if (t->index_count > 0) {
            u8 *ov = NULL; u64 ovl = 0;
            if (db->vt->get(&ov, &ovl, db->storage, del_keys[i], del_klens[i]) == 0
                && ov) {
                db_row oldr;
                if (row_deserialize(&oldr, ov, ovl) == 0) {
                    i64 rowid = 0;
                    for (i64 j = (i64)del_klens[i] - 1; j >= 0; --j) {
                        if (del_keys[i][j] == '/') {
                            char tmp[32]; tmp[0] = '\0';
                            u64 n = del_klens[i] - (u64)j - 1;
                            if (n < sizeof(tmp)) {
                                memcpy(tmp, del_keys[i] + j + 1, n); tmp[n] = '\0';
                                rowid = strtoll(tmp, NULL, 10);
                            }
                            break;
                        }
                    }
                    delete_row_indexes(db, t, &oldr, rowid);
                    row_free(&oldr);
                }
                free(ov);
            }
        }
        wal_del(db, del_keys[i], del_klens[i]);
        free(del_keys[i]);
        db->changes++;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Parse + execute a SQL statement (no result set)                    */
/* ------------------------------------------------------------------ */

/* Return 1 if the statement at `s` (null-terminated, whitespace-trimmed)
 * is a PRAGMA we should accept and no-op. Cookbook's migration uses
 * `PRAGMA foreign_keys = ON` and `PRAGMA journal_mode = WAL`; neither
 * is actionable in our engine yet, but the SQL must validate. */
static int is_pragma_statement(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (strncasecmp_a(s, "PRAGMA", 6) != 0) return 0;
    char next = s[6];
    return next == '\0' || next == ' ' || next == '\t'
         || next == '\n' || next == '\r';
}

/* Return 1 if the statement is empty / comment-only. */
static int is_empty_statement(const char *s) {
    while (*s) {
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') { s++; continue; }
        if (*s == '-' && s[1] == '-') {
            while (*s && *s != '\n') s++;
            continue;
        }
        return 0;
    }
    return 1;
}

static unsigned long exec_single_statement(db_conn *db, const char *sql) {
    if (is_empty_statement(sql)) return 0;
    if (is_pragma_statement(sql)) return 0;

    sql_token_list tokens;
    memset(&tokens, 0, sizeof(tokens));
    unsigned long rc = sql_tokenize(&tokens, sql);
    if (rc) return 1;

    sql_ast *ast = NULL;
    rc = sql_parse(&ast, &tokens);
    sql_token_list_free(&tokens);
    if (rc || !ast) return 2;

    switch (ast->type) {
    case SQL_AST_CREATE_TABLE: rc = exec_create_table(db, ast); break;
    case SQL_AST_CREATE_INDEX: rc = exec_create_index(db, ast); break;
    case SQL_AST_DROP_TABLE:   rc = exec_drop_table(db, ast);   break;
    case SQL_AST_DROP_INDEX:   rc = exec_drop_index(db, ast);   break;
    case SQL_AST_INSERT:
    case SQL_AST_INSERT_OR_REPLACE:
                               rc = exec_insert(db, ast);       break;
    case SQL_AST_UPDATE:       rc = exec_update(db, ast);       break;
    case SQL_AST_DELETE:       rc = exec_delete(db, ast);       break;
    default:                   rc = 3; break;
    }

    sql_ast_destroy(ast);
    return rc;
}

static unsigned long parse_and_exec(db_conn *db, const char *sql) {
    /* Cookbook ships its schema as a single semicolon-separated blob.
     * Split on top-level `;` (respecting single-quoted string literals)
     * and run each piece. Any one failure aborts the batch. */
    u64 len = strlen(sql);
    u64 start = 0;
    int in_string = 0;
    for (u64 i = 0; i <= len; ++i) {
        char c = sql[i];
        if (in_string) {
            if (c == '\'') {
                if (i + 1 < len && sql[i + 1] == '\'') { i++; continue; }
                in_string = 0;
            }
            continue;
        }
        if (c == '\'') { in_string = 1; continue; }
        if (c == ';' || c == '\0') {
            if (i > start) {
                u64 slen = i - start;
                char *stmt = (char *)malloc(slen + 1);
                if (!stmt) return 4;
                memcpy(stmt, sql + start, slen);
                stmt[slen] = '\0';
                unsigned long rc = exec_single_statement(db, stmt);
                free(stmt);
                if (rc) return rc;
            }
            start = i + 1;
        }
    }
    return 0;
}

/* ================================================================== */
/*  PUBLIC API                                                         */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/*  db_open                                                            */
/* ------------------------------------------------------------------ */

APENNINES_API unsigned long db_open_ex(db_conn **out, const char *path, u32 backend) {
    if (!out)  return 1;
    if (!path) return 2;

    db_conn *db = calloc(1, sizeof(db_conn));
    if (!db) return 3;

    snprintf(db->path, sizeof(db->path), "%s", path);

    if (db_storage_get_vt(&db->vt, backend) != 0) { free(db); return 7; }

    if (rwlock_create(&db->conn_lock) != 0) { free(db); return 6; }
    db->conn_lock_initialised = 1;

    /* Open storage (backend-specific file layout) */
    char kv_path[560];
    const char *suffix = (backend == DB_STORAGE_BTREE) ? ".btree" : ".kv";
    snprintf(kv_path, sizeof(kv_path), "%s%s", path, suffix);
    unsigned long rc = db->vt->open(&db->storage, kv_path);
    if (rc) {
        rwlock_destroy(&db->conn_lock);
        free(db);
        return 4;
    }

    /* Open WAL */
    char wal_path[560];
    snprintf(wal_path, sizeof(wal_path), "%s.wal", path);
    rc = wal_create(&db->log, wal_path);
    if (rc) {
        /* WAL is optional — proceed without it */
        db->log = NULL;
    }

    /* Verify / initialise the DB header. If the KV is empty (fresh DB),
     * or has tables but no header (legacy v1 pre-header), write one.
     * Refuse to open if the header is present but magic is wrong or the
     * version is ahead of what we understand. */
    u8 *hdr_bytes = NULL; u64 hdr_len = 0;
    unsigned long hrc = db->vt->get(&hdr_bytes, &hdr_len, db->storage,
                                 (const u8 *)DB_HDR_KEY, DB_HDR_KEY_LEN);
    if (hrc == 0 && hdr_bytes) {
        u16 disk_ver = 0;
        int hpr = hdr_parse(hdr_bytes, hdr_len, &disk_ver);
        free(hdr_bytes);
        if (hpr < 0) {
            if (db->log) wal_close(db->log);
            db->vt->close(db->storage);
            rwlock_destroy(&db->conn_lock);
            free(db);
            return 5;  /* unsupported / corrupt header */
        }
        /* disk_ver is valid and <= current; future migrations would
         * dispatch here. v1 → v1 is a no-op. */
    } else {
        /* No header key yet — write the current version. Covers both
         * brand-new KV stores and legacy v1 stores that were created
         * before we started writing headers. */
        u8 hdr_buf[DB_HDR_SIZE];
        hdr_serialize(hdr_buf);
        (void)db->vt->put(db->storage, (const u8 *)DB_HDR_KEY, DB_HDR_KEY_LEN,
                      hdr_buf, DB_HDR_SIZE);
    }

    /* Scan metadata keys to rebuild table registry */
    db_storage_iter *it = NULL;
    rc = db->vt->iter_create(&it, db->storage,
                        (const u8 *)DB_META_PREFIX, DB_META_PREFIX_LEN);
    if (rc == 0) {
        const u8 *k; u64 kl;
        const u8 *v; u64 vl;
        while (db_storage_iter_next(&k, &kl, &v, &vl, it) == 0) {
            if (db->table_count >= DB_MAX_TABLES) break;
            if (kl <= DB_META_PREFIX_LEN) continue;

            db_table *t = &db->tables[db->table_count];
            memset(t, 0, sizeof(*t));

            /* Extract table name from key */
            u64 nlen = kl - DB_META_PREFIX_LEN;
            if (nlen >= sizeof(t->name)) nlen = sizeof(t->name) - 1;
            memcpy(t->name, k + DB_META_PREFIX_LEN, nlen);
            t->name[nlen] = '\0';

            if (meta_deserialize(t, v, vl) == 0) {
                db->table_count++;
            }
        }
        db_storage_iter_destroy(it);
    }

    *out = db;
    return 0;
}

APENNINES_API unsigned long db_open(db_conn **out, const char *path) {
    return db_open_ex(out, path, DB_STORAGE_HASHKV);
}

/* ------------------------------------------------------------------ */
/*  db_close                                                           */
/* ------------------------------------------------------------------ */

APENNINES_API unsigned long db_close(db_conn *db) {
    if (!db) return 1;

    undo_clear(db);
    free(db->undo);
    if (db->log) wal_close(db->log);
    if (db->storage)  db->vt->close(db->storage);
    if (db->conn_lock_initialised) rwlock_destroy(&db->conn_lock);
    free(db);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Query planner                                                      */
/*                                                                     */
/*  Trivial for now: if the WHERE clause is a single top-level         */
/*  `col = literal` comparison and `col` has a secondary index, use    */
/*  the index to limit the scan to rows matching that value. WHERE     */
/*  clauses under AND / OR aren't recognised; we fall back to a full   */
/*  table scan. Range queries (>, <, BETWEEN) aren't yet index-backed. */
/* ------------------------------------------------------------------ */

/* Type-aware compare between two index-value texts (whatever
 * col_val_to_text emitted). INTEGER → strtoll; REAL → strtod;
 * TEXT/BLOB → strcmp. Returns -1/0/1. */
static int idx_val_compare(int col_type, const char *a, const char *b) {
    if (col_type == DB_TYPE_INTEGER) {
        long long av = strtoll(a, NULL, 10);
        long long bv = strtoll(b, NULL, 10);
        if (av < bv) return -1;
        if (av > bv) return 1;
        return 0;
    }
    if (col_type == DB_TYPE_REAL) {
        double av = strtod(a, NULL);
        double bv = strtod(b, NULL);
        if (av < bv) return -1;
        if (av > bv) return 1;
        return 0;
    }
    return strcmp(a, b);
}

/* Extract the first column's value from an index entry key, given
 * the prefix length (which covers "__idx__/<tbl>/<idx>/"). The value
 * runs up to the next '/'. Returns 0 on success. Assumes values
 * don't contain '/' — safe for INTEGER/REAL and for the TEXT columns
 * cookbook actually indexes (datetime strings, opaque UUIDs). */
static int extract_first_val_from_idx_key(const u8 *k, u64 kl,
                                             u64 prefix_len,
                                             char *out, u64 cap) {
    if (kl <= prefix_len) return -1;
    u64 start = prefix_len;
    u64 end = start;
    while (end < kl && k[end] != '/') end++;
    u64 vl = end - start;
    if (vl == 0 || vl >= cap) return -1;
    memcpy(out, k + start, vl);
    out[vl] = '\0';
    return 0;
}

/* Populate `out` plan from a WHERE node. Recognises top-level:
 *   col = lit                → EQ (tight prefix, no per-entry check)
 *   col > lit / >= / < / <=  → RANGE with one bound
 *   col BETWEEN lo AND hi    → RANGE with both bounds
 * Returns 1 if the plan is usable (an index exists for `col`),
 * 0 otherwise (full-scan fallback). */
static int planner_build(const sql_ast *where, const db_table *t,
                           const db_index **out_ix,
                           int *kind, int *col_type,
                           char *eq_val, u64 eq_val_cap,
                           int *has_lower, int *lower_inclusive,
                           char *lower, u64 lower_cap,
                           int *has_upper, int *upper_inclusive,
                           char *upper, u64 upper_cap) {
    *out_ix = NULL;
    *kind = IDX_USE_NONE;
    *has_lower = *has_upper = 0;
    if (!where || !t) return 0;
    if (where->type != SQL_AST_BINARY_OP || !where->text) return 0;

    const char *op = where->text;
    const char *col_name = NULL;
    const char *lit = NULL;
    const char *lo = NULL, *hi = NULL;

    if (strcmp(op, "BETWEEN") == 0) {
        if (!where->left || where->left->type != SQL_AST_COLUMN_REF) return 0;
        col_name = where->left->text;
        if (where->child_count < 2) return 0;
        lo = where->children[0].text;
        hi = where->children[1].text;
    } else if (strcmp(op, "=")  == 0 || strcmp(op, "<") == 0
            || strcmp(op, "<=") == 0 || strcmp(op, ">") == 0
            || strcmp(op, ">=") == 0) {
        if (!where->left || !where->right) return 0;
        if (where->left->type == SQL_AST_COLUMN_REF
            && where->right->type == SQL_AST_LITERAL) {
            col_name = where->left->text;
            lit = where->right->text;
        } else if (where->left->type == SQL_AST_LITERAL
                   && where->right->type == SQL_AST_COLUMN_REF) {
            col_name = where->right->text;
            lit = where->left->text;
            /* Flip direction: `lit < col` is `col > lit`, etc. */
            if      (strcmp(op, "<") == 0)  op = ">";
            else if (strcmp(op, "<=") == 0) op = ">=";
            else if (strcmp(op, ">") == 0)  op = "<";
            else if (strcmp(op, ">=") == 0) op = "<=";
        } else {
            return 0;
        }
    } else {
        return 0;
    }

    if (!col_name) return 0;
    int ci = col_index(t, col_name);
    if (ci < 0) return 0;
    const db_index *ix = NULL;
    for (u32 i = 0; i < t->index_count; ++i) {
        if (t->indexes[i].col_count > 0
            && t->indexes[i].col_idx[0] == (u32)ci) {
            ix = &t->indexes[i];
            break;
        }
    }
    if (!ix) return 0;

    *out_ix = ix;
    *col_type = t->cols[ci].type;

    if (lo && hi) {
        *kind = IDX_USE_RANGE;
        *has_lower = 1; *lower_inclusive = 1;
        snprintf(lower, lower_cap, "%s", lo);
        *has_upper = 1; *upper_inclusive = 1;
        snprintf(upper, upper_cap, "%s", hi);
        return 1;
    }
    if (lit && strcmp(op, "=") == 0) {
        *kind = IDX_USE_EQ;
        snprintf(eq_val, eq_val_cap, "%s", lit);
        return 1;
    }
    if (lit) {
        *kind = IDX_USE_RANGE;
        if (strcmp(op, ">")  == 0) { *has_lower = 1; *lower_inclusive = 0; snprintf(lower, lower_cap, "%s", lit); }
        if (strcmp(op, ">=") == 0) { *has_lower = 1; *lower_inclusive = 1; snprintf(lower, lower_cap, "%s", lit); }
        if (strcmp(op, "<")  == 0) { *has_upper = 1; *upper_inclusive = 0; snprintf(upper, upper_cap, "%s", lit); }
        if (strcmp(op, "<=") == 0) { *has_upper = 1; *upper_inclusive = 1; snprintf(upper, upper_cap, "%s", lit); }
        return 1;
    }
    return 0;
}

/* Canonical synthetic column name for an aggregate spec. Shared by
 * synth_group_table_build and rewrite_having_agg_refs so they agree. */
static void synth_agg_col_name(char *out, u64 cap, const db_agg_spec *spec,
                                 const db_table *source) {
    static const char *fn_names[] = {"COUNT","SUM","MIN","MAX","AVG"};
    const char *fn = fn_names[spec->fn];
    if (spec->star) {
        snprintf(out, cap, "__agg_%s_star__", fn);
    } else if (source && spec->col_idx >= 0
               && (u32)spec->col_idx < source->col_count) {
        snprintf(out, cap, "__agg_%s_%s__", fn,
                  source->cols[spec->col_idx].name);
    } else {
        snprintf(out, cap, "__agg_%s_?__", fn);
    }
}

/* Build stmt->synth_group_table so HAVING's eval_where can resolve
 * both the group column name and the synthetic aggregate names. */
static void synth_group_table_build(db_stmt *s) {
    db_table *gt = &s->synth_group_table;
    memset(gt, 0, sizeof(*gt));
    snprintf(gt->name, sizeof(gt->name), "__group__");
    u32 cc = 0;
    /* Group columns first, in declaration order, so HAVING can refer
     * to any of them by the source table's column name. */
    for (u32 i = 0; i < s->group_col_count && cc < DB_MAX_COLUMNS; ++i) {
        u32 gci = s->group_col_indices[i];
        if (s->table && gci < s->table->col_count) {
            gt->cols[cc++] = s->table->cols[gci];
        }
    }
    /* Then each aggregate, named "__agg_FN_arg__". */
    for (u32 i = 0; i < s->agg_count && cc < DB_MAX_COLUMNS; ++i) {
        db_col_def *c = &gt->cols[cc++];
        memset(c, 0, sizeof(*c));
        synth_agg_col_name(c->name, sizeof(c->name),
                            &s->agg_specs[i], s->table);
        c->type = (s->agg_specs[i].fn == DB_AGG_AVG) ? DB_TYPE_REAL
                                                      : DB_TYPE_INTEGER;
    }
    gt->col_count = cc;
    s->synth_group_table_built = 1;
}

/* Walk a HAVING AST and rewrite every SQL_AST_AGG_REF into a
 * SQL_AST_COLUMN_REF with the synthetic column name that
 * synth_group_table carries. After this the regular eval_where path
 * handles HAVING without any special casing. */
static void rewrite_having_agg_refs(sql_ast *node, const db_stmt *s) {
    if (!node) return;
    if (node->type == SQL_AST_AGG_REF && node->text) {
        char fn_name[16], arg[64];
        fn_name[0] = arg[0] = '\0';
        sscanf(node->text, "%15s %63s", fn_name, arg);
        int fn_code = -1;
        if      (strcasecmp_a(fn_name, "COUNT") == 0) fn_code = DB_AGG_COUNT;
        else if (strcasecmp_a(fn_name, "SUM")   == 0) fn_code = DB_AGG_SUM;
        else if (strcasecmp_a(fn_name, "MIN")   == 0) fn_code = DB_AGG_MIN;
        else if (strcasecmp_a(fn_name, "MAX")   == 0) fn_code = DB_AGG_MAX;
        else if (strcasecmp_a(fn_name, "AVG")   == 0) fn_code = DB_AGG_AVG;
        int star = (strcmp(arg, "*") == 0);
        /* Find matching spec in stmt's agg_specs. */
        int found = -1;
        for (u32 i = 0; i < s->agg_count; ++i) {
            if ((int)s->agg_specs[i].fn != fn_code) continue;
            if (s->agg_specs[i].star != star) continue;
            if (!star && s->table) {
                if (s->agg_specs[i].col_idx < 0) continue;
                if (strcasecmp_a(
                        s->table->cols[s->agg_specs[i].col_idx].name,
                        arg) != 0) continue;
            }
            found = (int)i;
            break;
        }
        if (found >= 0) {
            char synth_name[128];
            synth_agg_col_name(synth_name, sizeof(synth_name),
                                &s->agg_specs[found], s->table);
            free(node->text);
            node->text = (char *)malloc(strlen(synth_name) + 1);
            if (node->text) strcpy(node->text, synth_name);
            node->type = SQL_AST_COLUMN_REF;
        }
    }
    if (node->left)  rewrite_having_agg_refs(node->left,  s);
    if (node->right) rewrite_having_agg_refs(node->right, s);
    for (u64 i = 0; i < node->child_count; ++i) {
        rewrite_having_agg_refs(&node->children[i], s);
    }
}

/* ------------------------------------------------------------------ */
/*  Aggregate evaluation                                               */
/*                                                                     */
/*  Walk every matching row and update each db_agg_spec's accumulator. */
/*  Build a one-row synthetic result in `out`. NULL semantics follow   */
/*  SQLite: COUNT(*) counts rows regardless; COUNT(col), SUM, MIN,     */
/*  MAX, AVG all ignore NULLs. SUM/AVG over zero non-null rows yields  */
/*  NULL (not 0). MIN/MAX over zero rows yields NULL. COUNT always     */
/*  returns INTEGER.                                                   */
/* ------------------------------------------------------------------ */

static void agg_update(db_agg_spec *spec, const db_row *row) {
    if (spec->fn == DB_AGG_COUNT) {
        if (spec->star) {
            spec->count_nonnull++;
            return;
        }
        if (spec->col_idx < 0) return;
        if ((u32)spec->col_idx >= row->col_count) return;
        if (row->types[spec->col_idx] != DB_TYPE_NULL) spec->count_nonnull++;
        return;
    }
    if (spec->col_idx < 0) return;
    if ((u32)spec->col_idx >= row->col_count) return;
    int ty = row->types[spec->col_idx];
    if (ty == DB_TYPE_NULL) return;

    double dv = 0.0;
    i64 iv = 0;
    int is_real = 0;
    if (ty == DB_TYPE_INTEGER) { iv = row->ivals[spec->col_idx]; dv = (double)iv; }
    else if (ty == DB_TYPE_REAL) { dv = row->fvals[spec->col_idx]; is_real = 1; }
    else return;  /* TEXT/BLOB: ignore for numeric aggregates */

    spec->count_nonnull++;
    switch (spec->fn) {
    case DB_AGG_SUM:
    case DB_AGG_AVG:
        spec->dsum += dv;
        if (!is_real) spec->isum += iv;
        if (is_real) spec->saw_real = 1;
        break;
    case DB_AGG_MIN:
        if (!spec->have_any) {
            spec->imin = iv; spec->fmin = dv;
            if (is_real) spec->saw_real = 1;
            spec->have_any = 1;
        } else {
            if (is_real || spec->saw_real) {
                if (dv < spec->fmin) spec->fmin = dv;
                spec->saw_real = spec->saw_real || is_real;
            } else {
                if (iv < spec->imin) spec->imin = iv;
                spec->fmin = (double)spec->imin;
            }
        }
        break;
    case DB_AGG_MAX:
        if (!spec->have_any) {
            spec->imax = iv; spec->fmax = dv;
            if (is_real) spec->saw_real = 1;
            spec->have_any = 1;
        } else {
            if (is_real || spec->saw_real) {
                if (dv > spec->fmax) spec->fmax = dv;
                spec->saw_real = spec->saw_real || is_real;
            } else {
                if (iv > spec->imax) spec->imax = iv;
                spec->fmax = (double)spec->imax;
            }
        }
        break;
    default: break;
    }
}

static void agg_resolve_into_row(const db_agg_spec *spec, db_row *out, u32 col) {
    switch (spec->fn) {
    case DB_AGG_COUNT:
        out->types[col] = DB_TYPE_INTEGER;
        out->ivals[col] = spec->count_nonnull;
        break;
    case DB_AGG_SUM:
        if (spec->count_nonnull == 0) { out->types[col] = DB_TYPE_NULL; break; }
        if (spec->saw_real) {
            out->types[col] = DB_TYPE_REAL;
            out->fvals[col] = spec->dsum;
        } else {
            out->types[col] = DB_TYPE_INTEGER;
            out->ivals[col] = spec->isum;
        }
        break;
    case DB_AGG_AVG:
        if (spec->count_nonnull == 0) { out->types[col] = DB_TYPE_NULL; break; }
        out->types[col] = DB_TYPE_REAL;
        out->fvals[col] = spec->dsum / (double)spec->count_nonnull;
        break;
    case DB_AGG_MIN:
        if (!spec->have_any) { out->types[col] = DB_TYPE_NULL; break; }
        if (spec->saw_real) {
            out->types[col] = DB_TYPE_REAL;
            out->fvals[col] = spec->fmin;
        } else {
            out->types[col] = DB_TYPE_INTEGER;
            out->ivals[col] = spec->imin;
        }
        break;
    case DB_AGG_MAX:
        if (!spec->have_any) { out->types[col] = DB_TYPE_NULL; break; }
        if (spec->saw_real) {
            out->types[col] = DB_TYPE_REAL;
            out->fvals[col] = spec->fmax;
        } else {
            out->types[col] = DB_TYPE_INTEGER;
            out->ivals[col] = spec->imax;
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  GROUP BY materialisation                                           */
/*                                                                     */
/*  Walks matching rows, buckets by the GROUP BY column's value, runs */
/*  aggregates per bucket, then emits one row per bucket into          */
/*  sorted_rows. Each row's column 0 is the group value; columns      */
/*  1..agg_count are the resolved aggregate values. HAVING is         */
/*  evaluated on the emitted row as a post-filter.                    */
/* ------------------------------------------------------------------ */

typedef struct {
    db_agg_spec specs[DB_MAX_COLUMNS];  /* one per stmt agg_spec */
    /* N parallel arrays holding the group values for each group column,
     * preserved so the emitted row carries correct types. */
    int    gv_types[DB_MAX_GROUP_COLS];
    i64    gv_ivals[DB_MAX_GROUP_COLS];
    double gv_fvals[DB_MAX_GROUP_COLS];
    u8    *gv_bvals[DB_MAX_GROUP_COLS];  /* owned, null-term'd for strcmp */
    u64    gv_blens[DB_MAX_GROUP_COLS];
    /* Concatenated text projection of the group-col values (separated
     * by \x01) — the bucket hash/equality key for linear probe lookup. */
    char   key[1024];
} db_group_bucket;

static void set_group_value_from_row_at(db_group_bucket *b, u32 slot,
                                          const db_row *row, u32 gci) {
    b->gv_types[slot] = row->types[gci];
    b->gv_ivals[slot] = row->ivals[gci];
    b->gv_fvals[slot] = row->fvals[gci];
    b->gv_blens[slot] = row->blens[gci];
    if ((b->gv_types[slot] == DB_TYPE_TEXT
         || b->gv_types[slot] == DB_TYPE_BLOB)
        && row->bvals[gci] && row->blens[gci] > 0) {
        b->gv_bvals[slot] = (u8 *)malloc(row->blens[gci] + 1);
        if (b->gv_bvals[slot]) {
            memcpy(b->gv_bvals[slot], row->bvals[gci], row->blens[gci]);
            b->gv_bvals[slot][row->blens[gci]] = 0;
        }
    }
}

static unsigned long materialise_grouped_result(db_stmt *s,
                                                   const sql_ast *where_node) {
    u8 prefix[DB_MAX_KEY_LEN];
    u64 plen = build_prefix_key(prefix, sizeof(prefix), s->table->name);
    if (plen == 0) return 1;
    db_storage_iter *it = NULL;
    if (s->db->vt->iter_create(&it, s->db->storage, prefix, plen) != 0) return 2;

    u64 cap = 16;
    db_group_bucket *buckets = (db_group_bucket *)calloc(cap, sizeof(*buckets));
    if (!buckets) { db_storage_iter_destroy(it); return 3; }
    u64 n_buckets = 0;

    const u8 *k; u64 kl;
    const u8 *v; u64 vl;
    while (db_storage_iter_next(&k, &kl, &v, &vl, it) == 0) {
        db_row row;
        if (row_deserialize(&row, v, vl) != 0) continue;
        if (!eval_where(where_node, &row, s->table)) { row_free(&row); continue; }

        /* Build composite bucket key: text projections of every group
         * col separated by \x01. \x01 can't appear in any well-formed
         * text/integer/real text projection so no ambiguity. */
        char key[1024]; key[0] = '\0';
        u64 off = 0;
        int bad = 0;
        for (u32 g = 0; g < s->group_col_count; ++g) {
            char part[256];
            if (col_val_to_text(part, sizeof(part), &row,
                                  s->group_col_indices[g]) != 0) {
                bad = 1; break;
            }
            u64 pl = strlen(part);
            if (off + pl + 2 >= sizeof(key)) { bad = 1; break; }
            if (g > 0) key[off++] = '\x01';
            memcpy(key + off, part, pl); off += pl;
            key[off] = '\0';
        }
        if (bad) { row_free(&row); continue; }

        /* Linear probe. For cookbook-scale group counts (< 1000) this
         * is fine; swap for a hash later if profiles show otherwise. */
        u64 bi = 0;
        for (; bi < n_buckets; ++bi) {
            if (strcmp(buckets[bi].key, key) == 0) break;
        }
        if (bi == n_buckets) {
            if (n_buckets == cap) {
                u64 nc = cap * 2;
                db_group_bucket *nb = (db_group_bucket *)realloc(buckets,
                                                                   nc * sizeof(*buckets));
                if (!nb) {
                    row_free(&row);
                    for (u64 i = 0; i < n_buckets; ++i) {
                        for (u32 gg = 0; gg < DB_MAX_GROUP_COLS; gg++) {
                            free(buckets[i].gv_bvals[gg]);
                        }
                    }
                    free(buckets);
                    db_storage_iter_destroy(it);
                    return 4;
                }
                memset(nb + cap, 0, (nc - cap) * sizeof(*buckets));
                buckets = nb;
                cap = nc;
            }
            /* Init a new bucket: copy stmt->agg_specs template and
             * clear accumulators. */
            memset(&buckets[n_buckets], 0, sizeof(db_group_bucket));
            for (u32 i = 0; i < s->agg_count; ++i) {
                buckets[n_buckets].specs[i] = s->agg_specs[i];
                buckets[n_buckets].specs[i].count_nonnull = 0;
                buckets[n_buckets].specs[i].isum = 0;
                buckets[n_buckets].specs[i].dsum = 0;
                buckets[n_buckets].specs[i].saw_real = 0;
                buckets[n_buckets].specs[i].have_any = 0;
            }
            snprintf(buckets[n_buckets].key,
                      sizeof(buckets[n_buckets].key), "%s", key);
            for (u32 g = 0; g < s->group_col_count; ++g) {
                set_group_value_from_row_at(&buckets[n_buckets], g,
                                              &row, s->group_col_indices[g]);
            }
            n_buckets++;
        }

        /* Update each aggregate. */
        for (u32 i = 0; i < s->agg_count; ++i) {
            agg_update(&buckets[bi].specs[i], &row);
        }
        row_free(&row);
    }
    db_storage_iter_destroy(it);

    /* Materialise one db_row per bucket. Cols 0..group_col_count-1 are
     * the group values; the next agg_count cols are the aggregates. */
    db_row *rows = (db_row *)calloc(n_buckets ? n_buckets : 1, sizeof(db_row));
    if (!rows) {
        for (u64 i = 0; i < n_buckets; ++i) {
            for (u32 gg = 0; gg < DB_MAX_GROUP_COLS; gg++) {
                free(buckets[i].gv_bvals[gg]);
            }
        }
        free(buckets);
        return 5;
    }
    u64 out_n = 0;
    for (u64 i = 0; i < n_buckets; ++i) {
        db_row r;
        memset(&r, 0, sizeof(r));
        r.col_count = s->group_col_count + s->agg_count;
        /* Cols 0..group_col_count-1: the group values, typed per source. */
        for (u32 g = 0; g < s->group_col_count; ++g) {
            r.types[g] = buckets[i].gv_types[g];
            r.ivals[g] = buckets[i].gv_ivals[g];
            r.fvals[g] = buckets[i].gv_fvals[g];
            r.blens[g] = buckets[i].gv_blens[g];
            if (buckets[i].gv_bvals[g] && buckets[i].gv_blens[g] > 0) {
                r.bvals[g] = (u8 *)malloc(buckets[i].gv_blens[g] + 1);
                if (r.bvals[g]) {
                    memcpy(r.bvals[g], buckets[i].gv_bvals[g],
                            buckets[i].gv_blens[g]);
                    r.bvals[g][buckets[i].gv_blens[g]] = 0;
                }
            }
        }
        /* Agg results at offset group_col_count + j. */
        for (u32 j = 0; j < s->agg_count; ++j) {
            agg_resolve_into_row(&buckets[i].specs[j], &r,
                                  s->group_col_count + j);
        }
        /* HAVING is evaluated here, on the produced group row.
         * Naive: re-use eval_where by passing a synthetic table
         * whose col layout matches the group row. Simplest: defer
         * HAVING to a post-filter that only handles aggregate
         * comparisons by column index. For v1, HAVING is parsed
         * but applied as plain WHERE against the group row using
         * s->table — works when HAVING references the group column
         * directly. Aggregate HAVING (`COUNT(*) > 5`) would need
         * the multival path plus an aggregate-aware compare; left
         * for a later step. */
        if (s->having_node) {
            /* HAVING is evaluated against the synthesized group-row
             * table so both group-col references and rewritten
             * aggregate references (AGG_REF → COLUMN_REF "__agg_...__")
             * resolve cleanly through eval_where's normal path. */
            const db_table *ht = s->synth_group_table_built
                                   ? &s->synth_group_table
                                   : s->table;
            if (!eval_where(s->having_node, &r, ht)) {
                row_free(&r);
                continue;
            }
        }
        rows[out_n++] = r;
    }
    for (u64 i = 0; i < n_buckets; ++i) {
        for (u32 gg = 0; gg < DB_MAX_GROUP_COLS; gg++) {
            free(buckets[i].gv_bvals[gg]);
        }
    }
    free(buckets);

    s->sorted_rows = rows;
    s->sorted_count = out_n;
    s->sorted_pos = 0;
    return 0;
}

/* Walk every matching row, feed each to every agg_spec, build the
 * synthetic one-row result into *out. Returns 0 on success. */
static unsigned long evaluate_aggregates(db_stmt *s,
                                           const sql_ast *where_node,
                                           db_row *out) {
    /* Reset specs — handles db_reset + re-step. */
    for (u32 i = 0; i < s->agg_count; ++i) {
        db_agg_spec *sp = &s->agg_specs[i];
        sp->count_nonnull = 0;
        sp->isum = 0;
        sp->dsum = 0.0;
        sp->saw_real = 0;
        sp->imin = 0; sp->imax = 0;
        sp->fmin = 0; sp->fmax = 0;
        sp->have_any = 0;
    }

    u8 prefix[DB_MAX_KEY_LEN];
    u64 plen = build_prefix_key(prefix, sizeof(prefix), s->table->name);
    if (plen == 0) return 1;
    db_storage_iter *it = NULL;
    if (s->db->vt->iter_create(&it, s->db->storage, prefix, plen) != 0) return 2;

    const u8 *k; u64 kl;
    const u8 *v; u64 vl;
    while (db_storage_iter_next(&k, &kl, &v, &vl, it) == 0) {
        db_row row;
        if (row_deserialize(&row, v, vl) != 0) continue;
        if (!eval_where(where_node, &row, s->table)) { row_free(&row); continue; }
        for (u32 i = 0; i < s->agg_count; ++i) agg_update(&s->agg_specs[i], &row);
        row_free(&row);
    }
    db_storage_iter_destroy(it);

    memset(out, 0, sizeof(*out));
    out->col_count = s->agg_count;
    for (u32 i = 0; i < s->agg_count; ++i) {
        agg_resolve_into_row(&s->agg_specs[i], out, i);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  ORDER BY materialisation + comparator                              */
/*                                                                     */
/*  For SELECTs with an ORDER BY clause, we materialise the full       */
/*  matching row set (pass 1), sort by the per-clause specs (pass 2),  */
/*  then apply OFFSET + LIMIT by trimming the sorted array (pass 3).   */
/*  db_step then serves rows from sorted_rows[sorted_pos++].           */
/*                                                                     */
/*  qsort doesn't pass a context; we use a file-scope pointer to the   */
/*  stmt during the sort call, safe because db_step runs under the     */
/*  conn mutex (one sort at a time per connection).                    */
/* ------------------------------------------------------------------ */

static const db_stmt *g_sort_stmt;

static int cmp_rows_by_order(const void *a, const void *b) {
    const db_row *ra = (const db_row *)a;
    const db_row *rb = (const db_row *)b;
    for (u32 i = 0; i < g_sort_stmt->order_count; ++i) {
        u32 ci = g_sort_stmt->order_specs[i].col_idx;
        int desc = g_sort_stmt->order_specs[i].descending;
        if (ci >= ra->col_count || ci >= rb->col_count) continue;
        int ta = ra->types[ci], tb = rb->types[ci];
        int r = 0;
        /* NULLs sort first in ASC order (SQLite default). */
        if (ta == DB_TYPE_NULL && tb != DB_TYPE_NULL) r = -1;
        else if (ta != DB_TYPE_NULL && tb == DB_TYPE_NULL) r = 1;
        else if (ta == DB_TYPE_NULL && tb == DB_TYPE_NULL) r = 0;
        else if (ta == DB_TYPE_INTEGER && tb == DB_TYPE_INTEGER) {
            r = (ra->ivals[ci] < rb->ivals[ci]) ? -1 :
                (ra->ivals[ci] > rb->ivals[ci]) ?  1 : 0;
        } else if (ta == DB_TYPE_REAL && tb == DB_TYPE_REAL) {
            r = (ra->fvals[ci] < rb->fvals[ci]) ? -1 :
                (ra->fvals[ci] > rb->fvals[ci]) ?  1 : 0;
        } else if ((ta == DB_TYPE_TEXT || ta == DB_TYPE_BLOB)
                && (tb == DB_TYPE_TEXT || tb == DB_TYPE_BLOB)) {
            u64 la = ra->blens[ci], lb = rb->blens[ci];
            u64 m = la < lb ? la : lb;
            r = (m > 0) ? memcmp(ra->bvals[ci], rb->bvals[ci], m) : 0;
            if (r == 0) r = (la < lb) ? -1 : (la > lb) ? 1 : 0;
        }
        if (r != 0) return desc ? -r : r;
    }
    return 0;
}

/* Drain every matching row into s->sorted_rows, sort it, and apply
 * OFFSET+LIMIT by trimming. Ownership of db_row buffers transfers
 * to sorted_rows — the array must be freed via db_stmt finalisation
 * (any unconsumed slots get row_free'd; consumed slots were zeroed
 * during db_step so they don't double-free).  Returns 0 on success. */
static unsigned long materialise_ordered_result(db_stmt *s,
                                                  const sql_ast *where_node) {
    db_storage_iter *it = NULL;
    u8 prefix[DB_MAX_KEY_LEN];
    u64 plen = build_prefix_key(prefix, sizeof(prefix), s->table->name);
    if (plen == 0) return 1;
    if (s->db->vt->iter_create(&it, s->db->storage, prefix, plen) != 0) return 2;

    u64 cap = 16;
    db_row *rows = (db_row *)calloc(cap, sizeof(db_row));
    if (!rows) { db_storage_iter_destroy(it); return 3; }
    u64 n = 0;

    const u8 *k; u64 kl;
    const u8 *v; u64 vl;
    while (db_storage_iter_next(&k, &kl, &v, &vl, it) == 0) {
        db_row row;
        if (row_deserialize(&row, v, vl) != 0) continue;
        if (!eval_where(where_node, &row, s->table)) { row_free(&row); continue; }
        if (n == cap) {
            u64 nc = cap * 2;
            db_row *nr = (db_row *)realloc(rows, nc * sizeof(db_row));
            if (!nr) {
                row_free(&row);
                for (u64 i = 0; i < n; ++i) row_free(&rows[i]);
                free(rows);
                db_storage_iter_destroy(it);
                return 4;
            }
            rows = nr;
            cap = nc;
        }
        rows[n++] = row;
    }
    db_storage_iter_destroy(it);

    /* Sort. */
    if (n > 1) {
        g_sort_stmt = s;
        qsort(rows, n, sizeof(db_row), cmp_rows_by_order);
        g_sort_stmt = NULL;
    }

    /* Apply OFFSET by dropping the first offset_n rows. */
    u64 start = s->offset_n < n ? s->offset_n : n;
    for (u64 i = 0; i < start; ++i) row_free(&rows[i]);
    if (start > 0) {
        memmove(rows, rows + start, (n - start) * sizeof(db_row));
        n -= start;
    }

    /* Apply LIMIT by truncating beyond limit_n. */
    if (s->has_limit && n > s->limit_n) {
        for (u64 i = s->limit_n; i < n; ++i) row_free(&rows[i]);
        n = s->limit_n;
    }

    s->sorted_rows = rows;
    s->sorted_count = n;
    s->sorted_pos = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Placeholder collection + substitution                              */
/*                                                                     */
/*  We walk the parsed AST once at prepare time and record every       */
/*  SQL_AST_LITERAL node whose text begins with '?'. Each db_step      */
/*  rewrites those nodes' text from the current bind values. If a      */
/*  placeholder has no matching bind, it resolves to NULL (SQLite      */
/*  semantics). Bound text with embedded nulls is not representable    */
/*  via string substitution; use the typed-bind path (not yet wired).  */
/* ------------------------------------------------------------------ */

/* Parse the numeric suffix of a placeholder token ("?" or "?N"). If no
 * suffix, auto-number from the caller-supplied counter. Returns 0-based
 * bind slot index, or -1 on out-of-range. */
static int parse_placeholder_index(const char *text, u32 *auto_counter) {
    if (!text || text[0] != '?') return -1;
    if (text[1] == '\0') {
        u32 idx = (*auto_counter)++;
        if (idx >= DB_BIND_MAX) return -1;
        return (int)idx;
    }
    long v = strtol(text + 1, NULL, 10);
    if (v <= 0 || v > (long)DB_BIND_MAX) return -1;
    return (int)(v - 1);
}

/* Recursive walker — collects literal nodes whose text is a placeholder. */
static void collect_placeholders(sql_ast *node, db_stmt *s, u32 *auto_counter) {
    if (!node) return;
    if (node->type == SQL_AST_LITERAL && node->text && node->text[0] == '?') {
        int idx = parse_placeholder_index(node->text, auto_counter);
        if (idx >= 0 && s->placeholder_count < DB_BIND_MAX) {
            s->placeholders[s->placeholder_count].node  = node;
            s->placeholders[s->placeholder_count].index = (u32)idx;
            s->placeholder_count++;
        }
    }
    if (node->left)  collect_placeholders(node->left,  s, auto_counter);
    if (node->right) collect_placeholders(node->right, s, auto_counter);
    for (u64 i = 0; i < node->child_count; ++i) {
        collect_placeholders(&node->children[i], s, auto_counter);
    }
}

/* Rewrite one placeholder node's text from its bind value. */
static unsigned long substitute_placeholder(sql_ast *node, const db_bind_val *bv) {
    char *new_text = NULL;
    if (!bv || bv->type == DB_TYPE_NULL) {
        new_text = malloc(5);
        if (!new_text) return 1;
        memcpy(new_text, "NULL", 5);
    } else if (bv->type == DB_TYPE_INTEGER) {
        new_text = malloc(32);
        if (!new_text) return 1;
        snprintf(new_text, 32, "%lld", (long long)bv->ival);
    } else if (bv->type == DB_TYPE_REAL) {
        new_text = malloc(48);
        if (!new_text) return 1;
        snprintf(new_text, 48, "%.17g", bv->fval);
    } else if (bv->type == DB_TYPE_TEXT) {
        u64 len = bv->blen;
        new_text = malloc(len + 1);
        if (!new_text) return 1;
        if (len && bv->bval) memcpy(new_text, bv->bval, len);
        new_text[len] = '\0';
    } else if (bv->type == DB_TYPE_BLOB) {
        /* string substitution is lossy for blobs with embedded nulls;
         * for now, treat as text and rely on caller not to bind binary
         * into text columns. Safer typed path to come in step 2. */
        u64 len = bv->blen;
        new_text = malloc(len + 1);
        if (!new_text) return 1;
        if (len && bv->bval) memcpy(new_text, bv->bval, len);
        new_text[len] = '\0';
    } else {
        new_text = malloc(5);
        if (!new_text) return 1;
        memcpy(new_text, "NULL", 5);
    }
    free(node->text);
    node->text = new_text;
    return 0;
}

/* Resolve every placeholder from the current binds. Called at db_step. */
static unsigned long apply_placeholders(db_stmt *s) {
    for (u32 i = 0; i < s->placeholder_count; ++i) {
        u32 bi = s->placeholders[i].index;
        const db_bind_val *bv = (bi < s->bind_count) ? &s->binds[bi] : NULL;
        unsigned long rc = substitute_placeholder(s->placeholders[i].node, bv);
        if (rc) return rc;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  db_exec                                                            */
/* ------------------------------------------------------------------ */

APENNINES_API unsigned long db_exec(db_conn *db, const char *sql) {
    if (!db)  return 1;
    if (!sql) return 2;
    rwlock_write_lock(&db->conn_lock);
    unsigned long rc = parse_and_exec(db, sql);
    rwlock_write_unlock(&db->conn_lock);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  db_prepare                                                         */
/* ------------------------------------------------------------------ */

static unsigned long db_prepare_locked(db_stmt **out, db_conn *db, const char *sql) {
    db_stmt *s = calloc(1, sizeof(db_stmt));
    if (!s) return 4;
    s->db = db;

    unsigned long rc = sql_tokenize(&s->tokens, sql);
    if (rc) { free(s); return 5; }

    rc = sql_parse(&s->ast, &s->tokens);
    if (rc || !s->ast) {
        sql_token_list_free(&s->tokens);
        free(s);
        return 6;
    }

    /* For SELECT, pre-parse columns and table */
    if (s->ast->type == SQL_AST_SELECT) {
        /* calloc zeroed group_col_count already; no other init needed. */
        const char *tname = extract_table_name(s->ast->text);
        if (tname) s->table = find_table(db, tname);

        /* Extract column list */
        for (u64 i = 0; i < s->ast->child_count; ++i) {
            const sql_ast *child = &s->ast->children[i];
            if (child->type == SQL_AST_COLUMN_REF) {
                if (child->text && strncmp(child->text, "__AGG__ ", 8) == 0
                    && s->agg_count < DB_MAX_COLUMNS) {
                    /* "__AGG__ FUNC arg" */
                    char fn[16], arg[64];
                    fn[0] = arg[0] = '\0';
                    sscanf(child->text + 8, "%15s %63s", fn, arg);
                    db_agg_spec *spec = &s->agg_specs[s->agg_count];
                    memset(spec, 0, sizeof(*spec));
                    if      (strcasecmp_a(fn, "COUNT") == 0) spec->fn = DB_AGG_COUNT;
                    else if (strcasecmp_a(fn, "SUM")   == 0) spec->fn = DB_AGG_SUM;
                    else if (strcasecmp_a(fn, "MIN")   == 0) spec->fn = DB_AGG_MIN;
                    else if (strcasecmp_a(fn, "MAX")   == 0) spec->fn = DB_AGG_MAX;
                    else if (strcasecmp_a(fn, "AVG")   == 0) spec->fn = DB_AGG_AVG;
                    else continue;
                    if (strcmp(arg, "*") == 0) {
                        spec->star = 1;
                        spec->col_idx = -1;
                    } else if (s->table) {
                        int ci = col_index(s->table, arg);
                        if (ci < 0) continue;
                        spec->star = 0;
                        spec->col_idx = ci;
                    }
                    s->agg_count++;
                } else if (child->text && strcmp(child->text, "*") == 0) {
                    s->sel_star = 1;
                } else if (child->text && s->sel_col_count < DB_MAX_COLUMNS) {
                    s->sel_cols[s->sel_col_count] = child->text;
                    s->sel_col_count++;
                }
            } else if (child->type == SQL_AST_GROUP_BY && child->text && s->table
                       && s->group_col_count < DB_MAX_GROUP_COLS) {
                int ci = col_index(s->table, child->text);
                if (ci >= 0) {
                    s->group_col_indices[s->group_col_count++] = (u32)ci;
                }
            } else if (child->type == SQL_AST_HAVING && child->left) {
                s->having_node = child->left;
            } else if (child->type == SQL_AST_LIMIT && child->text) {
                s->has_limit = 1;
                s->limit_n = (u64)strtoull(child->text, NULL, 10);
            } else if (child->type == SQL_AST_OFFSET && child->text) {
                s->offset_n = (u64)strtoull(child->text, NULL, 10);
            } else if (child->type == SQL_AST_ORDER_BY && child->text
                       && s->order_count < DB_MAX_COLUMNS && s->table) {
                /* Parse "col[ ASC|DESC]" into (col_idx, descending). */
                char colname[128]; colname[0] = '\0';
                const char *src = child->text;
                u64 sp = 0;
                while (src[sp] && src[sp] != ' ') sp++;
                u64 nl = sp < sizeof(colname) ? sp : sizeof(colname) - 1;
                memcpy(colname, src, nl); colname[nl] = '\0';
                int ci = col_index(s->table, colname);
                if (ci >= 0) {
                    db_order_spec *os = &s->order_specs[s->order_count++];
                    os->col_idx = (u32)ci;
                    os->descending = (strstr(src + sp, "DESC") != NULL) ||
                                     (strstr(src + sp, "desc") != NULL);
                }
            }
        }
    }

    /* If this SELECT aggregates and/or groups, build the synthetic
     * group-row table and rewrite the HAVING AST's AGG_REF nodes
     * into COLUMN_REFs that eval_where can resolve against it. */
    if (s->ast->type == SQL_AST_SELECT && s->agg_count > 0) {
        synth_group_table_build(s);
        if (s->having_node) {
            /* HAVING's condition subtree was attached with .left set. */
            rewrite_having_agg_refs((sql_ast *)s->having_node, s);
        }
    }

    /* Record every "?" / "?N" literal for per-step bind substitution. */
    u32 auto_counter = 0;
    collect_placeholders(s->ast, s, &auto_counter);

    *out = s;
    return 0;
}

APENNINES_API unsigned long db_prepare(db_stmt **out, db_conn *db, const char *sql) {
    if (!out) return 1;
    if (!db)  return 2;
    if (!sql) return 3;
    /* Prepare only reads table registry — read lock is fine, and lets
     * many threads prepare concurrently. */
    rwlock_read_lock(&db->conn_lock);
    unsigned long rc = db_prepare_locked(out, db, sql);
    rwlock_read_unlock(&db->conn_lock);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  db_bind_*                                                          */
/* ------------------------------------------------------------------ */

APENNINES_API unsigned long db_bind_i64(db_stmt *s, u32 idx, i64 val) {
    if (!s) return 1;
    if (idx >= DB_BIND_MAX) return 2;
    s->binds[idx].type = DB_TYPE_INTEGER;
    s->binds[idx].ival = val;
    if (idx >= s->bind_count) s->bind_count = idx + 1;
    return 0;
}

APENNINES_API unsigned long db_bind_f64(db_stmt *s, u32 idx, double val) {
    if (!s) return 1;
    if (idx >= DB_BIND_MAX) return 2;
    s->binds[idx].type = DB_TYPE_REAL;
    s->binds[idx].fval = val;
    if (idx >= s->bind_count) s->bind_count = idx + 1;
    return 0;
}

APENNINES_API unsigned long db_bind_str(db_stmt *s, u32 idx, const char *val, u64 len) {
    if (!s) return 1;
    if (idx >= DB_BIND_MAX) return 2;
    if (s->binds[idx].bval) { free(s->binds[idx].bval); s->binds[idx].bval = NULL; }
    s->binds[idx].type = DB_TYPE_TEXT;
    s->binds[idx].bval = malloc(len + 1);
    if (!s->binds[idx].bval) return 3;
    memcpy(s->binds[idx].bval, val, len);
    s->binds[idx].bval[len] = '\0';
    s->binds[idx].blen = len;
    if (idx >= s->bind_count) s->bind_count = idx + 1;
    return 0;
}

APENNINES_API unsigned long db_bind_bytes(db_stmt *s, u32 idx, const u8 *val, u64 len) {
    if (!s) return 1;
    if (idx >= DB_BIND_MAX) return 2;
    if (s->binds[idx].bval) { free(s->binds[idx].bval); s->binds[idx].bval = NULL; }
    s->binds[idx].type = DB_TYPE_BLOB;
    s->binds[idx].bval = malloc(len);
    if (!s->binds[idx].bval && len > 0) return 3;
    if (len > 0) memcpy(s->binds[idx].bval, val, len);
    s->binds[idx].blen = len;
    if (idx >= s->bind_count) s->bind_count = idx + 1;
    return 0;
}

APENNINES_API unsigned long db_bind_null(db_stmt *s, u32 idx) {
    if (!s) return 1;
    if (idx >= DB_BIND_MAX) return 2;
    if (s->binds[idx].bval) { free(s->binds[idx].bval); s->binds[idx].bval = NULL; }
    s->binds[idx].type = DB_TYPE_NULL;
    if (idx >= s->bind_count) s->bind_count = idx + 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  db_step                                                            */
/* ------------------------------------------------------------------ */

static unsigned long db_step_locked(db_stmt *s) {
    /* Rewrite placeholder AST text from current binds before execution.
     * Safe for all types cookbook uses (TEXT / INT / NULL); BLOB binds
     * are accepted but only round-trip cleanly when the data contains
     * no embedded nulls. */
    unsigned long prc = apply_placeholders(s);
    if (prc) return prc + 40;  /* shift so it doesn't collide with exec hatches */

    /* For non-SELECT statements, execute and return DONE */
    if (s->ast->type != SQL_AST_SELECT) {
        unsigned long rc = 0;
        switch (s->ast->type) {
        case SQL_AST_INSERT:
        case SQL_AST_INSERT_OR_REPLACE:
                                   rc = exec_insert(s->db, s->ast);       break;
        case SQL_AST_UPDATE:       rc = exec_update(s->db, s->ast);       break;
        case SQL_AST_DELETE:       rc = exec_delete(s->db, s->ast);       break;
        case SQL_AST_CREATE_TABLE: rc = exec_create_table(s->db, s->ast); break;
        case SQL_AST_CREATE_INDEX: rc = exec_create_index(s->db, s->ast); break;
        case SQL_AST_DROP_TABLE:   rc = exec_drop_table(s->db, s->ast);   break;
        case SQL_AST_DROP_INDEX:   rc = exec_drop_index(s->db, s->ast);   break;
        default: rc = 2; break;
        }
        if (rc) return rc;
        s->done = 1;
        return DB_STEP_DONE;
    }

    /* SELECT stepping */
    if (s->done) return DB_STEP_DONE;

    if (!s->table) {
        /* Resolve table on first step */
        const char *tname = extract_table_name(s->ast->text);
        if (!tname) return 3;
        s->table = find_table(s->db, tname);
        if (!s->table) return 4;
    }

    /* Free previous row data */
    if (s->have_row) {
        row_free(&s->cur_row);
        s->have_row = 0;
    }

    /* Find the WHERE condition */
    const sql_ast *where_node = find_where(s->ast);

    /* GROUP BY + aggregates: materialise one row per bucket and serve
     * via sorted_rows (shares the finalisation path). */
    if (s->group_col_count > 0 && s->agg_count > 0) {
        if (!s->materialised) {
            if (materialise_grouped_result(s, where_node) != 0) return 27;
            s->materialised = 1;
        }
        if (s->sorted_pos >= s->sorted_count) {
            s->done = 1;
            return DB_STEP_DONE;
        }
        s->cur_row = s->sorted_rows[s->sorted_pos];
        memset(&s->sorted_rows[s->sorted_pos], 0, sizeof(db_row));
        s->sorted_pos++;
        s->have_row = 1;
        return 0;
    }

    /* Aggregate SELECT: produce exactly one synthetic row (once). */
    if (s->agg_count > 0) {
        if (s->agg_produced) {
            s->done = 1;
            return DB_STEP_DONE;
        }
        db_row synth;
        unsigned long rc = evaluate_aggregates(s, where_node, &synth);
        if (rc) return rc + 20;
        s->cur_row = synth;
        s->have_row = 1;
        s->agg_produced = 1;
        return 0;
    }

    /* ORDER BY path: materialise on first step, then walk sorted_rows. */
    if (s->order_count > 0) {
        if (!s->materialised) {
            if (materialise_ordered_result(s, where_node) != 0) return 7;
            s->materialised = 1;
        }
        if (s->sorted_pos >= s->sorted_count) {
            s->done = 1;
            return DB_STEP_DONE;
        }
        s->cur_row = s->sorted_rows[s->sorted_pos];
        /* Transfer ownership so row_free on the next step (or at finalize)
         * doesn't double-free: clear the slot we just took. */
        memset(&s->sorted_rows[s->sorted_pos], 0, sizeof(db_row));
        s->sorted_pos++;
        s->have_row = 1;
        return 0;
    }

    /* Create iterator on first call (streaming path). Check planner:
     * if WHERE hits an indexed column (equality or range), iterate
     * the index rather than the full table. */
    if (!s->iter) {
        const db_index *ix = NULL;
        int kind = IDX_USE_NONE;
        int col_type = 0;
        char eq_val[256] = {0};
        int has_lower = 0, lower_inclusive = 0;
        int has_upper = 0, upper_inclusive = 0;
        char lower[256] = {0}, upper[256] = {0};
        if (planner_build(where_node, s->table, &ix, &kind, &col_type,
                           eq_val, sizeof(eq_val),
                           &has_lower, &lower_inclusive, lower, sizeof(lower),
                           &has_upper, &upper_inclusive, upper, sizeof(upper))) {
            u8 prefix[DB_MAX_KEY_LEN];
            u64 plen = 0;
            if (kind == IDX_USE_EQ) {
                const char *vs[1] = { eq_val };
                plen = build_idx_val_prefix_composite(prefix, sizeof(prefix),
                                                       s->table->name,
                                                       ix->name, vs, 1);
            } else {
                /* RANGE: iterate the whole index and filter per-entry. */
                plen = build_idx_all_prefix(prefix, sizeof(prefix),
                                             s->table->name, ix->name);
            }
            if (plen > 0 && s->db->vt->iter_create(&s->iter, s->db->storage,
                                             prefix, plen) == 0) {
                s->using_index = 1;
                s->idx_plan.kind = kind;
                s->idx_plan.col_type = col_type;
                s->idx_plan.has_lower = has_lower;
                s->idx_plan.lower_inclusive = lower_inclusive;
                snprintf(s->idx_plan.lower, sizeof(s->idx_plan.lower), "%s", lower);
                s->idx_plan.has_upper = has_upper;
                s->idx_plan.upper_inclusive = upper_inclusive;
                snprintf(s->idx_plan.upper, sizeof(s->idx_plan.upper), "%s", upper);
                s->idx_prefix_len = plen;
            }
        }
        if (!s->iter) {
            u8 prefix[DB_MAX_KEY_LEN];
            u64 plen = build_prefix_key(prefix, sizeof(prefix), s->table->name);
            if (plen == 0) return 5;
            unsigned long rc = s->db->vt->iter_create(&s->iter, s->db->storage, prefix, plen);
            if (rc) return 6;
        }
    }

    /* Respect LIMIT early. */
    if (s->has_limit && s->rows_returned >= s->limit_n) {
        s->done = 1;
        return DB_STEP_DONE;
    }

    /* Iterate until we find a matching row. When using_index, each
     * kv_iter_next yields an index entry; we parse the rowid and
     * fetch the actual row. Still run eval_where because the WHERE
     * might have further AND conditions. */
    const u8 *k; u64 kl;
    const u8 *v; u64 vl;
    while (db_storage_iter_next(&k, &kl, &v, &vl, s->iter) == 0) {
        db_row row;
        if (s->using_index) {
            /* RANGE plan: filter the index entry's first-col value
             * against the bounds before paying for a row fetch. */
            if (s->idx_plan.kind == IDX_USE_RANGE) {
                char ev[256];
                if (extract_first_val_from_idx_key(k, kl, s->idx_prefix_len,
                                                     ev, sizeof(ev)) != 0) continue;
                if (s->idx_plan.has_lower) {
                    int c = idx_val_compare(s->idx_plan.col_type, ev,
                                              s->idx_plan.lower);
                    if (s->idx_plan.lower_inclusive ? (c < 0) : (c <= 0)) continue;
                }
                if (s->idx_plan.has_upper) {
                    int c = idx_val_compare(s->idx_plan.col_type, ev,
                                              s->idx_plan.upper);
                    if (s->idx_plan.upper_inclusive ? (c > 0) : (c >= 0)) continue;
                }
            }
            i64 rowid = parse_rowid_from_idx_key(k, kl);
            if (rowid < 0) continue;
            u8 row_key[DB_MAX_KEY_LEN];
            u64 rkl = build_row_key(row_key, sizeof(row_key),
                                     s->table->name, rowid);
            if (rkl == 0) continue;
            u8 *rv = NULL; u64 rvl = 0;
            if (s->db->vt->get(&rv, &rvl, s->db->storage, row_key, rkl) != 0 || !rv) continue;
            int drc = row_deserialize(&row, rv, rvl);
            free(rv);
            if (drc != 0) continue;
        } else {
            if (row_deserialize(&row, v, vl) != 0) continue;
        }

        if (!eval_where(where_node, &row, s->table)) {
            row_free(&row);
            continue;
        }

        /* OFFSET pre-skip. */
        if (s->rows_skipped < s->offset_n) {
            s->rows_skipped++;
            row_free(&row);
            continue;
        }

        /* Row matches — store it */
        s->cur_row = row;
        s->have_row = 1;
        s->rows_returned++;
        return 0; /* row available */
    }

    /* No more rows */
    s->done = 1;
    return DB_STEP_DONE;
}

APENNINES_API unsigned long db_step(db_stmt *s) {
    if (!s) return 1;
    /* SELECT (including aggregate and ordered-materialise paths) is
     * read-only against conn/kv state — take the read side so multiple
     * SELECTs can overlap. Everything else mutates and takes write. */
    int is_select = (s->ast && s->ast->type == SQL_AST_SELECT);
    if (is_select) rwlock_read_lock(&s->db->conn_lock);
    else           rwlock_write_lock(&s->db->conn_lock);
    unsigned long rc = db_step_locked(s);
    if (is_select) rwlock_read_unlock(&s->db->conn_lock);
    else           rwlock_write_unlock(&s->db->conn_lock);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Column accessors                                                   */
/* ------------------------------------------------------------------ */

/* Map a SELECT column index to the actual row column index */
static int stmt_col_to_row_col(db_stmt *s, u32 idx) {
    /* GROUP BY result: cols 0..group_col_count-1 = group values,
     * next agg_count cols = aggs. Identity mapping. */
    if (s->group_col_count > 0 && s->agg_count > 0) {
        if (idx >= s->group_col_count + s->agg_count) return -1;
        return (int)idx;
    }
    /* Aggregate result: the synthetic row lays columns out in the
     * same order as agg_specs. Identity mapping. */
    if (s->agg_count > 0) {
        if (idx >= s->agg_count) return -1;
        return (int)idx;
    }
    if (s->sel_star) {
        return (int)idx;
    }
    if (idx >= s->sel_col_count) return -1;
    if (!s->table) return -1;
    return col_index(s->table, s->sel_cols[idx]);
}

APENNINES_API unsigned long db_column_count(u32 *out, db_stmt *s) {
    if (!out) return 1;
    if (!s)   return 2;
    if (s->group_col_count > 0 && s->agg_count > 0) {
        *out = s->group_col_count + s->agg_count;
    } else if (s->agg_count > 0) {
        *out = s->agg_count;
    } else if (s->sel_star && s->table) {
        *out = s->table->col_count;
    } else {
        *out = s->sel_col_count;
    }
    return 0;
}

APENNINES_API unsigned long db_column_name(const char **out, db_stmt *s, u32 idx) {
    if (!out) return 1;
    if (!s)   return 2;
    /* For aggregate SELECTs there's no backing table column; return
     * a canonical name like "COUNT(*)". */
    if (s->agg_count > 0) {
        if (idx >= s->agg_count) return 4;
        static const char *fn_names[] = {"COUNT","SUM","MIN","MAX","AVG"};
        static char bufs[DB_MAX_COLUMNS][96];
        const db_agg_spec *sp = &s->agg_specs[idx];
        const char *arg = sp->star ? "*" :
            (s->table && sp->col_idx >= 0
             ? s->table->cols[sp->col_idx].name : "?");
        snprintf(bufs[idx], sizeof(bufs[idx]), "%s(%s)", fn_names[sp->fn], arg);
        *out = bufs[idx];
        return 0;
    }
    if (!s->table) return 3;
    int ci = stmt_col_to_row_col(s, idx);
    if (ci < 0 || (u32)ci >= s->table->col_count) return 4;
    *out = s->table->cols[ci].name;
    return 0;
}

APENNINES_API unsigned long db_column_type(int *out, db_stmt *s, u32 idx) {
    if (!out) return 1;
    if (!s)   return 2;
    if (!s->have_row) return 3;
    int ci = stmt_col_to_row_col(s, idx);
    if (ci < 0 || (u32)ci >= s->cur_row.col_count) return 4;
    *out = s->cur_row.types[ci];
    return 0;
}

APENNINES_API unsigned long db_column_i64(i64 *out, db_stmt *s, u32 idx) {
    if (!out) return 1;
    if (!s)   return 2;
    if (!s->have_row) return 3;
    int ci = stmt_col_to_row_col(s, idx);
    if (ci < 0 || (u32)ci >= s->cur_row.col_count) return 4;
    if (s->cur_row.types[ci] == DB_TYPE_INTEGER) {
        *out = s->cur_row.ivals[ci];
    } else if (s->cur_row.types[ci] == DB_TYPE_REAL) {
        *out = (i64)s->cur_row.fvals[ci];
    } else if (s->cur_row.types[ci] == DB_TYPE_TEXT && s->cur_row.bvals[ci]) {
        *out = strtoll((const char *)s->cur_row.bvals[ci], NULL, 10);
    } else {
        *out = 0;
    }
    return 0;
}

APENNINES_API unsigned long db_column_f64(double *out, db_stmt *s, u32 idx) {
    if (!out) return 1;
    if (!s)   return 2;
    if (!s->have_row) return 3;
    int ci = stmt_col_to_row_col(s, idx);
    if (ci < 0 || (u32)ci >= s->cur_row.col_count) return 4;
    if (s->cur_row.types[ci] == DB_TYPE_REAL) {
        *out = s->cur_row.fvals[ci];
    } else if (s->cur_row.types[ci] == DB_TYPE_INTEGER) {
        *out = (double)s->cur_row.ivals[ci];
    } else if (s->cur_row.types[ci] == DB_TYPE_TEXT && s->cur_row.bvals[ci]) {
        *out = strtod((const char *)s->cur_row.bvals[ci], NULL);
    } else {
        *out = 0.0;
    }
    return 0;
}

APENNINES_API unsigned long db_column_str(const char **out, u64 *out_len,
                                           db_stmt *s, u32 idx) {
    if (!out)     return 1;
    if (!out_len) return 2;
    if (!s)       return 3;
    if (!s->have_row) return 4;
    int ci = stmt_col_to_row_col(s, idx);
    if (ci < 0 || (u32)ci >= s->cur_row.col_count) return 5;
    if (s->cur_row.types[ci] == DB_TYPE_TEXT || s->cur_row.types[ci] == DB_TYPE_BLOB) {
        *out = (const char *)s->cur_row.bvals[ci];
        *out_len = s->cur_row.blens[ci];
    } else {
        *out = NULL;
        *out_len = 0;
    }
    return 0;
}

APENNINES_API unsigned long db_column_bytes(const u8 **out, u64 *out_len,
                                             db_stmt *s, u32 idx) {
    if (!out)     return 1;
    if (!out_len) return 2;
    if (!s)       return 3;
    if (!s->have_row) return 4;
    int ci = stmt_col_to_row_col(s, idx);
    if (ci < 0 || (u32)ci >= s->cur_row.col_count) return 5;
    if (s->cur_row.types[ci] == DB_TYPE_BLOB || s->cur_row.types[ci] == DB_TYPE_TEXT) {
        *out = s->cur_row.bvals[ci];
        *out_len = s->cur_row.blens[ci];
    } else {
        *out = NULL;
        *out_len = 0;
    }
    return 0;
}

APENNINES_API unsigned long db_column_is_null(int *out, db_stmt *s, u32 idx) {
    if (!out) return 1;
    if (!s)   return 2;
    if (!s->have_row) return 3;
    int ci = stmt_col_to_row_col(s, idx);
    if (ci < 0 || (u32)ci >= s->cur_row.col_count) return 4;
    *out = (s->cur_row.types[ci] == DB_TYPE_NULL) ? 1 : 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  db_finalize                                                        */
/* ------------------------------------------------------------------ */

APENNINES_API unsigned long db_finalize(db_stmt *s) {
    if (!s) return 1;

    db_conn *db = s->db;
    rwlock_write_lock(&db->conn_lock);

    if (s->have_row) row_free(&s->cur_row);
    if (s->iter) db_storage_iter_destroy(s->iter);
    if (s->ast) sql_ast_destroy(s->ast);
    sql_token_list_free(&s->tokens);

    /* Free any remaining materialised rows (ORDER BY path). Slots that
     * were already consumed by db_step were zeroed and will row_free
     * as a no-op. */
    if (s->sorted_rows) {
        for (u64 i = 0; i < s->sorted_count; ++i) row_free(&s->sorted_rows[i]);
        free(s->sorted_rows);
    }

    /* Free bind slot data */
    for (u32 i = 0; i < s->bind_count; ++i) {
        if (s->binds[i].bval) free(s->binds[i].bval);
    }

    free(s);
    rwlock_write_unlock(&db->conn_lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  db_reset                                                           */
/* ------------------------------------------------------------------ */

APENNINES_API unsigned long db_reset(db_stmt *s) {
    if (!s) return 1;

    rwlock_write_lock(&s->db->conn_lock);
    if (s->have_row) {
        row_free(&s->cur_row);
        s->have_row = 0;
    }
    if (s->iter) {
        db_storage_iter_destroy(s->iter);
        s->iter = NULL;
    }
    /* Drop any materialised ordered-result buffer — next step will
     * rebuild it against current binds / data. */
    if (s->sorted_rows) {
        for (u64 i = 0; i < s->sorted_count; ++i) row_free(&s->sorted_rows[i]);
        free(s->sorted_rows);
        s->sorted_rows = NULL;
    }
    s->sorted_count = 0;
    s->sorted_pos = 0;
    s->materialised = 0;
    s->rows_returned = 0;
    s->rows_skipped = 0;
    s->agg_produced = 0;
    s->using_index = 0;
    s->done = 0;
    rwlock_write_unlock(&s->db->conn_lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Transactions                                                       */
/* ------------------------------------------------------------------ */

APENNINES_API unsigned long db_begin(db_conn *db) {
    if (!db) return 1;
    rwlock_write_lock(&db->conn_lock);
    unsigned long rc = 0;
    if (db->in_txn) rc = 2;
    else {
        db->in_txn = 1;
        db->changes = 0;
        undo_clear(db);
    }
    rwlock_write_unlock(&db->conn_lock);
    return rc;
}

APENNINES_API unsigned long db_commit(db_conn *db) {
    if (!db) return 1;
    rwlock_write_lock(&db->conn_lock);
    unsigned long rc = 0;
    if (!db->in_txn) { rc = 2; goto out; }
    if (db->log) {
        rc = wal_sync(db->log);
        if (rc) { rc = 3; goto out; }
    }
    db->in_txn = 0;
    /* Commit discards the undo log — the changes are now permanent. */
    undo_clear(db);
out:
    rwlock_write_unlock(&db->conn_lock);
    return rc;
}

/* Reverse-replay the undo log: put-back for deletes/updates, delete
 * for inserts. Meta keys (table registry changes) aren't logged, so
 * DDL is not rolled back — document the limitation; cookbook does
 * all its DDL outside of transactions. */
/* Rebuild db->tables[] from whatever __meta__ keys currently live in
 * the KV store. Called from db_open during initial scan and from
 * db_rollback after replay (so DDL that ran inside the txn gets
 * fully reverted in-memory — CREATE'd tables disappear, DROP'd
 * tables come back, and the column/index/FK/unique metadata is
 * re-read from the now-authoritative meta rows). */
static unsigned long rebuild_tables_from_meta(db_conn *db) {
    db->table_count = 0;
    db_storage_iter *it = NULL;
    unsigned long rc = db->vt->iter_create(&it, db->storage,
                                        (const u8 *)DB_META_PREFIX,
                                        DB_META_PREFIX_LEN);
    if (rc) return rc;
    const u8 *k; u64 kl;
    const u8 *v; u64 vl;
    while (db_storage_iter_next(&k, &kl, &v, &vl, it) == 0) {
        if (db->table_count >= DB_MAX_TABLES) break;
        if (kl <= DB_META_PREFIX_LEN) continue;
        db_table *t = &db->tables[db->table_count];
        memset(t, 0, sizeof(*t));
        u64 nlen = kl - DB_META_PREFIX_LEN;
        if (nlen >= sizeof(t->name)) nlen = sizeof(t->name) - 1;
        memcpy(t->name, k + DB_META_PREFIX_LEN, nlen);
        t->name[nlen] = '\0';
        if (meta_deserialize(t, v, vl) == 0) db->table_count++;
    }
    db_storage_iter_destroy(it);
    return 0;
}

static unsigned long db_rollback_locked(db_conn *db) {
    if (!db->in_txn) return 2;

    for (i64 i = (i64)db->undo_count - 1; i >= 0; --i) {
        db_undo_entry *e = &db->undo[(u64)i];
        if (e->existed_before) {
            /* The key had a value before this op — restore it. */
            db->vt->put(db->storage, e->key, e->klen, e->prev_val, e->prev_vlen);
        } else {
            /* The key was absent — ensure it's absent again. */
            db->vt->del(db->storage, e->key, e->klen);
        }
    }
    undo_clear(db);

    /* After rollback, the on-disk meta state is authoritative. Rebuild
     * the whole tables[] array from it — this reverts in-memory state
     * for any CREATE / DROP TABLE that happened during the txn, and
     * pulls back correct col_count / next_rowid / FK / index specs
     * from the restored meta rows. */
    rebuild_tables_from_meta(db);

    db->in_txn = 0;
    db->changes = 0;
    return 0;
}

APENNINES_API unsigned long db_rollback(db_conn *db) {
    if (!db) return 1;
    rwlock_write_lock(&db->conn_lock);
    unsigned long rc = db_rollback_locked(db);
    rwlock_write_unlock(&db->conn_lock);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Utilities                                                          */
/* ------------------------------------------------------------------ */

APENNINES_API unsigned long db_last_insert_id(i64 *out, db_conn *db) {
    if (!out) return 1;
    if (!db)  return 2;
    rwlock_read_lock(&db->conn_lock);
    *out = db->last_insert_id;
    rwlock_read_unlock(&db->conn_lock);
    return 0;
}

APENNINES_API unsigned long db_changes(u64 *out, db_conn *db) {
    if (!out) return 1;
    if (!db)  return 2;
    rwlock_read_lock(&db->conn_lock);
    *out = db->changes;
    rwlock_read_unlock(&db->conn_lock);
    return 0;
}

APENNINES_API unsigned long db_table_exists(int *out, db_conn *db, const char *name) {
    if (!out)  return 1;
    if (!db)   return 2;
    if (!name) return 3;
    rwlock_read_lock(&db->conn_lock);
    *out = find_table(db, name) ? 1 : 0;
    rwlock_read_unlock(&db->conn_lock);
    return 0;
}

static unsigned long db_backup_locked(db_conn *db, const char *dest_path) {
    /* Open destination using the same backend as the source */
    char dst_kv[560];
    snprintf(dst_kv, sizeof(dst_kv), "%s.kv", dest_path);
    void *dst = NULL;
    unsigned long rc = db->vt->open(&dst, dst_kv);
    if (rc) return 3;

    /* Copy all keys */
    db_storage_iter *it = NULL;
    rc = db->vt->iter_create(&it, db->storage, NULL, 0);
    if (rc) { db->vt->close(dst); return 4; }

    const u8 *k; u64 kl;
    const u8 *v; u64 vl;
    while (db_storage_iter_next(&k, &kl, &v, &vl, it) == 0) {
        db->vt->put(dst, k, kl, v, vl);
    }
    db_storage_iter_destroy(it);
    db->vt->close(dst);
    return 0;
}

APENNINES_API unsigned long db_backup(db_conn *db, const char *dest_path) {
    if (!db)        return 1;
    if (!dest_path) return 2;
    /* Backup only reads the source kv; destination is a separate file.
     * Read lock lets other readers proceed but blocks writers. */
    rwlock_read_lock(&db->conn_lock);
    unsigned long rc = db_backup_locked(db, dest_path);
    rwlock_read_unlock(&db->conn_lock);
    return rc;
}

APENNINES_API unsigned long db_vacuum(db_conn *db) {
    if (!db) return 1;
    rwlock_write_lock(&db->conn_lock);
    unsigned long rc = 0;
    if (!db->storage) rc = 2;
    else rc = db->vt->compact(db->storage);
    rwlock_write_unlock(&db->conn_lock);
    return rc;
}

static unsigned long db_integrity_check_locked(int *out_ok, db_conn *db) {
    *out_ok = 1;

    /* Verify each table's metadata can be deserialized and rows are valid */
    for (u32 ti = 0; ti < db->table_count; ++ti) {
        db_table *t = &db->tables[ti];

        /* Check metadata key exists */
        u8 mkey[DB_MAX_KEY_LEN];
        u64 mlen = build_meta_key(mkey, sizeof(mkey), t->name);
        if (mlen == 0) { *out_ok = 0; return 0; }

        u8 *mv = NULL; u64 mvl = 0;
        unsigned long rc = db->vt->get(&mv, &mvl, db->storage, mkey, mlen);
        if (rc) { *out_ok = 0; return 0; }

        db_table tmp;
        memset(&tmp, 0, sizeof(tmp));
        if (meta_deserialize(&tmp, mv, mvl) != 0) {
            free(mv);
            *out_ok = 0;
            return 0;
        }
        free(mv);

        /* Verify each row can be deserialized */
        u8 prefix[DB_MAX_KEY_LEN];
        u64 plen = build_prefix_key(prefix, sizeof(prefix), t->name);
        if (plen == 0) { *out_ok = 0; return 0; }

        db_storage_iter *it = NULL;
        rc = db->vt->iter_create(&it, db->storage, prefix, plen);
        if (rc) { *out_ok = 0; return 0; }

        const u8 *k; u64 kl;
        const u8 *v; u64 vl;
        while (db_storage_iter_next(&k, &kl, &v, &vl, it) == 0) {
            db_row row;
            if (row_deserialize(&row, v, vl) != 0) {
                db_storage_iter_destroy(it);
                *out_ok = 0;
                return 0;
            }
            if (row.col_count != t->col_count) {
                row_free(&row);
                db_storage_iter_destroy(it);
                *out_ok = 0;
                return 0;
            }
            row_free(&row);
        }
        db_storage_iter_destroy(it);
    }

    return 0;
}

APENNINES_API unsigned long db_integrity_check(int *out_ok, db_conn *db) {
    if (!out_ok) return 1;
    if (!db)     return 2;
    /* Integrity check is read-only. */
    rwlock_read_lock(&db->conn_lock);
    unsigned long rc = db_integrity_check_locked(out_ok, db);
    rwlock_read_unlock(&db->conn_lock);
    return rc;
}

/* ================================================================
 *  Gap-fill stubs — Section 36
 * ================================================================ */

APENNINES_API unsigned long db_query(db_stmt **out, db_conn *db, const char *sql) {
    return db_prepare(out, db, sql);
}

APENNINES_API unsigned long db_begin_immediate(db_conn *db) {
    return db_exec(db, "BEGIN IMMEDIATE");
}

APENNINES_API unsigned long db_savepoint(db_conn *db, const char *name) {
    if (!db) return 1;
    if (!name) return 2;
    char buf[256];
    snprintf(buf, sizeof(buf), "SAVEPOINT %s", name);
    return db_exec(db, buf);
}

APENNINES_API unsigned long db_release_savepoint(db_conn *db, const char *name) {
    if (!db) return 1;
    if (!name) return 2;
    char buf[256];
    snprintf(buf, sizeof(buf), "RELEASE SAVEPOINT %s", name);
    return db_exec(db, buf);
}

APENNINES_API unsigned long db_rollback_to_savepoint(db_conn *db, const char *name) {
    if (!db) return 1;
    if (!name) return 2;
    char buf[256];
    snprintf(buf, sizeof(buf), "ROLLBACK TO SAVEPOINT %s", name);
    return db_exec(db, buf);
}

APENNINES_API unsigned long db_bind_decimal(db_stmt *s, u32 idx,
                                             const char *decimal_str) {
    if (!s) return 1;
    if (!decimal_str) return 2;
    return db_bind_str(s, idx, decimal_str, (u64)strlen(decimal_str));
}

APENNINES_API unsigned long db_column_decimal(const char **out, db_stmt *s, u32 idx) {
    u64 len = 0;
    return db_column_str(out, &len, s, idx);
}

APENNINES_API unsigned long db_set_busy_timeout(db_conn *db, u64 ms) {
    if (!db) return 1;
    rwlock_write_lock(&db->conn_lock);
    db->busy_timeout_ms = ms;
    rwlock_write_unlock(&db->conn_lock);
    return 0;
}

APENNINES_API unsigned long db_set_journal_mode(db_conn *db, const char *mode) {
    if (!db)   return 1;
    if (!mode) return 2;
    /* Validate mode string. */
    if (strcasecmp_a(mode, "wal") != 0 &&
        strcasecmp_a(mode, "delete") != 0 &&
        strcasecmp_a(mode, "truncate") != 0 &&
        strcasecmp_a(mode, "persist") != 0 &&
        strcasecmp_a(mode, "off") != 0) {
        return 3;
    }
    rwlock_write_lock(&db->conn_lock);
    strncpy(db->journal_mode, mode, sizeof(db->journal_mode) - 1);
    db->journal_mode[sizeof(db->journal_mode) - 1] = '\0';
    rwlock_write_unlock(&db->conn_lock);
    return 0;
}

APENNINES_API unsigned long db_set_page_size(db_conn *db, u32 size) {
    if (!db) return 1;
    /* Page size must be a power of two between 512 and 65536. */
    if (size < 512 || size > 65536) return 2;
    if ((size & (size - 1)) != 0)   return 3;
    rwlock_write_lock(&db->conn_lock);
    db->page_size = size;
    rwlock_write_unlock(&db->conn_lock);
    return 0;
}

APENNINES_API unsigned long db_table_list(const char ***out, u64 *out_count,
                                           db_conn *db) {
    if (!out) return 1;
    if (!out_count) return 2;
    if (!db) return 3;
    rwlock_read_lock(&db->conn_lock);
    u64 count = db->table_count;
    const char **names = (const char **)calloc(count ? count : 1, sizeof(const char *));
    if (!names) { rwlock_read_unlock(&db->conn_lock); return 4; }
    for (u32 i = 0; i < db->table_count; i++) {
        names[i] = db->tables[i].name;
    }
    *out = names;
    *out_count = count;
    rwlock_read_unlock(&db->conn_lock);
    return 0;
}

APENNINES_API unsigned long db_checkpoint(db_conn *db) {
    if (!db) return 1;
    rwlock_write_lock(&db->conn_lock);
    unsigned long rc = wal_sync(db->log);
    rwlock_write_unlock(&db->conn_lock);
    return rc;
}
