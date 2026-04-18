#include "apennines/t2/crypto/argon2.h"
#include "apennines/t2/crypto/blake2b.h"
#include <stdlib.h>
#include <string.h>

/* RFC 9106 — Argon2id (single-threaded; parallelism lanes executed
 * sequentially). Cookbook needs the libsodium-compatible shape so
 * that's what we ship: encoded output + parse/verify.
 *
 * The compression function G is built on BLAKE2b's round (P). H' is
 * BLAKE2b with variable-length output (≤64 direct, else chained). */

#define ARGON2_BLOCK_BYTES   1024
#define ARGON2_BLOCK_QWORDS  128
#define ARGON2_TYPE_ID       2
#define ARGON2_VERSION       0x13

typedef struct {
    u64 v[ARGON2_BLOCK_QWORDS];
} argon2_block;

/* ── Byte helpers ───────────────────────────────────────────── */

static void store32_le(u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
static u64 load64_le(const u8 *p) {
    u64 v = 0;
    int i;
    for (i = 0; i < 8; i++) v |= ((u64)p[i]) << (8 * i);
    return v;
}
static void store64_le(u8 *p, u64 v) {
    int i;
    for (i = 0; i < 8; i++) p[i] = (u8)(v >> (8 * i));
}

/* ── H': variable-length hash (RFC 9106 §3.3) ───────────────── */

static unsigned long H_prime(u8 *out, u64 out_len,
                              const u8 *input, u64 input_len) {
    u8 len_le[4];
    store32_le(len_le, (u32)out_len);
    if (out_len <= 64) {
        blake2b_state s;
        unsigned long rc = blake2b_init(&s, (u32)out_len);
        if (rc) return rc;
        rc = blake2b_update(&s, len_le, 4);   if (rc) return rc;
        rc = blake2b_update(&s, input, input_len); if (rc) return rc;
        return blake2b_final(out, &s);
    } else {
        /* RFC 9106 §3.3: r = ceil(T/32) - 2
         *   V_1            = BLAKE2b(LE32(T) || X, 64)
         *   V_2 .. V_r     = BLAKE2b(V_{i-1}, 64)                [64-byte chain]
         *   V_{r+1}        = BLAKE2b(V_r, T - 32*r)              [tail, any length]
         *   out            = V_1[0..31] || V_2[0..31] || ... || V_r[0..31] || V_{r+1}
         *
         * Total = 32*r + (T - 32*r) = T. */
        u8 V[64];
        unsigned long rc;
        u64 r = (out_len + 31) / 32 - 2;
        u64 i;
        u64 tail;
        blake2b_state s;
        rc = blake2b_init(&s, 64); if (rc) return rc;
        rc = blake2b_update(&s, len_le, 4); if (rc) return rc;
        rc = blake2b_update(&s, input, input_len); if (rc) return rc;
        rc = blake2b_final(V, &s); if (rc) return rc;
        /* Emit first 32 bytes of V_1 .. V_{r-1}, chaining V forward so
         * V holds V_r by the end of the loop. */
        for (i = 1; i < r; i++) {
            memcpy(out + 32 * (i - 1), V, 32);
            rc = blake2b_digest(V, 64, V, 64);
            if (rc) return rc;
        }
        /* V now holds V_r. Emit V_r[0..31] and then the tail block. */
        memcpy(out + 32 * (r - 1), V, 32);
        tail = out_len - 32 * r;
        return blake2b_digest(out + 32 * r, (u32)tail, V, 64);
    }
}

/* ── G compression (RFC 9106 §3.6) ──────────────────────────── */

#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static u64 mul_lo32(u64 a, u64 b) {
    return 2ULL * (a & 0xFFFFFFFFULL) * (b & 0xFFFFFFFFULL);
}

#define G_B(a, b, c, d) do { \
    a = a + b + mul_lo32(a, b); d = ROTR64(d ^ a, 32); \
    c = c + d + mul_lo32(c, d); b = ROTR64(b ^ c, 24); \
    a = a + b + mul_lo32(a, b); d = ROTR64(d ^ a, 16); \
    c = c + d + mul_lo32(c, d); b = ROTR64(b ^ c, 63); \
} while(0)

