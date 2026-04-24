#include "apennines/t2/random/rng.h"
#include "apennines/t1/random/entropy.h"
#include <string.h>

/* ---- CSPRNG ----
 *
 * State layout (48 bytes):
 *   [0..31]  = 256-byte entropy buffer (only 32 bytes stored; we refill)
 *   Actually: [0..31] = buffer, [32..33] = u16 position, [34..47] = reserved
 *
 * Simplified approach: buffer 256 bytes from OS CSPRNG, consume from buffer.
 * We store a separate heap-less design: 32-byte key + 8-byte counter + 8-byte pos.
 *
 * For simplicity and security, we use entropy_get_system for bulk fills
 * and maintain a small 48-byte state that caches a refill batch.
 */

#define CSPRNG_BUF_SIZE 32  /* buffered bytes in state */

/* state layout: bytes 0..31 = buffer, bytes 32..39 = u64 position (how many consumed) */

static u64 csprng_get_pos(csprng *rng) {
    u64 pos;
    memcpy(&pos, rng->state + 32, sizeof(u64));
    return pos;
}

static void csprng_set_pos(csprng *rng, u64 pos) {
    memcpy(rng->state + 32, &pos, sizeof(u64));
}

static unsigned long csprng_refill(csprng *rng) {
    unsigned long rc;
    rc = entropy_get_system(rng->state, CSPRNG_BUF_SIZE);
    if (rc) return rc;
    csprng_set_pos(rng, 0);
    return 0;
}

unsigned long csprng_create(csprng *out) {
    unsigned long rc;

    if (!out) return 1;
    memset(out, 0, sizeof(csprng));
    rc = csprng_refill(out);
    if (rc) return 2;
    return 0;
}

unsigned long csprng_fill(u8 *out, u64 len, csprng *rng) {
    u64 pos;
    u64 avail;
    u64 written;
    unsigned long rc;

    if (!out) return 1;
    if (!rng) return 2;
    if (len == 0) return 0;

    written = 0;
    while (written < len) {
        pos = csprng_get_pos(rng);
        avail = CSPRNG_BUF_SIZE - pos;

        if (avail == 0) {
            rc = csprng_refill(rng);
            if (rc) return 3;
            pos = 0;
            avail = CSPRNG_BUF_SIZE;
        }

        {
            u64 chunk = len - written;
            if (chunk > avail) chunk = avail;
            memcpy(out + written, rng->state + pos, (size_t)chunk);
            csprng_set_pos(rng, pos + chunk);
            written += chunk;
        }
    }

    return 0;
}

unsigned long csprng_u64(u64 *out, csprng *rng) {
    u8 buf[8];
    unsigned long rc;

    if (!out) return 1;
    if (!rng) return 2;

    rc = csprng_fill(buf, 8, rng);
    if (rc) return 3;

    memcpy(out, buf, 8);
    return 0;
}

/* ---- xorshift128+ ---- */

unsigned long xorshift128_create(xorshift128 *out, u64 seed) {
    splitmix64 sm;
    unsigned long rc;

    if (!out) return 1;

    /* use splitmix64 to generate two state words from seed */
    rc = splitmix64_create(&sm, seed);
    if (rc) return 2;
    rc = splitmix64_next(&out->s[0], &sm);
    if (rc) return 2;
    rc = splitmix64_next(&out->s[1], &sm);
    if (rc) return 2;

    /* ensure state is not all zero */
    if (out->s[0] == 0 && out->s[1] == 0) {
        out->s[0] = 1;
    }

    return 0;
}

unsigned long xorshift128_next(u64 *out, xorshift128 *rng) {
    u64 s0;
    u64 s1;

    if (!out) return 1;
    if (!rng) return 2;

    s1 = rng->s[0];
    s0 = rng->s[1];
    rng->s[0] = s0;
    s1 ^= s1 << 23;
    s1 ^= s1 >> 17;
    s1 ^= s0;
    s1 ^= s0 >> 26;
    rng->s[1] = s1;

    *out = rng->s[0] + rng->s[1];
    return 0;
}

unsigned long xorshift128_fill(u8 *out, u64 len, xorshift128 *rng) {
    u64 written;
    unsigned long rc;

    if (!out) return 1;
    if (!rng) return 2;

    written = 0;
    while (written < len) {
        u64 val;
        u64 chunk;
        rc = xorshift128_next(&val, rng);
        if (rc) return 3;

        chunk = len - written;
        if (chunk > 8) chunk = 8;
        memcpy(out + written, &val, (size_t)chunk);
        written += chunk;
    }

    return 0;
}

/* ---- splitmix64 ---- */

unsigned long splitmix64_create(splitmix64 *out, u64 seed) {
    if (!out) return 1;
    out->s = seed;
    return 0;
}

unsigned long splitmix64_next(u64 *out, splitmix64 *rng) {
    u64 z;

    if (!out) return 1;
    if (!rng) return 2;

    rng->s += 0x9e3779b97f4a7c15ULL;
    z = rng->s;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    z = z ^ (z >> 31);

    *out = z;
    return 0;
}

/* ---- PCG32 (PCG-XSH-RR) ---- */

unsigned long pcg32_create(pcg32 *out, u64 seed) {
    if (!out) return 1;

    out->state = 0;
    out->inc = (seed << 1) | 1u;

    /* warm up */
    {
        u32 dummy;
        pcg32_next(&dummy, out);
        out->state += seed;
        pcg32_next(&dummy, out);
    }

    return 0;
}

unsigned long pcg32_next(u32 *out, pcg32 *rng) {
    u64 oldstate;
    u32 xorshifted;
    u32 rot;

    if (!out) return 1;
    if (!rng) return 2;

    oldstate = rng->state;
    rng->state = oldstate * 6364136223846793005ULL + rng->inc;

    xorshifted = (u32)(((oldstate >> 18) ^ oldstate) >> 27);
    rot = (u32)(oldstate >> 59);

    *out = (xorshifted >> rot) | (xorshifted << (((u32)(-(i32)rot)) & 31));
    return 0;
}

unsigned long pcg32_bounded(u32 *out, pcg32 *rng, u32 bound) {
    u32 threshold;
    unsigned long rc;

    if (!out) return 1;
    if (!rng) return 2;
    if (bound == 0) return 3;

    /* rejection sampling to avoid modulo bias */
    threshold = (u32)(-(i32)bound) % bound;

    for (;;) {
        u32 r;
        rc = pcg32_next(&r, rng);
        if (rc) return 4;

        if (r >= threshold) {
            *out = r % bound;
            return 0;
        }
    }
}
