#ifndef APENNINES_T2_CRYPTO_CT_H
#define APENNINES_T2_CRYPTO_CT_H

#include "apennines/export.h"
#include "apennines/types.h"

APENNINES_API unsigned long ct_compare(unsigned long *result, const u8 *a, const u8 *b, u64 len);
APENNINES_API unsigned long ct_select(u8 *out, const u8 *a, const u8 *b, u64 len, unsigned long selector);
APENNINES_API unsigned long ct_is_zero(unsigned long *result, const u8 *data, u64 len);
APENNINES_API unsigned long ct_copy_if(u8 *dst, const u8 *src, u64 len, unsigned long condition);

#endif /* APENNINES_T2_CRYPTO_CT_H */
