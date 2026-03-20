#include "apennines/t3/net/h2.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 *  Helpers
 * ================================================================ */

static char *str_dup(const char *s) {
    size_t len;
    char *d;
    if (!s) return NULL;
    len = strlen(s);
    d = (char *)malloc(len + 1);
    if (!d) return NULL;
    memcpy(d, s, len + 1);
    return d;
}

static char *str_ndup(const char *s, size_t n) {
    char *d;
    if (!s) return NULL;
    d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

/* ================================================================
 *  H2 frame constants
 * ================================================================ */

#define H2_FRAME_HEADER_SIZE       9
#define H2_DEFAULT_MAX_FRAME_SIZE  16384
#define H2_DEFAULT_WINDOW_SIZE     65535
#define H2_DEFAULT_MAX_CONCURRENT  100

/* Frame types */
#define H2_FT_DATA          0
#define H2_FT_HEADERS       1
#define H2_FT_PRIORITY      2
#define H2_FT_RST_STREAM    3
#define H2_FT_SETTINGS      4
#define H2_FT_PUSH_PROMISE  5
#define H2_FT_PING          6
#define H2_FT_GOAWAY        7
#define H2_FT_WINDOW_UPDATE 8
#define H2_FT_CONTINUATION  9

/* Flags */
#define H2_FLAG_END_STREAM  0x01
#define H2_FLAG_END_HEADERS 0x04
#define H2_FLAG_PADDED      0x08
#define H2_FLAG_PRIORITY    0x20
#define H2_FLAG_ACK         0x01

/* Settings identifiers */
#define H2_SET_HEADER_TABLE_SIZE      0x01
#define H2_SET_ENABLE_PUSH            0x02
#define H2_SET_MAX_CONCURRENT_STREAMS 0x03
#define H2_SET_INITIAL_WINDOW_SIZE    0x04
#define H2_SET_MAX_FRAME_SIZE         0x05
#define H2_SET_MAX_HEADER_LIST_SIZE   0x06

/* Error codes */
#define H2_NO_ERROR            0x00
#define H2_PROTOCOL_ERROR      0x01
#define H2_FLOW_CONTROL_ERROR  0x03
#define H2_ERR_STREAM_CLOSED   0x05
#define H2_FRAME_SIZE_ERROR    0x06
#define H2_COMPRESSION_ERROR   0x09

/* Connection preface */
static const char H2_CLIENT_PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
#define H2_CLIENT_PREFACE_LEN 24

/* ================================================================
 *  HPACK static table (RFC 7541 Appendix A)
 * ================================================================ */

typedef struct {
    const char *name;
    const char *value;
} hpack_static_entry;

static const hpack_static_entry STATIC_TABLE[] = {
    { NULL,                        NULL },  /* index 0 unused */
    { ":authority",                ""   },  /*  1 */
    { ":method",                   "GET"  },
    { ":method",                   "POST" },
    { ":path",                     "/"    },
    { ":path",                     "/index.html" },
    { ":scheme",                   "http"  },
    { ":scheme",                   "https" },
    { ":status",                   "200" },
    { ":status",                   "204" },
    { ":status",                   "206" }, /* 10 */
    { ":status",                   "304" },
    { ":status",                   "400" },
    { ":status",                   "404" },
    { ":status",                   "500" },
    { "accept-charset",            ""    },
    { "accept-encoding",           "gzip, deflate" },
    { "accept-language",           ""    },
    { "accept-ranges",             ""    },
    { "accept",                    ""    },
    { "access-control-allow-origin", "" }, /* 20 */
    { "age",                       ""    },
    { "allow",                     ""    },
    { "authorization",             ""    },
    { "cache-control",             ""    },
    { "content-disposition",       ""    },
    { "content-encoding",          ""    },
    { "content-language",          ""    },
    { "content-length",            ""    },
    { "content-location",          ""    },
    { "content-range",             ""    }, /* 30 */
    { "content-type",              ""    },
    { "cookie",                    ""    },
    { "date",                      ""    },
    { "etag",                      ""    },
    { "expect",                    ""    },
    { "expires",                   ""    },
    { "from",                      ""    },
    { "host",                      ""    },
    { "if-match",                  ""    },
    { "if-modified-since",         ""    }, /* 40 */
    { "if-none-match",             ""    },
    { "if-range",                  ""    },
    { "if-unmodified-since",       ""    },
    { "last-modified",             ""    },
    { "link",                      ""    },
    { "location",                  ""    },
    { "max-forwards",              ""    },
    { "proxy-authenticate",        ""    },
    { "proxy-authorization",       ""    },
    { "range",                     ""    }, /* 50 */
    { "referer",                   ""    },
    { "refresh",                   ""    },
    { "retry-after",               ""    },
    { "server",                    ""    },
    { "set-cookie",                ""    },
    { "strict-transport-security", ""    },
    { "transfer-encoding",         ""    },
    { "user-agent",                ""    },
    { "vary",                      ""    },
    { "via",                       ""    }, /* 60 */
    { "www-authenticate",          ""    }  /* 61 */
};
#define STATIC_TABLE_LEN 61

/* ================================================================
 *  HPACK dynamic table
 * ================================================================ */

typedef struct {
    char *name;
    char *value;
} hpack_dyn_entry;

struct h2_hpack_ctx {
    hpack_dyn_entry *entries;
    u64 count;
    u64 capacity;
    u64 max_size;       /* max dynamic table size in bytes (SETTINGS) */
    u64 current_size;   /* current size in bytes */
};

#define HPACK_DYN_INIT_CAP  64
#define HPACK_ENTRY_OVERHEAD 32   /* RFC 7541 sec 4.1: 32 bytes overhead per entry */
#define HPACK_DEFAULT_MAX_SIZE 4096

static u64 hpack_entry_size(const char *name, const char *value) {
    return (u64)(strlen(name) + strlen(value) + HPACK_ENTRY_OVERHEAD);
}

/* Evict oldest entries until current_size <= max_size */
static void hpack_evict(h2_hpack_ctx *ctx) {
    while (ctx->current_size > ctx->max_size && ctx->count > 0) {
        u64 last = ctx->count - 1;
        ctx->current_size -= hpack_entry_size(ctx->entries[last].name,
                                              ctx->entries[last].value);
        free(ctx->entries[last].name);
        free(ctx->entries[last].value);
        ctx->count--;
    }
}

/* Insert at front (index 0), shift everything else down. */
static int hpack_insert(h2_hpack_ctx *ctx, const char *name, const char *value) {
    u64 esize = hpack_entry_size(name, value);
    char *n, *v;

    /* Evict until there's room */
    while (ctx->current_size + esize > ctx->max_size && ctx->count > 0) {
        u64 last = ctx->count - 1;
        ctx->current_size -= hpack_entry_size(ctx->entries[last].name,
                                              ctx->entries[last].value);
        free(ctx->entries[last].name);
        free(ctx->entries[last].value);
        ctx->count--;
    }

    /* If entry itself is larger than max, table is empty and we can't add it */
    if (esize > ctx->max_size) return 0; /* success: table cleared, entry not added */

    /* Grow if needed */
    if (ctx->count >= ctx->capacity) {
        u64 newcap = ctx->capacity * 2;
        hpack_dyn_entry *ne = (hpack_dyn_entry *)realloc(ctx->entries,
                                    newcap * sizeof(hpack_dyn_entry));
        if (!ne) return -1;
        ctx->entries = ne;
        ctx->capacity = newcap;
    }

    n = str_dup(name);
    v = str_dup(value);
    if (!n || !v) { free(n); free(v); return -1; }

    /* Shift right */
    if (ctx->count > 0) {
        memmove(&ctx->entries[1], &ctx->entries[0],
                ctx->count * sizeof(hpack_dyn_entry));
    }
    ctx->entries[0].name = n;
    ctx->entries[0].value = v;
    ctx->count++;
    ctx->current_size += esize;
    return 0;
}

/* Lookup: returns 1-based index (static 1-61, dynamic 62+).
 * Sets *name_only=1 if only name matches. Returns 0 if no match. */
static u64 hpack_lookup(const h2_hpack_ctx *ctx, const char *name,
                         const char *value, int *name_only) {
    u64 i, name_match = 0;
    *name_only = 0;

    /* Search static table */
    for (i = 1; i <= STATIC_TABLE_LEN; i++) {
        if (strcmp(STATIC_TABLE[i].name, name) == 0) {
            if (strcmp(STATIC_TABLE[i].value, value) == 0) return i;
            if (name_match == 0) name_match = i;
        }
    }

    /* Search dynamic table */
    for (i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->entries[i].name, name) == 0) {
            if (strcmp(ctx->entries[i].value, value) == 0) {
                return STATIC_TABLE_LEN + 1 + i;
            }
            if (name_match == 0) name_match = STATIC_TABLE_LEN + 1 + i;
        }
    }

    if (name_match > 0) {
        *name_only = 1;
        return name_match;
    }
    return 0;
}

