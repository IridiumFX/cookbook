#ifndef COOKBOOK_TLS_H
#define COOKBOOK_TLS_H

/*
 * cookbook_tls.h — TLS 1.3 client (RFC 8446)
 *
 * Minimal TLS 1.3 client built on:
 *   - cookbook_socket.h for TCP
 *   - apennines cipher (AES-128-GCM, ChaCha20-Poly1305)
 *   - apennines hash (HKDF-SHA-256)
 *   - apennines ec (X25519 ECDH)
 *   - apennines x509 (certificate verification)
 *
 * Supports:
 *   - TLS_AES_128_GCM_SHA256 (mandatory cipher suite)
 *   - X25519 key exchange (mandatory key exchange)
 *   - Server certificate verification (RSA + ECDSA + Ed25519)
 *   - SNI (Server Name Indication)
 *
 * Does NOT support:
 *   - Client certificates
 *   - Session resumption / PSK
 *   - 0-RTT
 *   - Renegotiation (not in TLS 1.3)
 */

#include "cookbook.h"
#include "cookbook_socket.h"
#include <stddef.h>

/* Opaque TLS connection context */
typedef struct cookbook_tls cookbook_tls;

/* Perform TLS 1.3 handshake over an existing TCP socket.
   hostname is used for SNI and certificate verification.
   Returns NULL on handshake failure. */
COOKBOOK_API cookbook_tls *cookbook_tls_connect(cookbook_sock_t sock,
                                              const char *hostname);

/* Send data over the TLS connection.
   Returns 0 on success, -1 on failure. */
COOKBOOK_API int cookbook_tls_send(cookbook_tls *tls,
                                  const void *data, size_t len);

/* Receive data from the TLS connection.
   Returns bytes received (> 0), 0 on EOF, -1 on error. */
COOKBOOK_API int cookbook_tls_recv(cookbook_tls *tls,
                                  void *buf, size_t len);

/* Close the TLS connection (sends close_notify).
   Does NOT close the underlying socket. */
COOKBOOK_API void cookbook_tls_close(cookbook_tls *tls);

#endif /* COOKBOOK_TLS_H */
