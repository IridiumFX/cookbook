#include "apennines/t2/crypto/blake2b.h"
#include <string.h>

/* RFC 7693 BLAKE2b. Constants and compression function taken verbatim
 * from the RFC. */

static const u64 blake2b_iv[8] = {
    0x6A09E667F3BCC908ULL, 0xBB67AE8584CAA73BULL,
    0x3C6EF372FE94F82BULL, 0xA54FF53A5F1D36F1ULL,
    0x510E527FADE682D1ULL, 0x9B05688C2B3E6C1FULL,
    0x1F83D9ABFB41BD6BULL, 0x5BE0CD19137E2179ULL
};

static const u8 blake2b_sigma[12][16] = {
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
    { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
    {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
    {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
    {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
    { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
    { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
    {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
    { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
    /* Rounds 11 and 12 repeat rounds 1 and 2 (per RFC 7693 §3.2). */
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 }
};

static u64 rotr64(u64 x, int n) {
    return (x >> n) | (x << (64 - n));
}

static u64 load_u64_le(const u8 *p) {
    return ((u64)p[0])       | ((u64)p[1] <<  8) |
           ((u64)p[2] << 16) | ((u64)p[3] << 24) |
           ((u64)p[4] << 32) | ((u64)p[5] << 40) |
           ((u64)p[6] << 48) | ((u64)p[7] << 56);
}

static void store_u64_le(u8 *p, u64 v) {
    p[0] = (u8)(v);       p[1] = (u8)(v >>  8);
    p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
    p[4] = (u8)(v >> 32); p[5] = (u8)(v >> 40);
    p[6] = (u8)(v >> 48); p[7] = (u8)(v >> 56);
}

#define G(r, i, a, b, c, d)                                     \
    do {                                                        \
        a = a + b + m[blake2b_sigma[r][2 * (i) + 0]];           \
        d = rotr64(d ^ a, 32);                                  \
        c = c + d;                                              \
        b = rotr64(b ^ c, 24);                                  \
        a = a + b + m[blake2b_sigma[r][2 * (i) + 1]];           \
        d = rotr64(d ^ a, 16);                                  \
        c = c + d;                                              \
        b = rotr64(b ^ c, 63);                                  \
    } while (0)

static void blake2b_compress(blake2b_state *s, const u8 block[BLAKE2B_BLOCKBYTES]) {
    u64 m[16];
    u64 v[16];
    int i, r;

    for (i = 0; i < 16; i++) m[i] = load_u64_le(block + i * 8);

    for (i = 0; i < 8; i++) v[i]     = s->h[i];
    for (i = 0; i < 8; i++) v[i + 8] = blake2b_iv[i];
    v[12] ^= s->t[0];
    v[13] ^= s->t[1];
    v[14] ^= s->f[0];
    v[15] ^= s->f[1];

    for (r = 0; r < 12; r++) {
        G(r, 0, v[0], v[4], v[ 8], v[12]);
        G(r, 1, v[1], v[5], v[ 9], v[13]);
        G(r, 2, v[2], v[6], v[10], v[14]);
        G(r, 3, v[3], v[7], v[11], v[15]);
        G(r, 4, v[0], v[5], v[10], v[15]);
        G(r, 5, v[1], v[6], v[11], v[12]);
        G(r, 6, v[2], v[7], v[ 8], v[13]);
        G(r, 7, v[3], v[4], v[ 9], v[14]);
    }

    for (i = 0; i < 8; i++) s->h[i] ^= v[i] ^ v[i + 8];
}

unsigned long blake2b_init(blake2b_state *s, u32 outlen) {
    int i;
    if (!s) return 1;
    if (outlen == 0 || outlen > BLAKE2B_OUTBYTES) return 2;

    for (i = 0; i < 8; i++) s->h[i] = blake2b_iv[i];

    /* Parameter block XOR for an unkeyed BLAKE2b:
     *   param[0] = 0x0000_0000_0101_00NN where NN = outlen. */
    s->h[0] ^= 0x0000000001010000ULL | (u64)outlen;

    s->t[0] = 0;
    s->t[1] = 0;
    s->f[0] = 0;
    s->f[1] = 0;
    s->buflen = 0;
    s->outlen = outlen;
    memset(s->buf, 0, BLAKE2B_BLOCKBYTES);
    return 0;
}

unsigned long blake2b_update(blake2b_state *s, const u8 *data, u64 len) {
    if (!s) return 1;
    if (len > 0 && !data) return 2;
    if (s->f[0] != 0) return 3; /* already finalised */

    while (len > 0) {
        u64 fill = BLAKE2B_BLOCKBYTES - s->buflen;
        if (len <= fill) {
            memcpy(s->buf + s->buflen, data, (size_t)len);
            s->buflen += len;
            return 0;
        }
        /* More data after filling the buffer — compress as non-final. */
        memcpy(s->buf + s->buflen, data, (size_t)fill);
        data += fill;
        len  -= fill;
        s->t[0] += BLAKE2B_BLOCKBYTES;
        if (s->t[0] < (u64)BLAKE2B_BLOCKBYTES) s->t[1]++;
        blake2b_compress(s, s->buf);
        s->buflen = 0;
    }
    return 0;
}

unsigned long blake2b_final(u8 *out, blake2b_state *s) {
    u8 tmp[BLAKE2B_OUTBYTES];
    u32 i;

    if (!out) return 1;
    if (!s) return 2;
    if (s->f[0] != 0) return 3; /* already finalised */

    s->t[0] += s->buflen;
    if (s->t[0] < s->buflen) s->t[1]++;
    s->f[0] = 0xFFFFFFFFFFFFFFFFULL;

    /* Pad the last block with zeros. */
    memset(s->buf + s->buflen, 0, BLAKE2B_BLOCKBYTES - s->buflen);
    blake2b_compress(s, s->buf);

    for (i = 0; i < 8; i++) store_u64_le(tmp + i * 8, s->h[i]);
    memcpy(out, tmp, (size_t)s->outlen);
    return 0;
}

unsigned long blake2b_digest(u8 *out, u32 outlen, const u8 *data, u64 len) {
    blake2b_state s;
    unsigned long rc;
    if (!out) return 1;
    rc = blake2b_init(&s, outlen);   if (rc) return 2;
    rc = blake2b_update(&s, data, len); if (rc) return 3;
    rc = blake2b_final(out, &s);     if (rc) return 4;
    return 0;
}
