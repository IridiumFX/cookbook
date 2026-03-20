/*
 * cookbook_connpool.c — Connection pool for outbound TCP/TLS sockets
 *
 * Simple bounded pool: stores idle connections keyed by host:port:tls.
 * Thread-safe via mutex. Expired connections pruned on each get/put.
 */

#include "cookbook_connpool.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#define POOL_MAX_ENTRIES 128

typedef struct {
    cookbook_sock_t sock;
    char           host[256];
    int            port;
    int            use_tls;
    int64_t        idle_since;  /* unix timestamp */
} pool_entry;

struct cookbook_connpool {
    pool_entry entries[POOL_MAX_ENTRIES];
    int        count;
    int        max_idle;
    int        idle_timeout_sec;
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t  lock;
#endif
};

cookbook_connpool *cookbook_connpool_create(int max_idle, int idle_timeout_sec) {
    cookbook_connpool *pool = calloc(1, sizeof(*pool));
    if (!pool) return NULL;
    pool->max_idle = max_idle > 0 ? max_idle : 4;
    pool->idle_timeout_sec = idle_timeout_sec > 0 ? idle_timeout_sec : 60;
#ifdef _WIN32
    InitializeCriticalSection(&pool->lock);
#else
    pthread_mutex_init(&pool->lock, NULL);
#endif
    return pool;
}

static void pool_lock(cookbook_connpool *pool) {
#ifdef _WIN32
    EnterCriticalSection(&pool->lock);
#else
    pthread_mutex_lock(&pool->lock);
#endif
}

static void pool_unlock(cookbook_connpool *pool) {
#ifdef _WIN32
    LeaveCriticalSection(&pool->lock);
#else
    pthread_mutex_unlock(&pool->lock);
#endif
}

/* Prune expired entries (must be called with lock held) */
static void pool_prune(cookbook_connpool *pool) {
    int64_t now = (int64_t)time(NULL);
    int w = 0;
    for (int i = 0; i < pool->count; i++) {
        if (now - pool->entries[i].idle_since < pool->idle_timeout_sec) {
            if (w != i) pool->entries[w] = pool->entries[i];
            w++;
        } else {
            cookbook_sock_close(pool->entries[i].sock);
        }
    }
    pool->count = w;
}

cookbook_sock_t cookbook_connpool_get(cookbook_connpool *pool,
                                     const char *host, int port,
                                     int use_tls) {
    if (!pool || !host) return COOKBOOK_SOCK_INVALID;

    pool_lock(pool);
    pool_prune(pool);

    /* find a matching idle connection */
    for (int i = 0; i < pool->count; i++) {
        if (pool->entries[i].port == port &&
            pool->entries[i].use_tls == use_tls &&
            strcmp(pool->entries[i].host, host) == 0) {
            cookbook_sock_t s = pool->entries[i].sock;
            /* remove from pool */
            pool->entries[i] = pool->entries[--pool->count];
            pool_unlock(pool);
            return s;
        }
    }
    pool_unlock(pool);

    /* no pooled connection — create new */
    if (use_tls) {
        /* for TLS, we can't pool at the raw socket level easily
           because the TLS state is per-connection. Return INVALID
           to signal caller should use cookbook_sock_connect_tls. */
        return COOKBOOK_SOCK_INVALID;
    }
    return cookbook_sock_connect(host, port, 10);
}

void cookbook_connpool_put(cookbook_connpool *pool, cookbook_sock_t sock,
                           const char *host, int port, int use_tls) {
    if (!pool || sock == COOKBOOK_SOCK_INVALID) return;

    pool_lock(pool);
    pool_prune(pool);

    /* count connections to this host */
    int host_count = 0;
    for (int i = 0; i < pool->count; i++) {
        if (pool->entries[i].port == port &&
            strcmp(pool->entries[i].host, host) == 0)
            host_count++;
    }

    if (host_count >= pool->max_idle || pool->count >= POOL_MAX_ENTRIES) {
        /* pool full — close the connection */
        pool_unlock(pool);
        cookbook_sock_close(sock);
        return;
    }

    pool_entry *e = &pool->entries[pool->count++];
    e->sock = sock;
    snprintf(e->host, sizeof(e->host), "%s", host);
    e->port = port;
    e->use_tls = use_tls;
    e->idle_since = (int64_t)time(NULL);

    pool_unlock(pool);
}

void cookbook_connpool_destroy(cookbook_connpool *pool) {
    if (!pool) return;

    pool_lock(pool);
    for (int i = 0; i < pool->count; i++)
        cookbook_sock_close(pool->entries[i].sock);
    pool->count = 0;
    pool_unlock(pool);

#ifdef _WIN32
    DeleteCriticalSection(&pool->lock);
#else
    pthread_mutex_destroy(&pool->lock);
#endif
    free(pool);
}
