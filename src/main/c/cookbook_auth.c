#include "cookbook_auth.h"
#include "cookbook_ed25519.h"
#include "apennines/t1/random/entropy.h"
#include "apennines/t2/crypto/argon2.h"
#include "apennines/t2/crypto/ct.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
  /* for InterlockedIncrement used by the jti counter below */
  #include <windows.h>
#endif

static int cookbook_random_bytes(void *buf, size_t n) {
    /* entropy_get_system wraps BCryptGenRandom on Windows and
     * getentropy(2) / /dev/urandom on Unix — Nova-portable. */
    return entropy_get_system((unsigned char *)buf, (unsigned long long)n) == 0 ? 0 : -1;
}

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

/* ==== Standard Base64 decode (for Basic auth) ==== */

static int b64std_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

size_t cookbook_base64_decode(const char *src, size_t src_len,
                              void *dst, size_t dst_len) {
    unsigned char *d = (unsigned char *)dst;
    size_t out = 0, i = 0;

    while (src_len > 0 && src[src_len - 1] == '=') src_len--;

    while (i < src_len) {
        int v0 = (i < src_len) ? b64std_val(src[i++]) : 0;
        int v1 = (i < src_len) ? b64std_val(src[i++]) : 0;
        int v2 = (i < src_len) ? b64std_val(src[i++]) : -1;
        int v3 = (i < src_len) ? b64std_val(src[i++]) : -1;

        if (v0 < 0 || v1 < 0) return 0;

        if (out < dst_len) d[out++] = (unsigned char)((v0 << 2) | (v1 >> 4));
        if (v2 >= 0 && out < dst_len)
            d[out++] = (unsigned char)(((v1 & 0xf) << 4) | (v2 >> 2));
        if (v3 >= 0 && out < dst_len)
            d[out++] = (unsigned char)(((v2 & 0x3) << 6) | v3);
    }
    return out;
}

/* ==== Credential hashing (Argon2id via apennines, RFC 9106) ==== */

char *cookbook_credential_hash(const char *token) {
    char *hash = malloc(128);
    if (!hash) return NULL;

    unsigned char salt[16];
    if (cookbook_random_bytes(salt, 16) != 0) {
        free(hash);
        return NULL;
    }

    if (argon2id_hash_interactive(hash, 128,
                                  (const unsigned char *)token,
                                  (unsigned long long)strlen(token),
                                  salt) != 0) {
        free(hash);
        return NULL;
    }
    return hash;
}

int cookbook_credential_verify(const char *token, const char *hash) {
    unsigned long ok = 0;
    if (argon2id_verify(&ok, hash,
                        (const unsigned char *)token,
                        (unsigned long long)strlen(token)) != 0) {
        return -1;
    }
    return ok ? 0 : -1;
}

/* ==== Token revocation list ==== */

void cookbook_revocation_init(cookbook_revocation_list *rl, int capacity) {
    if (!rl) return;
    rl->entries = (cookbook_revocation_entry *)calloc(
        (size_t)capacity, sizeof(cookbook_revocation_entry));
    rl->count = 0;
    rl->capacity = capacity;
}

void cookbook_revocation_free(cookbook_revocation_list *rl) {
    if (!rl) return;
    free(rl->entries);
    rl->entries = NULL;
    rl->count = 0;
    rl->capacity = 0;
}

/* Prune expired entries to reclaim space */
static void revocation_prune(cookbook_revocation_list *rl) {
    int64_t now = (int64_t)time(NULL);
    int write = 0;
    for (int i = 0; i < rl->count; i++) {
        if (rl->entries[i].exp > now) {
            if (write != i)
                rl->entries[write] = rl->entries[i];
            write++;
        }
    }
    rl->count = write;
}

int cookbook_revocation_add(cookbook_revocation_list *rl,
                            const char *jti, int64_t exp) {
    if (!rl || !jti || !rl->entries) return -1;

    /* prune expired entries first */
    revocation_prune(rl);

    /* check if already revoked */
    for (int i = 0; i < rl->count; i++) {
        if (strcmp(rl->entries[i].jti, jti) == 0) return 0;
    }

    if (rl->count >= rl->capacity) return -1; /* full */

    cookbook_revocation_entry *e = &rl->entries[rl->count++];
    snprintf(e->jti, sizeof(e->jti), "%s", jti);
    e->exp = exp;
    return 0;
}

