#ifndef APENNINES_T2_COMPRESS_H
#define APENNINES_T2_COMPRESS_H

#include "apennines/export.h"
#include "apennines/types.h"
#include "apennines/t1/buffer/buf.h"

typedef enum {
    COMPRESS_LEVEL_FAST    = 1,
    COMPRESS_LEVEL_DEFAULT = 6,
    COMPRESS_LEVEL_BEST    = 9
} compress_level;

APENNINES_API unsigned long lz4_compress(buf *out, u8 *data, u64 len);
APENNINES_API unsigned long lz4_decompress(buf *out, u8 *data, u64 len);

APENNINES_API unsigned long deflate_compress(buf *out, u8 *data, u64 len, compress_level level);
APENNINES_API unsigned long deflate_decompress(buf *out, u8 *data, u64 len);

#endif /* APENNINES_T2_COMPRESS_H */