static void P(u64 *v) {
    G_B(v[0], v[4], v[ 8], v[12]);
    G_B(v[1], v[5], v[ 9], v[13]);
    G_B(v[2], v[6], v[10], v[14]);
    G_B(v[3], v[7], v[11], v[15]);
    G_B(v[0], v[5], v[10], v[15]);
    G_B(v[1], v[6], v[11], v[12]);
    G_B(v[2], v[7], v[ 8], v[13]);
    G_B(v[3], v[4], v[ 9], v[14]);
}

static void block_xor(argon2_block *dst, const argon2_block *a,
                      const argon2_block *b) {
    int i;
    for (i = 0; i < ARGON2_BLOCK_QWORDS; i++) dst->v[i] = a->v[i] ^ b->v[i];
}

static void block_xor_in(argon2_block *dst, const argon2_block *a) {
    int i;
    for (i = 0; i < ARGON2_BLOCK_QWORDS; i++) dst->v[i] ^= a->v[i];
}

static void compress_G(argon2_block *out, const argon2_block *X,
                        const argon2_block *Y, int xor_into_out) {
    argon2_block R, Z;
    int i;
    block_xor(&R, X, Y);
    Z = R;
    /* Apply P to each of 8 "rows" (16 contiguous 64-bit words). */
    for (i = 0; i < 8; i++) P(&Z.v[16 * i]);
    /* Apply P to each of 8 "columns". Column i is formed by taking
     * words (2i, 2i+1) from each of the 8 rows. */
    for (i = 0; i < 8; i++) {
        u64 col[16];
        int j;
        for (j = 0; j < 8; j++) {
            col[2 * j]     = Z.v[16 * j + 2 * i];
            col[2 * j + 1] = Z.v[16 * j + 2 * i + 1];
        }
        P(col);
        for (j = 0; j < 8; j++) {
            Z.v[16 * j + 2 * i]     = col[2 * j];
            Z.v[16 * j + 2 * i + 1] = col[2 * j + 1];
        }
    }
    if (xor_into_out) {
        int k;
        for (k = 0; k < ARGON2_BLOCK_QWORDS; k++) {
            out->v[k] ^= Z.v[k] ^ R.v[k];
        }
    } else {
        for (i = 0; i < ARGON2_BLOCK_QWORDS; i++) {
            out->v[i] = Z.v[i] ^ R.v[i];
        }
    }
}

/* ── Memory instance ────────────────────────────────────────── */

typedef struct {
    argon2_block *memory;
    u32           m_kib;
    u32           m_actual;
    u32           q;         /* blocks per lane */
    u32           seg;       /* segment length = q / 4 */
    u32           lanes;
    u32           t_cost;
    u32           tag_len;
    u8            type;
} argon2_inst;

/* ── H0 preimage (RFC 9106 §3.2) ────────────────────────────── */

/* Test hook: when non-NULL, use these in place of empty secret / AD. */
static const u8 *g_dbg_secret = NULL; static u64 g_dbg_secret_len = 0;
static const u8 *g_dbg_ad     = NULL; static u64 g_dbg_ad_len     = 0;

APENNINES_API void argon2_dbg_set_secret_ad(const u8 *secret, u64 secret_len,
                                             const u8 *ad, u64 ad_len);
void argon2_dbg_set_secret_ad(const u8 *secret, u64 secret_len,
                               const u8 *ad, u64 ad_len) {
    g_dbg_secret = secret; g_dbg_secret_len = secret_len;
    g_dbg_ad = ad; g_dbg_ad_len = ad_len;
}