/* Get entry by 1-based index. Returns 0 on success. */
static int hpack_get(const h2_hpack_ctx *ctx, u64 index,
                     const char **name, const char **value) {
    if (index == 0) return -1;
    if (index <= STATIC_TABLE_LEN) {
        *name = STATIC_TABLE[index].name;
        *value = STATIC_TABLE[index].value;
        return 0;
    }
    index -= (STATIC_TABLE_LEN + 1);
    if (index >= ctx->count) return -1;
    *name = ctx->entries[index].name;
    *value = ctx->entries[index].value;
    return 0;
}

/* ================================================================
 *  HPACK integer encoding / decoding (RFC 7541 sec 5.1)
 * ================================================================ */

static int hpack_encode_int(u8 *buf, u64 buflen, u64 *written,
                            u64 val, u8 prefix_bits, u8 pattern) {
    u64 max_prefix = ((u64)1 << prefix_bits) - 1;
    u64 pos = 0;

    if (buflen == 0) return -1;

    if (val < max_prefix) {
        buf[pos++] = (u8)(pattern | val);
    } else {
        buf[pos++] = (u8)(pattern | max_prefix);
        val -= max_prefix;
        while (val >= 128) {
            if (pos >= buflen) return -1;
            buf[pos++] = (u8)((val & 0x7F) | 0x80);
            val >>= 7;
        }
        if (pos >= buflen) return -1;
        buf[pos++] = (u8)val;
    }
    *written = pos;
    return 0;
}

static int hpack_decode_int(u64 *out, const u8 *data, u64 len,
                            u64 *consumed, u8 prefix_bits) {
    u64 max_prefix = ((u64)1 << prefix_bits) - 1;
    u64 val, pos = 0, shift;

    if (len == 0) return -1;

    val = data[pos] & (u8)max_prefix;
    pos++;

    if (val < max_prefix) {
        *out = val;
        *consumed = pos;
        return 0;
    }

    shift = 0;
    do {
        if (pos >= len) return -1;
        val += ((u64)(data[pos] & 0x7F)) << shift;
        shift += 7;
        if (shift > 63) return -1;  /* overflow protection */
    } while (data[pos++] & 0x80);

    *out = val;
    *consumed = pos;
    return 0;
}

/* Encode a string literal (no Huffman). */
static int hpack_encode_str(u8 *buf, u64 buflen, u64 *written,
                            const char *s) {
    u64 slen = strlen(s);
    u64 int_len, pos = 0;

    /* H=0 (no Huffman), 7-bit prefix for string length */
    if (hpack_encode_int(buf, buflen, &int_len, slen, 7, 0x00) != 0)
        return -1;
    pos += int_len;
    if (pos + slen > buflen) return -1;
    memcpy(buf + pos, s, slen);
    pos += slen;
    *written = pos;
    return 0;
}

/* Decode a string literal. Caller must free *out. */
static int hpack_decode_str(char **out, const u8 *data, u64 len, u64 *consumed) {
    u64 slen, int_consumed;
    int huffman;

    if (len == 0) return -1;
    huffman = (data[0] & 0x80) ? 1 : 0;
    (void)huffman; /* We don't support Huffman decoding; treat as literal */

    if (hpack_decode_int(&slen, data, len, &int_consumed, 7) != 0) return -1;

    if (int_consumed + slen > len) return -1;

    *out = str_ndup((const char *)(data + int_consumed), (size_t)slen);
    if (!*out) return -1;

    *consumed = int_consumed + slen;
    return 0;
}

/* ================================================================
 *  HPACK API
 * ================================================================ */

APENNINES_API unsigned long h2_hpack_ctx_create(h2_hpack_ctx **out) {
    h2_hpack_ctx *ctx;
    if (!out) return 1;
    ctx = (h2_hpack_ctx *)calloc(1, sizeof(h2_hpack_ctx));
    if (!ctx) return 2;
    ctx->capacity = HPACK_DYN_INIT_CAP;
    ctx->entries = (hpack_dyn_entry *)calloc(ctx->capacity, sizeof(hpack_dyn_entry));
    if (!ctx->entries) { free(ctx); return 2; }
    ctx->max_size = HPACK_DEFAULT_MAX_SIZE;
    *out = ctx;
    return 0;
}

