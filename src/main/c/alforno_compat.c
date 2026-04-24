/* Shim for the sibling alforno API over our t4/pasta/alforno. */

#include "apennines/t4/pasta/pasta.h"
#include "apennines/t4/pasta/alforno.h"
#include "apennines/t1/buffer/buf.h"
/* Internal struct access so the shim can strip `include` sections
 * in-place. Not part of the public API but we vendor this header
 * alongside the sources for the compat path. */
#include "pasta_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

/* Sibling types — minimal, parallel to alforno_compat.h but without
 * including that header (to avoid remapping our alforno_* calls via
 * its macros). */
typedef enum {
    TALF_AGGREGATE,
    TALF_CONFLATE,
    TALF_SCATTER,
    TALF_GATHER
} talf_op;

typedef enum {
    TALF_LAST_WINS,
    TALF_FIRST_FOUND
} talf_prec;

typedef enum {
    TALF_OK = 0,
    TALF_ERR_ALLOC,
    TALF_ERR_PARSE,
    TALF_ERR_NOT_SECTIONS,
    TALF_ERR_MISSING_CONSUMES,
    TALF_ERR_BAD_RECIPE,
    TALF_ERR_UNRESOLVED_VAR,
    TALF_ERR_CYCLE,
    TALF_ERR_VALIDATION,
    TALF_ERR_INCLUDE,
    TALF_ERR_IO
} talf_err;

typedef struct {
    talf_err code;
    int      pass;
    char     section[64];
    char     message[256];
} talf_result_t;

typedef struct TAlfCtx {
    alforno_ctx *inner;
    talf_op op;
    talf_prec prec;
    char *base_dir;            /* held so we can re-apply after adds */
    char **tags;                /* copies, held for the context's life */
    size_t tag_count;

    /* Sticky error: set when add_input fails; process re-surfaces
     * the same error. Matches sibling semantics (callers that
     * ignore add_input's return still see the error via process). */
    int poisoned;
    talf_err sticky_code;
    int sticky_pass;
    char sticky_msg[256];
} TAlfCtx;

static talf_err map_hatch(unsigned long h, int *out_pass) {
    /* Maps our alforno_process hatches to the sibling enum + pass
     * number. Our codes are: 3=no-inputs, 4=internal oom,
     * 6=unresolved var, 7=missing link target, 8=unknown merge
     * strategy, 9=missing consumes, 20=required missing,
     * 21=type mismatch, 50..=include errors. */
    if (out_pass) *out_pass = 0;
    switch (h) {
        case 0:  return TALF_OK;
        case 6:  if (out_pass) *out_pass = 1; return TALF_ERR_UNRESOLVED_VAR;
        case 7:  if (out_pass) *out_pass = 3; return TALF_ERR_CYCLE;
        case 8:  if (out_pass) *out_pass = 2; return TALF_ERR_BAD_RECIPE;
        case 9:  if (out_pass) *out_pass = 2; return TALF_ERR_MISSING_CONSUMES;
        case 10: if (out_pass) *out_pass = 3; return TALF_ERR_CYCLE;
        case 20: if (out_pass) *out_pass = 4; return TALF_ERR_VALIDATION;
        case 21: if (out_pass) *out_pass = 4; return TALF_ERR_VALIDATION;
        case 50:
        case 51:
        case 52:
        case 53:
        case 54:
        case 55:
            if (out_pass) *out_pass = 0;
            return TALF_ERR_INCLUDE;
        default: return TALF_ERR_ALLOC;
    }
}

/* Forward declaration — used by both talf_set_recipe and
 * talf_add_input, but the definition lives below. */
static int starts_with_at(const char *src, size_t len);

static void fill_ok(talf_result_t *r) {
    if (!r) return;
    memset(r, 0, sizeof(*r));
}

static void fill_err(talf_result_t *r, talf_err code, int pass, const char *msg) {
    if (!r) return;
    memset(r, 0, sizeof(*r));
    r->code = code;
    r->pass = pass;
    if (msg) snprintf(r->message, sizeof(r->message), "%s", msg);
}

