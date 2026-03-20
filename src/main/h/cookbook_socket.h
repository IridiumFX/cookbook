#ifndef COOKBOOK_SOCKET_H
#define COOKBOOK_SOCKET_H

/*
 * cookbook_socket.h — Platform-abstracted TCP socket layer
 *
 * Provides a minimal TCP client API used by grid federation, S3 backend,
 * LDAP authentication, and future OIDC. The platform-specific code is
 * isolated here so the rest of cookbook is pure C11.
 *
 * Platform support:
 *   - Windows: winsock2 (ws2_32.dll)
 *   - POSIX:   BSD sockets
 *   - Nova:    swap this file with Nova networking API
 */

#include "cookbook.h"
#include <stddef.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET cookbook_sock_t;
#define COOKBOOK_SOCK_INVALID INVALID_SOCKET
#else
typedef int cookbook_sock_t;
#define COOKBOOK_SOCK_INVALID (-1)
#endif

/* Connect to host:port via TCP. Returns COOKBOOK_SOCK_INVALID on failure.
   timeout_sec: 0 = no timeout. */
COOKBOOK_API cookbook_sock_t cookbook_sock_connect(const char *host, int port,
                                                 int timeout_sec);

/* Send exactly len bytes. Returns 0 on success, -1 on failure. */
COOKBOOK_API int cookbook_sock_send(cookbook_sock_t s,
                                   const void *data, size_t len);

/* Receive exactly len bytes. Returns 0 on success, -1 on failure. */
COOKBOOK_API int cookbook_sock_recv(cookbook_sock_t s,
                                   void *buf, size_t len);

/* Receive an HTTP response (dynamic allocation).
   Returns malloc'd buffer (caller frees), sets *out_len and *status.
   Returns NULL on failure. */
COOKBOOK_API char *cookbook_sock_recv_http(cookbook_sock_t s,
                                          size_t *out_len, int *status);

/* Receive up to max_len bytes (non-blocking partial read).
   Returns bytes received, 0 on EOF, -1 on error. */
COOKBOOK_API int cookbook_sock_recv_partial(cookbook_sock_t s,
                                           void *buf, size_t max_len);

/* Close the socket. */
COOKBOOK_API void cookbook_sock_close(cookbook_sock_t s);

/* ---- TLS-wrapped socket ---- */

/* Opaque TLS-wrapped socket (uses cookbook_tls internally) */
typedef struct cookbook_tls_sock cookbook_tls_sock;

/* Connect with TLS. Returns NULL on failure.
   Performs TCP connect + TLS 1.3 handshake. */
COOKBOOK_API cookbook_tls_sock *cookbook_sock_connect_tls(const char *host,
                                                        int port,
                                                        int timeout_sec);

/* Send over TLS. Returns 0 on success, -1 on failure. */
COOKBOOK_API int cookbook_tls_sock_send(cookbook_tls_sock *ts,
                                       const void *data, size_t len);

/* Receive over TLS. Returns bytes received, 0 on EOF, -1 on error. */
COOKBOOK_API int cookbook_tls_sock_recv(cookbook_tls_sock *ts,
                                       void *buf, size_t len);

/* Close TLS + underlying socket. */
COOKBOOK_API void cookbook_tls_sock_close(cookbook_tls_sock *ts);

#endif /* COOKBOOK_SOCKET_H */