APENNINES_API unsigned long h2_hpack_encode(u8 **out, u64 *out_len,
                                             h2_hpack_ctx *ctx,
                                             const h2_header *headers, u64 count) {
    u64 buf_cap, pos, i;
    u8 *buf;

    if (!out) return 1;
    if (!out_len) return 2;
    if (!ctx) return 3;
    if (!headers && count > 0) return 4;

    /* Allocate a generous buffer; 256 bytes per header as estimate */
    buf_cap = (count + 1) * 256;
    if (buf_cap < 256) buf_cap = 256;
    buf = (u8 *)malloc(buf_cap);
    if (!buf) return 5;

    pos = 0;
    for (i = 0; i < count; i++) {
        int name_only = 0;
        u64 idx = hpack_lookup(ctx, headers[i].name, headers[i].value, &name_only);
        u64 written;

        /* Ensure we have space (realloc if needed) */
        if (pos + 512 > buf_cap) {
            u64 newcap = buf_cap * 2;
            u8 *nb = (u8 *)realloc(buf, newcap);
            if (!nb) { free(buf); return 5; }
            buf = nb;
            buf_cap = newcap;
        }

        if (idx > 0 && !name_only) {
            /* Fully indexed: 1-bit prefix = 1, 7-bit index */
            if (hpack_encode_int(buf + pos, buf_cap - pos, &written,
                                 idx, 7, 0x80) != 0) {
                free(buf);
                return 6;
            }
            pos += written;
        } else if (idx > 0 && name_only) {
            /* Literal with incremental indexing, name indexed:
             * 01xxxxxx for 6-bit index prefix */
            if (hpack_encode_int(buf + pos, buf_cap - pos, &written,
                                 idx, 6, 0x40) != 0) {
                free(buf);
                return 6;
            }
            pos += written;
            /* Value string */
            if (hpack_encode_str(buf + pos, buf_cap - pos, &written,
                                 headers[i].value) != 0) {
                free(buf);
                return 6;
            }
            pos += written;
            hpack_insert(ctx, headers[i].name, headers[i].value);
        } else {
            /* Literal with incremental indexing, new name:
             * 01000000 = 0x40 with index 0 */
            buf[pos++] = 0x40;
            /* Name string */
            if (hpack_encode_str(buf + pos, buf_cap - pos, &written,
                                 headers[i].name) != 0) {
                free(buf);
                return 6;
            }
            pos += written;
            /* Value string */
            if (hpack_encode_str(buf + pos, buf_cap - pos, &written,
                                 headers[i].value) != 0) {
                free(buf);
                return 6;
            }
            pos += written;
            hpack_insert(ctx, headers[i].name, headers[i].value);
        }
    }

    *out = buf;
    *out_len = pos;
    return 0;
}

APENNINES_API unsigned long h2_hpack_decode(h2_header **out, u64 *out_count,
                                             h2_hpack_ctx *ctx,
                                             const u8 *data, u64 len) {
    u64 pos = 0, hdr_cap = 16, hdr_count = 0;
    u64 cleanup_i;
    h2_header *hdrs;

    if (!out) return 1;
    if (!out_count) return 2;
    if (!ctx) return 3;

    hdrs = (h2_header *)calloc(hdr_cap, sizeof(h2_header));
    if (!hdrs) return 4;

    while (pos < len) {
        u8 byte = data[pos];
        char *name = NULL, *value = NULL;

        /* Grow header array if needed */
        if (hdr_count >= hdr_cap) {
            u64 newcap = hdr_cap * 2;
            h2_header *nh = (h2_header *)realloc(hdrs, newcap * sizeof(h2_header));
            if (!nh) goto decode_err;
            hdrs = nh;
            hdr_cap = newcap;
        }

        if (byte & 0x80) {
            /* Indexed header field (sec 6.1): 1xxxxxxx */
            u64 idx, consumed;
            const char *sn, *sv;
            if (hpack_decode_int(&idx, data + pos, len - pos, &consumed, 7) != 0)
                goto decode_err;
            pos += consumed;
            if (hpack_get(ctx, idx, &sn, &sv) != 0) goto decode_err;
            name = str_dup(sn);
            value = str_dup(sv);
            if (!name || !value) goto decode_err;
        } else if ((byte & 0xC0) == 0x40) {
            /* Literal with incremental indexing (sec 6.2.1): 01xxxxxx */
            u64 idx, consumed;
            if (hpack_decode_int(&idx, data + pos, len - pos, &consumed, 6) != 0)
                goto decode_err;
            pos += consumed;

            if (idx > 0) {
                /* Name from table */
                const char *sn, *sv;
                if (hpack_get(ctx, idx, &sn, &sv) != 0) goto decode_err;
                name = str_dup(sn);
                if (!name) goto decode_err;
            } else {
                /* New name literal */
                u64 str_consumed;
                if (hpack_decode_str(&name, data + pos, len - pos,
                                     &str_consumed) != 0)
                    goto decode_err;
                pos += str_consumed;
            }

            /* Value literal */
            {
                u64 str_consumed;
                if (hpack_decode_str(&value, data + pos, len - pos,
                                     &str_consumed) != 0) {
                    free(name);
                    goto decode_err;
                }
                pos += str_consumed;
            }

            /* Add to dynamic table */
            hpack_insert(ctx, name, value);
        } else if ((byte & 0xF0) == 0x00 || (byte & 0xF0) == 0x10) {
            /* Literal without indexing (sec 6.2.2): 0000xxxx
             * Literal never indexed (sec 6.2.3):   0001xxxx */
            u8 prefix = (byte & 0xF0) == 0x00 ? 4 : 4;
            u64 idx, consumed;
            if (hpack_decode_int(&idx, data + pos, len - pos, &consumed, prefix) != 0)
                goto decode_err;
            pos += consumed;

            if (idx > 0) {
                const char *sn, *sv;
                if (hpack_get(ctx, idx, &sn, &sv) != 0) goto decode_err;
                name = str_dup(sn);
                if (!name) goto decode_err;
            } else {
                u64 str_consumed;
                if (hpack_decode_str(&name, data + pos, len - pos,
                                     &str_consumed) != 0)
                    goto decode_err;
                pos += str_consumed;
            }

            {
                u64 str_consumed;
                if (hpack_decode_str(&value, data + pos, len - pos,
                                     &str_consumed) != 0) {
                    free(name);
                    goto decode_err;
                }
                pos += str_consumed;
            }
            /* Do NOT add to dynamic table */
        } else if ((byte & 0xE0) == 0x20) {
            /* Dynamic table size update (sec 6.3): 001xxxxx */
            u64 new_size, consumed;
            if (hpack_decode_int(&new_size, data + pos, len - pos,
                                 &consumed, 5) != 0)
                goto decode_err;
            pos += consumed;
            ctx->max_size = new_size;
            hpack_evict(ctx);
            continue;
        } else {
            goto decode_err;
        }

        hdrs[hdr_count].name = name;
        hdrs[hdr_count].value = value;
        hdr_count++;
        continue;

decode_err:
        /* Cleanup on error */
        for (cleanup_i = 0; cleanup_i < hdr_count; cleanup_i++) {
            free(hdrs[cleanup_i].name);
            free(hdrs[cleanup_i].value);
        }
        free(hdrs);
        return 5;
    }

    *out = hdrs;
    *out_count = hdr_count;
    return 0;
}

APENNINES_API unsigned long h2_hpack_ctx_destroy(h2_hpack_ctx *ctx) {
    u64 i;
    if (!ctx) return 1;
    for (i = 0; i < ctx->count; i++) {
        free(ctx->entries[i].name);
        free(ctx->entries[i].value);
    }
    free(ctx->entries);
    free(ctx);
    return 0;
}