void *talf_create(int op_in, void *result_v) {
    talf_result_t *result = (talf_result_t *)result_v;
    TAlfCtx *t = (TAlfCtx *)calloc(1, sizeof(*t));
    if (!t) { fill_err(result, TALF_ERR_ALLOC, 0, "oom"); return NULL; }

    alforno_op aop;
    switch (op_in) {
        case TALF_AGGREGATE: aop = ALFORNO_OP_AGGREGATE; break;
        case TALF_CONFLATE:  aop = ALFORNO_OP_CONFLATE;  break;
        case TALF_SCATTER:   aop = ALFORNO_OP_SCATTER;   break;
        case TALF_GATHER:    aop = ALFORNO_OP_GATHER;    break;
        default: free(t); fill_err(result, TALF_ERR_ALLOC, 0, "bad op"); return NULL;
    }
    if (alforno_create(&t->inner, aop) != 0) {
        free(t);
        fill_err(result, TALF_ERR_ALLOC, 0, "alforno_create failed");
        return NULL;
    }
    t->op = (talf_op)op_in;
    t->prec = TALF_LAST_WINS;
    fill_ok(result);
    return t;
}

int talf_set_precedence(void *ctx_v, int prec_in, void *result_v) {
    TAlfCtx *ctx = (TAlfCtx *)ctx_v;
    talf_result_t *result = (talf_result_t *)result_v;
    if (!ctx) { fill_err(result, TALF_ERR_ALLOC, 0, "null ctx"); return -1; }
    alforno_precedence p = (prec_in == TALF_FIRST_FOUND)
        ? ALFORNO_FIRST_FOUND : ALFORNO_LAST_WINS;
    if (alforno_set_precedence(ctx->inner, p) != 0) {
        fill_err(result, TALF_ERR_ALLOC, 0, "set_precedence failed");
        return -1;
    }
    ctx->prec = (talf_prec)prec_in;
    fill_ok(result);
    return 0;
}

int talf_set_tags(void *ctx_v, const char **tags, size_t count, void *result_v) {
    TAlfCtx *ctx = (TAlfCtx *)ctx_v;
    talf_result_t *result = (talf_result_t *)result_v;
    if (!ctx) { fill_err(result, TALF_ERR_ALLOC, 0, "null ctx"); return -1; }
    if (alforno_set_tags(ctx->inner, tags, (u64)count) != 0) {
        fill_err(result, TALF_ERR_ALLOC, 0, "set_tags failed");
        return -1;
    }
    fill_ok(result);
    return 0;
}

int talf_set_base_dir(void *ctx_v, const char *dir, void *result_v) {
    TAlfCtx *ctx = (TAlfCtx *)ctx_v;
    talf_result_t *result = (talf_result_t *)result_v;
    if (!ctx) { fill_err(result, TALF_ERR_ALLOC, 0, "null ctx"); return -1; }
    if (alforno_set_base_dir(ctx->inner, dir) != 0) {
        fill_err(result, TALF_ERR_ALLOC, 0, "set_base_dir failed");
        return -1;
    }
    fill_ok(result);
    return 0;
}

int talf_add_input_file(void *ctx_v, const char *path, void *result_v) {
    TAlfCtx *ctx = (TAlfCtx *)ctx_v;
    talf_result_t *result = (talf_result_t *)result_v;
    if (!ctx) { fill_err(result, TALF_ERR_ALLOC, 0, "null ctx"); return -1; }
    unsigned long rc = alforno_add_input_file(ctx->inner, path);
    if (rc != 0) {
        { int pass = 0; talf_err code = map_hatch(rc, &pass);
          fill_err(result, code, pass, "add_input_file failed"); }
        return -1;
    }
    fill_ok(result);
    return 0;
}

int talf_set_recipe(void *ctx_v, const char *src, size_t len, void *result_v) {
    TAlfCtx *ctx = (TAlfCtx *)ctx_v;
    talf_result_t *result = (talf_result_t *)result_v;
    pasta_value *rec = NULL;
    pasta_result pr;
    if (!ctx) { fill_err(result, TALF_ERR_ALLOC, 0, "null ctx"); return -1; }
    if (pasta_parse(&rec, &pr, (const u8 *)src, (u64)len) != 0) {
        fill_err(result, TALF_ERR_PARSE, 0, pr.msg[0] ? pr.msg : "recipe parse failed");
        return -1;
    }
    {
        pasta_kind k;
        pasta_kind_of(&k, rec);
        if (k != PASTA_MAP) {
            pasta_value_destroy(rec);
            fill_err(result, TALF_ERR_NOT_SECTIONS, 0, "recipe not a section map");
            return -1;
        }
    }
    if (!starts_with_at(src, len)) {
        pasta_value_destroy(rec);
        fill_err(result, TALF_ERR_NOT_SECTIONS, 0, "recipe is not a sections document");
        return -1;
    }
    if (alforno_set_recipe(ctx->inner, rec) != 0) {
        fill_err(result, TALF_ERR_BAD_RECIPE, 0, "set_recipe failed");
        return -1;
    }
    fill_ok(result);
    return 0;
}

