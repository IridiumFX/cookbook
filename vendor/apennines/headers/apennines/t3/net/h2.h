#ifndef APENNINES_T3_H2_H
#define APENNINES_T3_H2_H

#include "apennines/export.h"
#include "apennines/types.h"

/* ================================================================
 *  HTTP/2 — RFC 7540 multiplexed streams + HPACK
 * ================================================================ */

typedef struct h2_conn h2_conn;

typedef struct {
    char *name;
    char *value;
} h2_header;

typedef struct {
    u32        stream_id;
    u16        status;
    h2_header *headers;
    u64        header_count;
    u8        *body;
    u64        body_len;
} h2_response;

/* ---- HPACK ---- */

typedef struct h2_hpack_ctx h2_hpack_ctx;

APENNINES_API unsigned long h2_hpack_ctx_create(h2_hpack_ctx **out);
APENNINES_API unsigned long h2_hpack_encode(u8 **out, u64 *out_len,
                                             h2_hpack_ctx *ctx,
                                             const h2_header *headers, u64 count);
APENNINES_API unsigned long h2_hpack_decode(h2_header **out, u64 *out_count,
                                             h2_hpack_ctx *ctx,
                                             const u8 *data, u64 len);
APENNINES_API unsigned long h2_hpack_ctx_destroy(h2_hpack_ctx *ctx);

/* Free a header array returned by h2_hpack_decode. */
APENNINES_API unsigned long h2_headers_free(h2_header *headers, u64 count);

/* ---- Connection ---- */

/* Create H2 connection. fd_read/fd_write are function pointers for I/O.
 * For TLS: pass tls_conn_read/tls_conn_write wrappers.
 * Hatches: 1=null out, 2=null read_fn, 3=null write_fn, 4=alloc failure,
 *          5=preface exchange failed */
typedef unsigned long (*h2_read_fn)(u64 *n, void *ctx, u8 *buf, u64 len);
typedef unsigned long (*h2_write_fn)(u64 *n, void *ctx, const u8 *data, u64 len);

APENNINES_API unsigned long h2_conn_create(h2_conn **out,
                                            h2_read_fn read_fn,
                                            h2_write_fn write_fn,
                                            void *io_ctx);

/* Submit a request, get stream ID.
 * Hatches: 1=null out_stream_id, 2=null conn, 3=null method,
 *          4=null path, 5=alloc failure */
APENNINES_API unsigned long h2_conn_submit(u32 *out_stream_id, h2_conn *conn,
                                            const char *method, const char *path,
                                            const char *authority,
                                            const h2_header *extra_headers,
                                            u64 extra_count);

/* Send pending outbound frames. */
APENNINES_API unsigned long h2_conn_send(h2_conn *conn);

/* Receive and process inbound frames. */
APENNINES_API unsigned long h2_conn_recv(h2_conn *conn);

/* Read response for a stream (blocks until headers + body received).
 * Hatches: 1=null out, 2=null conn, 3=stream not found, 4=stream error,
 *          5=alloc failure */
APENNINES_API unsigned long h2_conn_read_response(h2_response *out,
                                                   h2_conn *conn, u32 stream_id);

/* Send DATA on a stream. */
APENNINES_API unsigned long h2_conn_send_data(h2_conn *conn, u32 stream_id,
                                               const u8 *data, u64 len,
                                               int end_stream);

APENNINES_API unsigned long h2_conn_close_stream(h2_conn *conn, u32 stream_id);
APENNINES_API unsigned long h2_conn_goaway(h2_conn *conn, u32 error_code);
APENNINES_API unsigned long h2_conn_ping(h2_conn *conn);
APENNINES_API unsigned long h2_conn_destroy(h2_conn *conn);
APENNINES_API unsigned long h2_response_free(h2_response *resp);

#endif /* APENNINES_T3_H2_H */