APENNINES_API unsigned long h2_headers_free(h2_header *headers, u64 count) {
    u64 i;
    if (!headers) return 1;
    for (i = 0; i < count; i++) {
        free(headers[i].name);
        free(headers[i].value);
    }
    free(headers);
    return 0;
}

/* ================================================================
 *  H2 stream
 * ================================================================ */

typedef enum {
    H2_STREAM_IDLE,
    H2_STREAM_OPEN,
    H2_STREAM_HALF_CLOSED_LOCAL,
    H2_STREAM_HALF_CLOSED_REMOTE,
    H2_STREAM_CLOSED
} h2_stream_state;

typedef struct h2_stream {
    u32             id;
    h2_stream_state state;
    /* Response storage */
    u16             status;
    h2_header      *headers;
    u64             header_count;
    u64             header_cap;
    u8             *body;
    u64             body_len;
    u64             body_cap;
    /* Flow control */
    i64             recv_window;
    i64             send_window;
    u32             error_code;
} h2_stream;

/* ================================================================
 *  H2 connection
 * ================================================================ */

struct h2_conn {
    h2_read_fn   read_fn;
    h2_write_fn  write_fn;
    void         *io_ctx;
    h2_hpack_ctx *hpack_enc;
    h2_hpack_ctx *hpack_dec;
    /* Stream table */
    h2_stream    *streams;
    u64           stream_count;
    u64           stream_cap;
    u32           next_stream_id;   /* odd for client */
    u32           last_stream_id;
    /* Settings (peer's) */
    u32           max_frame_size;
    u32           initial_window_size;
    u32           max_concurrent_streams;
    u32           header_table_size;
    /* Connection-level flow control */
    i64           recv_window;
    i64           send_window;
    /* Outbound buffer */
    u8           *outbuf;
    u64           outbuf_len;
    u64           outbuf_cap;
    /* GOAWAY state */
    int           goaway_received;
    u32           goaway_last_stream;
    u32           goaway_error;
};

#define CONN_INIT_STREAM_CAP  16
#define CONN_INIT_OUTBUF_CAP  4096

/* ================================================================
 *  Connection internals
 * ================================================================ */

static int outbuf_ensure(h2_conn *conn, u64 need) {
    if (conn->outbuf_len + need <= conn->outbuf_cap) return 0;
    u64 newcap = conn->outbuf_cap * 2;
    while (newcap < conn->outbuf_len + need) newcap *= 2;
    u8 *nb = (u8 *)realloc(conn->outbuf, newcap);
    if (!nb) return -1;
    conn->outbuf = nb;
    conn->outbuf_cap = newcap;
    return 0;
}

static void outbuf_append(h2_conn *conn, const u8 *data, u64 len) {
    memcpy(conn->outbuf + conn->outbuf_len, data, len);
    conn->outbuf_len += len;
}

/* Write a 9-byte frame header into buf. */
static void frame_header_write(u8 *buf, u32 length, u8 type, u8 flags, u32 stream_id) {
    buf[0] = (u8)((length >> 16) & 0xFF);
    buf[1] = (u8)((length >> 8) & 0xFF);
    buf[2] = (u8)(length & 0xFF);
    buf[3] = type;
    buf[4] = flags;
    buf[5] = (u8)((stream_id >> 24) & 0x7F); /* clear reserved bit */
    buf[6] = (u8)((stream_id >> 16) & 0xFF);
    buf[7] = (u8)((stream_id >> 8) & 0xFF);
    buf[8] = (u8)(stream_id & 0xFF);
}

/* Parse a 9-byte frame header from buf. */
static void frame_header_read(const u8 *buf, u32 *length, u8 *type,
                               u8 *flags, u32 *stream_id) {
    *length = ((u32)buf[0] << 16) | ((u32)buf[1] << 8) | (u32)buf[2];
    *type = buf[3];
    *flags = buf[4];
    *stream_id = ((u32)(buf[5] & 0x7F) << 24) | ((u32)buf[6] << 16) |
                 ((u32)buf[7] << 8) | (u32)buf[8];
}

/* Buffer a complete frame to outbuf. */
static int conn_buffer_frame(h2_conn *conn, u8 type, u8 flags,
                              u32 stream_id, const u8 *payload, u32 payload_len) {
    u8 hdr[H2_FRAME_HEADER_SIZE];
    if (outbuf_ensure(conn, H2_FRAME_HEADER_SIZE + payload_len) != 0) return -1;
    frame_header_write(hdr, payload_len, type, flags, stream_id);
    outbuf_append(conn, hdr, H2_FRAME_HEADER_SIZE);
    if (payload_len > 0 && payload) {
        outbuf_append(conn, payload, payload_len);
    }
    return 0;
}

/* Build SETTINGS frame payload from our preferred settings. */
static int conn_buffer_settings(h2_conn *conn) {
    /* Send our settings: max concurrent, initial window, max frame size */
    u8 payload[18]; /* 3 settings x 6 bytes each */
    u32 pos = 0;

    /* SETTINGS_MAX_CONCURRENT_STREAMS */
    payload[pos++] = 0x00; payload[pos++] = H2_SET_MAX_CONCURRENT_STREAMS;
    payload[pos++] = 0x00; payload[pos++] = 0x00;
    payload[pos++] = 0x00; payload[pos++] = (u8)H2_DEFAULT_MAX_CONCURRENT;

    /* SETTINGS_INITIAL_WINDOW_SIZE */
    payload[pos++] = 0x00; payload[pos++] = H2_SET_INITIAL_WINDOW_SIZE;
    payload[pos++] = (u8)((H2_DEFAULT_WINDOW_SIZE >> 24) & 0xFF);
    payload[pos++] = (u8)((H2_DEFAULT_WINDOW_SIZE >> 16) & 0xFF);
    payload[pos++] = (u8)((H2_DEFAULT_WINDOW_SIZE >> 8) & 0xFF);
    payload[pos++] = (u8)(H2_DEFAULT_WINDOW_SIZE & 0xFF);

    /* SETTINGS_MAX_FRAME_SIZE */
    payload[pos++] = 0x00; payload[pos++] = H2_SET_MAX_FRAME_SIZE;
    payload[pos++] = (u8)((H2_DEFAULT_MAX_FRAME_SIZE >> 24) & 0xFF);
    payload[pos++] = (u8)((H2_DEFAULT_MAX_FRAME_SIZE >> 16) & 0xFF);
    payload[pos++] = (u8)((H2_DEFAULT_MAX_FRAME_SIZE >> 8) & 0xFF);
    payload[pos++] = (u8)(H2_DEFAULT_MAX_FRAME_SIZE & 0xFF);

    return conn_buffer_frame(conn, H2_FT_SETTINGS, 0, 0, payload, pos);
}

/* Read exactly `len` bytes via read_fn. Returns 0 on success. */
static unsigned long conn_read_exact(h2_conn *conn, u8 *buf, u64 len) {
    u64 total = 0;
    while (total < len) {
        u64 n = 0;
        unsigned long rc = conn->read_fn(&n, conn->io_ctx, buf + total, len - total);
        if (rc != 0) return rc;
        if (n == 0) return 1; /* EOF */
        total += n;
    }
    return 0;
}

