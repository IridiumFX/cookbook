#include "apennines/t4/net/http_server.h"
#include "apennines/t3/net/tcp.h"
#include "apennines/t3/net/http.h"
#include "apennines/t3/net/tls.h"
#include "apennines/t3/async/threadpool.h"
#include "apennines/t2/net/addr.h"
#include "apennines/t1/sync/thread/thread.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ================================================================
 *  Constants
 * ================================================================ */

#define DEFAULT_THREADS         8
#define DEFAULT_MAX_CONNECTIONS 1024
#define DEFAULT_MAX_BODY_SIZE   (8 * 1024 * 1024)
#define DEFAULT_KEEP_ALIVE_MS   5000
#define DEFAULT_READ_TIMEOUT_MS 30000
#define DEFAULT_WRITE_TIMEOUT_MS 30000
#define READ_BUF_SIZE           8192
#define MAX_ROUTES              256
#define MAX_MIDDLEWARES          64
#define MAX_ROUTE_PARAMS        32
#define MAX_RESP_HEADERS        64
#define INITIAL_RESP_BUF        4096
#define PATH_BUF_SIZE           2048

/* ================================================================
 *  Internal helpers
 * ================================================================ */

static char *s_dup(const char *s) {
    size_t len;
    char *d;
    if (!s) return NULL;
    len = strlen(s);
    d = (char *)malloc(len + 1);
    if (!d) return NULL;
    memcpy(d, s, len + 1);
    return d;
}

static char *s_ndup(const char *s, size_t n) {
    char *d;
    if (!s) return NULL;
    d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static int ci_eq(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    }
    return *a == *b;
}

/* ================================================================
 *  Route param entry
 * ================================================================ */

typedef struct {
    char *name;
    char *value;
} route_param;

/* ================================================================
 *  Route definition
 * ================================================================ */

typedef struct {
    int             method;
    char           *pattern;
    http_handler_fn handler;
    /* static file serving */
    int             is_static;
    char           *static_dir;
} http_route;

/* ================================================================
 *  http_ctx — full definition
 * ================================================================ */

struct http_ctx {
    /* parsed request */
    int             method;
    char           *path;
    char           *query_string;
    http_headers    req_headers;
    u8             *body;
    u64             body_len;

    /* route params */
    route_param     params[MAX_ROUTE_PARAMS];
    u32             param_count;

    /* response state */
    u16             resp_status;
    http_headers    resp_headers;
    u8             *resp_body;
    u64             resp_body_len;
    int             resp_sent;
    int             stream_started;  /* headers sent via http_ctx_respond_stream */

    /* connection */
    tcp_conn       *conn;
    tls_conn       *tls;   /* non-NULL when the server was configured for TLS */
    int             is_tls;

    /* back-pointer to server (for config) */
    struct http_server *server;

    /* remote address */
    char            remote_addr[64];
};

/* ================================================================
 *  http_server — full definition
 * ================================================================ */

struct http_server {
    /* routes */
    http_route      routes[MAX_ROUTES];
    u32             route_count;

    /* middleware */
    http_middleware_fn middlewares[MAX_MIDDLEWARES];
    u32             mw_count;

    /* threadpool */
    threadpool     *pool;
    u32             num_threads;

    /* listener */
    tcp_listener    listener;
    int             listening;

    /* TLS: raw DER cert and key bytes + a live tls_config built from
     * them at http_server_start time. When tls_cfg != NULL, incoming
     * tcp_conns are wrapped in a tls_conn before any HTTP read. */
    u8             *tls_cert;
    u64             tls_cert_len;
    u8             *tls_key;
    u64             tls_key_len;
    tls_config     *tls_cfg;

    /* config */
    u64             keep_alive_ms;
    u32             max_connections;
    u64             max_body_size;
    u64             read_timeout_ms;
    u64             write_timeout_ms;

    /* control */
    volatile int    shutdown_flag;

    /* async listener (spawned by http_server_listen_async) */
    thread_handle   listener_thread;
    int             listener_running;
    char           *async_addr;
    u16             async_port;
};

/* ================================================================
 *  Method name table
 * ================================================================ */

static const char *method_names[] = {
    "GET", "POST", "PUT", "DELETE", "HEAD", "PATCH", "OPTIONS"
};
#define METHOD_COUNT (sizeof(method_names) / sizeof(method_names[0]))

static int method_from_str(const char *s, size_t len) {
    u32 i;
    for (i = 0; i < METHOD_COUNT; ++i) {
        if (strlen(method_names[i]) == len && memcmp(s, method_names[i], len) == 0)
            return (int)i;
    }
    return -1;
}

/* ================================================================
 *  Status reason phrase
 * ================================================================ */

static const char *reason_for(u16 status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}

/* ================================================================
 *  MIME type from extension
 * ================================================================ */

static const char *mime_from_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (ci_eq(dot, ".html") || ci_eq(dot, ".htm")) return "text/html";
    if (ci_eq(dot, ".css"))  return "text/css";
    if (ci_eq(dot, ".js"))   return "application/javascript";
    if (ci_eq(dot, ".json")) return "application/json";
    if (ci_eq(dot, ".png"))  return "image/png";
    if (ci_eq(dot, ".jpg") || ci_eq(dot, ".jpeg")) return "image/jpeg";
    if (ci_eq(dot, ".gif"))  return "image/gif";
    if (ci_eq(dot, ".svg"))  return "image/svg+xml";
    if (ci_eq(dot, ".ico"))  return "image/x-icon";
    if (ci_eq(dot, ".txt"))  return "text/plain";
    if (ci_eq(dot, ".xml"))  return "application/xml";
    if (ci_eq(dot, ".wasm")) return "application/wasm";
    if (ci_eq(dot, ".pdf"))  return "application/pdf";
    return "application/octet-stream";
}

/* ================================================================
 *  Route matching
 * ================================================================ */

/*
 * Match a request path against a route pattern.
 *
 * Pattern syntax:
 *   /literal            — exact match
 *   /:name              — single-segment capture into param `name`;
 *                         requires non-empty value (can't match "/")
 *   /*name              — splat capture: must be the final element of
 *                         the pattern; captures the rest of the path
 *                         including embedded slashes; requires at least
 *                         one non-slash byte
 *
 * Splat is useful for variable-depth paths like
 * `/artifact/:group_path_splat/:artifact/:version/:filename` that can't
 * be expressed with single-segment params because `group_path` varies
 * in depth. Usage:
 *   `/artifact/*tail` matches `/artifact/com/example/lib/1.0/jar.jar`
 *   with tail = "com/example/lib/1.0/jar.jar".
 *
 * Returns 1 on match, 0 otherwise. Populates params/param_count on match.
 */
