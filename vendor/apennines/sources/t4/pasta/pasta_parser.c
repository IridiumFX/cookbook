#include "pasta_internal.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 *  Pasta / Basta parser — recursive-descent.
 *
 *  Produces a pasta_value tree. A document with @section entries
 *  parses to a root map keyed by section name; a document with
 *  container-only content parses to the container itself (or an
 *  array wrapping multiple top-level containers).
 * ================================================================ */

typedef struct {
    const u8 *src;
    u64 len;
    u64 pos;
    u32 line;
    u32 col;
    pasta_result *res;
} parser;

/* ---- Error reporting ---- */

static void set_err(parser *p, unsigned long hatch, const char *msg) {
    if (!p->res) return;
    if (p->res->hatch != 0) return;     /* don't overwrite first error */
    p->res->hatch = hatch;
    p->res->offset = p->pos;
    p->res->line = p->line;
    p->res->col = p->col;
    if (msg) {
        u64 i;
        for (i = 0; i < sizeof(p->res->msg) - 1 && msg[i]; i++) {
            p->res->msg[i] = msg[i];
        }
        p->res->msg[i] = 0;
    }
}

/* ---- Cursor primitives ---- */

static int at_eof(parser *p) { return p->pos >= p->len; }

static u8 peek0(parser *p) {
    return at_eof(p) ? 0 : p->src[p->pos];
}

static u8 peek1(parser *p) {
    return (p->pos + 1 < p->len) ? p->src[p->pos + 1] : 0;
}

static void advance(parser *p) {
    if (at_eof(p)) return;
    if (p->src[p->pos] == '\n') { p->line++; p->col = 1; }
    else p->col++;
    p->pos++;
}

/* Skip whitespace, comments (;...), and newlines. */
static void skip_blanks(parser *p) {
    while (!at_eof(p)) {
        u8 c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(p);
        } else if (c == ';') {
            while (!at_eof(p) && p->src[p->pos] != '\n') advance(p);
        } else {
            break;
        }
    }
}

/* Accept exact byte; return 1 if consumed. */
static int accept_byte(parser *p, u8 c) {
    if (!at_eof(p) && p->src[p->pos] == c) { advance(p); return 1; }
    return 0;
}

/* ---- Label character classification ----
 *
 * unquoted-label = labelchar +
 * labelchar      = alphabet | digit | "!" | "#" | "$" | "%" | "&" | "." | "_"
 */

