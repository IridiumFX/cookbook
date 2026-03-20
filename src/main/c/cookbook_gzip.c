/*
 * cookbook_gzip.c — gzip compression wrapper for HTTP responses
 *
 * Wraps apennines deflate_compress with the standard gzip framing
 * (RFC 1952): 10-byte header + deflate payload + 4-byte CRC-32 + 4-byte size.
 */

#include "cookbook.h"
#include <apennines/t2/compress/compress.h>
#include <apennines/t1/buffer/buf.h>

#include <stdlib.h>
#include <string.h>

/* CRC-32 (IEEE 802.3, same as gzip/zlib) */
static uint32_t crc32_table[256];
static int crc32_table_init = 0;

static void crc32_init_table(void) {
    if (crc32_table_init) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0);
        crc32_table[i] = c;
    }
    crc32_table_init = 1;
}

static uint32_t crc32_compute(const void *data, size_t len) {
    crc32_init_table();
    const unsigned char *p = (const unsigned char *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* Compress data with gzip framing.
   Returns malloc'd buffer, sets *out_len. Returns NULL on failure. */
void *cookbook_gzip_compress(const void *data, size_t len, size_t *out_len) {
    if (!data || len == 0 || !out_len) return NULL;

    /* deflate compress */
    buf deflated;
    memset(&deflated, 0, sizeof(deflated));
    if (deflate_compress(&deflated, (u8 *)data, (u64)len,
                          COMPRESS_LEVEL_DEFAULT) != 0)
        return NULL;

    /* gzip = header(10) + deflated + crc32(4) + size(4) */
    size_t total = 10 + deflated.len + 8;
    unsigned char *out = malloc(total);
    if (!out) { free(deflated.data); return NULL; }

    /* gzip header (RFC 1952) */
    out[0] = 0x1F; /* ID1 */
    out[1] = 0x8B; /* ID2 */
    out[2] = 0x08; /* CM = deflate */
    out[3] = 0x00; /* FLG = none */
    out[4] = out[5] = out[6] = out[7] = 0x00; /* MTIME */
    out[8] = 0x00; /* XFL */
    out[9] = 0xFF; /* OS = unknown */

    /* deflated data */
    memcpy(out + 10, deflated.data, deflated.len);
    free(deflated.data);

    /* CRC-32 of original data (little-endian) */
    uint32_t crc = crc32_compute(data, len);
    size_t off = 10 + deflated.len;
    out[off++] = (unsigned char)(crc & 0xFF);
    out[off++] = (unsigned char)((crc >> 8) & 0xFF);
    out[off++] = (unsigned char)((crc >> 16) & 0xFF);
    out[off++] = (unsigned char)((crc >> 24) & 0xFF);

    /* original size (little-endian, mod 2^32) */
    uint32_t sz = (uint32_t)(len & 0xFFFFFFFF);
    out[off++] = (unsigned char)(sz & 0xFF);
    out[off++] = (unsigned char)((sz >> 8) & 0xFF);
    out[off++] = (unsigned char)((sz >> 16) & 0xFF);
    out[off++] = (unsigned char)((sz >> 24) & 0xFF);

    *out_len = total;
    return out;
}
