#ifndef COOKBOOK_OIDC_H
#define COOKBOOK_OIDC_H

#include "cookbook.h"
#include <stddef.h>

/* OIDC configuration */
typedef struct {
    const char *issuer;       /* e.g. "https://idp.example.com" */
    const char *client_id;    /* cookbook's registered client_id */
} cookbook_oidc_config;

/* Validate client credentials against the OIDC issuer's token endpoint.
   Posts grant_type=client_credentials to {issuer}/oauth/token (or discovered endpoint).
   On success, extracts the subject from the access token and returns 0.
   sub_out must be at least 128 bytes. */
COOKBOOK_API int cookbook_oidc_client_credentials(const cookbook_oidc_config *cfg,
                                                  const char *client_id,
                                                  const char *client_secret,
                                                  char *sub_out, size_t sub_sz);

#endif /* COOKBOOK_OIDC_H */
