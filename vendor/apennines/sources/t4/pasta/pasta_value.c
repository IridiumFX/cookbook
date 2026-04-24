#include "pasta_internal.h"
#include <stdlib.h>
#include <string.h>

/* ================================================================
 *  pasta_value — value-tree primitives.
 *
 *  All creators malloc a fresh node on the heap. Maps and arrays
 *  own their children; destroy walks the tree and frees everything.
 * ================================================================ */

static pasta_value *alloc_value(pasta_kind kind) {
    pasta_value *v = (pasta_value *)calloc(1, sizeof(*v));
    if (v) v->kind = kind;
    return v;
}

static u8 *dup_bytes(const u8 *src, u64 len) {
    u8 *p;
    if (len == 0) {
        p = (u8 *)malloc(1);
        if (p) p[0] = 0;
        return p;
    }
    p = (u8 *)malloc(len + 1);   /* +1 for NUL terminator on string-like */
    if (!p) return NULL;
    memcpy(p, src, len);
    p[len] = 0;
    return p;
}

/* ---- Creators ---- */

unsigned long pasta_new_null(pasta_value **out) {
    if (!out) return 1;
    *out = alloc_value(PASTA_NULL);
    return *out ? 0 : 2;
}

unsigned long pasta_new_bool(pasta_value **out, int b) {
    if (!out) return 1;
    *out = alloc_value(PASTA_BOOL);
    if (!*out) return 2;
    (*out)->u.b = b ? 1 : 0;
    return 0;
}

unsigned long pasta_new_integer(pasta_value **out, i64 v) {
    if (!out) return 1;
    *out = alloc_value(PASTA_INTEGER);
    if (!*out) return 2;
    (*out)->u.integer = v;
    return 0;
}

unsigned long pasta_new_integer_fmt(pasta_value **out, i64 v, int fmt) {
    unsigned long rc = pasta_new_integer(out, v);
    if (rc) return rc;
    if (fmt >= 0 && fmt <= 2) (*out)->num_fmt = (u8)fmt;
    return 0;
}

unsigned long pasta_number_fmt(int *out, const pasta_value *v) {
    if (!out) return 1;
    if (!v) return 2;
    *out = (int)v->num_fmt;
    return 0;
}

unsigned long pasta_new_number(pasta_value **out, f64 v) {
    if (!out) return 1;
    *out = alloc_value(PASTA_NUMBER);
    if (!*out) return 2;
    (*out)->u.number = v;
    return 0;
}

unsigned long pasta_new_string(pasta_value **out, const char *s) {
    u64 n = s ? (u64)strlen(s) : 0;
    return pasta_new_string_n(out, (const u8 *)s, n);
}

unsigned long pasta_new_string_n(pasta_value **out, const u8 *s, u64 n) {
    if (!out) return 1;
    if (n > 0 && !s) return 2;
    *out = alloc_value(PASTA_STRING);
    if (!*out) return 3;
    (*out)->u.s.data = dup_bytes(s, n);
    if (!(*out)->u.s.data) { free(*out); *out = NULL; return 3; }
    (*out)->u.s.len = n;
    return 0;
}

unsigned long pasta_new_label_ref(pasta_value **out, const char *name) {
    u64 n;
    if (!out) return 1;
    if (!name) return 2;
    n = (u64)strlen(name);
    *out = alloc_value(PASTA_LABEL_REF);
    if (!*out) return 3;
    (*out)->u.label.name = dup_bytes((const u8 *)name, n);
    if (!(*out)->u.label.name) { free(*out); *out = NULL; return 3; }
    (*out)->u.label.len = n;
    return 0;
}

unsigned long pasta_new_array(pasta_value **out) {
    if (!out) return 1;
    *out = alloc_value(PASTA_ARRAY);
    return *out ? 0 : 2;
}

unsigned long pasta_new_map(pasta_value **out) {
    if (!out) return 1;
    *out = alloc_value(PASTA_MAP);
    return *out ? 0 : 2;
}