static unsigned long compute_H0(u8 h0_out[64],
                                 const argon2_inst *inst,
                                 const u8 *pwd, u64 pwd_len,
                                 const u8 *salt, u64 salt_len) {
    blake2b_state s;
    u8 buf[4];
    unsigned long rc;
    rc = blake2b_init(&s, 64); if (rc) return rc;

    store32_le(buf, inst->lanes);       blake2b_update(&s, buf, 4);
    store32_le(buf, inst->tag_len);     blake2b_update(&s, buf, 4);
    store32_le(buf, inst->m_kib);       blake2b_update(&s, buf, 4);
    store32_le(buf, inst->t_cost);      blake2b_update(&s, buf, 4);
    store32_le(buf, ARGON2_VERSION);    blake2b_update(&s, buf, 4);
    store32_le(buf, inst->type);        blake2b_update(&s, buf, 4);

    store32_le(buf, (u32)pwd_len);      blake2b_update(&s, buf, 4);
    if (pwd_len > 0) blake2b_update(&s, pwd, pwd_len);
    store32_le(buf, (u32)salt_len);     blake2b_update(&s, buf, 4);
    if (salt_len > 0) blake2b_update(&s, salt, salt_len);
    store32_le(buf, (u32)g_dbg_secret_len); blake2b_update(&s, buf, 4);
    if (g_dbg_secret_len > 0) blake2b_update(&s, g_dbg_secret, g_dbg_secret_len);
    store32_le(buf, (u32)g_dbg_ad_len); blake2b_update(&s, buf, 4);
    if (g_dbg_ad_len > 0) blake2b_update(&s, g_dbg_ad, g_dbg_ad_len);

    return blake2b_final(h0_out, &s);
}

/* ── Initial blocks (RFC 9106 §3.3) ─────────────────────────── */

static unsigned long init_blocks(argon2_inst *inst, const u8 h0[64]) {
    u8 in[72];
    u32 lane;
    unsigned long rc;
    memcpy(in, h0, 64);
    for (lane = 0; lane < inst->lanes; lane++) {
        u8 out_bytes[ARGON2_BLOCK_BYTES];
        store32_le(in + 64, 0);
        store32_le(in + 68, lane);
        rc = H_prime(out_bytes, ARGON2_BLOCK_BYTES, in, 72);
        if (rc) return rc;
        {
            int i;
            argon2_block *B0 = &inst->memory[lane * inst->q + 0];
            for (i = 0; i < ARGON2_BLOCK_QWORDS; i++) {
                B0->v[i] = load64_le(out_bytes + 8 * i);
            }
        }
        store32_le(in + 64, 1);
        rc = H_prime(out_bytes, ARGON2_BLOCK_BYTES, in, 72);
        if (rc) return rc;
        {
            int i;
            argon2_block *B1 = &inst->memory[lane * inst->q + 1];
            for (i = 0; i < ARGON2_BLOCK_QWORDS; i++) {
                B1->v[i] = load64_le(out_bytes + 8 * i);
            }
        }
    }
    return 0;
}

/* ── Reference index (RFC 9106 §3.4.1.2) ────────────────────── */

static u32 index_alpha(u32 J1, u32 ref_area_size) {
    u64 J1_square = ((u64)J1 * (u64)J1) >> 32;
    u64 relative_pos = ((u64)ref_area_size * J1_square) >> 32;
    return (u32)(ref_area_size - 1 - relative_pos);
}

/* ── Fill one segment of one lane ───────────────────────────── */

