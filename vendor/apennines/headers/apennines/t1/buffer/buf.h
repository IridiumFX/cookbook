#ifndef APENNINES_T1_BUF_H
#define APENNINES_T1_BUF_H

#include "apennines/export.h"
#include "apennines/types.h"

typedef struct {
    u8 *data;
    u64 len;
    u64 cap;
    u8 is_static;
} buf;

APENNINES_API unsigned long buf_create(buf *out, u64 capacity);
APENNINES_API unsigned long buf_from_static(buf *out, u8 *data, u64 len);
APENNINES_API unsigned long buf_append(buf *b, u8 *data, u64 len);
APENNINES_API unsigned long buf_append_byte(buf *b, u8 byte);
APENNINES_API unsigned long buf_insert(buf *b, u64 offset, u8 *data, u64 len);
APENNINES_API unsigned long buf_remove(buf *b, u64 offset, u64 len);
APENNINES_API unsigned long buf_slice(u8 **out, u64 *out_len, buf *b, u64 offset, u64 len);
APENNINES_API unsigned long buf_truncate(buf *b, u64 len);
APENNINES_API unsigned long buf_reserve(buf *b, u64 capacity);
APENNINES_API unsigned long buf_shrink(buf *b);
APENNINES_API unsigned long buf_clear(buf *b);
APENNINES_API unsigned long buf_compare(long *result, buf *a, buf *b);
APENNINES_API unsigned long buf_clone(buf *out, buf *src);
APENNINES_API unsigned long buf_destroy(buf *b);
APENNINES_API unsigned long buf_len(u64 *out, buf *b);
APENNINES_API unsigned long buf_cap(u64 *out, buf *b);
APENNINES_API unsigned long buf_ptr(u8 **out, buf *b);

#endif /* APENNINES_T1_BUF_H */