unsigned long pasta_new_blob(pasta_value **out, const u8 *data, u64 n) {
    if (!out) return 1;
    if (n > 0 && !data) return 2;
    *out = alloc_value(PASTA_BLOB);
    if (!*out) return 3;
    /* Always allocate at least 1 byte so accessors return a
     * non-NULL pointer even for zero-length blobs (matches the
     * sibling Basta contract). */
    (*out)->u.blob.data = (u8 *)malloc(n > 0 ? n : 1);
    if (!(*out)->u.blob.data) { free(*out); *out = NULL; return 3; }
    if (n) memcpy((*out)->u.blob.data, data, n);
    else   (*out)->u.blob.data[0] = 0;
    (*out)->u.blob.len = n;
    return 0;
}

/* ---- Destroy (recursive) ---- */

unsigned long pasta_value_destroy(pasta_value *v) {
    u64 i;
    if (!v) return 0;
    switch (v->kind) {
        case PASTA_STRING:    free(v->u.s.data); break;
        case PASTA_LABEL_REF: free(v->u.label.name); break;
        case PASTA_BLOB:      free(v->u.blob.data); break;
        case PASTA_ARRAY:
            for (i = 0; i < v->u.array.count; i++) {
                pasta_value_destroy(v->u.array.items[i]);
            }
            free(v->u.array.items);
            break;
        case PASTA_MAP:
            for (i = 0; i < v->u.map.count; i++) {
                free(v->u.map.keys[i]);
                pasta_value_destroy(v->u.map.vals[i]);
            }
            free(v->u.map.keys);
            free(v->u.map.key_lens);
            free(v->u.map.vals);
            break;
        default: break;
    }
    free(v);
    return 0;
}

/* ---- Clone (deep) ---- */

unsigned long pasta_value_clone(pasta_value **out, const pasta_value *src) {
    unsigned long rc;
    u64 i;
    if (!out) return 1;
    if (!src) return 2;

    switch (src->kind) {
        case PASTA_NULL:      return pasta_new_null(out);
        case PASTA_BOOL:      return pasta_new_bool(out, src->u.b);
        case PASTA_INTEGER:   return pasta_new_integer(out, src->u.integer);
        case PASTA_NUMBER:    return pasta_new_number(out, src->u.number);
        case PASTA_STRING:
            return pasta_new_string_n(out, src->u.s.data, src->u.s.len);
        case PASTA_LABEL_REF: {
            pasta_value *v = alloc_value(PASTA_LABEL_REF);
            if (!v) return 3;
            v->u.label.name = dup_bytes(src->u.label.name, src->u.label.len);
            if (!v->u.label.name) { free(v); return 3; }
            v->u.label.len = src->u.label.len;
            *out = v;
            return 0;
        }
        case PASTA_BLOB:
            return pasta_new_blob(out, src->u.blob.data, src->u.blob.len);
        case PASTA_ARRAY: {
            pasta_value *arr = NULL;
            rc = pasta_new_array(&arr);
            if (rc) return rc;
            for (i = 0; i < src->u.array.count; i++) {
                pasta_value *child = NULL;
                rc = pasta_value_clone(&child, src->u.array.items[i]);
                if (rc) { pasta_value_destroy(arr); return rc; }
                rc = pasta_array_push(arr, child);
                if (rc) { pasta_value_destroy(child); pasta_value_destroy(arr); return rc; }
            }
            *out = arr;
            return 0;
        }
        case PASTA_MAP: {
            pasta_value *m = NULL;
            rc = pasta_new_map(&m);
            if (rc) return rc;
            for (i = 0; i < src->u.map.count; i++) {
                pasta_value *child = NULL;
                rc = pasta_value_clone(&child, src->u.map.vals[i]);
                if (rc) { pasta_value_destroy(m); return rc; }
                rc = pasta_map_put_n(m, src->u.map.keys[i],
                                       src->u.map.key_lens[i], child);
                if (rc) { pasta_value_destroy(child); pasta_value_destroy(m); return rc; }
            }
            *out = m;
            return 0;
        }
    }
    return 99;
}

