#include "apennines/t2/crypto/hash.h"
#include "apennines/t2/crypto/ct.h"
#include <string.h>
#include <stdlib.h>

/* ================================================================
 * SHA-256 (FIPS 180-4)
 * ================================================================ */

static const u32 sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define RR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHR32(x, n) ((x) >> (n))

#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0_256(x)    (RR32(x, 2) ^ RR32(x, 13) ^ RR32(x, 22))
#define EP1_256(x)    (RR32(x, 6) ^ RR32(x, 11) ^ RR32(x, 25))
#define SIG0_256(x)   (RR32(x, 7) ^ RR32(x, 18) ^ SHR32(x, 3))
#define SIG1_256(x)   (RR32(x, 17) ^ RR32(x, 19) ^ SHR32(x, 10))

static u32 load_be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8)  | (u32)p[3];
}

static void store_be32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)(v);
}

static void store_be64(u8 *p, u64 v) {
    p[0] = (u8)(v >> 56);
    p[1] = (u8)(v >> 48);
    p[2] = (u8)(v >> 40);
    p[3] = (u8)(v >> 32);
    p[4] = (u8)(v >> 24);
    p[5] = (u8)(v >> 16);
    p[6] = (u8)(v >> 8);
    p[7] = (u8)(v);
}

static u64 load_be64(const u8 *p) {
    return ((u64)p[0] << 56) | ((u64)p[1] << 48) |
           ((u64)p[2] << 40) | ((u64)p[3] << 32) |
           ((u64)p[4] << 24) | ((u64)p[5] << 16) |
           ((u64)p[6] << 8)  | (u64)p[7];
}

