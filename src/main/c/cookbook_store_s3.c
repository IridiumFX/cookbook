/* S3-compatible object store backend for cookbook.
   Implements the cookbook_store vtable using AWS Signature V4 over raw sockets.
   No libcurl dependency — uses HMAC-SHA256 from libsodium and our SHA-256. */

#include "cookbook_store.h"
#include "cookbook_sha256.h"
#include <sodium.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef SOCKET sock_t;
  #define SOCK_INVALID INVALID_SOCKET
  #define sock_close closesocket
#else
  #include <sys/socket.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int sock_t;
  #define SOCK_INVALID (-1)
  #define sock_close close
#endif

typedef struct {
    cookbook_store  base;
    char          *bucket;
    char          *region;
    char          *access_key;
    char          *secret_key;
    char          *endpoint_host;
    char          *endpoint_port;
    int            path_style;  /* 1 = path-style (MinIO), 0 = virtual-hosted */
} cookbook_store_s3;

/* ---- AWS Signature V4 helpers ---- */

static void hmac_sha256(const void *key, size_t key_len,
                         const void *data, size_t data_len,
                         unsigned char out[32]) {
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, key, key_len);
    crypto_auth_hmacsha256_update(&st, data, data_len);
    crypto_auth_hmacsha256_final(&st, out);
}

static void hex_encode(const unsigned char *data, size_t len,
                        char *out) {
    for (size_t i = 0; i < len; i++)
        sprintf(out + i * 2, "%02x", data[i]);
    out[len * 2] = '\0';
}

/* URL-encode a path component (encode everything except / and unreserved chars) */
static void uri_encode(const char *input, char *output, size_t out_sz) {
    size_t j = 0;
    for (size_t i = 0; input[i] && j + 3 < out_sz; i++) {
        char c = input[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~' || c == '/') {
            output[j++] = c;
        } else {
            j += (size_t)snprintf(output + j, out_sz - j, "%%%02X",
                                  (unsigned char)c);
        }
    }
    output[j] = '\0';
}

/* Get current UTC time in required formats. */
static void aws_timestamp(char date8[9], char datetime[17]) {
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    strftime(date8, 9, "%Y%m%d", &tm);
    strftime(datetime, 17, "%Y%m%dT%H%M%SZ", &tm);
}

/* Derive the signing key: HMAC chain per AWS Sig V4. */
static void derive_signing_key(const char *secret_key, const char *date8,
                                 const char *region, unsigned char out[32]) {
    char key_buf[256];
    snprintf(key_buf, sizeof(key_buf), "AWS4%s", secret_key);

    unsigned char date_key[32], region_key[32], service_key[32];
    hmac_sha256(key_buf, strlen(key_buf), date8, strlen(date8), date_key);
    hmac_sha256(date_key, 32, region, strlen(region), region_key);
    hmac_sha256(region_key, 32, "s3", 2, service_key);
    hmac_sha256(service_key, 32, "aws4_request", 12, out);
}

/* Build and sign an S3 request. Returns the complete HTTP request as a
   malloc'd string. content_sha256 should be the hex SHA-256 of the body
   (or "UNSIGNED-PAYLOAD" for unsigned). */
