#include "cookbook_server.h"
#include "cookbook_semver.h"
#include "cookbook_sha256.h"
#include "cookbook_auth.h"
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

    char *result = pasta_write(stripped, PASTA_PRETTY);
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

/* ==== route: GET /resolve/{group}/{artifact}/{range} ==== */

typedef struct {
    cookbook_range *range;
    int            include_snapshots;
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
    n = snprintf(ctx->buf + ctx->len, ctx->cap - ctx->len,
        "{\"version\":\"%s\",\"snapshot\":%s,\"triples\":[\"%s\"]}",
        version,
        (snapshot && snapshot[0] == '1') ? "true" : "false",
        triple ? triple : "noarch");
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

    const char *sql =
        "SELECT a.version, a.snapshot, a.triple "
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
        &range, include_snapshots, result_buf, 0, sizeof(result_buf), 0
    };

    cookbook_db_status st = srv->db->query_p(srv->db, sql, params, 2,
                                             resolve_filter_cb, &ctx);

    if (st != COOKBOOK_DB_OK) {
        METRIC_INC(srv->metrics.responses_5xx);
        send_json(conn, 500, "{\"error\":\"Database error\"}\n");
    } else {
        METRIC_INC(srv->metrics.responses_2xx);
        METRIC_INC(srv->metrics.artifacts_resolved);
        char response[8320];
        snprintf(response, sizeof(response),
                 "{\"versions\":[%s]}\n", result_buf);
        send_json(conn, 200, response);
    }

    free(path); free(group); free(artifact); free(range_str);
    return 1;
}

/* ==== route: /artifact/... ==== */

typedef struct {
    int found;
    int yanked;
} yanked_check_ctx;