static void sha256_transform(u32 state[8], const u8 block[64]) {
    u32 w[64];
    u32 a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = load_be32(block + i * 4);
    }
    for (i = 16; i < 64; i++) {
        w[i] = SIG1_256(w[i-2]) + w[i-7] + SIG0_256(w[i-15]) + w[i-16];
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 64; i++) {
        u32 t1 = h + EP1_256(e) + CH(e, f, g) + sha256_k[i] + w[i];
        u32 t2 = EP0_256(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

unsigned long sha256_init(sha256_ctx *ctx) {
    if (!ctx) return 1;

    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;

    return 0;
}

unsigned long sha256_update(sha256_ctx *ctx, const u8 *data, u64 len) {
    u64 buf_used;
    u64 i;

    if (!ctx) return 1;
    if (len > 0 && !data) return 2;

    buf_used = ctx->count % 64;

    ctx->count += len;
    i = 0;

    /* Fill partial buffer */
    if (buf_used > 0) {
        u64 space = 64 - buf_used;
        u64 copy = (len < space) ? len : space;
        memcpy(ctx->buf + buf_used, data, (size_t)copy);
        i += copy;
        if (buf_used + copy < 64) return 0;
        sha256_transform(ctx->state, ctx->buf);
    }

    /* Process full blocks */
    while (i + 64 <= len) {
        sha256_transform(ctx->state, data + i);
        i += 64;
    }

    /* Store remainder */
    if (i < len) {
        memcpy(ctx->buf, data + i, (size_t)(len - i));
    }

    return 0;
}

unsigned long sha256_final(u8 *out, sha256_ctx *ctx) {
    u64 bit_len;
    u64 buf_used;
    int i;

    if (!out) return 1;
    if (!ctx) return 2;

    bit_len = ctx->count * 8;
    buf_used = ctx->count % 64;

    /* Pad with 0x80 */
    ctx->buf[buf_used++] = 0x80;

    if (buf_used > 56) {
        memset(ctx->buf + buf_used, 0, (size_t)(64 - buf_used));
        sha256_transform(ctx->state, ctx->buf);
        buf_used = 0;
    }

    memset(ctx->buf + buf_used, 0, (size_t)(56 - buf_used));
    store_be64(ctx->buf + 56, bit_len);
    sha256_transform(ctx->state, ctx->buf);

    for (i = 0; i < 8; i++) {
        store_be32(out + i * 4, ctx->state[i]);
    }

    /* Wipe context */
    memset(ctx, 0, sizeof(*ctx));

    return 0;
}

unsigned long sha256_digest(u8 *out, const u8 *data, u64 len) {
    sha256_ctx ctx;
    unsigned long rc;

    if (!out) return 1;
    if (len > 0 && !data) return 2;

    rc = sha256_init(&ctx);
    if (rc) return 3;
    rc = sha256_update(&ctx, data, len);
    if (rc) return 4;
    rc = sha256_final(out, &ctx);
    if (rc) return 5;

    return 0;
}

/* ================================================================
 * SHA-512 (FIPS 180-4)
 * ================================================================ */

static const u64 sha512_k[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
    0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
    0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
    0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
    0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
    0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
    0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
    0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
    0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
    0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
    0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

#define RR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
#define SHR64(x, n) ((x) >> (n))

#define EP0_512(x)  (RR64(x, 28) ^ RR64(x, 34) ^ RR64(x, 39))
#define EP1_512(x)  (RR64(x, 14) ^ RR64(x, 18) ^ RR64(x, 41))
#define SIG0_512(x) (RR64(x, 1)  ^ RR64(x, 8)  ^ SHR64(x, 7))
#define SIG1_512(x) (RR64(x, 19) ^ RR64(x, 61) ^ SHR64(x, 6))

static void sha512_transform(u64 state[8], const u8 block[128]) {
    u64 w[80];
    u64 a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = load_be64(block + i * 8);
    }
    for (i = 16; i < 80; i++) {
        w[i] = SIG1_512(w[i-2]) + w[i-7] + SIG0_512(w[i-15]) + w[i-16];
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 80; i++) {
        u64 t1 = h + EP1_512(e) + CH(e, f, g) + sha512_k[i] + w[i];
        u64 t2 = EP0_512(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

unsigned long sha512_init(sha512_ctx *ctx) {
    if (!ctx) return 1;

    ctx->state[0] = 0x6a09e667f3bcc908ULL;
    ctx->state[1] = 0xbb67ae8584caa73bULL;
    ctx->state[2] = 0x3c6ef372fe94f82bULL;
    ctx->state[3] = 0xa54ff53a5f1d36f1ULL;
    ctx->state[4] = 0x510e527fade682d1ULL;
    ctx->state[5] = 0x9b05688c2b3e6c1fULL;
    ctx->state[6] = 0x1f83d9abfb41bd6bULL;
    ctx->state[7] = 0x5be0cd19137e2179ULL;
    ctx->count_lo = 0;
    ctx->count_hi = 0;

    return 0;
}

unsigned long sha512_update(sha512_ctx *ctx, const u8 *data, u64 len) {
    u64 buf_used;
    u64 i;

    if (!ctx) return 1;
    if (len > 0 && !data) return 2;

    buf_used = ctx->count_lo % 128;

    /* Update 128-bit byte count */
    {
        u64 old_lo = ctx->count_lo;
        ctx->count_lo += len;
        if (ctx->count_lo < old_lo) {
            ctx->count_hi++;
        }
    }

    i = 0;

    /* Fill partial buffer */
    if (buf_used > 0) {
        u64 space = 128 - buf_used;
        u64 copy = (len < space) ? len : space;
        memcpy(ctx->buf + buf_used, data, (size_t)copy);
        i += copy;
        if (buf_used + copy < 128) return 0;
        sha512_transform(ctx->state, ctx->buf);
    }

    /* Process full blocks */
    while (i + 128 <= len) {
        sha512_transform(ctx->state, data + i);
        i += 128;
    }

    /* Store remainder */
    if (i < len) {
        memcpy(ctx->buf, data + i, (size_t)(len - i));
    }

    return 0;
}

unsigned long sha512_final(u8 *out, sha512_ctx *ctx) {
    u64 bit_lo, bit_hi;
    u64 buf_used;
    int i;

    if (!out) return 1;
    if (!ctx) return 2;

    /* Compute bit length (128-bit) */
    bit_lo = ctx->count_lo << 3;
    bit_hi = (ctx->count_hi << 3) | (ctx->count_lo >> 61);

    buf_used = ctx->count_lo % 128;

    /* Pad with 0x80 */
    ctx->buf[buf_used++] = 0x80;

    if (buf_used > 112) {
        memset(ctx->buf + buf_used, 0, (size_t)(128 - buf_used));
        sha512_transform(ctx->state, ctx->buf);
        buf_used = 0;
    }

    memset(ctx->buf + buf_used, 0, (size_t)(112 - buf_used));
    store_be64(ctx->buf + 112, bit_hi);
    store_be64(ctx->buf + 120, bit_lo);
    sha512_transform(ctx->state, ctx->buf);

    for (i = 0; i < 8; i++) {
        store_be64(out + i * 8, ctx->state[i]);
    }

    /* Wipe context */
    memset(ctx, 0, sizeof(*ctx));

    return 0;
}

unsigned long sha512_digest(u8 *out, const u8 *data, u64 len) {
    sha512_ctx ctx;
    unsigned long rc;

    if (!out) return 1;
    if (len > 0 && !data) return 2;

    rc = sha512_init(&ctx);
    if (rc) return 3;
    rc = sha512_update(&ctx, data, len);
    if (rc) return 4;
    rc = sha512_final(out, &ctx);
    if (rc) return 5;

    return 0;
}

/* ================================================================
 * HMAC (RFC 2104)
 * ================================================================ */

#define SHA256_BLOCK_SIZE 64
#define SHA256_DIGEST_SIZE 32
#define SHA512_BLOCK_SIZE 128
#define SHA512_DIGEST_SIZE 64

static u64 hmac_block_size(unsigned long hash_id) {
    return (hash_id == HMAC_HASH_SHA512) ? SHA512_BLOCK_SIZE : SHA256_BLOCK_SIZE;
}

static u64 hmac_digest_size(unsigned long hash_id) {
    return (hash_id == HMAC_HASH_SHA512) ? SHA512_DIGEST_SIZE : SHA256_DIGEST_SIZE;
}

static unsigned long hmac_hash_init(unsigned long hash_id, void *ctx) {
    if (hash_id == HMAC_HASH_SHA256) return sha256_init((sha256_ctx *)ctx);
    if (hash_id == HMAC_HASH_SHA512) return sha512_init((sha512_ctx *)ctx);
    return 1;
}

static unsigned long hmac_hash_update(unsigned long hash_id, void *ctx, const u8 *data, u64 len) {
    if (hash_id == HMAC_HASH_SHA256) return sha256_update((sha256_ctx *)ctx, data, len);
    if (hash_id == HMAC_HASH_SHA512) return sha512_update((sha512_ctx *)ctx, data, len);
    return 1;
}

static unsigned long hmac_hash_final(unsigned long hash_id, u8 *out, void *ctx) {
    if (hash_id == HMAC_HASH_SHA256) return sha256_final(out, (sha256_ctx *)ctx);
    if (hash_id == HMAC_HASH_SHA512) return sha512_final(out, (sha512_ctx *)ctx);
    return 1;
}

static void *hmac_inner_ctx(hmac_ctx *ctx) {
    if (ctx->hash_id == HMAC_HASH_SHA512) return &ctx->inner.sha512;
    return &ctx->inner.sha256;
}

static void *hmac_outer_ctx(hmac_ctx *ctx) {
    if (ctx->hash_id == HMAC_HASH_SHA512) return &ctx->outer.sha512;
    return &ctx->outer.sha256;
}

unsigned long hmac_create(hmac_ctx *ctx, unsigned long hash_id, const u8 *key, u64 key_len) {
    u8 key_block[128]; /* max block size */
    u8 ipad[128];
    u8 opad[128];
    u64 block_sz;
    u64 i;
    unsigned long rc;

    if (!ctx) return 1;
    if (hash_id != HMAC_HASH_SHA256 && hash_id != HMAC_HASH_SHA512) return 2;
    if (key_len > 0 && !key) return 3;

    ctx->hash_id = hash_id;
    block_sz = hmac_block_size(hash_id);

    memset(key_block, 0, sizeof(key_block));

    if (key_len > block_sz) {
        /* Hash the key if it's longer than block size */
        u8 hashed_key[64]; /* max digest size */
        if (hash_id == HMAC_HASH_SHA256) {
            rc = sha256_digest(hashed_key, key, key_len);
        } else {
            rc = sha512_digest(hashed_key, key, key_len);
        }
        if (rc) return 4;
        memcpy(key_block, hashed_key, (size_t)hmac_digest_size(hash_id));
        memset(hashed_key, 0, sizeof(hashed_key));
    } else {
        memcpy(key_block, key, (size_t)key_len);
    }

    /* Build ipad and opad */
    for (i = 0; i < block_sz; i++) {
        ipad[i] = key_block[i] ^ 0x36;
        opad[i] = key_block[i] ^ 0x5c;
    }

    /* Initialize inner hash with ipad */
    rc = hmac_hash_init(hash_id, hmac_inner_ctx(ctx));
    if (rc) return 5;
    rc = hmac_hash_update(hash_id, hmac_inner_ctx(ctx), ipad, block_sz);
    if (rc) return 6;

    /* Initialize outer hash with opad */
    rc = hmac_hash_init(hash_id, hmac_outer_ctx(ctx));
    if (rc) return 7;
    rc = hmac_hash_update(hash_id, hmac_outer_ctx(ctx), opad, block_sz);
    if (rc) return 8;

    /* Wipe sensitive data */
    memset(key_block, 0, sizeof(key_block));
    memset(ipad, 0, sizeof(ipad));
    memset(opad, 0, sizeof(opad));

    return 0;
}

unsigned long hmac_update(hmac_ctx *ctx, const u8 *data, u64 len) {
    if (!ctx) return 1;
    if (len > 0 && !data) return 2;

    return hmac_hash_update(ctx->hash_id, hmac_inner_ctx(ctx), data, len);
}

unsigned long hmac_final(u8 *out, hmac_ctx *ctx) {
    u8 inner_digest[64]; /* max digest size */
    u64 digest_sz;
    unsigned long rc;

    if (!out) return 1;
    if (!ctx) return 2;

    digest_sz = hmac_digest_size(ctx->hash_id);

    /* Finalize inner hash */
    rc = hmac_hash_final(ctx->hash_id, inner_digest, hmac_inner_ctx(ctx));
    if (rc) return 3;

    /* Feed inner digest to outer hash and finalize */
    rc = hmac_hash_update(ctx->hash_id, hmac_outer_ctx(ctx), inner_digest, digest_sz);
    if (rc) return 4;
    rc = hmac_hash_final(ctx->hash_id, out, hmac_outer_ctx(ctx));
    if (rc) return 5;

    memset(inner_digest, 0, sizeof(inner_digest));
    memset(ctx, 0, sizeof(*ctx));

    return 0;
}

unsigned long hmac_digest(u8 *out, unsigned long hash_id, const u8 *key, u64 key_len, const u8 *data, u64 data_len) {
    hmac_ctx ctx;
    unsigned long rc;

    if (!out) return 1;
    if (hash_id != HMAC_HASH_SHA256 && hash_id != HMAC_HASH_SHA512) return 2;
    if (key_len > 0 && !key) return 3;
    if (data_len > 0 && !data) return 4;

    rc = hmac_create(&ctx, hash_id, key, key_len);
    if (rc) return 5;
    rc = hmac_update(&ctx, data, data_len);
    if (rc) return 6;
    rc = hmac_final(out, &ctx);
    if (rc) return 7;

    return 0;
}

unsigned long hmac_verify(unsigned long *result, const u8 *expected, unsigned long hash_id, const u8 *key, u64 key_len, const u8 *data, u64 data_len) {
    u8 computed[64]; /* max digest size */
    u64 digest_sz;
    unsigned long rc;

    if (!result) return 1;
    if (!expected) return 2;
    if (hash_id != HMAC_HASH_SHA256 && hash_id != HMAC_HASH_SHA512) return 3;
    if (key_len > 0 && !key) return 4;
    if (data_len > 0 && !data) return 5;

    digest_sz = hmac_digest_size(hash_id);

    rc = hmac_digest(computed, hash_id, key, key_len, data, data_len);
    if (rc) return 6;

    /* Constant-time comparison: result=0 means equal (match), 1 means different */
    rc = ct_compare(result, expected, computed, digest_sz);

    memset(computed, 0, sizeof(computed));

    if (rc) return 7;
    return 0;
}

/* ================================================================
 * HKDF (RFC 5869)
 * ================================================================ */

unsigned long hkdf_extract(u8 *prk, unsigned long hash_id, const u8 *salt, u64 salt_len, const u8 *ikm, u64 ikm_len) {
    u8 default_salt[64]; /* max digest size */
    const u8 *actual_salt;
    u64 actual_salt_len;
    unsigned long rc;

    if (!prk) return 1;
    if (hash_id != HMAC_HASH_SHA256 && hash_id != HMAC_HASH_SHA512) return 2;
    if (ikm_len > 0 && !ikm) return 3;

    /* If salt is not provided, use a string of HashLen zeros */
    if (!salt || salt_len == 0) {
        u64 digest_sz = hmac_digest_size(hash_id);
        memset(default_salt, 0, (size_t)digest_sz);
        actual_salt = default_salt;
        actual_salt_len = digest_sz;
    } else {
        actual_salt = salt;
        actual_salt_len = salt_len;
    }

    /* PRK = HMAC-Hash(salt, IKM) */
    rc = hmac_digest(prk, hash_id, actual_salt, actual_salt_len, ikm, ikm_len);
    if (rc) return 4;

    return 0;
}

unsigned long hkdf_expand(u8 *okm, u64 okm_len, unsigned long hash_id, const u8 *prk, u64 prk_len, const u8 *info, u64 info_len) {
    u64 digest_sz;
    u64 n;
    u64 offset;
    u8 t_prev[64]; /* max digest size */
    u64 t_prev_len;
    u8 counter;
    unsigned long rc;

    if (!okm) return 1;
    if (hash_id != HMAC_HASH_SHA256 && hash_id != HMAC_HASH_SHA512) return 2;
    if (!prk || prk_len == 0) return 3;
    if (info_len > 0 && !info) return 4;

    digest_sz = hmac_digest_size(hash_id);

    /* N = ceil(L/HashLen) */
    n = (okm_len + digest_sz - 1) / digest_sz;

    /* RFC 5869: N must be <= 255 */
    if (n > 255) return 5;
    if (okm_len == 0) return 0;

    offset = 0;
    t_prev_len = 0;

    for (counter = 1; counter <= (u8)n; counter++) {
        hmac_ctx ctx;
        u8 t_cur[64]; /* max digest size */
        u64 copy_len;

        rc = hmac_create(&ctx, hash_id, prk, prk_len);
        if (rc) return 6;

        /* T(i) = HMAC-Hash(PRK, T(i-1) | info | counter) */
        if (t_prev_len > 0) {
            rc = hmac_update(&ctx, t_prev, t_prev_len);
            if (rc) return 7;
        }
        if (info_len > 0) {
            rc = hmac_update(&ctx, info, info_len);
            if (rc) return 8;
        }
        rc = hmac_update(&ctx, &counter, 1);
        if (rc) return 9;
        rc = hmac_final(t_cur, &ctx);
        if (rc) return 10;

        copy_len = okm_len - offset;
        if (copy_len > digest_sz) copy_len = digest_sz;
        memcpy(okm + offset, t_cur, (size_t)copy_len);
        offset += copy_len;

        memcpy(t_prev, t_cur, (size_t)digest_sz);
        t_prev_len = digest_sz;

        memset(t_cur, 0, sizeof(t_cur));
    }

    memset(t_prev, 0, sizeof(t_prev));

    return 0;
}

unsigned long hkdf_derive(u8 *okm, u64 okm_len, unsigned long hash_id, const u8 *salt, u64 salt_len, const u8 *ikm, u64 ikm_len, const u8 *info, u64 info_len) {
    u8 prk[64]; /* max digest size */
    u64 digest_sz;
    unsigned long rc;

    if (!okm) return 1;
    if (hash_id != HMAC_HASH_SHA256 && hash_id != HMAC_HASH_SHA512) return 2;
    if (ikm_len > 0 && !ikm) return 3;
    if (info_len > 0 && !info) return 4;

    digest_sz = hmac_digest_size(hash_id);

    rc = hkdf_extract(prk, hash_id, salt, salt_len, ikm, ikm_len);
    if (rc) return 5;

    rc = hkdf_expand(okm, okm_len, hash_id, prk, digest_sz, info, info_len);
    if (rc) return 6;

    memset(prk, 0, sizeof(prk));

    return 0;
}
/* ================================================================
 * PBKDF2-HMAC-SHA256 (RFC 2898) — internal helper used by scrypt
 * ================================================================ */

static unsigned long pbkdf2_hmac_sha256(u8 *out, u64 out_len,
                                        const u8 *password, u64 password_len,
                                        const u8 *salt, u64 salt_len,
                                        u64 iterations) {
    hmac_ctx base_ctx;
    u64 digest_sz = 32;
    u64 blocks;
    u64 i;
    unsigned long rc;

    if (out_len == 0) return 0;

    /* RFC 2898: dkLen must be <= (2^32 - 1) * hLen. Effectively unlimited here. */
    blocks = (out_len + digest_sz - 1) / digest_sz;
    if (iterations == 0) return 1;

    /* Pre-build an HMAC context keyed with the password to avoid recomputing
     * the ipad/opad for each iteration. We'll copy it for each U_1. */
    rc = hmac_create(&base_ctx, HMAC_HASH_SHA256, password, password_len);
    if (rc) return 2;

    for (i = 1; i <= blocks; i++) {
        hmac_ctx ctx;
        u8 int_i[4];
        u8 u[32];
        u8 t[32];
        u64 j;
        u64 k;
        u64 copy_len;

        int_i[0] = (u8)(i >> 24);
        int_i[1] = (u8)(i >> 16);
        int_i[2] = (u8)(i >> 8);
        int_i[3] = (u8)(i);

        /* U_1 = HMAC(P, S || INT(i)) */
        memcpy(&ctx, &base_ctx, sizeof(hmac_ctx));
        rc = hmac_update(&ctx, salt, salt_len);
        if (rc) { memset(&base_ctx, 0, sizeof(base_ctx)); return 3; }
        rc = hmac_update(&ctx, int_i, 4);
        if (rc) { memset(&base_ctx, 0, sizeof(base_ctx)); return 4; }
        rc = hmac_final(u, &ctx);
        if (rc) { memset(&base_ctx, 0, sizeof(base_ctx)); return 5; }

        memcpy(t, u, 32);

        /* U_j = HMAC(P, U_{j-1}); T_i = U_1 XOR U_2 XOR ... XOR U_c */
        for (j = 2; j <= iterations; j++) {
            memcpy(&ctx, &base_ctx, sizeof(hmac_ctx));
            rc = hmac_update(&ctx, u, 32);
            if (rc) { memset(&base_ctx, 0, sizeof(base_ctx)); return 6; }
            rc = hmac_final(u, &ctx);
            if (rc) { memset(&base_ctx, 0, sizeof(base_ctx)); return 7; }
            for (k = 0; k < 32; k++) t[k] ^= u[k];
        }

        copy_len = digest_sz;
        if ((i - 1) * digest_sz + copy_len > out_len) {
            copy_len = out_len - (i - 1) * digest_sz;
        }
        memcpy(out + (i - 1) * digest_sz, t, (size_t)copy_len);

        memset(u, 0, sizeof(u));
        memset(t, 0, sizeof(t));
    }

    memset(&base_ctx, 0, sizeof(base_ctx));
    return 0;
}

/* ================================================================
 * scrypt (RFC 7914)
 * ================================================================ */

/* Little-endian 32-bit load/store (scrypt / Salsa20 operate on LE words) */
static u32 load_le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void store_le32(u8 *p, u32 v) {
    p[0] = (u8)(v);
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

/* Salsa20/8 core on a 64-byte block. RFC 7914 §3. */
static void salsa20_8(u8 out[64], const u8 in[64]) {
    u32 x[16];
    u32 orig[16];
    int i;

    for (i = 0; i < 16; i++) {
        x[i] = load_le32(in + i * 4);
        orig[i] = x[i];
    }

    for (i = 0; i < 8; i += 2) {
        /* Column round */
        x[ 4] ^= ROTL32(x[ 0] + x[12],  7);
        x[ 8] ^= ROTL32(x[ 4] + x[ 0],  9);
        x[12] ^= ROTL32(x[ 8] + x[ 4], 13);
        x[ 0] ^= ROTL32(x[12] + x[ 8], 18);
        x[ 9] ^= ROTL32(x[ 5] + x[ 1],  7);
        x[13] ^= ROTL32(x[ 9] + x[ 5],  9);
        x[ 1] ^= ROTL32(x[13] + x[ 9], 13);
        x[ 5] ^= ROTL32(x[ 1] + x[13], 18);
        x[14] ^= ROTL32(x[10] + x[ 6],  7);
        x[ 2] ^= ROTL32(x[14] + x[10],  9);
        x[ 6] ^= ROTL32(x[ 2] + x[14], 13);
        x[10] ^= ROTL32(x[ 6] + x[ 2], 18);
        x[ 3] ^= ROTL32(x[15] + x[11],  7);
        x[ 7] ^= ROTL32(x[ 3] + x[15],  9);
        x[11] ^= ROTL32(x[ 7] + x[ 3], 13);
        x[15] ^= ROTL32(x[11] + x[ 7], 18);
        /* Row round */
        x[ 1] ^= ROTL32(x[ 0] + x[ 3],  7);
        x[ 2] ^= ROTL32(x[ 1] + x[ 0],  9);
        x[ 3] ^= ROTL32(x[ 2] + x[ 1], 13);
        x[ 0] ^= ROTL32(x[ 3] + x[ 2], 18);
        x[ 6] ^= ROTL32(x[ 5] + x[ 4],  7);
        x[ 7] ^= ROTL32(x[ 6] + x[ 5],  9);
        x[ 4] ^= ROTL32(x[ 7] + x[ 6], 13);
        x[ 5] ^= ROTL32(x[ 4] + x[ 7], 18);
        x[11] ^= ROTL32(x[10] + x[ 9],  7);
        x[ 8] ^= ROTL32(x[11] + x[10],  9);
        x[ 9] ^= ROTL32(x[ 8] + x[11], 13);
        x[10] ^= ROTL32(x[ 9] + x[ 8], 18);
        x[12] ^= ROTL32(x[15] + x[14],  7);
        x[13] ^= ROTL32(x[12] + x[15],  9);
        x[14] ^= ROTL32(x[13] + x[12], 13);
        x[15] ^= ROTL32(x[14] + x[13], 18);
    }

    for (i = 0; i < 16; i++) {
        store_le32(out + i * 4, x[i] + orig[i]);
    }
}

/* scryptBlockMix: operates in-place on B (128*r bytes). RFC 7914 §4. */
static void scrypt_block_mix(u8 *B, u32 r, u8 *scratch /* 128*r bytes */) {
    u8 X[64];
    u32 i;
    u32 two_r = 2 * r;

    /* X = B_{2r-1} */
    memcpy(X, B + (two_r - 1) * 64, 64);

    for (i = 0; i < two_r; i++) {
        u32 j;
        /* X = X XOR B_i */
        for (j = 0; j < 64; j++) X[j] ^= B[i * 64 + j];
        /* X = Salsa20/8(X) */
        salsa20_8(X, X);
        /* Y_i = X — stored in interleaved order straight into scratch */
        /* B'_i = Y_{2i} for i=0..r-1; B'_{r+i} = Y_{2i+1} for i=0..r-1 */
        if ((i & 1) == 0) {
            memcpy(scratch + (i / 2) * 64, X, 64);
        } else {
            memcpy(scratch + (r + i / 2) * 64, X, 64);
        }
    }

    memcpy(B, scratch, (size_t)(128 * r));
    memset(X, 0, sizeof(X));
}

/* Integerify(B): treats last 64-byte block of B as little-endian integer and
 * returns its value modulo N. Since N is a power of two <= 2^64, we can simply
 * take the low 64 bits of B_{2r-1} and AND with N-1. */
static u64 scrypt_integerify(const u8 *B, u32 r, u64 N_mask) {
    const u8 *last = B + (2 * r - 1) * 64;
    u64 v = load_le32(last);
    v |= ((u64)load_le32(last + 4)) << 32;
    return v & N_mask;
}

/* scryptROMix: operates in-place on B (128*r bytes). RFC 7914 §5. */
static unsigned long scrypt_romix(u8 *B, u32 r, u64 N) {
    u64 block_sz = (u64)128 * r;
    u8 *V;
    u8 *scratch; /* scratch for block_mix */
    u64 i;
    u64 N_mask = N - 1;

    V = (u8 *)malloc((size_t)(block_sz * N));
    if (!V) return 1;
    scratch = (u8 *)malloc((size_t)block_sz);
    if (!scratch) { free(V); return 2; }

    /* Step 2: V_i = X; X = BlockMix(X) */
    for (i = 0; i < N; i++) {
        memcpy(V + i * block_sz, B, (size_t)block_sz);
        scrypt_block_mix(B, r, scratch);
    }

    /* Step 3: j = Integerify(X) mod N; X = BlockMix(X XOR V_j) */
    for (i = 0; i < N; i++) {
        u64 j = scrypt_integerify(B, r, N_mask);
        u64 k;
        u8 *Vj = V + j * block_sz;
        for (k = 0; k < block_sz; k++) B[k] ^= Vj[k];
        scrypt_block_mix(B, r, scratch);
    }

    /* Wipe & free */
    memset(V, 0, (size_t)(block_sz * N));
    memset(scratch, 0, (size_t)block_sz);
    free(V);
    free(scratch);
    return 0;
}

unsigned long scrypt_derive(u8 *out, u64 out_len,
                            const u8 *password, u64 password_len,
                            const u8 *salt, u64 salt_len,
                            u64 N, u32 r, u32 p) {
    u64 block_sz;
    u64 B_len;
    u8 *B = NULL;
    u32 i;
    unsigned long rc;

    if (!out) return 1;
    if (password_len > 0 && !password) return 2;
    if (salt_len > 0 && !salt) return 3;

    /* N must be a power of two and >= 2 */
    if (N < 2) return 4;
    if (N & (N - 1)) return 4;

    /* r, p >= 1. Also guard against overflow in p * 128 * r. */
    if (r == 0 || p == 0) return 5;
    /* RFC 7914: p <= ((2^32 - 1) * 32) / (128 * r). For our 64-bit arithmetic,
     * requiring (u64)p * 128 * r to fit in u64 is trivially true; we mainly
     * need to ensure malloc won't blow up. Use a conservative upper bound. */
    if ((u64)r > (u64)0xFFFFFFFFULL / 128) return 5;

    if (out_len == 0) return 0;

    block_sz = (u64)128 * r;
    B_len = block_sz * p;

    B = (u8 *)malloc((size_t)B_len);
    if (!B) return 6;

    /* Step 1: B = PBKDF2(P, S, 1, p*128*r) */
    rc = pbkdf2_hmac_sha256(B, B_len, password, password_len, salt, salt_len, 1);
    if (rc) { memset(B, 0, (size_t)B_len); free(B); return 6; }

    /* Step 2: for i = 0..p-1: B_i = scryptROMix(r, B_i, N) */
    for (i = 0; i < p; i++) {
        rc = scrypt_romix(B + (u64)i * block_sz, r, N);
        if (rc) { memset(B, 0, (size_t)B_len); free(B); return 6; }
    }

    /* Step 3: out = PBKDF2(P, B, 1, dkLen) */
    rc = pbkdf2_hmac_sha256(out, out_len, password, password_len, B, B_len, 1);

    memset(B, 0, (size_t)B_len);
    free(B);

    if (rc) return 6;
    return 0;
}
