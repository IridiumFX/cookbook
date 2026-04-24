#include "apennines/t4/pasta/alforno.h"
#include "pasta_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 *  alforno implementation.
 *
 *  Pipeline:
 *    1. parameterize    — {var} substitution in string values
 *    2. when-filter     — drop tag-guarded sections
 *    3. op-specific merge (aggregate / conflate / gather / scatter)
 *    4. link resolution — label-refs replaced by their targets
 *
 *  Steps are implemented as pure functions on pasta_value trees
 *  using pasta_value_clone to avoid aliasing. The clone cost is
 *  absorbable for config-scale pastlets.
 * ================================================================ */

struct alforno_ctx {
    alforno_op op;
    alforno_precedence precedence;

    char **tags;        /* each dup'd */
    u64 tag_count;

    pasta_value *recipe;    /* owned, conflate only */

    pasta_value **inputs;
    u64 input_count;
    u64 input_cap;

    char *base_dir;
};

/* ---- Helpers ---- */

static char *dup_cstr_n(const char *s, u64 n) {
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static int is_map(const pasta_value *v) {
    pasta_kind k;
    if (!v) return 0;
    if (pasta_kind_of(&k, v)) return 0;
    return k == PASTA_MAP;
}

static int is_array(const pasta_value *v) {
    pasta_kind k;
    if (!v) return 0;
    if (pasta_kind_of(&k, v)) return 0;
    return k == PASTA_ARRAY;
}

static int is_string(const pasta_value *v) {
    pasta_kind k;
    if (!v) return 0;
    if (pasta_kind_of(&k, v)) return 0;
    return k == PASTA_STRING;
}

/* ---- Lifecycle ---- */

unsigned long alforno_create(alforno_ctx **out, alforno_op op) {
    alforno_ctx *c;
    if (!out) return 1;
    c = (alforno_ctx *)calloc(1, sizeof(*c));
    if (!c) return 2;
    c->op = op;
    c->precedence = ALFORNO_LAST_WINS;
    *out = c;
    return 0;
}

unsigned long alforno_destroy(alforno_ctx *ctx) {
    u64 i;
    if (!ctx) return 0;
    for (i = 0; i < ctx->tag_count; i++) free(ctx->tags[i]);
    free(ctx->tags);
    for (i = 0; i < ctx->input_count; i++) pasta_value_destroy(ctx->inputs[i]);
    free(ctx->inputs);
    pasta_value_destroy(ctx->recipe);
    free(ctx->base_dir);
    free(ctx);
    return 0;
}

unsigned long alforno_set_base_dir(alforno_ctx *ctx, const char *dir) {
    if (!ctx) return 1;
    free(ctx->base_dir);
    ctx->base_dir = NULL;
    if (dir) {
        ctx->base_dir = dup_cstr_n(dir, (u64)strlen(dir));
        if (!ctx->base_dir) return 2;
    }
    return 0;
}

/* ---- @include file loader ---- */

static u8 *slurp_file(const char *path, u64 *out_len) {
    FILE *f = fopen(path, "rb");
    u8 *b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    b = (u8 *)malloc((size_t)n ? (size_t)n : 1);
    if (!b) { fclose(f); return NULL; }
    if (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b); fclose(f); return NULL;
    }
    fclose(f);
    *out_len = (u64)n;
    return b;
}

static void path_dir(char *out, u64 out_cap, const char *path) {
    u64 n = (u64)strlen(path);
    u64 i;
    u64 last_sep = 0;
    int found = 0;
    for (i = 0; i < n; i++) {
        if (path[i] == '/' || path[i] == '\\') { last_sep = i; found = 1; }
    }
    if (!found) { if (out_cap > 0) out[0] = 0; return; }
    if (last_sep + 1 > out_cap) last_sep = out_cap - 1;
    memcpy(out, path, last_sep);
    out[last_sep] = 0;
}

static void path_join(char *out, u64 out_cap,
                        const char *dir, const char *rel) {
    u64 dn = dir ? (u64)strlen(dir) : 0;
    u64 rn = (u64)strlen(rel);
    /* If rel is absolute (leading '/' or 'X:' drive letter), use as-is. */
    int rel_abs = 0;
    if (rn > 0 && (rel[0] == '/' || rel[0] == '\\')) rel_abs = 1;
    if (rn > 1 && rel[1] == ':') rel_abs = 1;
    if (rel_abs || dn == 0) {
        if (rn + 1 > out_cap) rn = out_cap - 1;
        memcpy(out, rel, rn);
        out[rn] = 0;
        return;
    }
    {
        int need_sep = (dir[dn - 1] != '/' && dir[dn - 1] != '\\');
        u64 total = dn + (need_sep ? 1 : 0) + rn;
        if (total + 1 > out_cap) total = out_cap - 1;
        if (dn > out_cap) dn = out_cap - 1;
        memcpy(out, dir, dn);
        if (need_sep && dn < out_cap - 1) {
            out[dn] = '/';
            dn++;
        }
        if (dn + rn < out_cap) {
            memcpy(out + dn, rel, rn);
            out[dn + rn] = 0;
        } else {
            memcpy(out + dn, rel, out_cap - dn - 1);
            out[out_cap - 1] = 0;
        }
    }
}

#define MAX_INCLUDE_DEPTH 32

static unsigned long add_file_rec(alforno_ctx *ctx, const char *path,
                                    char ***visited, u32 *vc, u32 depth);

/* Strip the @include section from a parsed value (it's consumed by
 * the include pass). Pasta parser stores section keys without the
 * leading '@' so we look up "include". */
static void strip_include(pasta_value *doc) {
    u64 i;
    if (!is_map(doc)) return;
    i = doc->u.map.count;
    while (i > 0) {
        i--;
        if (doc->u.map.key_lens[i] == 7
            && memcmp(doc->u.map.keys[i], "include", 7) == 0) {
            pasta_value_destroy(doc->u.map.vals[i]);
            free(doc->u.map.keys[i]);
            {
                u64 j;
                for (j = i + 1; j < doc->u.map.count; j++) {
                    doc->u.map.keys[j - 1] = doc->u.map.keys[j];
                    doc->u.map.key_lens[j - 1] = doc->u.map.key_lens[j];
                    doc->u.map.vals[j - 1] = doc->u.map.vals[j];
                }
            }
            doc->u.map.count--;
            break;
        }
    }
}

