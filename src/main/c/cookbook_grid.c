/* cookbook_grid.c — Grid federation: peer management and inter-node HTTP.
   Enables cookbook instances to form a mesh where any node can serve
   artifacts from any peer via redirect (307) or proxy. */

#include "cookbook_grid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  #include <unistd.h>
  typedef int sock_t;
  #define SOCK_INVALID (-1)
  #define sock_close close
#endif

/* ---- Peer loading ---- */

typedef struct {
    cookbook_peer *peers;
    int count;
    int cap;
} peer_load_ctx;

static int peer_load_cb(const cookbook_db_row *row, void *user) {
    peer_load_ctx *ctx = (peer_load_ctx *)user;
    if (!row->values[0] || !row->values[1] || !row->values[2])
        return 0;

    if (ctx->count >= ctx->cap) {
        ctx->cap = ctx->cap ? ctx->cap * 2 : 8;
        cookbook_peer *tmp = realloc(ctx->peers,
            (size_t)ctx->cap * sizeof(cookbook_peer));
        if (!tmp) return 0;
        ctx->peers = tmp;
    }

    cookbook_peer *p = &ctx->peers[ctx->count];
    p->peer_id = strdup(row->values[0]);
    p->name    = strdup(row->values[1]);
    p->url     = strdup(row->values[2]);
    p->mode    = (row->values[3] && row->values[3][0] == 'p') ? 'p' : 'r';
    p->priority = row->values[4] ? atoi(row->values[4]) : 100;
    p->enabled  = 1;
    ctx->count++;
    return 0;
}

int cookbook_grid_load_peers(cookbook_db *db, cookbook_peer **out) {
    peer_load_ctx ctx = { NULL, 0, 0 };
    db->query(db,
        "SELECT peer_id, name, url, mode, priority "
        "FROM peers WHERE enabled = 1 ORDER BY priority ASC",
        peer_load_cb, &ctx);
    *out = ctx.peers;
    return ctx.count;
}

void cookbook_grid_free_peers(cookbook_peer *peers, int count) {
    if (!peers) return;
    for (int i = 0; i < count; i++) {
        free(peers[i].peer_id);
        free(peers[i].name);
        free(peers[i].url);
    }
    free(peers);
}

/* ---- String helpers ---- */

/* strndup not available on all platforms (e.g., MinGW C11 strict) */
static char *grid_strndup(const char *s, size_t n) {
    size_t len = strlen(s);
    if (len > n) len = n;
    char *d = malloc(len + 1);
    if (!d) return NULL;
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

/* ---- URL parsing ---- */

/* Parse "http://host:port" from a base URL.
   Returns 0 on success. host_out and port_out must be freed by caller. */
static int parse_peer_url(const char *url,
                           char **host_out, char **port_out,
                           const char **path_prefix) {
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0)
        p += 7;
    else if (strncmp(p, "https://", 8) == 0)
        p += 8;
    else
        return -1;

    /* find host:port boundary */
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        *host_out = grid_strndup(p, (size_t)(colon - p));
        const char *port_start = colon + 1;
        if (slash)
            *port_out = grid_strndup(port_start, (size_t)(slash - port_start));
        else
            *port_out = strdup(port_start);
    } else {
        if (slash)
            *host_out = grid_strndup(p, (size_t)(slash - p));
        else
            *host_out = strdup(p);
        *port_out = strdup("80");
    }

    *path_prefix = slash ? slash : "";
    return 0;
}

/* ---- Socket helpers ---- */

static sock_t grid_connect(const char *host, const char *port) {
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

    /* set 5 second timeout */
#ifdef _WIN32
    DWORD timeout = 5000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
               sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout,
               sizeof(timeout));
#else
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    return fd;
}

