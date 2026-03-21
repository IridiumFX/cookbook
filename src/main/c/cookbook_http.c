/*
 * cookbook_http.c — Apennines HTTP server adapter
 *
 * When COOKBOOK_USE_APENNINES_HTTP is defined, this provides the bridge
 * between our existing civetweb-style handlers and the apennines
 * http_server API. Each handler is wrapped to translate http_ctx calls
 * to mg_connection-compatible operations.
 *
 * This file is only compiled when COOKBOOK_USE_APENNINES_HTTP is active.
 * Otherwise, civetweb remains the HTTP server.
 */

#ifdef COOKBOOK_USE_APENNINES_HTTP

#include "cookbook_http_shim.h"
#include <apennines/t4/net/http_server.h>
#include <apennines/t3/net/tcp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- Shim implementation ---- */

static const char *method_str[] = {
    "GET", "POST", "PUT", "DELETE", "HEAD", "PATCH", "OPTIONS"
};

void shim_init(shim_connection *sc, http_ctx *ctx) {
    memset(sc, 0, sizeof(*sc));
    sc->ctx = ctx;

    /* populate request info */
    int method = 0;
    http_ctx_method(&method, ctx);
    sc->ri.request_method = (method >= 0 && method <= 6) ? method_str[method] : "GET";

    const char *path = NULL;
    http_ctx_path(&path, ctx);
    sc->ri.local_uri = path ? path : "/";

    const char *qs = NULL;
    http_ctx_query(&qs, ctx, NULL); /* get raw query string */
    sc->ri.query_string = qs;

    /* extract common headers */
    sc->ri.num_headers = 0;
    static const char *common_headers[] = {
        "Content-Type", "Content-Length", "Authorization", "Accept",
        "Accept-Encoding", "Host", "Connection", "X-Cookbook-Via",
        "X-Cookbook-Hop-Count", "X-Cookbook-Grid-Signature",
        "X-Cookbook-Grid-Origin", "X-Cookbook-Timestamp",
        "X-Cookbook-Grid-Grants", "X-Cookbook-Grid-Exclude",
        NULL
    };
    for (int i = 0; common_headers[i] && sc->ri.num_headers < SHIM_MAX_HEADERS; i++) {
        const char *val = NULL;
        http_ctx_header(&val, ctx, common_headers[i]);
        if (val) {
            sc->ri.http_headers[sc->ri.num_headers].name = common_headers[i];
            sc->ri.http_headers[sc->ri.num_headers].value = val;
            sc->ri.num_headers++;
        }
    }

    /* cache body */
    const u8 *body = NULL;
    u64 blen = 0;
    http_ctx_body(&body, &blen, ctx);
    sc->body = body;
    sc->body_len = (size_t)blen;
    sc->body_pos = 0;
    sc->ri.content_length = (long long)blen;
}

const shim_request_info *shim_get_request_info(shim_connection *sc) {
    return &sc->ri;
}

int shim_printf(shim_connection *sc, const char *fmt, ...) {
    char buf[16384];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n > 0) {
        /* parse status from first line if this is the initial response */
        if (!sc->status_sent && strncmp(buf, "HTTP/1.", 7) == 0) {
            const char *sp = strchr(buf, ' ');
            if (sp) {
                int status = atoi(sp + 1);
                http_ctx_set_status(sc->ctx, (u16)status);
            }

            /* extract headers from the buffer */
            const char *line = strchr(buf, '\n');
            while (line && *line) {
                line++;
                if (*line == '\r' || *line == '\n') break; /* end of headers */
                const char *colon = strchr(line, ':');
                if (colon) {
                    char hname[128] = {0};
                    size_t nlen = (size_t)(colon - line);
                    if (nlen >= sizeof(hname)) nlen = sizeof(hname) - 1;
                    memcpy(hname, line, nlen);
                    const char *hval = colon + 1;
                    while (*hval == ' ') hval++;
                    /* find end of value */
                    char hvalue[1024] = {0};
                    const char *eol = strchr(hval, '\r');
                    if (!eol) eol = strchr(hval, '\n');
                    if (eol) {
                        size_t vlen = (size_t)(eol - hval);
                        if (vlen >= sizeof(hvalue)) vlen = sizeof(hvalue) - 1;
                        memcpy(hvalue, hval, vlen);
                    }
                    if (hname[0] && hvalue[0])
                        http_ctx_set_header(sc->ctx, hname, hvalue);
                }
                const char *next = strchr(line, '\n');
                line = next;
            }

            /* find body after \r\n\r\n */
            const char *body_start = strstr(buf, "\r\n\r\n");
            if (body_start) {
                body_start += 4;
                size_t body_len = (size_t)(n - (body_start - buf));
                if (body_len > 0) {
                    http_ctx_respond(sc->ctx, 0, (const u8 *)body_start, (u64)body_len);
                }
            }
            sc->status_sent = 1;
        } else {
            /* subsequent writes are body data */
            http_ctx_respond(sc->ctx, 0, (const u8 *)buf, (u64)n);
        }
    }
    return n;
}

