#ifndef COOKBOOK_HTTP_SHIM_H
#define COOKBOOK_HTTP_SHIM_H

/*
 * cookbook_http_shim.h — handler-facing adapter over apennines http_ctx.
 *
 * Handlers are written against a civetweb-style mg_connection API
 * (mg_printf / mg_write / mg_read / mg_get_request_info). This header
 * defines the shim types; cookbook_server.c #defines mg_* → shim_*.
 * cookbook_http.c is the implementation.
 *
 * History: civetweb was removed in rc3. The shim is kept because it's
 * a clean transport-isolation layer — handlers don't know or care that
 * the wire-level implementation is apennines.
 */

#include <apennines/t4/net/http_server.h>
#include <stddef.h>

/* Max headers we'll extract from http_ctx */
#define SHIM_MAX_HEADERS 64

typedef struct {
    const char *name;
    const char *value;
} shim_header;

struct shim_request_info {
    const char *request_method;
    const char *local_uri;
    const char *query_string;
    shim_header http_headers[SHIM_MAX_HEADERS];
    int         num_headers;
    long long   content_length;
};
typedef struct shim_request_info shim_request_info;

struct shim_connection {
    http_ctx                   *ctx;
    struct shim_request_info    ri;
    /* response state */
    int                status_sent;
    unsigned short     resp_status;
    /* request body cache */
    const unsigned char *body;
    size_t              body_len;
    size_t              body_pos;
};
typedef struct shim_connection shim_connection;

/* Initialize shim from http_ctx — populates request info */
void shim_init(shim_connection *sc, http_ctx *ctx);

/* mg_* compatible functions */
const shim_request_info *shim_get_request_info(shim_connection *sc);
int shim_printf(shim_connection *sc, const char *fmt, ...);
int shim_write(shim_connection *sc, const void *data, size_t len);
int shim_read(shim_connection *sc, void *buf, size_t len);
int shim_send_http_ok(shim_connection *sc, const char *content_type, long long len);

#endif /* COOKBOOK_HTTP_SHIM_H */