static int route_match(const http_route *route, const char *path,
                       route_param *params, u32 *param_count) {
    const char *pp = route->pattern;
    const char *rp = path;
    *param_count = 0;

    /* static route: prefix match */
    if (route->is_static) {
        size_t prefix_len = strlen(pp);
        while (prefix_len > 1 && pp[prefix_len - 1] == '/')
            --prefix_len;
        if (strlen(rp) < prefix_len) return 0;
        if (memcmp(rp, pp, prefix_len) != 0) return 0;
        if (rp[prefix_len] != '\0' && rp[prefix_len] != '/') return 0;
        return 1;
    }

    /* segment-by-segment match */
    while (*pp && *rp) {
        if (*pp == '/' && *rp == '/') {
            ++pp; ++rp;
            continue;
        }

        if (*pp == ':') {
            const char *pname_start = pp + 1;
            const char *pname_end = pname_start;
            const char *val_start = rp;
            const char *val_end;

            while (*pname_end && *pname_end != '/') ++pname_end;
            while (*rp && *rp != '/') ++rp;
            val_end = rp;

            if (val_start == val_end) return 0;
            if (*param_count >= MAX_ROUTE_PARAMS) return 0;

            params[*param_count].name = s_ndup(pname_start, (size_t)(pname_end - pname_start));
            params[*param_count].value = s_ndup(val_start, (size_t)(val_end - val_start));
            ++(*param_count);

            pp = pname_end;
            continue;
        }

        if (*pp == '*') {
            const char *pname_start = pp + 1;
            const char *pname_end = pname_start;
            const char *val_start = rp;
            const char *val_end;

            while (*pname_end && *pname_end != '/') ++pname_end;
            /* Splat must be the last element — no characters after it,
             * optionally a trailing slash the pattern-author wrote but
             * that we treat the same. */
            {
                const char *after = pname_end;
                while (*after == '/') ++after;
                if (*after != '\0') return 0;
            }
            /* Capture everything remaining in the path. */
            while (*rp) ++rp;
            val_end = rp;

            if (val_start == val_end) return 0;  /* splat requires ≥ 1 byte */
            if (*param_count >= MAX_ROUTE_PARAMS) return 0;

            params[*param_count].name = s_ndup(pname_start, (size_t)(pname_end - pname_start));
            params[*param_count].value = s_ndup(val_start, (size_t)(val_end - val_start));
            ++(*param_count);
            return 1;
        }

        /* literal match */
        if (*pp != *rp) {
            return 0;
        }
        ++pp; ++rp;
    }

    /* both must be exhausted (allow trailing slash diff) */
    while (*pp == '/') ++pp;
    while (*rp == '/') ++rp;
    return (*pp == '\0' && *rp == '\0') ? 1 : 0;
}

static void free_params(route_param *params, u32 count) {
    u32 i;
    for (i = 0; i < count; ++i) {
        free(params[i].name);
        free(params[i].value);
    }
}

/* ================================================================
 *  Request parsing (inline, from raw bytes)
 * ================================================================ */

static const u8 *find_crlf(const u8 *start, const u8 *end) {
    const u8 *p;
    for (p = start; p + 1 < end; ++p) {
        if (p[0] == '\r' && p[1] == '\n') return p;
    }
    return NULL;
}

static const u8 *find_header_end(const u8 *data, u64 len) {
    const u8 *p;
    for (p = data; p + 3 < data + len; ++p) {
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n')
            return p;
    }
    return NULL;
}

/*
 * Parse raw HTTP request bytes into an http_ctx.
 * Returns 0 on success, non-zero on parse failure.
 *   1=no data, 2=incomplete headers, 3=malformed request line,
 *   4=unknown method, 5=malformed header, 6=alloc failure
 */
static unsigned long parse_request(http_ctx *ctx, const u8 *data, u64 len) {
    const u8 *hdr_end, *line_end, *line_start, *body_start;
    const u8 *p, *method_end, *url_start, *url_end;
    const char *qmark;
    int m;
    u64 body_offset;

    if (!data || len == 0) return 1;

    hdr_end = find_header_end(data, len);
    if (!hdr_end) return 2;

    body_start = hdr_end + 4;
    body_offset = (u64)(body_start - data);

    /* request line: "METHOD URL HTTP/1.x\r\n" */
    line_end = find_crlf(data, hdr_end + 2);
    if (!line_end) return 3;

    p = data;
    method_end = p;
    while (method_end < line_end && *method_end != ' ') ++method_end;
    if (method_end == p || method_end >= line_end) return 3;

    m = method_from_str((const char *)p, (size_t)(method_end - p));
    if (m < 0) return 4;
    ctx->method = m;

    url_start = method_end + 1;
    url_end = url_start;
    while (url_end < line_end && *url_end != ' ') ++url_end;
    if (url_start == url_end) return 3;

    {
        char *full_url = s_ndup((const char *)url_start, (size_t)(url_end - url_start));
        if (!full_url) return 6;

        qmark = strchr(full_url, '?');
        if (qmark) {
            ctx->path = s_ndup(full_url, (size_t)(qmark - full_url));
            ctx->query_string = s_dup(qmark + 1);
        } else {
            ctx->path = s_dup(full_url);
            ctx->query_string = NULL;
        }
        free(full_url);
        if (!ctx->path) return 6;
    }

    /* parse headers */
    http_headers_create(&ctx->req_headers);
    line_start = line_end + 2;
    while (line_start < hdr_end) {
        const u8 *colon;
        char *hname, *hvalue;
        size_t name_len, value_len;

        line_end = find_crlf(line_start, hdr_end + 2);
        if (!line_end) break;

        colon = line_start;
        while (colon < line_end && *colon != ':') ++colon;
        if (colon >= line_end) { line_start = line_end + 2; continue; }

        name_len = (size_t)(colon - line_start);
        hname = s_ndup((const char *)line_start, name_len);
        if (!hname) return 6;

        colon++;
        while (colon < line_end && *colon == ' ') ++colon;
        value_len = (size_t)(line_end - colon);
        hvalue = s_ndup((const char *)colon, value_len);
        if (!hvalue) { free(hname); return 6; }

        http_headers_set(&ctx->req_headers, hname, hvalue);
        free(hname);
        free(hvalue);

        line_start = line_end + 2;
    }

    /* body */
    {
        const char *cl_val = NULL;
        http_headers_get(&cl_val, &ctx->req_headers, "Content-Length");
        if (cl_val) {
            u64 content_len = (u64)strtoull(cl_val, NULL, 10);
            u64 avail = (len > body_offset) ? (len - body_offset) : 0;
            if (avail < content_len) content_len = avail;
            if (content_len > 0) {
                ctx->body = (u8 *)malloc((size_t)content_len);
                if (!ctx->body) return 6;
                memcpy(ctx->body, body_start, (size_t)content_len);
                ctx->body_len = content_len;
            }
        }
    }

    return 0;
}

