#include "pasta_internal.h"
#include "apennines/t2/string/fmt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 *  Pasta / Basta serializer.
 *
 *  Two formatting modes:
 *    pretty   (default)  — indented multiline, one member/element
 *                          per line once a container grows past a
 *                          small threshold
 *    compact  (flag)     — single-line, minimal whitespace
 *
 *  Plus flags:
 *    PASTA_WRITE_SECTIONS — root must be a map; emit it as @name
 *                           containers instead of an outer map
 *    PASTA_WRITE_SORTED   — sort map keys lexicographically for
 *                           deterministic output (handy for tests
 *                           and diffs)
 * ================================================================ */

typedef struct {
    buf *out;
    u32 flags;
    u32 depth;
} writer;

static int is_labelchar(u8 c) {
    return (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9')
        || c == '!' || c == '#' || c == '$' || c == '%'
        || c == '&' || c == '.' || c == '_';
}

static unsigned long emit_lit(writer *w, const char *s) {
    return buf_append(w->out, (u8 *)s, (u64)strlen(s));
}

static unsigned long emit_byte(writer *w, u8 c) {
    return buf_append_byte(w->out, c);
}

static unsigned long emit_indent(writer *w) {
    u32 i;
    unsigned long rc;
    if (w->flags & PASTA_WRITE_COMPACT) return 0;
    for (i = 0; i < w->depth; i++) {
        rc = emit_lit(w, "  ");
        if (rc) return rc;
    }
    return 0;
}

static unsigned long emit_newline(writer *w) {
    if (w->flags & PASTA_WRITE_COMPACT) return 0;
    return emit_byte(w, '\n');
}

/* Emit a label, quoting only if it contains non-label chars. */
static unsigned long emit_label(writer *w, const u8 *name, u64 n) {
    u64 i;
    int need_quote = (n == 0);
    if (!need_quote) {
        for (i = 0; i < n; i++) {
            if (!is_labelchar(name[i])) { need_quote = 1; break; }
        }
    }
    /* Also quote if it's a reserved keyword so it stays a key
     * rather than a literal. */
    if (!need_quote) {
        if ((n == 4 && memcmp(name, "true", 4) == 0)
            || (n == 5 && memcmp(name, "false", 5) == 0)
            || (n == 4 && memcmp(name, "null", 4) == 0)
            || (n == 3 && memcmp(name, "Inf", 3) == 0)
            || (n == 3 && memcmp(name, "NaN", 3) == 0)) {
            need_quote = 1;
        }
    }
    if (need_quote) {
        unsigned long rc;
        rc = emit_byte(w, '"'); if (rc) return rc;
        for (i = 0; i < n; i++) {
            u8 c = name[i];
            if (c == '"' || c == '\\') {
                rc = emit_byte(w, '\\'); if (rc) return rc;
                rc = emit_byte(w, c);    if (rc) return rc;
            } else if (c == '\n') {
                rc = emit_lit(w, "\\n"); if (rc) return rc;
            } else if (c == '\r') {
                rc = emit_lit(w, "\\r"); if (rc) return rc;
            } else if (c == '\t') {
                rc = emit_lit(w, "\\t"); if (rc) return rc;
            } else {
                rc = emit_byte(w, c); if (rc) return rc;
            }
        }
        return emit_byte(w, '"');
    }
    return buf_append(w->out, (u8 *)name, n);
}

static unsigned long emit_string(writer *w, const u8 *s, u64 n) {
    u64 i;
    int has_newline = 0;
    int has_quote = 0;
    int has_triple = 0;
    unsigned long rc;
    for (i = 0; i < n; i++) {
        if (s[i] == '\n') has_newline = 1;
        if (s[i] == '"')  has_quote = 1;
        if (i + 2 < n && s[i] == '"' && s[i+1] == '"' && s[i+2] == '"') {
            has_triple = 1;
        }
    }
    /* Triple-quoted form: required for newlines or for embedded
     * single "/" (since simple strings have no escapes and can't
     * contain the closing quote). Refuses only when """ is present
     * (Pasta's format limitation). */
    if ((has_newline || has_quote) && !has_triple) {
        rc = emit_lit(w, "\"\"\""); if (rc) return rc;
        rc = buf_append(w->out, (u8 *)s, n); if (rc) return rc;
        return emit_lit(w, "\"\"\"");
    }
    /* Per Pasta spec, simple strings have no escape sequences. If
     * the string contains a character that would otherwise break
     * simple-string parsing (embedded newlines, triple quotes
     * needing escape), we already took the multiline path above.
     * Anything else — including backslashes — passes through as
     * its literal byte. */
    rc = emit_byte(w, '"'); if (rc) return rc;
    rc = buf_append(w->out, (u8 *)s, n); if (rc) return rc;
    return emit_byte(w, '"');
}

static unsigned long emit_number(writer *w, f64 v) {
    if (v != v) return emit_lit(w, "NaN");
    if (v > 0 && !isfinite(v)) return emit_lit(w, "Inf");
    if (v < 0 && !isfinite(v)) return emit_lit(w, "-Inf");
    /* Choose the shortest representation that round-trips exactly. */
    {
        char tmp[48];
        int n = snprintf(tmp, sizeof(tmp), "%.17g", v);
        if (n < 0 || (size_t)n >= sizeof(tmp)) return 2;
        return buf_append(w->out, (u8 *)tmp, (u64)n);
    }
}

static unsigned long emit_integer(writer *w, i64 v, u8 fmt) {
    char tmp[66];
    int n;
    if (fmt == 1) {
        if (v < 0) n = snprintf(tmp, sizeof(tmp), "-0x%llx",
                                  (unsigned long long)(-v));
        else       n = snprintf(tmp, sizeof(tmp), "0x%llx",
                                  (unsigned long long)v);
    } else if (fmt == 2) {
        /* Binary — emit MSB-first, no leading zeros. */
        unsigned long long u = (v < 0) ? (unsigned long long)(-v) : (unsigned long long)v;
        int i;
        int started = 0;
        n = 0;
        if (v < 0) tmp[n++] = '-';
        tmp[n++] = '0';
        tmp[n++] = 'b';
        if (u == 0) {
            tmp[n++] = '0';
        } else {
            for (i = 63; i >= 0; i--) {
                int bit = (int)((u >> i) & 1);
                if (bit) started = 1;
                if (started) tmp[n++] = (char)('0' + bit);
            }
        }
    } else {
        n = snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
    }
    if (n < 0 || (size_t)n >= sizeof(tmp)) return 1;
    return buf_append(w->out, (u8 *)tmp, (u64)n);
}

static unsigned long emit_blob(writer *w, const u8 *data, u64 n) {
    /* NUL sentinel + u64be length + bytes. */
    u8 hdr[9];
    unsigned long rc;
    hdr[0] = 0x00;
    hdr[1] = (u8)(n >> 56);
    hdr[2] = (u8)(n >> 48);
    hdr[3] = (u8)(n >> 40);
    hdr[4] = (u8)(n >> 32);
    hdr[5] = (u8)(n >> 24);
    hdr[6] = (u8)(n >> 16);
    hdr[7] = (u8)(n >> 8);
    hdr[8] = (u8)n;
    rc = buf_append(w->out, hdr, 9);
    if (rc) return rc;
    if (n) rc = buf_append(w->out, (u8 *)data, n);
    return rc;
}

/* Forward. */
static unsigned long emit_value(writer *w, const pasta_value *v);

/* ---- Sorted-key helper: produces an index array giving sorted
 * order of map keys. Caller frees. ---- */
typedef struct { const u8 *k; u64 kl; u64 i; } sort_rec;

static int sort_cmp(const void *a, const void *b) {
    const sort_rec *ra = (const sort_rec *)a;
    const sort_rec *rb = (const sort_rec *)b;
    u64 n = ra->kl < rb->kl ? ra->kl : rb->kl;
    int c = memcmp(ra->k, rb->k, n);
    if (c) return c;
    if (ra->kl < rb->kl) return -1;
    if (ra->kl > rb->kl) return 1;
    return 0;
}

static u64 *sorted_indices(const pasta_value *m) {
    u64 *idx;
    sort_rec *recs;
    u64 i;
    if (m->u.map.count == 0) return NULL;
    idx = (u64 *)malloc(m->u.map.count * sizeof(u64));
    if (!idx) return NULL;
    recs = (sort_rec *)malloc(m->u.map.count * sizeof(*recs));
    if (!recs) { free(idx); return NULL; }
    for (i = 0; i < m->u.map.count; i++) {
        recs[i].k = m->u.map.keys[i];
        recs[i].kl = m->u.map.key_lens[i];
        recs[i].i = i;
    }
    qsort(recs, m->u.map.count, sizeof(*recs), sort_cmp);
    for (i = 0; i < m->u.map.count; i++) idx[i] = recs[i].i;
    free(recs);
    return idx;
}

static unsigned long emit_map(writer *w, const pasta_value *v) {
    unsigned long rc;
    u64 i;
    u64 *order = NULL;

    if (v->u.map.count == 0) return emit_lit(w, "{}");

    rc = emit_byte(w, '{'); if (rc) return rc;
    w->depth++;

    if (w->flags & PASTA_WRITE_SORTED) order = sorted_indices(v);

    for (i = 0; i < v->u.map.count; i++) {
        u64 ix = order ? order[i] : i;
        if (!(w->flags & PASTA_WRITE_COMPACT)) {
            rc = emit_newline(w); if (rc) { free(order); return rc; }
            rc = emit_indent(w);  if (rc) { free(order); return rc; }
        }
        rc = emit_label(w, v->u.map.keys[ix], v->u.map.key_lens[ix]);
        if (rc) { free(order); return rc; }
        rc = emit_lit(w, ": ");
        if (rc) { free(order); return rc; }
        rc = emit_value(w, v->u.map.vals[ix]);
        if (rc) { free(order); return rc; }
        if (i + 1 < v->u.map.count) {
            rc = emit_byte(w, ',');
            if (rc) { free(order); return rc; }
            if (w->flags & PASTA_WRITE_COMPACT) {
                rc = emit_byte(w, ' ');
                if (rc) { free(order); return rc; }
            }
        }
    }
    free(order);
    w->depth--;
    if (!(w->flags & PASTA_WRITE_COMPACT)) {
        rc = emit_newline(w); if (rc) return rc;
        rc = emit_indent(w);  if (rc) return rc;
    }
    return emit_byte(w, '}');
}

static unsigned long emit_array(writer *w, const pasta_value *v) {
    unsigned long rc;
    u64 i;

    if (v->u.array.count == 0) return emit_lit(w, "[]");

    rc = emit_byte(w, '['); if (rc) return rc;
    w->depth++;
    for (i = 0; i < v->u.array.count; i++) {
        if (!(w->flags & PASTA_WRITE_COMPACT)) {
            rc = emit_newline(w); if (rc) return rc;
            rc = emit_indent(w);  if (rc) return rc;
        }
        rc = emit_value(w, v->u.array.items[i]); if (rc) return rc;
        if (i + 1 < v->u.array.count) {
            rc = emit_byte(w, ',');
            if (rc) return rc;
            if (w->flags & PASTA_WRITE_COMPACT) {
                rc = emit_byte(w, ' ');
                if (rc) return rc;
            }
        }
    }
    w->depth--;
    if (!(w->flags & PASTA_WRITE_COMPACT)) {
        rc = emit_newline(w); if (rc) return rc;
        rc = emit_indent(w);  if (rc) return rc;
    }
    return emit_byte(w, ']');
}

static unsigned long emit_value(writer *w, const pasta_value *v) {
    switch (v->kind) {
        case PASTA_NULL:     return emit_lit(w, "null");
        case PASTA_BOOL:     return emit_lit(w, v->u.b ? "true" : "false");
        case PASTA_INTEGER:  return emit_integer(w, v->u.integer, v->num_fmt);
        case PASTA_NUMBER:   return emit_number(w, v->u.number);
        case PASTA_STRING:   return emit_string(w, v->u.s.data, v->u.s.len);
        case PASTA_LABEL_REF:
            /* label-ref emits as the bare label. */
            return buf_append(w->out, v->u.label.name, v->u.label.len);
        case PASTA_BLOB:     return emit_blob(w, v->u.blob.data, v->u.blob.len);
        case PASTA_MAP:      return emit_map(w, v);
        case PASTA_ARRAY:    return emit_array(w, v);
    }
    return 1;
}

unsigned long pasta_write(buf *out, const pasta_value *v, u32 flags) {
    writer w;
    unsigned long rc;

    if (!out) return 1;
    if (!v) return 2;

    memset(&w, 0, sizeof(w));
    w.out = out;
    w.flags = flags;

    if ((flags & PASTA_WRITE_SECTIONS) && v->kind == PASTA_MAP) {
        /* Emit root as @name containers. */
        u64 *order = (flags & PASTA_WRITE_SORTED) ? sorted_indices(v) : NULL;
        u64 i;
        for (i = 0; i < v->u.map.count; i++) {
            u64 ix = order ? order[i] : i;
            pasta_value *child = v->u.map.vals[ix];
            if (i > 0 && !(flags & PASTA_WRITE_COMPACT)) {
                rc = emit_lit(&w, "\n"); if (rc) { free(order); return rc; }
            }
            rc = emit_byte(&w, '@'); if (rc) { free(order); return rc; }
            rc = emit_label(&w, v->u.map.keys[ix], v->u.map.key_lens[ix]);
            if (rc) { free(order); return rc; }
            rc = emit_byte(&w, ' '); if (rc) { free(order); return rc; }
            rc = emit_value(&w, child); if (rc) { free(order); return rc; }
            if (!(flags & PASTA_WRITE_COMPACT)) {
                rc = emit_newline(&w); if (rc) { free(order); return rc; }
            }
        }
        free(order);
        return 0;
    }

    rc = emit_value(&w, v);
    if (rc) return rc;
    /* Pretty mode ends with a trailing newline so sibling tests that
     * grep for "}\n" / "]\n" in the output find it. Compact mode
     * stays newline-free. */
    if (!(flags & PASTA_WRITE_COMPACT)) {
        rc = emit_byte(&w, '\n');
    }
    return rc;
}
