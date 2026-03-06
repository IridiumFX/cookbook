#include "cookbook_auth.h"
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ==== Base64url ==== */

static const char b64url_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t cookbook_base64url_encode(const void *src, size_t src_len,
                                 char *dst, size_t dst_len) {
    const unsigned char *s = (const unsigned char *)src;
    size_t out = 0;
    size_t i;

    for (i = 0; i + 2 < src_len; i += 3) {
        if (out + 4 > dst_len) return 0;
        uint32_t v = ((uint32_t)s[i] << 16) | ((uint32_t)s[i+1] << 8) | s[i+2];
        dst[out++] = b64url_chars[(v >> 18) & 0x3f];
        dst[out++] = b64url_chars[(v >> 12) & 0x3f];
        dst[out++] = b64url_chars[(v >>  6) & 0x3f];
        dst[out++] = b64url_chars[v & 0x3f];
    }
    if (i < src_len) {
        if (out + 4 > dst_len) return 0;
        uint32_t v = (uint32_t)s[i] << 16;
        if (i + 1 < src_len) v |= (uint32_t)s[i+1] << 8;
        dst[out++] = b64url_chars[(v >> 18) & 0x3f];
        dst[out++] = b64url_chars[(v >> 12) & 0x3f];
        if (i + 1 < src_len)
            dst[out++] = b64url_chars[(v >> 6) & 0x3f];
    }
    if (out < dst_len) dst[out] = '\0';
    return out;
}

static int b64url_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

size_t cookbook_base64url_decode(const char *src, size_t src_len,
                                 void *dst, size_t dst_len) {
    unsigned char *d = (unsigned char *)dst;
    size_t out = 0;
    size_t i = 0;

    /* strip padding if present */
    while (src_len > 0 && src[src_len - 1] == '=') src_len--;

    while (i < src_len) {
        int v0 = (i < src_len) ? b64url_val(src[i++]) : 0;
        int v1 = (i < src_len) ? b64url_val(src[i++]) : 0;
        int v2 = (i < src_len) ? b64url_val(src[i++]) : -1;
        int v3 = (i < src_len) ? b64url_val(src[i++]) : -1;

        if (v0 < 0 || v1 < 0) return 0;

        if (out < dst_len) d[out++] = (unsigned char)((v0 << 2) | (v1 >> 4));
        if (v2 >= 0 && out < dst_len)
            d[out++] = (unsigned char)(((v1 & 0xf) << 4) | (v2 >> 2));
        if (v3 >= 0 && out < dst_len)
            d[out++] = (unsigned char)(((v2 & 0x3) << 6) | v3);
    }
    return out;
}

/* ==== Minimal JSON helpers (for JWT only) ==== */

/* Write a JSON string value, escaping as needed. Returns chars written. */
static int json_write_string(char *buf, size_t sz, const char *key,
                              const char *val, int first) {
    return snprintf(buf, sz, "%s\"%s\":\"%s\"", first ? "" : ",", key, val);
}

static int json_write_int(char *buf, size_t sz, const char *key,
                            int64_t val, int first) {
    return snprintf(buf, sz, "%s\"%s\":%lld", first ? "" : ",", key,
                    (long long)val);
}