/* ================================================================
 *  Response serialization
 * ================================================================ */

typedef struct {
    u8 *data;
    u64 len;
    u64 cap;
} resp_buf;

static unsigned long rb_init(resp_buf *rb) {
    rb->data = (u8 *)malloc(INITIAL_RESP_BUF);
    if (!rb->data) return 1;
    rb->len = 0;
    rb->cap = INITIAL_RESP_BUF;
    return 0;
}

static unsigned long rb_append(resp_buf *rb, const u8 *src, u64 n) {
    if (rb->len + n > rb->cap) {
        u64 new_cap = rb->cap * 2;
        u8 *tmp;
        while (new_cap < rb->len + n) new_cap *= 2;
        tmp = (u8 *)realloc(rb->data, (size_t)new_cap);
        if (!tmp) return 1;
        rb->data = tmp;
        rb->cap = new_cap;
    }
    memcpy(rb->data + rb->len, src, (size_t)n);
    rb->len += n;
    return 0;
}

static unsigned long rb_append_str(resp_buf *rb, const char *s) {
    return rb_append(rb, (const u8 *)s, (u64)strlen(s));
}

static unsigned long serialize_response(resp_buf *rb, u16 status,
                                         const http_headers *hdrs,
                                         const u8 *body, u64 body_len) {
    char line[256];
    u64 i;

    if (rb_init(rb) != 0) return 1;

    /* status line */
    snprintf(line, sizeof(line), "HTTP/1.1 %u %s\r\n", (unsigned)status, reason_for(status));
    if (rb_append_str(rb, line) != 0) return 2;

    /* headers */
    for (i = 0; i < hdrs->count; ++i) {
        if (rb_append_str(rb, hdrs->items[i].name) != 0) return 2;
        if (rb_append_str(rb, ": ") != 0) return 2;
        if (rb_append_str(rb, hdrs->items[i].value) != 0) return 2;
        if (rb_append_str(rb, "\r\n") != 0) return 2;
    }

    /* content-length if not already present */
    {
        const char *cl = NULL;
        http_headers_get(&cl, hdrs, "Content-Length");
        if (!cl) {
            snprintf(line, sizeof(line), "Content-Length: %llu\r\n",
                     (unsigned long long)body_len);
            if (rb_append_str(rb, line) != 0) return 2;
        }
    }

    if (rb_append_str(rb, "\r\n") != 0) return 2;

    /* body */
    if (body && body_len > 0) {
        if (rb_append(rb, body, body_len) != 0) return 2;
    }

    return 0;
}

/* ================================================================
 *  ctx lifecycle helpers
 * ================================================================ */

static http_ctx *ctx_create(http_server *server, tcp_conn *conn) {
    http_ctx *ctx = (http_ctx *)calloc(1, sizeof(http_ctx));
    if (!ctx) return NULL;
    ctx->server = server;
    ctx->conn = conn;
    ctx->tls = NULL;
    ctx->resp_status = 200;
    http_headers_create(&ctx->resp_headers);
    return ctx;
}

static http_ctx *ctx_create_tls(http_server *server, tcp_conn *conn,
                                 tls_conn *tls) {
    http_ctx *ctx = ctx_create(server, conn);
    if (!ctx) return NULL;
    ctx->tls = tls;
    ctx->is_tls = (tls != NULL) ? 1 : 0;
    return ctx;
}

/* Transport-level read/write that branch on whether the connection
 * has been upgraded to TLS. Used by the worker loop and by
 * http_ctx_respond's write path. */
static unsigned long srv_read(u64 *out_n, tcp_conn *tcp, tls_conn *tls,
                               u8 *buf, u64 len) {
    if (tls) return tls_conn_read(out_n, tls, buf, len);
    return tcp_conn_read(out_n, tcp, buf, len);
}

static unsigned long srv_write_all(u64 *out_written, tcp_conn *tcp,
                                    tls_conn *tls,
                                    const u8 *data, u64 len) {
    if (tls) return tls_conn_write_all(out_written, tls, data, len);
    return tcp_conn_write_all(out_written, tcp, data, len);
}

static void ctx_destroy(http_ctx *ctx) {
    if (!ctx) return;
    free(ctx->path);
    free(ctx->query_string);
    http_headers_destroy(&ctx->req_headers);
    http_headers_destroy(&ctx->resp_headers);
    free(ctx->body);
    free(ctx->resp_body);
    free_params(ctx->params, ctx->param_count);
    free(ctx);
}

/* ================================================================
 *  Static file handler
 * ================================================================ */

static unsigned long static_file_handler(http_ctx *ctx) {
    return http_ctx_respond_file(ctx, NULL);
}

/* ================================================================
 *  Middleware chain runner
 * ================================================================ */

typedef struct {
    http_server       *server;
    http_ctx          *ctx;
    http_handler_fn    handler;
    u32                mw_index;
} mw_chain;

static unsigned long mw_chain_next(http_ctx *ctx);

/*
 * We store a per-ctx chain state in thread-local since the middleware
 * signature only passes ctx + next.  For simplicity we use a static
 * thread-local pointer.
 */
#ifdef _WIN32
static __declspec(thread) mw_chain *tl_chain = NULL;
#else
static __thread mw_chain *tl_chain = NULL;
#endif

static unsigned long mw_chain_next(http_ctx *ctx) {
    mw_chain *chain = tl_chain;
    if (!chain) return 1;

    if (chain->mw_index >= chain->server->mw_count) {
        /* end of middleware chain — call the route handler */
        return chain->handler(ctx);
    } else {
        http_middleware_fn mw = chain->server->middlewares[chain->mw_index];
        chain->mw_index++;
        return mw(ctx, mw_chain_next);
    }
}

static unsigned long run_handler_with_middleware(http_server *server,
                                                  http_ctx *ctx,
                                                  http_handler_fn handler) {
    mw_chain chain;
    chain.server = server;
    chain.ctx = ctx;
    chain.handler = handler;
    chain.mw_index = 0;

    tl_chain = &chain;
    {
        unsigned long rc = mw_chain_next(ctx);
        tl_chain = NULL;
        return rc;
    }
}