static char *s3_sign_request(cookbook_store_s3 *self,
                              const char *method, const char *path,
                              const char *content_sha256,
                              size_t content_length,
                              char *extra_headers,
                              size_t extra_headers_len) {
    char date8[9], datetime[17];
    aws_timestamp(date8, datetime);

    /* build canonical URI */
    char canonical_uri[2048];
    if (self->path_style) {
        snprintf(canonical_uri, sizeof(canonical_uri), "/%s/%s",
                 self->bucket, path);
    } else {
        snprintf(canonical_uri, sizeof(canonical_uri), "/%s", path);
    }

    /* URL-encode the path */
    char encoded_uri[4096];
    uri_encode(canonical_uri, encoded_uri, sizeof(encoded_uri));

    /* build host header */
    char host[512];
    if (self->path_style) {
        snprintf(host, sizeof(host), "%s:%s",
                 self->endpoint_host, self->endpoint_port);
    } else {
        snprintf(host, sizeof(host), "%s.%s:%s",
                 self->bucket, self->endpoint_host, self->endpoint_port);
    }
    /* strip :80 or :443 for standard ports */
    if (strcmp(self->endpoint_port, "80") == 0 ||
        strcmp(self->endpoint_port, "443") == 0) {
        if (self->path_style)
            snprintf(host, sizeof(host), "%s", self->endpoint_host);
        else
            snprintf(host, sizeof(host), "%s.%s",
                     self->bucket, self->endpoint_host);
    }

    /* canonical headers (must be sorted by lowercase name) */
    char canonical_headers[2048];
    snprintf(canonical_headers, sizeof(canonical_headers),
        "host:%s\n"
        "x-amz-content-sha256:%s\n"
        "x-amz-date:%s\n",
        host, content_sha256, datetime);

    const char *signed_headers = "host;x-amz-content-sha256;x-amz-date";

    /* canonical request */
    char canonical_request[8192];
    snprintf(canonical_request, sizeof(canonical_request),
        "%s\n%s\n\n%s\n%s\n%s",
        method, encoded_uri, canonical_headers,
        signed_headers, content_sha256);

    /* hash the canonical request */
    char cr_hash[65];
    cookbook_sha256_hex(canonical_request, strlen(canonical_request), cr_hash);

    /* string to sign */
    char scope[128];
    snprintf(scope, sizeof(scope), "%s/%s/s3/aws4_request",
             date8, self->region);

    char string_to_sign[512];
    snprintf(string_to_sign, sizeof(string_to_sign),
        "AWS4-HMAC-SHA256\n%s\n%s\n%s",
        datetime, scope, cr_hash);

    /* signing key */
    unsigned char signing_key[32];
    derive_signing_key(self->secret_key, date8, self->region, signing_key);

    /* signature */
    unsigned char sig_raw[32];
    hmac_sha256(signing_key, 32,
                string_to_sign, strlen(string_to_sign), sig_raw);
    char signature[65];
    hex_encode(sig_raw, 32, signature);

    /* authorization header */
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header),
        "AWS4-HMAC-SHA256 Credential=%s/%s,SignedHeaders=%s,Signature=%s",
        self->access_key, scope, signed_headers, signature);

    /* build full HTTP request */
    size_t req_sz = 4096 + extra_headers_len + content_length;
    char *request = malloc(req_sz);
    if (!request) return NULL;

    int hdr_len = snprintf(request, req_sz,
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "x-amz-date: %s\r\n"
        "x-amz-content-sha256: %s\r\n"
        "Authorization: %s\r\n"
        "Content-Length: %zu\r\n"
        "\r\n",
        method, canonical_uri, host,
        datetime, content_sha256, auth_header,
        content_length);

    (void)extra_headers;
    (void)extra_headers_len;

    /* caller will append body after headers */
    /* store header length for the caller */
    /* we'll use a convention: first 4 bytes store the header length as int */
    /* Actually, just return the headers. Caller manages body separately. */

    request[hdr_len] = '\0';
    return request;
}

/* ---- socket helpers ---- */

static sock_t s3_connect(cookbook_store_s3 *self) {
    const char *host = self->endpoint_host;
    const char *port = self->endpoint_port;

    /* for virtual-hosted style, connect to the same endpoint */
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res)
        return SOCK_INVALID;

    sock_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == SOCK_INVALID) {
        freeaddrinfo(res);
        return SOCK_INVALID;
    }

    if (connect(fd, res->ai_addr, (int)res->ai_addrlen) != 0) {
        sock_close(fd);
        freeaddrinfo(res);
        return SOCK_INVALID;
    }
    freeaddrinfo(res);
    return fd;
}

/* Send data over socket, handling partial sends. */
static int sock_send_all(sock_t fd, const void *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int chunk = (len - sent > 65536) ? 65536 : (int)(len - sent);
        int n = send(fd, (const char *)data + sent, chunk, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Receive full HTTP response. Returns malloc'd buffer, sets *out_len.
   Parses status code into *status. */
static char *sock_recv_response(sock_t fd, size_t *out_len, int *status) {
    size_t cap = 65536, total = 0;
    char *buf = malloc(cap);
    if (!buf) { *out_len = 0; *status = -1; return NULL; }

    for (;;) {
        if (total >= cap - 1) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); *out_len = 0; *status = -1; return NULL; }
            buf = tmp;
        }
        int n = recv(fd, buf + total, (int)(cap - 1 - total), 0);
        if (n <= 0) break;
        total += (size_t)n;

        /* check if we've received the full response by looking for
           Content-Length or end of chunked transfer.
           Simple approach: check if headers are complete and body matches
           Content-Length. */
        buf[total] = '\0';
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            size_t hdr_len = (size_t)(hdr_end - buf) + 4;
            char *cl_str = strstr(buf, "Content-Length: ");
            if (!cl_str) cl_str = strstr(buf, "content-length: ");
            if (cl_str) {
                size_t content_len = (size_t)atol(cl_str + 16);
                if (total >= hdr_len + content_len)
                    break;
            } else {
                /* no content-length; check for "0\r\n\r\n" chunked end
                   or just read until connection closes */
                if (strstr(buf, "Transfer-Encoding: chunked") ||
                    strstr(buf, "transfer-encoding: chunked")) {
                    if (strstr(hdr_end + 4, "\r\n0\r\n"))
                        break;
                }
                /* for HEAD requests or empty bodies, headers alone are enough */
                if (total == hdr_len) break;
            }
        }
    }

    buf[total] = '\0';
    *out_len = total;

    /* parse status code */
    *status = 0;
    if (total > 12 && strncmp(buf, "HTTP/1.", 7) == 0)
        *status = atoi(buf + 9);

    return buf;
}

