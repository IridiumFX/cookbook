/*
 * cookbook_oidc.c — OIDC client credentials flow (RFC 6749 §4.4)
 *
 * Validates client_id/client_secret against an OIDC issuer's token endpoint
 * via HTTPS POST. Pure C11 over cookbook_socket + cookbook_tls — no libcurl.
 *
 * Flow:
 * 1. POST to {issuer}/oauth/token with grant_type=client_credentials
 * 2. Parse JSON response for access_token
 * 3. Decode the JWT access_token to extract "sub" claim
 * 4. Return the subject — caller issues a cookbook JWT
 */

#include "cookbook_oidc.h"
#include "cookbook_auth.h"  /* for cookbook_base64_decode */
#include <apennines/t4/net/https_client.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Simple JSON string extraction: find "key":"value" and copy value */
static int json_get(const char *json, const char *key,
                     char *out, size_t out_sz) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return -1;
    size_t len = (size_t)(end - p);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

/* Decode a JWT payload (second dot-separated segment, base64url) */
static int decode_jwt_sub(const char *jwt, char *sub, size_t sub_sz) {
    /* find the payload between first and second dot */
    const char *dot1 = strchr(jwt, '.');
    if (!dot1) return -1;
    const char *payload = dot1 + 1;
    const char *dot2 = strchr(payload, '.');
    if (!dot2) return -1;

    size_t b64_len = (size_t)(dot2 - payload);
    char decoded[4096];
    /* base64url decode: replace - with +, _ with / */
    char *b64 = malloc(b64_len + 4);
    if (!b64) return -1;
    memcpy(b64, payload, b64_len);
    b64[b64_len] = '\0';
    for (size_t i = 0; i < b64_len; i++) {
        if (b64[i] == '-') b64[i] = '+';
        else if (b64[i] == '_') b64[i] = '/';
    }
    /* add padding */
    while (b64_len % 4 != 0) b64[b64_len++] = '=';
    b64[b64_len] = '\0';

    size_t dec_len = cookbook_base64_decode(b64, b64_len,
                                            decoded, sizeof(decoded) - 1);
    free(b64);
    if (dec_len == 0) return -1;
    decoded[dec_len] = '\0';

    /* extract "sub" from the JSON payload */
    return json_get(decoded, "sub", sub, sub_sz);
}

int cookbook_oidc_client_credentials(const cookbook_oidc_config *cfg,
                                      const char *client_id,
                                      const char *client_secret,
                                      char *sub_out, size_t sub_sz) {
    if (!cfg || !cfg->issuer || !client_id || !client_secret)
        return -1;

    /* build token endpoint URL */
    char token_url[512];
    snprintf(token_url, sizeof(token_url), "%s/oauth/token", cfg->issuer);

    /* build POST body */
    char body[1024];
    int body_len = snprintf(body, sizeof(body),
        "grant_type=client_credentials&client_id=%s&client_secret=%s",
        client_id, client_secret);

    /* POST via apennines HTTPS client */
    https_client *hc = NULL;
    if (https_client_create(&hc) != 0) return -1;
    https_client_set_timeout(hc, 10000);

    https_response hr;
    memset(&hr, 0, sizeof(hr));
    unsigned long hrc = https_client_post(&hr, hc, token_url,
                                            (const u8 *)body, (u64)body_len,
                                            "application/x-www-form-urlencoded");
    if (hrc != 0 || hr.status != 200) {
        https_response_free(&hr);
        https_client_destroy(hc);
        return -1;
    }

    /* null-terminate body for string parsing */
    char *resp_body = malloc(hr.body_len + 1);
    if (!resp_body) {
        https_response_free(&hr);
        https_client_destroy(hc);
        return -1;
    }
    memcpy(resp_body, hr.body, hr.body_len);
    resp_body[hr.body_len] = '\0';
    https_response_free(&hr);
    https_client_destroy(hc);

    /* extract access_token from JSON response */
    char access_token[4096] = {0};
    if (json_get(resp_body, "access_token", access_token,
                  sizeof(access_token)) != 0) {
        free(resp_body);
        return -1;
    }

    /* decode JWT to extract sub */
    if (decode_jwt_sub(access_token, sub_out, sub_sz) != 0) {
        /* if the access_token isn't a JWT (opaque token),
           try the "sub" field directly from the token response */
        if (json_get(resp_body, "sub", sub_out, sub_sz) != 0) {
            /* last resort: use client_id as subject */
            snprintf(sub_out, sub_sz, "%s", client_id);
        }
    }

    free(resp_body);
    return 0;
}