/* Write all bytes via write_fn. Returns 0 on success. */
static unsigned long conn_write_all(h2_conn *conn, const u8 *data, u64 len) {
    u64 total = 0;
    while (total < len) {
        u64 n = 0;
        unsigned long rc = conn->write_fn(&n, conn->io_ctx, data + total, len - total);
        if (rc != 0) return rc;
        if (n == 0) return 1;
        total += n;
    }
    return 0;
}

/* Find or create stream by ID. */
static h2_stream *conn_find_stream(h2_conn *conn, u32 id) {
    u64 i;
    for (i = 0; i < conn->stream_count; i++) {
        if (conn->streams[i].id == id) return &conn->streams[i];
    }
    return NULL;
}

static h2_stream *conn_create_stream(h2_conn *conn, u32 id) {
    h2_stream *s;
    if (conn->stream_count >= conn->stream_cap) {
        u64 newcap = conn->stream_cap * 2;
        h2_stream *ns = (h2_stream *)realloc(conn->streams,
                             newcap * sizeof(h2_stream));
        if (!ns) return NULL;
        conn->streams = ns;
        conn->stream_cap = newcap;
    }
    s = &conn->streams[conn->stream_count++];
    memset(s, 0, sizeof(h2_stream));
    s->id = id;
    s->state = H2_STREAM_OPEN;
    s->recv_window = (i64)conn->initial_window_size;
    s->send_window = (i64)conn->initial_window_size;
    s->header_cap = 16;
    s->headers = (h2_header *)calloc(s->header_cap, sizeof(h2_header));
    s->body_cap = 1024;
    s->body = (u8 *)malloc(s->body_cap);
    return s;
}

/* Append headers to a stream. */
static int stream_add_headers(h2_stream *s, const h2_header *hdrs, u64 count) {
    u64 i;
    for (i = 0; i < count; i++) {
        if (s->header_count >= s->header_cap) {
            u64 newcap = s->header_cap * 2;
            h2_header *nh = (h2_header *)realloc(s->headers,
                                 newcap * sizeof(h2_header));
            if (!nh) return -1;
            s->headers = nh;
            s->header_cap = newcap;
        }

        /* Check for :status pseudo-header */
        if (strcmp(hdrs[i].name, ":status") == 0) {
            s->status = (u16)atoi(hdrs[i].value);
        }

        s->headers[s->header_count].name = str_dup(hdrs[i].name);
        s->headers[s->header_count].value = str_dup(hdrs[i].value);
        if (!s->headers[s->header_count].name ||
            !s->headers[s->header_count].value) return -1;
        s->header_count++;
    }
    return 0;
}

/* Append data to a stream body. */
static int stream_append_body(h2_stream *s, const u8 *data, u64 len) {
    if (s->body_len + len > s->body_cap) {
        u64 newcap = s->body_cap * 2;
        while (newcap < s->body_len + len) newcap *= 2;
        u8 *nb = (u8 *)realloc(s->body, newcap);
        if (!nb) return -1;
        s->body = nb;
        s->body_cap = newcap;
    }
    memcpy(s->body + s->body_len, data, len);
    s->body_len += len;
    return 0;
}

static void stream_free_contents(h2_stream *s) {
    u64 i;
    for (i = 0; i < s->header_count; i++) {
        free(s->headers[i].name);
        free(s->headers[i].value);
    }
    free(s->headers);
    free(s->body);
}

/* ================================================================
 *  Frame processing (recv)
 * ================================================================ */

static unsigned long process_settings(h2_conn *conn, const u8 *payload,
                                       u32 len, u8 flags) {
    u32 pos;

    /* ACK: no payload expected */
    if (flags & H2_FLAG_ACK) return 0;

    if (len % 6 != 0) return 1; /* frame size error */

    for (pos = 0; pos + 6 <= len; pos += 6) {
        u16 id = ((u16)payload[pos] << 8) | (u16)payload[pos + 1];
        u32 val = ((u32)payload[pos + 2] << 24) | ((u32)payload[pos + 3] << 16) |
                  ((u32)payload[pos + 4] << 8) | (u32)payload[pos + 5];

        switch (id) {
        case H2_SET_HEADER_TABLE_SIZE:
            conn->header_table_size = val;
            break;
        case H2_SET_MAX_CONCURRENT_STREAMS:
            conn->max_concurrent_streams = val;
            break;
        case H2_SET_INITIAL_WINDOW_SIZE:
            if (val > 0x7FFFFFFF) return 1; /* flow control error */
            conn->initial_window_size = val;
            break;
        case H2_SET_MAX_FRAME_SIZE:
            if (val < 16384 || val > 16777215) return 1;
            conn->max_frame_size = val;
            break;
        default:
            /* Unknown settings are ignored per spec */
            break;
        }
    }

    /* Send SETTINGS ACK */
    return (unsigned long)conn_buffer_frame(conn, H2_FT_SETTINGS, H2_FLAG_ACK,
                                            0, NULL, 0);
}

static unsigned long process_headers(h2_conn *conn, const u8 *payload,
                                      u32 len, u8 flags, u32 stream_id) {
    h2_header *decoded = NULL;
    u64 decoded_count = 0;
    u32 hdr_offset = 0;
    h2_stream *s;
    unsigned long rc;

    if (stream_id == 0) return 1; /* protocol error */

    /* Skip padding and priority if present */
    if (flags & H2_FLAG_PADDED) {
        if (len < 1) return 1;
        u8 pad_len = payload[0];
        hdr_offset += 1;
        if (hdr_offset + pad_len > len) return 1;
        len -= pad_len; /* trim padding from end */
    }
    if (flags & H2_FLAG_PRIORITY) {
        if (hdr_offset + 5 > len) return 1;
        hdr_offset += 5; /* skip stream dependency (4) + weight (1) */
    }

    /* HPACK-decode the header block */
    rc = h2_hpack_decode(&decoded, &decoded_count, conn->hpack_dec,
                         payload + hdr_offset, (u64)(len - hdr_offset));
    if (rc != 0) return 2;

    /* Find or create stream */
    s = conn_find_stream(conn, stream_id);
    if (!s) {
        s = conn_create_stream(conn, stream_id);
        if (!s) { h2_headers_free(decoded, decoded_count); return 3; }
    }

    if (stream_add_headers(s, decoded, decoded_count) != 0) {
        h2_headers_free(decoded, decoded_count);
        return 3;
    }
    h2_headers_free(decoded, decoded_count);

    if (flags & H2_FLAG_END_STREAM) {
        s->state = (s->state == H2_STREAM_HALF_CLOSED_LOCAL)
                   ? H2_STREAM_CLOSED : H2_STREAM_HALF_CLOSED_REMOTE;
    }

    /* Track last peer-initiated stream */
    if (stream_id > conn->last_stream_id) {
        conn->last_stream_id = stream_id;
    }

    return 0;
}

