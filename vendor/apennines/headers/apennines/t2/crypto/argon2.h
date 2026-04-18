#ifndef APENNINES_T2_CRYPTO_ARGON2_H
#define APENNINES_T2_CRYPTO_ARGON2_H
#include "apennines/export.h"
#include "apennines/types.h"

/* RFC 9106 — Argon2id password hashing function.
 *
 * Hybrid of Argon2i (data-independent addressing, side-channel resistant
 * on the first half of the first pass — important for passwords) and
 * Argon2d (data-dependent addressing, GPU-resistant — everywhere else).
 *
 * Three use modes:
 *   1. Raw hash — caller specifies all parameters and salt.
 *   2. Encoded string — libsodium-compatible output:
 *      "$argon2id$v=19$m=<m>,t=<t>,p=<p>$<salt_b64>$<hash_b64>"
 *      with unpadded standard base64.
 *   3. Verify — parses an encoded string and constant-time checks.
 *
 * Cookbook consumes (2) and (3) directly in place of libsodium's
 * crypto_pwhash_str / crypto_pwhash_str_verify. */

/* ── raw hash ───────────────────────────────────────────────── */

/* out       — caller-allocated, tag_len bytes
 * tag_len   — 4 to 2^32-1 bytes; 32 for the common case
 * pwd       — password / passphrase (may be 0 length)
 * salt      — at least 8 bytes; 16 is the libsodium default
 * t_cost    — number of passes (≥ 1; libsodium INTERACTIVE = 2)
 * m_kib     — memory in KiB (must be ≥ 8 * parallelism; libsodium
 *             INTERACTIVE = 65536 = 64 MiB)
 * lanes     — parallelism p (1 is fine; libsodium default). */
APENNINES_API unsigned long argon2id_hash(u8 *out, u64 tag_len,
                                           const u8 *pwd, u64 pwd_len,
                                           const u8 *salt, u64 salt_len,
                                           u32 t_cost, u32 m_kib,
                                           u32 lanes);

/* ── encoded string (libsodium-compatible) ──────────────────── */

/* out_cap must be at least 128 to hold the default-parameter string. */
APENNINES_API unsigned long argon2id_hash_encoded(char *out, u64 out_cap,
                                                   const u8 *pwd, u64 pwd_len,
                                                   const u8 *salt, u64 salt_len,
                                                   u64 tag_len,
                                                   u32 t_cost, u32 m_kib,
                                                   u32 lanes);

/* Verify `pwd` against `encoded`. On success *ok = 1; on mismatch *ok = 0
 * (the return itself is 0 in both cases unless the string is malformed
 * or parameters are out of range). */
APENNINES_API unsigned long argon2id_verify(unsigned long *ok,
                                             const char *encoded,
                                             const u8 *pwd, u64 pwd_len);

/* Convenience: libsodium-INTERACTIVE defaults (t=2, m=65536, p=1,
 * tag=32, salt=16). Caller provides a 16-byte salt. */
APENNINES_API unsigned long argon2id_hash_interactive(char *out, u64 out_cap,
                                                       const u8 *pwd, u64 pwd_len,
                                                       const u8 salt[16]);

#endif /* APENNINES_T2_CRYPTO_ARGON2_H */