/* Extract a JSON string value for a given key (minimal parser). */
static int json_get_string(const char *json, const char *key,
                            char *out, size_t out_sz) {
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *start = strstr(json, pattern);
    if (!start) return -1;
    start += strlen(pattern);
    const char *end = strchr(start, '"');
    if (!end) return -1;
    size_t len = (size_t)(end - start);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

static int json_get_int(const char *json, const char *key, int64_t *out) {
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *start = strstr(json, pattern);
    if (!start) return -1;
    start += strlen(pattern);
    *out = strtoll(start, NULL, 10);
    return 0;
}

/* ==== JWT ==== */

/* JWT header for EdDSA (Ed25519). Fixed. */
static const char JWT_HEADER[] = "{\"alg\":\"EdDSA\",\"typ\":\"JWT\"}";

char *cookbook_jwt_create(const char *sub, const char *groups,
                           int64_t ttl_sec,
                           const unsigned char signing_key[64]) {
    if (!sub || !signing_key) return NULL;

    int64_t now = (int64_t)time(NULL);
    int64_t exp = now + ttl_sec;

    /* build payload JSON */
    char payload[2048];
    int off = 0;
    off += snprintf(payload + off, sizeof(payload) - (size_t)off, "{");
    off += json_write_string(payload + off, sizeof(payload) - (size_t)off,
                              "sub", sub, 1);
    if (groups && *groups)
        off += json_write_string(payload + off, sizeof(payload) - (size_t)off,
                                  "groups", groups, 0);
    off += json_write_int(payload + off, sizeof(payload) - (size_t)off,
                           "iat", now, 0);
    off += json_write_int(payload + off, sizeof(payload) - (size_t)off,
                           "exp", exp, 0);
    snprintf(payload + off, sizeof(payload) - (size_t)off, "}");

    /* base64url encode header and payload */
    char hdr_b64[256], pay_b64[4096];
    size_t hdr_len = cookbook_base64url_encode(JWT_HEADER, strlen(JWT_HEADER),
                                               hdr_b64, sizeof(hdr_b64));
    size_t pay_len = cookbook_base64url_encode(payload, strlen(payload),
                                               pay_b64, sizeof(pay_b64));
    if (hdr_len == 0 || pay_len == 0) return NULL;

    /* build signing input: header.payload */
    char signing_input[4096 + 256];
    int si_len = snprintf(signing_input, sizeof(signing_input),
                           "%s.%s", hdr_b64, pay_b64);

    /* sign with Ed25519 */
    unsigned char sig[64];
    if (crypto_sign_ed25519_detached(sig, NULL,
                                      (const unsigned char *)signing_input,
                                      (unsigned long long)si_len,
                                      signing_key) != 0)
        return NULL;

    /* base64url encode signature */
    char sig_b64[128];
    size_t sig_len = cookbook_base64url_encode(sig, 64, sig_b64, sizeof(sig_b64));
    if (sig_len == 0) return NULL;

    /* assemble token */
    size_t token_len = (size_t)si_len + 1 + sig_len + 1;
    char *token = malloc(token_len);
    if (!token) return NULL;
    snprintf(token, token_len, "%s.%s", signing_input, sig_b64);
    return token;
}

int cookbook_jwt_verify(const char *token, const unsigned char verify_key[32],
                        cookbook_jwt_claims *claims) {
    if (!token || !verify_key || !claims) return -1;
    memset(claims, 0, sizeof(*claims));

    /* find the two dots */
    const char *dot1 = strchr(token, '.');
    if (!dot1) return -1;
    const char *dot2 = strchr(dot1 + 1, '.');
    if (!dot2) return -1;

    /* signing input = everything before the second dot */
    size_t si_len = (size_t)(dot2 - token);

    /* decode signature */
    const char *sig_b64 = dot2 + 1;
    size_t sig_b64_len = strlen(sig_b64);
    unsigned char sig[64];
    size_t sig_dec_len = cookbook_base64url_decode(sig_b64, sig_b64_len,
                                                    sig, sizeof(sig));
    if (sig_dec_len != 64) return -1;

    /* verify Ed25519 signature */
    if (crypto_sign_ed25519_verify_detached(sig,
                                             (const unsigned char *)token,
                                             (unsigned long long)si_len,
                                             verify_key) != 0)
        return -1;

    /* decode payload */
    const char *pay_b64 = dot1 + 1;
    size_t pay_b64_len = (size_t)(dot2 - pay_b64);
    char payload[4096];
    size_t pay_len = cookbook_base64url_decode(pay_b64, pay_b64_len,
                                               payload, sizeof(payload) - 1);
    if (pay_len == 0) return -1;
    payload[pay_len] = '\0';

    /* extract claims */
    json_get_string(payload, "sub", claims->sub, sizeof(claims->sub));
    json_get_string(payload, "groups", claims->groups, sizeof(claims->groups));
    json_get_int(payload, "exp", &claims->exp);
    json_get_int(payload, "iat", &claims->iat);

    /* check expiration */
    int64_t now = (int64_t)time(NULL);
    if (claims->exp > 0 && now > claims->exp) return -1;

    claims->valid = 1;
    return 0;
}

int cookbook_jwt_has_group(const cookbook_jwt_claims *claims, const char *group) {
    if (!claims || !group || !claims->groups[0]) return 0;

    size_t glen = strlen(group);
    const char *p = claims->groups;

    while (*p) {
        const char *comma = strchr(p, ',');
        size_t seg_len = comma ? (size_t)(comma - p) : strlen(p);

        if (seg_len == glen && memcmp(p, group, glen) == 0)
            return 1;

        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

/* ==== Ed25519 key/sign/verify ==== */

int cookbook_keygen(unsigned char pk[32], unsigned char sk[64]) {
    if (sodium_init() < 0) return -1;
    crypto_sign_ed25519_keypair(pk, sk);
    return 0;
}

int cookbook_sign(const void *msg, size_t msg_len,
                  unsigned char sig[64],
                  const unsigned char sk[64]) {
    return crypto_sign_ed25519_detached(sig, NULL,
                                         (const unsigned char *)msg,
                                         (unsigned long long)msg_len,
                                         sk);
}

int cookbook_verify(const void *msg, size_t msg_len,
                    const unsigned char sig[64],
                    const unsigned char pk[32]) {
    return crypto_sign_ed25519_verify_detached(sig,
                                                (const unsigned char *)msg,
                                                (unsigned long long)msg_len,
                                                pk);
}