static unsigned long fill_segment(argon2_inst *inst, u32 pass, u32 slice,
                                   u32 lane) {
    u32 i;
    u32 data_independent;
    argon2_block address_block, input_block, zero_block;
    u32 addr_counter = 0;
    u32 starting_index;

    data_independent = (inst->type == ARGON2_TYPE_ID && pass == 0 && slice < 2) ? 1 : 0;
    memset(&address_block, 0, sizeof(address_block));
    memset(&input_block, 0, sizeof(input_block));
    memset(&zero_block, 0, sizeof(zero_block));

    if (data_independent) {
        input_block.v[0] = pass;
        input_block.v[1] = lane;
        input_block.v[2] = slice;
        input_block.v[3] = inst->m_actual;
        input_block.v[4] = inst->t_cost;
        input_block.v[5] = inst->type;
    }

    starting_index = 0;
    if (pass == 0 && slice == 0) starting_index = 2;
    for (i = starting_index; i < inst->seg; i++) {
        u32 curr_offset, prev_offset;
        u32 J1, J2;
        u32 ref_lane, ref_index;
        u32 same_lane;

        curr_offset = lane * inst->q + slice * inst->seg + i;
        if (curr_offset % inst->q == 0) {
            prev_offset = curr_offset + inst->q - 1;
        } else {
            prev_offset = curr_offset - 1;
        }

        if (data_independent) {
            if ((addr_counter % ARGON2_BLOCK_QWORDS) == 0) {
                input_block.v[6]++;
                /* address_block = G(ZERO, G(ZERO, input_block)). */
                compress_G(&address_block, &zero_block, &input_block, 0);
                {
                    argon2_block tmp = address_block;
                    compress_G(&address_block, &zero_block, &tmp, 0);
                }
            }
            {
                u64 w = address_block.v[addr_counter % ARGON2_BLOCK_QWORDS];
                J1 = (u32)(w & 0xFFFFFFFFu);
                J2 = (u32)(w >> 32);
            }
            addr_counter++;
        } else {
            u64 w = inst->memory[prev_offset].v[0];
            J1 = (u32)(w & 0xFFFFFFFFu);
            J2 = (u32)(w >> 32);
        }

        if (pass == 0 && slice == 0) {
            ref_lane = lane;
        } else {
            ref_lane = J2 % inst->lanes;
        }
        same_lane = (ref_lane == lane) ? 1 : 0;

        {
            u32 ref_area;
            if (pass == 0) {
                if (slice == 0) {
                    ref_area = i - 1;
                } else if (same_lane) {
                    ref_area = slice * inst->seg + i - 1;
                } else {
                    ref_area = slice * inst->seg - (i == 0 ? 1 : 0);
                }
            } else {
                if (same_lane) {
                    ref_area = inst->q - inst->seg + i - 1;
                } else {
                    ref_area = inst->q - inst->seg - (i == 0 ? 1 : 0);
                }
            }
            {
                u32 rel_pos = index_alpha(J1, ref_area);
                u32 start_position = (pass != 0 && slice != 3)
                    ? ((slice + 1) * inst->seg) : 0;
                ref_index = (start_position + rel_pos) % inst->q;
            }
        }

        {
            argon2_block *prev = &inst->memory[prev_offset];
            argon2_block *ref  = &inst->memory[ref_lane * inst->q + ref_index];
            argon2_block *cur  = &inst->memory[curr_offset];
            int xor_in = (pass > 0) ? 1 : 0;
            compress_G(cur, prev, ref, xor_in);
        }
    }

    return 0;
}

/* ── Finalise ───────────────────────────────────────────────── */

static unsigned long finalise(u8 *tag, u64 tag_len, argon2_inst *inst) {
    argon2_block final_block;
    u8 final_bytes[ARGON2_BLOCK_BYTES];
    u32 lane;
    int i;
    final_block = inst->memory[(inst->lanes - 1) * inst->q + (inst->q - 1)];
    for (lane = 0; lane + 1 < inst->lanes; lane++) {
        argon2_block *last = &inst->memory[lane * inst->q + (inst->q - 1)];
        block_xor_in(&final_block, last);
    }
    for (i = 0; i < ARGON2_BLOCK_QWORDS; i++) {
        store64_le(final_bytes + 8 * i, final_block.v[i]);
    }
    return H_prime(tag, tag_len, final_bytes, ARGON2_BLOCK_BYTES);
}

/* ── Top-level driver ───────────────────────────────────────── */