/* ---- Queries ---- */

unsigned long pasta_kind_of(pasta_kind *out, const pasta_value *v) {
    if (!out) return 1;
    if (!v) return 2;
    *out = v->kind;
    return 0;
}

unsigned long pasta_as_bool(int *out, const pasta_value *v) {
    if (!out) return 1;
    if (!v) return 2;
    if (v->kind != PASTA_BOOL) return 3;
    *out = v->u.b;
    return 0;
}

unsigned long pasta_as_integer(i64 *out, const pasta_value *v) {
    if (!out) return 1;
    if (!v) return 2;
    if (v->kind == PASTA_INTEGER) { *out = v->u.integer; return 0; }
    if (v->kind == PASTA_NUMBER)  { *out = (i64)v->u.number; return 0; }
    return 3;
}

unsigned long pasta_as_number(f64 *out, const pasta_value *v) {
    if (!out) return 1;
    if (!v) return 2;
    if (v->kind == PASTA_NUMBER)  { *out = v->u.number; return 0; }
    if (v->kind == PASTA_INTEGER) { *out = (f64)v->u.integer; return 0; }
    return 3;
}

unsigned long pasta_as_string(const u8 **out_ptr, u64 *out_len,
                                const pasta_value *v) {
    if (!out_ptr) return 1;
    if (!out_len) return 2;
    if (!v) return 3;
    if (v->kind != PASTA_STRING) return 4;
    *out_ptr = v->u.s.data;
    *out_len = v->u.s.len;
    return 0;
}

unsigned long pasta_as_label(const u8 **out_ptr, u64 *out_len,
                                const pasta_value *v) {
    if (!out_ptr) return 1;
    if (!out_len) return 2;
    if (!v) return 3;
    if (v->kind != PASTA_LABEL_REF) return 4;
    *out_ptr = v->u.label.name;
    *out_len = v->u.label.len;
    return 0;
}

unsigned long pasta_as_blob(const u8 **out_ptr, u64 *out_len,
                              const pasta_value *v) {
    if (!out_ptr) return 1;
    if (!out_len) return 2;
    if (!v) return 3;
    if (v->kind != PASTA_BLOB) return 4;
    *out_ptr = v->u.blob.data;
    *out_len = v->u.blob.len;
    return 0;
}

unsigned long pasta_count(u64 *out, const pasta_value *v) {
    if (!out) return 1;
    if (!v) return 2;
    if (v->kind == PASTA_ARRAY) { *out = v->u.array.count; return 0; }
    if (v->kind == PASTA_MAP)   { *out = v->u.map.count;   return 0; }
    return 3;
}

unsigned long pasta_array_at(pasta_value **out, const pasta_value *v, u64 idx) {
    if (!out) return 1;
    if (!v) return 2;
    if (v->kind != PASTA_ARRAY) return 3;
    if (idx >= v->u.array.count) return 4;
    *out = v->u.array.items[idx];
    return 0;
}

unsigned long pasta_map_get(pasta_value **out, const pasta_value *v,
                              const char *key) {
    u64 klen, i;
    if (!out) return 1;
    if (!v) return 2;
    if (!key) return 3;
    if (v->kind != PASTA_MAP) return 4;
    klen = (u64)strlen(key);
    for (i = 0; i < v->u.map.count; i++) {
        if (v->u.map.key_lens[i] == klen
            && memcmp(v->u.map.keys[i], key, klen) == 0) {
            *out = v->u.map.vals[i];
            return 0;
        }
    }
    *out = NULL;
    return 5;
}

unsigned long pasta_map_key(const u8 **out_ptr, u64 *out_len,
                              const pasta_value *v, u64 idx) {
    if (!out_ptr) return 1;
    if (!out_len) return 2;
    if (!v) return 3;
    if (v->kind != PASTA_MAP) return 4;
    if (idx >= v->u.map.count) return 5;
    *out_ptr = v->u.map.keys[idx];
    *out_len = v->u.map.key_lens[idx];
    return 0;
}

