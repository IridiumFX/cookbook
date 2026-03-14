#ifndef COOKBOOK_GRID_H
#define COOKBOOK_GRID_H

#include "cookbook.h"
#include "cookbook_db.h"
#include <stddef.h>

/* ---- Peer definition ---- */

typedef struct {
    char   *peer_id;
    char   *name;
    char   *url;        /* base URL, e.g. "http://east-1:8080" */
    char    mode;       /* 'r' = redirect, 'p' = proxy */
    int     priority;   /* lower = preferred */
    int     enabled;
} cookbook_peer;

/* ---- Grid HTTP response ---- */

typedef struct {
    int     status;     /* HTTP status code */
    char   *body;       /* malloc'd response body (NUL-terminated) */
    size_t  body_len;
} cookbook_grid_response;

/* ---- Peer management ---- */

/* Load enabled peers from DB, sorted by priority.
   Returns count. Caller must free with cookbook_grid_free_peers(). */
COOKBOOK_API int cookbook_grid_load_peers(cookbook_db *db,
                                         cookbook_peer **out);

COOKBOOK_API void cookbook_grid_free_peers(cookbook_peer *peers, int count);

/* ---- Grid HTTP client ---- */

/* Perform GET against a peer's /grid/ endpoint.
   path should start with "/" (e.g., "/grid/resolve/org/acme/lib/*").
   origin_id: this node's registry_id (for Via header).
   via_chain: existing Via chain (may be NULL).
   hop_count: current hop count (incremented before sending).
   Returns 0 on success, -1 on error. Caller must free response->body. */
COOKBOOK_API int cookbook_grid_get(const cookbook_peer *peer,
                                  const char *path,
                                  const char *origin_id,
                                  const char *via_chain,
                                  int hop_count,
                                  cookbook_grid_response *response);

/* Perform HEAD against a peer's /grid/ endpoint.
   Same as grid_get but only retrieves status code (body is NULL). */
COOKBOOK_API int cookbook_grid_head(const cookbook_peer *peer,
                                   const char *path,
                                   const char *origin_id,
                                   const char *via_chain,
                                   int hop_count,
                                   cookbook_grid_response *response);

/* ---- Loop detection ---- */

/* Check if origin_id appears in the via_chain.
   Returns 1 if loop detected, 0 if safe. */
COOKBOOK_API int cookbook_grid_is_loop(const char *origin_id,
                                      const char *via_chain);

/* Default max hops. */
#define COOKBOOK_GRID_MAX_HOPS_DEFAULT 3

#endif /* COOKBOOK_GRID_H */