static unsigned long process_data(h2_conn *conn, const u8 *payload,
                                   u32 len, u8 flags, u32 stream_id) {
    h2_stream *s;
    u32 data_offset = 0;
    u32 data_len = len;

    if (stream_id == 0) return 1;

    /* Handle padding */
    if (flags & H2_FLAG_PADDED) {
        if (len < 1) return 1;
        u8 pad_len = payload[0];
        data_offset = 1;
        if (data_offset + pad_len > len) return 1;
        data_len = len - data_offset - pad_len;
    }

    s = conn_find_stream(conn, stream_id);
    if (!s) return 2; /* stream not found, protocol error */

    if (data_len > 0) {
        if (stream_append_body(s, payload + data_offset, data_len) != 0)
            return 3;
    }

    /* Update flow control */
    conn->recv_window -= (i64)len;
    s->recv_window -= (i64)len;

    /* Send WINDOW_UPDATE if window gets low */
    if (conn->recv_window < (i64)(H2_DEFAULT_WINDOW_SIZE / 2)) {
        u32 increment = H2_DEFAULT_WINDOW_SIZE - (u32)conn->recv_window;
        u8 wupd[4];
        wupd[0] = (u8)((increment >> 24) & 0x7F);
        wupd[1] = (u8)((increment >> 16) & 0xFF);
        wupd[2] = (u8)((increment >> 8) & 0xFF);
        wupd[3] = (u8)(increment & 0xFF);
        conn_buffer_frame(conn, H2_FT_WINDOW_UPDATE, 0, 0, wupd, 4);
        conn->recv_window += (i64)increment;
    }

    if (s->recv_window < (i64)(H2_DEFAULT_WINDOW_SIZE / 2)) {
        u32 increment = H2_DEFAULT_WINDOW_SIZE - (u32)s->recv_window;
        u8 wupd[4];
        wupd[0] = (u8)((increment >> 24) & 0x7F);
        wupd[1] = (u8)((increment >> 16) & 0xFF);
        wupd[2] = (u8)((increment >> 8) & 0xFF);
        wupd[3] = (u8)(increment & 0xFF);
        conn_buffer_frame(conn, H2_FT_WINDOW_UPDATE, 0, stream_id, wupd, 4);
        s->recv_window += (i64)increment;
    }

    if (flags & H2_FLAG_END_STREAM) {
        s->state = (s->state == H2_STREAM_HALF_CLOSED_LOCAL)
                   ? H2_STREAM_CLOSED : H2_STREAM_HALF_CLOSED_REMOTE;
    }

    return 0;
}

static unsigned long process_rst_stream(h2_conn *conn, const u8 *payload,
                                         u32 len, u32 stream_id) {
    h2_stream *s;
    if (len != 4 || stream_id == 0) return 1;
    s = conn_find_stream(conn, stream_id);
    if (s) {
        s->error_code = ((u32)payload[0] << 24) | ((u32)payload[1] << 16) |
                        ((u32)payload[2] << 8) | (u32)payload[3];
        s->state = H2_STREAM_CLOSED;
    }
    return 0;
}

static unsigned long process_ping(h2_conn *conn, const u8 *payload,
                                   u32 len, u8 flags) {
    if (len != 8) return 1;
    if (flags & H2_FLAG_ACK) return 0; /* response to our ping, ignore */
    /* Send PING ACK with same opaque data */
    return (unsigned long)conn_buffer_frame(conn, H2_FT_PING, H2_FLAG_ACK,
                                            0, payload, 8);
}

static unsigned long process_goaway(h2_conn *conn, const u8 *payload, u32 len) {
    if (len < 8) return 1;
    conn->goaway_received = 1;
    conn->goaway_last_stream = ((u32)(payload[0] & 0x7F) << 24) |
                               ((u32)payload[1] << 16) |
                               ((u32)payload[2] << 8) |
                               (u32)payload[3];
    conn->goaway_error = ((u32)payload[4] << 24) | ((u32)payload[5] << 16) |
                         ((u32)payload[6] << 8) | (u32)payload[7];
    return 0;
}

static unsigned long process_window_update(h2_conn *conn, const u8 *payload,
                                            u32 len, u32 stream_id) {
    u32 increment;
    if (len != 4) return 1;
    increment = ((u32)(payload[0] & 0x7F) << 24) | ((u32)payload[1] << 16) |
                ((u32)payload[2] << 8) | (u32)payload[3];
    if (increment == 0) return 1; /* protocol error */

    if (stream_id == 0) {
        conn->send_window += (i64)increment;
    } else {
        h2_stream *s = conn_find_stream(conn, stream_id);
        if (s) s->send_window += (i64)increment;
    }
    return 0;
}

/* ================================================================
 *  Connection API
 * ================================================================ */

APENNINES_API unsigned long h2_conn_create(h2_conn **out,
                                            h2_read_fn read_fn,
                                            h2_write_fn write_fn,
                                            void *io_ctx) {
    h2_conn *conn;
    unsigned long rc;
    u8 frame_buf[H2_FRAME_HEADER_SIZE];
    u32 frame_len;
    u8 frame_type, frame_flags;
    u32 frame_stream;
    u8 *settings_payload = NULL;

    if (!out) return 1;
    if (!read_fn) return 2;
    if (!write_fn) return 3;

    conn = (h2_conn *)calloc(1, sizeof(h2_conn));
    if (!conn) return 4;

    conn->read_fn = read_fn;
    conn->write_fn = write_fn;
    conn->io_ctx = io_ctx;
    conn->next_stream_id = 1; /* client: odd */
    conn->max_frame_size = H2_DEFAULT_MAX_FRAME_SIZE;
    conn->initial_window_size = H2_DEFAULT_WINDOW_SIZE;
    conn->max_concurrent_streams = H2_DEFAULT_MAX_CONCURRENT;
    conn->header_table_size = HPACK_DEFAULT_MAX_SIZE;
    conn->recv_window = (i64)H2_DEFAULT_WINDOW_SIZE;
    conn->send_window = (i64)H2_DEFAULT_WINDOW_SIZE;

    /* Allocate stream table */
    conn->stream_cap = CONN_INIT_STREAM_CAP;
    conn->streams = (h2_stream *)calloc(conn->stream_cap, sizeof(h2_stream));
    if (!conn->streams) { free(conn); return 4; }

    /* Allocate outbuf */
    conn->outbuf_cap = CONN_INIT_OUTBUF_CAP;
    conn->outbuf = (u8 *)malloc(conn->outbuf_cap);
    if (!conn->outbuf) { free(conn->streams); free(conn); return 4; }
    conn->outbuf_len = 0;

    /* Create HPACK contexts */
    rc = h2_hpack_ctx_create(&conn->hpack_enc);
    if (rc != 0) {
        free(conn->outbuf); free(conn->streams); free(conn);
        return 4;
    }
    rc = h2_hpack_ctx_create(&conn->hpack_dec);
    if (rc != 0) {
        h2_hpack_ctx_destroy(conn->hpack_enc);
        free(conn->outbuf); free(conn->streams); free(conn);
        return 4;
    }

    /* ---- Connection preface exchange ---- */

    /* 1. Send client connection preface */
    rc = conn_write_all(conn, (const u8 *)H2_CLIENT_PREFACE, H2_CLIENT_PREFACE_LEN);
    if (rc != 0) goto preface_fail;

    /* 2. Send our SETTINGS */
    if (conn_buffer_settings(conn) != 0) goto preface_fail;

    /* Flush outbuf */
    rc = conn_write_all(conn, conn->outbuf, conn->outbuf_len);
    conn->outbuf_len = 0;
    if (rc != 0) goto preface_fail;

    /* 3. Read server's SETTINGS frame */
    rc = conn_read_exact(conn, frame_buf, H2_FRAME_HEADER_SIZE);
    if (rc != 0) goto preface_fail;

    frame_header_read(frame_buf, &frame_len, &frame_type, &frame_flags, &frame_stream);
    if (frame_type != H2_FT_SETTINGS || (frame_flags & H2_FLAG_ACK)) goto preface_fail;

    if (frame_len > 0) {
        settings_payload = (u8 *)malloc(frame_len);
        if (!settings_payload) goto preface_fail;
        rc = conn_read_exact(conn, settings_payload, frame_len);
        if (rc != 0) { free(settings_payload); goto preface_fail; }
        process_settings(conn, settings_payload, frame_len, frame_flags);
        free(settings_payload);
    }

    /* 4. Send SETTINGS ACK (already buffered by process_settings).
     *    Flush the ACK. */
    if (conn->outbuf_len > 0) {
        rc = conn_write_all(conn, conn->outbuf, conn->outbuf_len);
        conn->outbuf_len = 0;
        if (rc != 0) goto preface_fail;
    }

    *out = conn;
    return 0;

preface_fail:
    h2_hpack_ctx_destroy(conn->hpack_enc);
    h2_hpack_ctx_destroy(conn->hpack_dec);
    free(conn->outbuf);
    free(conn->streams);
    free(conn);
    return 5;
}

