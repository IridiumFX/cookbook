#ifndef COOKBOOK_SERVER_H
#define COOKBOOK_SERVER_H

#include "cookbook.h"
#include "cookbook_db.h"
#include "cookbook_store.h"

typedef struct cookbook_server cookbook_server;

typedef struct {
    const char     *listen_url;    /* e.g. "http://0.0.0.0:8080" */
    const char     *registry_id;   /* e.g. "central" */
    cookbook_db     *db;
    cookbook_store  *store;
    int             max_upload_mb;        /* 0 = unlimited */
    int             pending_timeout_sec;  /* stale pending cleanup; 0 = 3600 */
    int             jwt_ttl_sec;          /* JWT lifetime; 0 = 3600 */
    int             rate_limit_per_min;   /* per-sub rate limit; 0 = unlimited */
    const unsigned char *registry_pk;     /* 32-byte Ed25519 public key */
    const unsigned char *registry_sk;     /* 64-byte Ed25519 secret key */
    int             grid_enabled;         /* 1 = grid federation on */
    int             grid_max_hops;        /* 0 = default (3) */
} cookbook_server_opts;

/* Create and start the server. Returns NULL on failure. */
COOKBOOK_API cookbook_server *cookbook_server_start(const cookbook_server_opts *opts);

/* Run the event loop (poll). Call this repeatedly.
   Returns 0 normally, -1 if the server should stop. */
COOKBOOK_API int cookbook_server_poll(cookbook_server *srv, int timeout_ms);

/* Stop and free the server. */
COOKBOOK_API void cookbook_server_stop(cookbook_server *srv);

/* #8: content negotiation utilities (exposed for testing) */

/* Validate that input is pure US-ASCII (no byte > 0x7F and no NUL).
   Returns 0 if valid, or the 1-based offset of the first bad byte. */
COOKBOOK_API size_t cookbook_validate_ascii(const char *data, size_t len);

/* Serialize a PastaValue tree to JSON. Returns malloc'd string, NULL on error. */
struct PastaValue;
COOKBOOK_API char *cookbook_pasta_to_json(const struct PastaValue *v);

#endif /* COOKBOOK_SERVER_H */