unsigned long argon2id_hash(u8 *out, u64 tag_len,
                             const u8 *pwd, u64 pwd_len,
                             const u8 *salt, u64 salt_len,
                             u32 t_cost, u32 m_kib, u32 lanes) {
    argon2_inst inst;
    u8 h0[64];
    u32 m_actual, pass, slice, lane;
    unsigned long rc;
    if (!out)         return 1;
    if (tag_len < 4)  return 2;
    if (pwd_len > 0 && !pwd) return 3;
    if (salt_len < 8) return 4;
    if (!salt)        return 5;
    if (t_cost < 1)   return 6;
    if (lanes < 1)    return 7;
    if (m_kib < 8 * lanes) return 8;

    m_actual = (m_kib / (4 * lanes)) * 4 * lanes;
    inst.memory = (argon2_block *)calloc(m_actual, sizeof(argon2_block));
    if (!inst.memory) return 9;
    inst.m_kib    = m_kib;
    inst.m_actual = m_actual;
    inst.lanes    = lanes;
    inst.q        = m_actual / lanes;
    inst.seg      = inst.q / 4;
    inst.t_cost   = t_cost;
    inst.tag_len  = (u32)tag_len;
    inst.type     = ARGON2_TYPE_ID;

    rc = compute_H0(h0, &inst, pwd, pwd_len, salt, salt_len);
    if (rc) { free(inst.memory); return rc; }
    rc = init_blocks(&inst, h0);
    if (rc) { free(inst.memory); return rc; }

    for (pass = 0; pass < t_cost; pass++) {
        for (slice = 0; slice < 4; slice++) {
            for (lane = 0; lane < lanes; lane++) {
                rc = fill_segment(&inst, pass, slice, lane);
                if (rc) { free(inst.memory); return rc; }
            }
        }
    }

    rc = finalise(out, tag_len, &inst);
    memset(inst.memory, 0, (size_t)m_actual * sizeof(argon2_block));
    free(inst.memory);
    return rc;
}

/* ── Base64 (unpadded, standard alphabet) ───────────────────── */

static const char B64_ALPHA[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static u64 b64_encode_nopad(char *out, const u8 *in, u64 len) {
    u64 i = 0, o = 0;
    while (i + 3 <= len) {
        u32 n = ((u32)in[i] << 16) | ((u32)in[i + 1] << 8) | (u32)in[i + 2];
        out[o++] = B64_ALPHA[(n >> 18) & 0x3F];
        out[o++] = B64_ALPHA[(n >> 12) & 0x3F];
        out[o++] = B64_ALPHA[(n >>  6) & 0x3F];
        out[o++] = B64_ALPHA[ n        & 0x3F];
        i += 3;
    }
    if (i + 1 == len) {
        u32 n = (u32)in[i] << 16;
        out[o++] = B64_ALPHA[(n >> 18) & 0x3F];
        out[o++] = B64_ALPHA[(n >> 12) & 0x3F];
    } else if (i + 2 == len) {
        u32 n = ((u32)in[i] << 16) | ((u32)in[i + 1] << 8);
        out[o++] = B64_ALPHA[(n >> 18) & 0x3F];
        out[o++] = B64_ALPHA[(n >> 12) & 0x3F];
        out[o++] = B64_ALPHA[(n >>  6) & 0x3F];
    }
    return o;
}

static int b64_char_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static unsigned long b64_decode_nopad(u8 *out, u64 out_cap,
                                       u64 *out_len,
                                       const char *in, u64 in_len) {
    u32 acc = 0;
    u32 bits = 0;
    u64 i, o = 0;
    for (i = 0; i < in_len; i++) {
        int v = b64_char_value(in[i]);
        if (v < 0) return 1;
        acc = (acc << 6) | (u32)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_cap) return 2;
            out[o++] = (u8)((acc >> bits) & 0xFF);
        }
    }
    *out_len = o;
    return 0;
}

/* ── Encoded string ─────────────────────────────────────────── */