APENNINES_API unsigned long h2_conn_submit(u32 *out_stream_id, h2_conn *conn,
                                            const char *method, const char *path,
                                            const char *authority,
                                            const h2_header *extra_headers,
                                            u64 extra_count) {
    u64 total_count, i;
    h2_header *all_headers = NULL;
    u8 *encoded = NULL;
    u64 encoded_len = 0;
    u32 stream_id;
    h2_stream *s;
    unsigned long rc;

    if (!out_stream_id) return 1;
    if (!conn) return 2;
    if (!method) return 3;
    if (!path) return 4;

    /* Build pseudo-headers + extra headers */
    total_count = 4 + extra_count; /* :method, :path, :scheme, :authority */
    if (!authority) total_count = 3; /* skip :authority if NULL */

    all_headers = (h2_header *)calloc(total_count, sizeof(h2_header));
    if (!all_headers) return 5;

    i = 0;
    all_headers[i].name = str_dup(":method");
    all_headers[i].value = str_dup(method);
    i++;
    all_headers[i].name = str_dup(":path");
    all_headers[i].value = str_dup(path);
    i++;
    all_headers[i].name = str_dup(":scheme");
    all_headers[i].value = str_dup("https");
    i++;
    if (authority) {
        all_headers[i].name = str_dup(":authority");
        all_headers[i].value = str_dup(authority);
        i++;
    }

    /* Append extra headers */
    for (u64 j = 0; j < extra_count; j++, i++) {
        all_headers[i].name = str_dup(extra_headers[j].name);
        all_headers[i].value = str_dup(extra_headers[j].value);
    }

    /* Check all allocations */
    for (i = 0; i < total_count; i++) {
        if (!all_headers[i].name || !all_headers[i].value) {
            h2_headers_free(all_headers, total_count);
            return 5;
        }
    }

    /* HPACK-encode */
    rc = h2_hpack_encode(&encoded, &encoded_len, conn->hpack_enc,
                         all_headers, total_count);
    h2_headers_free(all_headers, total_count);
    if (rc != 0) return 5;

    /* Allocate stream ID */
    stream_id = conn->next_stream_id;
    conn->next_stream_id += 2;

    /* Create stream */
    s = conn_create_stream(conn, stream_id);
    if (!s) { free(encoded); return 5; }
    s->state = H2_STREAM_OPEN;

    /* Buffer HEADERS frame(s) */
    {
        u64 offset = 0;
        while (offset < encoded_len) {
            u32 chunk = (u32)(encoded_len - offset);
            if (chunk > conn->max_frame_size) chunk = conn->max_frame_size;

            u8 flags = 0;
            u8 type;
            if (offset == 0) {
                type = H2_FT_HEADERS;
                if (offset + chunk >= encoded_len) flags |= H2_FLAG_END_HEADERS;
            } else {
                type = H2_FT_CONTINUATION;
                if (offset + chunk >= encoded_len) flags |= H2_FLAG_END_HEADERS;
            }

            if (conn_buffer_frame(conn, type, flags, stream_id,
                                  encoded + offset, chunk) != 0) {
                free(encoded);
                return 5;
            }
            offset += chunk;
        }
    }

    free(encoded);
    *out_stream_id = stream_id;
    return 0;
}

APENNINES_API unsigned long h2_conn_send(h2_conn *conn) {
    unsigned long rc;
    if (!conn) return 1;
    if (conn->outbuf_len == 0) return 0;
    rc = conn_write_all(conn, conn->outbuf, conn->outbuf_len);
    conn->outbuf_len = 0;
    return rc != 0 ? 2 : 0;
}

APENNINES_API unsigned long h2_conn_recv(h2_conn *conn) {
    u8 frame_buf[H2_FRAME_HEADER_SIZE];
    u32 frame_len;
    u8 frame_type, frame_flags;
    u32 frame_stream;
    u8 *payload = NULL;
    unsigned long rc;

    if (!conn) return 1;

    /* Read one frame */
    rc = conn_read_exact(conn, frame_buf, H2_FRAME_HEADER_SIZE);
    if (rc != 0) return 2;

    frame_header_read(frame_buf, &frame_len, &frame_type, &frame_flags, &frame_stream);

    /* Validate frame size */
    if (frame_len > conn->max_frame_size && frame_type != H2_FT_SETTINGS) {
        return 3; /* FRAME_SIZE_ERROR */
    }

    /* Read payload */
    if (frame_len > 0) {
        payload = (u8 *)malloc(frame_len);
        if (!payload) return 4;
        rc = conn_read_exact(conn, payload, frame_len);
        if (rc != 0) { free(payload); return 2; }
    }

    /* Dispatch by frame type */
    switch (frame_type) {
    case H2_FT_DATA:
        rc = process_data(conn, payload, frame_len, frame_flags, frame_stream);
        break;
    case H2_FT_HEADERS:
        rc = process_headers(conn, payload, frame_len, frame_flags, frame_stream);
        break;
    case H2_FT_PRIORITY:
        /* Ignored — advisory only */
        rc = 0;
        break;
    case H2_FT_RST_STREAM:
        rc = process_rst_stream(conn, payload, frame_len, frame_stream);
        break;
    case H2_FT_SETTINGS:
        rc = process_settings(conn, payload, frame_len, frame_flags);
        break;
    case H2_FT_PUSH_PROMISE:
        /* Not implemented for client: ignore or RST */
        rc = 0;
        break;
    case H2_FT_PING:
        rc = process_ping(conn, payload, frame_len, frame_flags);
        break;
    case H2_FT_GOAWAY:
        rc = process_goaway(conn, payload, frame_len);
        break;
    case H2_FT_WINDOW_UPDATE:
        rc = process_window_update(conn, payload, frame_len, frame_stream);
        break;
    case H2_FT_CONTINUATION:
        /* Should be handled as part of HEADERS sequence; standalone is error */
        rc = 0;
        break;
    default:
        /* Unknown frame types are ignored per spec */
        rc = 0;
        break;
    }

    free(payload);
    return rc != 0 ? 5 : 0;
}