/* ================================================================
 *  Connection worker (runs in threadpool)
 * ================================================================ */

typedef struct {
    http_server *server;
    tcp_conn     conn;
    tls_conn    *tls;  /* NULL for plain HTTP; live TLS conn for HTTPS */
} conn_task_arg;

static unsigned long conn_worker(void *arg, void **result) {
    conn_task_arg *cta = (conn_task_arg *)arg;
    http_server *server = cta->server;
    tcp_conn conn = cta->conn;
    tls_conn *tls = cta->tls;
    u8 *read_buf = NULL;
    u64 total_read = 0;
    u64 buf_cap = READ_BUF_SIZE;
    unsigned long rc;
    int keep_alive = 1;

    (void)result;

    read_buf = (u8 *)malloc((size_t)buf_cap);
    if (!read_buf) {
        if (tls) tls_conn_destroy(tls);
        tcp_conn_destroy(&conn);
        free(cta);
        return 1;
    }

    /* keep-alive loop */
    while (keep_alive && !server->shutdown_flag) {
        u64 bytes_read = 0;
        http_ctx *ctx = NULL;
        u32 i;
        int matched = 0;
        http_handler_fn handler = NULL;
        const char *conn_hdr = NULL;
        resp_buf rb;

        total_read = 0;

        /* read until we have complete headers */
        for (;;) {
            if (total_read + READ_BUF_SIZE > buf_cap) {
                u64 new_cap = buf_cap * 2;
                u8 *tmp;
                if (new_cap > server->max_body_size + 65536) {
                    /* too large */
                    goto close_conn;
                }
                tmp = (u8 *)realloc(read_buf, (size_t)new_cap);
                if (!tmp) goto close_conn;
                read_buf = tmp;
                buf_cap = new_cap;
            }

            rc = srv_read(&bytes_read, &conn, tls, read_buf + total_read, READ_BUF_SIZE);
            if (rc != 0 || bytes_read == 0) goto close_conn;
            total_read += bytes_read;

            if (find_header_end(read_buf, total_read)) break;
        }

        /* if there's a Content-Length, read remaining body bytes */
        {
            const u8 *hdr_end = find_header_end(read_buf, total_read);
            if (hdr_end) {
                u64 header_size = (u64)(hdr_end + 4 - read_buf);
                /* scan for Content-Length in raw headers */
                const u8 *scan = read_buf;
                u64 content_len = 0;
                int found_cl = 0;

                while (scan < hdr_end) {
                    const u8 *le = find_crlf(scan, hdr_end + 2);
                    if (!le) break;
                    if ((size_t)(le - scan) > 16 &&
                        (memcmp(scan, "Content-Length: ", 16) == 0 ||
                         memcmp(scan, "content-length: ", 16) == 0)) {
                        content_len = (u64)strtoull((const char *)(scan + 16), NULL, 10);
                        found_cl = 1;
                        break;
                    }
                    /* also check case-insensitive */
                    if (!found_cl && (size_t)(le - scan) > 15) {
                        char tmp_name[20];
                        size_t nlen = 0;
                        const u8 *colon = scan;
                        while (colon < le && *colon != ':') { ++colon; ++nlen; }
                        if (nlen == 14 && colon < le) {
                            memcpy(tmp_name, scan, 14);
                            tmp_name[14] = '\0';
                            if (ci_eq(tmp_name, "content-length")) {
                                const u8 *vp = colon + 1;
                                while (vp < le && *vp == ' ') ++vp;
                                content_len = (u64)strtoull((const char *)vp, NULL, 10);
                                found_cl = 1;
                                break;
                            }
                        }
                    }
                    scan = le + 2;
                }

                if (found_cl && content_len > server->max_body_size) {
                    /* 413 Payload Too Large */
                    ctx = ctx_create_tls(server, &conn, tls);
                    if (ctx) {
                        http_ctx_respond(ctx, 413, (const u8 *)"Payload Too Large", 17);
                        ctx_destroy(ctx);
                    }
                    goto close_conn;
                }

                if (found_cl) {
                    u64 need = header_size + content_len;
                    while (total_read < need) {
                        if (need > buf_cap) {
                            u64 new_cap = buf_cap * 2;
                            u8 *tmp;
                            while (new_cap < need) new_cap *= 2;
                            tmp = (u8 *)realloc(read_buf, (size_t)new_cap);
                            if (!tmp) goto close_conn;
                            read_buf = tmp;
                            buf_cap = new_cap;
                        }
                        rc = srv_read(&bytes_read, &conn, tls,
                                       read_buf + total_read,
                                       (u64)(need - total_read));
                        if (rc != 0 || bytes_read == 0) goto close_conn;
                        total_read += bytes_read;
                    }
                }
                /* Downstream paths allocate ctx via ctx_create_tls to
                 * plumb `tls` into response write. */
                (void)tls;
            }
        }

        /* parse request */
        ctx = ctx_create_tls(server, &conn, tls);
        if (!ctx) goto close_conn;

        rc = parse_request(ctx, read_buf, total_read);
        if (rc != 0) {
            http_ctx_respond(ctx, 400, (const u8 *)"Bad Request", 11);
            ctx_destroy(ctx);
            goto close_conn;
        }

        /* find matching route */
        for (i = 0; i < server->route_count; ++i) {
            http_route *rt = &server->routes[i];

            /* method check (static routes accept GET and HEAD) */
            if (rt->is_static) {
                if (ctx->method != HTTP_GET && ctx->method != HTTP_HEAD)
                    continue;
            } else {
                if (rt->method != ctx->method) continue;
            }

            if (route_match(rt, ctx->path, ctx->params, &ctx->param_count)) {
                matched = 1;
                handler = rt->handler;

                /* for static routes, build the file path */
                if (rt->is_static) {
                    size_t prefix_len = strlen(rt->pattern);
                    const char *sub;
                    char file_path[PATH_BUF_SIZE];

                    while (prefix_len > 1 && rt->pattern[prefix_len - 1] == '/')
                        --prefix_len;

                    sub = ctx->path + prefix_len;
                    if (*sub == '/') ++sub;
                    if (*sub == '\0') sub = "index.html";

                    /* reject path traversal */
                    if (strstr(sub, "..")) {
                        http_ctx_respond(ctx, 403, (const u8 *)"Forbidden", 9);
                        ctx_destroy(ctx);
                        matched = 0;
                        keep_alive = 0;
                        break;
                    }

                    snprintf(file_path, sizeof(file_path), "%s/%s", rt->static_dir, sub);
                    http_ctx_respond_file(ctx, file_path);
                    matched = 2; /* special: already responded */
                }
                break;
            }
        }

        if (matched == 0) {
            http_ctx_respond(ctx, 404, (const u8 *)"Not Found", 9);
        } else if (matched == 1 && handler) {
            run_handler_with_middleware(server, ctx, handler);

            /* if handler didn't send a response, send what's accumulated */
            if (!ctx->resp_sent) {
                if (!ctx->resp_body) {
                    http_ctx_respond(ctx, ctx->resp_status, NULL, 0);
                } else {
                    http_ctx_respond(ctx, ctx->resp_status,
                                     ctx->resp_body, ctx->resp_body_len);
                }
            }
        }
        /* matched == 2: already responded by static handler */

        /* check keep-alive */
        http_headers_get(&conn_hdr, &ctx->req_headers, "Connection");
        if (conn_hdr && ci_eq(conn_hdr, "close")) {
            keep_alive = 0;
        }
        if (!server->keep_alive_ms) {
            keep_alive = 0;
        }

        ctx_destroy(ctx);
    }

close_conn:
    free(read_buf);
    if (tls) tls_conn_destroy(tls);
    tcp_conn_shutdown(&conn, TCP_SHUTDOWN_BOTH);
    tcp_conn_destroy(&conn);
    free(cta);
    return 0;
}

