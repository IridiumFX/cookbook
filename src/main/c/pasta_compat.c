/* Shim that realises the sibling Pasta API over our t4/pasta.
 *
 * Public entry points are named tpasta_* to avoid symbol collision
 * with our native apennines pasta_* API. The pasta_compat.h header
 * provides macros that rewrite sibling source calls onto these
 * tpasta_* names — see pasta_compat.h.
 */

#include "apennines/t4/pasta/pasta.h"
#include "apennines/t1/buffer/buf.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Redeclare the sibling types + enum we return, WITHOUT including
 * pasta_compat.h (which would remap our own apennines pasta_* calls
 * via the macros). */

typedef enum {
    TPASTA_NULL = 0,
    TPASTA_BOOL,
    TPASTA_NUMBER,
    TPASTA_STRING,
    TPASTA_ARRAY,
    TPASTA_MAP,
    TPASTA_LABEL
} tpasta_type_e;

typedef enum {
    TPASTA_OK = 0,
    TPASTA_ERR_ALLOC,
    TPASTA_ERR_SYNTAX,
    TPASTA_ERR_UNEXPECTED_TOKEN,
    TPASTA_ERR_UNEXPECTED_EOF
} tpasta_error_e;

typedef struct {
    tpasta_error_e code;
    int            line;
    int            col;
    int            sections;
    char           message[256];
} tpasta_result_t;

/* Sibling PastaValue* handles map 1:1 to our pasta_value*. */
#define WRAP(p) ((void *)(p))
#define AS(p)   ((pasta_value *)(void *)(p))

/* Fold our parser hatches into the sibling's enum. */
static tpasta_error_e map_hatch(unsigned long h) {
    if (h == 0) return TPASTA_OK;
    if (h == 50)            return TPASTA_ERR_UNEXPECTED_EOF;
    if (h == 14 || h == 13) return TPASTA_ERR_UNEXPECTED_EOF;
    if (h == 11 || h == 3 || h == 7) return TPASTA_ERR_ALLOC;
    return TPASTA_ERR_SYNTAX;
}

static void fill_result(tpasta_result_t *res, pasta_result *ours, int is_sec) {
    if (!res) return;
    res->code = map_hatch(ours->hatch);
    res->line = (int)ours->line;
    res->col = (int)ours->col;
    res->sections = is_sec ? 1 : 0;
    if (ours->msg[0]) {
        snprintf(res->message, sizeof(res->message), "%s", ours->msg);
    } else if (ours->hatch) {
        snprintf(res->message, sizeof(res->message), "parse error hatch=%lu", ours->hatch);
    } else {
        res->message[0] = 0;
    }
}

/* ================================================================
 *  Public entry points — tpasta_xxx.
 *
 *  These don't declare sibling-style prototypes; the compiler is
 *  happy with implicit K&R-style for integer/pointer args if we
 *  write prototypes where called. We just define bodies here with
 *  concrete types.
 * ================================================================ */

