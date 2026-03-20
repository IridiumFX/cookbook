#include "apennines/t2/compress/compress.h"
#include "apennines/t1/buffer/buf.h"
#include <string.h>
#include <stdlib.h>

/* ================================================================
 *  LZ4 Block Compress
 *
 *  Format per sequence:
 *    token byte: high nibble = literal length, low nibble = match length - 4
 *    if literal length >= 15: additional bytes (255... then remainder)
 *    literal bytes
 *    2-byte little-endian match offset (not present on last sequence if no match)
 *    if match length - 4 >= 15: additional bytes (255... then remainder)
 *
 *  Uses a 4096-slot hash table for match finding.
 *  Minimum match length = 4.
 * ================================================================ */

#define LZ4_HASH_BITS    12
#define LZ4_HASH_SIZE    (1 << LZ4_HASH_BITS)
#define LZ4_MIN_MATCH    4
#define LZ4_MAX_OFFSET   65535

static u32 lz4_hash(u32 val) {
    return (val * 2654435761u) >> (32 - LZ4_HASH_BITS);
}

static u32 lz4_read32(const u8 *p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

static unsigned long lz4_write_len(buf *out, u64 length) {
    unsigned long rc;
    while (length >= 255) {
        u8 b = 255;
        rc = buf_append_byte(out, b);
        if (rc) return rc;
        length -= 255;
    }
    {
        u8 b = (u8)length;
        rc = buf_append_byte(out, b);
        if (rc) return rc;
    }
    return 0;
}

unsigned long lz4_compress(buf *out, u8 *data, u64 len) {
    u32 hash_table[LZ4_HASH_SIZE];
    u64 anchor;
    u64 ip;
    unsigned long rc;

    if (!out) return 1;
    if (len > 0 && !data) return 2;

    if (len == 0) return 0;

    memset(hash_table, 0, sizeof(hash_table));
    anchor = 0;
    ip = 0;

    /* We need at least LZ4_MIN_MATCH bytes remaining to attempt matching.
       The last 5 bytes are always emitted as literals (LZ4 safety margin). */
    while (ip + LZ4_MIN_MATCH + 1 <= len) {
        u32 h = lz4_hash(lz4_read32(data + ip));
        u64 ref = hash_table[h];
        hash_table[h] = (u32)ip;

        /* Check match: same 4 bytes, within max offset, and ref is valid */
        if (ref > 0 && ip - ref <= LZ4_MAX_OFFSET &&
            ip - ref > 0 &&
            lz4_read32(data + ref) == lz4_read32(data + ip)) {
            /* Extend match forward */
            u64 match_len = LZ4_MIN_MATCH;
            while (ip + match_len < len && data[ref + match_len] == data[ip + match_len]) {
                match_len++;
            }

            /* Emit sequence */
            {
                u64 lit_len = ip - anchor;
                u64 ml = match_len - LZ4_MIN_MATCH;
                u8 token;
                u16 offset;

                /* Token byte */
                token = (u8)(((lit_len < 15 ? lit_len : 15) << 4) |
                              (ml < 15 ? ml : 15));
                rc = buf_append_byte(out, token);
                if (rc) return 3;

                /* Extra literal length */
                if (lit_len >= 15) {
                    rc = lz4_write_len(out, lit_len - 15);
                    if (rc) return 3;
                }

                /* Literals */
                if (lit_len > 0) {
                    rc = buf_append(out, data + anchor, lit_len);
                    if (rc) return 3;
                }

                /* Offset (little-endian) */
                offset = (u16)(ip - ref);
                {
                    u8 off_bytes[2];
                    off_bytes[0] = (u8)(offset & 0xFF);
                    off_bytes[1] = (u8)((offset >> 8) & 0xFF);
                    rc = buf_append(out, off_bytes, 2);
                    if (rc) return 3;
                }

                /* Extra match length */
                if (ml >= 15) {
                    rc = lz4_write_len(out, ml - 15);
                    if (rc) return 3;
                }
            }

            ip += match_len;
            anchor = ip;
        } else {
            ip++;
        }
    }

    /* Emit remaining literals (last sequence, no match) */
    {
        u64 lit_len = len - anchor;
        if (lit_len > 0) {
            u8 token = (u8)((lit_len < 15 ? lit_len : 15) << 4);
            rc = buf_append_byte(out, token);
            if (rc) return 3;

            if (lit_len >= 15) {
                rc = lz4_write_len(out, lit_len - 15);
                if (rc) return 3;
            }

            rc = buf_append(out, data + anchor, lit_len);
            if (rc) return 3;
        }
    }

    return 0;
}

/* ================================================================
 *  LZ4 Block Decompress
 * ================================================================ */

unsigned long lz4_decompress(buf *out, u8 *data, u64 len) {
    u64 pos;
    unsigned long rc;

    if (!out) return 1;
    if (len > 0 && !data) return 2;

    if (len == 0) return 0;

    pos = 0;

    while (pos < len) {
        u8 token;
        u64 lit_len;
        u64 match_len;

        /* Read token */
        token = data[pos++];
        lit_len = (token >> 4) & 0x0F;

        /* Extended literal length */
        if (lit_len == 15) {
            u8 extra;
            do {
                if (pos >= len) return 3;
                extra = data[pos++];
                lit_len += extra;
            } while (extra == 255);
        }

        /* Copy literals */
        if (lit_len > 0) {
            if (pos + lit_len > len) return 3;
            rc = buf_append(out, data + pos, lit_len);
            if (rc) return 4;
            pos += lit_len;
        }

        /* Check if this was the last sequence (no match part) */
        if (pos >= len) break;

        /* Read offset */
        if (pos + 2 > len) return 3;
        {
            u16 offset = (u16)(data[pos] | ((u16)data[pos + 1] << 8));
            pos += 2;

            if (offset == 0) return 3;

            match_len = (token & 0x0F) + LZ4_MIN_MATCH;

            /* Extended match length */
            if ((token & 0x0F) == 15) {
                u8 extra;
                do {
                    if (pos >= len) return 3;
                    extra = data[pos++];
                    match_len += extra;
                } while (extra == 255);
            }

            /* Copy match from output buffer (may overlap) */
            {
                u64 match_start;
                u64 i;
                if (out->len < offset) return 3;
                match_start = out->len - offset;
                for (i = 0; i < match_len; i++) {
                    /* Must read byte-by-byte because match may overlap */
                    u8 b = out->data[match_start + i];
                    rc = buf_append_byte(out, b);
                    if (rc) return 4;
                }
            }
        }
    }

    return 0;
}

/* ================================================================
 *  Deflate Compress (Fixed Huffman only, RFC 1951)
 *
 *  Uses fixed Huffman codes for simplicity:
 *    Lit 0-143:   8 bits  00110000 - 10111111
 *    Lit 144-255: 9 bits  110010000 - 111111111
 *    Lit 256:     7 bits  0000000
 *    Lengths 257-285: 7-8 bits
 *    Distances:   5 bits (fixed codes 0-29)
 *
 *  Hash-chain match finding with 32K window.
 * ================================================================ */

/* Bit-writer state for deflate */
typedef struct {
    buf *out;
    u32 bits;
    u32 nbits;
} bit_writer;

static void bw_init(bit_writer *bw, buf *out) {
    bw->out = out;
    bw->bits = 0;
    bw->nbits = 0;
}

static unsigned long bw_write(bit_writer *bw, u32 value, u32 count) {
    unsigned long rc;
    bw->bits |= (value << bw->nbits);
    bw->nbits += count;
    while (bw->nbits >= 8) {
        u8 b = (u8)(bw->bits & 0xFF);
        rc = buf_append_byte(bw->out, b);
        if (rc) return rc;
        bw->bits >>= 8;
        bw->nbits -= 8;
    }
    return 0;
}

static unsigned long bw_flush(bit_writer *bw) {
    unsigned long rc;
    if (bw->nbits > 0) {
        u8 b = (u8)(bw->bits & 0xFF);
        rc = buf_append_byte(bw->out, b);
        if (rc) return rc;
        bw->bits = 0;
        bw->nbits = 0;
    }
    return 0;
}

/* Write a fixed Huffman literal/length code (RFC 1951 section 3.2.6).
   Codes are written LSB-first. The fixed code values are:
     0-143:   8 bits, codes 0x30-0xBF  (reversed)
     144-255: 9 bits, codes 0x190-0x1FF (reversed)
     256-279: 7 bits, codes 0x00-0x17   (reversed)
     280-287: 8 bits, codes 0xC0-0xC7   (reversed)
*/

static u32 reverse_bits(u32 val, u32 nbits) {
    u32 result = 0;
    u32 i;
    for (i = 0; i < nbits; i++) {
        result = (result << 1) | (val & 1);
        val >>= 1;
    }
    return result;
}

static unsigned long deflate_write_fixed_code(bit_writer *bw, u32 symbol) {
    if (symbol <= 143) {
        /* 8 bits: code = 0x30 + symbol */
        return bw_write(bw, reverse_bits(0x30 + symbol, 8), 8);
    } else if (symbol <= 255) {
        /* 9 bits: code = 0x190 + (symbol - 144) */
        return bw_write(bw, reverse_bits(0x190 + (symbol - 144), 9), 9);
    } else if (symbol <= 279) {
        /* 7 bits: code = 0x00 + (symbol - 256) */
        return bw_write(bw, reverse_bits(0x00 + (symbol - 256), 7), 7);
    } else {
        /* 8 bits: code = 0xC0 + (symbol - 280) */
        return bw_write(bw, reverse_bits(0xC0 + (symbol - 280), 8), 8);
    }
}

/* Length encoding tables (RFC 1951 section 3.2.5) */
static const u16 len_base[] = {
    3,4,5,6,7,8,9,10, 11,13,15,17, 19,23,27,31,
    35,43,51,59, 67,83,99,115, 131,163,195,227, 258
};
static const u8 len_extra[] = {
    0,0,0,0,0,0,0,0, 1,1,1,1, 2,2,2,2,
    3,3,3,3, 4,4,4,4, 5,5,5,5, 0
};

/* Distance encoding tables */
static const u16 dist_base[] = {
    1,2,3,4, 5,7,9,13, 17,25,33,49, 65,97,129,193,
    257,385,513,769, 1025,1537,2049,3073, 4097,6145,8193,12289,
    16385,24577
};
static const u8 dist_extra[] = {
    0,0,0,0, 1,1,2,2, 3,3,4,4, 5,5,6,6,
    7,7,8,8, 9,9,10,10, 11,11,12,12, 13,13
};

static u32 deflate_find_len_code(u32 length) {
    u32 i;
    for (i = 0; i < 29; i++) {
        u32 top = (i < 28) ? len_base[i + 1] : 259;
        if (length < top) return i;
    }
    return 28;
}

static u32 deflate_find_dist_code(u32 distance) {
    u32 i;
    for (i = 0; i < 30; i++) {
        u32 top = (i < 29) ? dist_base[i + 1] : 32769;
        if (distance < top) return i;
    }
    return 29;
}

#define DEFLATE_HASH_BITS  15
#define DEFLATE_HASH_SIZE  (1 << DEFLATE_HASH_BITS)
#define DEFLATE_WINDOW     32768
#define DEFLATE_MIN_MATCH  3
#define DEFLATE_MAX_MATCH  258

static u32 deflate_hash(u32 val) {
    return (val * 2654435761u) >> (32 - DEFLATE_HASH_BITS);
}

unsigned long deflate_compress(buf *out, u8 *data, u64 len, compress_level level) {
    bit_writer bw;
    u32 *hash_table;
    u64 ip, anchor;
    unsigned long rc;
    u32 max_chain;

    if (!out) return 1;
    if (len > 0 && !data) return 2;

    /* Use level to tune search depth */
    if ((int)level <= 1) {
        max_chain = 1;
    } else if ((int)level <= 6) {
        max_chain = 4;
    } else {
        max_chain = 16;
    }
    (void)max_chain; /* single-entry hash, chain depth not used yet */

    bw_init(&bw, out);

    /* BFINAL=1, BTYPE=01 (fixed Huffman) */
    rc = bw_write(&bw, 1, 1);  /* BFINAL */
    if (rc) return 3;
    rc = bw_write(&bw, 1, 2);  /* BTYPE = 01 fixed */
    if (rc) return 3;

    if (len == 0) {
        /* Just write end-of-block */
        rc = deflate_write_fixed_code(&bw, 256);
        if (rc) return 3;
        rc = bw_flush(&bw);
        if (rc) return 3;
        return 0;
    }

    hash_table = (u32 *)calloc(DEFLATE_HASH_SIZE, sizeof(u32));
    if (!hash_table) return 4;

    /* We store ip+1 in hash_table so that 0 remains the "empty" sentinel.
       When reading back we subtract 1 to recover the real position. */

    ip = 0;

    while (ip < len) {
        u64 best_len = 0;
        u64 best_dist = 0;

        /* Try to find a match (need at least 3 bytes remaining) */
        if (ip + DEFLATE_MIN_MATCH <= len) {
            u32 h = deflate_hash(lz4_read32(data + ip) & 0xFFFFFF);
            u64 raw = hash_table[h];
            hash_table[h] = (u32)(ip + 1);

            if (raw > 0) {
                u64 ref = raw - 1;
                if (ip > ref && ip - ref <= DEFLATE_WINDOW) {
                    u64 ml = 0;
                    while (ip + ml < len && ml < DEFLATE_MAX_MATCH &&
                           data[ref + ml] == data[ip + ml]) {
                        ml++;
                    }
                    if (ml >= DEFLATE_MIN_MATCH) {
                        best_len = ml;
                        best_dist = ip - ref;
                    }
                }
            }
        }

        if (best_len >= DEFLATE_MIN_MATCH) {
            /* Emit length/distance pair */
            u32 lc = deflate_find_len_code((u32)best_len);
            u32 dc = deflate_find_dist_code((u32)best_dist);

            /* Length code (symbol 257 + lc) */
            rc = deflate_write_fixed_code(&bw, 257 + lc);
            if (rc) { free(hash_table); return 3; }

            /* Length extra bits */
            if (len_extra[lc] > 0) {
                rc = bw_write(&bw, (u32)(best_len - len_base[lc]), len_extra[lc]);
                if (rc) { free(hash_table); return 3; }
            }

            /* Distance code (5 bits, reversed) */
            rc = bw_write(&bw, reverse_bits(dc, 5), 5);
            if (rc) { free(hash_table); return 3; }

            /* Distance extra bits */
            if (dist_extra[dc] > 0) {
                rc = bw_write(&bw, (u32)(best_dist - dist_base[dc]), dist_extra[dc]);
                if (rc) { free(hash_table); return 3; }
            }

            /* Update hash for skipped positions */
            {
                u64 j;
                for (j = 1; j < best_len && ip + j + DEFLATE_MIN_MATCH <= len; j++) {
                    u32 hh = deflate_hash(lz4_read32(data + ip + j) & 0xFFFFFF);
                    hash_table[hh] = (u32)(ip + j + 1);
                }
            }

            ip += best_len;
        } else {
            /* Emit literal */
            rc = deflate_write_fixed_code(&bw, data[ip]);
            if (rc) { free(hash_table); return 3; }
            ip++;
        }
    }

    /* End of block */
    rc = deflate_write_fixed_code(&bw, 256);
    if (rc) { free(hash_table); return 3; }

    rc = bw_flush(&bw);
    if (rc) { free(hash_table); return 3; }

    free(hash_table);
    return 0;
}

/* ================================================================
 *  Deflate Decompress (RFC 1951, fixed Huffman only)
 * ================================================================ */

typedef struct {
    u8  *data;
    u64  len;
    u64  pos;
    u32  bits;
    u32  nbits;
} bit_reader;

static void br_init(bit_reader *br, u8 *data, u64 len) {
    br->data = data;
    br->len = len;
    br->pos = 0;
    br->bits = 0;
    br->nbits = 0;
}

static unsigned long br_read(bit_reader *br, u32 count, u32 *result) {
    while (br->nbits < count) {
        if (br->pos >= br->len) return 1;
        br->bits |= ((u32)br->data[br->pos++]) << br->nbits;
        br->nbits += 8;
    }
    *result = br->bits & ((1u << count) - 1);
    br->bits >>= count;
    br->nbits -= count;
    return 0;
}

/* Decode a fixed Huffman literal/length symbol.
   We read bits LSB-first and decode:
     7-bit codes 0000000-0010111 -> symbols 256-279
     8-bit codes 00110000-10111111 -> symbols 0-143
     8-bit codes 11000000-11000111 -> symbols 280-287
     9-bit codes 110010000-111111111 -> symbols 144-255
*/
static unsigned long deflate_decode_fixed_symbol(bit_reader *br, u32 *sym) {
    u32 code;
    unsigned long rc;

    /* Read 7 bits */
    rc = br_read(br, 7, &code);
    if (rc) return rc;

    code = reverse_bits(code, 7);

    if (code <= 0x17) {
        /* 7-bit code: symbols 256-279 */
        *sym = 256 + code;
        return 0;
    }

    /* Need 1 more bit (total 8) */
    {
        u32 extra;
        rc = br_read(br, 1, &extra);
        if (rc) return rc;
        code = (code << 1) | extra;
    }

    if (code >= 0x30 && code <= 0xBF) {
        *sym = code - 0x30;
        return 0;
    }
    if (code >= 0xC0 && code <= 0xC7) {
        *sym = 280 + (code - 0xC0);
        return 0;
    }

    /* Need 1 more bit (total 9) */
    {
        u32 extra;
        rc = br_read(br, 1, &extra);
        if (rc) return rc;
        code = (code << 1) | extra;
    }

    if (code >= 0x190 && code <= 0x1FF) {
        *sym = 144 + (code - 0x190);
        return 0;
    }

    return 1; /* invalid code */
}

unsigned long deflate_decompress(buf *out, u8 *data, u64 len) {
    bit_reader br;
    u32 bfinal;
    unsigned long rc;

    if (!out) return 1;
    if (len > 0 && !data) return 2;

    if (len == 0) return 0;

    br_init(&br, data, len);

    do {
        u32 btype;

        rc = br_read(&br, 1, &bfinal);
        if (rc) return 3;

        rc = br_read(&br, 2, &btype);
        if (rc) return 3;

        if (btype == 0) {
            /* Stored block (no compression) */
            u32 block_len, nlen;
            /* Discard remaining bits in current byte */
            br.bits = 0;
            br.nbits = 0;

            if (br.pos + 4 > br.len) return 3;
            block_len = (u32)(br.data[br.pos] | ((u32)br.data[br.pos + 1] << 8));
            nlen = (u32)(br.data[br.pos + 2] | ((u32)br.data[br.pos + 3] << 8));
            br.pos += 4;

            if ((block_len ^ 0xFFFF) != nlen) return 3;
            if (br.pos + block_len > br.len) return 3;

            rc = buf_append(out, br.data + br.pos, block_len);
            if (rc) return 4;
            br.pos += block_len;
        } else if (btype == 1) {
            /* Fixed Huffman codes */
            for (;;) {
                u32 sym;
                rc = deflate_decode_fixed_symbol(&br, &sym);
                if (rc) return 3;

                if (sym < 256) {
                    u8 b = (u8)sym;
                    rc = buf_append_byte(out, b);
                    if (rc) return 4;
                } else if (sym == 256) {
                    break; /* end of block */
                } else {
                    /* Length/distance pair */
                    u32 lc = sym - 257;
                    u32 length;
                    u32 dc_bits, dc;
                    u32 distance;

                    if (lc >= 29) return 3;

                    length = len_base[lc];
                    if (len_extra[lc] > 0) {
                        u32 extra;
                        rc = br_read(&br, len_extra[lc], &extra);
                        if (rc) return 3;
                        length += extra;
                    }

                    /* Distance: 5 bits fixed code, reversed */
                    rc = br_read(&br, 5, &dc_bits);
                    if (rc) return 3;
                    dc = reverse_bits(dc_bits, 5);

                    if (dc >= 30) return 3;
                    distance = dist_base[dc];
                    if (dist_extra[dc] > 0) {
                        u32 extra;
                        rc = br_read(&br, dist_extra[dc], &extra);
                        if (rc) return 3;
                        distance += extra;
                    }

                    /* Copy from output, byte-by-byte for overlap */
                    if (out->len < distance) return 3;
                    {
                        u64 src_pos = out->len - distance;
                        u32 i;
                        for (i = 0; i < length; i++) {
                            u8 b = out->data[src_pos + i];
                            rc = buf_append_byte(out, b);
                            if (rc) return 4;
                        }
                    }
                }
            }
        } else if (btype == 2) {
            /* Dynamic Huffman — not supported in this implementation */
            return 5;
        } else {
            /* Reserved block type */
            return 3;
        }
    } while (!bfinal);

    return 0;
}
