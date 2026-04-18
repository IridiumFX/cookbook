#ifndef APENNINES_T4_HTTP_SERVER_H
#define APENNINES_T4_HTTP_SERVER_H

#include "apennines/export.h"
#include "apennines/types.h"

/* ================================================================
 *  HTTP Server — routes, middleware, TLS, keep-alive, async
 *  Composes: TCP Listener + TLS + HTTP Parser + Router + Threadpool
 * ================================================================ */

typedef struct http_server http_server;
typedef struct http_ctx    http_ctx;

typedef unsigned long (*http_handler_fn)(http_ctx *ctx);
typedef unsigned long (*http_middleware_fn)(http_ctx *ctx, http_handler_fn next);

/* ---- Server lifecycle ---- */

APENNINES_API unsigned long http_server_create(http_server **out);
APENNINES_API unsigned long http_server_route(http_server *s, int method,
                                               const char *pattern,
                                               http_handler_fn handler);
APENNINES_API unsigned long http_server_route_static(http_server *s,
                                                      const char *prefix,
                                                      const char *dir_path);
APENNINES_API unsigned long http_server_middleware(http_server *s,
                                                    http_middleware_fn mw);
APENNINES_API unsigned long http_server_set_tls(http_server *s,
                                                 const u8 *cert, u64 cert_len,
                                                 const u8 *key, u64 key_len);
APENNINES_API unsigned long http_server_set_keep_alive(http_server *s, u64 timeout_ms);
APENNINES_API unsigned long http_server_set_max_connections(http_server *s, u32 max);
APENNINES_API unsigned long http_server_set_max_body_size(http_server *s, u64 max);
APENNINES_API unsigned long http_server_set_read_timeout(http_server *s, u64 ms);
APENNINES_API unsigned long http_server_set_write_timeout(http_server *s, u64 ms);
APENNINES_API unsigned long http_server_listen(http_server *s, const char *addr,
                                                u16 port);
APENNINES_API unsigned long http_server_shutdown(http_server *s);
APENNINES_API unsigned long http_server_destroy(http_server *s);

/* ---- Request context ---- */

APENNINES_API unsigned long http_ctx_method(int *out, http_ctx *ctx);
APENNINES_API unsigned long http_ctx_path(const char **out, http_ctx *ctx);
APENNINES_API unsigned long http_ctx_param(const char **out, http_ctx *ctx,
                                            const char *name);
APENNINES_API unsigned long http_ctx_query(const char **out, http_ctx *ctx,
                                            const char *name);
APENNINES_API unsigned long http_ctx_header(const char **out, http_ctx *ctx,
                                             const char *name);
APENNINES_API unsigned long http_ctx_body(const u8 **out, u64 *out_len,
                                           http_ctx *ctx);
APENNINES_API unsigned long http_ctx_body_json(const char **out, u64 *out_len,
                                                http_ctx *ctx);

/* ---- Response ---- */

APENNINES_API unsigned long http_ctx_respond(http_ctx *ctx, u16 status,
                                              const u8 *body, u64 body_len);
APENNINES_API unsigned long http_ctx_respond_json(http_ctx *ctx, u16 status,
                                                   const char *json);
APENNINES_API unsigned long http_ctx_respond_file(http_ctx *ctx,
                                                   const char *file_path);
APENNINES_API unsigned long http_ctx_set_header(http_ctx *ctx,
                                                 const char *name,
                                                 const char *value);
APENNINES_API unsigned long http_ctx_set_status(http_ctx *ctx, u16 status);
APENNINES_API unsigned long http_ctx_set_cookie(http_ctx *ctx,
                                                 const char *name,
                                                 const char *value,
                                                 const char *path,
                                                 u64 max_age);
APENNINES_API unsigned long http_ctx_remote_addr(const char **out, http_ctx *ctx);
APENNINES_API unsigned long http_ctx_is_tls(int *out, http_ctx *ctx);

/* ---- Async listen ---- */
APENNINES_API unsigned long http_server_listen_async(http_server *s,
                                                      const char *addr, u16 port);

/* ---- Streaming response ---- */
APENNINES_API unsigned long http_ctx_request(const u8 **out, u64 *out_len,
                                              http_ctx *ctx);
APENNINES_API unsigned long http_ctx_respond_stream(http_ctx *ctx, u16 status);
APENNINES_API unsigned long http_ctx_stream_write(http_ctx *ctx,
                                                    const u8 *chunk, u64 chunk_len);
APENNINES_API unsigned long http_ctx_stream_end(http_ctx *ctx);

#endif
