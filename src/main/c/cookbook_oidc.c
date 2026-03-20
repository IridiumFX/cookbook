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
#include "cookbook_socket.h"
#include "cookbook_auth.h"  /* for cookbook_base64_decode */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parse host, port, path from an HTTPS URL */
static int parse_url(const char *url, char *host, size_t host_sz,
                      int *port, char *path, size_t path_sz) {
    const char *p = url;
    *port = 443;

    if (strncmp(p, "https://", 8) == 0)
        p += 8;
    else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        *port = 80;
    } else {
        return -1;
    }

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    if (colon && (!slash || colon < slash)) {
        size_t hl = (size_t)(colon - p);
        if (hl >= host_sz) hl = host_sz - 1;
        memcpy(host, p, hl);
        host[hl] = '\0';
        *port = atoi(colon + 1);
    } else if (slash) {
        size_t hl = (size_t)(slash - p);
        if (hl >= host_sz) hl = host_sz - 1;
        memcpy(host, p, hl);
        host[hl] = '\0';
    } else {
        snprintf(host, host_sz, "%s", p);
        slash = NULL;
    }

    /* strip trailing slash from host */
    size_t hlen = strlen(host);
    while (hlen > 0 && host[hlen - 1] == '/') host[--hlen] = '\0';

    if (slash)
        snprintf(path, path_sz, "%s", slash);
    else
        snprintf(path, path_sz, "/");

    return 0;
}

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

    /* build token endpoint URL: {issuer}/oauth/token
       (standard OIDC: {issuer}/token, but /oauth/token is also common) */
    char token_url[512];
    snprintf(token_url, sizeof(token_url), "%s/oauth/token", cfg->issuer);

    char host[256], path[256];
    int port;
    if (parse_url(token_url, host, sizeof(host), &port,
                   path, sizeof(path)) != 0)
        return -1;

    /* build POST body: grant_type=client_credentials&client_id=...&client_secret=... */
    char body[1024];
    int body_len = snprintf(body, sizeof(body),
        "grant_type=client_credentials&client_id=%s&client_secret=%s",
        client_id, client_secret);

    /* build HTTP request */
    char request[2048];
    int req_len = snprintf(request, sizeof(request),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, body_len, body);

    /* connect via TLS (OIDC issuers are always HTTPS) */
    cookbook_tls_sock *ts = cookbook_sock_connect_tls(host, port, 10);
    if (!ts) return -1;

    /* send request */
    if (cookbook_tls_sock_send(ts, request, (size_t)req_len) != 0) {
        cookbook_tls_sock_close(ts);
        return -1;
    }

    /* receive response */
    char resp[8192];
    size_t total = 0;
    for (;;) {
        int n = cookbook_tls_sock_recv(ts, resp + total,
                                       sizeof(resp) - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    resp[total] = '\0';
    cookbook_tls_sock_close(ts);

    /* check HTTP status */
    const char *sp = strchr(resp, ' ');
    int status = sp ? atoi(sp + 1) : 0;
    if (status != 200) return -1;

    /* find response body (after \r\n\r\n) */
    const char *resp_body = strstr(resp, "\r\n\r\n");
    if (!resp_body) return -1;
    resp_body += 4;

    /* extract access_token from JSON response */
    char access_token[4096] = {0};
    if (json_get(resp_body, "access_token", access_token,
                  sizeof(access_token)) != 0)
        return -1;

    /* decode JWT to extract sub */
    if (decode_jwt_sub(access_token, sub_out, sub_sz) != 0) {
        /* if the access_token isn't a JWT (opaque token),
           try the "sub" field directly from the token response */
        if (json_get(resp_body, "sub", sub_out, sub_sz) != 0) {
            /* last resort: use client_id as subject */
            snprintf(sub_out, sub_sz, "%s", client_id);
        }
    }

    return 0;
}