static unsigned long add_file_rec(alforno_ctx *ctx, const char *path,
                                    char ***visited, u32 *vc, u32 depth) {
    u64 len = 0;
    u8 *bytes;
    pasta_value *doc = NULL;
    pasta_result res;
    pasta_value *includes = NULL;
    unsigned long rc;
    u32 i;
    char dir_buf[1024];

    if (depth > MAX_INCLUDE_DEPTH) return 50;

    for (i = 0; i < *vc; i++) {
        if (strcmp((*visited)[i], path) == 0) return 51;   /* cycle */
    }

    bytes = slurp_file(path, &len);
    if (!bytes) return 52;

    rc = pasta_parse(&doc, &res, bytes, len);
    free(bytes);
    if (rc) return 53;

    /* Record visit. */
    {
        char **nv = (char **)realloc(*visited, (*vc + 1) * sizeof(char *));
        if (!nv) { pasta_value_destroy(doc); return 54; }
        *visited = nv;
        (*visited)[*vc] = dup_cstr_n(path, (u64)strlen(path));
        if (!(*visited)[*vc]) { pasta_value_destroy(doc); return 54; }
        (*vc)++;
    }

    path_dir(dir_buf, sizeof(dir_buf), path);

    /* Resolve @include entries (array of path strings). Parser
     * strips the leading '@' so the key is "include". */
    (void)pasta_map_get(&includes, doc, "include");
    if (includes && is_array(includes)) {
        u64 n, k;
        pasta_count(&n, includes);
        for (k = 0; k < n; k++) {
            pasta_value *item = NULL;
            const u8 *sp; u64 sl;
            char child[1024];
            pasta_array_at(&item, includes, k);
            if (!item || pasta_as_string(&sp, &sl, item) != 0) continue;
            if (sl >= sizeof(child)) { pasta_value_destroy(doc); return 55; }
            {
                char rel[1024];
                memcpy(rel, sp, sl); rel[sl] = 0;
                path_join(child, sizeof(child),
                            dir_buf[0] ? dir_buf
                                       : (ctx->base_dir ? ctx->base_dir : ""),
                            rel);
            }
            rc = add_file_rec(ctx, child, visited, vc, depth + 1);
            if (rc) { pasta_value_destroy(doc); return rc; }
        }
    }

    strip_include(doc);

    rc = alforno_add_input(ctx, doc);
    if (rc) { pasta_value_destroy(doc); return rc; }
    return 0;
}

unsigned long alforno_add_input_file(alforno_ctx *ctx, const char *path) {
    char **visited = NULL;
    u32 vc = 0;
    unsigned long rc;
    u32 i;
    char resolved[1024];

    if (!ctx) return 1;
    if (!path) return 2;

    /* If path is relative and base_dir is set, resolve against it. */
    path_join(resolved, sizeof(resolved),
                ctx->base_dir ? ctx->base_dir : "", path);

    rc = add_file_rec(ctx, resolved, &visited, &vc, 0);

    for (i = 0; i < vc; i++) free(visited[i]);
    free(visited);
    return rc;
}

unsigned long alforno_set_precedence(alforno_ctx *ctx, alforno_precedence p) {
    if (!ctx) return 1;
    ctx->precedence = p;
    return 0;
}

unsigned long alforno_set_tags(alforno_ctx *ctx, const char **tags, u64 count) {
    u64 i;
    char **arr;
    if (!ctx) return 1;
    if (count > 0 && !tags) return 2;

    /* Replace any prior set. */
    for (i = 0; i < ctx->tag_count; i++) free(ctx->tags[i]);
    free(ctx->tags);
    ctx->tags = NULL;
    ctx->tag_count = 0;

    if (count == 0) return 0;

    arr = (char **)calloc(count, sizeof(char *));
    if (!arr) return 3;
    for (i = 0; i < count; i++) {
        u64 n = (u64)strlen(tags[i]);
        arr[i] = dup_cstr_n(tags[i], n);
        if (!arr[i]) {
            u64 j;
            for (j = 0; j < i; j++) free(arr[j]);
            free(arr);
            return 3;
        }
    }
    ctx->tags = arr;
    ctx->tag_count = count;
    return 0;
}

unsigned long alforno_set_recipe(alforno_ctx *ctx, pasta_value *recipe) {
    if (!ctx) return 1;
    if (!recipe) return 2;
    if (!is_map(recipe)) return 3;
    pasta_value_destroy(ctx->recipe);
    ctx->recipe = recipe;
    return 0;
}

unsigned long alforno_add_input(alforno_ctx *ctx, pasta_value *pastlet) {
    if (!ctx) return 1;
    if (!pastlet) return 2;
    if (!is_map(pastlet)) return 3;

    if (ctx->input_count >= ctx->input_cap) {
        u64 new_cap = ctx->input_cap ? ctx->input_cap * 2 : 4;
        pasta_value **new_arr = (pasta_value **)realloc(ctx->inputs,
                                                          new_cap * sizeof(*new_arr));
        if (!new_arr) return 4;
        ctx->inputs = new_arr;
        ctx->input_cap = new_cap;
    }
    ctx->inputs[ctx->input_count++] = pastlet;
    return 0;
}

/* ================================================================
 *  Pass 1 — parameterize.
 *
 *  Collect @vars maps from all inputs (last-write-wins merge), then
 *  walk every string value and substitute {name} tokens against
 *  the var table. Unresolved variables are hard errors.
 * ================================================================ */

/* Look up a variable by name in the @vars map. Returns NULL if
 * absent. */
static pasta_value *vars_lookup(pasta_value *vars, const u8 *name, u64 nlen) {
    u64 i;
    u64 n;
    if (!vars) return NULL;
    if (pasta_count(&n, vars)) return NULL;
    for (i = 0; i < n; i++) {
        const u8 *k;
        u64 kl;
        if (pasta_map_key(&k, &kl, vars, i)) continue;
        if (kl == nlen && memcmp(k, name, nlen) == 0) {
            pasta_value *v;
            if (pasta_map_value(&v, vars, i)) return NULL;
            return v;
        }
    }
    return NULL;
}

/* Render a pasta_value as a string for interpolation — scalars
 * only; containers aren't legal interpolation targets. */
static unsigned long render_scalar(u8 **out, u64 *out_len,
                                     const pasta_value *v) {
    char tmp[64];
    int n;
    pasta_kind k;
    pasta_kind_of(&k, v);
    switch (k) {
        case PASTA_STRING: {
            const u8 *p; u64 len;
            if (pasta_as_string(&p, &len, v)) return 1;
            *out = (u8 *)malloc(len + 1);
            if (!*out) return 2;
            if (len) memcpy(*out, p, len);
            (*out)[len] = 0;
            *out_len = len;
            return 0;
        }
        case PASTA_INTEGER: {
            i64 iv;
            if (pasta_as_integer(&iv, v)) return 1;
            n = snprintf(tmp, sizeof(tmp), "%lld", (long long)iv);
            goto copy;
        }
        case PASTA_NUMBER: {
            f64 dv;
            if (pasta_as_number(&dv, v)) return 1;
            n = snprintf(tmp, sizeof(tmp), "%g", dv);
            goto copy;
        }
        case PASTA_BOOL: {
            int b = 0;
            pasta_as_bool(&b, v);
            n = snprintf(tmp, sizeof(tmp), "%s", b ? "true" : "false");
            goto copy;
        }
        case PASTA_NULL:
            n = snprintf(tmp, sizeof(tmp), "null");
            goto copy;
        default:
            return 3;
    }
copy:
    if (n < 0 || (size_t)n >= sizeof(tmp)) return 4;
    *out = (u8 *)malloc((u64)n + 1);
    if (!*out) return 2;
    memcpy(*out, tmp, (u64)n);
    (*out)[n] = 0;
    *out_len = (u64)n;
    return 0;
}

