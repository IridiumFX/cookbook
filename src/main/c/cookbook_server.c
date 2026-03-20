#include "cookbook_server.h"
#include "cookbook_semver.h"
#include "cookbook_sha256.h"
#include "cookbook_auth.h"
#include "cookbook_grid.h"
#include "cookbook_ed25519.h"
#include "cookbook_policy.h"
#include "cookbook_ldap.h"
#include "cookbook_oidc.h"
#include <apennines/t1/random/entropy.h>
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
    int                 grid_peer_auth;
    cookbook_revocation_list revocations;
    FILE               *audit_auth;
    FILE               *audit_access;
    FILE               *audit_admin;
    int                 object_cache_ttl_sec;
    cookbook_ldap_config ldap_cfg;
    cookbook_oidc_config oidc_cfg;

    /* device code flow state */
    struct device_code_entry {
        char device_code[64];
        char user_code[16];
        char subject[128];       /* filled when authorized */
        char groups[1024];       /* filled when authorized */
        int  authorized;         /* 0=pending, 1=authorized, -1=denied */
        int64_t expires_at;
    } device_codes[64];
    int device_code_count;

    volatile int        reconcile_running;
#ifdef _WIN32
    HANDLE              reconcile_thread;
    CRITICAL_SECTION    rate_lock;
    CRITICAL_SECTION    audit_lock;
#else
    pthread_t           reconcile_thread;
    pthread_mutex_t     rate_lock;
    pthread_mutex_t     audit_lock;
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

    int rc = cookbook_jwt_verify(auth + 7, srv->registry_pk, claims);
    if (rc != 0) return rc;

    /* check revocation list */
    if (claims->jti[0] &&
        cookbook_revocation_check(&srv->revocations, claims->jti)) {
        cookbook_jwt_claims_free(claims);
        memset(claims, 0, sizeof(*claims));
        return -1; /* token revoked */
    }

    return 0;
}

/* Phase 3: v2 auth enforcement helper.
   Extracts JWT, checks v2 grants or v1 group, handles rate limiting.
   Returns 1 if allowed (claims populated), 0 if denied (error sent).
   If auth is disabled (no registry key), returns 1 with zeroed claims. */
/* forward declaration — defined after utc_now */
static void audit_log(cookbook_server *srv, const char *event,
                       const char *subject, const char *target,
                       const char *result);

static int require_auth_v2(cookbook_server *srv, struct mg_connection *conn,
                            const struct mg_request_info *ri,
                            const char *group_id, char op,
                            cookbook_jwt_claims *claims) {
    memset(claims, 0, sizeof(*claims));
    if (!srv->has_registry_key) return 1; /* auth disabled */

    if (extract_bearer_jwt(srv, ri, claims) != 0) {
        METRIC_INC(srv->metrics.responses_4xx);
        METRIC_INC(srv->metrics.auth_failures);
        audit_log(srv, "auth", "unknown", group_id, "jwt-invalid");
        send_json(conn, 401,
            "{\"error\":\"Valid Bearer JWT required\"}\n");
        return 0;
    }

    /* rate limiting */
    if (check_rate_limit(srv, claims->sub)) {
        METRIC_INC(srv->metrics.responses_4xx);
        audit_log(srv, "auth", claims->sub, group_id, "rate-limited");
        cookbook_jwt_claims_free(claims);
        send_json(conn, 429, "{\"error\":\"Rate limit exceeded\"}\n");
        return 0;
    }

    if (claims->version == 2) {
        /* v2: fine-grained auth check */
        if (!cookbook_auth_check(claims->grants_json, claims->exclude_json,
                                group_id, op)) {
            METRIC_INC(srv->metrics.responses_4xx);
            METRIC_INC(srv->metrics.auth_failures);
            audit_log(srv, "auth", claims->sub, group_id, "denied");
            cookbook_jwt_claims_free(claims);
            send_json(conn, 403,
                "{\"error\":\"Insufficient permissions\"}\n");
            return 0;
        }
    } else {
        /* v1: for writes check group membership, reads are open */
        if (op != 'r' && !cookbook_jwt_has_group(claims, group_id)) {
            METRIC_INC(srv->metrics.responses_4xx);
            METRIC_INC(srv->metrics.auth_failures);
            audit_log(srv, "auth", claims->sub, group_id, "denied");
            send_json(conn, 403,
                "{\"error\":\"JWT does not authorize this group\"}\n");
            return 0;
        }
    }

    return 1;
}

/* Build X-Cookbook-Grid-Grants/Exclude headers from JWT claims.
   Returns malloc'd header string or NULL. Caller must free. */