static int yanked_check_cb(const cookbook_db_row *row, void *user) {
    yanked_check_ctx *ctx = (yanked_check_ctx *)user;
    ctx->found = 1;
    if (row->values[0] && row->values[0][0] == '1')
        ctx->yanked = 1;
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
        const char *ysql =
            "UPDATE artifacts SET yanked = 1 "
            "WHERE group_id = ?1 AND artifact = ?2 AND version = ?3";
        cookbook_db_param yp[] = {
            COOKBOOK_P_TEXT(ygroup),
            COOKBOOK_P_TEXT(yartifact),
            COOKBOOK_P_TEXT(yversion)
        };
        cookbook_db_status yst = srv->db->exec_p(srv->db, ysql, yp, 3);
        if (yst != COOKBOOK_DB_OK) {
            METRIC_INC(srv->metrics.responses_5xx);
            send_json(conn, 500, "{\"error\":\"Database error\"}\n");
        } else {
            METRIC_INC(srv->metrics.responses_2xx);
            METRIC_INC(srv->metrics.artifacts_yanked);
            send_json(conn, 200, "{\"status\":\"yanked\"}\n");
        }
        free(path); free(ygroup); free(yartifact); free(yversion);
        return 1;
    }

    /* parse group/artifact/version/filename from path */
    char *group = NULL, *artifact = NULL, *ver_file = NULL;
    if (split_coord(path, &group, &artifact, &ver_file) != 0 || !ver_file) {
        send_json(conn, 400, "{\"error\":\"Malformed path\"}\n");
        free(path); free(group); free(artifact); free(ver_file);
        return 1;
    }

    /* ver_file is "version/filename" — split it */
    char *slash = strchr(ver_file, '/');
    char *version_str = NULL, *filename = NULL;
    if (slash) {
        *slash = '\0';
        version_str = ver_file;
        filename = slash + 1;
    } else {
        send_json(conn, 400, "{\"error\":\"Malformed artifact path\"}\n");
        free(path); free(group); free(artifact); free(ver_file);
        return 1;
    }

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
            send_json(conn, 404, "{\"error\":\"Not found\"}\n");
        } else if (sst != COOKBOOK_STORE_OK) {
            send_json(conn, 500, "{\"error\":\"Storage error\"}\n");
        } else {
            const char *ct = "application/octet-stream";
            int is_pasta = 0;
            if (strstr(path, ".sha256")) ct = "text/plain";
            else if (strstr(path, ".sig")) ct = "application/octet-stream";
            else if (filename && strcmp(filename, "now.pasta") == 0) {
                ct = "text/plain";
                is_pasta = 1;
            }
            else if (strstr(path, ".tar.gz")) ct = "application/gzip";
            else if (strstr(path, ".tar.zst")) ct = "application/zstd";

            /* #5: check yanked status */
            yanked_check_ctx yctx = { 0, 0 };
            const char *yanked_sql =
                "SELECT yanked FROM artifacts "
                "WHERE group_id = ?1 AND artifact = ?2 AND version = ?3 "
                "LIMIT 1";
            cookbook_db_param yparams[] = {
                COOKBOOK_P_TEXT(group),
                COOKBOOK_P_TEXT(artifact),
                COOKBOOK_P_TEXT(version_str)
            };
            srv->db->query_p(srv->db, yanked_sql, yparams, 3,
                              yanked_check_cb, &yctx);

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

            METRIC_INC(srv->metrics.responses_2xx);
            METRIC_ADD(srv->metrics.bytes_downloaded, (long)serve_len);
            mg_printf(conn,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %zu\r\n"
                "%s"
                "\r\n",
                ct, serve_len,
                yctx.yanked ? "X-Now-Yanked: true\r\n" : "");
            mg_write(conn, serve_data, serve_len);

            if (stripped) free(stripped);
            srv->store->free_buf(data);
        }

    } else if (strcmp(ri->request_method, "PUT") == 0) {
        /* ---- PUT: publish artifact ---- */

        if (validate_group(group) != 0 || validate_artifact(artifact) != 0) {
            send_json(conn, 400,
                "{\"error\":\"Invalid group or artifact identifier\"}\n");
            free(key); free(path); free(group); free(artifact); free(ver_file);
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
                free(ver_file);
                return 1;
            }

            /* #11: group claim checking */
            if (!cookbook_jwt_has_group(&claims, group)) {
                METRIC_INC(srv->metrics.responses_4xx);
                METRIC_INC(srv->metrics.auth_failures);
                send_json(conn, 403,
                    "{\"error\":\"JWT does not authorize this group\"}\n");
                free(key); free(path); free(group); free(artifact);
                free(ver_file);
                return 1;
            }

            /* #28: rate limiting per JWT sub */
            if (check_rate_limit(srv, claims.sub)) {
                METRIC_INC(srv->metrics.responses_4xx);
                send_json(conn, 429,
                    "{\"error\":\"Rate limit exceeded\"}\n");
                free(key); free(path); free(group); free(artifact);
                free(ver_file);
                return 1;
            }
        }

        /* immutability check */
        if (srv->store->exists(srv->store, key) == COOKBOOK_STORE_OK) {
            send_json(conn, 409,
                "{\"error\":\"Release coordinate already published\"}\n");
            free(key); free(path); free(group); free(artifact); free(ver_file);
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
            free(group); free(artifact); free(ver_file);
            return 1;
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
            free(group); free(artifact); free(ver_file);
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
                free(group); free(artifact); free(ver_file);
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
                        free(group); free(artifact); free(ver_file);
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
    free(ver_file);
    return 1;
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

    /* read body — expect JSON like {"sub":"alice","groups":"org.acme,org.beta"} */
    size_t body_len = 0;
    char *body = read_body(conn, ri, &body_len, 4096);
    if (!body || body_len == 0) {
        send_json(conn, 400, "{\"error\":\"Request body required\"}\n");
        free(body);
        return 1;
    }

    /* minimal JSON parse for sub and groups */
    char sub[128] = {0}, groups[1024] = {0};
    /* find "sub":"..." */
    const char *sub_start = strstr(body, "\"sub\"");
    if (sub_start) {
        sub_start = strchr(sub_start + 4, '"');
        if (sub_start) {
            sub_start++; /* skip opening quote after colon */
            /* skip the colon quote: "sub":"value" — we need to find :"
               Let's just find :" pattern */
        }
    }
    /* simpler: just scan for "sub":"value" and "groups":"value" */
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
        send_json(conn, 400, "{\"error\":\"Missing 'sub' field\"}\n");
        return 1;
    }

    /* TODO: authenticate the caller (e.g., check password/API key).
       For now, token issuance is open — auth will be enforced by
       whatever identity provider is fronting cookbook in production. */

    char *token = cookbook_jwt_create(sub, groups, srv->jwt_ttl_sec,
                                       srv->registry_sk);
    if (!token) {
        send_json(conn, 500, "{\"error\":\"Failed to create token\"}\n");
        return 1;
    }

    METRIC_INC(srv->metrics.responses_2xx);
    METRIC_INC(srv->metrics.auth_tokens_issued);
    char resp[8192];
    snprintf(resp, sizeof(resp),
        "{\"token\":\"%s\",\"expires_in\":%d}\n",
        token, srv->jwt_ttl_sec);
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