/* ================================================================
 *  Public API: Server lifecycle
 * ================================================================ */

unsigned long http_server_create(http_server **out) {
    http_server *s;
    unsigned long rc;

    if (!out) return 1;

    s = (http_server *)calloc(1, sizeof(http_server));
    if (!s) return 2;

    s->num_threads = DEFAULT_THREADS;
    s->keep_alive_ms = DEFAULT_KEEP_ALIVE_MS;
    s->max_connections = DEFAULT_MAX_CONNECTIONS;
    s->max_body_size = DEFAULT_MAX_BODY_SIZE;
    s->read_timeout_ms = DEFAULT_READ_TIMEOUT_MS;
    s->write_timeout_ms = DEFAULT_WRITE_TIMEOUT_MS;

    rc = threadpool_create(&s->pool, s->num_threads);
    if (rc != 0) {
        free(s);
        return 3;
    }

    *out = s;
    return 0;
}

unsigned long http_server_route(http_server *s, int method,
                                 const char *pattern,
                                 http_handler_fn handler) {
    http_route *rt;

    if (!s) return 1;
    if (!pattern) return 2;
    if (!handler) return 3;
    if (method < 0 || method >= (int)METHOD_COUNT) return 4;
    if (s->route_count >= MAX_ROUTES) return 5;

    rt = &s->routes[s->route_count];
    rt->method = method;
    rt->pattern = s_dup(pattern);
    if (!rt->pattern) return 6;
    rt->handler = handler;
    rt->is_static = 0;
    rt->static_dir = NULL;
    s->route_count++;

    return 0;
}

unsigned long http_server_route_static(http_server *s,
                                        const char *prefix,
                                        const char *dir_path) {
    http_route *rt;

    if (!s) return 1;
    if (!prefix) return 2;
    if (!dir_path) return 3;
    if (s->route_count >= MAX_ROUTES) return 4;

    rt = &s->routes[s->route_count];
    rt->method = HTTP_GET;
    rt->pattern = s_dup(prefix);
    if (!rt->pattern) return 5;
    rt->handler = static_file_handler;
    rt->is_static = 1;
    rt->static_dir = s_dup(dir_path);
    if (!rt->static_dir) { free(rt->pattern); return 5; }
    s->route_count++;

    return 0;
}

unsigned long http_server_middleware(http_server *s, http_middleware_fn mw) {
    if (!s) return 1;
    if (!mw) return 2;
    if (s->mw_count >= MAX_MIDDLEWARES) return 3;

    s->middlewares[s->mw_count] = mw;
    s->mw_count++;
    return 0;
}

unsigned long http_server_set_tls(http_server *s,
                                   const u8 *cert, u64 cert_len,
                                   const u8 *key, u64 key_len) {
    if (!s) return 1;
    if (!cert || cert_len == 0) return 2;
    if (!key || key_len == 0) return 3;

    free(s->tls_cert);
    free(s->tls_key);

    s->tls_cert = (u8 *)malloc((size_t)cert_len);
    if (!s->tls_cert) return 4;
    memcpy(s->tls_cert, cert, (size_t)cert_len);
    s->tls_cert_len = cert_len;

    s->tls_key = (u8 *)malloc((size_t)key_len);
    if (!s->tls_key) { free(s->tls_cert); s->tls_cert = NULL; return 4; }
    memcpy(s->tls_key, key, (size_t)key_len);
    s->tls_key_len = key_len;

    return 0;
}

unsigned long http_server_set_keep_alive(http_server *s, u64 timeout_ms) {
    if (!s) return 1;
    s->keep_alive_ms = timeout_ms;
    return 0;
}

unsigned long http_server_set_max_connections(http_server *s, u32 max) {
    if (!s) return 1;
    if (max == 0) return 2;
    s->max_connections = max;
    return 0;
}

unsigned long http_server_set_max_body_size(http_server *s, u64 max) {
    if (!s) return 1;
    s->max_body_size = max;
    return 0;
}

unsigned long http_server_set_read_timeout(http_server *s, u64 ms) {
    if (!s) return 1;
    s->read_timeout_ms = ms;
    return 0;
}

unsigned long http_server_set_write_timeout(http_server *s, u64 ms) {
    if (!s) return 1;
    s->write_timeout_ms = ms;
    return 0;
}