static char *build_grid_grant_headers(const cookbook_jwt_claims *claims) {
    if (claims->version != 2 || !claims->grants_json) return NULL;

    size_t cap = 2048;
    char *hdrs = (char *)malloc(cap);
    if (!hdrs) return NULL;
    size_t pos = 0;

    /* serialize grants_json to compact header format: group:perms,... */
    pos += (size_t)snprintf(hdrs + pos, cap - pos,
        "X-Cookbook-Grid-Grants: ");

    /* walk the grants JSON map */
    const char *gs = claims->grants_json;
    const char *p = strchr(gs, '{');
    if (p) p++;
    int first = 1;
    while (p && *p && *p != '}') {
        while (*p == ' ' || *p == '\n' || *p == ',') p++;
        if (*p == '"') {
            p++;
            const char *kend = strchr(p, '"');
            if (!kend) break;
            size_t klen = (size_t)(kend - p);
            const char *key = p;
            p = kend + 1;
            while (*p == ' ' || *p == ':') p++;
            if (*p == '"') {
                p++;
                const char *vend = strchr(p, '"');
                if (!vend) break;
                size_t vlen = (size_t)(vend - p);
                if (!first && pos < cap)
                    hdrs[pos++] = ',';
                pos += (size_t)snprintf(hdrs + pos, cap - pos,
                    "%.*s:%.*s", (int)klen, key, (int)vlen, p);
                p = vend + 1;
                first = 0;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    pos += (size_t)snprintf(hdrs + pos, cap - pos, "\r\n");

    /* serialize exclude if present */
    if (claims->exclude_json) {
        pos += (size_t)snprintf(hdrs + pos, cap - pos,
            "X-Cookbook-Grid-Exclude: ");
        const char *es = claims->exclude_json;
        const char *ep = strchr(es, '{');
        if (ep) ep++;
        first = 1;
        while (ep && *ep && *ep != '}') {
            while (*ep == ' ' || *ep == '\n' || *ep == ',') ep++;
            if (*ep == '"') {
                ep++;
                const char *kend = strchr(ep, '"');
                if (!kend) break;
                size_t klen = (size_t)(kend - ep);
                if (!first && pos < cap)
                    hdrs[pos++] = ',';
                pos += (size_t)snprintf(hdrs + pos, cap - pos,
                    "%.*s", (int)klen, ep);
                ep = kend + 1;
                /* skip past : and value */
                while (*ep && *ep != ',' && *ep != '}') ep++;
                first = 0;
            } else {
                break;
            }
        }
        pos += (size_t)snprintf(hdrs + pos, cap - pos, "\r\n");
    }

    hdrs[pos] = '\0';
    return hdrs;
}

/* Parse X-Cookbook-Grid-Grants header into a JSON string for auth_check.
   Input format: "com.iridiumfx:crwd,org.acme:r"
   Returns malloc'd JSON string like {"com.iridiumfx":"crwd","org.acme":"r"}
   or NULL if header not present. */
static char *parse_grid_grants_header(const struct mg_request_info *ri) {
    const char *hval = NULL;
    for (int i = 0; i < ri->num_headers; i++) {
        if (strcasecmp(ri->http_headers[i].name,
                       "X-Cookbook-Grid-Grants") == 0) {
            hval = ri->http_headers[i].value;
            break;
        }
    }
    if (!hval || !*hval) return NULL;

    size_t cap = strlen(hval) * 3 + 16;
    char *json = (char *)malloc(cap);
    if (!json) return NULL;
    size_t pos = 0;
    json[pos++] = '{';

    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", hval);
    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    int first = 1;
    while (tok) {
        while (*tok == ' ') tok++;
        char *colon = strchr(tok, ':');
        if (colon) {
            *colon = '\0';
            if (!first) json[pos++] = ',';
            pos += (size_t)snprintf(json + pos, cap - pos,
                "\"%s\":\"%s\"", tok, colon + 1);
            first = 0;
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }
    json[pos++] = '}';
    json[pos] = '\0';
    return json;
}

/* Parse X-Cookbook-Grid-Exclude header into a JSON string.
   Input format: "com.iridiumfx.secret,org.acme.internal"
   Returns malloc'd JSON like {"com.iridiumfx.secret":true,...} */
static char *parse_grid_exclude_header(const struct mg_request_info *ri) {
    const char *hval = NULL;
    for (int i = 0; i < ri->num_headers; i++) {
        if (strcasecmp(ri->http_headers[i].name,
                       "X-Cookbook-Grid-Exclude") == 0) {
            hval = ri->http_headers[i].value;
            break;
        }
    }
    if (!hval || !*hval) return NULL;

    size_t cap = strlen(hval) * 3 + 16;
    char *json = (char *)malloc(cap);
    if (!json) return NULL;
    size_t pos = 0;
    json[pos++] = '{';

    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", hval);
    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    int first = 1;
    while (tok) {
        while (*tok == ' ') tok++;
        if (!first) json[pos++] = ',';
        pos += (size_t)snprintf(json + pos, cap - pos,
            "\"%s\":true", tok);
        first = 0;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    json[pos++] = '}';
    json[pos] = '\0';
    return json;
}

/* Check grid grant headers for authorization.
   Returns 1 if allowed, 0 if denied.
   If no grid grant headers present, returns 1 (open access). */
static int check_grid_auth(const struct mg_request_info *ri,
                            const char *group_id, char op) {
    char *grants = parse_grid_grants_header(ri);
    if (!grants) return 1; /* no grant headers = open */

    char *exclude = parse_grid_exclude_header(ri);
    int allowed = cookbook_auth_check(grants, exclude, group_id, op);
    free(grants);
    free(exclude);
    return allowed;
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

/* ==== audit log (pasta format) ==== */

static void audit_log(cookbook_server *srv, const char *event,
                       const char *subject, const char *target,
                       const char *result) {
    /* select the right file based on event category */
    FILE *f = NULL;
    if (strcmp(event, "auth") == 0)        f = srv->audit_auth;
    else if (strcmp(event, "admin") == 0)   f = srv->audit_admin;
    else                                    f = srv->audit_access;
    if (!f) return;

    char ts[64];
    utc_now(ts, sizeof(ts));

    /* escape quotes in target */
    char safe_target[512] = {0};
    if (target) {
        size_t j = 0;
        for (size_t i = 0; target[i] && j < sizeof(safe_target) - 1; i++) {
            safe_target[j++] = (target[i] == '"') ? '\'' : target[i];
        }
    }

    char line[1024];
    int n;
    if (target && target[0]) {
        n = snprintf(line, sizeof(line),
            "{ timestamp: \"%s\", event: \"%s\", subject: \"%s\", "
            "target: \"%s\", result: \"%s\" }\n",
            ts, event,
            subject ? subject : "anonymous",
            safe_target,
            result ? result : "ok");
    } else {
        n = snprintf(line, sizeof(line),
            "{ timestamp: \"%s\", event: \"%s\", subject: \"%s\", "
            "result: \"%s\" }\n",
            ts, event,
            subject ? subject : "anonymous",
            result ? result : "ok");
    }

    if (n > 0) {
#ifdef _WIN32
        EnterCriticalSection(&srv->audit_lock);
#else
        pthread_mutex_lock(&srv->audit_lock);
#endif
        fwrite(line, 1, (size_t)n, f);
        fflush(f);
#ifdef _WIN32
        LeaveCriticalSection(&srv->audit_lock);
#else
        pthread_mutex_unlock(&srv->audit_lock);
#endif
    }
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

/* ==== Object cache TTL eviction ==== */

static int srv_count_cb(const cookbook_db_row *row, void *user) {
    (void)row;
    int *count = (int *)user;
    (*count)++;
    return 0;
}

typedef struct {
    char **keys;        /* store_key values to delete */
    char **cache_keys;  /* cache_key values for DB delete */
    int count;
    int cap;
} evict_ctx;

static int evict_cb(const cookbook_db_row *row, void *user) {
    evict_ctx *ctx = (evict_ctx *)user;
    if (row->ncols < 2 || !row->values[0] || !row->values[1]) return 0;
    if (ctx->count >= ctx->cap) return 0;

    ctx->keys[ctx->count] = strdup(row->values[0]);       /* store_key */
    ctx->cache_keys[ctx->count] = strdup(row->values[1]);  /* cache_key */
    ctx->count++;
    return 0;
}

static void evict_expired_objects(cookbook_server *srv) {
    if (srv->object_cache_ttl_sec <= 0) return;

    int64_t cutoff = (int64_t)time(NULL) - srv->object_cache_ttl_sec;
    char cutoff_str[32];
    snprintf(cutoff_str, sizeof(cutoff_str), "%lld", (long long)cutoff);

    evict_ctx ctx = { NULL, NULL, 0, 0 };

    /* count expired entries first */
    int expired_count = 0;
    cookbook_db_param cp[] = { COOKBOOK_P_TEXT(cutoff_str) };
    srv->db->query_p(srv->db,
        "SELECT store_key FROM object_cache WHERE created_at < ?1",
        cp, 1, srv_count_cb, &expired_count);

    if (expired_count == 0) return;

    /* allocate and collect */
    ctx.cap = expired_count;
    ctx.keys = calloc((size_t)expired_count, sizeof(char *));
    ctx.cache_keys = calloc((size_t)expired_count, sizeof(char *));
    if (!ctx.keys || !ctx.cache_keys) {
        free(ctx.keys);
        free(ctx.cache_keys);
        return;
    }

    srv->db->query_p(srv->db,
        "SELECT store_key, cache_key FROM object_cache WHERE created_at < ?1",
        cp, 1, evict_cb, &ctx);

    /* delete from store and DB */
    for (int i = 0; i < ctx.count; i++) {
        if (ctx.keys[i]) {
            srv->store->del(srv->store, ctx.keys[i]);
            free(ctx.keys[i]);
        }
        if (ctx.cache_keys[i]) {
            cookbook_db_param dp[] = { COOKBOOK_P_TEXT(ctx.cache_keys[i]) };
            srv->db->exec_p(srv->db,
                "DELETE FROM object_cache WHERE cache_key = ?1", dp, 1);
            free(ctx.cache_keys[i]);
        }
    }

    if (ctx.count > 0)
        fprintf(stdout, "cookbook: evicted %d expired cached objects\n",
                ctx.count);

    free(ctx.keys);
    free(ctx.cache_keys);
}

#ifdef _WIN32
static DWORD WINAPI reconcile_thread_fn(LPVOID arg) {
    cookbook_server *srv = (cookbook_server *)arg;
    while (srv->reconcile_running) {
        Sleep(60000);  /* check every 60 seconds */
        if (srv->reconcile_running) {
            reconcile_stale_pending(srv);
            evict_expired_objects(srv);
        }
    }
    return 0;
}
#else
static void *reconcile_thread_fn(void *arg) {
    cookbook_server *srv = (cookbook_server *)arg;
    while (srv->reconcile_running) {
        sleep(60);  /* check every 60 seconds */
        if (srv->reconcile_running) {
            reconcile_stale_pending(srv);
            evict_expired_objects(srv);
        }
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

/* ==== route: GET /.well-known/now-registry ==== */

static int handle_registry_discovery(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    if (strcmp(ri->request_method, "GET") != 0) {
        send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
        return 1;
    }

    /* build pasta discovery document */
    char resp[2048];
    int n = 0;
    n += snprintf(resp + n, sizeof(resp) - (size_t)n,
        "{ registry_id: \"%s\"", srv->registry_id);

    /* auth capabilities */
    if (srv->has_registry_key) {
        n += snprintf(resp + n, sizeof(resp) - (size_t)n,
            ", auth: { enabled: true"
            ", methods: [\"token\"%s%s]"
            ", algorithm: \"ed25519\"",
            srv->ldap_cfg.url ? ", \"ldap\"" : "",
            srv->oidc_cfg.issuer ? ", \"oidc\"" : "");
        char pk_hex[65];
        for (int i = 0; i < 32; i++)
            snprintf(pk_hex + i * 2, 3, "%02x", srv->registry_pk[i]);
        n += snprintf(resp + n, sizeof(resp) - (size_t)n,
            ", public_key: \"%s\" }", pk_hex);
    } else {
        n += snprintf(resp + n, sizeof(resp) - (size_t)n,
            ", auth: { enabled: false }");
    }

    /* grid capabilities */
    n += snprintf(resp + n, sizeof(resp) - (size_t)n,
        ", grid: { enabled: %s, max_hops: %d }",
        srv->grid_enabled ? "true" : "false",
        srv->grid_max_hops);

    /* endpoints */
    n += snprintf(resp + n, sizeof(resp) - (size_t)n,
        ", endpoints: ["
        "\"resolve\", \"artifact\", \"auth/token\", \"auth/revoke\""
        ", \"keys\", \"mirror/manifest\", \"metrics\""
        ", \"admin/credentials\", \"admin/groups\", \"admin/policies\""
        ", \"admin/peers\", \"objects\""
        "]");

    /* content types */
    n += snprintf(resp + n, sizeof(resp) - (size_t)n,
        ", content_types: ["
        "\"application/x-pasta\", \"application/json\""
        "]");

    n += snprintf(resp + n, sizeof(resp) - (size_t)n, " }\n");

    mg_printf(conn,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/x-pasta; charset=US-ASCII\r\n"
        "Content-Length: %d\r\n\r\n", n);
    mg_write(conn, resp, (size_t)n);
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

    /* Phase 3: auth enforcement on resolve */
    cookbook_jwt_claims claims;
    if (!require_auth_v2(srv, conn, ri, group, 'r', &claims)) {
        free(path); free(group); free(artifact); free(range_str);
        return 1;
    }

    cookbook_range range;
    if (cookbook_range_parse(range_str, &range) != 0) {
        send_json(conn, 400, "{\"error\":\"Malformed range string\"}\n");
        cookbook_jwt_claims_free(&claims);
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
            char *grid_hdrs = build_grid_grant_headers(&claims);
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
                cookbook_grid_sign_ctx sctx = {
                    srv->registry_id, srv->registry_sk,
                    srv->has_registry_key
                };
                if (cookbook_grid_get_signed(&peers[pi], grid_path,
                        srv->registry_id, NULL, 0, grid_hdrs,
                        &sctx, &gresp) == 0
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
            free(grid_hdrs);
        }

        METRIC_INC(srv->metrics.responses_2xx);
        METRIC_INC(srv->metrics.artifacts_resolved);
        audit_log(srv, "resolve", claims.sub, path, "ok");

        /* #8: content negotiation on /resolve/ */
        content_pref pref = parse_accept(ri);
        if (pref == CT_UNKNOWN) {
            METRIC_INC(srv->metrics.responses_4xx);
            send_json(conn, 406,
                "{\"error\":\"Not Acceptable — "
                "supported: application/x-pasta, "
                "application/json\"}\n");
            cookbook_jwt_claims_free(&claims);
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
                        "Content-Type: application/x-pasta; charset=US-ASCII\r\n"
                        "Content-Length: %zu\r\n"
                        "\r\n",
                        plen);
                    mg_write(conn, pasta_out, plen);
                    free(pasta_out);
                    cookbook_jwt_claims_free(&claims);
                    free(path); free(group); free(artifact);
                    free(range_str);
                    return 1;
                }
            }
            /* fallback to JSON if Pasta serialization fails */
        }

        send_json(conn, 200, response);
    }

    cookbook_jwt_claims_free(&claims);
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
        /* Phase 3: auth enforcement on yank */
        char yank_sub[128] = {0};
        {
            cookbook_jwt_claims yclaims;
            if (!require_auth_v2(srv, conn, ri, ygroup, 'w', &yclaims)) {
                free(path); free(ygroup); free(yartifact); free(yversion);
                return 1;
            }
            snprintf(yank_sub, sizeof(yank_sub), "%s", yclaims.sub);
            cookbook_jwt_claims_free(&yclaims);
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
            audit_log(srv, "yank", yank_sub, path, "ok");
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

    /* Phase 3: auth enforcement on artifact access */
    char art_auth_op = 'r'; /* GET */
    if (strcmp(ri->request_method, "PUT") == 0) art_auth_op = 'c';

    cookbook_jwt_claims art_claims;
    if (!require_auth_v2(srv, conn, ri, group, art_auth_op, &art_claims)) {
        free(path); free(group); free(artifact);
        free(ver_file); free(filename);
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
            /* G4: grid fan-out on local 404 */
            int grid_handled = 0;
            if (srv->grid_enabled && !grid_get_via(ri)) {
                char *art_grid_hdrs = build_grid_grant_headers(&art_claims);
                cookbook_peer *peers = NULL;
                int npeers = cookbook_grid_load_peers(srv->db, &peers);
                for (int pi = 0; pi < npeers; pi++) {
                    char grid_path[2048];
                    snprintf(grid_path, sizeof(grid_path),
                        "/grid/artifact/%s", path);

                    cookbook_grid_sign_ctx asctx = {
                        srv->registry_id, srv->registry_sk,
                        srv->has_registry_key
                    };
                    if (peers[pi].mode == 'r') {
                        /* redirect mode: HEAD check then 307 */
                        cookbook_grid_response gresp;
                        if (cookbook_grid_head_signed(&peers[pi], grid_path,
                                srv->registry_id, NULL, 0,
                                art_grid_hdrs, &asctx, &gresp) == 0
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
                        if (cookbook_grid_get_signed(&peers[pi], grid_path,
                                srv->registry_id, NULL, 0,
                                art_grid_hdrs, &asctx, &gresp) == 0
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
                free(art_grid_hdrs);
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
                ct = "application/x-pasta; charset=US-ASCII";
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
                                "Content-Type: application/x-pasta; charset=US-ASCII\r\n"
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

        /* auth already checked by require_auth_v2 above */

        /* immutability check */
        if (srv->store->exists(srv->store, key) == COOKBOOK_STORE_OK) {
            audit_log(srv, "publish", art_claims.sub, key, "duplicate");
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

        /* validate .repro sidecar files (reproducibility attestation) */
        if (filename) {
            size_t fnlen = strlen(filename);
            if (fnlen > 6 && strcmp(filename + fnlen - 6, ".repro") == 0) {
                /* must be valid ASCII */
                size_t bad = validate_ascii(body, body_len);
                if (bad) {
                    char err[256];
                    snprintf(err, sizeof(err),
                        "{\"error\":\"Non-ASCII byte at offset %zu"
                        " in .repro file\"}\n", bad);
                    METRIC_INC(srv->metrics.responses_4xx);
                    send_json(conn, 400, err);
                    free(body); free(key); free(path);
                    free(group); free(artifact);
                    free(ver_file); free(filename);
                    return 1;
                }
                /* must be valid pasta */
                PastaResult rpr;
                PastaValue *repro = pasta_parse(body, body_len, &rpr);
                if (!repro) {
                    METRIC_INC(srv->metrics.responses_4xx);
                    send_json(conn, 400,
                        "{\"error\":\"Invalid .repro file: "
                        "must be valid pasta\"}\n");
                    free(body); free(key); free(path);
                    free(group); free(artifact);
                    free(ver_file); free(filename);
                    return 1;
                }
                /* check required fields: format, artifact_hash */
                int has_format = 0, has_hash = 0;
                if (pasta_type(repro) == PASTA_MAP) {
                    if (basta_map_get(repro, "format")) has_format = 1;
                    if (basta_map_get(repro, "artifact_hash")) has_hash = 1;
                }
                pasta_free(repro);
                if (!has_format || !has_hash) {
                    METRIC_INC(srv->metrics.responses_4xx);
                    send_json(conn, 400,
                        "{\"error\":\"Invalid .repro file: "
                        "requires 'format' and 'artifact_hash' fields\"}\n");
                    free(body); free(key); free(path);
                    free(group); free(artifact);
                    free(ver_file); free(filename);
                    return 1;
                }
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
                        audit_log(srv, "publish", art_claims.sub, key,
                                  "validation-failed");
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
                        const char *owner_sub =
                            art_claims.sub[0] ? art_claims.sub : "anonymous";
                        const char *grp_sql =
                            "INSERT OR IGNORE INTO groups "
                            "(group_id, owner_sub) VALUES (?1, ?2)";
                        cookbook_db_param gp[] = {
                            COOKBOOK_P_TEXT(grp),
                            COOKBOOK_P_TEXT(owner_sub)
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
        audit_log(srv, "publish", art_claims.sub, key, "ok");
        char resp[256];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"created\",\"sha256\":\"%s\",\"triple\":\"%s\"}\n",
            sha256_hex, triple);
        send_json(conn, 201, resp);
        free(body);

    } else {
        send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
    }

    cookbook_jwt_claims_free(&art_claims);
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
                audit_log(srv, "auth", sub, "token-issue", "bad-credentials");
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

        /* minimal JSON parse for sub/subject, token, and groups */
        {
            const char *p = strstr(body, "\"sub\":");
            if (!p) p = strstr(body, "\"subject\":");
            if (p) {
                /* advance past key + colon */
                p = strchr(p, ':');
                if (p) {
                    p++;
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

        /* parse method and grant_type fields */
        char method[32] = "token";
        char grant_type[64] = {0};
        char client_secret[512] = {0};
        {
            const char *mp = strstr(body, "\"method\":");
            if (mp) {
                mp = strchr(mp, ':');
                if (mp) {
                    mp++;
                    while (*mp == ' ' || *mp == '\t') mp++;
                    if (*mp == '"') {
                        mp++;
                        const char *me = strchr(mp, '"');
                        if (me) {
                            size_t ml = (size_t)(me - mp);
                            if (ml >= sizeof(method)) ml = sizeof(method) - 1;
                            memcpy(method, mp, ml);
                            method[ml] = '\0';
                        }
                    }
                }
            }
        }

        /* parse grant_type */
        {
            const char *gp = strstr(body, "\"grant_type\":");
            if (gp) {
                gp = strchr(gp, ':');
                if (gp) {
                    gp++;
                    while (*gp == ' ' || *gp == '\t') gp++;
                    if (*gp == '"') {
                        gp++;
                        const char *ge = strchr(gp, '"');
                        if (ge) {
                            size_t gl = (size_t)(ge - gp);
                            if (gl >= sizeof(grant_type)) gl = sizeof(grant_type) - 1;
                            memcpy(grant_type, gp, gl);
                            grant_type[gl] = '\0';
                        }
                    }
                }
            }
        }

        /* parse client_secret */
        {
            const char *cp = strstr(body, "\"client_secret\":");
            if (cp) {
                cp = strchr(cp, ':');
                if (cp) {
                    cp++;
                    while (*cp == ' ' || *cp == '\t') cp++;
                    if (*cp == '"') {
                        cp++;
                        const char *ce = strchr(cp, '"');
                        if (ce) {
                            size_t cl = (size_t)(ce - cp);
                            if (cl >= sizeof(client_secret))
                                cl = sizeof(client_secret) - 1;
                            memcpy(client_secret, cp, cl);
                            client_secret[cl] = '\0';
                        }
                    }
                }
            }
        }

        /* parse client_id from body (may override sub for OIDC) */
        {
            const char *cip = strstr(body, "\"client_id\":");
            if (cip && !sub[0]) {
                cip = strchr(cip, ':');
                if (cip) {
                    cip++;
                    while (*cip == ' ' || *cip == '\t') cip++;
                    if (*cip == '"') {
                        cip++;
                        const char *cie = strchr(cip, '"');
                        if (cie) {
                            size_t cil = (size_t)(cie - cip);
                            if (cil >= sizeof(sub)) cil = sizeof(sub) - 1;
                            memcpy(sub, cip, cil);
                            sub[cil] = '\0';
                        }
                    }
                }
            }
        }

        /* extract token/password from body */
        char body_token[512] = {0};
        if (sub[0]) {
            const char *tp = strstr(body, "\"token\":");
            if (tp) {
                tp = strchr(tp, ':');
                if (tp) {
                    tp++;
                    while (*tp == ' ' || *tp == '\t') tp++;
                    if (*tp == '"') {
                        tp++;
                        const char *te = strchr(tp, '"');
                        if (te) {
                            size_t tl = (size_t)(te - tp);
                            if (tl >= sizeof(body_token))
                                tl = sizeof(body_token) - 1;
                            memcpy(body_token, tp, tl);
                        }
                    }
                }
            }
        }

        /* OIDC client_credentials flow (separate from method-based auth) */
        if (strcmp(grant_type, "client_credentials") == 0 &&
            srv->oidc_cfg.issuer && client_secret[0]) {
            char oidc_sub[128] = {0};
            const char *cid = sub[0] ? sub : (srv->oidc_cfg.client_id ? srv->oidc_cfg.client_id : "");
            if (cookbook_oidc_client_credentials(&srv->oidc_cfg, cid,
                                                  client_secret,
                                                  oidc_sub, sizeof(oidc_sub)) != 0) {
                free(body);
                METRIC_INC(srv->metrics.responses_4xx);
                METRIC_INC(srv->metrics.auth_failures);
                audit_log(srv, "auth", cid, "token-issue", "oidc-failed");
                send_json(conn, 401,
                    "{\"error\":\"OIDC client credentials failed\"}\n");
                return 1;
            }
            if (oidc_sub[0]) snprintf(sub, sizeof(sub), "%s", oidc_sub);
            else if (!sub[0]) snprintf(sub, sizeof(sub), "%s", cid);
            cred_verified = 1;
            free(body);
            body = NULL;
            /* fall through to JWT issuance */
            goto issue_jwt;
        }

        /* verify credentials based on method */
        if (sub[0] && body_token[0]) {
            if (strcmp(method, "ldap") == 0 && srv->ldap_cfg.url) {
                /* LDAP bind verification */
                char *ldap_groups = NULL;
                if (cookbook_ldap_bind(&srv->ldap_cfg, sub,
                                       body_token, &ldap_groups) != 0) {
                    free(body);
                    METRIC_INC(srv->metrics.responses_4xx);
                    METRIC_INC(srv->metrics.auth_failures);
                    audit_log(srv, "auth", sub,
                              "token-issue", "ldap-bind-failed");
                    send_json(conn, 401,
                        "{\"error\":\"LDAP authentication failed\"}\n");
                    return 1;
                }
                if (ldap_groups && ldap_groups[0]) {
                    snprintf(groups, sizeof(groups), "%s", ldap_groups);
                }
                free(ldap_groups);
                cred_verified = 1;
            } else {
                /* local credential store (Argon2id) */
                cred_lookup_ctx clctx = { {0}, {0}, 0 };
                cookbook_db_param cp[] = { COOKBOOK_P_TEXT(sub) };
                srv->db->query_p(srv->db,
                    "SELECT token_hash, groups "
                    "FROM credentials "
                    "WHERE subject = ?1 "
                    "AND revoked_at IS NULL",
                    cp, 1, cred_lookup_cb, &clctx);

                if (clctx.found) {
                    if (cookbook_credential_verify(
                            body_token, clctx.hash) != 0) {
                        free(body);
                        METRIC_INC(srv->metrics.responses_4xx);
                        METRIC_INC(srv->metrics.auth_failures);
                        audit_log(srv, "auth", sub,
                                  "token-issue", "bad-credentials");
                        send_json(conn, 401,
                            "{\"error\":\"Invalid credentials\"}\n");
                        return 1;
                    }
                    memcpy(groups, clctx.groups, sizeof(groups));
                    cred_verified = 1;
                }
            }
        }
        free(body);

        if (!sub[0]) {
            send_json(conn, 400,
                "{\"error\":\"Missing 'sub' or 'subject' field\"}\n");
            return 1;
        }
    }

issue_jwt:
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
    audit_log(srv, "auth", sub, "token-issue", "ok");
    char resp[8192];
    snprintf(resp, sizeof(resp),
        "{\"token\":\"%s\",\"expires_in\":%d,\"version\":%d}\n",
        token, srv->jwt_ttl_sec, token_version);
    free(token);

    send_json(conn, 200, resp);
    return 1;
}

/* ==== Token revocation: POST /auth/revoke ==== */

static int handle_auth_revoke(struct mg_connection *conn, void *cbdata) {
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
            "{\"error\":\"Auth not configured\"}\n");
        return 1;
    }

    /* read body: {"token":"eyJ..."} */
    size_t body_len = 0;
    char *body = read_body(conn, ri, &body_len, 16384);
    if (!body || body_len == 0) {
        send_json(conn, 400,
            "{\"error\":\"Body required with 'token' field\"}\n");
        free(body);
        return 1;
    }

    /* extract token from body */
    const char *tp = strstr(body, "\"token\":\"");
    if (!tp) {
        free(body);
        METRIC_INC(srv->metrics.responses_4xx);
        send_json(conn, 400,
            "{\"error\":\"Missing 'token' field\"}\n");
        return 1;
    }
    tp += 9;
    const char *tend = strchr(tp, '"');
    if (!tend) {
        free(body);
        METRIC_INC(srv->metrics.responses_4xx);
        send_json(conn, 400, "{\"error\":\"Malformed token field\"}\n");
        return 1;
    }
    size_t tlen = (size_t)(tend - tp);
    char *token_str = (char *)malloc(tlen + 1);
    if (!token_str) { free(body); return 1; }
    memcpy(token_str, tp, tlen);
    token_str[tlen] = '\0';
    free(body);

    /* verify the token first (must be a valid JWT) */
    cookbook_jwt_claims claims;
    if (cookbook_jwt_verify(token_str, srv->registry_pk, &claims) != 0) {
        free(token_str);
        METRIC_INC(srv->metrics.responses_4xx);
        send_json(conn, 401,
            "{\"error\":\"Invalid or expired token\"}\n");
        return 1;
    }
    free(token_str);

    if (!claims.jti[0]) {
        cookbook_jwt_claims_free(&claims);
        METRIC_INC(srv->metrics.responses_4xx);
        send_json(conn, 400,
            "{\"error\":\"Token has no jti claim (pre-revocation era)\"}\n");
        return 1;
    }

    /* add to in-memory revocation list */
    int rc = cookbook_revocation_add(&srv->revocations,
                                      claims.jti, claims.exp);
    if (rc != 0) {
        cookbook_jwt_claims_free(&claims);
        send_json(conn, 507,
            "{\"error\":\"Revocation list full\"}\n");
        return 1;
    }

    /* persist to database */
    {
        char now_ts[64];
        utc_now(now_ts, sizeof(now_ts));
        char exp_str[32];
        snprintf(exp_str, sizeof(exp_str), "%lld", (long long)claims.exp);
        cookbook_db_param rp[] = {
            COOKBOOK_P_TEXT(claims.jti),
            COOKBOOK_P_TEXT(claims.sub),
            COOKBOOK_P_TEXT(now_ts),
            COOKBOOK_P_TEXT(exp_str)
        };
        srv->db->exec_p(srv->db,
            "INSERT OR IGNORE INTO revocations "
            "(jti, subject, revoked_at, expires_at) "
            "VALUES (?1, ?2, ?3, ?4)",
            rp, 4);
    }

    audit_log(srv, "auth", claims.sub, "token-revoke", "ok");
    cookbook_jwt_claims_free(&claims);

    METRIC_INC(srv->metrics.responses_2xx);
    send_json(conn, 200,
        "{\"status\":\"revoked\"}\n");
    return 1;
}

/* ==== OIDC device code flow: /auth/device ==== */

static void generate_user_code(char *out, size_t sz) {
    /* 8-char alphanumeric code: ABCD-EFGH */
    unsigned char rand[4];
    entropy_get_system(rand, 4);
    const char *alpha = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; /* no I,O,0,1 */
    snprintf(out, sz, "%c%c%c%c-%c%c%c%c",
             alpha[rand[0] & 0x1F], alpha[(rand[0] >> 5) | ((rand[1] & 0x03) << 3)],
             alpha[(rand[1] >> 2) & 0x1F], alpha[(rand[1] >> 7) | ((rand[2] & 0x0F) << 1)],
             alpha[(rand[2] >> 4) | ((rand[3] & 0x01) << 4)], alpha[(rand[3] >> 1) & 0x1F],
             alpha[(rand[3] >> 6) | ((rand[0] & 0x0F) << 2)], alpha[(rand[2] >> 3) & 0x1F]);
}

static int handle_auth_device(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    METRIC_INC(srv->metrics.requests_total);
    METRIC_INC(srv->metrics.requests_post);

    if (strcmp(ri->request_method, "POST") != 0) {
        send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
        return 1;
    }

    if (!srv->has_registry_key) {
        send_json(conn, 503, "{\"error\":\"Auth not configured\"}\n");
        return 1;
    }

    /* prune expired device codes */
    int64_t now = (int64_t)time(NULL);
    int write = 0;
    for (int i = 0; i < srv->device_code_count; i++) {
        if (srv->device_codes[i].expires_at > now)
            srv->device_codes[write++] = srv->device_codes[i];
    }
    srv->device_code_count = write;

    if (srv->device_code_count >= 64) {
        send_json(conn, 503,
            "{\"error\":\"Too many pending device codes\"}\n");
        return 1;
    }

    /* generate codes */
    struct device_code_entry *entry =
        &srv->device_codes[srv->device_code_count++];
    memset(entry, 0, sizeof(*entry));

    /* device_code: 32-char hex random */
    unsigned char rand_bytes[16];
    entropy_get_system(rand_bytes, 16);
    for (int i = 0; i < 16; i++)
        snprintf(entry->device_code + i * 2, 3, "%02x", rand_bytes[i]);

    generate_user_code(entry->user_code, sizeof(entry->user_code));
    entry->authorized = 0;
    entry->expires_at = now + 900; /* 15 minutes */

    /* build verification URI */
    char resp[1024];
    snprintf(resp, sizeof(resp),
        "{\"device_code\":\"%s\","
        "\"user_code\":\"%s\","
        "\"verification_uri\":\"http://localhost:%s/auth/device/verify\","
        "\"interval\":5,"
        "\"expires_in\":900}\n",
        entry->device_code, entry->user_code,
        "8080"); /* TODO: use actual port */

    audit_log(srv, "auth", "unknown", "device-code-issued", "ok");
    METRIC_INC(srv->metrics.responses_2xx);
    send_json(conn, 200, resp);
    return 1;
}

static int handle_auth_device_token(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    METRIC_INC(srv->metrics.requests_total);
    METRIC_INC(srv->metrics.requests_post);

    if (strcmp(ri->request_method, "POST") != 0) {
        send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
        return 1;
    }

    /* read body: {"device_code":"...", "grant_type":"urn:ietf:params:oauth:grant-type:device_code"} */
    size_t body_len = 0;
    char *body = read_body(conn, ri, &body_len, 4096);
    if (!body || body_len == 0) {
        free(body);
        send_json(conn, 400, "{\"error\":\"Body required\"}\n");
        return 1;
    }

    /* extract device_code */
    char dc[64] = {0};
    const char *dp = strstr(body, "\"device_code\":\"");
    if (dp) {
        dp += 15;
        const char *de = strchr(dp, '"');
        if (de) {
            size_t dl = (size_t)(de - dp);
            if (dl >= sizeof(dc)) dl = sizeof(dc) - 1;
            memcpy(dc, dp, dl);
        }
    }
    free(body);

    if (!dc[0]) {
        send_json(conn, 400, "{\"error\":\"Missing device_code\"}\n");
        return 1;
    }

    /* find the device code entry */
    int64_t now = (int64_t)time(NULL);
    for (int i = 0; i < srv->device_code_count; i++) {
        if (strcmp(srv->device_codes[i].device_code, dc) != 0)
            continue;

        if (srv->device_codes[i].expires_at <= now) {
            send_json(conn, 400, "{\"error\":\"expired_token\"}\n");
            return 1;
        }

        if (srv->device_codes[i].authorized == 0) {
            /* still pending */
            mg_printf(conn,
                "HTTP/1.1 428 Precondition Required\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 40\r\n\r\n"
                "{\"error\":\"authorization_pending\"}\n");
            return 1;
        }

        if (srv->device_codes[i].authorized < 0) {
            send_json(conn, 403, "{\"error\":\"access_denied\"}\n");
            /* remove entry */
            srv->device_codes[i] = srv->device_codes[--srv->device_code_count];
            return 1;
        }

        /* authorized! issue JWT */
        char *sub = srv->device_codes[i].subject;
        char *groups = srv->device_codes[i].groups;

        char *resolved = cookbook_policy_resolve(srv->db, sub);
        char *token = NULL;
        int token_version = 1;
        if (resolved) {
            token = cookbook_jwt_create_v2(sub, groups[0] ? groups : NULL,
                                            resolved, srv->jwt_ttl_sec,
                                            srv->registry_sk);
            token_version = 2;
            free(resolved);
        } else {
            token = cookbook_jwt_create(sub, groups, srv->jwt_ttl_sec,
                                         srv->registry_sk);
        }

        /* remove entry */
        srv->device_codes[i] = srv->device_codes[--srv->device_code_count];

        if (!token) {
            send_json(conn, 500, "{\"error\":\"Token creation failed\"}\n");
            return 1;
        }

        audit_log(srv, "auth", sub, "device-code-token", "ok");
        METRIC_INC(srv->metrics.responses_2xx);
        METRIC_INC(srv->metrics.auth_tokens_issued);

        char resp[8192];
        snprintf(resp, sizeof(resp),
            "{\"token\":\"%s\",\"expires_in\":%d,\"version\":%d}\n",
            token, srv->jwt_ttl_sec, token_version);
        send_json(conn, 200, resp);
        free(token);
        return 1;
    }

    send_json(conn, 400, "{\"error\":\"Invalid device_code\"}\n");
    return 1;
}

/* POST /auth/device/verify — user submits user_code + credentials to authorize */
static int handle_auth_device_verify(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    METRIC_INC(srv->metrics.requests_total);

    if (strcmp(ri->request_method, "POST") != 0) {
        send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
        return 1;
    }

    /* read body: {"user_code":"ABCD-EFGH","subject":"alice","token":"secret"} */
    size_t body_len = 0;
    char *body = read_body(conn, ri, &body_len, 4096);
    if (!body || body_len == 0) {
        free(body);
        send_json(conn, 400, "{\"error\":\"Body required\"}\n");
        return 1;
    }

    char user_code[16] = {0}, subject[128] = {0}, token[512] = {0};

    const char *p;
    p = strstr(body, "\"user_code\":\"");
    if (p) { p += 13; const char *e = strchr(p, '"');
             if (e) { size_t l = (size_t)(e-p); if (l >= sizeof(user_code)) l = sizeof(user_code)-1;
                       memcpy(user_code, p, l); } }

    p = strstr(body, "\"subject\":\"");
    if (!p) p = strstr(body, "\"sub\":\"");
    if (p) { p = strchr(p, ':') + 1; while(*p==' '||*p=='\t') p++;
             if (*p=='"') { p++; const char *e = strchr(p, '"');
             if (e) { size_t l = (size_t)(e-p); if (l >= sizeof(subject)) l = sizeof(subject)-1;
                       memcpy(subject, p, l); } } }

    p = strstr(body, "\"token\":\"");
    if (p) { p += 9; const char *e = strchr(p, '"');
             if (e) { size_t l = (size_t)(e-p); if (l >= sizeof(token)) l = sizeof(token)-1;
                       memcpy(token, p, l); } }
    free(body);

    if (!user_code[0] || !subject[0] || !token[0]) {
        send_json(conn, 400,
            "{\"error\":\"user_code, subject, and token required\"}\n");
        return 1;
    }

    /* verify credentials */
    cred_lookup_ctx clctx = { {0}, {0}, 0 };
    cookbook_db_param cp[] = { COOKBOOK_P_TEXT(subject) };
    srv->db->query_p(srv->db,
        "SELECT token_hash, groups FROM credentials "
        "WHERE subject = ?1 AND revoked_at IS NULL",
        cp, 1, cred_lookup_cb, &clctx);

    if (!clctx.found || cookbook_credential_verify(token, clctx.hash) != 0) {
        audit_log(srv, "auth", subject, "device-verify", "bad-credentials");
        send_json(conn, 401, "{\"error\":\"Invalid credentials\"}\n");
        return 1;
    }

    /* find matching device code and authorize it */
    int64_t now = (int64_t)time(NULL);
    for (int i = 0; i < srv->device_code_count; i++) {
        if (strcmp(srv->device_codes[i].user_code, user_code) != 0)
            continue;
        if (srv->device_codes[i].expires_at <= now) {
            send_json(conn, 400, "{\"error\":\"Device code expired\"}\n");
            return 1;
        }

        snprintf(srv->device_codes[i].subject,
                  sizeof(srv->device_codes[i].subject), "%s", subject);
        snprintf(srv->device_codes[i].groups,
                  sizeof(srv->device_codes[i].groups), "%s", clctx.groups);
        srv->device_codes[i].authorized = 1;

        audit_log(srv, "auth", subject, "device-verify", "ok");
        METRIC_INC(srv->metrics.responses_2xx);
        send_json(conn, 200, "{\"status\":\"authorized\"}\n");
        return 1;
    }

    send_json(conn, 404, "{\"error\":\"Unknown user_code\"}\n");
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

    /* Phase 3: auth enforcement on key registration */
    {
        cookbook_jwt_claims kclaims;
        if (!require_auth_v2(srv, conn, ri, group_id, 'c', &kclaims)) {
            return 1;
        }
        cookbook_jwt_claims_free(&kclaims);
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
    const char *grants_json;   /* v2 visibility filter (NULL = no filtering) */
    const char *exclude_json;  /* v2 exclude filter (NULL = no filtering) */
} mirror_ctx;

static int mirror_collect_cb(const cookbook_db_row *row, void *user) {
    mirror_ctx *ctx = (mirror_ctx *)user;
    /* row: group_id, artifact, version, triple */
    const char *grp = row->values[0];
    const char *art = row->values[1];
    const char *ver = row->values[2];

    if (!grp || !art || !ver) return 0;

    /* Phase 3: visibility filtering */
    if (ctx->grants_json) {
        if (!cookbook_auth_check(ctx->grants_json, ctx->exclude_json,
                                grp, 'r'))
            return 0; /* skip — not visible to this user */
    }

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

    /* Phase 3: auth enforcement on mirror manifest */
    cookbook_jwt_claims mirror_claims;
    memset(&mirror_claims, 0, sizeof(mirror_claims));
    if (srv->has_registry_key) {
        if (extract_bearer_jwt(srv, ri, &mirror_claims) != 0) {
            METRIC_INC(srv->metrics.responses_4xx);
            METRIC_INC(srv->metrics.auth_failures);
            send_json(conn, 401,
                "{\"error\":\"Valid Bearer JWT required\"}\n");
            return 1;
        }
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
                           srv->registry_id,
                           mirror_claims.grants_json,
                           mirror_claims.exclude_json };

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
                cookbook_grid_sign_ctx msctx = {
                    srv->registry_id, srv->registry_sk,
                    srv->has_registry_key
                };
                if (cookbook_grid_get_signed(&peers[pi], "/grid/manifest",
                        srv->registry_id, NULL, 0, NULL,
                        &msctx, &gresp) == 0
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
        cookbook_jwt_claims_free(&mirror_claims);
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
                       srv->registry_id,
                       mirror_claims.grants_json,
                       mirror_claims.exclude_json };

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
        cookbook_jwt_claims_free(&mirror_claims);
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
    cookbook_jwt_claims_free(&mirror_claims);
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

/* ==== grid peer auth verification ==== */

static const char *grid_get_header(const struct mg_request_info *ri,
                                     const char *name) {
    for (int i = 0; i < ri->num_headers; i++) {
        if (strcasecmp(ri->http_headers[i].name, name) == 0)
            return ri->http_headers[i].value;
    }
    return NULL;
}

/* Verify inbound grid peer signature. Returns 1 if OK, 0 if rejected.
   When grid_peer_auth is 0, unsigned requests are accepted. */
static int verify_grid_peer_sig(cookbook_server *srv,
                                  const struct mg_request_info *ri) {
    const char *origin = grid_get_header(ri, "X-Cookbook-Grid-Origin");
    const char *sig_b64 = grid_get_header(ri, "X-Cookbook-Grid-Signature");
    const char *ts_str = grid_get_header(ri, "X-Cookbook-Timestamp");

    /* No signature headers — accept if auth not required */
    if (!sig_b64 || !origin || !ts_str) {
        return srv->grid_peer_auth ? 0 : 1;
    }

    /* Check timestamp freshness (5-minute window) */
    int64_t ts = strtoll(ts_str, NULL, 10);
    int64_t now = (int64_t)time(NULL);
    int64_t diff = now - ts;
    if (diff < 0) diff = -diff;
    if (diff > 300) return 0;

    /* Look up peer's public key */
    unsigned char peer_pk[32];
    if (cookbook_grid_load_peer_key(srv->db, origin, peer_pk) != 0)
        return 0;

    /* Decode signature */
    unsigned char sig[64];
    size_t sig_len = cookbook_base64url_decode(sig_b64, strlen(sig_b64),
                                                sig, sizeof(sig));
    if (sig_len != 64) return 0;

    /* Reconstruct canonical signing input */
    const char *via = grid_get_via(ri);
    int hop_count = grid_get_hop_count(ri);
    const char *grants = grid_get_header(ri, "X-Cookbook-Grid-Grants");
    const char *exclude = grid_get_header(ri, "X-Cookbook-Grid-Exclude");

    size_t canon_len = 0;
    char *canonical = cookbook_grid_build_canonical(
        ri->request_method, ri->local_uri,
        via, hop_count,
        grants, exclude,
        ts, &canon_len);
    if (!canonical) return 0;

    int ok = (cookbook_ed25519_verify(sig, canonical, canon_len, peer_pk) == 0);
    free(canonical);
    return ok;
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
    if (!verify_grid_peer_sig(srv, ri)) {
        send_json(conn, 401, "{\"error\":\"Invalid grid peer signature\"}\n");
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

    /* Phase 3: grid grant enforcement */
    if (!check_grid_auth(ri, group, 'r')) {
        send_json(conn, 403,
            "{\"error\":\"Grid grants deny access to this group\"}\n");
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
    if (!verify_grid_peer_sig(srv, ri)) {
        send_json(conn, 401, "{\"error\":\"Invalid grid peer signature\"}\n");
        return 1;
    }

    char *path = path_after(ri->local_uri, "/grid/artifact/");
    if (!path) {
        send_json(conn, 400, "{\"error\":\"Bad request\"}\n");
        return 1;
    }

    /* Phase 3: extract group from artifact path for grid grant check.
       Path format: {group_path}/{artifact}/{version}/{filename}
       Peel off last 3 segments to get group_path, convert slashes to dots. */
    {
        const char *s3 = strrchr(path, '/');
        if (s3 && s3 > path) {
            const char *s2 = s3 - 1;
            while (s2 > path && *s2 != '/') s2--;
            if (*s2 == '/') {
                const char *s1 = s2 - 1;
                while (s1 > path && *s1 != '/') s1--;
                if (*s1 == '/') {
                    size_t grp_len = (size_t)(s1 - path);
                    char grid_group[256] = {0};
                    if (grp_len < sizeof(grid_group)) {
                        memcpy(grid_group, path, grp_len);
                        grid_group[grp_len] = '\0';
                        for (size_t gi = 0; gi < grp_len; gi++)
                            if (grid_group[gi] == '/') grid_group[gi] = '.';
                        if (!check_grid_auth(ri, grid_group, 'r')) {
                            send_json(conn, 403,
                                "{\"error\":\"Grid grants deny access\"}\n");
                            free(path);
                            return 1;
                        }
                    }
                }
            }
        }
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
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    if (!verify_grid_peer_sig(srv, ri)) {
        send_json(conn, 401, "{\"error\":\"Invalid grid peer signature\"}\n");
        return 1;
    }
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
            if (peers[i].has_public_key) {
                char pk_hex[65];
                for (int b = 0; b < 32; b++)
                    snprintf(pk_hex + b * 2, 3, "%02x",
                              peers[i].public_key[b]);
                pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"peer_id\":\"%s\",\"name\":\"%s\",\"url\":\"%s\","
                    "\"mode\":\"%s\",\"priority\":%d,"
                    "\"public_key\":\"%s\"}",
                    peers[i].peer_id, peers[i].name, peers[i].url,
                    peers[i].mode == 'p' ? "proxy" : "redirect",
                    peers[i].priority, pk_hex);
            } else {
                pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"peer_id\":\"%s\",\"name\":\"%s\",\"url\":\"%s\","
                    "\"mode\":\"%s\",\"priority\":%d}",
                    peers[i].peer_id, peers[i].name, peers[i].url,
                    peers[i].mode == 'p' ? "proxy" : "redirect",
                    peers[i].priority);
            }
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

        char public_key[128] = {0};
        GRID_PARSE_STR(peer_id, "peer_id", sizeof(peer_id));
        GRID_PARSE_STR(name, "name", sizeof(name));
        GRID_PARSE_STR(url, "url", sizeof(url));
        GRID_PARSE_STR(mode, "mode", sizeof(mode));
        GRID_PARSE_STR(public_key, "public_key", sizeof(public_key));
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

        /* validate public_key if provided: must be exactly 64 hex chars */
        const char *pk_sql = NULL;
        if (public_key[0]) {
            if (strlen(public_key) != 64) {
                send_json(conn, 400,
                    "{\"error\":\"public_key must be 64 hex characters\"}\n");
                return 1;
            }
            for (size_t k = 0; k < 64; k++) {
                char c = public_key[k];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                      (c >= 'A' && c <= 'F'))) {
                    send_json(conn, 400,
                        "{\"error\":\"public_key contains non-hex chars\"}\n");
                    return 1;
                }
            }
            pk_sql = public_key;
        }

        cookbook_db_param pp2[] = {
            COOKBOOK_P_TEXT(peer_id),
            COOKBOOK_P_TEXT(name),
            COOKBOOK_P_TEXT(url),
            COOKBOOK_P_TEXT(mode_str),
            COOKBOOK_P_INT(priority),
            pk_sql ? (cookbook_db_param)COOKBOOK_P_TEXT(pk_sql)
                   : (cookbook_db_param)COOKBOOK_P_NULL()
        };
        /* upsert: try insert, on conflict update */
        cookbook_db_status st = srv->db->exec_p(srv->db,
            "INSERT OR REPLACE INTO peers "
            "(peer_id, name, url, mode, priority, enabled, public_key, added_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, 1, ?6, datetime('now'))",
            pp2, 6);

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

/* ==== Object cache: /objects/{key} ==== */

static int handle_objects(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    METRIC_INC(srv->metrics.requests_total);

    /* extract cache key from /objects/{key} */
    char *cache_key = path_after(ri->local_uri, "/objects/");
    if (!cache_key || !cache_key[0]) {
        free(cache_key);
        METRIC_INC(srv->metrics.responses_4xx);
        send_json(conn, 400, "{\"error\":\"Missing cache key\"}\n");
        return 1;
    }

    /* validate: key must be hex chars + optional extension (.o, .obj, etc.)
       reject path traversal */
    if (validate_path_segment(cache_key) != 0) {
        free(cache_key);
        METRIC_INC(srv->metrics.responses_4xx);
        send_json(conn, 400, "{\"error\":\"Invalid cache key\"}\n");
        return 1;
    }

    /* build store key: {registry_id}/objects/{cache_key} */
    size_t store_key_len = strlen(srv->registry_id) + 9 + strlen(cache_key);
    char *store_key = malloc(store_key_len + 1);
    if (!store_key) { free(cache_key); return 1; }
    snprintf(store_key, store_key_len + 1,
             "%s/objects/%s", srv->registry_id, cache_key);

    if (strcmp(ri->request_method, "GET") == 0) {
        METRIC_INC(srv->metrics.requests_get);

        void *data = NULL;
        size_t len = 0;
        cookbook_store_status sst = srv->store->get(srv->store,
                                                     store_key, &data, &len);
        if (sst == COOKBOOK_STORE_NOT_FOUND) {
            METRIC_INC(srv->metrics.responses_4xx);
            send_json(conn, 404, "{\"error\":\"Object not found\"}\n");
        } else {
            METRIC_INC(srv->metrics.responses_2xx);
            METRIC_ADD(srv->metrics.bytes_downloaded, (long)len);
            mg_send_http_ok(conn, "application/octet-stream", (long long)len);
            mg_write(conn, data, len);
        }
        free(data);
        free(store_key);
        free(cache_key);
        return 1;
    }

    if (strcmp(ri->request_method, "HEAD") == 0) {
        METRIC_INC(srv->metrics.requests_get);

        void *data = NULL;
        size_t len = 0;
        cookbook_store_status sst = srv->store->get(srv->store,
                                                     store_key, &data, &len);
        free(data);
        if (sst == COOKBOOK_STORE_NOT_FOUND) {
            mg_printf(conn,
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Length: 0\r\n\r\n");
        } else {
            mg_printf(conn,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Content-Length: %zu\r\n\r\n", len);
        }
        free(store_key);
        free(cache_key);
        return 1;
    }

    if (strcmp(ri->request_method, "PUT") == 0) {
        METRIC_INC(srv->metrics.requests_put);

        /* auth: require 'c' on _objects prefix */
        cookbook_jwt_claims claims;
        if (!require_auth_v2(srv, conn, ri, "_objects", 'c', &claims)) {
            free(store_key);
            free(cache_key);
            return 1;
        }
        cookbook_jwt_claims_free(&claims);

        /* read body */
        size_t max = srv->max_upload_bytes > 0
            ? srv->max_upload_bytes : 256 * 1024 * 1024;
        size_t body_len = 0;
        char *body = read_body(conn, ri, &body_len, max);
        if (!body || body_len == 0) {
            free(body);
            free(store_key);
            free(cache_key);
            METRIC_INC(srv->metrics.responses_4xx);
            send_json(conn, 400, "{\"error\":\"Empty body\"}\n");
            return 1;
        }

        cookbook_store_status sst = srv->store->put(srv->store,
                                                     store_key, body, body_len);
        free(body);

        if (sst == COOKBOOK_STORE_OK) {
            METRIC_INC(srv->metrics.responses_2xx);
            METRIC_ADD(srv->metrics.bytes_uploaded, (long)body_len);

            /* track in object_cache table for TTL eviction */
            {
                char now_epoch[32];
                snprintf(now_epoch, sizeof(now_epoch), "%lld",
                         (long long)time(NULL));
                char sz_str[32];
                snprintf(sz_str, sizeof(sz_str), "%zu", body_len);
                cookbook_db_param op[] = {
                    COOKBOOK_P_TEXT(cache_key),
                    COOKBOOK_P_TEXT(store_key),
                    COOKBOOK_P_TEXT(sz_str),
                    COOKBOOK_P_TEXT(now_epoch)
                };
                srv->db->exec_p(srv->db,
                    "INSERT OR REPLACE INTO object_cache "
                    "(cache_key, store_key, size_bytes, created_at) "
                    "VALUES (?1, ?2, ?3, ?4)",
                    op, 4);
            }

            audit_log(srv, "objects", claims.sub, cache_key, "stored");
            send_json(conn, 201, "{\"status\":\"stored\"}\n");
        } else {
            send_json(conn, 500, "{\"error\":\"Store write failed\"}\n");
        }
        free(store_key);
        free(cache_key);
        return 1;
    }

    METRIC_INC(srv->metrics.responses_4xx);
    send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
    free(store_key);
    free(cache_key);
    return 1;
}

/* ==== Group management: /admin/groups ==== */

/* callback for listing groups */
typedef struct {
    char *buf;
    size_t pos;
    size_t cap;
    int count;
} group_list_ctx;

static int group_list_cb(const cookbook_db_row *row, void *user) {
    group_list_ctx *ctx = (group_list_ctx *)user;
    if (row->ncols < 4 || !row->values[0]) return 0;

    const char *gid   = row->values[0] ? row->values[0] : "";
    const char *owner = row->values[1] ? row->values[1] : "";
    const char *cdate = row->values[2] ? row->values[2] : "";
    const char *desc  = row->values[3];

    if (ctx->count > 0 && ctx->pos < ctx->cap)
        ctx->buf[ctx->pos++] = ',';

    int n;
    if (desc) {
        n = snprintf(ctx->buf + ctx->pos, ctx->cap - ctx->pos,
            "{\"group_id\":\"%s\",\"owner\":\"%s\",\"created_at\":\"%s\","
            "\"description\":\"%s\"}", gid, owner, cdate, desc);
    } else {
        n = snprintf(ctx->buf + ctx->pos, ctx->cap - ctx->pos,
            "{\"group_id\":\"%s\",\"owner\":\"%s\",\"created_at\":\"%s\"}",
            gid, owner, cdate);
    }
    if (n > 0) ctx->pos += (size_t)n;
    ctx->count++;
    return 0;
}

/* callback for counting rows (DELETE artifact check) */
typedef struct { int count; } group_count_ctx;

static int group_count_cb(const cookbook_db_row *row, void *user) {
    group_count_ctx *c = (group_count_ctx *)user;
    if (row->ncols > 0 && row->values[0])
        c->count = atoi(row->values[0]);
    return 0;
}

static int handle_admin_groups(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    METRIC_INC(srv->metrics.requests_total);

    /* extract sub-path: /admin/groups or /admin/groups/{group_id} */
    const char *uri = ri->local_uri;
    const char *path = uri + strlen("/admin/groups");
    char group_id[256] = {0};

    if (path[0] == '/' && path[1]) {
        const char *start = path + 1;
        size_t slen = strlen(start);
        /* strip trailing slash */
        while (slen > 0 && start[slen - 1] == '/') slen--;
        if (slen >= sizeof(group_id)) slen = sizeof(group_id) - 1;
        memcpy(group_id, start, slen);
        group_id[slen] = '\0';

        /* convert path slashes back to dots for group_id
           (e.g., /admin/groups/com/iridiumfx → com.iridiumfx) */
        for (size_t i = 0; i < slen; i++) {
            if (group_id[i] == '/') group_id[i] = '.';
        }
    }

    if (strcmp(ri->request_method, "GET") == 0) {
        if (group_id[0]) {
            /* GET /admin/groups/{group_id} — get single group */
            cookbook_db_param gp[] = { COOKBOOK_P_TEXT(group_id) };
            char buf[2048] = {0};
            size_t pos = 0;
            group_list_ctx ctx = { buf, pos, sizeof(buf), 0 };
            srv->db->query_p(srv->db,
                "SELECT group_id, owner_sub, created_at, description "
                "FROM groups WHERE group_id = ?1",
                gp, 1, group_list_cb, &ctx);
            if (ctx.count == 0) {
                METRIC_INC(srv->metrics.responses_4xx);
                send_json(conn, 404, "{\"error\":\"Group not found\"}\n");
            } else {
                METRIC_INC(srv->metrics.responses_2xx);
                /* wrap single result in newline */
                size_t end = ctx.pos;
                buf[end++] = '\n';
                buf[end] = '\0';
                send_json(conn, 200, buf);
            }
        } else {
            /* GET /admin/groups — list all groups */
            char buf[16384] = {0};
            size_t pos = 0;
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                "{\"groups\":[");

            group_list_ctx ctx = { buf, pos, sizeof(buf), 0 };
            srv->db->query(srv->db,
                "SELECT group_id, owner_sub, created_at, description "
                "FROM groups ORDER BY group_id",
                group_list_cb, &ctx);
            pos = ctx.pos;
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "]}\n");
            METRIC_INC(srv->metrics.responses_2xx);
            send_json(conn, 200, buf);
        }
        return 1;
    }

    if (strcmp(ri->request_method, "PUT") == 0) {
        /* PUT /admin/groups — create/register a group
           Body: {"group_id":"com.iridiumfx","description":"..."} */
        size_t body_len = 0;
        char *body = read_body(conn, ri, &body_len, 4096);
        if (!body || body_len == 0) {
            free(body);
            METRIC_INC(srv->metrics.responses_4xx);
            send_json(conn, 400, "{\"error\":\"Body required\"}\n");
            return 1;
        }

        /* extract group_id and optional description from JSON body */
        char gid[256] = {0}, desc[512] = {0};
        const char *p;

        p = strstr(body, "\"group_id\":\"");
        if (p) {
            p += strlen("\"group_id\":\"");
            const char *e = strchr(p, '"');
            if (e) {
                size_t l = (size_t)(e - p);
                if (l >= sizeof(gid)) l = sizeof(gid) - 1;
                memcpy(gid, p, l);
                gid[l] = '\0';
            }
        }

        p = strstr(body, "\"description\":\"");
        if (p) {
            p += strlen("\"description\":\"");
            const char *e = strchr(p, '"');
            if (e) {
                size_t l = (size_t)(e - p);
                if (l >= sizeof(desc)) l = sizeof(desc) - 1;
                memcpy(desc, p, l);
                desc[l] = '\0';
            }
        }

        if (!gid[0]) {
            free(body);
            METRIC_INC(srv->metrics.responses_4xx);
            send_json(conn, 400,
                "{\"error\":\"Missing group_id\"}\n");
            return 1;
        }

        /* auth: require 'c' permission on the group prefix */
        cookbook_jwt_claims claims;
        if (!require_auth_v2(srv, conn, ri, gid, 'c', &claims)) {
            free(body);
            return 1;
        }

        const char *owner = claims.sub[0] ? claims.sub : "anonymous";

        char now_ts[64];
        utc_now(now_ts, sizeof(now_ts));

        if (desc[0]) {
            cookbook_db_param gp[] = {
                COOKBOOK_P_TEXT(gid),
                COOKBOOK_P_TEXT(owner),
                COOKBOOK_P_TEXT(now_ts),
                COOKBOOK_P_TEXT(desc)
            };
            int rc = srv->db->exec_p(srv->db,
                "INSERT INTO groups (group_id, owner_sub, created_at, description) "
                "VALUES (?1, ?2, ?3, ?4)",
                gp, 4);
            if (rc != COOKBOOK_DB_OK) {
                /* likely duplicate */
                METRIC_INC(srv->metrics.responses_4xx);
                send_json(conn, 409,
                    "{\"error\":\"Group already exists\"}\n");
                cookbook_jwt_claims_free(&claims);
                free(body);
                return 1;
            }
        } else {
            cookbook_db_param gp[] = {
                COOKBOOK_P_TEXT(gid),
                COOKBOOK_P_TEXT(owner),
                COOKBOOK_P_TEXT(now_ts)
            };
            int rc = srv->db->exec_p(srv->db,
                "INSERT INTO groups (group_id, owner_sub, created_at) "
                "VALUES (?1, ?2, ?3)",
                gp, 3);
            if (rc != COOKBOOK_DB_OK) {
                METRIC_INC(srv->metrics.responses_4xx);
                send_json(conn, 409,
                    "{\"error\":\"Group already exists\"}\n");
                cookbook_jwt_claims_free(&claims);
                free(body);
                return 1;
            }
        }

        METRIC_INC(srv->metrics.responses_2xx);
        audit_log(srv, "admin", owner, gid, "group-created");
        char resp[512];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"created\",\"group_id\":\"%s\",\"owner\":\"%s\"}\n",
            gid, owner);
        send_json(conn, 201, resp);
        cookbook_jwt_claims_free(&claims);
        free(body);
        return 1;
    }

    if (strcmp(ri->request_method, "PATCH") == 0 && group_id[0]) {
        /* PATCH /admin/groups/{group_id} — update owner or description
           Body: {"owner":"bob"} or {"description":"new desc"} or both */
        size_t body_len = 0;
        char *body = read_body(conn, ri, &body_len, 4096);
        if (!body || body_len == 0) {
            free(body);
            METRIC_INC(srv->metrics.responses_4xx);
            send_json(conn, 400, "{\"error\":\"Body required\"}\n");
            return 1;
        }

        /* auth: require 'w' on the group to modify it */
        cookbook_jwt_claims claims;
        if (!require_auth_v2(srv, conn, ri, group_id, 'w', &claims)) {
            free(body);
            return 1;
        }

        char new_owner[128] = {0}, new_desc[512] = {0};
        int has_owner = 0, has_desc = 0;
        const char *p;

        p = strstr(body, "\"owner\":\"");
        if (p) {
            p += strlen("\"owner\":\"");
            const char *e = strchr(p, '"');
            if (e) {
                size_t l = (size_t)(e - p);
                if (l >= sizeof(new_owner)) l = sizeof(new_owner) - 1;
                memcpy(new_owner, p, l);
                new_owner[l] = '\0';
                has_owner = 1;
            }
        }

        p = strstr(body, "\"description\":\"");
        if (p) {
            p += strlen("\"description\":\"");
            const char *e = strchr(p, '"');
            if (e) {
                size_t l = (size_t)(e - p);
                if (l >= sizeof(new_desc)) l = sizeof(new_desc) - 1;
                memcpy(new_desc, p, l);
                new_desc[l] = '\0';
                has_desc = 1;
            }
        }

        if (!has_owner && !has_desc) {
            free(body);
            cookbook_jwt_claims_free(&claims);
            METRIC_INC(srv->metrics.responses_4xx);
            send_json(conn, 400,
                "{\"error\":\"Nothing to update (provide owner or description)\"}\n");
            return 1;
        }

        int ok = 1;
        if (has_owner) {
            cookbook_db_param up[] = {
                COOKBOOK_P_TEXT(new_owner),
                COOKBOOK_P_TEXT(group_id)
            };
            if (srv->db->exec_p(srv->db,
                    "UPDATE groups SET owner_sub = ?1 WHERE group_id = ?2",
                    up, 2) != COOKBOOK_DB_OK)
                ok = 0;
        }
        if (has_desc) {
            cookbook_db_param up[] = {
                COOKBOOK_P_TEXT(new_desc),
                COOKBOOK_P_TEXT(group_id)
            };
            if (srv->db->exec_p(srv->db,
                    "UPDATE groups SET description = ?1 WHERE group_id = ?2",
                    up, 2) != COOKBOOK_DB_OK)
                ok = 0;
        }

        if (ok) {
            METRIC_INC(srv->metrics.responses_2xx);
            audit_log(srv, "admin", claims.sub, group_id, "group-updated");
            send_json(conn, 200, "{\"status\":\"updated\"}\n");
        } else {
            send_json(conn, 404, "{\"error\":\"Group not found\"}\n");
        }
        cookbook_jwt_claims_free(&claims);
        free(body);
        return 1;
    }

    if (strcmp(ri->request_method, "DELETE") == 0 && group_id[0]) {
        /* DELETE /admin/groups/{group_id} — remove a group
           Refuses if artifacts still reference it. */
        cookbook_jwt_claims claims;
        if (!require_auth_v2(srv, conn, ri, group_id, 'd', &claims)) {
            return 1;
        }

        /* check for existing artifacts in this group */
        cookbook_db_param cp[] = { COOKBOOK_P_TEXT(group_id) };
        group_count_ctx cctx = { 0 };
        srv->db->query_p(srv->db,
            "SELECT COUNT(*) FROM artifacts WHERE group_id = ?1",
            cp, 1, group_count_cb, &cctx);

        if (cctx.count > 0) {
            char err[256];
            snprintf(err, sizeof(err),
                "{\"error\":\"Cannot delete group '%s': "
                "%d artifact(s) still reference it\"}\n",
                group_id, cctx.count);
            METRIC_INC(srv->metrics.responses_4xx);
            send_json(conn, 409, err);
            cookbook_jwt_claims_free(&claims);
            return 1;
        }

        cookbook_db_param dp[] = { COOKBOOK_P_TEXT(group_id) };
        int rc = srv->db->exec_p(srv->db,
            "DELETE FROM groups WHERE group_id = ?1", dp, 1);
        if (rc == COOKBOOK_DB_OK) {
            METRIC_INC(srv->metrics.responses_2xx);
            audit_log(srv, "admin", claims.sub, group_id, "group-deleted");
            send_json(conn, 200, "{\"status\":\"deleted\"}\n");
        } else {
            send_json(conn, 404, "{\"error\":\"Group not found\"}\n");
        }
        cookbook_jwt_claims_free(&claims);
        return 1;
    }

    METRIC_INC(srv->metrics.responses_4xx);
    send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
    return 1;
}

/* ==== Credential management: /admin/credentials ==== */

/* callback for listing credentials */
typedef struct {
    char *buf;
    size_t pos;
    size_t cap;
    int count;
} cred_list_ctx;

static int cred_list_cb(const cookbook_db_row *row, void *user) {
    cred_list_ctx *ctx = (cred_list_ctx *)user;
    if (row->ncols < 4 || !row->values[0]) return 0;

    /* subject, groups, created_at, revoked_at */
    const char *subject = row->values[0] ? row->values[0] : "";
    const char *groups = row->values[1] ? row->values[1] : "";
    const char *created = row->values[2] ? row->values[2] : "";
    int revoked = row->values[3] != NULL;

    if (ctx->count > 0 && ctx->pos < ctx->cap)
        ctx->buf[ctx->pos++] = ',';

    int n = snprintf(ctx->buf + ctx->pos, ctx->cap - ctx->pos,
        "{\"subject\":\"%s\",\"groups\":\"%s\",\"created_at\":\"%s\"%s}",
        subject, groups, created,
        revoked ? ",\"revoked\":true" : "");
    if (n > 0) ctx->pos += (size_t)n;
    ctx->count++;
    return 0;
}

static int handle_admin_credentials(struct mg_connection *conn, void *cbdata) {
    cookbook_server *srv = (cookbook_server *)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    METRIC_INC(srv->metrics.requests_total);

    /* extract sub-path: /admin/credentials or /admin/credentials/{subject}
       or /admin/credentials/{subject}/revoke */
    const char *uri = ri->local_uri;
    const char *path = uri + strlen("/admin/credentials");
    char subject[128] = {0};
    int is_revoke = 0;

    if (path[0] == '/' && path[1]) {
        const char *start = path + 1;
        /* check for /revoke suffix */
        const char *revoke_suf = strstr(start, "/revoke");
        if (revoke_suf) {
            size_t slen = (size_t)(revoke_suf - start);
            if (slen >= sizeof(subject)) slen = sizeof(subject) - 1;
            memcpy(subject, start, slen);
            subject[slen] = '\0';
            is_revoke = 1;
        } else {
            size_t slen = strlen(start);
            if (slen >= sizeof(subject)) slen = sizeof(subject) - 1;
            memcpy(subject, start, slen);
            subject[slen] = '\0';
        }
    }

    if (strcmp(ri->request_method, "GET") == 0) {
        /* list all credentials (no password hashes) */
        char buf[8192];
        size_t pos = 0;
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
            "{\"credentials\":[");

        cred_list_ctx ctx = { buf, pos, sizeof(buf), 0 };
        srv->db->query(srv->db,
            "SELECT subject, groups, created_at, revoked_at "
            "FROM credentials ORDER BY subject",
            cred_list_cb, &ctx);
        pos = ctx.pos;

        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "]}\n");
        METRIC_INC(srv->metrics.responses_2xx);
        send_json(conn, 200, buf);
        return 1;
    }

    if (strcmp(ri->request_method, "POST") == 0 && is_revoke && subject[0]) {
        /* POST /admin/credentials/{subject}/revoke */
        cookbook_db_param rp[] = { COOKBOOK_P_TEXT(subject) };
        int rc = srv->db->exec_p(srv->db,
            "UPDATE credentials SET revoked_at = datetime('now') "
            "WHERE subject = ?1 AND revoked_at IS NULL",
            rp, 1);
        if (rc == COOKBOOK_DB_OK) {
            METRIC_INC(srv->metrics.responses_2xx);
            audit_log(srv, "admin", subject, "credential-revoke", "ok");
            send_json(conn, 200, "{\"status\":\"revoked\"}\n");
        } else {
            send_json(conn, 500,
                "{\"error\":\"Failed to revoke credential\"}\n");
        }
        return 1;
    }

    if (strcmp(ri->request_method, "PUT") == 0) {
        /* PUT /admin/credentials — create/update credential
           Body: {"subject":"alice","token":"secret123","groups":"admin,publish"} */
        size_t body_len = 0;
        char *body = read_body(conn, ri, &body_len, 4096);
        if (!body || body_len == 0) {
            free(body);
            METRIC_INC(srv->metrics.responses_4xx);
            send_json(conn, 400,
                "{\"error\":\"Body required\"}\n");
            return 1;
        }

        char cred_sub[128] = {0}, token[512] = {0}, groups[1024] = {0};

        /* parse subject */
        const char *sp = strstr(body, "\"subject\":\"");
        if (sp) {
            sp += 11;
            const char *se = strchr(sp, '"');
            if (se) {
                size_t len = (size_t)(se - sp);
                if (len >= sizeof(cred_sub)) len = sizeof(cred_sub) - 1;
                memcpy(cred_sub, sp, len);
            }
        }

        /* parse token */
        const char *tp = strstr(body, "\"token\":\"");
        if (tp) {
            tp += 9;
            const char *te = strchr(tp, '"');
            if (te) {
                size_t len = (size_t)(te - tp);
                if (len >= sizeof(token)) len = sizeof(token) - 1;
                memcpy(token, tp, len);
            }
        }

        /* parse groups */
        const char *gp = strstr(body, "\"groups\":\"");
        if (gp) {
            gp += 10;
            const char *ge = strchr(gp, '"');
            if (ge) {
                size_t len = (size_t)(ge - gp);
                if (len >= sizeof(groups)) len = sizeof(groups) - 1;
                memcpy(groups, gp, len);
            }
        }

        free(body);

        if (!cred_sub[0] || !token[0] || !groups[0]) {
            METRIC_INC(srv->metrics.responses_4xx);
            send_json(conn, 400,
                "{\"error\":\"subject, token, and groups required\"}\n");
            return 1;
        }

        /* hash the token with Argon2id */
        char *hash = cookbook_credential_hash(token);
        if (!hash) {
            send_json(conn, 500,
                "{\"error\":\"Failed to hash credential\"}\n");
            return 1;
        }

        /* insert or update */
        cookbook_db_param ip[] = {
            COOKBOOK_P_TEXT(cred_sub),
            COOKBOOK_P_TEXT(hash),
            COOKBOOK_P_TEXT(groups)
        };
        int rc = srv->db->exec_p(srv->db,
            "INSERT OR REPLACE INTO credentials "
            "(subject, token_hash, groups, created_at, revoked_at) "
            "VALUES (?1, ?2, ?3, datetime('now'), NULL)",
            ip, 3);
        free(hash);

        if (rc == COOKBOOK_DB_OK) {
            METRIC_INC(srv->metrics.responses_2xx);
            audit_log(srv, "admin", cred_sub, "credential-create", "ok");
            char resp[256];
            snprintf(resp, sizeof(resp),
                "{\"status\":\"created\",\"subject\":\"%s\"}\n",
                cred_sub);
            send_json(conn, 201, resp);
        } else {
            send_json(conn, 500,
                "{\"error\":\"Failed to store credential\"}\n");
        }
        return 1;
    }

    if (strcmp(ri->request_method, "DELETE") == 0 && subject[0]) {
        /* DELETE /admin/credentials/{subject} — hard delete */
        cookbook_db_param dp[] = { COOKBOOK_P_TEXT(subject) };
        int rc = srv->db->exec_p(srv->db,
            "DELETE FROM credentials WHERE subject = ?1",
            dp, 1);
        if (rc == COOKBOOK_DB_OK) {
            METRIC_INC(srv->metrics.responses_2xx);
            audit_log(srv, "admin", subject, "credential-delete", "ok");
            send_json(conn, 200, "{\"status\":\"deleted\"}\n");
        } else {
            send_json(conn, 500,
                "{\"error\":\"Failed to delete credential\"}\n");
        }
        return 1;
    }

    METRIC_INC(srv->metrics.responses_4xx);
    send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
    return 1;
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
                "Content-Type: application/x-pasta; charset=US-ASCII\r\n"
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
        audit_log(srv, "admin", subject, "policy-put", "ok");
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
        audit_log(srv, "admin", path, "policy-delete", "ok");
        cookbook_policy_delete(srv->db, path);
        free(path);
        send_json(conn, 200, "{\"status\":\"deleted\"}\n");
        return 1;
    }

    send_json(conn, 405, "{\"error\":\"Method not allowed\"}\n");
    return 1;
}

/* ==== public API ==== */

/* callback for loading persisted revocations at startup */
typedef struct { cookbook_revocation_list *rl; int loaded; } revoke_load_ctx;

static int revoke_load_cb(const cookbook_db_row *row, void *user) {
    revoke_load_ctx *ctx = (revoke_load_ctx *)user;
    if (row->ncols < 2 || !row->values[0] || !row->values[1]) return 0;
    const char *jti = row->values[0];
    int64_t exp = (int64_t)strtoll(row->values[1], NULL, 10);
    if (cookbook_revocation_add(ctx->rl, jti, exp) == 0)
        ctx->loaded++;
    return 0;
}

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
    srv->grid_peer_auth = opts->grid_peer_auth;
    srv->object_cache_ttl_sec = opts->object_cache_ttl_sec;

    /* LDAP backend config */
    if (opts->ldap_url) {
        srv->ldap_cfg.url       = opts->ldap_url;
        srv->ldap_cfg.base_dn   = opts->ldap_base_dn;
        srv->ldap_cfg.user_attr = opts->ldap_user_attr;
        fprintf(stdout, "cookbook: LDAP backend: %s (base: %s)\n",
                opts->ldap_url,
                opts->ldap_base_dn ? opts->ldap_base_dn : "(none)");
    }

    /* OIDC backend config */
    if (opts->oidc_issuer) {
        srv->oidc_cfg.issuer    = opts->oidc_issuer;
        srv->oidc_cfg.client_id = opts->oidc_client_id;
        fprintf(stdout, "cookbook: OIDC backend: %s\n", opts->oidc_issuer);
    }

    /* initialize token revocation list (max 4096 entries) */
    cookbook_revocation_init(&srv->revocations, 4096);

    /* load persisted revocations from database */
    {
        revoke_load_ctx rctx = { &srv->revocations, 0 };
        char exp_str[32];
        snprintf(exp_str, sizeof(exp_str), "%lld", (long long)time(NULL));
        cookbook_db_param tp[] = { COOKBOOK_P_TEXT(exp_str) };
        srv->db->query_p(srv->db,
            "SELECT jti, expires_at FROM revocations "
            "WHERE expires_at > ?1",
            tp, 1, revoke_load_cb, &rctx);
        if (rctx.loaded > 0)
            fprintf(stdout, "cookbook: loaded %d persisted revocations\n",
                    rctx.loaded);

        /* prune expired entries from DB */
        srv->db->exec_p(srv->db,
            "DELETE FROM revocations WHERE expires_at <= ?1",
            tp, 1);
    }

    /* initialize rate limiter lock */
#ifdef _WIN32
    InitializeCriticalSection(&srv->rate_lock);
    InitializeCriticalSection(&srv->audit_lock);
#else
    pthread_mutex_init(&srv->rate_lock, NULL);
    pthread_mutex_init(&srv->audit_lock, NULL);
#endif

    /* audit logs — three separate files by category */
    if (opts->audit_log_dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/audit-auth.pasta", opts->audit_log_dir);
        srv->audit_auth = fopen(path, "a");
        snprintf(path, sizeof(path), "%s/audit-access.pasta", opts->audit_log_dir);
        srv->audit_access = fopen(path, "a");
        snprintf(path, sizeof(path), "%s/audit-admin.pasta", opts->audit_log_dir);
        srv->audit_admin = fopen(path, "a");
        if (srv->audit_auth && srv->audit_access && srv->audit_admin)
            fprintf(stdout, "cookbook: audit logs: %s/audit-{auth,access,admin}.pasta\n",
                    opts->audit_log_dir);
        else
            fprintf(stderr, "cookbook: warning: some audit logs failed to open in %s\n",
                    opts->audit_log_dir);
    }

    if (srv->object_cache_ttl_sec > 0)
        fprintf(stdout, "cookbook: object cache TTL: %d sec\n",
                srv->object_cache_ttl_sec);

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
    mg_set_request_handler(srv->ctx, "/.well-known/now-registry",
                           handle_registry_discovery, srv);
    mg_set_request_handler(srv->ctx, "/auth/token", handle_auth_token, srv);
    mg_set_request_handler(srv->ctx, "/auth/revoke", handle_auth_revoke, srv);
    mg_set_request_handler(srv->ctx, "/auth/device/token",
                           handle_auth_device_token, srv);
    mg_set_request_handler(srv->ctx, "/auth/device/verify",
                           handle_auth_device_verify, srv);
    mg_set_request_handler(srv->ctx, "/auth/device",
                           handle_auth_device, srv);
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

    /* object cache endpoints */
    mg_set_request_handler(srv->ctx, "/objects/",
                           handle_objects, srv);

    /* group management endpoints */
    mg_set_request_handler(srv->ctx, "/admin/groups",
                           handle_admin_groups, srv);

    /* credential management endpoints */
    mg_set_request_handler(srv->ctx, "/admin/credentials",
                           handle_admin_credentials, srv);

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

    /* clean up revocation list */
    cookbook_revocation_free(&srv->revocations);

    /* clean up rate limit buckets */
    rate_bucket *b = srv->rate_buckets;
    while (b) {
        rate_bucket *next = b->next;
        free(b);
        b = next;
    }

#ifdef _WIN32
    DeleteCriticalSection(&srv->rate_lock);
    DeleteCriticalSection(&srv->audit_lock);
#else
    pthread_mutex_destroy(&srv->rate_lock);
    pthread_mutex_destroy(&srv->audit_lock);
#endif

    if (srv->audit_auth)   fclose(srv->audit_auth);
    if (srv->audit_access) fclose(srv->audit_access);
    if (srv->audit_admin)  fclose(srv->audit_admin);

    sodium_memzero(srv->registry_sk, 64);
    free(srv->registry_id);
    free(srv);
}
