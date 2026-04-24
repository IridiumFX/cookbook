#ifndef APENNINES_T1_UTF8_H
#define APENNINES_T1_UTF8_H

#include "apennines/export.h"
#include "apennines/types.h"

APENNINES_API unsigned long utf8_len_from_byte(u64 *out, u8 lead);
APENNINES_API unsigned long utf8_decode_one(u32 *codepoint, u64 *advance, u8 *data, u64 len);
APENNINES_API unsigned long utf8_encode_one(u64 *bytes_written, u8 *out, u64 out_cap, u32 codepoint);
APENNINES_API unsigned long utf8_validate(u64 *invalid_offset, u8 *data, u64 len);
APENNINES_API unsigned long utf8_codepoint_count(u64 *count, u8 *data, u64 len);

#endif /* APENNINES_T1_UTF8_H */
