/*
 * cookbook_socket.c — Platform-abstracted TCP socket layer
 *
 * Win32: winsock2 (ws2_32.dll, already linked for civetweb/S3/grid)
 * POSIX: BSD sockets
 * Nova:  replace this file with Nova networking API
 */

#include "cookbook_socket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
/* ws2_32 already linked via civetweb */
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#endif

cookbook_sock_t cookbook_sock_connect(const char *host, int port,
                                     int timeout_sec) {
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return COOKBOOK_SOCK_INVALID;

    cookbook_sock_t s = socket(res->ai_family, res->ai_socktype,
                               res->ai_protocol);
    if (s == COOKBOOK_SOCK_INVALID) {
        freeaddrinfo(res);
        return COOKBOOK_SOCK_INVALID;
    }

    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        cookbook_sock_close(s);
        freeaddrinfo(res);
        return COOKBOOK_SOCK_INVALID;
    }
    freeaddrinfo(res);

    /* set timeout if requested */
    if (timeout_sec > 0) {
#ifdef _WIN32
        DWORD ms = (DWORD)timeout_sec * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
#else
        struct timeval tv;
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    }

    return s;
}

int cookbook_sock_send(cookbook_sock_t s, const void *data, size_t len) {
    const char *p = (const char *)data;
    while (len > 0) {
        int chunk = (len > 65536) ? 65536 : (int)len;
        int n = send(s, p, chunk, 0);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int cookbook_sock_recv(cookbook_sock_t s, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t got = 0;
    while (got < len) {
        int n = recv(s, p + got, (int)(len - got), 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

int cookbook_sock_recv_partial(cookbook_sock_t s, void *buf, size_t max_len) {
    return recv(s, (char *)buf, (int)max_len, 0);
}

char *cookbook_sock_recv_http(cookbook_sock_t s, size_t *out_len, int *status) {
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
        int n = cookbook_sock_recv_partial(s, buf + total, cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    buf[total] = '\0';

    /* parse HTTP status from first line: "HTTP/1.x NNN ..." */
    *status = 0;
    const char *sp = strchr(buf, ' ');
    if (sp) *status = atoi(sp + 1);

    *out_len = total;
    return buf;
}

void cookbook_sock_close(cookbook_sock_t s) {
    if (s == COOKBOOK_SOCK_INVALID) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}
