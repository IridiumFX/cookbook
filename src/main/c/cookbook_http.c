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

#include <apennines/t4/net/http_server.h>
#include <apennines/t3/net/tcp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#endif /* COOKBOOK_USE_APENNINES_HTTP */