static int grid_send_all(sock_t fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int chunk = (len - sent > 65536) ? 65536 : (int)(len - sent);
        int n = send(fd, data + sent, chunk, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static char *grid_recv_response(sock_t fd, size_t *out_len, int *status) {
    size_t cap = 8192, total = 0;
    char *buf = malloc(cap);
    if (!buf) { *out_len = 0; *status = -1; return NULL; }

    for (;;) {
        if (total >= cap - 1) {
            cap *= 2;
            if (cap > 4 * 1024 * 1024) break; /* 4MB safety limit */
            char *tmp = realloc(buf, cap);
            if (!tmp) break;
            buf = tmp;
        }
        int n = recv(fd, buf + total, (int)(cap - 1 - total), 0);
        if (n <= 0) break;
        total += (size_t)n;

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
            }
        }
    }

    buf[total] = '\0';

    /* parse status code */
    *status = 0;
    if (total > 12 && strncmp(buf, "HTTP/1.", 7) == 0) {
        *status = atoi(buf + 9);
    }

    /* extract body */
    char *hdr_end = strstr(buf, "\r\n\r\n");
    if (hdr_end) {
        size_t hdr_len = (size_t)(hdr_end - buf) + 4;
        size_t body_len = total - hdr_len;
        char *body = malloc(body_len + 1);
        if (body) {
            memcpy(body, buf + hdr_len, body_len);
            body[body_len] = '\0';
            *out_len = body_len;
            free(buf);
            return body;
        }
    }

    *out_len = 0;
    free(buf);
    return NULL;
}

/* ---- Grid HTTP client ---- */

static int grid_request(const cookbook_peer *peer,
                         const char *method,
                         const char *path,
                         const char *origin_id,
                         const char *via_chain,
                         int hop_count,
                         cookbook_grid_response *response) {
    char *host = NULL, *port = NULL;
    const char *prefix = NULL;

    memset(response, 0, sizeof(*response));

    if (parse_peer_url(peer->url, &host, &port, &prefix) != 0)
        return -1;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    sock_t fd = grid_connect(host, port);
    if (fd == SOCK_INVALID) {
        free(host); free(port);
        return -1;
    }

    /* build new via chain */
    char via[1024] = {0};
    if (via_chain && via_chain[0])
        snprintf(via, sizeof(via), "%s,%s", via_chain, origin_id);
    else
        snprintf(via, sizeof(via), "%s", origin_id);

    /* build request */
    char request[4096];
    int rlen = snprintf(request, sizeof(request),
        "%s %s%s HTTP/1.1\r\n"
        "Host: %s:%s\r\n"
        "X-Cookbook-Via: %s\r\n"
        "X-Cookbook-Hop-Count: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        method, prefix, path,
        host, port,
        via,
        hop_count + 1);

    int rc = grid_send_all(fd, request, (size_t)rlen);
    if (rc != 0) {
        sock_close(fd);
        free(host); free(port);
        return -1;
    }

    size_t body_len = 0;
    int status = 0;
    char *body = grid_recv_response(fd, &body_len, &status);
    sock_close(fd);
    free(host); free(port);

    response->status = status;
    response->body = body;
    response->body_len = body_len;
    return 0;
}

int cookbook_grid_get(const cookbook_peer *peer,
                      const char *path,
                      const char *origin_id,
                      const char *via_chain,
                      int hop_count,
                      cookbook_grid_response *response) {
    return grid_request(peer, "GET", path, origin_id, via_chain,
                        hop_count, response);
}

int cookbook_grid_head(const cookbook_peer *peer,
                       const char *path,
                       const char *origin_id,
                       const char *via_chain,
                       int hop_count,
                       cookbook_grid_response *response) {
    return grid_request(peer, "HEAD", path, origin_id, via_chain,
                        hop_count, response);
}

/* ---- Loop detection ---- */

int cookbook_grid_is_loop(const char *origin_id, const char *via_chain) {
    if (!origin_id || !via_chain) return 0;

    size_t id_len = strlen(origin_id);
    const char *p = via_chain;

    while (*p) {
        /* skip whitespace and commas */
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;

        /* find end of this segment */
        const char *end = p;
        while (*end && *end != ',' && *end != ' ') end++;
        size_t seg_len = (size_t)(end - p);

        if (seg_len == id_len && memcmp(p, origin_id, id_len) == 0)
            return 1;

        p = end;
    }
    return 0;
}