static u64 write_u32_dec(char *out, u32 v) {
    char tmp[16];
    u64 n = 0, i;
    if (v == 0) { out[0] = '0'; return 1; }
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

unsigned long argon2id_hash_encoded(char *out, u64 out_cap,
                                     const u8 *pwd, u64 pwd_len,
                                     const u8 *salt, u64 salt_len,
                                     u64 tag_len,
                                     u32 t_cost, u32 m_kib, u32 lanes) {
    u8 hash[128];
    u64 pos = 0;
    unsigned long rc;
    if (!out) return 1;
    if (tag_len > sizeof(hash)) return 2;
    if (out_cap < 128) return 3;

    rc = argon2id_hash(hash, tag_len, pwd, pwd_len, salt, salt_len,
                       t_cost, m_kib, lanes);
    if (rc) return rc;

    {
        const char *prefix = "$argon2id$v=19$m=";
        u64 plen = strlen(prefix);
        if (pos + plen >= out_cap) return 4;
        memcpy(out + pos, prefix, plen);
        pos += plen;
    }
    pos += write_u32_dec(out + pos, m_kib);
    if (pos + 3 >= out_cap) return 4;
    out[pos++] = ','; out[pos++] = 't'; out[pos++] = '=';
    pos += write_u32_dec(out + pos, t_cost);
    if (pos + 3 >= out_cap) return 4;
    out[pos++] = ','; out[pos++] = 'p'; out[pos++] = '=';
    pos += write_u32_dec(out + pos, lanes);
    if (pos + 1 >= out_cap) return 4;
    out[pos++] = '$';
    pos += b64_encode_nopad(out + pos, salt, salt_len);
    if (pos + 1 >= out_cap) return 4;
    out[pos++] = '$';
    pos += b64_encode_nopad(out + pos, hash, tag_len);
    if (pos >= out_cap) return 4;
    out[pos] = '\0';
    return 0;
}

/* ── Parse + verify ─────────────────────────────────────────── */

static int str_startswith(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int parse_u32(const char **p, u32 *out) {
    u32 v = 0;
    int any = 0;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (u32)(**p - '0');
        (*p)++;
        any = 1;
    }
    *out = v;
    return any ? 0 : -1;
}

unsigned long argon2id_verify(unsigned long *ok, const char *encoded,
                               const u8 *pwd, u64 pwd_len) {
    const char *p = encoded;
    u32 m_kib = 0, t_cost = 0, lanes = 0;
    u8 salt_bin[64];
    u64 salt_len = 0;
    u8 hash_bin[128];
    u64 hash_len = 0;
    u8 computed[128];
    unsigned long rc;
    u64 i;
    u32 diff = 0;

    if (!ok) return 1;
    *ok = 0;
    if (!encoded) return 2;
    if (!str_startswith(p, "$argon2id$v=19$m=")) return 3;
    p += strlen("$argon2id$v=19$m=");
    if (parse_u32(&p, &m_kib)) return 4;
    if (*p++ != ',') return 4;
    if (*p++ != 't') return 4;
    if (*p++ != '=') return 4;
    if (parse_u32(&p, &t_cost)) return 4;
    if (*p++ != ',') return 4;
    if (*p++ != 'p') return 4;
    if (*p++ != '=') return 4;
    if (parse_u32(&p, &lanes)) return 4;
    if (*p++ != '$') return 4;

    {
        const char *salt_start = p;
        while (*p && *p != '$') p++;
        if (*p != '$') return 4;
        rc = b64_decode_nopad(salt_bin, sizeof(salt_bin), &salt_len,
                              salt_start, (u64)(p - salt_start));
        if (rc) return 5;
        p++;
    }
    {
        const char *hash_start = p;
        while (*p) p++;
        rc = b64_decode_nopad(hash_bin, sizeof(hash_bin), &hash_len,
                              hash_start, (u64)(p - hash_start));
        if (rc) return 5;
    }

    rc = argon2id_hash(computed, hash_len, pwd, pwd_len,
                       salt_bin, salt_len, t_cost, m_kib, lanes);
    if (rc) return rc;

    for (i = 0; i < hash_len; i++) diff |= computed[i] ^ hash_bin[i];
    *ok = (diff == 0) ? 1 : 0;
    memset(computed, 0, sizeof(computed));
    return 0;
}

/* ── libsodium-INTERACTIVE convenience ──────────────────────── */

unsigned long argon2id_hash_interactive(char *out, u64 out_cap,
                                         const u8 *pwd, u64 pwd_len,
                                         const u8 salt16[16]) {
    return argon2id_hash_encoded(out, out_cap, pwd, pwd_len,
                                  salt16, 16,
                                  32,
                                  2,
                                  65536,
                                  1);
}
