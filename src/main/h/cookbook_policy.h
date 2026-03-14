#ifndef COOKBOOK_POLICY_H
#define COOKBOOK_POLICY_H

#include "cookbook.h"
#include "cookbook_db.h"

/* ---- Policy storage (CRUD) ---- */

/* Store or replace a policy pastlet for a subject.
   kind is "user" or "team". pastlet is raw pasta text. */
COOKBOOK_API int cookbook_policy_put(cookbook_db *db, const char *subject,
                                    const char *kind, const char *pastlet);

/* Load a policy pastlet for a subject. Returns malloc'd string or NULL. */
COOKBOOK_API char *cookbook_policy_get(cookbook_db *db, const char *subject);

/* Delete a policy for a subject. Returns 0 on success. */
COOKBOOK_API int cookbook_policy_delete(cookbook_db *db, const char *subject);

/* ---- Permission resolution via alforno ---- */

/*
 * Resolve effective permissions for a subject by aggregating their personal
 * pastlet with all team pastlets they belong to.
 *
 * Returns a malloc'd JSON string with the resolved grants and excludes:
 *   {"grants":{"com.iridiumfx":"crwd",...},"exclude":{"com.secret":true,...}}
 *
 * Returns NULL on error (no policy found, alforno failure, etc.).
 */
COOKBOOK_API char *cookbook_policy_resolve(cookbook_db *db, const char *subject);

/* ---- Permission checking ---- */

/*
 * Check if a grant set allows an operation on a group.
 *
 * grants_json:  JSON string from JWT v2 "grants" field
 * exclude_json: JSON string from JWT v2 "exclude" field (may be NULL)
 * group_id:     the artifact group being accessed (e.g. "com.iridiumfx.pasta")
 * op:           operation character: 'c', 'r', 'w', or 'd'
 *
 * Returns 1 if allowed, 0 if denied.
 */
COOKBOOK_API int cookbook_auth_check(const char *grants_json,
                                    const char *exclude_json,
                                    const char *group_id,
                                    char op);

#endif /* COOKBOOK_POLICY_H */