/* Detect whether `src` starts (after blanks + comments) with '@'.
 * Sibling alforno requires section-style input; bare map/array
 * documents are rejected with ERR_NOT_SECTIONS. */
static int starts_with_at(const char *src, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)src[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        if (c == ';') { while (i < len && src[i] != '\n') i++; continue; }
        return c == '@';
    }
    return 0;
}

int talf_add_input(void *ctx_v, const char *src, size_t len, void *result_v) {
    TAlfCtx *ctx = (TAlfCtx *)ctx_v;
    talf_result_t *result = (talf_result_t *)result_v;
    pasta_value *v = NULL;
    pasta_result pr;
    if (!ctx) { fill_err(result, TALF_ERR_ALLOC, 0, "null ctx"); return -1; }
    if (pasta_parse(&v, &pr, (const u8 *)src, (u64)len) != 0) {
        fill_err(result, TALF_ERR_PARSE, 0, pr.msg[0] ? pr.msg : "input parse failed");
        return -1;
    }
    {
        pasta_kind k;
        pasta_kind_of(&k, v);
        if (k != PASTA_MAP) {
            pasta_value_destroy(v);
            fill_err(result, TALF_ERR_NOT_SECTIONS, 0, "input not a section map");
            return -1;
        }
    }
    if (!starts_with_at(src, len)) {
        pasta_value_destroy(v);
        fill_err(result, TALF_ERR_NOT_SECTIONS, 0, "input is not a sections document");
        return -1;
    }

    /* Handle @include in the source by delegating each listed file
     * to alforno_add_input_file (which already resolves nested
     * includes + cycles). Our apennines alforno_add_input itself
     * doesn't touch includes, so the shim does it here. */
    {
        pasta_value *includes = NULL;
        (void)pasta_map_get(&includes, v, "include");
        if (includes) {
            pasta_kind kk;
            pasta_kind_of(&kk, includes);
            if (kk == PASTA_ARRAY) {
                u64 i, cnt;
                pasta_count(&cnt, includes);
                for (i = 0; i < cnt; i++) {
                    pasta_value *item = NULL;
                    const u8 *sp; u64 sl;
                    char child_path[512];
                    pasta_array_at(&item, includes, i);
                    if (!item || pasta_as_string(&sp, &sl, item) != 0) continue;
                    if (sl >= sizeof(child_path)) continue;
                    memcpy(child_path, sp, sl); child_path[sl] = 0;
                    unsigned long rc = alforno_add_input_file(ctx->inner, child_path);
                    if (rc != 0) {
                        pasta_value_destroy(v);
                        { int pass = 0; talf_err code = map_hatch(rc, &pass);
                          fill_err(result, code, pass, "include resolution failed");
                          ctx->poisoned = 1; ctx->sticky_code = code;
                          ctx->sticky_pass = pass;
                          snprintf(ctx->sticky_msg, sizeof(ctx->sticky_msg),
                                   "%s", "include resolution failed"); }
                        return -1;
                    }
                }
            }
            /* Strip the include section from the value before we add it. */
            {
                u64 j;
                for (j = 0; j < v->u.map.count; j++) {
                    if (v->u.map.key_lens[j] == 7
                        && memcmp(v->u.map.keys[j], "include", 7) == 0) {
                        pasta_value_destroy(v->u.map.vals[j]);
                        free(v->u.map.keys[j]);
                        {
                            u64 m;
                            for (m = j + 1; m < v->u.map.count; m++) {
                                v->u.map.keys[m - 1] = v->u.map.keys[m];
                                v->u.map.key_lens[m - 1] = v->u.map.key_lens[m];
                                v->u.map.vals[m - 1] = v->u.map.vals[m];
                            }
                        }
                        v->u.map.count--;
                        break;
                    }
                }
            }
        }
    }

    if (alforno_add_input(ctx->inner, v) != 0) {
        fill_err(result, TALF_ERR_ALLOC, 0, "add_input failed");
        return -1;
    }
    fill_ok(result);
    return 0;
}

