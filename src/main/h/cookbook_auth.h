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
    char groups[1024];      /* comma-separated group list (v1 compat) */
    char jti[64];           /* JWT ID (unique token identifier for revocation) */
    char *grants_json;      /* malloc'd: resolved grants JSON (v2), NULL if v1 */
    char *exclude_json;     /* malloc'd: resolved exclude JSON (v2), NULL if v1 */
    int64_t exp;            /* expiration (unix timestamp) */
    int64_t iat;            /* issued-at (unix timestamp) */
    int version;            /* JWT version: 1 (legacy) or 2 (policy-based) */
    int valid;              /* 1 if signature and expiry are valid */
} cookbook_jwt_claims;

/* ==== Token revocation ==== */

/* Bounded revocation list. Entries auto-expire when the JWT expires. */
typedef struct {
    char   jti[64];
    int64_t exp;   /* when this revocation entry expires */
} cookbook_revocation_entry;

typedef struct {
    cookbook_revocation_entry *entries;
    int count;
    int capacity;
} cookbook_revocation_list;

/* Initialize a revocation list with given capacity. */
COOKBOOK_API void cookbook_revocation_init(cookbook_revocation_list *rl, int capacity);

/* Free the revocation list. */
COOKBOOK_API void cookbook_revocation_free(cookbook_revocation_list *rl);

/* Add a jti to the revocation list. Expired entries are pruned on insert. */
COOKBOOK_API int cookbook_revocation_add(cookbook_revocation_list *rl,
                                        const char *jti, int64_t exp);

/* Check if a jti is revoked. Returns 1 if revoked, 0 if not. */
COOKBOOK_API int cookbook_revocation_check(const cookbook_revocation_list *rl,
                                           const char *jti);

/* Free malloc'd fields in claims (grants_json, exclude_json). */
COOKBOOK_API void cookbook_jwt_claims_free(cookbook_jwt_claims *claims);

/* Create a signed JWT (v1, legacy). Returns malloc'd token string.
   signing_key must be 64-byte Ed25519 secret key.
   groups is a comma-separated list like "org.acme,org.beta". */
COOKBOOK_API char *cookbook_jwt_create(const char *sub, const char *groups,
                                      int64_t ttl_sec,
                                      const unsigned char signing_key[64]);

/* Create a signed JWT v2 with embedded grants/exclude.
   resolved_json is the output of cookbook_policy_resolve() — a JSON object
   with "grants" and "exclude" sub-objects.  groups may be NULL.
   Returns malloc'd token string, caller must free. */
COOKBOOK_API char *cookbook_jwt_create_v2(const char *sub, const char *groups,
                                         const char *resolved_json,
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

/* Standard base64 decode (for Authorization: Basic header).
   Returns decoded length, 0 on error. */
COOKBOOK_API size_t cookbook_base64_decode(const char *src, size_t src_len,
                                          void *dst, size_t dst_len);

/* Hash a credential token for storage (Argon2id via apennines, RFC 9106).
   Returns malloc'd hash string, caller must free. */
COOKBOOK_API char *cookbook_credential_hash(const char *token);

/* Verify a token against a stored hash. Returns 0 on match, -1 on mismatch. */
COOKBOOK_API int cookbook_credential_verify(const char *token, const char *hash);

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