unsigned long http_server_listen(http_server *s, const char *addr, u16 port) {
    net_sock_addr sa;
    unsigned long rc;

    if (!s) return 1;
    if (!addr) return 2;
    if (s->listening) return 3;

    rc = addr_sockaddr_create(&sa, addr, port);
    if (rc != 0) return 4;

    rc = tcp_listener_create(&s->listener, &sa, (int)s->max_connections);
    if (rc != 0) return 5;
    s->listening = 1;

    /* Build the tls_config once if TLS material is configured. */
    if (s->tls_cert && s->tls_key && !s->tls_cfg) {
        rc = tls_config_create(&s->tls_cfg);
        if (rc != 0) return 6;
        rc = tls_config_set_cert(s->tls_cfg, s->tls_cert, s->tls_cert_len);
        if (rc != 0) return 7;
        rc = tls_config_set_key(s->tls_cfg, s->tls_key, s->tls_key_len);
        if (rc != 0) return 8;
    }

    /* accept loop */
    while (!s->shutdown_flag) {
        tcp_conn accepted;
        conn_task_arg *cta;
        tls_conn *tls = NULL;

        rc = tcp_listener_accept(&accepted, &s->listener);
        if (rc != 0) {
            if (s->shutdown_flag) break;
            continue; /* transient error, keep going */
        }

        tcp_conn_set_nodelay(&accepted, 1);
        if (s->keep_alive_ms > 0) {
            tcp_conn_set_keepalive(&accepted, 1);
        }

        /* Allocate cta before handshake so tls_conn can hold a stable
         * pointer to cta->conn instead of a stack address. (tls_conn
         * stores tcp_conn* by pointer, not by value — passing &accepted
         * here would leave tls->tcp dangling once this frame unwinds.) */
        cta = (conn_task_arg *)malloc(sizeof(conn_task_arg));
        if (!cta) {
            tcp_conn_destroy(&accepted);
            continue;
        }
        cta->server = s;
        cta->conn = accepted;
        cta->tls = NULL;

        /* Handshake upfront — if this fails the peer isn't talking TLS
         * and there's no point running the HTTP worker. Done on the
         * accept thread; for very high connection rates this could be
         * moved into the pool, but that's a later optimisation. */
        if (s->tls_cfg) {
            rc = tls_conn_create_server(&tls, &cta->conn, s->tls_cfg);
            if (rc != 0) {
                tcp_conn_destroy(&cta->conn);
                free(cta);
                continue;
            }
            cta->tls = tls;
        }

        /* Fire-and-forget via a detached submit: no future allocated,
         * nothing to destroy early. Previous code used threadpool_submit
         * + immediate future_destroy, which raced with the worker's
         * future_mark_done — use-after-free that crashed the process on
         * first request. */
        rc = threadpool_submit_detached(s->pool, conn_worker, cta);
        if (rc != 0) {
            if (cta->tls) tls_conn_destroy(cta->tls);
            tcp_conn_destroy(&cta->conn);
            free(cta);
            continue;
        }
    }

    return 0;
}

unsigned long http_server_shutdown(http_server *s) {
    if (!s) return 1;
    s->shutdown_flag = 1;

    /* Destroy the listener socket first. This unblocks an accept() that
     * might be parked in the async listener thread so it can observe
     * shutdown_flag and return. */
    if (s->listening) {
        tcp_listener_destroy(&s->listener);
        s->listening = 0;
    }

    /* If we spawned a background listener via http_server_listen_async,
     * wait for it to drain. */
    if (s->listener_running) {
        unsigned long ret = 0;
        thread_join(&ret, &s->listener_thread);
        s->listener_running = 0;
    }

    return 0;
}

unsigned long http_server_destroy(http_server *s) {
    u32 i;

    if (!s) return 1;

    /* ensure shutdown (also joins the async listener if one is live) */
    if (!s->shutdown_flag) {
        http_server_shutdown(s);
    } else if (s->listener_running) {
        /* shutdown_flag was set another way — still need to join */
        unsigned long ret = 0;
        if (s->listening) {
            tcp_listener_destroy(&s->listener);
            s->listening = 0;
        }
        thread_join(&ret, &s->listener_thread);
        s->listener_running = 0;
    }

    /* shutdown threadpool */
    if (s->pool) {
        threadpool_shutdown(s->pool);
        threadpool_destroy(s->pool);
        s->pool = NULL;
    }

    /* free routes */
    for (i = 0; i < s->route_count; ++i) {
        free(s->routes[i].pattern);
        free(s->routes[i].static_dir);
    }

    /* free TLS material */
    if (s->tls_cfg) tls_config_destroy(s->tls_cfg);
    free(s->tls_cert);
    free(s->tls_key);

    free(s->async_addr);

    free(s);
    return 0;
}

/* ================================================================
 *  Public API: Request context accessors
 * ================================================================ */

unsigned long http_ctx_method(int *out, http_ctx *ctx) {
    if (!out) return 1;
    if (!ctx) return 2;
    *out = ctx->method;
    return 0;
}

unsigned long http_ctx_path(const char **out, http_ctx *ctx) {
    if (!out) return 1;
    if (!ctx) return 2;
    *out = ctx->path;
    return 0;
}

unsigned long http_ctx_param(const char **out, http_ctx *ctx, const char *name) {
    u32 i;
    if (!out) return 1;
    if (!ctx) return 2;
    if (!name) return 3;

    for (i = 0; i < ctx->param_count; ++i) {
        if (strcmp(ctx->params[i].name, name) == 0) {
            *out = ctx->params[i].value;
            return 0;
        }
    }
    return 4; /* param not found */
}

unsigned long http_ctx_query(const char **out, http_ctx *ctx, const char *name) {
    http_query q;
    u64 i;
    unsigned long rc;

    if (!out) return 1;
    if (!ctx) return 2;
    if (!name) return 3;
    if (!ctx->query_string) return 4;

    memset(&q, 0, sizeof(q));
    rc = http_query_parse(&q, ctx->query_string, (u64)strlen(ctx->query_string));
    if (rc != 0) return 5;

    for (i = 0; i < q.count; ++i) {
        if (strcmp(q.params[i].key, name) == 0) {
            /*
             * Return a pointer into the query_string (re-scan) since
             * the parsed query will be freed.  For safety we search
             * the original query string directly.
             */
            const char *qs = ctx->query_string;
            size_t nlen = strlen(name);

            http_query_free(&q);

            /* simple scan for key=value in query string */
            while (*qs) {
                if (memcmp(qs, name, nlen) == 0 && qs[nlen] == '=') {
                    *out = qs + nlen + 1;
                    return 0;
                }
                while (*qs && *qs != '&') ++qs;
                if (*qs == '&') ++qs;
            }
            return 4;
        }
    }

    http_query_free(&q);
    return 4; /* not found */
}

unsigned long http_ctx_header(const char **out, http_ctx *ctx, const char *name) {
    if (!out) return 1;
    if (!ctx) return 2;
    if (!name) return 3;

    {
        unsigned long rc = http_headers_get(out, &ctx->req_headers, name);
        if (rc != 0) return 4;
    }
    return 0;
}

