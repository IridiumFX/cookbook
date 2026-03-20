#ifndef COOKBOOK_CONNPOOL_H
#define COOKBOOK_CONNPOOL_H

/*
 * cookbook_connpool.h — Connection pool for outbound TCP/TLS sockets
 *
 * Reuses idle connections to the same host:port, avoiding TCP handshake
 * and TLS negotiation overhead on repeated requests (grid federation,
 * S3 operations, OIDC token exchange).
 *
 * Thread-safe: protected by mutex. Idle connections expire after timeout.
 */

#include "cookbook.h"
#include "cookbook_socket.h"
#include <stddef.h>

typedef struct cookbook_connpool cookbook_connpool;

/* Create a connection pool.
   max_idle: maximum idle connections to keep (per host, 0 = no pooling).
   idle_timeout_sec: seconds before idle connections are closed. */
COOKBOOK_API cookbook_connpool *cookbook_connpool_create(int max_idle,
                                                       int idle_timeout_sec);

/* Get a connection from the pool, or create a new one.
   Returns COOKBOOK_SOCK_INVALID on failure.
   use_tls: 1 for TLS, 0 for plain TCP. */
COOKBOOK_API cookbook_sock_t cookbook_connpool_get(cookbook_connpool *pool,
                                                 const char *host, int port,
                                                 int use_tls);

/* Return a connection to the pool for reuse.
   If the pool is full, the connection is closed. */
COOKBOOK_API void cookbook_connpool_put(cookbook_connpool *pool,
                                       cookbook_sock_t sock,
                                       const char *host, int port,
                                       int use_tls);

/* Close all idle connections and free the pool. */
COOKBOOK_API void cookbook_connpool_destroy(cookbook_connpool *pool);

#endif /* COOKBOOK_CONNPOOL_H */