APENNINES_API unsigned long h2_conn_read_response(h2_response *out,
                                                   h2_conn *conn, u32 stream_id) {
    h2_stream *s;
    u64 i;

    if (!out) return 1;
    if (!conn) return 2;

    s = conn_find_stream(conn, stream_id);
    if (!s) return 3;
    if (s->error_code != 0) return 4;

    memset(out, 0, sizeof(h2_response));
    out->stream_id = s->id;
    out->status = s->status;

    /* Copy headers */
    if (s->header_count > 0) {
        out->headers = (h2_header *)calloc(s->header_count, sizeof(h2_header));
        if (!out->headers) return 5;
        for (i = 0; i < s->header_count; i++) {
            out->headers[i].name = str_dup(s->headers[i].name);
            out->headers[i].value = str_dup(s->headers[i].value);
            if (!out->headers[i].name || !out->headers[i].value) {
                h2_headers_free(out->headers, i + 1);
                out->headers = NULL;
                return 5;
            }
        }
        out->header_count = s->header_count;
    }

    /* Copy body */
    if (s->body_len > 0) {
        out->body = (u8 *)malloc(s->body_len);
        if (!out->body) {
            h2_headers_free(out->headers, out->header_count);
            out->headers = NULL;
            return 5;
        }
        memcpy(out->body, s->body, s->body_len);
        out->body_len = s->body_len;
    }

    return 0;
}

APENNINES_API unsigned long h2_conn_send_data(h2_conn *conn, u32 stream_id,
                                               const u8 *data, u64 len,
                                               int end_stream) {
    h2_stream *s;
    u64 offset;

    if (!conn) return 1;
    if (stream_id == 0) return 2;

    s = conn_find_stream(conn, stream_id);
    if (!s) return 3;
    if (s->state == H2_STREAM_CLOSED ||
        s->state == H2_STREAM_HALF_CLOSED_LOCAL) return 4;

    /* Fragment into max_frame_size chunks */
    offset = 0;
    while (offset < len || (offset == 0 && len == 0)) {
        u32 chunk = (u32)(len - offset);
        if (chunk > conn->max_frame_size) chunk = conn->max_frame_size;

        u8 flags = 0;
        if (end_stream && offset + chunk >= len) flags |= H2_FLAG_END_STREAM;

        if (conn_buffer_frame(conn, H2_FT_DATA, flags, stream_id,
                              data ? data + offset : NULL, chunk) != 0)
            return 5;

        offset += chunk;
        if (len == 0) break; /* empty DATA frame case */
    }

    if (end_stream) {
        s->state = (s->state == H2_STREAM_HALF_CLOSED_REMOTE)
                   ? H2_STREAM_CLOSED : H2_STREAM_HALF_CLOSED_LOCAL;
    }

    return 0;
}

APENNINES_API unsigned long h2_conn_close_stream(h2_conn *conn, u32 stream_id) {
    h2_stream *s;
    u8 payload[4];

    if (!conn) return 1;
    if (stream_id == 0) return 2;

    s = conn_find_stream(conn, stream_id);
    if (!s) return 3;

    /* Send RST_STREAM with NO_ERROR */
    payload[0] = 0; payload[1] = 0; payload[2] = 0; payload[3] = 0;
    if (conn_buffer_frame(conn, H2_FT_RST_STREAM, 0, stream_id, payload, 4) != 0)
        return 4;

    s->state = H2_STREAM_CLOSED;
    return 0;
}

APENNINES_API unsigned long h2_conn_goaway(h2_conn *conn, u32 error_code) {
    u8 payload[8];

    if (!conn) return 1;

    /* Last stream ID we've processed */
    payload[0] = (u8)((conn->last_stream_id >> 24) & 0x7F);
    payload[1] = (u8)((conn->last_stream_id >> 16) & 0xFF);
    payload[2] = (u8)((conn->last_stream_id >> 8) & 0xFF);
    payload[3] = (u8)(conn->last_stream_id & 0xFF);
    /* Error code */
    payload[4] = (u8)((error_code >> 24) & 0xFF);
    payload[5] = (u8)((error_code >> 16) & 0xFF);
    payload[6] = (u8)((error_code >> 8) & 0xFF);
    payload[7] = (u8)(error_code & 0xFF);

    if (conn_buffer_frame(conn, H2_FT_GOAWAY, 0, 0, payload, 8) != 0) return 2;
    return 0;
}

APENNINES_API unsigned long h2_conn_ping(h2_conn *conn) {
    u8 payload[8];

    if (!conn) return 1;

    /* Opaque data: use a simple counter-like pattern */
    memset(payload, 0, 8);
    payload[0] = 'P'; payload[1] = 'I'; payload[2] = 'N'; payload[3] = 'G';

    if (conn_buffer_frame(conn, H2_FT_PING, 0, 0, payload, 8) != 0) return 2;
    return 0;
}

APENNINES_API unsigned long h2_conn_destroy(h2_conn *conn) {
    u64 i;
    if (!conn) return 1;

    /* Free all streams */
    for (i = 0; i < conn->stream_count; i++) {
        stream_free_contents(&conn->streams[i]);
    }
    free(conn->streams);

    /* Free HPACK contexts */
    h2_hpack_ctx_destroy(conn->hpack_enc);
    h2_hpack_ctx_destroy(conn->hpack_dec);

    /* Free outbuf */
    free(conn->outbuf);
    free(conn);
    return 0;
}

APENNINES_API unsigned long h2_response_free(h2_response *resp) {
    u64 i;
    if (!resp) return 1;
    for (i = 0; i < resp->header_count; i++) {
        free(resp->headers[i].name);
        free(resp->headers[i].value);
    }
    free(resp->headers);
    free(resp->body);
    /* Zero out the struct but don't free it — it's caller-owned */
    memset(resp, 0, sizeof(h2_response));
    return 0;
}
