#ifndef APENNINES_T2_CRYPTO_CT_H
#define APENNINES_T2_CRYPTO_CT_H

#include "apennines/export.h"
#include "apennines/types.h"

APENNINES_API unsigned long ct_compare(unsigned long *result, const u8 *a, const u8 *b, u64 len);
APENNINES_API unsigned long ct_select(u8 *out, const u8 *a, const u8 *b, u64 len, unsigned long selector);
APENNINES_API unsigned long ct_is_zero(unsigned long *result, const u8 *data, u64 len);
APENNINES_API unsigned long ct_copy_if(u8 *dst, const u8 *src, u64 len, unsigned long condition);

/* Wipe `len` bytes at `data` — the write is through a volatile pointer
 * so the compiler cannot optimise it away (as plain memset would be
 * eligible to be elided if `data` is dead-after-call). Drop-in
 * replacement for libsodium's sodium_memzero. */
APENNINES_API unsigned long ct_memzero(void *data, u64 len);

#endif /* APENNINES_T2_CRYPTO_CT_H */
