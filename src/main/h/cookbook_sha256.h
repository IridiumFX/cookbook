#ifndef COOKBOOK_SHA256_H
#define COOKBOOK_SHA256_H

#include "cookbook.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buf[64];
} cookbook_sha256_ctx;

COOKBOOK_API void cookbook_sha256_init(cookbook_sha256_ctx *ctx);
COOKBOOK_API void cookbook_sha256_update(cookbook_sha256_ctx *ctx,
                                       const void *data, size_t len);
COOKBOOK_API void cookbook_sha256_final(cookbook_sha256_ctx *ctx,
                                      uint8_t digest[32]);

/* Convenience: hash data in one call, write 64-char hex + NUL to hex_out. */
COOKBOOK_API void cookbook_sha256_hex(const void *data, size_t len,
                                     char hex_out[65]);

#endif /* COOKBOOK_SHA256_H */
