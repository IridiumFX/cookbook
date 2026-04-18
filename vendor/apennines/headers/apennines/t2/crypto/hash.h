#ifndef APENNINES_T2_CRYPTO_HASH_H
#define APENNINES_T2_CRYPTO_HASH_H

#include "apennines/export.h"
#include "apennines/types.h"

/* ---- SHA-256 ---- */

typedef struct {
    u32 state[8];
    u64 count;
    u8  buf[64];
} sha256_ctx;

APENNINES_API unsigned long sha256_init(sha256_ctx *ctx);
APENNINES_API unsigned long sha256_update(sha256_ctx *ctx, const u8 *data, u64 len);
APENNINES_API unsigned long sha256_final(u8 *out, sha256_ctx *ctx);
APENNINES_API unsigned long sha256_digest(u8 *out, const u8 *data, u64 len);

/* ---- SHA-512 ---- */

typedef struct {
    u64 state[8];
    u64 count_lo;
    u64 count_hi;
    u8  buf[128];
} sha512_ctx;

APENNINES_API unsigned long sha512_init(sha512_ctx *ctx);
APENNINES_API unsigned long sha512_update(sha512_ctx *ctx, const u8 *data, u64 len);
APENNINES_API unsigned long sha512_final(u8 *out, sha512_ctx *ctx);
APENNINES_API unsigned long sha512_digest(u8 *out, const u8 *data, u64 len);

/* ---- HMAC ---- */

#define HMAC_HASH_SHA256 0
#define HMAC_HASH_SHA512 1

typedef struct {
    unsigned long hash_id;
    union {
        sha256_ctx sha256;
        sha512_ctx sha512;
    } inner;
    union {
        sha256_ctx sha256;
        sha512_ctx sha512;
    } outer;
} hmac_ctx;

APENNINES_API unsigned long hmac_create(hmac_ctx *ctx, unsigned long hash_id, const u8 *key, u64 key_len);
APENNINES_API unsigned long hmac_update(hmac_ctx *ctx, const u8 *data, u64 len);
APENNINES_API unsigned long hmac_final(u8 *out, hmac_ctx *ctx);
APENNINES_API unsigned long hmac_digest(u8 *out, unsigned long hash_id, const u8 *key, u64 key_len, const u8 *data, u64 data_len);
APENNINES_API unsigned long hmac_verify(unsigned long *result, const u8 *expected, unsigned long hash_id, const u8 *key, u64 key_len, const u8 *data, u64 data_len);

/* ---- HKDF (RFC 5869) ---- */

APENNINES_API unsigned long hkdf_extract(u8 *prk, unsigned long hash_id, const u8 *salt, u64 salt_len, const u8 *ikm, u64 ikm_len);
APENNINES_API unsigned long hkdf_expand(u8 *okm, u64 okm_len, unsigned long hash_id, const u8 *prk, u64 prk_len, const u8 *info, u64 info_len);
APENNINES_API unsigned long hkdf_derive(u8 *okm, u64 okm_len, unsigned long hash_id, const u8 *salt, u64 salt_len, const u8 *ikm, u64 ikm_len, const u8 *info, u64 info_len);

/* ---- scrypt (RFC 7914) ----
 * Memory-hard password-based KDF.
 *   out, out_len:          derived key output (caller-allocated, 1..2^32-1 * 32 bytes)
 *   password, password_len: user password
 *   salt, salt_len:        salt (recommended >= 16 bytes)
 *   N:                     CPU/memory cost (power of two, >= 2)
 *   r:                     block size in 64-byte units (e.g. 8)
 *   p:                     parallelisation factor (e.g. 1)
 * Hatches: 1=null out, 2=null password, 3=null salt, 4=invalid N,
 *          5=invalid r or p, 6=alloc fail */
APENNINES_API unsigned long scrypt_derive(u8 *out, u64 out_len,
                                          const u8 *password, u64 password_len,
                                          const u8 *salt, u64 salt_len,
                                          u64 N, u32 r, u32 p);

#endif /* APENNINES_T2_CRYPTO_HASH_H */
