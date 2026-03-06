#include "cookbook_sha256.h"
#include <string.h>

static const uint32_t K[64] = {
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

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROR(x,  2) ^ ROR(x, 13) ^ ROR(x, 22))
#define EP1(x) (ROR(x,  6) ^ ROR(x, 11) ^ ROR(x, 25))
#define SIG0(x) (ROR(x,  7) ^ ROR(x, 18) ^ ((x) >>  3))
#define SIG1(x) (ROR(x, 17) ^ ROR(x, 19) ^ ((x) >> 10))

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
    for (i = 16; i < 64; i++)
        w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + K[i] + w[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void cookbook_sha256_init(cookbook_sha256_ctx *ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

void cookbook_sha256_update(cookbook_sha256_ctx *ctx,
                            const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    size_t idx = (size_t)(ctx->count & 63);

    ctx->count += len;

    if (idx) {
        size_t fill = 64 - idx;
        if (len < fill) {
            memcpy(ctx->buf + idx, p, len);
            return;
        }
        memcpy(ctx->buf + idx, p, fill);
        sha256_transform(ctx->state, ctx->buf);
        p += fill;
        len -= fill;
    }

    while (len >= 64) {
        sha256_transform(ctx->state, p);
        p += 64;
        len -= 64;
    }

    if (len)
        memcpy(ctx->buf, p, len);
}

void cookbook_sha256_final(cookbook_sha256_ctx *ctx, uint8_t digest[32]) {
    uint64_t bits = ctx->count * 8;
    size_t idx = (size_t)(ctx->count & 63);
    int i;

    ctx->buf[idx++] = 0x80;

    if (idx > 56) {
        memset(ctx->buf + idx, 0, 64 - idx);
        sha256_transform(ctx->state, ctx->buf);
        idx = 0;
    }

    memset(ctx->buf + idx, 0, 56 - idx);

    for (i = 0; i < 8; i++)
        ctx->buf[56 + i] = (uint8_t)(bits >> (56 - i * 8));

    sha256_transform(ctx->state, ctx->buf);

    for (i = 0; i < 8; i++) {
        digest[i*4]   = (uint8_t)(ctx->state[i] >> 24);
        digest[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

void cookbook_sha256_hex(const void *data, size_t len, char hex_out[65]) {
    static const char hex_chars[] = "0123456789abcdef";
    uint8_t digest[32];
    cookbook_sha256_ctx ctx;
    int i;

    cookbook_sha256_init(&ctx);
    cookbook_sha256_update(&ctx, data, len);
    cookbook_sha256_final(&ctx, digest);

    for (i = 0; i < 32; i++) {
        hex_out[i*2]   = hex_chars[(digest[i] >> 4) & 0x0f];
        hex_out[i*2+1] = hex_chars[digest[i] & 0x0f];
    }
    hex_out[64] = '\0';
}
