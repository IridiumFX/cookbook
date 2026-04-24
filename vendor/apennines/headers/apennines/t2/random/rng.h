#ifndef APENNINES_T2_RNG_H
#define APENNINES_T2_RNG_H

#include "apennines/export.h"
#include "apennines/types.h"

typedef struct { u8 state[48]; } csprng;
typedef struct { u64 s[2]; } xorshift128;
typedef struct { u64 s; } splitmix64;
typedef struct { u64 state; u64 inc; } pcg32;

APENNINES_API unsigned long csprng_create(csprng *out);
APENNINES_API unsigned long csprng_fill(u8 *out, u64 len, csprng *rng);
APENNINES_API unsigned long csprng_u64(u64 *out, csprng *rng);
APENNINES_API unsigned long xorshift128_create(xorshift128 *out, u64 seed);
APENNINES_API unsigned long xorshift128_next(u64 *out, xorshift128 *rng);
APENNINES_API unsigned long xorshift128_fill(u8 *out, u64 len, xorshift128 *rng);
APENNINES_API unsigned long splitmix64_create(splitmix64 *out, u64 seed);
APENNINES_API unsigned long splitmix64_next(u64 *out, splitmix64 *rng);
APENNINES_API unsigned long pcg32_create(pcg32 *out, u64 seed);
APENNINES_API unsigned long pcg32_next(u32 *out, pcg32 *rng);
APENNINES_API unsigned long pcg32_bounded(u32 *out, pcg32 *rng, u32 bound);

#endif /* APENNINES_T2_RNG_H */
