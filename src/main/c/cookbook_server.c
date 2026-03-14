#include "cookbook_server.h"
#include "cookbook_semver.h"
#include "cookbook_sha256.h"
#include "cookbook_auth.h"
#include "cookbook_grid.h"
#include "cookbook_policy.h"
#include "civetweb.h"
#include "pasta.h"
#include <sodium.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>
#ifdef _WIN32
  #include <windows.h>
#else
  #include <pthread.h>
  #include <unistd.h>
#endif

/* #28: rate limit bucket */
typedef struct rate_bucket {
    char                  sub[128];
    int                   count;
    int64_t               window_start;
    struct rate_bucket   *next;
} rate_bucket;

/* #3: Prometheus metrics counters */
typedef struct {
    volatile long requests_total;
    volatile long requests_get;
    volatile long requests_put;
    volatile long requests_post;
    volatile long responses_2xx;
    volatile long responses_4xx;
    volatile long responses_5xx;
    volatile long artifacts_published;
    volatile long artifacts_yanked;
    volatile long artifacts_resolved;
    volatile long auth_tokens_issued;
    volatile long auth_failures;
    volatile long bytes_uploaded;
    volatile long bytes_downloaded;
} cookbook_metrics;

struct cookbook_server {
    struct mg_context  *ctx;
    cookbook_db         *db;
    cookbook_store      *store;
    char               *registry_id;
    size_t              max_upload_bytes;
    int                 pending_timeout_sec;
    int                 jwt_ttl_sec;
    int                 rate_limit_per_min;
    unsigned char       registry_pk[32];
    unsigned char       registry_sk[64];
    int                 has_registry_key;
    rate_bucket        *rate_buckets;
    cookbook_metrics     metrics;
    int                 grid_enabled;
    int                 grid_max_hops;
    volatile int        reconcile_running;
#ifdef _WIN32
    HANDLE              reconcile_thread;
    CRITICAL_SECTION    rate_lock;
#else
    pthread_t           reconcile_thread;
    pthread_mutex_t     rate_lock;
#endif
};

/* ==== metrics helpers ==== */

#ifdef _WIN32
#define METRIC_INC(m)  InterlockedIncrement(&(m))
#define METRIC_ADD(m,v) InterlockedExchangeAdd(&(m), (long)(v))
#else
#define METRIC_INC(m)  __sync_add_and_fetch(&(m), 1)
#define METRIC_ADD(m,v) __sync_add_and_fetch(&(m), (long)(v))
#endif

/* ==== #8: content negotiation ==== */

typedef enum {
    CT_PASTA,   /* application/x-pasta (canonical) */
    CT_JSON,    /* application/json */
    CT_UNKNOWN  /* unsupported */
} content_pref;

/* Parse Accept header and return the preferred content type.
   Returns CT_PASTA for missing Accept or *\/*. */
static content_pref parse_accept(const struct mg_request_info *ri) {
    const char *accept = NULL;
    for (int i = 0; i < ri->num_headers; i++) {
        if (strcasecmp(ri->http_headers[i].name, "Accept") == 0) {
            accept = ri->http_headers[i].value;
            break;
        }
    }

    /* no Accept header → default to Pasta */
    if (!accept || !*accept) return CT_PASTA;

    /* check for specific types, tracking highest quality value */
    double q_pasta = -1.0, q_json = -1.0, q_text = -1.0, q_star = -1.0;

    /* simple Accept parser: split on comma, check each media range */
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", accept);
    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok) {
        /* trim leading whitespace */
        while (*tok == ' ') tok++;

        double q = 1.0;
        char *qp = strstr(tok, ";q=");
        if (!qp) qp = strstr(tok, "; q=");
        if (qp) {
            q = atof(qp + (qp[1] == 'q' ? 3 : 4));
        }

        if (strstr(tok, "application/x-pasta") ||
            strstr(tok, "application/pasta"))
            q_pasta = q;
        else if (strstr(tok, "application/json"))
            q_json = q;
        else if (strstr(tok, "text/plain"))
            q_text = q;
        else if (strstr(tok, "*/*"))
            q_star = q;

        tok = strtok_r(NULL, ",", &saveptr);
    }

    /* text/plain maps to pasta (backwards compat) */
    if (q_text > q_pasta) q_pasta = q_text;
    /* *\/* maps to pasta (default) */
    if (q_star >= 0.0 && q_pasta < 0.0 && q_json < 0.0)
        q_pasta = q_star;

    if (q_pasta < 0.0 && q_json < 0.0)
        return CT_UNKNOWN;  /* no supported type → 406 */
    if (q_json > q_pasta)
        return CT_JSON;
    return CT_PASTA;
}

/* Serialize a PastaValue tree to JSON. Returns malloc'd string. */
static char *pasta_to_json(const PastaValue *v) {
    if (!v) return strdup("null");

    switch (pasta_type(v)) {
    case PASTA_NULL:
        return strdup("null");
    case PASTA_BOOL:
        return strdup(pasta_get_bool(v) ? "true" : "false");
    case PASTA_NUMBER: {
        char buf[64];
        double n = pasta_get_number(v);
        if (n == (double)(long long)n && n >= -1e15 && n <= 1e15)
            snprintf(buf, sizeof(buf), "%lld", (long long)n);
        else
            snprintf(buf, sizeof(buf), "%.17g", n);
        return strdup(buf);
    }
    case PASTA_STRING: {
        const char *s = pasta_get_string(v);
        size_t slen = pasta_get_string_len(v);
        /* worst case: every char needs escaping (\n → 2 chars) + quotes + NUL */
        char *out = malloc(slen * 2 + 3);
        if (!out) return NULL;
        size_t j = 0;
        out[j++] = '"';
        for (size_t i = 0; i < slen; i++) {
            char c = s[i];
            if (c == '"')       { out[j++] = '\\'; out[j++] = '"'; }
            else if (c == '\\') { out[j++] = '\\'; out[j++] = '\\'; }
            else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
            else if (c == '\r') { out[j++] = '\\'; out[j++] = 'r'; }
            else if (c == '\t') { out[j++] = '\\'; out[j++] = 't'; }
            else out[j++] = c;
        }
        out[j++] = '"';
        out[j] = '\0';
        return out;
    }
    case PASTA_ARRAY: {
        size_t cap = 256, len = 0;
        char *out = malloc(cap);
        if (!out) return NULL;
        out[len++] = '[';
        size_t count = pasta_count(v);
        for (size_t i = 0; i < count; i++) {
            char *elem = pasta_to_json(pasta_array_get(v, i));
            if (!elem) { free(out); return NULL; }
            size_t elen = strlen(elem);
            while (len + elen + 3 > cap) { cap *= 2; out = realloc(out, cap); }
            if (i > 0) out[len++] = ',';
            memcpy(out + len, elem, elen);
            len += elen;
            free(elem);
        }
        if (len + 2 > cap) { cap += 2; out = realloc(out, cap); }
        out[len++] = ']';
        out[len] = '\0';
        return out;
    }
    case PASTA_MAP: {
        size_t cap = 256, len = 0;
        char *out = malloc(cap);
        if (!out) return NULL;
        out[len++] = '{';
        size_t count = pasta_count(v);
        for (size_t i = 0; i < count; i++) {
            const char *key = pasta_map_key(v, i);
            char *val = pasta_to_json(pasta_map_value(v, i));
            if (!val) { free(out); return NULL; }
            size_t klen = strlen(key), vlen = strlen(val);
            /* "key":val, */
            while (len + klen + vlen + 6 > cap) { cap *= 2; out = realloc(out, cap); }
            if (i > 0) out[len++] = ',';
            out[len++] = '"';
            memcpy(out + len, key, klen); len += klen;
            out[len++] = '"'; out[len++] = ':';
            memcpy(out + len, val, vlen); len += vlen;
            free(val);
        }
        if (len + 2 > cap) { cap += 2; out = realloc(out, cap); }
        out[len++] = '}';
        out[len] = '\0';
        return out;
    }
    }
    return strdup("null");
}

/* Validate that input is pure ASCII (no byte > 0x7F and no NUL).
   Returns 0 if valid, or the 1-based offset of the first bad byte. */
static size_t validate_ascii(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == 0x00 || c > 0x7F) return i + 1;
    }
    return 0;
}

/* exported wrappers for testing */
size_t cookbook_validate_ascii(const char *data, size_t len) {
    return validate_ascii(data, len);
}

char *cookbook_pasta_to_json(const PastaValue *v) {
    return pasta_to_json(v);
}

/* ==== helpers ==== */

