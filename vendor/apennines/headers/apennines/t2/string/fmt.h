#ifndef APENNINES_T2_FMT_H
#define APENNINES_T2_FMT_H

#include "apennines/export.h"
#include "apennines/types.h"
#include "apennines/t1/buffer/buf.h"

APENNINES_API unsigned long fmt_u64(buf *out, u64 val);
APENNINES_API unsigned long fmt_i64(buf *out, i64 val);
APENNINES_API unsigned long fmt_u64_hex(buf *out, u64 val);
APENNINES_API unsigned long fmt_u64_bin(buf *out, u64 val);
APENNINES_API unsigned long fmt_f64(buf *out, double val, u32 precision);
APENNINES_API unsigned long fmt_bool(buf *out, unsigned long val);
APENNINES_API unsigned long fmt_bytes_hex(buf *out, u8 *data, u64 len);
APENNINES_API unsigned long fmt_write(buf *out, const char *fmt, ...);

APENNINES_API unsigned long parse_u64(u64 *out, u8 *data, u64 len);
APENNINES_API unsigned long parse_i64(i64 *out, u8 *data, u64 len);
APENNINES_API unsigned long parse_u64_hex(u64 *out, u8 *data, u64 len);
APENNINES_API unsigned long parse_f64(double *out, u8 *data, u64 len);
APENNINES_API unsigned long parse_bool(unsigned long *out, u8 *data, u64 len);
APENNINES_API unsigned long parse_bytes_hex(u8 *out, u64 *out_len, u8 *data, u64 len);

#endif /* APENNINES_T2_FMT_H */