unsigned long http_ctx_body(const u8 **out, u64 *out_len, http_ctx *ctx) {
    if (!out) return 1;
    if (!out_len) return 2;
    if (!ctx) return 3;

    *out = ctx->body;
    *out_len = ctx->body_len;
    return 0;
}

unsigned long http_ctx_body_json(const char **out, u64 *out_len, http_ctx *ctx) {
    if (!out) return 1;
    if (!out_len) return 2;
    if (!ctx) return 3;

    /* just return the body as-is; caller validates JSON */
    *out = (const char *)ctx->body;
    *out_len = ctx->body_len;
    return 0;
}

/* ================================================================
 *  Public API: Response
 * ================================================================ */

unsigned long http_ctx_set_header(http_ctx *ctx, const char *name, const char *value) {
    if (!ctx) return 1;
    if (!name) return 2;
    if (!value) return 3;
    return http_headers_set(&ctx->resp_headers, name, value);
}

unsigned long http_ctx_set_status(http_ctx *ctx, u16 status) {
    if (!ctx) return 1;
    ctx->resp_status = status;
    return 0;
}

unsigned long http_ctx_set_cookie(http_ctx *ctx, const char *name,
                                   const char *value, const char *path,
                                   u64 max_age) {
    char buf[1024];

    if (!ctx) return 1;
    if (!name) return 2;
    if (!value) return 3;

    if (path && max_age > 0) {
        snprintf(buf, sizeof(buf), "%s=%s; Path=%s; Max-Age=%llu",
                 name, value, path, (unsigned long long)max_age);
    } else if (path) {
        snprintf(buf, sizeof(buf), "%s=%s; Path=%s", name, value, path);
    } else if (max_age > 0) {
        snprintf(buf, sizeof(buf), "%s=%s; Max-Age=%llu",
                 name, value, (unsigned long long)max_age);
    } else {
        snprintf(buf, sizeof(buf), "%s=%s", name, value);
    }

    return http_headers_set(&ctx->resp_headers, "Set-Cookie", buf);
}

unsigned long http_ctx_respond(http_ctx *ctx, u16 status,
                                const u8 *body, u64 body_len) {
    resp_buf rb;
    unsigned long rc;
    u64 written = 0;

    if (!ctx) return 1;
    if (ctx->resp_sent) return 2;
    if (ctx->stream_started) return 5;  /* already committed to chunked */

    ctx->resp_status = status;

    /* ensure Content-Type if not set and body present */
    if (body && body_len > 0) {
        const char *ct = NULL;
        http_headers_get(&ct, &ctx->resp_headers, "Content-Type");
        if (!ct) {
            http_headers_set(&ctx->resp_headers, "Content-Type", "text/plain");
        }
    }

    rc = serialize_response(&rb, status, &ctx->resp_headers, body, body_len);
    if (rc != 0) return 3;

    rc = srv_write_all(&written, ctx->conn, ctx->tls, rb.data, rb.len);
    free(rb.data);
    if (rc != 0) return 4;

    ctx->resp_sent = 1;
    return 0;
}

unsigned long http_ctx_respond_json(http_ctx *ctx, u16 status, const char *json) {
    u64 len;
    if (!ctx) return 1;
    if (!json) return 2;

    http_headers_set(&ctx->resp_headers, "Content-Type", "application/json");
    len = (u64)strlen(json);
    return http_ctx_respond(ctx, status, (const u8 *)json, len);
}

unsigned long http_ctx_respond_file(http_ctx *ctx, const char *file_path) {
    FILE *fp;
    u8 *buf = NULL;
    long fsize;
    const char *mime;
    unsigned long rc;

    if (!ctx) return 1;
    if (!file_path) return 2;

    fp = fopen(file_path, "rb");
    if (!fp) {
        return http_ctx_respond(ctx, 404, (const u8 *)"Not Found", 9);
    }

    fseek(fp, 0, SEEK_END);
    fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize < 0) { fclose(fp); return 3; }
    if (fsize == 0) {
        fclose(fp);
        mime = mime_from_ext(file_path);
        http_headers_set(&ctx->resp_headers, "Content-Type", mime);
        return http_ctx_respond(ctx, 200, NULL, 0);
    }

    buf = (u8 *)malloc((size_t)fsize);
    if (!buf) { fclose(fp); return 4; }

    if (fread(buf, 1, (size_t)fsize, fp) != (size_t)fsize) {
        fclose(fp);
        free(buf);
        return 5;
    }
    fclose(fp);

    mime = mime_from_ext(file_path);
    http_headers_set(&ctx->resp_headers, "Content-Type", mime);

    rc = http_ctx_respond(ctx, 200, buf, (u64)fsize);
    free(buf);
    return rc;
}

unsigned long http_ctx_remote_addr(const char **out, http_ctx *ctx) {
    if (!out) return 1;
    if (!ctx) return 2;
    *out = ctx->remote_addr;
    return 0;
}

unsigned long http_ctx_is_tls(int *out, http_ctx *ctx) {
    if (!out) return 1;
    if (!ctx) return 2;
    *out = ctx->is_tls;
    return 0;
}

/* ================================================================
 *  Gap-fill stubs — Section 36
 * ================================================================ */

static unsigned long async_listen_trampoline(void *arg) {
    http_server *s = (http_server *)arg;
    return http_server_listen(s, s->async_addr, s->async_port);
}

unsigned long http_server_listen_async(http_server *s, const char *addr, u16 port) {
    unsigned long rc;

    if (!s) return 1;
    if (!addr) return 2;
    if (s->listener_running) return 3;
    if (s->listening) return 4;

    free(s->async_addr);
    s->async_addr = s_dup(addr);
    if (!s->async_addr) return 5;
    s->async_port = port;

    rc = thread_create(&s->listener_thread, async_listen_trampoline, s);
    if (rc != 0) {
        free(s->async_addr);
        s->async_addr = NULL;
        return 6;
    }
    s->listener_running = 1;
    return 0;
}

unsigned long http_ctx_request(const u8 **out, u64 *out_len, http_ctx *ctx) {
    if (!out) return 1;
    if (!out_len) return 2;
    if (!ctx) return 3;
    return http_ctx_body(out, out_len, ctx);
}