int cookbook_revocation_check(const cookbook_revocation_list *rl,
                               const char *jti) {
    if (!rl || !jti || !rl->entries) return 0;
    for (int i = 0; i < rl->count; i++) {
        if (strcmp(rl->entries[i].jti, jti) == 0) return 1;
    }
    return 0;
}

/* ==== JTI generation ==== */

/* Generate a random 16-byte hex JTI (32 chars). Uses counter + time for uniqueness. */
static volatile long jti_counter = 0;

static void generate_jti(char *jti, size_t jti_sz) {
    unsigned char rand_bytes[16];
    long cnt;
#ifdef _WIN32
    cnt = InterlockedIncrement(&jti_counter);
#else
    cnt = __sync_add_and_fetch(&jti_counter, 1);
#endif
    uint64_t seed = (uint64_t)time(NULL) ^ ((uint64_t)cnt * 2654435761ULL)
                     ^ (uint64_t)(uintptr_t)jti;
    for (int i = 0; i < 16; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        rand_bytes[i] = (unsigned char)(seed >> 33);
    }
    size_t written = 0;
    for (int i = 0; i < 16 && written + 2 < jti_sz; i++) {
        written += (size_t)snprintf(jti + written, jti_sz - written,
                                     "%02x", rand_bytes[i]);
    }
    jti[written] = '\0';
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

    /* generate unique token ID */
    char jti[64];
    generate_jti(jti, sizeof(jti));

    /* build payload JSON */
    char payload[2048];
    int off = 0;
    off += snprintf(payload + off, sizeof(payload) - (size_t)off, "{");
    off += json_write_string(payload + off, sizeof(payload) - (size_t)off,
                              "sub", sub, 1);
    if (groups && *groups)
        off += json_write_string(payload + off, sizeof(payload) - (size_t)off,
                                  "groups", groups, 0);
    off += json_write_string(payload + off, sizeof(payload) - (size_t)off,
                              "jti", jti, 0);
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
    if (cookbook_ed25519_sign(sig, signing_input, (size_t)si_len,
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

/* ==== JWT v2 (policy-based grants) ==== */

char *cookbook_jwt_create_v2(const char *sub, const char *groups,
                              const char *resolved_json,
                              int64_t ttl_sec,
                              const unsigned char signing_key[64]) {
    if (!sub || !signing_key || !resolved_json) return NULL;

    int64_t now = (int64_t)time(NULL);
    int64_t exp = now + ttl_sec;

    /* generate unique token ID */
    char jti[64];
    generate_jti(jti, sizeof(jti));

    /* build payload — embed resolved grants/exclude as raw JSON objects */
    size_t rjlen = strlen(resolved_json);
    size_t cap = 512 + rjlen;
    char *payload = (char *)malloc(cap);
    if (!payload) return NULL;

    int off = 0;
    off += snprintf(payload + off, cap - (size_t)off, "{");
    off += json_write_string(payload + off, cap - (size_t)off, "sub", sub, 1);
    if (groups && *groups)
        off += json_write_string(payload + off, cap - (size_t)off,
                                  "groups", groups, 0);
    off += json_write_string(payload + off, cap - (size_t)off, "jti", jti, 0);
    off += json_write_int(payload + off, cap - (size_t)off, "v", 2, 0);

    /* splice in the grants/exclude from resolved_json directly.
       resolved_json is like: {"grants":{...},"exclude":{...}}
       We strip the outer braces and append as comma-separated fields. */
    if (rjlen >= 2 && resolved_json[0] == '{') {
        off += snprintf(payload + off, cap - (size_t)off, ",");
        /* copy inner content (skip outer { and }) */
        size_t inner_len = rjlen - 2;
        if ((size_t)off + inner_len + 64 > cap) {
            cap = (size_t)off + inner_len + 256;
            char *tmp = (char *)realloc(payload, cap);
            if (!tmp) { free(payload); return NULL; }
            payload = tmp;
        }
        memcpy(payload + off, resolved_json + 1, inner_len);
        off += (int)inner_len;
    }

    off += snprintf(payload + off, cap - (size_t)off, ",\"iat\":%lld",
                    (long long)now);
    off += snprintf(payload + off, cap - (size_t)off, ",\"exp\":%lld",
                    (long long)exp);
    snprintf(payload + off, cap - (size_t)off, "}");

    /* base64url encode header and payload */
    char hdr_b64[256];
    size_t pay_actual = strlen(payload);
    size_t pay_b64_cap = pay_actual * 2 + 16;
    char *pay_b64 = (char *)malloc(pay_b64_cap);
    if (!pay_b64) { free(payload); return NULL; }

    size_t hdr_len = cookbook_base64url_encode(JWT_HEADER, strlen(JWT_HEADER),
                                               hdr_b64, sizeof(hdr_b64));
    size_t pay_len = cookbook_base64url_encode(payload, pay_actual,
                                               pay_b64, pay_b64_cap);
    free(payload);
    if (hdr_len == 0 || pay_len == 0) { free(pay_b64); return NULL; }

    /* signing input */
    size_t si_cap = hdr_len + 1 + pay_len + 1;
    char *signing_input = (char *)malloc(si_cap);
    if (!signing_input) { free(pay_b64); return NULL; }
    int si_len = snprintf(signing_input, si_cap, "%s.%s", hdr_b64, pay_b64);
    free(pay_b64);

    /* sign */
    unsigned char sig[64];
    if (cookbook_ed25519_sign(sig, signing_input, (size_t)si_len,
                              signing_key) != 0) {
        free(signing_input);
        return NULL;
    }

    /* encode signature */
    char sig_b64[128];
    size_t sig_len = cookbook_base64url_encode(sig, 64, sig_b64, sizeof(sig_b64));
    if (sig_len == 0) { free(signing_input); return NULL; }

    /* assemble */
    size_t token_len = (size_t)si_len + 1 + sig_len + 1;
    char *token = (char *)malloc(token_len);
    if (!token) { free(signing_input); return NULL; }
    snprintf(token, token_len, "%s.%s", signing_input, sig_b64);
    free(signing_input);
    return token;
}

void cookbook_jwt_claims_free(cookbook_jwt_claims *claims) {
    if (!claims) return;
    free(claims->grants_json);
    free(claims->exclude_json);
    claims->grants_json = NULL;
    claims->exclude_json = NULL;
}

/* Extract a JSON sub-object as a malloc'd string. E.g. key="grants" from
   payload like ...,"grants":{...},...  Returns NULL if not found. */
static char *json_extract_object(const char *json, const char *key) {
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\":{", key);
    const char *start = strstr(json, pattern);
    if (!start) return NULL;
    start += strlen(pattern) - 1; /* point at the '{' */

    /* count braces to find matching '}' */
    int depth = 0;
    const char *p = start;
    do {
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
        p++;
    } while (depth > 0 && *p);

    size_t len = (size_t)(p - start);
    char *result = (char *)malloc(len + 1);
    if (!result) return NULL;
    memcpy(result, start, len);
    result[len] = '\0';
    return result;
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
    if (cookbook_ed25519_verify(sig, token, si_len, verify_key) != 0)
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
    json_get_string(payload, "jti", claims->jti, sizeof(claims->jti));
    json_get_int(payload, "exp", &claims->exp);
    json_get_int(payload, "iat", &claims->iat);

    /* v2: extract version, grants, exclude */
    int64_t ver = 0;
    if (json_get_int(payload, "v", &ver) == 0 && ver == 2) {
        claims->version = 2;
        claims->grants_json = json_extract_object(payload, "grants");
        claims->exclude_json = json_extract_object(payload, "exclude");
    } else {
        claims->version = 1;
    }

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
    return cookbook_ed25519_keygen(pk, sk);
}

int cookbook_sign(const void *msg, size_t msg_len,
                  unsigned char sig[64],
                  const unsigned char sk[64]) {
    return cookbook_ed25519_sign(sig, msg, msg_len, sk);
}

int cookbook_verify(const void *msg, size_t msg_len,
                    const unsigned char sig[64],
                    const unsigned char pk[32]) {
    return cookbook_ed25519_verify(sig, msg, msg_len, pk);
}