/* Extract response body (past \r\n\r\n) from full HTTP response. */
static const char *response_body(const char *resp, size_t resp_len,
                                   size_t *body_len) {
    const char *hdr_end = strstr(resp, "\r\n\r\n");
    if (!hdr_end) {
        *body_len = 0;
        return NULL;
    }
    const char *body = hdr_end + 4;
    *body_len = resp_len - (size_t)(body - resp);
    return body;
}

/* ---- vtable implementation ---- */

static cookbook_store_status s3_put(cookbook_store *store, const char *key,
                                    const void *data, size_t len) {
    cookbook_store_s3 *self = (cookbook_store_s3 *)store;

    /* compute SHA-256 of body */
    char content_sha256[65];
    cookbook_sha256_hex(data, len, content_sha256);

    char *headers = s3_sign_request(self, "PUT", key, content_sha256,
                                     len, NULL, 0);
    if (!headers) return COOKBOOK_STORE_ERROR;

    sock_t fd = s3_connect(self);
    if (fd == SOCK_INVALID) {
        free(headers);
        return COOKBOOK_STORE_ERROR;
    }

    /* send headers then body */
    int rc = sock_send_all(fd, headers, strlen(headers));
    free(headers);
    if (rc != 0) { sock_close(fd); return COOKBOOK_STORE_ERROR; }

    if (len > 0) {
        rc = sock_send_all(fd, data, len);
        if (rc != 0) { sock_close(fd); return COOKBOOK_STORE_ERROR; }
    }

    /* read response */
    size_t resp_len;
    int status;
    char *resp = sock_recv_response(fd, &resp_len, &status);
    sock_close(fd);
    free(resp);

    return (status == 200) ? COOKBOOK_STORE_OK : COOKBOOK_STORE_ERROR;
}

static cookbook_store_status s3_get(cookbook_store *store, const char *key,
                                    void **data, size_t *len) {
    cookbook_store_s3 *self = (cookbook_store_s3 *)store;

    /* empty body SHA-256 */
    char empty_sha[65];
    cookbook_sha256_hex("", 0, empty_sha);

    char *headers = s3_sign_request(self, "GET", key, empty_sha,
                                     0, NULL, 0);
    if (!headers) return COOKBOOK_STORE_ERROR;

    sock_t fd = s3_connect(self);
    if (fd == SOCK_INVALID) {
        free(headers);
        return COOKBOOK_STORE_ERROR;
    }

    int rc = sock_send_all(fd, headers, strlen(headers));
    free(headers);
    if (rc != 0) { sock_close(fd); return COOKBOOK_STORE_ERROR; }

    size_t resp_len;
    int status;
    char *resp = sock_recv_response(fd, &resp_len, &status);
    sock_close(fd);

    if (!resp) return COOKBOOK_STORE_ERROR;
    if (status == 404) { free(resp); return COOKBOOK_STORE_NOT_FOUND; }
    if (status != 200) { free(resp); return COOKBOOK_STORE_ERROR; }

    size_t body_len;
    const char *body = response_body(resp, resp_len, &body_len);
    if (!body || body_len == 0) {
        free(resp);
        *data = NULL;
        *len = 0;
        return COOKBOOK_STORE_OK;
    }

    /* copy body into caller-owned buffer */
    void *buf = malloc(body_len);
    if (!buf) { free(resp); return COOKBOOK_STORE_ERROR; }
    memcpy(buf, body, body_len);
    *data = buf;
    *len = body_len;
    free(resp);
    return COOKBOOK_STORE_OK;
}