/*
 * Streaming response: HTTP/1.1 chunked Transfer-Encoding (RFC 7230 §4.1).
 *
 * Flow:
 *   http_ctx_respond_stream(ctx, status)   — send status line + headers
 *                                            with "Transfer-Encoding: chunked".
 *                                            Any caller-set Content-Length is
 *                                            stripped (they're mutually
 *                                            exclusive per RFC 7230 §3.3.3).
 *   http_ctx_stream_write(ctx, buf, len)   — send one chunk:
 *                                            "<hex-len>\r\n<data>\r\n".
 *                                            No-op on zero-length (which would
 *                                            otherwise be interpreted as the
 *                                            terminator).
 *   http_ctx_stream_end(ctx)               — send "0\r\n\r\n" terminator.
 *
 * After respond_stream, http_ctx_respond / http_ctx_respond_json / _file must
 * not be called. After stream_end, nothing more may be written.
 */

unsigned long http_ctx_respond_stream(http_ctx *ctx, u16 status) {
    resp_buf rb;
    char line[256];
    u64 i;
    u64 written = 0;
    unsigned long rc;

    if (!ctx) return 1;
    if (ctx->resp_sent) return 2;
    if (ctx->stream_started) return 3;

    ctx->resp_status = status;

    if (rb_init(&rb) != 0) return 4;

    /* status line */
    snprintf(line, sizeof(line), "HTTP/1.1 %u %s\r\n",
             (unsigned)status, reason_for(status));
    if (rb_append_str(&rb, line) != 0) { free(rb.data); return 4; }

    /* headers — skip any caller-set Content-Length (incompatible with
     * Transfer-Encoding: chunked per RFC 7230 §3.3.3) */
    for (i = 0; i < ctx->resp_headers.count; ++i) {
        const char *name = ctx->resp_headers.items[i].name;
        if (ci_eq(name, "Content-Length")) continue;
        if (ci_eq(name, "Transfer-Encoding")) continue;
        if (rb_append_str(&rb, name) != 0) { free(rb.data); return 4; }
        if (rb_append_str(&rb, ": ") != 0) { free(rb.data); return 4; }
        if (rb_append_str(&rb, ctx->resp_headers.items[i].value) != 0) {
            free(rb.data); return 4;
        }
        if (rb_append_str(&rb, "\r\n") != 0) { free(rb.data); return 4; }
    }

    if (rb_append_str(&rb, "Transfer-Encoding: chunked\r\n") != 0) {
        free(rb.data); return 4;
    }
    if (rb_append_str(&rb, "\r\n") != 0) { free(rb.data); return 4; }

    rc = srv_write_all(&written, ctx->conn, ctx->tls, rb.data, rb.len);
    free(rb.data);
    if (rc != 0) return 5;

    ctx->stream_started = 1;
    return 0;
}

unsigned long http_ctx_stream_write(http_ctx *ctx, const u8 *chunk, u64 chunk_len) {
    char hdr[32];
    int hdr_len;
    u64 written = 0;
    unsigned long rc;

    if (!ctx) return 1;
    if (!chunk && chunk_len > 0) return 2;
    if (!ctx->stream_started) return 3;
    if (ctx->resp_sent) return 4;

    /* A zero-length chunk would be interpreted as the terminator, so skip
     * it silently. Callers should use http_ctx_stream_end for that. */
    if (chunk_len == 0) return 0;

    /* chunk size line: "<hex>\r\n" */
    hdr_len = snprintf(hdr, sizeof(hdr), "%llx\r\n",
                       (unsigned long long)chunk_len);
    if (hdr_len <= 0 || hdr_len >= (int)sizeof(hdr)) return 5;

    rc = srv_write_all(&written, ctx->conn, ctx->tls,
                       (const u8 *)hdr, (u64)hdr_len);
    if (rc != 0) return 6;

    rc = srv_write_all(&written, ctx->conn, ctx->tls, chunk, chunk_len);
    if (rc != 0) return 7;

    rc = srv_write_all(&written, ctx->conn, ctx->tls,
                       (const u8 *)"\r\n", 2);
    if (rc != 0) return 8;

    return 0;
}

unsigned long http_ctx_stream_end(http_ctx *ctx) {
    u64 written = 0;
    unsigned long rc;

    if (!ctx) return 1;
    if (!ctx->stream_started) return 2;
    if (ctx->resp_sent) return 3;

    /* Final chunk: "0\r\n\r\n" (zero-length chunk + empty trailer) */
    rc = srv_write_all(&written, ctx->conn, ctx->tls,
                       (const u8 *)"0\r\n\r\n", 5);
    if (rc != 0) return 4;

    ctx->resp_sent = 1;
    return 0;
}

/* ================================================================
 *  Test-only route matching hook. Thin wrapper around route_match
 *  so the test suite can verify pattern→path behaviour without
 *  spinning up a real listener. Not part of the public API; the
 *  `_dbg` suffix warns callers.
 * ================================================================ */

APENNINES_API unsigned long http_server_dbg_route_match(
    int *out_matched,
    char *out_param_names, u64 out_names_cap,
    char *out_param_values, u64 out_values_cap,
    const char *pattern, const char *path);

unsigned long http_server_dbg_route_match(
    int *out_matched,
    char *out_param_names, u64 out_names_cap,
    char *out_param_values, u64 out_values_cap,
    const char *pattern, const char *path) {
    http_route r;
    route_param params[MAX_ROUTE_PARAMS];
    u32 param_count = 0;
    int matched;
    u32 i;
    u64 np = 0, vp = 0;

    if (!out_matched) return 1;
    if (!pattern)     return 2;
    if (!path)        return 3;

    memset(&r, 0, sizeof(r));
    r.pattern = (char *)pattern;  /* read-only in route_match */
    r.is_static = 0;

    matched = route_match(&r, path, params, &param_count);
    *out_matched = matched;

    /* Serialise captured params as comma-separated names/values into
     * caller buffers for easy string-compare in tests. */
    if (out_param_names && out_names_cap > 0) out_param_names[0] = '\0';
    if (out_param_values && out_values_cap > 0) out_param_values[0] = '\0';
    for (i = 0; i < param_count; i++) {
        u64 nl = params[i].name ? strlen(params[i].name) : 0;
        u64 vl = params[i].value ? strlen(params[i].value) : 0;
        if (out_param_names && np + nl + 2 < out_names_cap) {
            if (np > 0) out_param_names[np++] = ',';
            memcpy(out_param_names + np, params[i].name, (size_t)nl);
            np += nl;
            out_param_names[np] = '\0';
        }
        if (out_param_values && vp + vl + 2 < out_values_cap) {
            if (vp > 0) out_param_values[vp++] = ',';
            memcpy(out_param_values + vp, params[i].value, (size_t)vl);
            vp += vl;
            out_param_values[vp] = '\0';
        }
    }
    free_params(params, param_count);
    return 0;
}