static int is_labelchar(u8 c) {
    return (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9')
        || c == '!' || c == '#' || c == '$' || c == '%'
        || c == '&' || c == '.' || c == '_';
}

/* ---- String readers ---- */

/* Read a simple or triple-quoted string starting at the current `"`.
 * Advances past the closing quote. */
static unsigned long read_string(parser *p, u8 **out_data, u64 *out_len) {
    u8 *buf = NULL;
    u64 cap = 0;
    u64 n = 0;
    int triple = 0;

    if (!accept_byte(p, '"')) {
        set_err(p, 10, "expected '\"'");
        return 10;
    }

    /* Check for triple quote. */
    if (peek0(p) == '"' && peek1(p) == '"') {
        advance(p); advance(p);
        triple = 1;
    }

    while (!at_eof(p)) {
        u8 c = p->src[p->pos];

        if (triple) {
            /* Triple: read until """ sentinel. */
            if (c == '"' && p->pos + 2 < p->len
                && p->src[p->pos + 1] == '"' && p->src[p->pos + 2] == '"') {
                advance(p); advance(p); advance(p);
                if (!buf) {
                    buf = (u8 *)malloc(1);
                    if (!buf) { set_err(p, 11, "oom"); return 11; }
                }
                buf[n] = 0;
                *out_data = buf;
                *out_len = n;
                return 0;
            }
        } else {
            if (c == '"') {
                advance(p);
                if (!buf) {
                    buf = (u8 *)malloc(1);
                    if (!buf) { set_err(p, 11, "oom"); return 11; }
                }
                buf[n] = 0;
                *out_data = buf;
                *out_len = n;
                return 0;
            }
            if (c == '\n') {
                set_err(p, 12, "newline in simple string (use triple quotes)");
                free(buf);
                return 12;
            }
            /* Per the Pasta spec: simple strings have NO escape
             * sequences. Backslash, quote-in-string etc. are
             * handled by triple-quoted strings for anything the
             * simple form can't carry. */
        }

        advance(p);

    append:
        if (n + 1 > cap) {
            u64 new_cap = cap ? cap * 2 : 16;
            u8 *new_buf = (u8 *)realloc(buf, new_cap + 1);
            if (!new_buf) { set_err(p, 11, "oom"); free(buf); return 11; }
            buf = new_buf;
            cap = new_cap;
        }
        buf[n++] = c;
    }

    set_err(p, 14, "unterminated string");
    free(buf);
    return 14;
}

/* Read an unquoted label into a fresh malloc'd buffer. */
static unsigned long read_unquoted_label(parser *p, u8 **out, u64 *out_len) {
    u64 start = p->pos;
    u64 n;
    u8 *b;

    while (!at_eof(p) && is_labelchar(p->src[p->pos])) advance(p);
    n = p->pos - start;
    if (n == 0) { set_err(p, 20, "expected label"); return 20; }
    b = (u8 *)malloc(n + 1);
    if (!b) { set_err(p, 11, "oom"); return 11; }
    memcpy(b, p->src + start, n);
    b[n] = 0;
    *out = b;
    *out_len = n;
    return 0;
}

/* ---- Number parsing ----
 *
 * Supports: signed integer, 0x/0X hex, 0b/0B bin, decimal with
 * optional fractional part. Caller is responsible for classifying
 * against 'Inf', 'NaN' keywords before invoking this. */

static int is_digit(u8 c) { return c >= '0' && c <= '9'; }
static int is_hex(u8 c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int is_bin(u8 c) { return c == '0' || c == '1'; }

static int hex_val(u8 c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static unsigned long read_number(parser *p, pasta_value **out) {
    int neg = 0;
    u64 start;
    u8 c;

    if (peek0(p) == '-') { neg = 1; advance(p); }

    start = p->pos;
    c = peek0(p);

    /* Hex / bin — tag with format hint so the writer can round-trip
     * the original literal form. */
    if (c == '0' && (peek1(p) == 'x' || peek1(p) == 'X')) {
        u64 v = 0;
        advance(p); advance(p);
        if (!is_hex(peek0(p))) { set_err(p, 30, "expected hex digit"); return 30; }
        while (!at_eof(p) && is_hex(p->src[p->pos])) {
            v = v * 16 + (u64)hex_val(p->src[p->pos]);
            advance(p);
        }
        return pasta_new_integer_fmt(out, neg ? -(i64)v : (i64)v, 1);
    }
    if (c == '0' && (peek1(p) == 'b' || peek1(p) == 'B')) {
        u64 v = 0;
        advance(p); advance(p);
        if (!is_bin(peek0(p))) { set_err(p, 31, "expected binary digit"); return 31; }
        while (!at_eof(p) && is_bin(p->src[p->pos])) {
            v = v * 2 + (u64)(p->src[p->pos] - '0');
            advance(p);
        }
        return pasta_new_integer_fmt(out, neg ? -(i64)v : (i64)v, 2);
    }

    /* Decimal: integer or float. */
    if (!is_digit(c)) { set_err(p, 32, "expected digit"); return 32; }

    {
        int is_float = 0;
        u64 int_part = 0;
        f64 frac_val = 0.0;
        while (!at_eof(p) && is_digit(p->src[p->pos])) {
            int_part = int_part * 10 + (u64)(p->src[p->pos] - '0');
            advance(p);
        }
        if (peek0(p) == '.') {
            f64 divisor = 10.0;
            is_float = 1;
            advance(p);
            while (!at_eof(p) && is_digit(p->src[p->pos])) {
                frac_val += (p->src[p->pos] - '0') / divisor;
                divisor *= 10.0;
                advance(p);
            }
        }
        /* Exponent (e.g. 1.5e2) — not in the BNF but tolerant of it. */
        if (peek0(p) == 'e' || peek0(p) == 'E') {
            int exp_neg = 0;
            i64 exp_val = 0;
            is_float = 1;
            advance(p);
            if (peek0(p) == '+') advance(p);
            else if (peek0(p) == '-') { exp_neg = 1; advance(p); }
            while (!at_eof(p) && is_digit(p->src[p->pos])) {
                exp_val = exp_val * 10 + (p->src[p->pos] - '0');
                advance(p);
            }
            {
                f64 base = (f64)int_part + frac_val;
                f64 mult = pow(10.0, exp_neg ? -(f64)exp_val : (f64)exp_val);
                f64 v = base * mult;
                return pasta_new_number(out, neg ? -v : v);
            }
        }

        (void)start;
        if (is_float) {
            f64 v = (f64)int_part + frac_val;
            return pasta_new_number(out, neg ? -v : v);
        } else {
            return pasta_new_integer(out, neg ? -(i64)int_part : (i64)int_part);
        }
    }
}

/* ---- Blob (basta only) ---- */

static unsigned long read_blob(parser *p, pasta_value **out) {
    u64 n = 0;
    u64 i;
    if (!accept_byte(p, 0x00)) { set_err(p, 40, "expected NUL"); return 40; }
    if (p->pos + 8 > p->len) { set_err(p, 41, "blob length truncated"); return 41; }
    for (i = 0; i < 8; i++) {
        n = (n << 8) | p->src[p->pos + i];
    }
    p->pos += 8;
    p->col += 8;                  /* cursor accounting approximate for binary */
    if (p->pos + n > p->len) { set_err(p, 42, "blob bytes truncated"); return 42; }
    {
        unsigned long rc = pasta_new_blob(out, p->src + p->pos, n);
        if (rc) { set_err(p, rc + 100, "blob alloc"); return rc + 100; }
    }
    p->pos += n;
    /* Don't try to track line/col across raw bytes. */
    return 0;
}

/* ---- Forward declarations ---- */

static unsigned long parse_value(parser *p, pasta_value **out);
static unsigned long parse_container(parser *p, pasta_value **out);

/* ---- Atoms ---- */

static int starts_with(parser *p, const char *word) {
    u64 n = (u64)strlen(word);
    if (p->pos + n > p->len) return 0;
    if (memcmp(p->src + p->pos, word, n) != 0) return 0;
    /* Must be followed by a non-labelchar. */
    if (p->pos + n < p->len && is_labelchar(p->src[p->pos + n])) return 0;
    return 1;
}

static void consume(parser *p, u64 n) {
    u64 i;
    for (i = 0; i < n; i++) advance(p);
}

static unsigned long parse_atom_unquoted(parser *p, pasta_value **out) {
    /* Called with peek0 being a labelchar. Could be:
     *   true / false / null / Inf / NaN, or a label-ref. */
    if (starts_with(p, "true"))  { consume(p, 4); return pasta_new_bool(out, 1); }
    if (starts_with(p, "false")) { consume(p, 5); return pasta_new_bool(out, 0); }
    if (starts_with(p, "null"))  { consume(p, 4); return pasta_new_null(out); }
    if (starts_with(p, "Inf"))   { consume(p, 3); return pasta_new_number(out, INFINITY); }
    if (starts_with(p, "NaN"))   { consume(p, 3); return pasta_new_number(out, NAN); }

    /* Otherwise: label-ref. */
    {
        u8 *name = NULL;
        u64 nlen = 0;
        pasta_value *v = NULL;
        unsigned long rc = read_unquoted_label(p, &name, &nlen);
        if (rc) return rc;
        v = calloc(1, sizeof(*v));
        if (!v) { free(name); set_err(p, 11, "oom"); return 11; }
        v->kind = PASTA_LABEL_REF;
        v->u.label.name = name;
        v->u.label.len = nlen;
        *out = v;
        return 0;
    }
}

/* ---- Value ---- */

static unsigned long parse_value(parser *p, pasta_value **out) {
    u8 c;

    skip_blanks(p);
    if (at_eof(p)) { set_err(p, 50, "unexpected eof"); return 50; }

    c = p->src[p->pos];

    if (c == '{' || c == '[') return parse_container(p, out);
    if (c == '"') {
        u8 *data = NULL;
        u64 len = 0;
        unsigned long rc = read_string(p, &data, &len);
        if (rc) return rc;
        {
            pasta_value *v = calloc(1, sizeof(*v));
            if (!v) { free(data); set_err(p, 11, "oom"); return 11; }
            v->kind = PASTA_STRING;
            v->u.s.data = data;
            v->u.s.len = len;
            *out = v;
            return 0;
        }
    }
    if (c == 0x00) return read_blob(p, out);

    /* -Inf as a special case (unquoted starts with '-'). */
    if (c == '-' && p->pos + 3 < p->len
        && memcmp(p->src + p->pos, "-Inf", 4) == 0
        && (p->pos + 4 == p->len || !is_labelchar(p->src[p->pos + 4]))) {
        consume(p, 4);
        return pasta_new_number(out, -INFINITY);
    }
    if (c == '-' || (c >= '0' && c <= '9')) return read_number(p, out);

    if (is_labelchar(c)) return parse_atom_unquoted(p, out);

    set_err(p, 51, "unexpected character");
    return 51;
}

/* ---- Container ---- */

static unsigned long parse_container(parser *p, pasta_value **out) {
    u8 first;

    skip_blanks(p);
    first = peek0(p);

    if (first == '{') {
        pasta_value *m = NULL;
        unsigned long rc;
        advance(p);  /* '{' */
        rc = pasta_new_map(&m);
        if (rc) { set_err(p, 11, "oom"); return rc; }

        skip_blanks(p);
        if (peek0(p) == '}') {
            advance(p);
            *out = m;
            return 0;
        }

        for (;;) {
            u8 *key = NULL;
            u64 klen = 0;
            pasta_value *val = NULL;

            skip_blanks(p);
            if (peek0(p) == '"') {
                rc = read_string(p, &key, &klen);
                if (rc) { pasta_value_destroy(m); return rc; }
            } else if (is_labelchar(peek0(p))) {
                rc = read_unquoted_label(p, &key, &klen);
                if (rc) { pasta_value_destroy(m); return rc; }
            } else {
                set_err(p, 60, "expected map key");
                pasta_value_destroy(m);
                return 60;
            }

            skip_blanks(p);
            if (!accept_byte(p, ':')) {
                free(key);
                set_err(p, 61, "expected ':'");
                pasta_value_destroy(m);
                return 61;
            }

            skip_blanks(p);
            rc = parse_value(p, &val);
            if (rc) { free(key); pasta_value_destroy(m); return rc; }

            rc = pasta_map_put_n(m, key, klen, val);
            free(key);
            if (rc) { pasta_value_destroy(val); pasta_value_destroy(m); return rc; }

            skip_blanks(p);
            if (peek0(p) == ',') { advance(p); continue; }
            if (peek0(p) == '}') { advance(p); break; }
            set_err(p, 62, "expected ',' or '}'");
            pasta_value_destroy(m);
            return 62;
        }
        *out = m;
        return 0;
    }

    if (first == '[') {
        pasta_value *a = NULL;
        unsigned long rc;
        advance(p);
        rc = pasta_new_array(&a);
        if (rc) { set_err(p, 11, "oom"); return rc; }

        skip_blanks(p);
        if (peek0(p) == ']') {
            advance(p);
            *out = a;
            return 0;
        }

        for (;;) {
            pasta_value *val = NULL;
            skip_blanks(p);
            rc = parse_value(p, &val);
            if (rc) { pasta_value_destroy(a); return rc; }
            rc = pasta_array_push(a, val);
            if (rc) { pasta_value_destroy(val); pasta_value_destroy(a); return rc; }

            skip_blanks(p);
            if (peek0(p) == ',') { advance(p); continue; }
            if (peek0(p) == ']') { advance(p); break; }
            set_err(p, 70, "expected ',' or ']'");
            pasta_value_destroy(a);
            return 70;
        }
        *out = a;
        return 0;
    }

    set_err(p, 80, "expected '{' or '['");
    return 80;
}

/* ---- Top level ---- */

unsigned long pasta_parse(pasta_value **out, pasta_result *res,
                            const u8 *src, u64 len) {
    parser p;
    unsigned long rc;
    int saw_section = 0;
    pasta_value *root_map = NULL;
    pasta_value *single = NULL;

    if (!out) return 1;
    if (len > 0 && !src) return 2;

    memset(&p, 0, sizeof(p));
    p.src = src;
    p.len = len;
    p.line = 1;
    p.col = 1;
    if (res) {
        res->hatch = 0;
        res->offset = 0;
        res->line = 1;
        res->col = 1;
        res->msg[0] = 0;
    }
    p.res = res;

    skip_blanks(&p);
    if (at_eof(&p)) {
        /* Empty document (including whitespace-only and
         * comments-only) is a parse error — matches sibling
         * project's contract. */
        set_err(&p, 50, "empty document");
        return 50;
    }

    /* Detect section-mode by looking for leading '@'. */
    if (peek0(&p) == '@') saw_section = 1;

    if (saw_section) {
        rc = pasta_new_map(&root_map);
        if (rc) { if (res) { res->hatch = rc; } return rc; }

        while (!at_eof(&p)) {
            u8 *name;
            u64 nlen;
            pasta_value *sec = NULL;

            skip_blanks(&p);
            if (at_eof(&p)) break;
            if (!accept_byte(&p, '@')) {
                set_err(&p, 90, "expected '@' between sections");
                pasta_value_destroy(root_map);
                return 90;
            }
            if (peek0(&p) == '"') {
                rc = read_string(&p, &name, &nlen);
            } else {
                rc = read_unquoted_label(&p, &name, &nlen);
            }
            if (rc) { pasta_value_destroy(root_map); return rc; }
            skip_blanks(&p);
            rc = parse_container(&p, &sec);
            if (rc) { free(name); pasta_value_destroy(root_map); return rc; }
            rc = pasta_map_put_n(root_map, name, nlen, sec);
            free(name);
            if (rc) {
                pasta_value_destroy(sec);
                pasta_value_destroy(root_map);
                return rc;
            }
            skip_blanks(&p);
        }
        *out = root_map;
        return 0;
    }

    /* Single-value doc. Accept any value at the top level — the BNF
     * in the spec restricts to containers but the sibling impl (and
     * its test suite) accepts bare scalars + label-refs too, so we
     * match. If there are multiple top-level values they wrap into
     * an array. */
    rc = parse_value(&p, &single);
    if (rc) return rc;
    skip_blanks(&p);
    if (!at_eof(&p)) {
        pasta_value *arr = NULL;
        rc = pasta_new_array(&arr);
        if (rc) { pasta_value_destroy(single); return rc; }
        rc = pasta_array_push(arr, single);
        if (rc) { pasta_value_destroy(single); pasta_value_destroy(arr); return rc; }
        while (!at_eof(&p)) {
            pasta_value *more = NULL;
            rc = parse_value(&p, &more);
            if (rc) { pasta_value_destroy(arr); return rc; }
            rc = pasta_array_push(arr, more);
            if (rc) { pasta_value_destroy(more); pasta_value_destroy(arr); return rc; }
            skip_blanks(&p);
        }
        *out = arr;
        return 0;
    }

    *out = single;
    return 0;
}