/* Substitute {var} tokens in a string value. Returns a new pasta
 * STRING on success; *out_errored is set if any var is unresolved. */
static unsigned long subst_string(pasta_value **out,
                                    const pasta_value *src,
                                    pasta_value *vars,
                                    int *out_errored) {
    const u8 *sp;
    u64 sl;
    u8 *buf = NULL;
    u64 cap = 0, n = 0;
    u64 i;
    unsigned long rc;

    rc = pasta_as_string(&sp, &sl, src);
    if (rc) return rc;

    for (i = 0; i < sl; i++) {
        /* Handle literal {{ and }} escapes by consuming the double. */
        if (sp[i] == '{' && i + 1 < sl && sp[i+1] == '{') {
            if (n + 1 > cap) {
                u64 nc = cap ? cap * 2 : 16;
                u8 *nb = (u8 *)realloc(buf, nc + 1);
                if (!nb) { free(buf); return 2; }
                buf = nb; cap = nc;
            }
            buf[n++] = '{';
            i++;
            continue;
        }
        if (sp[i] == '}' && i + 1 < sl && sp[i+1] == '}') {
            if (n + 1 > cap) {
                u64 nc = cap ? cap * 2 : 16;
                u8 *nb = (u8 *)realloc(buf, nc + 1);
                if (!nb) { free(buf); return 2; }
                buf = nb; cap = nc;
            }
            buf[n++] = '}';
            i++;
            continue;
        }

        if (sp[i] == '{') {
            u64 j = i + 1;
            u64 end;
            pasta_value *got;
            u8 *rendered = NULL;
            u64 rlen = 0;

            while (j < sl && sp[j] != '}') j++;
            if (j >= sl) {
                /* No closing brace — pass through as literal. */
                if (n + 1 > cap) {
                    u64 nc = cap ? cap * 2 : 16;
                    u8 *nb = (u8 *)realloc(buf, nc + 1);
                    if (!nb) { free(buf); return 2; }
                    buf = nb; cap = nc;
                }
                buf[n++] = sp[i];
                continue;
            }
            end = j;

            got = vars_lookup(vars, sp + i + 1, end - (i + 1));
            if (!got) {
                /* Signal unresolved-var to the caller via the flag;
                 * the actual hatch-6 decision is made at the outer
                 * process() level so pass tracking stays correct. */
                *out_errored = 1;
                free(buf);
                /* Return 0 here — the err flag communicates the
                 * failure, and alforno_process translates it to the
                 * UNRESOLVED_VAR hatch. */
                return pasta_new_string(out, "");
            }
            rc = render_scalar(&rendered, &rlen, got);
            if (rc) { free(buf); return rc; }

            if (n + rlen > cap) {
                u64 nc = cap ? cap * 2 : 16;
                while (nc < n + rlen) nc *= 2;
                {
                    u8 *nb = (u8 *)realloc(buf, nc + 1);
                    if (!nb) { free(rendered); free(buf); return 2; }
                    buf = nb; cap = nc;
                }
            }
            if (rlen) memcpy(buf + n, rendered, rlen);
            n += rlen;
            free(rendered);

            i = end;
            continue;
        }

        if (n + 1 > cap) {
            u64 nc = cap ? cap * 2 : 16;
            u8 *nb = (u8 *)realloc(buf, nc + 1);
            if (!nb) { free(buf); return 2; }
            buf = nb; cap = nc;
        }
        buf[n++] = sp[i];
    }

    if (!buf) {
        buf = (u8 *)malloc(1);
        if (!buf) return 2;
    }
    buf[n] = 0;

    rc = pasta_new_string_n(out, buf, n);
    free(buf);
    return rc;
}

/* Recursively parameterize a value tree in place (returns a new
 * tree; caller destroys src if desired). */
static unsigned long parameterize(pasta_value **out,
                                    const pasta_value *src,
                                    pasta_value *vars,
                                    int *out_errored) {
    pasta_kind k;
    unsigned long rc;

    rc = pasta_kind_of(&k, src);
    if (rc) return rc;

    if (k == PASTA_STRING) {
        return subst_string(out, src, vars, out_errored);
    }
    if (k == PASTA_MAP) {
        pasta_value *m = NULL;
        u64 i, n;
        rc = pasta_new_map(&m);
        if (rc) return rc;
        if ((rc = pasta_count(&n, src))) { pasta_value_destroy(m); return rc; }
        for (i = 0; i < n; i++) {
            const u8 *key; u64 klen;
            pasta_value *val, *new_val = NULL;
            if ((rc = pasta_map_key(&key, &klen, src, i))) goto fail_m;
            if ((rc = pasta_map_value(&val, src, i))) goto fail_m;
            rc = parameterize(&new_val, val, vars, out_errored);
            if (rc) goto fail_m;
            rc = pasta_map_put_n(m, key, klen, new_val);
            if (rc) { pasta_value_destroy(new_val); goto fail_m; }
        }
        *out = m;
        return 0;
    fail_m:
        pasta_value_destroy(m);
        return rc;
    }
    if (k == PASTA_ARRAY) {
        pasta_value *a = NULL;
        u64 i, n;
        rc = pasta_new_array(&a);
        if (rc) return rc;
        if ((rc = pasta_count(&n, src))) { pasta_value_destroy(a); return rc; }
        for (i = 0; i < n; i++) {
            pasta_value *elt, *new_elt = NULL;
            if ((rc = pasta_array_at(&elt, src, i))) goto fail_a;
            rc = parameterize(&new_elt, elt, vars, out_errored);
            if (rc) goto fail_a;
            rc = pasta_array_push(a, new_elt);
            if (rc) { pasta_value_destroy(new_elt); goto fail_a; }
        }
        *out = a;
        return 0;
    fail_a:
        pasta_value_destroy(a);
        return rc;
    }
    /* Scalars clone as-is. */
    return pasta_value_clone(out, src);
}

/* ================================================================
 *  Pass 1.5 — when-filter.
 *
 *  A section whose map contains a `when:` key (string or array of
 *  strings) is kept only if at least one of its tags matches an
 *  active tag. Sections without `when` pass through. The `when`
 *  key is stripped before output.
 * ================================================================ */

static int string_eq(const pasta_value *v, const char *s) {
    const u8 *p;
    u64 n;
    u64 sl = (u64)strlen(s);
    if (pasta_as_string(&p, &n, v)) return 0;
    return n == sl && memcmp(p, s, sl) == 0;
}