static cookbook_store_status s3_exists(cookbook_store *store, const char *key) {
    cookbook_store_s3 *self = (cookbook_store_s3 *)store;

    char empty_sha[65];
    cookbook_sha256_hex("", 0, empty_sha);

    char *headers = s3_sign_request(self, "HEAD", key, empty_sha,
                                     0, NULL, 0);
    if (!headers) return COOKBOOK_STORE_ERROR;

    sock_t fd = s3_connect(self);
    if (fd == SOCK_INVALID) {
        free(headers);
        return COOKBOOK_STORE_ERROR;
    }

    int rc = sock_send_all(fd, headers, strlen(headers));
    free(headers);
    if (rc != 0) { sock_close(fd); return COOKBOOK_STORE_ERROR; }

    size_t resp_len;
    int status;
    char *resp = sock_recv_response(fd, &resp_len, &status);
    sock_close(fd);
    free(resp);

    if (status == 200) return COOKBOOK_STORE_OK;
    if (status == 404) return COOKBOOK_STORE_NOT_FOUND;
    return COOKBOOK_STORE_ERROR;
}

static cookbook_store_status s3_del(cookbook_store *store, const char *key) {
    cookbook_store_s3 *self = (cookbook_store_s3 *)store;

    char empty_sha[65];
    cookbook_sha256_hex("", 0, empty_sha);

    char *headers = s3_sign_request(self, "DELETE", key, empty_sha,
                                     0, NULL, 0);
    if (!headers) return COOKBOOK_STORE_ERROR;

    sock_t fd = s3_connect(self);
    if (fd == SOCK_INVALID) {
        free(headers);
        return COOKBOOK_STORE_ERROR;
    }

    int rc = sock_send_all(fd, headers, strlen(headers));
    free(headers);
    if (rc != 0) { sock_close(fd); return COOKBOOK_STORE_ERROR; }

    size_t resp_len;
    int status;
    char *resp = sock_recv_response(fd, &resp_len, &status);
    sock_close(fd);
    free(resp);

    /* S3 returns 204 on successful delete */
    if (status == 204 || status == 200) return COOKBOOK_STORE_OK;
    if (status == 404) return COOKBOOK_STORE_NOT_FOUND;
    return COOKBOOK_STORE_ERROR;
}

static void s3_free_buf(void *data) {
    free(data);
}

static void s3_close(cookbook_store *store) {
    cookbook_store_s3 *self = (cookbook_store_s3 *)store;
    free(self->bucket);
    free(self->region);
    /* zero secret key before freeing */
    if (self->secret_key) {
        sodium_memzero(self->secret_key, strlen(self->secret_key));
        free(self->secret_key);
    }
    free(self->access_key);
    free(self->endpoint_host);
    free(self->endpoint_port);
    free(self);
}

cookbook_store *cookbook_store_open_s3(const char *bucket,
                                      const char *region,
                                      const char *access_key,
                                      const char *secret_key,
                                      const char *endpoint) {
    if (!bucket || !region || !access_key || !secret_key) return NULL;

    cookbook_store_s3 *self = calloc(1, sizeof(*self));
    if (!self) return NULL;

    self->bucket     = strdup(bucket);
    self->region     = strdup(region);
    self->access_key = strdup(access_key);
    self->secret_key = strdup(secret_key);

    /* parse endpoint: "host:port" or just "host" */
    if (endpoint && *endpoint) {
        const char *colon = strrchr(endpoint, ':');
        if (colon) {
            size_t hl = (size_t)(colon - endpoint);
            self->endpoint_host = malloc(hl + 1);
            if (self->endpoint_host) {
                memcpy(self->endpoint_host, endpoint, hl);
                self->endpoint_host[hl] = '\0';
            }
            self->endpoint_port = strdup(colon + 1);
        } else {
            self->endpoint_host = strdup(endpoint);
            self->endpoint_port = strdup("443");
        }
        /* path-style for custom endpoints (MinIO, etc.) */
        self->path_style = 1;
    } else {
        /* default: AWS S3 virtual-hosted style */
        self->endpoint_host = strdup("s3.amazonaws.com");
        self->endpoint_port = strdup("443");
        self->path_style = 0;
    }

    self->base.put      = s3_put;
    self->base.get      = s3_get;
    self->base.exists   = s3_exists;
    self->base.del      = s3_del;
    self->base.free_buf = s3_free_buf;
    self->base.close    = s3_close;
    return &self->base;
}