unsigned long pasta_map_value(pasta_value **out, const pasta_value *v, u64 idx) {
    if (!out) return 1;
    if (!v) return 2;
    if (v->kind != PASTA_MAP) return 3;
    if (idx >= v->u.map.count) return 4;
    *out = v->u.map.vals[idx];
    return 0;
}

/* ---- Mutation ---- */

unsigned long pasta__array_reserve(pasta_value *a, u64 need) {
    u64 new_cap;
    pasta_value **new_items;
    if (need <= a->u.array.cap) return 0;
    new_cap = a->u.array.cap ? a->u.array.cap : 4;
    while (new_cap < need) new_cap *= 2;
    new_items = (pasta_value **)realloc(a->u.array.items,
                                          new_cap * sizeof(*new_items));
    if (!new_items) return 1;
    a->u.array.items = new_items;
    a->u.array.cap = new_cap;
    return 0;
}

unsigned long pasta__map_reserve(pasta_value *m, u64 need) {
    u64 new_cap;
    u8 **new_keys;
    u64 *new_lens;
    pasta_value **new_vals;
    if (need <= m->u.map.cap) return 0;
    new_cap = m->u.map.cap ? m->u.map.cap : 4;
    while (new_cap < need) new_cap *= 2;
    new_keys = (u8 **)realloc(m->u.map.keys, new_cap * sizeof(*new_keys));
    if (!new_keys) return 1;
    m->u.map.keys = new_keys;
    new_lens = (u64 *)realloc(m->u.map.key_lens, new_cap * sizeof(*new_lens));
    if (!new_lens) return 1;
    m->u.map.key_lens = new_lens;
    new_vals = (pasta_value **)realloc(m->u.map.vals, new_cap * sizeof(*new_vals));
    if (!new_vals) return 1;
    m->u.map.vals = new_vals;
    m->u.map.cap = new_cap;
    return 0;
}

unsigned long pasta_array_push(pasta_value *a, pasta_value *val) {
    unsigned long rc;
    if (!a) return 1;
    if (!val) return 2;
    if (a->kind != PASTA_ARRAY) return 3;
    rc = pasta__array_reserve(a, a->u.array.count + 1);
    if (rc) return rc;
    a->u.array.items[a->u.array.count++] = val;
    return 0;
}

unsigned long pasta_map_put(pasta_value *m, const char *key, pasta_value *val) {
    return pasta_map_put_n(m, (const u8 *)key,
                             key ? (u64)strlen(key) : 0, val);
}

unsigned long pasta_map_put_n(pasta_value *m, const u8 *key, u64 key_len,
                                pasta_value *val) {
    u64 i;
    unsigned long rc;
    u8 *kdup;

    if (!m) return 1;
    if (!val) return 2;
    if (m->kind != PASTA_MAP) return 3;
    if (key_len > 0 && !key) return 4;

    /* Replace-in-place on duplicate key (last-write-wins). */
    for (i = 0; i < m->u.map.count; i++) {
        if (m->u.map.key_lens[i] == key_len
            && (key_len == 0 || memcmp(m->u.map.keys[i], key, key_len) == 0)) {
            pasta_value_destroy(m->u.map.vals[i]);
            m->u.map.vals[i] = val;
            return 0;
        }
    }

    rc = pasta__map_reserve(m, m->u.map.count + 1);
    if (rc) return rc;

    kdup = (u8 *)malloc(key_len + 1);
    if (!kdup) return 5;
    if (key_len) memcpy(kdup, key, key_len);
    kdup[key_len] = 0;

    m->u.map.keys[m->u.map.count] = kdup;
    m->u.map.key_lens[m->u.map.count] = key_len;
    m->u.map.vals[m->u.map.count] = val;
    m->u.map.count++;
    return 0;
}