void *tpasta_parse(const char *input, size_t len, void *result_v) {
    tpasta_result_t *result = (tpasta_result_t *)result_v;
    pasta_value *v = NULL;
    pasta_result res;
    memset(&res, 0, sizeof(res));

    int is_sec = 0;
    if (input && len > 0) {
        size_t i = 0;
        while (i < len) {
            unsigned char c = (unsigned char)input[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
            if (c == ';') { while (i < len && input[i] != '\n') i++; continue; }
            if (c == '@') is_sec = 1;
            break;
        }
    }

    unsigned long rc = pasta_parse(&v, &res, (const u8 *)input, (u64)len);
    if (rc != 0) {
        if (result) {
            memset(result, 0, sizeof(*result));
            fill_result(result, &res, is_sec);
        }
        if (v) pasta_value_destroy(v);
        return NULL;
    }
    if (result) {
        memset(result, 0, sizeof(*result));
        result->code = TPASTA_OK;
        result->sections = is_sec;
        result->line = 1;
        result->col = 1;
    }
    return WRAP(v);
}

void *tpasta_parse_cstr(const char *input, void *result) {
    size_t n = input ? strlen(input) : 0;
    return tpasta_parse(input, n, result);
}

void tpasta_free(void *value) {
    if (!value) return;
    pasta_value_destroy(AS(value));
}

/* Queries */
int tpasta_type(const void *v) {
    pasta_kind k;
    if (!v || pasta_kind_of(&k, AS(v)) != 0) return TPASTA_NULL;
    switch (k) {
        case PASTA_NULL:      return TPASTA_NULL;
        case PASTA_BOOL:      return TPASTA_BOOL;
        case PASTA_INTEGER:   return TPASTA_NUMBER;
        case PASTA_NUMBER:    return TPASTA_NUMBER;
        case PASTA_STRING:    return TPASTA_STRING;
        case PASTA_LABEL_REF: return TPASTA_LABEL;
        case PASTA_ARRAY:     return TPASTA_ARRAY;
        case PASTA_MAP:       return TPASTA_MAP;
        case PASTA_BLOB:      return TPASTA_STRING;
    }
    return TPASTA_NULL;
}

int tpasta_is_null(const void *v) { return tpasta_type(v) == TPASTA_NULL; }

int tpasta_get_bool(const void *v) {
    int out = 0;
    if (!v) return 0;
    if (pasta_as_bool(&out, AS(v)) != 0) return 0;
    return out;
}

double tpasta_get_number(const void *v) {
    double out = 0;
    if (!v) return 0.0;
    if (pasta_as_number(&out, AS(v)) != 0) return 0.0;
    return out;
}

int tpasta_get_number_fmt(const void *v) {
    int out = 0;
    if (!v) return 0;
    pasta_number_fmt(&out, AS(v));
    return out;
}

const char *tpasta_get_string(const void *v) {
    const u8 *p = NULL;
    u64 n = 0;
    if (!v) return NULL;
    if (pasta_as_string(&p, &n, AS(v)) != 0) return NULL;
    return (const char *)p;
}
size_t tpasta_get_string_len(const void *v) {
    const u8 *p; u64 n = 0;
    if (!v) return 0;
    if (pasta_as_string(&p, &n, AS(v)) != 0) return 0;
    return (size_t)n;
}
const char *tpasta_get_label(const void *v) {
    const u8 *p = NULL;
    u64 n = 0;
    if (!v) return NULL;
    if (pasta_as_label(&p, &n, AS(v)) != 0) return NULL;
    return (const char *)p;
}
size_t tpasta_get_label_len(const void *v) {
    const u8 *p; u64 n = 0;
    if (!v) return 0;
    if (pasta_as_label(&p, &n, AS(v)) != 0) return 0;
    return (size_t)n;
}

size_t tpasta_count(const void *v) {
    u64 n = 0;
    if (!v) return 0;
    if (pasta_count(&n, AS(v)) != 0) return 0;
    return (size_t)n;
}

const void *tpasta_array_get(const void *v, size_t index) {
    pasta_value *out = NULL;
    if (!v) return NULL;
    if (pasta_array_at(&out, AS(v), (u64)index) != 0) return NULL;
    return WRAP(out);
}

const void *tpasta_map_get(const void *v, const char *key) {
    pasta_value *out = NULL;
    if (!v || !key) return NULL;
    if (pasta_map_get(&out, AS(v), key) != 0) return NULL;
    return WRAP(out);
}

const char *tpasta_map_key(const void *v, size_t index) {
    const u8 *p = NULL;
    u64 n = 0;
    if (!v) return NULL;
    if (pasta_map_key(&p, &n, AS(v), (u64)index) != 0) return NULL;
    return (const char *)p;
}

const void *tpasta_map_value(const void *v, size_t index) {
    pasta_value *out = NULL;
    if (!v) return NULL;
    if (pasta_map_value(&out, AS(v), (u64)index) != 0) return NULL;
    return WRAP(out);
}

/* Constructors */
void *tpasta_new_null(void) {
    pasta_value *v = NULL;
    if (pasta_new_null(&v) != 0) return NULL;
    return WRAP(v);
}
void *tpasta_new_bool(int b) {
    pasta_value *v = NULL;
    if (pasta_new_bool(&v, b) != 0) return NULL;
    return WRAP(v);
}
void *tpasta_new_number(double n) {
    pasta_value *v = NULL;
    /* Preserve integer-ness when n is an exact integer — the
     * sibling tests expect "3.0" to roundtrip as "3", not "3.0". */
    if (n == (double)(long long)n && n >= -1e18 && n <= 1e18) {
        if (pasta_new_integer(&v, (long long)n) != 0) return NULL;
    } else {
        if (pasta_new_number(&v, n) != 0) return NULL;
    }
    return WRAP(v);
}
void *tpasta_new_number_fmt(double n, int fmt) {
    /* Only integers can carry a hex/bin format hint. Fall back to
     * plain number for floats. */
    if (fmt != 0 && n == (double)(long long)n && n >= -1e18 && n <= 1e18) {
        pasta_value *v = NULL;
        if (pasta_new_integer_fmt(&v, (long long)n, fmt) != 0) return NULL;
        return WRAP(v);
    }
    return tpasta_new_number(n);
}
void *tpasta_new_string(const char *s) {
    pasta_value *v = NULL;
    if (pasta_new_string(&v, s) != 0) return NULL;
    return WRAP(v);
}
void *tpasta_new_string_len(const char *s, size_t len) {
    pasta_value *v = NULL;
    if (pasta_new_string_n(&v, (const u8 *)s, (u64)len) != 0) return NULL;
    return WRAP(v);
}
void *tpasta_new_label(const char *s) {
    pasta_value *v = NULL;
    if (pasta_new_label_ref(&v, s) != 0) return NULL;
    return WRAP(v);
}
void *tpasta_new_label_len(const char *s, size_t len) {
    char *tmp = (char *)malloc(len + 1);
    pasta_value *v = NULL;
    if (!tmp) return NULL;
    if (len) memcpy(tmp, s, len);
    tmp[len] = 0;
    if (pasta_new_label_ref(&v, tmp) != 0) { free(tmp); return NULL; }
    free(tmp);
    return WRAP(v);
}
void *tpasta_new_array(void) {
    pasta_value *v = NULL;
    if (pasta_new_array(&v) != 0) return NULL;
    return WRAP(v);
}
void *tpasta_new_map(void) {
    pasta_value *v = NULL;
    if (pasta_new_map(&v) != 0) return NULL;
    return WRAP(v);
}

int tpasta_push(void *array, void *item) {
    if (!array || !item) return -1;
    return pasta_array_push(AS(array), AS(item)) == 0 ? 0 : -1;
}
int tpasta_set(void *map, const char *key, void *value) {
    if (!map || !key || !value) return -1;
    return pasta_map_put(AS(map), key, AS(value)) == 0 ? 0 : -1;
}
int tpasta_set_len(void *map, const char *key, size_t key_len, void *value) {
    if (!map || !key || !value) return -1;
    return pasta_map_put_n(AS(map), (const u8 *)key, (u64)key_len, AS(value)) == 0 ? 0 : -1;
}

/* Writer */
char *tpasta_write(const void *v, int flags) {
    buf out;
    u8 *p;
    u64 n;
    char *result;
    u32 our_flags = 0;

    /* Sibling treats NULL value as "null" literal. */
    if (!v) {
        char *s = (char *)malloc(5);
        if (!s) return NULL;
        memcpy(s, "null", 5);
        return s;
    }
    if (buf_create(&out, 0) != 0) return NULL;

    if (flags & 1) our_flags |= PASTA_WRITE_COMPACT;
    if (flags & 2) our_flags |= PASTA_WRITE_SECTIONS;
    if (flags & 4) our_flags |= PASTA_WRITE_SORTED;

    if (pasta_write(&out, AS(v), our_flags) != 0) {
        buf_destroy(&out);
        return NULL;
    }
    if (buf_ptr(&p, &out) != 0 || buf_len(&n, &out) != 0) {
        buf_destroy(&out);
        return NULL;
    }
    result = (char *)malloc(n + 1);
    if (!result) { buf_destroy(&out); return NULL; }
    if (n) memcpy(result, p, n);
    result[n] = 0;
    buf_destroy(&out);
    return result;
}

int tpasta_write_fp(const void *v, int flags, void *fp) {
    char *s = tpasta_write(v, flags);
    if (!s) return -1;
    size_t n = strlen(s);
    size_t wrote = fwrite(s, 1, n, (FILE *)fp);
    free(s);
    return wrote == n ? 0 : -1;
}