void *talf_process(void *ctx_v, void *result_v) {
    TAlfCtx *ctx = (TAlfCtx *)ctx_v;
    talf_result_t *result = (talf_result_t *)result_v;
    pasta_value *out = NULL;
    unsigned long rc;
    if (!ctx) { fill_err(result, TALF_ERR_ALLOC, 0, "null ctx"); return NULL; }
    if (ctx->poisoned) {
        fill_err(result, ctx->sticky_code, ctx->sticky_pass, ctx->sticky_msg);
        return NULL;
    }
    rc = alforno_process(&out, ctx->inner);
    if (rc != 0) {
        { int pass = 0; talf_err code = map_hatch(rc, &pass);
          fill_err(result, code, pass, "process failed"); }
        return NULL;
    }
    fill_ok(result);
    return out;
}

char *talf_process_to_string(void *ctx_v, int flags, void *result_v) {
    pasta_value *out = talf_process(ctx_v, result_v);
    buf b;
    u8 *p;
    u64 n;
    char *s;
    u32 our_flags = 0;
    if (!out) return NULL;
    if (buf_create(&b, 0) != 0) {
        pasta_value_destroy(out);
        return NULL;
    }
    if (flags & 1) our_flags |= PASTA_WRITE_COMPACT;
    if (flags & 2) our_flags |= PASTA_WRITE_SECTIONS;
    if (flags & 4) our_flags |= PASTA_WRITE_SORTED;
    if (pasta_write(&b, out, our_flags) != 0) {
        buf_destroy(&b);
        pasta_value_destroy(out);
        return NULL;
    }
    buf_ptr(&p, &b);
    buf_len(&n, &b);
    s = (char *)malloc(n + 1);
    if (!s) { buf_destroy(&b); pasta_value_destroy(out); return NULL; }
    if (n) memcpy(s, p, n);
    s[n] = 0;
    buf_destroy(&b);
    pasta_value_destroy(out);
    return s;
}

static int make_dir(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

int talf_scatter_to_dir(void *ctx_v, const char *output_dir, const char *ext, void *result_v) {
    TAlfCtx *ctx = (TAlfCtx *)ctx_v;
    talf_result_t *result = (talf_result_t *)result_v;
    pasta_value *out = NULL;
    int count = 0;
    unsigned long rc;
    if (!ctx) { fill_err(result, TALF_ERR_ALLOC, 0, "null ctx"); return -1; }
    if (!output_dir || !ext) { fill_err(result, TALF_ERR_ALLOC, 0, "null args"); return -1; }

    make_dir(output_dir);   /* best-effort; ignore EEXIST */

    rc = alforno_process(&out, ctx->inner);
    if (rc != 0) {
        { int pass = 0; talf_err code = map_hatch(rc, &pass);
          fill_err(result, code, pass, "process failed"); }
        return -1;
    }
    /* out is a map of section_name -> pastlet (single-section map). */
    {
        u64 i, n;
        if (pasta_count(&n, out) != 0) {
            pasta_value_destroy(out);
            fill_err(result, TALF_ERR_IO, 0, "bad scatter output shape");
            return -1;
        }
        for (i = 0; i < n; i++) {
            const u8 *kname;
            u64 klen;
            pasta_value *pastlet = NULL;
            char path[512];
            char name[256];
            FILE *f;
            buf b;
            u8 *p;
            u64 pn;

            pasta_map_key(&kname, &klen, out, i);
            pasta_map_value(&pastlet, out, i);
            if (klen >= sizeof(name)) continue;
            memcpy(name, kname, klen);
            name[klen] = 0;
            snprintf(path, sizeof(path), "%s/%s%s", output_dir, name, ext);

            if (buf_create(&b, 0) != 0) continue;
            if (pasta_write(&b, pastlet, PASTA_WRITE_SECTIONS) != 0) {
                buf_destroy(&b);
                continue;
            }
            buf_ptr(&p, &b);
            buf_len(&pn, &b);

            f = fopen(path, "wb");
            if (!f) { buf_destroy(&b); continue; }
            if (pn) fwrite(p, 1, (size_t)pn, f);
            fclose(f);
            buf_destroy(&b);
            count++;
        }
    }
    pasta_value_destroy(out);
    fill_ok(result);
    return count;
}

void talf_free(void *ctx_v) {
    TAlfCtx *ctx = (TAlfCtx *)ctx_v;
    if (!ctx) return;
    alforno_destroy(ctx->inner);
    free(ctx->base_dir);
    {
        size_t i;
        for (i = 0; i < ctx->tag_count; i++) free(ctx->tags[i]);
        free(ctx->tags);
    }
    free(ctx);
}