static int has_tag(alforno_ctx *ctx, const char *s) {
    u64 i;
    u64 sl = (u64)strlen(s);
    for (i = 0; i < ctx->tag_count; i++) {
        if (strlen(ctx->tags[i]) == sl && memcmp(ctx->tags[i], s, sl) == 0) {
            return 1;
        }
    }
    return 0;
}

static int when_accepts(alforno_ctx *ctx, const pasta_value *when_val) {
    pasta_kind k;
    u64 i, n;
    pasta_value *elt;

    pasta_kind_of(&k, when_val);
    if (k == PASTA_STRING) {
        const u8 *p; u64 sl;
        char buf[256];
        if (pasta_as_string(&p, &sl, when_val)) return 0;
        if (sl >= sizeof(buf)) return 0;
        memcpy(buf, p, sl); buf[sl] = 0;
        return has_tag(ctx, buf);
    }
    if (k == PASTA_ARRAY) {
        pasta_count(&n, when_val);
        for (i = 0; i < n; i++) {
            pasta_array_at(&elt, when_val, i);
            {
                pasta_kind ek; pasta_kind_of(&ek, elt);
                if (ek != PASTA_STRING) continue;
                {
                    const u8 *p; u64 sl;
                    char buf[256];
                    if (pasta_as_string(&p, &sl, elt)) continue;
                    if (sl >= sizeof(buf)) continue;
                    memcpy(buf, p, sl); buf[sl] = 0;
                    if (has_tag(ctx, buf)) return 1;
                }
            }
        }
        return 0;
    }
    /* Unknown when type — reject defensively. */
    return 0;
}

/* Remove sections whose when-tags don't match. Also strips the
 * `when` key from accepted sections. Modifies `doc` in place. */
static unsigned long when_filter(alforno_ctx *ctx, pasta_value *doc) {
    u64 i;

    if (ctx->tag_count == 0) {
        /* When no tags are set, any when-guarded section is filtered
         * out (per spec: "If no tags are set, all when-guarded
         * sections are excluded"). */
    }

    /* Iterate in reverse so removing entries doesn't skip. */
    i = doc->u.map.count;
    while (i > 0) {
        pasta_value *sec;
        pasta_value *when_val = NULL;
        i--;
        sec = doc->u.map.vals[i];
        if (!is_map(sec)) continue;
        pasta_map_get(&when_val, sec, "when");
        if (!when_val) continue;

        if (!when_accepts(ctx, when_val)) {
            /* Drop this section. */
            pasta_value_destroy(sec);
            free(doc->u.map.keys[i]);
            {
                u64 j;
                for (j = i + 1; j < doc->u.map.count; j++) {
                    doc->u.map.keys[j - 1] = doc->u.map.keys[j];
                    doc->u.map.key_lens[j - 1] = doc->u.map.key_lens[j];
                    doc->u.map.vals[j - 1] = doc->u.map.vals[j];
                }
            }
            doc->u.map.count--;
        } else {
            /* Strip the when key from the accepted section. */
            u64 j;
            for (j = 0; j < sec->u.map.count; j++) {
                if (sec->u.map.key_lens[j] == 4
                    && memcmp(sec->u.map.keys[j], "when", 4) == 0) {
                    free(sec->u.map.keys[j]);
                    pasta_value_destroy(sec->u.map.vals[j]);
                    {
                        u64 k;
                        for (k = j + 1; k < sec->u.map.count; k++) {
                            sec->u.map.keys[k - 1] = sec->u.map.keys[k];
                            sec->u.map.key_lens[k - 1] = sec->u.map.key_lens[k];
                            sec->u.map.vals[k - 1] = sec->u.map.vals[k];
                        }
                    }
                    sec->u.map.count--;
                    break;
                }
            }
        }
    }
    return 0;
}

/* ================================================================
 *  Merge
 *
 *  For aggregate + conflate + gather (last-wins) we walk input
 *  sections in order, putting each into the output map. Because
 *  pasta_map_put replaces on duplicate key, this naturally yields
 *  last-write-wins semantics for top-level sections and for keys
 *  within a section (when we descend one level).
 *
 *  For gather first-found, we skip puts for keys already present.
 * ================================================================ */

