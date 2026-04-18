#ifndef APENNINES_T2_CRYPTO_BLAKE2B_H
#define APENNINES_T2_CRYPTO_BLAKE2B_H
#include "apennines/export.h"
#include "apennines/types.h"

/* BLAKE2b — RFC 7693. Unkeyed, output length 1..64 bytes.
 *
 * Streaming API: init → update* → final. The one-shot `blake2b_digest`
 * collapses all three for callers who have all the input in one buffer. */

#define BLAKE2B_BLOCKBYTES 128
#define BLAKE2B_OUTBYTES    64

typedef struct {
    u64  h[8];
    u64  t[2];              /* 128-bit byte counter */
    u64  f[2];              /* finalization flags */
    u8   buf[BLAKE2B_BLOCKBYTES];
    u64  buflen;
    u32  outlen;
} blake2b_state;

/* Initialise a state for an `outlen`-byte output (1..64). */
APENNINES_API unsigned long blake2b_init(blake2b_state *s, u32 outlen);

/* Feed arbitrary bytes. May be called repeatedly. */
APENNINES_API unsigned long blake2b_update(blake2b_state *s,
                                           const u8 *data, u64 len);

/* Finalise and write the digest into `out`. The state is not usable for
 * further updates after this. */
APENNINES_API unsigned long blake2b_final(u8 *out, blake2b_state *s);

/* One-shot: hash `data` into a caller-sized output (1..64 bytes). */
APENNINES_API unsigned long blake2b_digest(u8 *out, u32 outlen,
                                           const u8 *data, u64 len);

#endif /* APENNINES_T2_CRYPTO_BLAKE2B_H */