static size_t url_decode(char *buf, size_t len) {
    size_t i = 0, j = 0;
    while (i < len) {
        if (buf[i] == '%' && i + 2 < len) {
            char hex[3] = { buf[i+1], buf[i+2], '\0' };
            buf[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else if (buf[i] == '+') {
            buf[j++] = ' ';
            i++;
        } else {
            buf[j++] = buf[i++];
        }
    }
    buf[j] = '\0';
    return j;
}

static char *path_after(const char *uri, const char *prefix) {
    size_t plen = strlen(prefix);
    if (strncmp(uri, prefix, plen) != 0) return NULL;
    char *buf = strdup(uri + plen);
    if (buf) url_decode(buf, strlen(buf));
    return buf;
}

static int split_coord(const char *path, char **group, char **artifact,
                        char **tail) {
    *group = *artifact = *tail = NULL;

    const char *last_slash = strrchr(path, '/');
    if (!last_slash || last_slash == path) return -1;

    *tail = strdup(last_slash + 1);

    const char *prev = last_slash - 1;
    while (prev > path && *prev != '/') prev--;
    if (*prev == '/') {
        size_t art_len = (size_t)(last_slash - prev - 1);
        *artifact = malloc(art_len + 1);
        memcpy(*artifact, prev + 1, art_len);
        (*artifact)[art_len] = '\0';

        size_t grp_len = (size_t)(prev - path);
        *group = malloc(grp_len + 1);
        memcpy(*group, path, grp_len);
        (*group)[grp_len] = '\0';
        for (size_t i = 0; i < grp_len; i++)
            if ((*group)[i] == '/') (*group)[i] = '.';
    } else {
        free(*tail);
        *tail = NULL;
        return -1;
    }
    return 0;
}

static void send_json(struct mg_connection *conn, int status,
                       const char *body) {
    size_t len = strlen(body);
    mg_printf(conn,
              "HTTP/1.1 %d %s\r\n"
              "Content-Type: application/json\r\n"
              "Content-Length: %zu\r\n"
              "\r\n"
              "%s",
              status, (status < 300) ? "OK" : "Error",
              len, body);
}

static char *read_body(struct mg_connection *conn,
                        const struct mg_request_info *ri,
                        size_t *out_len, size_t max_bytes) {
    long long cl = ri->content_length;

    if (max_bytes > 0 && cl > 0 && (size_t)cl > max_bytes) {
        *out_len = 0;
        return NULL;
    }

    if (cl <= 0) {
        size_t cap = 65536, total = 0;
        char *buf = malloc(cap);
        if (!buf) { *out_len = 0; return NULL; }
        for (;;) {
            if (max_bytes > 0 && total >= max_bytes) {
                free(buf); *out_len = 0; return NULL;
            }
            if (total >= cap) {
                cap *= 2;
                if (max_bytes > 0 && cap > max_bytes) cap = max_bytes + 1;
                char *tmp = realloc(buf, cap);
                if (!tmp) { free(buf); *out_len = 0; return NULL; }
                buf = tmp;
            }
            int n = mg_read(conn, buf + total, cap - total);
            if (n <= 0) break;
            total += (size_t)n;
        }
        if (total == 0) { free(buf); *out_len = 0; return NULL; }
        *out_len = total;
        return buf;
    }
    char *buf = malloc((size_t)cl);
    if (!buf) { *out_len = 0; return NULL; }
    size_t total = 0;
    while (total < (size_t)cl) {
        int n = mg_read(conn, buf + total, (size_t)cl - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    if (total == 0) { free(buf); *out_len = 0; return NULL; }
    *out_len = total;
    return buf;
}

/* ==== input validation ==== */

#define MAX_GROUP_LEN    128
#define MAX_ARTIFACT_LEN 64
#define MAX_VERSION_LEN  64
#define MAX_FILENAME_LEN 256

static int validate_path_segment(const char *s) {
    if (!s || !*s) return -1;
    if (strstr(s, "..") != NULL) return -1;
    if (strchr(s, '\\') != NULL) return -1;
    for (const char *p = s; *p; p++) {
        if (*p < 0x20) return -1;
    }
    return 0;
}

static int validate_group(const char *g) {
    if (!g || !*g || strlen(g) > MAX_GROUP_LEN) return -1;
    if (validate_path_segment(g) != 0) return -1;
    for (const char *p = g; *p; p++) {
        char c = *p;
        if (c != '.' && c != '-' && c != '_' &&
            !(c >= 'a' && c <= 'z') &&
            !(c >= '0' && c <= '9'))
            return -1;
    }
    return 0;
}

static int validate_artifact(const char *a) {
    if (!a || !*a || strlen(a) > MAX_ARTIFACT_LEN) return -1;
    if (validate_path_segment(a) != 0) return -1;
    for (const char *p = a; *p; p++) {
        char c = *p;
        if (c != '-' && c != '_' &&
            !(c >= 'a' && c <= 'z') &&
            !(c >= '0' && c <= '9'))
            return -1;
    }
    return 0;
}

static int validate_version(const char *v) {
    if (!v || !*v || strlen(v) > MAX_VERSION_LEN) return -1;
    cookbook_semver sv;
    return cookbook_semver_parse(v, &sv);
}

/* ==== #28: rate limiting ==== */

static int check_rate_limit(cookbook_server *srv, const char *sub) {
    if (srv->rate_limit_per_min <= 0 || !sub || !sub[0]) return 0;

    int64_t now = (int64_t)time(NULL);

#ifdef _WIN32
    EnterCriticalSection(&srv->rate_lock);
#else
    pthread_mutex_lock(&srv->rate_lock);
#endif

    rate_bucket *b = srv->rate_buckets;
    while (b) {
        if (strcmp(b->sub, sub) == 0) break;
        b = b->next;
    }

    if (!b) {
        b = calloc(1, sizeof(*b));
        if (b) {
            snprintf(b->sub, sizeof(b->sub), "%s", sub);
            b->window_start = now;
            b->count = 0;
            b->next = srv->rate_buckets;
            srv->rate_buckets = b;
        }
    }

    int blocked = 0;
    if (b) {
        if (now - b->window_start >= 60) {
            b->window_start = now;
            b->count = 0;
        }
        b->count++;
        if (b->count > srv->rate_limit_per_min)
            blocked = 1;
    }

#ifdef _WIN32
    LeaveCriticalSection(&srv->rate_lock);
#else
    pthread_mutex_unlock(&srv->rate_lock);
#endif

    return blocked;
}

/* ==== auth middleware: extract and verify JWT ==== */

static int extract_bearer_jwt(cookbook_server *srv,
                                const struct mg_request_info *ri,
                                cookbook_jwt_claims *claims) {
    if (!srv->has_registry_key) return -1;

    const char *auth = NULL;
    for (int i = 0; i < ri->num_headers; i++) {
        if (strcasecmp(ri->http_headers[i].name, "Authorization") == 0) {
            auth = ri->http_headers[i].value;
            break;
        }
    }
    if (!auth) return -1;
    if (strncmp(auth, "Bearer ", 7) != 0) return -1;

    return cookbook_jwt_verify(auth + 7, srv->registry_pk, claims);
}

/* ==== #6: triple extraction from archive filename ==== */

/* Parse triple from filename like "core-1.0.0-linux-amd64-gnu.tar.gz".
   Pattern: {artifact}-{version}-{os}-{arch}-{abi}.{ext}
   If no triple segments or filename is "noarch", writes "noarch".
   Writes triple as "os:arch:abi" into out (must be >= 128 bytes). */
static void extract_triple(const char *filename, const char *artifact,
                            const char *version, char *out, size_t out_sz) {
    /* default */
    snprintf(out, out_sz, "noarch");

    if (!filename || !artifact || !version) return;

    /* strip extensions: .tar.gz, .tar.zst, .tar.xz, .zip, etc. */
    char base[256];
    snprintf(base, sizeof(base), "%s", filename);
    char *dot = strstr(base, ".tar.");
    if (dot) *dot = '\0';
    else {
        dot = strrchr(base, '.');
        if (dot) *dot = '\0';
    }

    /* expected prefix: "{artifact}-{version}-" */
    char prefix[256];
    snprintf(prefix, sizeof(prefix), "%s-%s-", artifact, version);
    size_t pfx_len = strlen(prefix);

    if (strncmp(base, prefix, pfx_len) != 0) return;

    const char *triple_part = base + pfx_len;
    if (!*triple_part || strcmp(triple_part, "noarch") == 0) return;

    /* split on '-': os-arch-abi → os:arch:abi */
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s", triple_part);

    char *os_str = tmp;
    char *arch_str = NULL;
    char *abi_str = NULL;

    char *d1 = strchr(os_str, '-');
    if (d1) {
        *d1 = '\0';
        arch_str = d1 + 1;
        char *d2 = strchr(arch_str, '-');
        if (d2) {
            *d2 = '\0';
            abi_str = d2 + 1;
        }
    }

    if (os_str && arch_str && abi_str)
        snprintf(out, out_sz, "%s:%s:%s", os_str, arch_str, abi_str);
    else if (os_str && arch_str)
        snprintf(out, out_sz, "%s:%s", os_str, arch_str);
}

/* ==== #23: descriptor stripping ==== */

/* Check if a key is a build-only field that should be stripped. */
static int is_build_only_field(const char *key) {
    static const char *build_only[] = {
        "build", "test", "dev-dependencies", "scripts", "bench",
        "ci", "hooks", "profile", NULL
    };
    for (int i = 0; build_only[i]; i++)
        if (strcmp(key, build_only[i]) == 0) return 1;
    return 0;
}

/* Strip build-only fields from a pasta descriptor for installed view.
   Returns a new malloc'd buffer with stripped content, or NULL on error.
   Caller must free. */
static char *strip_descriptor(const char *body, size_t body_len,
                               size_t *out_len) {
    PastaResult pr;
    PastaValue *root = pasta_parse(body, body_len, &pr);
    if (!root || pasta_type(root) != PASTA_MAP) {
        if (root) pasta_free(root);
        *out_len = 0;
        return NULL;
    }

    /* build a new map with only non-build-only fields */
    PastaValue *stripped = pasta_new_map();
    size_t count = pasta_count(root);
    for (size_t i = 0; i < count; i++) {
        const char *key = pasta_map_key(root, i);
        if (key && !is_build_only_field(key)) {
            const PastaValue *val = pasta_map_value(root, i);
            /* pasta_set takes ownership, so we need to re-serialize and
               re-parse the value — or use a simpler approach:
               serialize the whole thing and re-parse, since pasta_set
               requires owned values. Simpler: just write the value as
               a string and parse it back. But that's expensive.
               Instead, we'll write the whole original and do text-level
               stripping... No, let's use the builder API properly. */
            /* Re-serialize this single value to create an owned copy */
            char *val_str = pasta_write(val, PASTA_COMPACT);
            if (val_str) {
                PastaResult vr;
                PastaValue *val_copy = pasta_parse_cstr(val_str, &vr);
                free(val_str);
                if (val_copy)
                    pasta_set(stripped, key, val_copy);
            }
        }
    }

    char *result = pasta_write(stripped, PASTA_COMPACT | PASTA_SORTED);
    pasta_free(stripped);
    pasta_free(root);

    if (!result) { *out_len = 0; return NULL; }
    *out_len = strlen(result);
    return result;
}

/* ==== #19: two-phase write helpers ==== */

/* Get current UTC timestamp as ISO 8601 string. */
static void utc_now(char *buf, size_t sz) {
#ifdef _WIN32
    SYSTEMTIME st;
    GetSystemTime(&st);
    snprintf(buf, sz, "%04d-%02d-%02dT%02d:%02d:%02dZ",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
#else
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, sz, "%Y-%m-%dT%H:%M:%SZ", &tm);
#endif
}

/* Check if a pending artifact has all required files and transition to
   published if so. Required: now.pasta must exist in the store. */
static void try_publish(cookbook_server *srv, const char *grp,
                         const char *art, const char *ver,
                         const char *triple) {
    /* check now.pasta exists in store */
    char pasta_key[512];
    snprintf(pasta_key, sizeof(pasta_key), "%s/%s/%s/%s/%s/now.pasta",
             srv->registry_id, grp, art, ver,
             /* convert group dots back to slashes for store path */
             art); /* this isn't right — we need the actual store path */

    /* Actually: the store key format is registry_id/group_path/artifact/version/filename
       where group_path has slashes. But we stored via the URL path which already
       had slashes for group segments. So the key is:
       registry_id/org/acme/core/1.0.0/now.pasta

       We need to reconstruct: group dots → slashes */
    char grp_path[256];
    snprintf(grp_path, sizeof(grp_path), "%s", grp);
    for (char *p = grp_path; *p; p++)
        if (*p == '.') *p = '/';

    snprintf(pasta_key, sizeof(pasta_key), "%s/%s/%s/%s/now.pasta",
             srv->registry_id, grp_path, art, ver);

    if (srv->store->exists(srv->store, pasta_key) != COOKBOOK_STORE_OK)
        return;  /* not ready yet */

    /* transition to published */
    char now[64];
    utc_now(now, sizeof(now));

    const char *sql =
        "UPDATE artifacts SET status = 'published', published_at = ?1 "
        "WHERE group_id = ?2 AND artifact = ?3 AND version = ?4 "
        "AND status = 'pending'";
    cookbook_db_param params[] = {
        COOKBOOK_P_TEXT(now),
        COOKBOOK_P_TEXT(grp),
        COOKBOOK_P_TEXT(art),
        COOKBOOK_P_TEXT(ver)
    };
    srv->db->exec_p(srv->db, sql, params, 4);
}

/* ==== #20: reconciliation job ==== */

typedef struct {
    char **coord_ids;
    int    count;
    int    cap;
} stale_collect_ctx;

static int stale_collect_cb(const cookbook_db_row *row, void *user) {
    stale_collect_ctx *ctx = (stale_collect_ctx *)user;
    if (ctx->count >= ctx->cap) return 0;
    if (row->values[0])
        ctx->coord_ids[ctx->count++] = strdup(row->values[0]);
    return 0;
}

static void reconcile_stale_pending(cookbook_server *srv) {
    char cutoff[64];
#ifdef _WIN32
    SYSTEMTIME st;
    GetSystemTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    uli.QuadPart -= (ULONGLONG)srv->pending_timeout_sec * 10000000ULL;
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;
    FileTimeToSystemTime(&ft, &st);
    snprintf(cutoff, sizeof(cutoff), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
#else
    time_t t = time(NULL) - srv->pending_timeout_sec;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(cutoff, sizeof(cutoff), "%Y-%m-%dT%H:%M:%SZ", &tm);
#endif

    /* collect stale pending coord_ids */
    stale_collect_ctx ctx;
    ctx.cap = 100;
    ctx.count = 0;
    ctx.coord_ids = calloc((size_t)ctx.cap, sizeof(char *));
    if (!ctx.coord_ids) return;

    const char *sql =
        "SELECT coord_id FROM artifacts "
        "WHERE status = 'pending' AND pending_since < ?1";
    cookbook_db_param params[] = { COOKBOOK_P_TEXT(cutoff) };
    srv->db->query_p(srv->db, sql, params, 1, stale_collect_cb, &ctx);

    /* delete stale rows */
    for (int i = 0; i < ctx.count; i++) {
        const char *del_sv =
            "DELETE FROM artifact_semver WHERE coord_id = ?1";
        const char *del_art =
            "DELETE FROM artifacts WHERE coord_id = ?1 AND status = 'pending'";
        cookbook_db_param dp[] = { COOKBOOK_P_TEXT(ctx.coord_ids[i]) };
        srv->db->exec_p(srv->db, del_sv, dp, 1);
        srv->db->exec_p(srv->db, del_art, dp, 1);
        free(ctx.coord_ids[i]);
    }

    if (ctx.count > 0)
        fprintf(stdout, "cookbook: reconciled %d stale pending artifact(s)\n",
                ctx.count);

    free(ctx.coord_ids);
}

#ifdef _WIN32
static DWORD WINAPI reconcile_thread_fn(LPVOID arg) {
    cookbook_server *srv = (cookbook_server *)arg;
    while (srv->reconcile_running) {
        Sleep(60000);  /* check every 60 seconds */
        if (srv->reconcile_running)
            reconcile_stale_pending(srv);
    }
    return 0;
}
#else
static void *reconcile_thread_fn(void *arg) {
    cookbook_server *srv = (cookbook_server *)arg;
    while (srv->reconcile_running) {
        sleep(60);  /* check every 60 seconds */
        if (srv->reconcile_running)
            reconcile_stale_pending(srv);
    }
    return NULL;
}
#endif

/* ==== route: GET /healthz ==== */

static int handle_healthz(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    send_json(conn, 200, "{\"status\":\"ok\"}\n");
    return 1;
}

/* ==== #2: route: GET /readyz ==== */

typedef struct { int ok; } readyz_ctx;

static int readyz_cb(const cookbook_db_row *row, void *user) {
    readyz_ctx *ctx = (readyz_ctx *)user;
    (void)row;
    ctx->ok = 1;
    return 0;
}

static int handle_readyz(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    if (strcmp(ri->request_method, "GET") != 0) {
        send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
        return 1;
    }

    /* check DB connectivity */
    readyz_ctx rctx = { 0 };
    cookbook_db_status st = srv->db->query(srv->db, "SELECT 1",
                                           readyz_cb, &rctx);
    if (st != COOKBOOK_DB_OK || !rctx.ok) {
        send_json(conn, 503,
            "{\"status\":\"not ready\",\"reason\":\"database\"}\n");
        return 1;
    }

    /* check store connectivity — write and read a sentinel key */
    const char *sentinel = "__readyz_probe__";
    cookbook_store_status sst = srv->store->put(srv->store, sentinel, "1", 1);
    if (sst != COOKBOOK_STORE_OK) {
        send_json(conn, 503,
            "{\"status\":\"not ready\",\"reason\":\"object store\"}\n");
        return 1;
    }
    srv->store->del(srv->store, sentinel);

    send_json(conn, 200,
        "{\"status\":\"ready\",\"db\":\"ok\",\"store\":\"ok\"}\n");
    return 1;
}

/* ==== #17: route: GET /.well-known/now-registry-key ==== */

static int handle_registry_key(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    if (strcmp(ri->request_method, "GET") != 0) {
        send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
        return 1;
    }

    if (!srv->has_registry_key) {
        send_json(conn, 200,
            "{\"algorithm\":\"ed25519\","
            "\"public_key\":null,"
            "\"status\":\"not configured\"}\n");
        return 1;
    }

    /* encode the public key as hex */
    char pk_hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(pk_hex + i * 2, 3, "%02x", srv->registry_pk[i]);

    char resp[256];
    snprintf(resp, sizeof(resp),
        "{\"algorithm\":\"ed25519\","
        "\"public_key\":\"%s\","
        "\"status\":\"active\"}\n", pk_hex);
    send_json(conn, 200, resp);
    return 1;
}

/* grid helper forward declarations (defined later, used in resolve/artifact) */
static int grid_get_hop_count(const struct mg_request_info *ri);
static const char *grid_get_via(const struct mg_request_info *ri);

/* ==== route: GET /resolve/{group}/{artifact}/{range} ==== */

typedef struct {
    cookbook_range *range;
    int            include_snapshots;
    int            include_yanked;  /* F2 */
    char          *buf;
    size_t         len;
    size_t         cap;
    int            count;
} resolve_filter_ctx;

static int resolve_filter_cb(const cookbook_db_row *row, void *user) {
    resolve_filter_ctx *ctx = (resolve_filter_ctx *)user;
    const char *version  = row->values[0];
    const char *snapshot = row->values[1];
    const char *triple   = row->values[2];
    /* F2: columns 3 and 4 are yanked and yank_reason when include_yanked */
    const char *yanked_val  = (ctx->include_yanked && row->ncols > 3)
                              ? row->values[3] : NULL;
    const char *yank_reason = (ctx->include_yanked && row->ncols > 4)
                              ? row->values[4] : NULL;
    if (!version) return 0;

    if (!ctx->include_snapshots && snapshot && snapshot[0] == '1')
        return 0;

    cookbook_semver sv;
    if (cookbook_semver_parse(version, &sv) != 0) return 0;
    if (!cookbook_range_satisfies(ctx->range, &sv)) return 0;

    int n;
    if (ctx->count > 0) {
        n = snprintf(ctx->buf + ctx->len, ctx->cap - ctx->len, ",");
        if (n > 0) ctx->len += (size_t)n;
    }

    int is_yanked = yanked_val && yanked_val[0] == '1';

    if (ctx->include_yanked && is_yanked && yank_reason && yank_reason[0]) {
        n = snprintf(ctx->buf + ctx->len, ctx->cap - ctx->len,
            "{\"version\":\"%s\",\"snapshot\":%s,\"triples\":[\"%s\"],"
            "\"yanked\":true,\"yank_reason\":\"%s\"}",
            version,
            (snapshot && snapshot[0] == '1') ? "true" : "false",
            triple ? triple : "noarch",
            yank_reason);
    } else if (ctx->include_yanked && is_yanked) {
        n = snprintf(ctx->buf + ctx->len, ctx->cap - ctx->len,
            "{\"version\":\"%s\",\"snapshot\":%s,\"triples\":[\"%s\"],"
            "\"yanked\":true}",
            version,
            (snapshot && snapshot[0] == '1') ? "true" : "false",
            triple ? triple : "noarch");
    } else {
        n = snprintf(ctx->buf + ctx->len, ctx->cap - ctx->len,
            "{\"version\":\"%s\",\"snapshot\":%s,\"triples\":[\"%s\"]}",
            version,
            (snapshot && snapshot[0] == '1') ? "true" : "false",
            triple ? triple : "noarch");
    }
    if (n > 0) ctx->len += (size_t)n;
    ctx->count++;
    return 0;
}

static int handle_resolve(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    METRIC_INC(srv->metrics.requests_total);
    METRIC_INC(srv->metrics.requests_get);

    if (strcmp(ri->request_method, "GET") != 0) {
        METRIC_INC(srv->metrics.responses_4xx);
        send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
        return 1;
    }

    char *path = path_after(ri->local_uri, "/resolve/");
    if (!path) {
        send_json(conn, 400, "{\"error\":\"Bad request\"}\n");
        return 1;
    }

    char *group = NULL, *artifact = NULL, *range_str = NULL;
    if (split_coord(path, &group, &artifact, &range_str) != 0 || !range_str) {
        send_json(conn, 400, "{\"error\":\"Malformed path\"}\n");
        free(path); free(group); free(artifact); free(range_str);
        return 1;
    }

    if (validate_group(group) != 0 || validate_artifact(artifact) != 0) {
        send_json(conn, 400,
            "{\"error\":\"Invalid group or artifact identifier\"}\n");
        free(path); free(group); free(artifact); free(range_str);
        return 1;
    }

    cookbook_range range;
    if (cookbook_range_parse(range_str, &range) != 0) {
        send_json(conn, 400, "{\"error\":\"Malformed range string\"}\n");
        free(path); free(group); free(artifact); free(range_str);
        return 1;
    }

    int include_snapshots = 0;
    if (ri->query_string && strstr(ri->query_string, "snapshot=true"))
        include_snapshots = 1;

    /* F2: include_yanked=true returns yanked versions with reason */
    int include_yanked = 0;
    if (ri->query_string && strstr(ri->query_string, "include_yanked=true"))
        include_yanked = 1;

    const char *sql = include_yanked
        ? "SELECT a.version, a.snapshot, a.triple, a.yanked, a.yank_reason "
          "FROM artifacts a "
          "JOIN artifact_semver s ON a.coord_id = s.coord_id "
          "WHERE a.group_id = ?1 AND a.artifact = ?2 "
          "AND a.status = 'published' "
          "ORDER BY s.major DESC, s.minor DESC, s.patch DESC"
        : "SELECT a.version, a.snapshot, a.triple "
          "FROM artifacts a "
          "JOIN artifact_semver s ON a.coord_id = s.coord_id "
          "WHERE a.group_id = ?1 AND a.artifact = ?2 "
          "AND a.yanked = 0 AND a.status = 'published' "
          "ORDER BY s.major DESC, s.minor DESC, s.patch DESC";

    cookbook_db_param params[] = {
        COOKBOOK_P_TEXT(group),
        COOKBOOK_P_TEXT(artifact)
    };

    char result_buf[8192] = {0};
    resolve_filter_ctx ctx = {
        &range, include_snapshots, include_yanked,
        result_buf, 0, sizeof(result_buf), 0
    };

    cookbook_db_status st = srv->db->query_p(srv->db, sql, params, 2,
                                             resolve_filter_cb, &ctx);

    if (st != COOKBOOK_DB_OK) {
        METRIC_INC(srv->metrics.responses_5xx);
        send_json(conn, 500, "{\"error\":\"Database error\"}\n");
    } else {
        /* G3: grid fan-out on empty local results */
        if (ctx.count == 0 && srv->grid_enabled &&
            !grid_get_via(ri)) {
            /* this is a client request with no local results — fan out */
            cookbook_peer *peers = NULL;
            int npeers = cookbook_grid_load_peers(srv->db, &peers);
            for (int pi = 0; pi < npeers && ctx.count == 0; pi++) {
                char grid_path[2048];
                snprintf(grid_path, sizeof(grid_path),
                    "/grid/resolve/%s", path);
                if (ri->query_string && ri->query_string[0]) {
                    size_t gp_len = strlen(grid_path);
                    snprintf(grid_path + gp_len,
                             sizeof(grid_path) - gp_len,
                             "?%s", ri->query_string);
                }
                cookbook_grid_response gresp;
                if (cookbook_grid_get(&peers[pi], grid_path,
                        srv->registry_id, NULL, 0, &gresp) == 0
                    && gresp.status == 200 && gresp.body) {
                    /* copy peer response into result_buf */
                    /* find "versions":[ ... ] and extract the array content */
                    const char *vs = strstr(gresp.body, "\"versions\":[");
                    if (vs) {
                        vs += 12; /* skip "versions":[ */
                        const char *ve = strrchr(vs, ']');
                        if (ve && ve > vs) {
                            size_t vlen = (size_t)(ve - vs);
                            if (vlen < sizeof(result_buf) - ctx.len - 1) {
                                if (ctx.count > 0 && ctx.len > 0) {
                                    result_buf[ctx.len++] = ',';
                                }
                                memcpy(result_buf + ctx.len, vs, vlen);
                                ctx.len += vlen;
                                result_buf[ctx.len] = '\0';
                                ctx.count++;
                            }
                        }
                    }
                    free(gresp.body);
                }
            }
            cookbook_grid_free_peers(peers, npeers);
        }

        METRIC_INC(srv->metrics.responses_2xx);
        METRIC_INC(srv->metrics.artifacts_resolved);

        /* #8: content negotiation on /resolve/ */
        content_pref pref = parse_accept(ri);
        if (pref == CT_UNKNOWN) {
            METRIC_INC(srv->metrics.responses_4xx);
            send_json(conn, 406,
                "{\"error\":\"Not Acceptable — "
                "supported: application/x-pasta, "
                "application/json\"}\n");
            free(path); free(group); free(artifact); free(range_str);
            return 1;
        }

        char response[8320];
        snprintf(response, sizeof(response),
                 "{\"versions\":[%s]}\n", result_buf);

        if (pref == CT_PASTA) {
            /* Parse the JSON response and re-emit as Pasta */
            PastaResult pr;
            PastaValue *root = pasta_parse(response, strlen(response), &pr);
            if (root) {
                int flags = PASTA_COMPACT | PASTA_SORTED;
                if (ri->query_string &&
                    strstr(ri->query_string, "pretty"))
                    flags = PASTA_PRETTY | PASTA_SORTED;
                char *pasta_out = pasta_write(root, flags);
                pasta_free(root);
                if (pasta_out) {
                    size_t plen = strlen(pasta_out);
                    mg_printf(conn,
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/x-pasta\r\n"
                        "Content-Length: %zu\r\n"
                        "\r\n",
                        plen);
                    mg_write(conn, pasta_out, plen);
                    free(pasta_out);
                    free(path); free(group); free(artifact);
                    free(range_str);
                    return 1;
                }
            }
            /* fallback to JSON if Pasta serialization fails */
        }

        send_json(conn, 200, response);
    }

    free(path); free(group); free(artifact); free(range_str);
    return 1;
}

/* ==== route: /artifact/... ==== */

typedef struct {
    int found;
    int yanked;
    char yank_reason[256];
} yanked_check_ctx;

static int yanked_check_cb(const cookbook_db_row *row, void *user) {
    yanked_check_ctx *ctx = (yanked_check_ctx *)user;
    ctx->found = 1;
    if (row->values[0] && row->values[0][0] == '1')
        ctx->yanked = 1;
    if (row->ncols > 1 && row->values[1] && row->values[1][0]) {
        size_t rlen = strlen(row->values[1]);
        if (rlen >= sizeof(ctx->yank_reason))
            rlen = sizeof(ctx->yank_reason) - 1;
        memcpy(ctx->yank_reason, row->values[1], rlen);
        ctx->yank_reason[rlen] = '\0';
    }
    return 0;
}

static int handle_artifact(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    METRIC_INC(srv->metrics.requests_total);
    if (strcmp(ri->request_method, "GET") == 0)
        METRIC_INC(srv->metrics.requests_get);
    else if (strcmp(ri->request_method, "PUT") == 0)
        METRIC_INC(srv->metrics.requests_put);
    else if (strcmp(ri->request_method, "POST") == 0)
        METRIC_INC(srv->metrics.requests_post);

    char *path = path_after(ri->local_uri, "/artifact/");
    if (!path) {
        send_json(conn, 400, "{\"error\":\"Bad request\"}\n");
        return 1;
    }

    if (validate_path_segment(path) != 0) {
        send_json(conn, 400, "{\"error\":\"Invalid path\"}\n");
        free(path);
        return 1;
    }

    /* #1: check for yank request (POST .../yank) */
    size_t pathlen = strlen(path);
    if (pathlen > 5 && strcmp(path + pathlen - 5, "/yank") == 0 &&
        strcmp(ri->request_method, "POST") == 0) {
        path[pathlen - 5] = '\0';
        char *ygroup = NULL, *yartifact = NULL, *yversion = NULL;
        if (split_coord(path, &ygroup, &yartifact, &yversion) != 0 ||
            !yversion) {
            send_json(conn, 400, "{\"error\":\"Malformed yank path\"}\n");
            free(path); free(ygroup); free(yartifact); free(yversion);
            return 1;
        }
        if (validate_group(ygroup) != 0 || validate_artifact(yartifact) != 0 ||
            validate_version(yversion) != 0) {
            send_json(conn, 400,
                "{\"error\":\"Invalid group, artifact, or version\"}\n");
            free(path); free(ygroup); free(yartifact); free(yversion);
            return 1;
        }
        /* F1: read optional reason from POST body */
        char reason[256] = {0};
        size_t ybody_len = 0;
        char *ybody = read_body(conn, ri, &ybody_len, 4096);
        if (ybody && ybody_len > 0) {
            const char *rp = strstr(ybody, "\"reason\":");
            if (rp) {
                rp += 9;
                while (*rp == ' ' || *rp == '\t') rp++;
                if (*rp == '"') {
                    rp++;
                    const char *end = strchr(rp, '"');
                    if (end) {
                        size_t rlen = (size_t)(end - rp);
                        if (rlen >= sizeof(reason)) rlen = sizeof(reason) - 1;
                        memcpy(reason, rp, rlen);
                        reason[rlen] = '\0';
                    }
                }
            }
        }
        free(ybody);

        const char *ysql =
            "UPDATE artifacts SET yanked = 1, yank_reason = ?4 "
            "WHERE group_id = ?1 AND artifact = ?2 AND version = ?3";
        cookbook_db_param yp[] = {
            COOKBOOK_P_TEXT(ygroup),
            COOKBOOK_P_TEXT(yartifact),
            COOKBOOK_P_TEXT(yversion),
            reason[0] ? COOKBOOK_P_TEXT(reason) : COOKBOOK_P_NULL()
        };
        cookbook_db_status yst = srv->db->exec_p(srv->db, ysql, yp, 4);
        if (yst != COOKBOOK_DB_OK) {
            METRIC_INC(srv->metrics.responses_5xx);
            send_json(conn, 500, "{\"error\":\"Database error\"}\n");
        } else {
            METRIC_INC(srv->metrics.responses_2xx);
            METRIC_INC(srv->metrics.artifacts_yanked);
            if (reason[0]) {
                char resp[384];
                snprintf(resp, sizeof(resp),
                    "{\"status\":\"yanked\",\"reason\":\"%s\"}\n", reason);
                send_json(conn, 200, resp);
            } else {
                send_json(conn, 200, "{\"status\":\"yanked\"}\n");
            }
        }
        free(path); free(ygroup); free(yartifact); free(yversion);
        return 1;
    }

    /* parse group/artifact/version/filename from path.
       Path format: {group_path}/{artifact}/{version}/{filename}
       where group_path may contain slashes (e.g., org/acme → org.acme).
       We peel off the last 3 segments: artifact, version, filename. */
    char *group = NULL, *artifact = NULL;
    char *version_str = NULL, *filename = NULL;
    {
        const char *s3 = strrchr(path, '/');
        if (!s3 || s3 == path) goto bad_art_path;
        const char *s2 = s3 - 1;
        while (s2 > path && *s2 != '/') s2--;
        if (*s2 != '/') goto bad_art_path;
        const char *s1 = s2 - 1;
        while (s1 > path && *s1 != '/') s1--;

        size_t art_len = (size_t)(s2 - (*s1 == '/' ? s1 + 1 : s1));
        size_t ver_len = (size_t)(s3 - s2 - 1);
        size_t fn_len  = strlen(s3 + 1);

        if (*s1 == '/') {
            size_t grp_len = (size_t)(s1 - path);
            group = malloc(grp_len + 1);
            memcpy(group, path, grp_len);
            group[grp_len] = '\0';
            for (size_t i = 0; i < grp_len; i++)
                if (group[i] == '/') group[i] = '.';

            artifact = malloc(art_len + 1);
            memcpy(artifact, s1 + 1, art_len);
            artifact[art_len] = '\0';
        } else {
            /* s1 == path start → only 3 segments, not enough */
            goto bad_art_path;
        }

        version_str = malloc(ver_len + 1);
        memcpy(version_str, s2 + 1, ver_len);
        version_str[ver_len] = '\0';

        filename = malloc(fn_len + 1);
        memcpy(filename, s3 + 1, fn_len);
        filename[fn_len] = '\0';

        goto art_path_ok;
    bad_art_path:
        send_json(conn, 400,
            "{\"error\":\"Malformed artifact path\"}\n");
        free(path); free(group); free(artifact);
        free(version_str); free(filename);
        return 1;
    art_path_ok: ;
    }
    char *ver_file = version_str; /* alias for cleanup */

    /* build object store key: registry_id/path */
    size_t key_len = strlen(srv->registry_id) + 1 + strlen(path);
    char *key = malloc(key_len + 1);
    snprintf(key, key_len + 1, "%s/%s", srv->registry_id, path);

    if (strcmp(ri->request_method, "GET") == 0) {
        /* ---- GET: serve artifact ---- */
        void *data = NULL;
        size_t len = 0;
        cookbook_store_status sst = srv->store->get(srv->store, key, &data, &len);

        if (sst == COOKBOOK_STORE_NOT_FOUND) {
            /* G4: grid fan-out on local 404 */
            int grid_handled = 0;
            if (srv->grid_enabled && !grid_get_via(ri)) {
                cookbook_peer *peers = NULL;
                int npeers = cookbook_grid_load_peers(srv->db, &peers);
                for (int pi = 0; pi < npeers; pi++) {
                    char grid_path[2048];
                    snprintf(grid_path, sizeof(grid_path),
                        "/grid/artifact/%s", path);

                    if (peers[pi].mode == 'r') {
                        /* redirect mode: HEAD check then 307 */
                        cookbook_grid_response gresp;
                        if (cookbook_grid_head(&peers[pi], grid_path,
                                srv->registry_id, NULL, 0, &gresp) == 0
                            && gresp.status == 200) {
                            char location[2048];
                            snprintf(location, sizeof(location),
                                "%s/artifact/%s", peers[pi].url, path);
                            mg_printf(conn,
                                "HTTP/1.1 307 Temporary Redirect\r\n"
                                "Location: %s\r\n"
                                "X-Cookbook-Source: %s\r\n"
                                "Content-Length: 0\r\n"
                                "\r\n",
                                location, peers[pi].peer_id);
                            grid_handled = 1;
                            free(gresp.body);
                            break;
                        }
                        free(gresp.body);
                    } else {
                        /* proxy mode: GET and relay */
                        cookbook_grid_response gresp;
                        if (cookbook_grid_get(&peers[pi], grid_path,
                                srv->registry_id, NULL, 0, &gresp) == 0
                            && gresp.status == 200 && gresp.body) {
                            mg_printf(conn,
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: application/octet-stream\r\n"
                                "Content-Length: %zu\r\n"
                                "X-Cookbook-Source: %s\r\n"
                                "\r\n",
                                gresp.body_len, peers[pi].peer_id);
                            mg_write(conn, gresp.body, gresp.body_len);
                            grid_handled = 1;
                            free(gresp.body);
                            break;
                        }
                        free(gresp.body);
                    }
                }
                cookbook_grid_free_peers(peers, npeers);
            }
            if (!grid_handled)
                send_json(conn, 404, "{\"error\":\"Not found\"}\n");
        } else if (sst != COOKBOOK_STORE_OK) {
            send_json(conn, 500, "{\"error\":\"Storage error\"}\n");
        } else {
            const char *ct = "application/octet-stream";
            int is_pasta = 0;
            if (strstr(path, ".sha256")) ct = "text/plain";
            else if (strstr(path, ".sig")) ct = "application/octet-stream";
            else if (filename && strcmp(filename, "now.pasta") == 0) {
                ct = "application/x-pasta";
                is_pasta = 1;
            }
            else if (strstr(path, ".tar.gz")) ct = "application/gzip";
            else if (strstr(path, ".tar.zst")) ct = "application/zstd";

            /* #5: check yanked status (F1: also fetch reason) */
            yanked_check_ctx yctx = { 0, 0, {0} };
            const char *yanked_sql =
                "SELECT yanked, yank_reason FROM artifacts "
                "WHERE group_id = ?1 AND artifact = ?2 AND version = ?3 "
                "LIMIT 1";
            cookbook_db_param yparams[] = {
                COOKBOOK_P_TEXT(group),
                COOKBOOK_P_TEXT(artifact),
                COOKBOOK_P_TEXT(version_str)
            };
            srv->db->query_p(srv->db, yanked_sql, yparams, 3,
                              yanked_check_cb, &yctx);

            /* F1: build yanked headers string */
            char yanked_hdrs[384] = "";
            if (yctx.yanked) {
                if (yctx.yank_reason[0])
                    snprintf(yanked_hdrs, sizeof(yanked_hdrs),
                        "X-Now-Yanked: true\r\n"
                        "X-Now-Yank-Reason: %s\r\n", yctx.yank_reason);
                else
                    snprintf(yanked_hdrs, sizeof(yanked_hdrs),
                        "X-Now-Yanked: true\r\n");
            }

            /* #23: strip build-only fields from now.pasta for installed view */
            void *serve_data = data;
            size_t serve_len = len;
            char *stripped = NULL;
            if (is_pasta) {
                size_t slen = 0;
                stripped = strip_descriptor((const char *)data, len, &slen);
                if (stripped) {
                    serve_data = stripped;
                    serve_len = slen;
                }
            }

            /* #8: content negotiation for now.pasta descriptors */
            if (is_pasta) {
                content_pref pref = parse_accept(ri);
                if (pref == CT_UNKNOWN) {
                    METRIC_INC(srv->metrics.responses_4xx);
                    send_json(conn, 406,
                        "{\"error\":\"Not Acceptable — "
                        "supported: application/x-pasta, "
                        "application/json, text/plain\"}\n");
                    if (stripped) free(stripped);
                    srv->store->free_buf(data);
                    free(key); free(path); free(group);
                    free(artifact); free(ver_file); free(filename);
                    return 1;
                }
                if (pref == CT_JSON) {
                    /* serve JSON representation */
                    PastaResult pr;
                    PastaValue *root = pasta_parse(
                        (const char *)serve_data, serve_len, &pr);
                    if (root) {
                        char *json = pasta_to_json(root);
                        pasta_free(root);
                        if (json) {
                            size_t jlen = strlen(json);
                            METRIC_INC(srv->metrics.responses_2xx);
                            METRIC_ADD(srv->metrics.bytes_downloaded,
                                       (long)jlen);
                            mg_printf(conn,
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: application/json\r\n"
                                "Content-Length: %zu\r\n"
                                "%s"
                                "\r\n",
                                jlen,
                                yanked_hdrs);
                            mg_write(conn, json, jlen);
                            free(json);
                            if (stripped) free(stripped);
                            srv->store->free_buf(data);
                            free(key); free(path); free(group);
                            free(artifact); free(ver_file); free(filename);
                            return 1;
                        }
                    }
                    /* fallback to Pasta if parse/serialize fails */
                }
                /* CT_PASTA: check ?pretty query param */
                if (ri->query_string &&
                    strstr(ri->query_string, "pretty")) {
                    PastaResult pr;
                    PastaValue *root = pasta_parse(
                        (const char *)serve_data, serve_len, &pr);
                    if (root) {
                        char *pretty = pasta_write(root,
                            PASTA_PRETTY | PASTA_SORTED);
                        pasta_free(root);
                        if (pretty) {
                            size_t plen = strlen(pretty);
                            METRIC_INC(srv->metrics.responses_2xx);
                            METRIC_ADD(srv->metrics.bytes_downloaded,
                                       (long)plen);
                            mg_printf(conn,
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: application/x-pasta\r\n"
                                "Content-Length: %zu\r\n"
                                "%s"
                                "\r\n",
                                plen,
                                yanked_hdrs);
                            mg_write(conn, pretty, plen);
                            free(pretty);
                            if (stripped) free(stripped);
                            srv->store->free_buf(data);
                            free(key); free(path); free(group);
                            free(artifact); free(ver_file); free(filename);
                            return 1;
                        }
                    }
                    /* fallback to raw if pretty-print fails */
                }
            }

            METRIC_INC(srv->metrics.responses_2xx);
            METRIC_ADD(srv->metrics.bytes_downloaded, (long)serve_len);
            mg_printf(conn,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %zu\r\n"
                "%s"
                "\r\n",
                ct, serve_len,
                yanked_hdrs);
            mg_write(conn, serve_data, serve_len);

            if (stripped) free(stripped);
            srv->store->free_buf(data);
        }

    } else if (strcmp(ri->request_method, "PUT") == 0) {
        /* ---- PUT: publish artifact ---- */

        if (validate_group(group) != 0 || validate_artifact(artifact) != 0) {
            send_json(conn, 400,
                "{\"error\":\"Invalid group or artifact identifier\"}\n");
            free(key); free(path); free(group); free(artifact); free(ver_file); free(filename);
            return 1;
        }

        /* #10: JWT validation on PUT (if registry key is configured) */
        if (srv->has_registry_key) {
            cookbook_jwt_claims claims;
            if (extract_bearer_jwt(srv, ri, &claims) != 0) {
                METRIC_INC(srv->metrics.responses_4xx);
                METRIC_INC(srv->metrics.auth_failures);
                send_json(conn, 401,
                    "{\"error\":\"Valid Bearer JWT required\"}\n");
                free(key); free(path); free(group); free(artifact);
                free(ver_file); free(filename);
                return 1;
            }

            /* #11: group claim checking */
            if (!cookbook_jwt_has_group(&claims, group)) {
                METRIC_INC(srv->metrics.responses_4xx);
                METRIC_INC(srv->metrics.auth_failures);
                send_json(conn, 403,
                    "{\"error\":\"JWT does not authorize this group\"}\n");
                free(key); free(path); free(group); free(artifact);
                free(ver_file); free(filename);
                return 1;
            }

            /* #28: rate limiting per JWT sub */
            if (check_rate_limit(srv, claims.sub)) {
                METRIC_INC(srv->metrics.responses_4xx);
                send_json(conn, 429,
                    "{\"error\":\"Rate limit exceeded\"}\n");
                free(key); free(path); free(group); free(artifact);
                free(ver_file); free(filename);
                return 1;
            }
        }

        /* immutability check */
        if (srv->store->exists(srv->store, key) == COOKBOOK_STORE_OK) {
            send_json(conn, 409,
                "{\"error\":\"Release coordinate already published\"}\n");
            free(key); free(path); free(group); free(artifact); free(ver_file); free(filename);
            return 1;
        }

        /* read body */
        size_t body_len = 0;
        char *body = read_body(conn, ri, &body_len, srv->max_upload_bytes);
        if (!body || body_len == 0) {
            if (srv->max_upload_bytes > 0 && ri->content_length > 0 &&
                (size_t)ri->content_length > srv->max_upload_bytes) {
                send_json(conn, 413,
                    "{\"error\":\"Artifact exceeds maximum upload size\"}\n");
            } else {
                send_json(conn, 400, "{\"error\":\"Empty body\"}\n");
            }
            free(body); free(key); free(path);
            free(group); free(artifact); free(ver_file); free(filename);
            return 1;
        }

        /* #8: reject non-ASCII bytes on now.pasta PUT */
        if (filename && strcmp(filename, "now.pasta") == 0) {
            size_t bad = validate_ascii(body, body_len);
            if (bad) {
                char err[256];
                snprintf(err, sizeof(err),
                    "{\"error\":\"Non-ASCII byte at offset %zu"
                    " — Pasta requires US-ASCII input\"}\n", bad);
                METRIC_INC(srv->metrics.responses_4xx);
                send_json(conn, 400, err);
                free(body); free(key); free(path);
                free(group); free(artifact); free(ver_file); free(filename);
                return 1;
            }
        }

        /* compute SHA-256 */
        char sha256_hex[65];
        cookbook_sha256_hex(body, body_len, sha256_hex);

        /* store */
        cookbook_store_status sst = srv->store->put(srv->store, key,
                                                     body, body_len);
        if (sst != COOKBOOK_STORE_OK) {
            send_json(conn, 500, "{\"error\":\"Storage error\"}\n");
            free(body); free(key); free(path);
            free(group); free(artifact); free(ver_file); free(filename);
            return 1;
        }

        /* #6: extract triple from archive filename */
        char triple[128];
        extract_triple(filename, artifact, version_str, triple, sizeof(triple));

        /* #15: verify .sig files against publisher Ed25519 key */
        if (filename && strstr(filename, ".sig") != NULL &&
            srv->has_registry_key) {
            /* The .sig file is an Ed25519 signature over the corresponding
               artifact. Look up the publisher's public key from the DB. */
            /* For now, store the .sig; verification requires the publisher
               key to be registered (Phase C #12). The signature will be
               verified when the publisher key is available. */
        }

        /* #16: registry countersign — sign the SHA-256 with registry key */
        if (srv->has_registry_key) {
            unsigned char countersig[64];
            if (cookbook_sign(sha256_hex, 64, countersig,
                              srv->registry_sk) == 0) {
                char csig_key[512];
                snprintf(csig_key, sizeof(csig_key), "%s.countersig", key);
                srv->store->put(srv->store, csig_key, countersig, 64);
            }
        }

        /* if now.pasta, parse and register metadata */
        if (filename && strcmp(filename, "now.pasta") == 0) {
            PastaResult pr;
            PastaValue *root = pasta_parse(body, body_len, &pr);
            if (!root) {
                char err[512];
                snprintf(err, sizeof(err),
                    "{\"error\":\"Invalid now.pasta: %s\"}\n", pr.message);
                srv->store->del(srv->store, key);
                send_json(conn, 400, err);
                free(body); free(key); free(path);
                free(group); free(artifact); free(ver_file); free(filename);
                return 1;
            }

            const PastaValue *v_group = pasta_map_get(root, "group");
            const PastaValue *v_artifact = pasta_map_get(root, "artifact");
            const PastaValue *v_version = pasta_map_get(root, "version");

            if (v_group && v_artifact && v_version) {
                const char *grp = pasta_get_string(v_group);
                const char *art = pasta_get_string(v_artifact);
                const char *ver = pasta_get_string(v_version);

                if (grp && art && ver) {
                    /* #24: descriptor field validation */
                    int valid = 1;
                    for (const char *p = art; *p && valid; p++) {
                        if (*p >= 'A' && *p <= 'Z') valid = 0;
                    }
                    cookbook_semver sv;
                    if (cookbook_semver_parse(ver, &sv) != 0) valid = 0;
                    if (validate_group(grp) != 0) valid = 0;
                    if (validate_artifact(art) != 0) valid = 0;

                    if (!valid) {
                        pasta_free(root);
                        srv->store->del(srv->store, key);
                        send_json(conn, 400,
                            "{\"error\":\"Descriptor validation failed: "
                            "artifact must be lowercase, version must be "
                            "valid semver, group/artifact must match naming "
                            "rules\"}\n");
                        free(body); free(key); free(path);
                        free(group); free(artifact); free(ver_file); free(filename);
                        return 1;
                    }

                    /* #18: compute and store now.pasta.sha256 */
                    char desc_sha_key[512];
                    snprintf(desc_sha_key, sizeof(desc_sha_key),
                             "%s.sha256", key);
                    char desc_sha_content[65];
                    cookbook_sha256_hex(body, body_len, desc_sha_content);
                    srv->store->put(srv->store, desc_sha_key,
                                     desc_sha_content, 64);

                    /* ensure group exists */
                    {
                        const char *grp_sql =
                            "INSERT OR IGNORE INTO groups "
                            "(group_id, owner_sub) VALUES (?1, ?2)";
                        cookbook_db_param gp[] = {
                            COOKBOOK_P_TEXT(grp),
                            COOKBOOK_P_TEXT("anonymous")
                        };
                        srv->db->exec_p(srv->db, grp_sql, gp, 2);
                    }

                    int is_snapshot = (sv.pre_release[0] != '\0' &&
                                      strstr(sv.pre_release, "SNAPSHOT") != NULL)
                                     ? 1 : 0;

                    /* #19: two-phase write — insert as pending */
                    char now[64];
                    utc_now(now, sizeof(now));

                    {
                        char coord_id[256];
                        snprintf(coord_id, sizeof(coord_id),
                                 "%s:%s:%s:%s", grp, art, ver, triple);

                        const char *art_sql =
                            "INSERT OR IGNORE INTO artifacts "
                            "(coord_id, group_id, artifact, version, triple, "
                            " sha256, descriptor_sha256, snapshot, status, "
                            " size_bytes, pending_since) "
                            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, "
                            " 'pending', ?9, ?10)";
                        cookbook_db_param ap[] = {
                            COOKBOOK_P_TEXT(coord_id),
                            COOKBOOK_P_TEXT(grp),
                            COOKBOOK_P_TEXT(art),
                            COOKBOOK_P_TEXT(ver),
                            COOKBOOK_P_TEXT(triple),
                            COOKBOOK_P_TEXT(sha256_hex),
                            COOKBOOK_P_TEXT(desc_sha_content),
                            COOKBOOK_P_INT(is_snapshot),
                            COOKBOOK_P_INT((int64_t)body_len),
                            COOKBOOK_P_TEXT(now)
                        };
                        srv->db->exec_p(srv->db, art_sql, ap, 10);

                        const char *sv_sql =
                            "INSERT OR IGNORE INTO artifact_semver "
                            "(coord_id, major, minor, patch, pre_release, "
                            "build_meta) VALUES (?1, ?2, ?3, ?4, ?5, ?6)";
                        cookbook_db_param sp[] = {
                            COOKBOOK_P_TEXT(coord_id),
                            COOKBOOK_P_INT(sv.major),
                            COOKBOOK_P_INT(sv.minor),
                            COOKBOOK_P_INT(sv.patch),
                            COOKBOOK_P_TEXT(sv.pre_release),
                            COOKBOOK_P_TEXT(sv.build_meta)
                        };
                        srv->db->exec_p(srv->db, sv_sql, sp, 6);
                    }

                    /* #19: try to transition pending → published
                       (now.pasta was just uploaded, so this will publish) */
                    try_publish(srv, grp, art, ver, triple);
                }
            }

            pasta_free(root);
        } else {
            /* Non-pasta file (archive, .sig, etc.) — if an artifact record
               already exists as pending, try to publish it now that we have
               more files. */
            try_publish(srv, group, artifact, version_str, triple);
        }

        METRIC_INC(srv->metrics.responses_2xx);
        METRIC_INC(srv->metrics.artifacts_published);
        METRIC_ADD(srv->metrics.bytes_uploaded, (long)body_len);
        char resp[256];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"created\",\"sha256\":\"%s\",\"triple\":\"%s\"}\n",
            sha256_hex, triple);
        send_json(conn, 201, resp);
        free(body);

    } else {
        send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
    }

    free(key);
    free(path);
    free(group);
    free(artifact);
    free(ver_file); free(filename);
    return 1;
}

/* ==== F3: credential lookup helper ==== */

typedef struct {
    char hash[256];
    char groups[1024];
    int found;
} cred_lookup_ctx;

static int cred_lookup_cb(const cookbook_db_row *row, void *user) {
    cred_lookup_ctx *c = (cred_lookup_ctx *)user;
    c->found = 1;
    if (row->values[0]) {
        size_t l = strlen(row->values[0]);
        if (l >= sizeof(c->hash)) l = sizeof(c->hash) - 1;
        memcpy(c->hash, row->values[0], l);
        c->hash[l] = '\0';
    }
    if (row->values[1]) {
        size_t l = strlen(row->values[1]);
        if (l >= sizeof(c->groups)) l = sizeof(c->groups) - 1;
        memcpy(c->groups, row->values[1], l);
        c->groups[l] = '\0';
    }
    return 0;
}

/* ==== #9: route: POST /auth/token ==== */

static int handle_auth_token(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    METRIC_INC(srv->metrics.requests_total);
    METRIC_INC(srv->metrics.requests_post);

    if (strcmp(ri->request_method, "POST") != 0) {
        METRIC_INC(srv->metrics.responses_4xx);
        send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
        return 1;
    }

    if (!srv->has_registry_key) {
        send_json(conn, 503,
            "{\"error\":\"Auth not configured — no registry key\"}\n");
        return 1;
    }

    /* F3: credential verification via Authorization: Basic header.
       Format: "Basic base64(subject:token)"
       Falls back to JSON body for backwards compatibility when no
       credentials table is populated. */

    char sub[128] = {0}, groups[1024] = {0};
    int cred_verified = 0;

    const char *auth_hdr = NULL;
    for (int i = 0; i < ri->num_headers; i++) {
        if (strcasecmp(ri->http_headers[i].name, "Authorization") == 0) {
            auth_hdr = ri->http_headers[i].value;
            break;
        }
    }

    if (auth_hdr && strncmp(auth_hdr, "Basic ", 6) == 0) {
        /* decode base64(subject:token) */
        const char *b64 = auth_hdr + 6;
        size_t b64_len = strlen(b64);
        char decoded[512] = {0};
        size_t dec_len = cookbook_base64_decode(b64, b64_len,
                                                decoded, sizeof(decoded) - 1);
        if (dec_len == 0) {
            METRIC_INC(srv->metrics.responses_4xx);
            METRIC_INC(srv->metrics.auth_failures);
            send_json(conn, 401,
                "{\"error\":\"Invalid Basic auth encoding\"}\n");
            return 1;
        }
        decoded[dec_len] = '\0';

        /* split at first ':' */
        char *colon = strchr(decoded, ':');
        if (!colon || colon == decoded) {
            METRIC_INC(srv->metrics.responses_4xx);
            METRIC_INC(srv->metrics.auth_failures);
            send_json(conn, 401,
                "{\"error\":\"Invalid Basic auth format\"}\n");
            return 1;
        }
        *colon = '\0';
        const char *cred_sub = decoded;
        const char *cred_tok = colon + 1;

        /* copy subject */
        size_t slen = strlen(cred_sub);
        if (slen >= sizeof(sub)) slen = sizeof(sub) - 1;
        memcpy(sub, cred_sub, slen);
        sub[slen] = '\0';

        /* look up stored hash and groups */
        cred_lookup_ctx clctx = { {0}, {0}, 0 };

        cookbook_db_param cp[] = { COOKBOOK_P_TEXT(sub) };
        srv->db->query_p(srv->db,
            "SELECT token_hash, groups FROM credentials "
            "WHERE subject = ?1 AND revoked_at IS NULL",
            cp, 1, cred_lookup_cb, &clctx);

        if (clctx.found) {
            /* verify token against stored hash */
            if (cookbook_credential_verify(cred_tok, clctx.hash) != 0) {
                METRIC_INC(srv->metrics.responses_4xx);
                METRIC_INC(srv->metrics.auth_failures);
                send_json(conn, 401,
                    "{\"error\":\"Invalid credentials\"}\n");
                return 1;
            }
            /* use groups from credential store */
            memcpy(groups, clctx.groups, sizeof(groups));
            cred_verified = 1;
        }
        /* if no credential record found, fall through — open issuance */
    }

    if (!sub[0]) {
        /* no Basic auth header — fall back to JSON body */
        size_t body_len = 0;
        char *body = read_body(conn, ri, &body_len, 4096);
        if (!body || body_len == 0) {
            send_json(conn, 400,
                "{\"error\":\"Authorization header or body required\"}\n");
            free(body);
            return 1;
        }

        /* minimal JSON parse for sub and groups */
        {
            const char *p = strstr(body, "\"sub\":");
            if (p) {
                p += 6;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '"') {
                    p++;
                    const char *end = strchr(p, '"');
                    if (end) {
                        size_t len = (size_t)(end - p);
                        if (len >= sizeof(sub)) len = sizeof(sub) - 1;
                        memcpy(sub, p, len);
                        sub[len] = '\0';
                    }
                }
            }
        }
        {
            const char *p = strstr(body, "\"groups\":");
            if (p) {
                p += 9;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '"') {
                    p++;
                    const char *end = strchr(p, '"');
                    if (end) {
                        size_t len = (size_t)(end - p);
                        if (len >= sizeof(groups)) len = sizeof(groups) - 1;
                        memcpy(groups, p, len);
                        groups[len] = '\0';
                    }
                }
            }
        }
        free(body);

        if (!sub[0]) {
            send_json(conn, 400,
                "{\"error\":\"Missing 'sub' field\"}\n");
            return 1;
        }
    }

    /* Phase 2: try policy-based JWT v2 — resolve via alforno */
    char *resolved = cookbook_policy_resolve(srv->db, sub);
    char *token = NULL;
    int token_version = 1;

    if (resolved) {
        /* v2: embed resolved grants/exclude in JWT */
        token = cookbook_jwt_create_v2(sub, groups[0] ? groups : NULL,
                                        resolved, srv->jwt_ttl_sec,
                                        srv->registry_sk);
        token_version = 2;
        free(resolved);
    } else {
        /* v1 fallback: legacy comma-separated groups */
        token = cookbook_jwt_create(sub, groups, srv->jwt_ttl_sec,
                                     srv->registry_sk);
    }

    if (!token) {
        send_json(conn, 500, "{\"error\":\"Failed to create token\"}\n");
        return 1;
    }

    METRIC_INC(srv->metrics.responses_2xx);
    METRIC_INC(srv->metrics.auth_tokens_issued);
    char resp[8192];
    snprintf(resp, sizeof(resp),
        "{\"token\":\"%s\",\"expires_in\":%d,\"version\":%d}\n",
        token, srv->jwt_ttl_sec, token_version);
    free(token);

    send_json(conn, 200, resp);
    return 1;
}

/* ==== #12/#13: route: POST /keys, POST /keys/{id}/revoke ==== */

static int handle_keys(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    if (strcmp(ri->request_method, "POST") != 0) {
        send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
        return 1;
    }

    /* check for /keys/{id}/revoke */
    char *path = path_after(ri->local_uri, "/keys/");
    if (path && strlen(path) > 0) {
        /* #13: key revocation — /keys/{key_id}/revoke */
        size_t plen = strlen(path);
        if (plen > 7 && strcmp(path + plen - 7, "/revoke") == 0) {
            path[plen - 7] = '\0';
            const char *key_id = path;

            if (validate_path_segment(key_id) != 0) {
                send_json(conn, 400, "{\"error\":\"Invalid key ID\"}\n");
                free(path);
                return 1;
            }

            char now[64];
            utc_now(now, sizeof(now));

            const char *sql =
                "UPDATE publisher_keys SET revoked_at = ?1 "
                "WHERE key_id = ?2 AND revoked_at IS NULL";
            cookbook_db_param params[] = {
                COOKBOOK_P_TEXT(now),
                COOKBOOK_P_TEXT(key_id)
            };
            cookbook_db_status st = srv->db->exec_p(srv->db, sql, params, 2);
            if (st != COOKBOOK_DB_OK) {
                send_json(conn, 500, "{\"error\":\"Database error\"}\n");
            } else {
                send_json(conn, 200, "{\"status\":\"revoked\"}\n");
            }
            free(path);
            return 1;
        }
        free(path);
        send_json(conn, 400, "{\"error\":\"Unknown keys sub-route\"}\n");
        return 1;
    }
    free(path);

    /* #12: POST /keys — register a publisher key */
    size_t body_len = 0;
    char *body = read_body(conn, ri, &body_len, 4096);
    if (!body || body_len == 0) {
        send_json(conn, 400, "{\"error\":\"Request body required\"}\n");
        free(body);
        return 1;
    }

    /* parse: {"key_id":"...","group_id":"...","public_key":"...","comment":"..."} */
    char key_id[128] = {0}, group_id[128] = {0};
    char public_key[256] = {0}, comment[256] = {0};
    {
        const char *p;
        p = strstr(body, "\"key_id\":\"");
        if (p) { p += 10; const char *e = strchr(p, '"');
            if (e) { size_t l = (size_t)(e-p); if (l >= sizeof(key_id)) l = sizeof(key_id)-1;
                memcpy(key_id, p, l); key_id[l] = '\0'; } }
        p = strstr(body, "\"group_id\":\"");
        if (p) { p += 12; const char *e = strchr(p, '"');
            if (e) { size_t l = (size_t)(e-p); if (l >= sizeof(group_id)) l = sizeof(group_id)-1;
                memcpy(group_id, p, l); group_id[l] = '\0'; } }
        p = strstr(body, "\"public_key\":\"");
        if (p) { p += 14; const char *e = strchr(p, '"');
            if (e) { size_t l = (size_t)(e-p); if (l >= sizeof(public_key)) l = sizeof(public_key)-1;
                memcpy(public_key, p, l); public_key[l] = '\0'; } }
        p = strstr(body, "\"comment\":\"");
        if (p) { p += 11; const char *e = strchr(p, '"');
            if (e) { size_t l = (size_t)(e-p); if (l >= sizeof(comment)) l = sizeof(comment)-1;
                memcpy(comment, p, l); comment[l] = '\0'; } }
    }
    free(body);

    if (!key_id[0] || !group_id[0] || !public_key[0]) {
        send_json(conn, 400,
            "{\"error\":\"Missing key_id, group_id, or public_key\"}\n");
        return 1;
    }

    char now[64];
    utc_now(now, sizeof(now));

    const char *sql =
        "INSERT INTO publisher_keys "
        "(key_id, group_id, public_key, comment, added_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5)";
    cookbook_db_param params[] = {
        COOKBOOK_P_TEXT(key_id),
        COOKBOOK_P_TEXT(group_id),
        COOKBOOK_P_TEXT(public_key),
        COOKBOOK_P_TEXT(comment),
        COOKBOOK_P_TEXT(now)
    };
    cookbook_db_status st = srv->db->exec_p(srv->db, sql, params, 5);

    if (st == COOKBOOK_DB_CONSTRAINT) {
        send_json(conn, 409, "{\"error\":\"Key ID already exists\"}\n");
    } else if (st != COOKBOOK_DB_OK) {
        send_json(conn, 500, "{\"error\":\"Database error\"}\n");
    } else {
        send_json(conn, 201, "{\"status\":\"registered\"}\n");
    }

    return 1;
}

/* ==== #3: route: GET /metrics (Prometheus) ==== */

static int handle_metrics(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    if (strcmp(ri->request_method, "GET") != 0) {
        send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
        return 1;
    }

    cookbook_metrics *m = &srv->metrics;
    char body[4096];
    int n = snprintf(body, sizeof(body),
        "# HELP cookbook_requests_total Total HTTP requests.\n"
        "# TYPE cookbook_requests_total counter\n"
        "cookbook_requests_total %ld\n"
        "# HELP cookbook_requests_by_method HTTP requests by method.\n"
        "# TYPE cookbook_requests_by_method counter\n"
        "cookbook_requests_by_method{method=\"GET\"} %ld\n"
        "cookbook_requests_by_method{method=\"PUT\"} %ld\n"
        "cookbook_requests_by_method{method=\"POST\"} %ld\n"
        "# HELP cookbook_responses_by_status HTTP responses by status class.\n"
        "# TYPE cookbook_responses_by_status counter\n"
        "cookbook_responses_by_status{class=\"2xx\"} %ld\n"
        "cookbook_responses_by_status{class=\"4xx\"} %ld\n"
        "cookbook_responses_by_status{class=\"5xx\"} %ld\n"
        "# HELP cookbook_artifacts_published_total Artifacts published.\n"
        "# TYPE cookbook_artifacts_published_total counter\n"
        "cookbook_artifacts_published_total %ld\n"
        "# HELP cookbook_artifacts_yanked_total Artifacts yanked.\n"
        "# TYPE cookbook_artifacts_yanked_total counter\n"
        "cookbook_artifacts_yanked_total %ld\n"
        "# HELP cookbook_artifacts_resolved_total Version resolutions performed.\n"
        "# TYPE cookbook_artifacts_resolved_total counter\n"
        "cookbook_artifacts_resolved_total %ld\n"
        "# HELP cookbook_auth_tokens_issued_total JWT tokens issued.\n"
        "# TYPE cookbook_auth_tokens_issued_total counter\n"
        "cookbook_auth_tokens_issued_total %ld\n"
        "# HELP cookbook_auth_failures_total Authentication failures.\n"
        "# TYPE cookbook_auth_failures_total counter\n"
        "cookbook_auth_failures_total %ld\n"
        "# HELP cookbook_bytes_uploaded_total Bytes uploaded.\n"
        "# TYPE cookbook_bytes_uploaded_total counter\n"
        "cookbook_bytes_uploaded_total %ld\n"
        "# HELP cookbook_bytes_downloaded_total Bytes downloaded.\n"
        "# TYPE cookbook_bytes_downloaded_total counter\n"
        "cookbook_bytes_downloaded_total %ld\n",
        m->requests_total,
        m->requests_get, m->requests_put, m->requests_post,
        m->responses_2xx, m->responses_4xx, m->responses_5xx,
        m->artifacts_published, m->artifacts_yanked,
        m->artifacts_resolved,
        m->auth_tokens_issued, m->auth_failures,
        m->bytes_uploaded, m->bytes_downloaded);

    mg_printf(conn,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s", n, body);
    return 1;
}

/* ==== #25: route: GET /mirror/manifest ==== */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    int    count;
    const char *registry_id;
} mirror_ctx;

static int mirror_collect_cb(const cookbook_db_row *row, void *user) {
    mirror_ctx *ctx = (mirror_ctx *)user;
    /* row: group_id, artifact, version, triple */
    const char *grp = row->values[0];
    const char *art = row->values[1];
    const char *ver = row->values[2];

    if (!grp || !art || !ver) return 0;

    /* convert group dots to slashes for store path */
    char grp_path[256];
    snprintf(grp_path, sizeof(grp_path), "%s", grp);
    for (char *p = grp_path; *p; p++)
        if (*p == '.') *p = '/';

    /* list the files that should exist for this artifact:
       now.pasta, now.pasta.sha256, and any archive files.
       We output the store key prefix so the mirror client knows
       which paths to fetch. */
    int n;
    if (ctx->count > 0) {
        n = snprintf(ctx->buf + ctx->len, ctx->cap - ctx->len, ",");
        if (n > 0) ctx->len += (size_t)n;
    }
    n = snprintf(ctx->buf + ctx->len, ctx->cap - ctx->len,
        "{\"group\":\"%s\",\"artifact\":\"%s\",\"version\":\"%s\","
        "\"base_path\":\"%s/%s/%s/%s\"}",
        grp, art, ver,
        ctx->registry_id, grp_path, art, ver);
    if (n > 0) ctx->len += (size_t)n;
    ctx->count++;
    return 0;
}

static int handle_mirror_manifest(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    METRIC_INC(srv->metrics.requests_total);
    METRIC_INC(srv->metrics.requests_get);

    if (strcmp(ri->request_method, "GET") != 0) {
        METRIC_INC(srv->metrics.responses_4xx);
        send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
        return 1;
    }

    /* parse ?coords=group:artifact:range,group:artifact:range,... */
    const char *coords_param = NULL;
    if (ri->query_string)
        coords_param = strstr(ri->query_string, "coords=");

    if (!coords_param) {
        /* return all published artifacts */
        const char *sql =
            "SELECT DISTINCT group_id, artifact, version, triple "
            "FROM artifacts WHERE status = 'published' AND yanked = 0 "
            "ORDER BY group_id, artifact, version";

        char result_buf[65536] = {0};
        mirror_ctx ctx = { result_buf, 0, sizeof(result_buf), 0,
                           srv->registry_id };

        cookbook_db_status st = srv->db->query(srv->db, sql,
                                               mirror_collect_cb, &ctx);
        if (st != COOKBOOK_DB_OK) {
            METRIC_INC(srv->metrics.responses_5xx);
            send_json(conn, 500, "{\"error\":\"Database error\"}\n");
            return 1;
        }

        /* G5: grid-aware manifest — merge peer manifests */
        int grid_mode = 0;
        if (srv->grid_enabled && ri->query_string &&
            strstr(ri->query_string, "grid=true"))
            grid_mode = 1;

        if (grid_mode) {
            cookbook_peer *peers = NULL;
            int npeers = cookbook_grid_load_peers(srv->db, &peers);
            for (int pi = 0; pi < npeers; pi++) {
                cookbook_grid_response gresp;
                if (cookbook_grid_get(&peers[pi], "/grid/manifest",
                        srv->registry_id, NULL, 0, &gresp) == 0
                    && gresp.status == 200 && gresp.body) {
                    /* extract "artifacts":[ ... ] from peer response */
                    const char *as = strstr(gresp.body, "\"artifacts\":[");
                    if (as) {
                        as += 13; /* skip "artifacts":[ */
                        const char *ae = strrchr(as, ']');
                        if (ae && ae > as) {
                            size_t alen = (size_t)(ae - as);
                            if (alen < ctx.cap - ctx.len - 2) {
                                if (ctx.count > 0 && ctx.len > 0)
                                    result_buf[ctx.len++] = ',';
                                memcpy(result_buf + ctx.len, as, alen);
                                ctx.len += alen;
                                result_buf[ctx.len] = '\0';
                                ctx.count++;
                            }
                        }
                    }
                    free(gresp.body);
                }
            }
            cookbook_grid_free_peers(peers, npeers);
        }

        char *response = malloc(ctx.len + 128);
        if (!response) {
            METRIC_INC(srv->metrics.responses_5xx);
            send_json(conn, 500, "{\"error\":\"Out of memory\"}\n");
            return 1;
        }
        int rlen = sprintf(response,
            "{\"registry\":\"%s\",\"artifacts\":[%s]}\n",
            srv->registry_id, result_buf);

        METRIC_INC(srv->metrics.responses_2xx);
        mg_printf(conn,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s", rlen, response);
        free(response);
        return 1;
    }

    /* parse specific coordinates from query param */
    coords_param += 7; /* skip "coords=" */
    char coords_copy[4096];
    snprintf(coords_copy, sizeof(coords_copy), "%s", coords_param);

    /* URL-decode the coords */
    url_decode(coords_copy, strlen(coords_copy));

    char result_buf[65536] = {0};
    mirror_ctx ctx = { result_buf, 0, sizeof(result_buf), 0,
                       srv->registry_id };

    /* split on comma, each entry is "group:artifact" or "group:artifact:version" */
    char *saveptr = NULL;
    char *tok = strtok_r(coords_copy, ",", &saveptr);
    while (tok) {
        /* parse group:artifact or group:artifact:version */
        char grp[128] = {0}, art[64] = {0}, ver[64] = {0};
        char *c1 = strchr(tok, ':');
        if (c1) {
            size_t gl = (size_t)(c1 - tok);
            if (gl >= sizeof(grp)) gl = sizeof(grp) - 1;
            memcpy(grp, tok, gl);
            grp[gl] = '\0';

            char *c2 = strchr(c1 + 1, ':');
            if (c2) {
                size_t al = (size_t)(c2 - c1 - 1);
                if (al >= sizeof(art)) al = sizeof(art) - 1;
                memcpy(art, c1 + 1, al);
                art[al] = '\0';
                snprintf(ver, sizeof(ver), "%s", c2 + 1);
            } else {
                snprintf(art, sizeof(art), "%s", c1 + 1);
            }
        }

        if (grp[0] && art[0]) {
            const char *sql;
            if (ver[0]) {
                sql = "SELECT group_id, artifact, version, triple "
                      "FROM artifacts WHERE group_id = ?1 AND artifact = ?2 "
                      "AND version = ?3 AND status = 'published'";
                cookbook_db_param params[] = {
                    COOKBOOK_P_TEXT(grp),
                    COOKBOOK_P_TEXT(art),
                    COOKBOOK_P_TEXT(ver)
                };
                srv->db->query_p(srv->db, sql, params, 3,
                                  mirror_collect_cb, &ctx);
            } else {
                sql = "SELECT group_id, artifact, version, triple "
                      "FROM artifacts WHERE group_id = ?1 AND artifact = ?2 "
                      "AND status = 'published'";
                cookbook_db_param params[] = {
                    COOKBOOK_P_TEXT(grp),
                    COOKBOOK_P_TEXT(art)
                };
                srv->db->query_p(srv->db, sql, params, 2,
                                  mirror_collect_cb, &ctx);
            }
        }

        tok = strtok_r(NULL, ",", &saveptr);
    }

    char *response = malloc(ctx.len + 128);
    if (!response) {
        METRIC_INC(srv->metrics.responses_5xx);
        send_json(conn, 500, "{\"error\":\"Out of memory\"}\n");
        return 1;
    }
    int rlen = sprintf(response,
        "{\"registry\":\"%s\",\"artifacts\":[%s]}\n",
        srv->registry_id, result_buf);

    METRIC_INC(srv->metrics.responses_2xx);
    mg_printf(conn,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s", rlen, response);
    free(response);
    return 1;
}

/* ==== grid helpers ==== */

/* Extract grid hop headers from request. */
static int grid_get_hop_count(const struct mg_request_info *ri) {
    for (int i = 0; i < ri->num_headers; i++) {
        if (strcasecmp(ri->http_headers[i].name,
                       "X-Cookbook-Hop-Count") == 0)
            return atoi(ri->http_headers[i].value);
    }
    return 0;
}

static const char *grid_get_via(const struct mg_request_info *ri) {
    for (int i = 0; i < ri->num_headers; i++) {
        if (strcasecmp(ri->http_headers[i].name,
                       "X-Cookbook-Via") == 0)
            return ri->http_headers[i].value;
    }
    return NULL;
}

/* ==== route: /grid/resolve/ (internal, local-only) ==== */

static int handle_grid_resolve(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    if (strcmp(ri->request_method, "GET") != 0) {
        send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
        return 1;
    }

    /* loop detection */
    const char *via = grid_get_via(ri);
    int hop = grid_get_hop_count(ri);
    if (hop > srv->grid_max_hops) {
        send_json(conn, 508, "{\"error\":\"Grid hop limit exceeded\"}\n");
        return 1;
    }
    if (via && cookbook_grid_is_loop(srv->registry_id, via)) {
        send_json(conn, 508, "{\"error\":\"Grid loop detected\"}\n");
        return 1;
    }

    /* reuse resolve logic but path is /grid/resolve/... */
    char *path = path_after(ri->local_uri, "/grid/resolve/");
    if (!path) {
        send_json(conn, 400, "{\"error\":\"Bad request\"}\n");
        return 1;
    }

    char *group = NULL, *artifact = NULL, *range_str = NULL;
    if (split_coord(path, &group, &artifact, &range_str) != 0 || !range_str) {
        send_json(conn, 400, "{\"error\":\"Malformed path\"}\n");
        free(path); free(group); free(artifact); free(range_str);
        return 1;
    }

    cookbook_range range;
    if (cookbook_range_parse(range_str, &range) != 0) {
        send_json(conn, 400, "{\"error\":\"Malformed range\"}\n");
        free(path); free(group); free(artifact); free(range_str);
        return 1;
    }

    int include_snapshots = 0;
    if (ri->query_string && strstr(ri->query_string, "snapshot=true"))
        include_snapshots = 1;
    int include_yanked = 0;
    if (ri->query_string && strstr(ri->query_string, "include_yanked=true"))
        include_yanked = 1;

    const char *sql = include_yanked
        ? "SELECT a.version, a.snapshot, a.triple, a.yanked, a.yank_reason "
          "FROM artifacts a "
          "JOIN artifact_semver s ON a.coord_id = s.coord_id "
          "WHERE a.group_id = ?1 AND a.artifact = ?2 "
          "AND a.status = 'published' "
          "ORDER BY s.major DESC, s.minor DESC, s.patch DESC"
        : "SELECT a.version, a.snapshot, a.triple "
          "FROM artifacts a "
          "JOIN artifact_semver s ON a.coord_id = s.coord_id "
          "WHERE a.group_id = ?1 AND a.artifact = ?2 "
          "AND a.yanked = 0 AND a.status = 'published' "
          "ORDER BY s.major DESC, s.minor DESC, s.patch DESC";

    cookbook_db_param params[] = {
        COOKBOOK_P_TEXT(group), COOKBOOK_P_TEXT(artifact)
    };

    char result_buf[8192] = {0};
    resolve_filter_ctx ctx = {
        &range, include_snapshots, include_yanked,
        result_buf, 0, sizeof(result_buf), 0
    };

    srv->db->query_p(srv->db, sql, params, 2,
                      resolve_filter_cb, &ctx);

    /* always respond JSON for grid internal calls */
    char response[8320];
    snprintf(response, sizeof(response),
             "{\"versions\":[%s],\"source\":\"%s\"}\n",
             result_buf, srv->registry_id);
    send_json(conn, 200, response);

    free(path); free(group); free(artifact); free(range_str);
    return 1;
}

/* ==== route: /grid/artifact/ (internal, local-only) ==== */

static int handle_grid_artifact(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    /* loop detection */
    const char *via = grid_get_via(ri);
    int hop = grid_get_hop_count(ri);
    if (hop > srv->grid_max_hops) {
        send_json(conn, 508, "{\"error\":\"Grid hop limit exceeded\"}\n");
        return 1;
    }
    if (via && cookbook_grid_is_loop(srv->registry_id, via)) {
        send_json(conn, 508, "{\"error\":\"Grid loop detected\"}\n");
        return 1;
    }

    char *path = path_after(ri->local_uri, "/grid/artifact/");
    if (!path) {
        send_json(conn, 400, "{\"error\":\"Bad request\"}\n");
        return 1;
    }

    if (strcmp(ri->request_method, "HEAD") == 0) {
        /* existence check: just probe the object store */
        size_t key_len = strlen(srv->registry_id) + 1 + strlen(path);
        char *key = malloc(key_len + 1);
        snprintf(key, key_len + 1, "%s/%s", srv->registry_id, path);

        int exists = srv->store->exists(srv->store, key);
        free(key);
        free(path);

        if (exists) {
            mg_printf(conn, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
        } else {
            mg_printf(conn, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
        }
        return 1;
    }

    if (strcmp(ri->request_method, "GET") == 0) {
        /* serve artifact from local store */
        size_t key_len = strlen(srv->registry_id) + 1 + strlen(path);
        char *key = malloc(key_len + 1);
        snprintf(key, key_len + 1, "%s/%s", srv->registry_id, path);

        void *data = NULL;
        size_t len = 0;
        cookbook_store_status sst = srv->store->get(srv->store, key, &data, &len);
        free(key);

        if (sst == COOKBOOK_STORE_NOT_FOUND) {
            send_json(conn, 404, "{\"error\":\"Not found\"}\n");
        } else if (sst != COOKBOOK_STORE_OK) {
            send_json(conn, 500, "{\"error\":\"Storage error\"}\n");
        } else {
            mg_printf(conn,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Content-Length: %zu\r\n"
                "X-Cookbook-Source: %s\r\n"
                "\r\n",
                len, srv->registry_id);
            mg_write(conn, data, len);
            srv->store->free_buf(data);
        }
        free(path);
        return 1;
    }

    send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
    free(path);
    return 1;
}

/* ==== route: /grid/manifest (internal, local-only) ==== */

static int handle_grid_manifest(struct mg_connection *conn, void *cbdata) {
    /* delegates to existing mirror manifest logic (local-only) */
    return handle_mirror_manifest(conn, cbdata);
}

/* ==== route: /admin/peers ==== */

static int handle_admin_peers(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    if (strcmp(ri->request_method, "GET") == 0) {
        /* list peers */
        cookbook_peer *peers = NULL;
        int n = cookbook_grid_load_peers(srv->db, &peers);

        char buf[8192] = {0};
        size_t pos = 0;
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "{\"peers\":[");
        for (int i = 0; i < n; i++) {
            if (i > 0)
                pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ",");
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                "{\"peer_id\":\"%s\",\"name\":\"%s\",\"url\":\"%s\","
                "\"mode\":\"%s\",\"priority\":%d}",
                peers[i].peer_id, peers[i].name, peers[i].url,
                peers[i].mode == 'p' ? "proxy" : "redirect",
                peers[i].priority);
        }
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "]}\n");

        cookbook_grid_free_peers(peers, n);
        send_json(conn, 200, buf);
        return 1;
    }

    if (strcmp(ri->request_method, "PUT") == 0) {
        /* add/update peer */
        size_t body_len = 0;
        char *body = read_body(conn, ri, &body_len, 4096);
        if (!body) {
            send_json(conn, 400, "{\"error\":\"Body required\"}\n");
            return 1;
        }

        /* minimal JSON parse for peer fields */
        char peer_id[128] = {0}, name[128] = {0}, url[512] = {0}, mode[16] = {0};
        int priority = 100;

        #define GRID_PARSE_STR(field, key, sz) do { \
            const char *_p = strstr(body, "\"" key "\":"); \
            if (_p) { \
                _p += strlen("\"" key "\":"); \
                while (*_p == ' ' || *_p == '\t') _p++; \
                if (*_p == '"') { \
                    _p++; \
                    const char *_e = strchr(_p, '"'); \
                    if (_e) { \
                        size_t _l = (size_t)(_e - _p); \
                        if (_l >= (sz)) _l = (sz) - 1; \
                        memcpy(field, _p, _l); \
                        field[_l] = '\0'; \
                    } \
                } \
            } \
        } while(0)

        GRID_PARSE_STR(peer_id, "peer_id", sizeof(peer_id));
        GRID_PARSE_STR(name, "name", sizeof(name));
        GRID_PARSE_STR(url, "url", sizeof(url));
        GRID_PARSE_STR(mode, "mode", sizeof(mode));
        #undef GRID_PARSE_STR

        /* parse priority */
        const char *pp = strstr(body, "\"priority\":");
        if (pp) priority = atoi(pp + 11);

        free(body);

        if (!peer_id[0] || !name[0] || !url[0]) {
            send_json(conn, 400,
                "{\"error\":\"peer_id, name, and url required\"}\n");
            return 1;
        }

        char mode_char = (strcmp(mode, "proxy") == 0) ? 'p' : 'r';
        const char *mode_str = mode_char == 'p' ? "proxy" : "redirect";

        cookbook_db_param pp2[] = {
            COOKBOOK_P_TEXT(peer_id),
            COOKBOOK_P_TEXT(name),
            COOKBOOK_P_TEXT(url),
            COOKBOOK_P_TEXT(mode_str),
            COOKBOOK_P_INT(priority)
        };
        /* upsert: try insert, on conflict update */
        cookbook_db_status st = srv->db->exec_p(srv->db,
            "INSERT OR REPLACE INTO peers "
            "(peer_id, name, url, mode, priority, enabled, added_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, 1, datetime('now'))",
            pp2, 5);

        if (st != COOKBOOK_DB_OK) {
            send_json(conn, 500, "{\"error\":\"Database error\"}\n");
        } else {
            send_json(conn, 200, "{\"status\":\"ok\"}\n");
        }
        return 1;
    }

    if (strcmp(ri->request_method, "DELETE") == 0) {
        char *path = path_after(ri->local_uri, "/admin/peers/");
        if (!path || !path[0]) {
            send_json(conn, 400, "{\"error\":\"Peer ID required\"}\n");
            free(path);
            return 1;
        }
        cookbook_db_param dp[] = { COOKBOOK_P_TEXT(path) };
        srv->db->exec_p(srv->db,
            "DELETE FROM peers WHERE peer_id = ?1", dp, 1);
        send_json(conn, 200, "{\"status\":\"deleted\"}\n");
        free(path);
        return 1;
    }

    send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
    return 1;
}

/* ==== route: /admin/policies ==== */

typedef struct {
    char  *buf;
    size_t pos;
    size_t cap;
    int    count;
} policy_list_ctx;

static int policy_list_cb(const cookbook_db_row *row, void *user) {
    policy_list_ctx *ctx = (policy_list_ctx *)user;
    if (row->ncols < 3 || !row->values[0]) return 0;
    const char *sub  = row->values[0];
    const char *kind = row->values[1] ? row->values[1] : "user";
    const char *upd  = row->values[2] ? row->values[2] : "";

    int n;
    if (ctx->count > 0) {
        n = snprintf(ctx->buf + ctx->pos, ctx->cap - ctx->pos, ",");
        if (n > 0) ctx->pos += (size_t)n;
    }
    n = snprintf(ctx->buf + ctx->pos, ctx->cap - ctx->pos,
        "{\"subject\":\"%s\",\"kind\":\"%s\",\"updated_at\":\"%s\"}",
        sub, kind, upd);
    if (n > 0) ctx->pos += (size_t)n;
    ctx->count++;
    return 0;
}

static int handle_admin_policies(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    if (strcmp(ri->request_method, "GET") == 0) {
        /* check for specific subject: /admin/policies/{subject} */
        char *path = path_after(ri->local_uri, "/admin/policies/");
        if (path && path[0]) {
            /* check for /effective suffix */
            char *eff = strstr(path, "/effective");
            if (eff) {
                *eff = '\0'; /* truncate to get subject */
                char *json = cookbook_policy_resolve(srv->db, path);
                free(path);
                if (!json) {
                    send_json(conn, 404,
                        "{\"error\":\"Policy not found or resolution failed\"}\n");
                    return 1;
                }
                size_t jlen = strlen(json);
                mg_printf(conn,
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %zu\r\n"
                    "\r\n"
                    "%s", jlen, json);
                free(json);
                return 1;
            }

            /* get specific policy */
            char *pastlet = cookbook_policy_get(srv->db, path);
            free(path);
            if (!pastlet) {
                send_json(conn, 404, "{\"error\":\"Policy not found\"}\n");
                return 1;
            }
            /* return as application/x-pasta */
            size_t plen = strlen(pastlet);
            mg_printf(conn,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/x-pasta\r\n"
                "Content-Length: %zu\r\n"
                "\r\n"
                "%s", plen, pastlet);
            free(pastlet);
            return 1;
        }
        free(path);

        /* list all policies */
        char lbuf[8192] = {0};
        policy_list_ctx plctx = { lbuf, 0, sizeof(lbuf), 0 };
        srv->db->query(srv->db,
            "SELECT subject, kind, updated_at FROM policies ORDER BY subject",
            policy_list_cb, &plctx);

        char resp[8320];
        int rlen = snprintf(resp, sizeof(resp),
            "{\"policies\":[%s]}\n", lbuf);
        mg_printf(conn,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s", rlen, resp);
        return 1;
    }

    if (strcmp(ri->request_method, "PUT") == 0) {
        char *path = path_after(ri->local_uri, "/admin/policies/");
        if (!path || !path[0]) {
            free(path);
            send_json(conn, 400, "{\"error\":\"Subject required in path\"}\n");
            return 1;
        }
        char subject[128];
        snprintf(subject, sizeof(subject), "%s", path);
        free(path);

        /* read body — the pastlet */
        size_t body_len = 0;
        char *body = read_body(conn, ri, &body_len, 65536);
        if (!body || body_len == 0) {
            free(body);
            send_json(conn, 400, "{\"error\":\"Pastlet body required\"}\n");
            return 1;
        }

        /* validate it's parseable pasta */
        PastaResult pr;
        PastaValue *test = pasta_parse(body, body_len, &pr);
        if (!test) {
            free(body);
            char err[512];
            snprintf(err, sizeof(err),
                "{\"error\":\"Invalid pasta at %d:%d: %s\"}\n",
                pr.line, pr.col, pr.message);
            send_json(conn, 400, err);
            return 1;
        }
        pasta_free(test);

        /* extract kind from pastlet if present */
        const char *kind = "user";
        PastaValue *v2 = pasta_parse(body, body_len, NULL);
        if (v2) {
            const PastaValue *id = pasta_map_get(v2, "identity");
            if (id && pasta_type(id) == PASTA_MAP) {
                const PastaValue *kv = pasta_map_get(id, "kind");
                if (kv && pasta_type(kv) == PASTA_STRING) {
                    const char *ks = pasta_get_string(kv);
                    if (strcmp(ks, "team") == 0) kind = "team";
                }
            }
            pasta_free(v2);
        }

        if (cookbook_policy_put(srv->db, subject, kind, body) != 0) {
            free(body);
            send_json(conn, 500, "{\"error\":\"Database error\"}\n");
            return 1;
        }
        free(body);
        send_json(conn, 200, "{\"status\":\"ok\"}\n");
        return 1;
    }

    if (strcmp(ri->request_method, "DELETE") == 0) {
        char *path = path_after(ri->local_uri, "/admin/policies/");
        if (!path || !path[0]) {
            free(path);
            send_json(conn, 400, "{\"error\":\"Subject required in path\"}\n");
            return 1;
        }
        cookbook_policy_delete(srv->db, path);
        free(path);
        send_json(conn, 200, "{\"status\":\"deleted\"}\n");
        return 1;
    }

    send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
    return 1;
}

/* ==== public API ==== */

cookbook_server *cookbook_server_start(const cookbook_server_opts *opts) {
    if (!opts || !opts->db || !opts->store) return NULL;

    cookbook_server *srv = calloc(1, sizeof(*srv));
    if (!srv) return NULL;

    srv->db    = opts->db;
    srv->store = opts->store;
    srv->registry_id = strdup(opts->registry_id ? opts->registry_id : "central");
    srv->max_upload_bytes = opts->max_upload_mb > 0
        ? (size_t)opts->max_upload_mb * 1024 * 1024
        : 0;
    srv->pending_timeout_sec = opts->pending_timeout_sec > 0
        ? opts->pending_timeout_sec
        : 3600;  /* default 1 hour */
    srv->jwt_ttl_sec = opts->jwt_ttl_sec > 0 ? opts->jwt_ttl_sec : 3600;
    srv->rate_limit_per_min = opts->rate_limit_per_min;
    srv->grid_enabled = opts->grid_enabled;
    srv->grid_max_hops = opts->grid_max_hops > 0
        ? opts->grid_max_hops
        : COOKBOOK_GRID_MAX_HOPS_DEFAULT;

    /* initialize rate limiter lock */
#ifdef _WIN32
    InitializeCriticalSection(&srv->rate_lock);
#else
    pthread_mutex_init(&srv->rate_lock, NULL);
#endif

    /* registry Ed25519 key pair */
    if (opts->registry_pk && opts->registry_sk) {
        memcpy(srv->registry_pk, opts->registry_pk, 32);
        memcpy(srv->registry_sk, opts->registry_sk, 64);
        srv->has_registry_key = 1;
    }

    /* extract port from listen_url */
    const char *url = opts->listen_url ? opts->listen_url : "http://0.0.0.0:8080";
    const char *port = "8080";
    const char *colon = strrchr(url, ':');
    if (colon) port = colon + 1;

    const char *civetweb_opts[] = {
        "listening_ports", port,
        "num_threads", "4",
        "request_timeout_ms", "30000",
        NULL
    };

    srv->ctx = mg_start(NULL, NULL, civetweb_opts);
    if (!srv->ctx) {
        fprintf(stderr, "cookbook: failed to start civetweb on port %s\n", port);
        free(srv->registry_id);
        free(srv);
        return NULL;
    }

    /* register route handlers */
    mg_set_request_handler(srv->ctx, "/healthz", handle_healthz, srv);
    mg_set_request_handler(srv->ctx, "/readyz", handle_readyz, srv);
    mg_set_request_handler(srv->ctx, "/.well-known/now-registry-key",
                           handle_registry_key, srv);
    mg_set_request_handler(srv->ctx, "/auth/token", handle_auth_token, srv);
    mg_set_request_handler(srv->ctx, "/keys", handle_keys, srv);
    mg_set_request_handler(srv->ctx, "/metrics", handle_metrics, srv);
    mg_set_request_handler(srv->ctx, "/mirror/manifest", handle_mirror_manifest, srv);
    mg_set_request_handler(srv->ctx, "/resolve/", handle_resolve, srv);
    mg_set_request_handler(srv->ctx, "/artifact/", handle_artifact, srv);

    /* grid federation endpoints */
    if (srv->grid_enabled) {
        mg_set_request_handler(srv->ctx, "/grid/resolve/",
                               handle_grid_resolve, srv);
        mg_set_request_handler(srv->ctx, "/grid/artifact/",
                               handle_grid_artifact, srv);
        mg_set_request_handler(srv->ctx, "/grid/manifest",
                               handle_grid_manifest, srv);
        mg_set_request_handler(srv->ctx, "/admin/peers",
                               handle_admin_peers, srv);
    }

    /* auth v2: policy admin endpoints */
    mg_set_request_handler(srv->ctx, "/admin/policies",
                           handle_admin_policies, srv);

    /* #20: start reconciliation thread */
    srv->reconcile_running = 1;
#ifdef _WIN32
    srv->reconcile_thread = CreateThread(NULL, 0, reconcile_thread_fn,
                                          srv, 0, NULL);
#else
    pthread_create(&srv->reconcile_thread, NULL, reconcile_thread_fn, srv);
#endif

    fprintf(stdout, "cookbook: listening on %s (registry: %s)\n",
            url, srv->registry_id);
    if (srv->max_upload_bytes > 0)
        fprintf(stdout, "cookbook: max upload size: %zu MB\n",
                srv->max_upload_bytes / (1024 * 1024));
    fprintf(stdout, "cookbook: pending timeout: %d sec\n",
            srv->pending_timeout_sec);
    fprintf(stdout, "cookbook: auth: %s\n",
            srv->has_registry_key ? "enabled (Ed25519)" : "disabled (no key)");
    if (srv->rate_limit_per_min > 0)
        fprintf(stdout, "cookbook: rate limit: %d req/min per subject\n",
                srv->rate_limit_per_min);
    if (srv->grid_enabled)
        fprintf(stdout, "cookbook: grid federation: enabled (max hops: %d)\n",
                srv->grid_max_hops);
    return srv;
}

int cookbook_server_poll(cookbook_server *srv, int timeout_ms) {
    if (!srv || !srv->ctx) return -1;
#ifdef _WIN32
    Sleep((DWORD)timeout_ms);
#else
    struct timespec ts = { timeout_ms / 1000, (timeout_ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
    return 0;
}

void cookbook_server_stop(cookbook_server *srv) {
    if (!srv) return;

    /* stop reconciliation thread */
    srv->reconcile_running = 0;
#ifdef _WIN32
    if (srv->reconcile_thread) {
        WaitForSingleObject(srv->reconcile_thread, 5000);
        CloseHandle(srv->reconcile_thread);
    }
#else
    pthread_join(srv->reconcile_thread, NULL);
#endif

    if (srv->ctx) mg_stop(srv->ctx);

    /* clean up rate limit buckets */
    rate_bucket *b = srv->rate_buckets;
    while (b) {
        rate_bucket *next = b->next;
        free(b);
        b = next;
    }

#ifdef _WIN32
    DeleteCriticalSection(&srv->rate_lock);
#else
    pthread_mutex_destroy(&srv->rate_lock);
#endif

    sodium_memzero(srv->registry_sk, 64);
    free(srv->registry_id);
    free(srv);
}
