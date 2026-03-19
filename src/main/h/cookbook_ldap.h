#ifndef COOKBOOK_LDAP_H
#define COOKBOOK_LDAP_H

#include "cookbook.h"

/* LDAP authentication configuration */
typedef struct {
    const char *url;         /* ldap://host:389 or ldaps://host:636 */
    const char *base_dn;     /* e.g. "ou=users,dc=example,dc=com" */
    const char *user_attr;   /* e.g. "uid" (default) or "cn" or "sAMAccountName" */
    const char *group_attr;  /* e.g. "memberOf" — NULL to skip group lookup */
    const char *group_base;  /* base DN for group search — NULL to use base_dn */
} cookbook_ldap_config;

/* Attempt LDAP simple bind for the given subject/password.
   Returns 0 on success, -1 on failure.
   If groups_out is non-NULL and group_attr is set, fills it with a
   comma-separated list of group CNs (caller must free). */
COOKBOOK_API int cookbook_ldap_bind(const cookbook_ldap_config *cfg,
                                   const char *subject,
                                   const char *password,
                                   char **groups_out);

#endif /* COOKBOOK_LDAP_H */
