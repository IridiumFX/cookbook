#ifndef COOKBOOK_AUTH_H
#define COOKBOOK_AUTH_H

#include "cookbook.h"
#include <stddef.h>
#include <stdint.h>

/* Base64url encode/decode (no padding). */
COOKBOOK_API size_t cookbook_base64url_encode(const void *src, size_t src_len,
                                             char *dst, size_t dst_len);
COOKBOOK_API size_t cookbook_base64url_decode(const char *src, size_t src_len,
                                             void *dst, size_t dst_len);

/* JWT claims extracted after verification. */
typedef struct {
    char sub[128];          /* subject (user/service ID) */
    char groups[1024];      /* comma-separated group list */
    int64_t exp;            /* expiration (unix timestamp) */
    int64_t iat;            /* issued-at (unix timestamp) */
    int valid;              /* 1 if signature and expiry are valid */
} cookbook_jwt_claims;

/* Create a signed JWT. Returns malloc'd token string, caller must free.
   signing_key must be 64-byte Ed25519 secret key.
   groups is a comma-separated list like "org.acme,org.beta". */
COOKBOOK_API char *cookbook_jwt_create(const char *sub, const char *groups,
                                      int64_t ttl_sec,
                                      const unsigned char signing_key[64]);

/* Verify and decode a JWT. Returns 0 on success, -1 on failure.
   verify_key must be 32-byte Ed25519 public key. */
COOKBOOK_API int cookbook_jwt_verify(const char *token,
                                    const unsigned char verify_key[32],
                                    cookbook_jwt_claims *claims);

/* Check if a JWT's groups claim contains the given group.
   Returns 1 if yes, 0 if no. */
COOKBOOK_API int cookbook_jwt_has_group(const cookbook_jwt_claims *claims,
                                       const char *group);

/* Ed25519 key pair generation. */
COOKBOOK_API int cookbook_keygen(unsigned char pk[32], unsigned char sk[64]);

/* Ed25519 sign/verify for .sig files. */
COOKBOOK_API int cookbook_sign(const void *msg, size_t msg_len,
                              unsigned char sig[64],
                              const unsigned char sk[64]);

COOKBOOK_API int cookbook_verify(const void *msg, size_t msg_len,
                                const unsigned char sig[64],
                                const unsigned char pk[32]);

#endif /* COOKBOOK_AUTH_H */