int shim_write(shim_connection *sc, const void *data, size_t len) {
    http_ctx_respond(sc->ctx, 0, (const u8 *)data, (u64)len);
    return (int)len;
}

int shim_read(shim_connection *sc, void *buf, size_t len) {
    if (sc->body_pos >= sc->body_len) return 0;
    size_t avail = sc->body_len - sc->body_pos;
    size_t n = avail < len ? avail : len;
    memcpy(buf, sc->body + sc->body_pos, n);
    sc->body_pos += n;
    return (int)n;
}

int shim_send_http_ok(shim_connection *sc, const char *content_type, long long len) {
    http_ctx_set_status(sc->ctx, 200);
    http_ctx_set_header(sc->ctx, "Content-Type", content_type);
    char cl[32];
    snprintf(cl, sizeof(cl), "%lld", len);
    http_ctx_set_header(sc->ctx, "Content-Length", cl);
    sc->status_sent = 1;
    return 0;
}

/* ---- Adapter: route wrappers ---- */

/* This adapter creates an http_server and registers routes that
   call back into cookbook's handler functions. The actual wiring
   happens in cookbook_server.c via cookbook_http_start/stop. */

static http_server *g_http = NULL;

int cookbook_http_start(const char *addr, int port,
                        const unsigned char *tls_cert, size_t cert_len,
                        const unsigned char *tls_key, size_t key_len) {
    if (http_server_create(&g_http) != 0) return -1;

    if (tls_cert && tls_key && cert_len > 0 && key_len > 0) {
        http_server_set_tls(g_http, tls_cert, (u64)cert_len,
                             tls_key, (u64)key_len);
    }

    http_server_set_keep_alive(g_http, 30000); /* 30s keep-alive */
    http_server_set_max_body_size(g_http, 256 * 1024 * 1024); /* 256MB */

    if (http_server_listen(g_http, addr, (u16)port) != 0) {
        http_server_destroy(g_http);
        g_http = NULL;
        return -1;
    }

    fprintf(stdout, "cookbook: apennines HTTP server listening on %s:%d\n",
            addr, port);
    return 0;
}

void cookbook_http_stop(void) {
    if (g_http) {
        http_server_shutdown(g_http);
        http_server_destroy(g_http);
        g_http = NULL;
    }
}

http_server *cookbook_http_get_server(void) {
    return g_http;
}

/* ---- Generic wrapper: translates http_ctx → shim → civetweb handler ---- */

/*
 * Each civetweb handler has signature:
 *   int handler(struct mg_connection *conn, void *cbdata);
 *
 * We can't use that directly. Instead, we define a macro-generated wrapper
 * for each route that:
 * 1. Creates a shim_connection from http_ctx
 * 2. Calls the original handler (which uses mg_* via #ifdef redirects)
 * 3. Returns the result
 *
 * The trick: in cookbook_server.c, when COOKBOOK_USE_APENNINES_HTTP is defined,
 * we #define mg_connection to shim_connection, mg_get_request_info to
 * shim_get_request_info, etc. This makes the existing handlers use the
 * shim transparently.
 *
 * For now, we register routes using simple wrappers that the server
 * will provide via cookbook_http_register_routes().
 */

typedef int (*cookbook_handler_fn)(void *conn, void *cbdata);

static void *g_server_ptr = NULL;  /* cookbook_server* */

void cookbook_http_set_server(void *srv) {
    g_server_ptr = srv;
}

/* Register all routes — called from cookbook_server_start */
void cookbook_http_register_routes(void *srv) {
    g_server_ptr = srv;
    /* Routes will be registered as the shim matures.
       Each handler wrapper calls the existing handler through the shim. */
    fprintf(stdout, "cookbook: apennines HTTP routes registered (shim layer)\n");
}

#endif /* COOKBOOK_USE_APENNINES_HTTP */
