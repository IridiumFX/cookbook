#ifndef COOKBOOK_GRID_H
#define COOKBOOK_GRID_H

#include "cookbook.h"
#include "cookbook_db.h"
#include <stddef.h>
#include <stdint.h>

/* ---- Peer definition ---- */

typedef struct {
    char   *peer_id;
    char   *name;
    char   *url;        /* base URL, e.g. "http://east-1:8080" */
    char    mode;       /* 'r' = redirect, 'p' = proxy */
    int     priority;   /* lower = preferred */
    int     enabled;
    unsigned char public_key[32]; /* Ed25519 public key (binary) */
    int     has_public_key;       /* 1 if public_key is populated */
} cookbook_peer;

/* ---- Grid peer auth signing context ---- */

typedef struct {
    const char          *registry_id;  /* origin for X-Cookbook-Grid-Origin */
    const unsigned char *registry_sk;  /* 64-byte Ed25519 secret key */
    int                  has_key;      /* 0 = skip signing */
} cookbook_grid_sign_ctx;

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

/* Like cookbook_grid_get but with extra headers (for grant propagation).
   extra_headers: additional raw header lines to append (may be NULL).
   Must be pre-formatted as "Header: value\r\n" strings. */
COOKBOOK_API int cookbook_grid_get_ex(const cookbook_peer *peer,
                                      const char *path,
                                      const char *origin_id,
                                      const char *via_chain,
                                      int hop_count,
                                      const char *extra_headers,
                                      cookbook_grid_response *response);

/* Perform HEAD against a peer's /grid/ endpoint.
   Same as grid_get but only retrieves status code (body is NULL). */
COOKBOOK_API int cookbook_grid_head(const cookbook_peer *peer,
                                   const char *path,
                                   const char *origin_id,
                                   const char *via_chain,
                                   int hop_count,
                                   cookbook_grid_response *response);

/* Like cookbook_grid_head but with extra headers. */
COOKBOOK_API int cookbook_grid_head_ex(const cookbook_peer *peer,
                                       const char *path,
                                       const char *origin_id,
                                       const char *via_chain,
                                       int hop_count,
                                       const char *extra_headers,
                                       cookbook_grid_response *response);

/* ---- Signed grid requests (Phase 4 peer auth) ---- */

/* Like cookbook_grid_get_ex but signs the request with the registry's Ed25519 key.
   sign_ctx may be NULL (no signing). */
COOKBOOK_API int cookbook_grid_get_signed(const cookbook_peer *peer,
                                          const char *path,
                                          const char *origin_id,
                                          const char *via_chain,
                                          int hop_count,
                                          const char *extra_headers,
                                          const cookbook_grid_sign_ctx *sign_ctx,
                                          cookbook_grid_response *response);

/* Like cookbook_grid_head_ex but signed. */
COOKBOOK_API int cookbook_grid_head_signed(const cookbook_peer *peer,
                                           const char *path,
                                           const char *origin_id,
                                           const char *via_chain,
                                           int hop_count,
                                           const char *extra_headers,
                                           const cookbook_grid_sign_ctx *sign_ctx,
                                           cookbook_grid_response *response);

/* ---- Peer key lookup ---- */

/* Load a peer's Ed25519 public key by peer_id.
   Returns 0 on success, -1 if not found or no key stored.
   pk_out must be 32 bytes. */
COOKBOOK_API int cookbook_grid_load_peer_key(cookbook_db *db,
                                             const char *peer_id,
                                             unsigned char pk_out[32]);

/* Build the canonical signing input for a grid request.
   Returns malloc'd string and sets *out_len. Caller must free.
   Returns NULL on allocation failure. */
COOKBOOK_API char *cookbook_grid_build_canonical(const char *method,
                                                 const char *path,
                                                 const char *via,
                                                 int hop_count,
                                                 const char *grants,
                                                 const char *exclude,
                                                 int64_t timestamp,
                                                 size_t *out_len);

/* ---- Loop detection ---- */

/* Check if origin_id appears in the via_chain.
   Returns 1 if loop detected, 0 if safe. */
COOKBOOK_API int cookbook_grid_is_loop(const char *origin_id,
                                      const char *via_chain);

/* Default max hops. */
#define COOKBOOK_GRID_MAX_HOPS_DEFAULT 3

/* Connection pool for grid peer connections. */
COOKBOOK_API void cookbook_grid_init_pool(void);
COOKBOOK_API void cookbook_grid_destroy_pool(void);

#endif /* COOKBOOK_GRID_H */