static int map_has_key(const pasta_value *m, const u8 *key, u64 klen) {
    u64 i;
    for (i = 0; i < m->u.map.count; i++) {
        if (m->u.map.key_lens[i] == klen
            && memcmp(m->u.map.keys[i], key, klen) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Map-level merge: overlay `src` into `dst`. LWW by default;
 * first-found when `first_found`. */
static unsigned long merge_map_into(pasta_value *dst, const pasta_value *src,
                                      int first_found) {
    u64 i, n;
    unsigned long rc;
    rc = pasta_count(&n, src);
    if (rc) return rc;
    for (i = 0; i < n; i++) {
        const u8 *key;
        u64 klen;
        pasta_value *val, *cloned = NULL;
        rc = pasta_map_key(&key, &klen, src, i); if (rc) return rc;
        rc = pasta_map_value(&val, src, i);      if (rc) return rc;
        if (first_found && map_has_key(dst, key, klen)) continue;

        rc = pasta_value_clone(&cloned, val); if (rc) return rc;
        rc = pasta_map_put_n(dst, key, klen, cloned);
        if (rc) { pasta_value_destroy(cloned); return rc; }
    }
    return 0;
}

/* ---- "collect" merge strategy ---- *
 * Colliding keys coalesce into an array. If both sides are already
 * arrays, they concat rather than nest. */
static unsigned long merge_map_collect(pasta_value *dst,
                                         const pasta_value *src) {
    u64 i, n;
    unsigned long rc;
    rc = pasta_count(&n, src);
    if (rc) return rc;
    for (i = 0; i < n; i++) {
        const u8 *key; u64 klen;
        pasta_value *val, *existing = NULL;
        pasta_kind ek, vk;
        rc = pasta_map_key(&key, &klen, src, i); if (rc) return rc;
        rc = pasta_map_value(&val, src, i);      if (rc) return rc;

        {
            u64 j;
            for (j = 0; j < dst->u.map.count; j++) {
                if (dst->u.map.key_lens[j] == klen
                    && memcmp(dst->u.map.keys[j], key, klen) == 0) {
                    existing = dst->u.map.vals[j];
                    break;
                }
            }
        }

        if (!existing) {
            pasta_value *cloned = NULL;
            rc = pasta_value_clone(&cloned, val); if (rc) return rc;
            rc = pasta_map_put_n(dst, key, klen, cloned);
            if (rc) { pasta_value_destroy(cloned); return rc; }
            continue;
        }

        pasta_kind_of(&ek, existing);
        pasta_kind_of(&vk, val);

        if (ek == PASTA_ARRAY && vk == PASTA_ARRAY) {
            /* Concat. */
            u64 m, a;
            pasta_count(&m, val);
            for (a = 0; a < m; a++) {
                pasta_value *elt, *elt_clone = NULL;
                pasta_array_at(&elt, val, a);
                rc = pasta_value_clone(&elt_clone, elt); if (rc) return rc;
                rc = pasta_array_push(existing, elt_clone);
                if (rc) { pasta_value_destroy(elt_clone); return rc; }
            }
            continue;
        }

        if (ek == PASTA_ARRAY) {
            /* Append new value to the array. */
            pasta_value *elt_clone = NULL;
            rc = pasta_value_clone(&elt_clone, val); if (rc) return rc;
            rc = pasta_array_push(existing, elt_clone);
            if (rc) { pasta_value_destroy(elt_clone); return rc; }
            continue;
        }

        /* First collision: wrap existing into a new array [existing, new]. */
        {
            pasta_value *arr = NULL;
            pasta_value *old_clone = NULL;
            pasta_value *new_clone = NULL;
            rc = pasta_new_array(&arr);               if (rc) return rc;
            rc = pasta_value_clone(&old_clone, existing);
            if (rc) { pasta_value_destroy(arr); return rc; }
            rc = pasta_array_push(arr, old_clone);
            if (rc) { pasta_value_destroy(old_clone); pasta_value_destroy(arr); return rc; }
            rc = pasta_value_clone(&new_clone, val);
            if (rc) { pasta_value_destroy(arr); return rc; }
            rc = pasta_array_push(arr, new_clone);
            if (rc) { pasta_value_destroy(new_clone); pasta_value_destroy(arr); return rc; }
            rc = pasta_map_put_n(dst, key, klen, arr);   /* replaces existing */
            if (rc) { pasta_value_destroy(arr); return rc; }
        }
    }
    return 0;
}

/* ---- "deep" merge strategy ---- *
 * Recurse into maps; non-map collisions are LWW; arrays replaced. */
static unsigned long merge_map_deep(pasta_value *dst, const pasta_value *src) {
    u64 i, n;
    unsigned long rc;
    rc = pasta_count(&n, src);
    if (rc) return rc;
    for (i = 0; i < n; i++) {
        const u8 *key; u64 klen;
        pasta_value *val, *existing = NULL;
        pasta_kind ek, vk;
        rc = pasta_map_key(&key, &klen, src, i); if (rc) return rc;
        rc = pasta_map_value(&val, src, i);      if (rc) return rc;

        {
            u64 j;
            for (j = 0; j < dst->u.map.count; j++) {
                if (dst->u.map.key_lens[j] == klen
                    && memcmp(dst->u.map.keys[j], key, klen) == 0) {
                    existing = dst->u.map.vals[j];
                    break;
                }
            }
        }

        if (!existing) {
            pasta_value *cloned = NULL;
            rc = pasta_value_clone(&cloned, val); if (rc) return rc;
            rc = pasta_map_put_n(dst, key, klen, cloned);
            if (rc) { pasta_value_destroy(cloned); return rc; }
            continue;
        }

        pasta_kind_of(&ek, existing);
        pasta_kind_of(&vk, val);

        if (ek == PASTA_MAP && vk == PASTA_MAP) {
            rc = merge_map_deep(existing, val);
            if (rc) return rc;
            continue;
        }
        /* Non-map: LWW. */
        {
            pasta_value *cloned = NULL;
            rc = pasta_value_clone(&cloned, val); if (rc) return rc;
            rc = pasta_map_put_n(dst, key, klen, cloned);
            if (rc) { pasta_value_destroy(cloned); return rc; }
        }
    }
    return 0;
}

/* Strategy dispatcher. */
typedef enum {
    STRAT_REPLACE,
    STRAT_COLLECT,
    STRAT_DEEP
} merge_strategy;

static unsigned long merge_map_by_strategy(pasta_value *dst,
                                             const pasta_value *src,
                                             merge_strategy strat) {
    switch (strat) {
        case STRAT_REPLACE: return merge_map_into(dst, src, 0);
        case STRAT_COLLECT: return merge_map_collect(dst, src);
        case STRAT_DEEP:    return merge_map_deep(dst, src);
    }
    return 99;
}

/* Read a recipe section's merge strategy. Returns STRAT_REPLACE on
 * missing/unknown (our default). Sets *unknown=1 if a value was
 * present but unrecognized. */
static merge_strategy read_strategy(const pasta_value *recipe_sec,
                                      int *unknown) {
    pasta_value *mv = NULL;
    const u8 *p; u64 n;
    if (unknown) *unknown = 0;
    if (!recipe_sec) return STRAT_REPLACE;
    if (pasta_map_get(&mv, recipe_sec, "merge")) return STRAT_REPLACE;
    if (pasta_as_string(&p, &n, mv)) return STRAT_REPLACE;
    if (n == 7 && memcmp(p, "replace", 7) == 0) return STRAT_REPLACE;
    if (n == 7 && memcmp(p, "collect", 7) == 0) return STRAT_COLLECT;
    if (n == 4 && memcmp(p, "deep",    4) == 0) return STRAT_DEEP;
    if (unknown) *unknown = 1;
    return STRAT_REPLACE;
}

/* Aggregate/gather: merge all inputs into a fresh map. For each
 * section present in multiple inputs, recurse into the section's
 * fields with the same precedence so per-field LWW works too. */
static unsigned long op_merge(pasta_value **out,
                                alforno_ctx *ctx,
                                pasta_value **inputs, u64 n,
                                int first_found) {
    pasta_value *m = NULL;
    unsigned long rc;
    u64 i, j, sn;

    rc = pasta_new_map(&m);
    if (rc) return rc;

    for (i = 0; i < n; i++) {
        pasta_value *in = inputs[i];
        rc = pasta_count(&sn, in); if (rc) goto fail;
        for (j = 0; j < sn; j++) {
            const u8 *key;
            u64 klen;
            pasta_value *sec;
            pasta_value *existing = NULL;

            rc = pasta_map_key(&key, &klen, in, j); if (rc) goto fail;
            rc = pasta_map_value(&sec, in, j);      if (rc) goto fail;

            /* Skip reserved sections from appearing in output. */
            if ((klen == 4 && memcmp(key, "vars", 4) == 0)
                || (klen == 7 && memcmp(key, "include", 7) == 0)) {
                continue;
            }

            {
                u64 ei;
                for (ei = 0; ei < m->u.map.count; ei++) {
                    if (m->u.map.key_lens[ei] == klen
                        && memcmp(m->u.map.keys[ei], key, klen) == 0) {
                        existing = m->u.map.vals[ei];
                        break;
                    }
                }
            }

            if (!existing) {
                pasta_value *cloned = NULL;
                rc = pasta_value_clone(&cloned, sec); if (rc) goto fail;
                rc = pasta_map_put_n(m, key, klen, cloned);
                if (rc) { pasta_value_destroy(cloned); goto fail; }
                continue;
            }

            if (is_map(existing) && is_map(sec)) {
                rc = merge_map_into(existing, sec, first_found);
                if (rc) goto fail;
            } else if (!first_found) {
                pasta_value *cloned = NULL;
                rc = pasta_value_clone(&cloned, sec); if (rc) goto fail;
                rc = pasta_map_put_n(m, key, klen, cloned);
                if (rc) { pasta_value_destroy(cloned); goto fail; }
            }
            /* first_found + scalar collision: keep existing. */
        }
    }

    *out = m;
    return 0;
fail:
    pasta_value_destroy(m);
    return rc;
}

/* ---- Recipe descriptor parsing (pass 4 type validation) ----
 *
 * Per the spec: descriptor strings matching
 *   "required <type>" / "optional <type>"
 *   "required"        / "optional"
 * enable validation. Types: string, number, bool, array, map.
 * Anything else is informational and ignored. */

typedef enum {
    DESC_NONE = 0,      /* not a validation descriptor */
    DESC_REQUIRED,
    DESC_OPTIONAL
} desc_mode;

typedef enum {
    DESC_TY_ANY = 0,
    DESC_TY_STRING,
    DESC_TY_NUMBER,
    DESC_TY_BOOL,
    DESC_TY_ARRAY,
    DESC_TY_MAP
} desc_type;

/* Parse a descriptor from a pasta_value. Returns DESC_NONE if the
 * value isn't a validation descriptor string. */
static desc_mode parse_descriptor(const pasta_value *v, desc_type *out_ty) {
    const u8 *p; u64 n;
    const u8 *word_start;
    u64 mode_len;
    desc_mode mode;
    u64 i;

    *out_ty = DESC_TY_ANY;
    if (!v) return DESC_NONE;
    if (pasta_as_string(&p, &n, v)) return DESC_NONE;

    if (n >= 8 && memcmp(p, "required", 8) == 0) {
        mode = DESC_REQUIRED;
        mode_len = 8;
    } else if (n >= 8 && memcmp(p, "optional", 8) == 0) {
        mode = DESC_OPTIONAL;
        mode_len = 8;
    } else {
        return DESC_NONE;
    }

    /* Exact "required" / "optional" with no type qualifier. */
    if (n == mode_len) return mode;

    /* Must be followed by a single space + type word. */
    if (p[mode_len] != ' ') return DESC_NONE;

    word_start = p + mode_len + 1;
    i = mode_len + 1;
    /* Skip any extra spaces. */
    while (i < n && p[i] == ' ') { word_start = p + i + 1; i++; }
    if (i >= n) return mode;      /* "required " (trailing spaces) */

    {
        u64 type_len = n - i;
        /* Trim trailing whitespace. */
        while (type_len > 0 && word_start[type_len - 1] == ' ') type_len--;
        if (type_len == 6 && memcmp(word_start, "string", 6) == 0) *out_ty = DESC_TY_STRING;
        else if (type_len == 6 && memcmp(word_start, "number", 6) == 0) *out_ty = DESC_TY_NUMBER;
        else if (type_len == 4 && memcmp(word_start, "bool",   4) == 0) *out_ty = DESC_TY_BOOL;
        else if (type_len == 5 && memcmp(word_start, "array",  5) == 0) *out_ty = DESC_TY_ARRAY;
        else if (type_len == 3 && memcmp(word_start, "map",    3) == 0) *out_ty = DESC_TY_MAP;
        /* Unknown type -> descriptor becomes purely presence check. */
    }
    return mode;
}

static int type_matches(const pasta_value *v, desc_type ty) {
    pasta_kind k;
    if (ty == DESC_TY_ANY) return 1;
    if (!v) return 0;
    if (pasta_kind_of(&k, v)) return 0;
    switch (ty) {
        case DESC_TY_STRING: return k == PASTA_STRING;
        case DESC_TY_NUMBER: return k == PASTA_INTEGER || k == PASTA_NUMBER;
        case DESC_TY_BOOL:   return k == PASTA_BOOL;
        case DESC_TY_ARRAY:  return k == PASTA_ARRAY;
        case DESC_TY_MAP:    return k == PASTA_MAP;
        default: return 0;
    }
}

/* Conflate: walk recipe; for each recipe section, collect all input
 * sections it `consumes` (default: the section itself), merge them
 * by strategy, then drop fields absent from the recipe.
 *
 * Recipe reserved keys per section: `consumes`, `merge`.
 * Any other key in the recipe section is a whitelisted output
 * field. */
static unsigned long op_conflate(pasta_value **out,
                                   alforno_ctx *ctx,
                                   pasta_value **inputs, u64 n) {
    pasta_value *result = NULL;
    pasta_value *rec = ctx->recipe;
    unsigned long rc;
    u64 ri;

    if (!rec) {
        /* No recipe -> degrade to aggregate. */
        return op_merge(out, ctx, inputs, n, 0);
    }

    rc = pasta_new_map(&result);
    if (rc) return rc;

    for (ri = 0; ri < rec->u.map.count; ri++) {
        const u8 *out_name = rec->u.map.keys[ri];
        u64 out_name_len = rec->u.map.key_lens[ri];
        pasta_value *recipe_sec = rec->u.map.vals[ri];
        pasta_value *accum = NULL;
        merge_strategy strat;
        int unknown = 0;
        pasta_value *consumes = NULL;
        u64 ii;

        if (!is_map(recipe_sec)) continue;

        strat = read_strategy(recipe_sec, &unknown);
        if (unknown) { pasta_value_destroy(result); return 8; }

        /* Per alforno spec, every conflate recipe section MUST
         * declare a `consumes` key. Treat its absence as a hard
         * error (hatch 9) rather than silently defaulting to the
         * same-named section. */
        (void)pasta_map_get(&consumes, recipe_sec, "consumes");
        if (!consumes) {
            pasta_value_destroy(result);
            return 9;
        }

        rc = pasta_new_map(&accum);
        if (rc) { pasta_value_destroy(result); return rc; }

        /* Walk inputs in declaration order, pulling each consumed
         * section into the accumulator using the strategy. */
        for (ii = 0; ii < n; ii++) {
            pasta_value *in = inputs[ii];
            if (!is_map(in)) continue;

            if (consumes && is_array(consumes)) {
                u64 ci, cn;
                pasta_count(&cn, consumes);
                for (ci = 0; ci < cn; ci++) {
                    pasta_value *nv = NULL;
                    const u8 *cname; u64 clen;
                    char key_cstr[256];
                    pasta_value *sec = NULL;
                    pasta_array_at(&nv, consumes, ci);
                    if (!nv) continue;
                    if (pasta_as_string(&cname, &clen, nv)) continue;
                    if (clen >= sizeof(key_cstr)) continue;
                    memcpy(key_cstr, cname, clen); key_cstr[clen] = 0;
                    if (pasta_map_get(&sec, in, key_cstr) == 0
                        && sec && is_map(sec)) {
                        rc = merge_map_by_strategy(accum, sec, strat);
                        if (rc) {
                            pasta_value_destroy(accum);
                            pasta_value_destroy(result);
                            return rc;
                        }
                    }
                }
            } else {
                /* Default: consume section with the same name. */
                char key_cstr[256];
                pasta_value *sec = NULL;
                if (out_name_len >= sizeof(key_cstr)) continue;
                memcpy(key_cstr, out_name, out_name_len);
                key_cstr[out_name_len] = 0;
                if (pasta_map_get(&sec, in, key_cstr) == 0
                    && sec && is_map(sec)) {
                    rc = merge_map_by_strategy(accum, sec, strat);
                    if (rc) {
                        pasta_value_destroy(accum);
                        pasta_value_destroy(result);
                        return rc;
                    }
                }
            }
        }

        /* Drop fields in accum that aren't whitelisted by recipe_sec. */
        {
            u64 fi = accum->u.map.count;
            while (fi > 0) {
                u64 rk;
                int present = 0;
                u8 *fk;
                u64 fkl;
                fi--;
                fk = accum->u.map.keys[fi];
                fkl = accum->u.map.key_lens[fi];
                for (rk = 0; rk < recipe_sec->u.map.count; rk++) {
                    u64 rkl = recipe_sec->u.map.key_lens[rk];
                    u8 *rkey = recipe_sec->u.map.keys[rk];
                    if ((rkl == 8 && memcmp(rkey, "consumes", 8) == 0)
                        || (rkl == 5 && memcmp(rkey, "merge",    5) == 0)) {
                        continue;
                    }
                    if (rkl == fkl && memcmp(rkey, fk, fkl) == 0) {
                        present = 1; break;
                    }
                }
                if (!present) {
                    pasta_value_destroy(accum->u.map.vals[fi]);
                    free(accum->u.map.keys[fi]);
                    {
                        u64 j;
                        for (j = fi + 1; j < accum->u.map.count; j++) {
                            accum->u.map.keys[j - 1] = accum->u.map.keys[j];
                            accum->u.map.key_lens[j - 1] = accum->u.map.key_lens[j];
                            accum->u.map.vals[j - 1] = accum->u.map.vals[j];
                        }
                    }
                    accum->u.map.count--;
                }
            }
        }

        /* Pass 4 validation: for each non-reserved recipe key whose
         * value is a descriptor string, check presence + type. */
        {
            u64 ri_f;
            for (ri_f = 0; ri_f < recipe_sec->u.map.count; ri_f++) {
                const u8 *rk = recipe_sec->u.map.keys[ri_f];
                u64 rkl = recipe_sec->u.map.key_lens[ri_f];
                pasta_value *rv = recipe_sec->u.map.vals[ri_f];
                desc_type ty;
                desc_mode mode;

                if ((rkl == 8 && memcmp(rk, "consumes", 8) == 0)
                    || (rkl == 5 && memcmp(rk, "merge",    5) == 0)) {
                    continue;
                }

                mode = parse_descriptor(rv, &ty);
                if (mode == DESC_NONE) continue;

                /* Lookup field in accum. */
                {
                    pasta_value *fv = NULL;
                    u64 ai;
                    for (ai = 0; ai < accum->u.map.count; ai++) {
                        if (accum->u.map.key_lens[ai] == rkl
                            && memcmp(accum->u.map.keys[ai], rk, rkl) == 0) {
                            fv = accum->u.map.vals[ai];
                            break;
                        }
                    }
                    if (!fv) {
                        if (mode == DESC_REQUIRED) {
                            pasta_value_destroy(accum);
                            pasta_value_destroy(result);
                            return 20;   /* required field missing */
                        }
                        continue;
                    }
                    if (!type_matches(fv, ty)) {
                        pasta_value_destroy(accum);
                        pasta_value_destroy(result);
                        return 21;       /* type mismatch */
                    }
                }
            }
        }

        rc = pasta_map_put_n(result, out_name, out_name_len, accum);
        if (rc) { pasta_value_destroy(accum); pasta_value_destroy(result); return rc; }
    }

    *out = result;
    return 0;
}

/* Scatter: produce a map { section_name -> pastlet containing that
 * single section }. Useful for "give me each section as its own
 * document". */
static unsigned long op_scatter(pasta_value **out,
                                  alforno_ctx *ctx,
                                  pasta_value **inputs, u64 n) {
    pasta_value *result = NULL;
    unsigned long rc;
    u64 i, j, sn;
    (void)ctx;

    rc = pasta_new_map(&result);
    if (rc) return rc;

    for (i = 0; i < n; i++) {
        pasta_value *in = inputs[i];
        rc = pasta_count(&sn, in); if (rc) goto fail;
        for (j = 0; j < sn; j++) {
            const u8 *key; u64 klen;
            pasta_value *sec, *sec_clone = NULL;
            pasta_value *single = NULL;

            rc = pasta_map_key(&key, &klen, in, j); if (rc) goto fail;
            rc = pasta_map_value(&sec, in, j);      if (rc) goto fail;

            rc = pasta_value_clone(&sec_clone, sec); if (rc) goto fail;

            /* Wrap in a one-section map (the scatter "pastlet"). */
            rc = pasta_new_map(&single); if (rc) { pasta_value_destroy(sec_clone); goto fail; }
            rc = pasta_map_put_n(single, key, klen, sec_clone);
            if (rc) { pasta_value_destroy(sec_clone); pasta_value_destroy(single); goto fail; }

            rc = pasta_map_put_n(result, key, klen, single);
            if (rc) { pasta_value_destroy(single); goto fail; }
        }
    }
    *out = result;
    return 0;
fail:
    pasta_value_destroy(result);
    return rc;
}

/* ================================================================
 *  Pass 3 — link resolution.
 *
 *  Walk the output tree; for each label-ref, look up the named
 *  section in the output map (or, failing that, in any input) and
 *  replace the label-ref value with a clone of the target's content.
 * ================================================================ */

static pasta_value *find_section(pasta_value *root, pasta_value **inputs,
                                   u64 input_count,
                                   const u8 *name, u64 nlen) {
    /* Lookup in output root first. */
    if (root && is_map(root)) {
        u64 i;
        for (i = 0; i < root->u.map.count; i++) {
            if (root->u.map.key_lens[i] == nlen
                && memcmp(root->u.map.keys[i], name, nlen) == 0) {
                return root->u.map.vals[i];
            }
        }
    }
    /* Fall back to inputs. */
    {
        u64 k;
        for (k = 0; k < input_count; k++) {
            pasta_value *in = inputs[k];
            u64 i;
            if (!is_map(in)) continue;
            for (i = 0; i < in->u.map.count; i++) {
                if (in->u.map.key_lens[i] == nlen
                    && memcmp(in->u.map.keys[i], name, nlen) == 0) {
                    return in->u.map.vals[i];
                }
            }
        }
    }
    return NULL;
}

static unsigned long resolve_links_depth(pasta_value *node,
                                            pasta_value *root,
                                            pasta_value **inputs,
                                            u64 input_count,
                                            int *out_missing,
                                            u32 depth);

static unsigned long resolve_links(pasta_value *node,
                                     pasta_value *root,
                                     pasta_value **inputs,
                                     u64 input_count,
                                     int *out_missing) {
    return resolve_links_depth(node, root, inputs, input_count,
                                 out_missing, 0);
}

#define ALF_MAX_LINK_DEPTH 32

static unsigned long resolve_links_depth(pasta_value *node,
                                            pasta_value *root,
                                            pasta_value **inputs,
                                            u64 input_count,
                                            int *out_missing,
                                            u32 depth) {
    pasta_kind k;
    u64 i, n;
    unsigned long rc;

    if (depth > ALF_MAX_LINK_DEPTH) return 10;   /* cycle / too-deep */

    pasta_kind_of(&k, node);

    if (k == PASTA_MAP) {
        pasta_count(&n, node);
        for (i = 0; i < n; i++) {
            pasta_value *val = node->u.map.vals[i];
            pasta_kind vk;
            pasta_kind_of(&vk, val);
            if (vk == PASTA_LABEL_REF) {
                pasta_value *target = find_section(root, inputs, input_count,
                                                     val->u.label.name,
                                                     val->u.label.len);
                if (!target) {
                    /* No matching section — keep the label as-is.
                     * Matches sibling alforno's lenient behaviour;
                     * the BNF reserves hard errors for explicitly-
                     * declared references that can never resolve. */
                    continue;
                }
                {
                    pasta_value *cloned = NULL;
                    rc = pasta_value_clone(&cloned, target); if (rc) return rc;
                    pasta_value_destroy(val);
                    node->u.map.vals[i] = cloned;
                    rc = resolve_links_depth(cloned, root, inputs, input_count, out_missing, depth + 1);
                    if (rc) return rc;
                }
            } else {
                rc = resolve_links_depth(val, root, inputs, input_count, out_missing, depth + 1);
                if (rc) return rc;
            }
        }
        return 0;
    }
    if (k == PASTA_ARRAY) {
        pasta_count(&n, node);
        for (i = 0; i < n; i++) {
            pasta_value *elt = node->u.array.items[i];
            pasta_kind ek;
            pasta_kind_of(&ek, elt);
            if (ek == PASTA_LABEL_REF) {
                pasta_value *target = find_section(root, inputs, input_count,
                                                     elt->u.label.name,
                                                     elt->u.label.len);
                if (!target) {
                    /* Same lenient policy as in the map branch above. */
                    continue;
                }
                {
                    pasta_value *cloned = NULL;
                    rc = pasta_value_clone(&cloned, target); if (rc) return rc;
                    pasta_value_destroy(elt);
                    node->u.array.items[i] = cloned;
                    rc = resolve_links_depth(cloned, root, inputs, input_count, out_missing, depth + 1);
                    if (rc) return rc;
                }
            } else {
                rc = resolve_links_depth(elt, root, inputs, input_count, out_missing, depth + 1);
                if (rc) return rc;
            }
        }
        return 0;
    }
    return 0;
}

/* ================================================================
 *  alforno_process — drive the pipeline.
 * ================================================================ */

unsigned long alforno_process(pasta_value **out, alforno_ctx *ctx) {
    pasta_value *merged_vars = NULL;
    pasta_value **params = NULL;
    u64 i;
    unsigned long rc;
    pasta_value *result = NULL;

    if (!out) return 1;
    *out = NULL;
    if (!ctx) return 2;
    if (ctx->input_count == 0) return 3;

    /* ---- collect @vars across all inputs (LWW) ---- */
    rc = pasta_new_map(&merged_vars);
    if (rc) return 4;
    for (i = 0; i < ctx->input_count; i++) {
        pasta_value *in = ctx->inputs[i];
        pasta_value *vars = NULL;
        (void)pasta_map_get(&vars, in, "vars");
        if (vars && is_map(vars)) {
            rc = merge_map_into(merged_vars, vars, 0);
            if (rc) goto cleanup;
        }
    }

    /* ---- parameterize each input ---- */
    params = (pasta_value **)calloc(ctx->input_count, sizeof(pasta_value *));
    if (!params) { rc = 5; goto cleanup; }

    for (i = 0; i < ctx->input_count; i++) {
        int err = 0;
        rc = parameterize(&params[i], ctx->inputs[i], merged_vars, &err);
        if (rc) goto cleanup_params;
        if (err) { rc = 6; goto cleanup_params; }

        /* Strip @vars / @include from the parameterized copy so they
         * don't leak into output. */
        {
            u64 j = params[i]->u.map.count;
            while (j > 0) {
                j--;
                {
                    u8 *k = params[i]->u.map.keys[j];
                    u64 kl = params[i]->u.map.key_lens[j];
                    if ((kl == 4 && memcmp(k, "vars", 4) == 0)
                        || (kl == 7 && memcmp(k, "include", 7) == 0)) {
                        pasta_value_destroy(params[i]->u.map.vals[j]);
                        free(params[i]->u.map.keys[j]);
                        {
                            u64 m;
                            for (m = j + 1; m < params[i]->u.map.count; m++) {
                                params[i]->u.map.keys[m - 1] = params[i]->u.map.keys[m];
                                params[i]->u.map.key_lens[m - 1] = params[i]->u.map.key_lens[m];
                                params[i]->u.map.vals[m - 1] = params[i]->u.map.vals[m];
                            }
                        }
                        params[i]->u.map.count--;
                    }
                }
            }
        }

        rc = when_filter(ctx, params[i]);
        if (rc) goto cleanup_params;
    }

    /* ---- operation-specific merge ---- */
    switch (ctx->op) {
        case ALFORNO_OP_AGGREGATE:
            rc = op_merge(&result, ctx, params, ctx->input_count, 0);
            break;
        case ALFORNO_OP_CONFLATE:
            rc = op_conflate(&result, ctx, params, ctx->input_count);
            break;
        case ALFORNO_OP_GATHER:
            rc = op_merge(&result, ctx, params, ctx->input_count,
                            ctx->precedence == ALFORNO_FIRST_FOUND);
            break;
        case ALFORNO_OP_SCATTER:
            rc = op_scatter(&result, ctx, params, ctx->input_count);
            break;
    }
    if (rc) goto cleanup_params;

    /* ---- Pass 3: link resolution (not on scatter, where the
     * output's shape is a map of single-section pastlets — label
     * refs there are rare and semantically ambiguous). ---- */
    if (ctx->op != ALFORNO_OP_SCATTER) {
        int missing = 0;
        rc = resolve_links(result, result, params, ctx->input_count, &missing);
        if (rc) { if (missing) rc = 7; goto cleanup_result; }
    }

    *out = result;
    rc = 0;

cleanup_result:
    if (rc && result) pasta_value_destroy(result);
cleanup_params:
    if (params) {
        for (i = 0; i < ctx->input_count; i++) pasta_value_destroy(params[i]);
        free(params);
    }
cleanup:
    pasta_value_destroy(merged_vars);
    return rc;
}
