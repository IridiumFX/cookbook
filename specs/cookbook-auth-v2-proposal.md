# Cookbook Auth v2 — Proposal

**Created**: 2026-03-10
**Updated**: 2026-03-14
**Status**: Phases 1–2 implemented, Phase 3 next
**Depends on**: Pasta #4 (dotted labels), Alforno #4 (merge:"collect"), Basta #2 (optional)

---

## 1. Motivation

Cookbook v1 auth has:
- JWT tokens with `sub` + `groups` (comma-separated string)
- Credential verification via Basic auth + Argon2id
- Publisher keys (Ed25519) for signing
- Rate limiting per-subject

This is sufficient for a single-team, single-registry deployment. It does not
address:

- **Differential visibility**: team A should not see team B's artifacts unless
  explicitly granted
- **Hierarchical group scoping**: access to `com.iridiumfx` implies access to
  `com.iridiumfx.pasta`, `com.iridiumfx.basta`, etc.
- **Fine-grained permissions**: separate create / read / write / delete
- **Explicit include/exclude lists**: grant a broad root, carve out exceptions
- **Grid propagation**: when node A fans out to node B on behalf of user Alice,
  what claims does B see?
- **User profiles**: display name, team memberships, contact info
- **Team-normalized access**: an individual contributor is also a team of one

---

## 2. Design Principles

1. **Pasta-native**: access policies are pastlets, not JSON or YAML
2. **Alforno-resolved**: effective permissions are computed by aggregating
   pastlets (personal grants + team grants + org grants)
3. **Convention over schema**: a team is just a pastlet with `@identity` and
   `@grants` sections — no new data model
4. **Hierarchical prefix matching**: `com.iridiumfx: "r"` implies read on
   everything under `com.iridiumfx.*`
5. **Individual = team of one**: Alice's subject `alice` is implicitly team
   `alice`, making the access model uniform
6. **JWT carries the resolved claim**: the server doesn't re-aggregate on
   every request — the JWT embeds the effective grants at token issue time

---

## 3. Pastlet Conventions

### 3.1 Identity Pastlet (user or team)

```
; alice.pasta — user profile
@identity {
    subject: "alice",
    display_name: "Alice Chen",
    email: "alice@iridiumfx.com",
    kind: "user",
    teams: [core, platform]
}

@grants {
    com.iridiumfx: "crwd",
    org.acme.sdk: "r"
}
```

```
; core-team.pasta — team profile
@identity {
    team_id: "core",
    display_name: "Core Team",
    kind: "team"
}

@grants {
    com.iridiumfx: "crwd",
    com.iridiumfx.internal: "crwd"
}
```

Notes:
- `teams` uses `PASTA_LABEL` values — bare identifiers referencing team
  pastlets, resolved by alforno link pass
- `kind` is `"user"` or `"team"` (informational; processing doesn't branch
  on it)
- Grant keys are group coordinate prefixes
- Grant values are permission strings: any combination of `c` `r` `w` `d`

### 3.2 Permission Characters

| Char | Meaning | HTTP methods |
|------|---------|--------------|
| `c`  | Create  | PUT (new artifact), POST /keys |
| `r`  | Read    | GET /resolve, GET /artifact, GET /mirror |
| `w`  | Write   | PUT (overwrite), POST /yank |
| `d`  | Delete  | DELETE (future: unpublish) |

A grant of `"r"` on `com.iridiumfx` means read-only access to all artifacts
whose `group_id` starts with `com.iridiumfx`.

### 3.3 Exclude Lists

```
@grants {
    com.iridiumfx: "crwd"
}

@exclude {
    com.iridiumfx.secret: true,
    com.iridiumfx.internal.keys: true
}
```

Exclude entries deny all permissions on the specified prefix, regardless of
grants. Excludes win over grants (deny-overrides-allow).

### 3.4 Effective Permission Resolution

Given user Alice with teams `[core, platform]`:

```
alforno conflate --recipe <dynamic> alice.pasta core-team.pasta platform-team.pasta
```

The resolver uses `ALF_CONFLATE` with `merge: "collect"` on the `@grants`
section. This means same-key collisions produce arrays of all values instead
of last-write-wins. For example, if alice.pasta has `com.iridiumfx: "r"` and
core-team.pasta has `com.iridiumfx: "cw"`, the collected output is
`com.iridiumfx: ["r", "cw"]`. The application then ORs all permission
characters together → `"rcw"`.

The `@exclude` section uses `merge: "replace"` (last-write-wins is fine for
boolean deny flags).

**Dynamic recipe**: Because conflate drops fields not listed in the recipe,
and we don't know grant keys ahead of time, `cookbook_policy_resolve()` scans
all input pastlets for `@grants`/`@exclude` keys and builds a recipe
containing the full field union at runtime.

**Resolution algorithm** (applied after alforno conflate):

1. Collect all `@grants` entries from the conflated output
2. For array values (from merge:"collect"), OR all permission characters
3. Collect all `@exclude` entries from the conflated output
4. For a request to `group_id = G` with operation `op`:
   a. Find the longest grant prefix matching `G`
   b. Check if `op` character is in the resolved grant value
   c. Check if any exclude prefix matches `G`
   d. **Allow** if grant matches AND op permitted AND no exclude matches

### 3.5 Pastlet Storage

Pastlets can be stored:
- **In the database** — new `policies` table with `subject TEXT PK`,
  `pastlet TEXT NOT NULL`, `updated_at TEXT`
- **On the filesystem** — `COOKBOOK_POLICY_DIR` env var pointing to a
  directory of `.pasta` files (one per subject/team)
- **In the object store** — under a reserved `_policies/` prefix

The database approach is simplest for single-node. Filesystem is better for
GitOps workflows where policies are version-controlled. Object store suits
grid deployments where policies should replicate.

---

## 4. JWT v2

### 4.1 Token Claims

Current JWT payload:
```json
{"sub": "alice", "groups": "admin,publish", "iat": ..., "exp": ...}
```

Proposed JWT v2 payload:
```json
{
    "sub": "alice",
    "kind": "user",
    "grants": {
        "com.iridiumfx": "crwd",
        "org.acme.sdk": "r"
    },
    "exclude": {
        "com.iridiumfx.secret": true
    },
    "teams": ["core", "platform"],
    "iat": 1741564800,
    "exp": 1741568400
}
```

The `grants` and `exclude` maps are the resolved output from alforno
conflate with `merge: "collect"`. Permission arrays are OR'd into single
strings before JWT embedding. They are computed once at token issue time
(`POST /auth/token`) and embedded in the JWT. This means:

- No per-request policy lookups
- Token is self-contained for grid propagation
- Policy changes take effect on next token refresh

### 4.2 Backward Compatibility

If the JWT contains `groups` but no `grants`, fall back to v1 behavior
(groups-based access, no differential visibility). This allows rolling
upgrades.

### 4.3 Token Issue Flow

```
POST /auth/token
Authorization: Basic base64(alice:token)

1. Verify credential (existing Argon2id flow)
2. Load alice.pasta from policy store
3. Resolve team memberships → load team pastlets
4. Build dynamic conflate recipe (union of all grant/exclude keys)
5. alforno conflate (merge:"collect") alice.pasta + team pastlets
6. OR collected permission arrays → single permission strings
7. Extract @grants and @exclude from resolved output, serialize to JSON
8. Build JWT v2 with grants/exclude/teams (cookbook_jwt_create_v2)
9. Sign and return (v1 fallback if no policy exists for subject)
```

---

## 5. Enforcement

### 5.1 Middleware Check

Every request handler gains a permission check after JWT validation:

```c
/* Returns 1 if allowed, 0 if denied */
int cookbook_auth_check(const char *grants_json,
                       const char *exclude_json,
                       const char *group_id,
                       char operation);  /* 'c', 'r', 'w', 'd' */
```

The function:
1. Parses `grants_json` (cached from JWT decode)
2. Finds the longest prefix in grants matching `group_id`
3. Checks the operation character is present
4. Checks no exclude prefix matches
5. Returns allow/deny

### 5.2 Endpoint Mapping

| Endpoint | Operation |
|----------|-----------|
| `GET /resolve/{g}/{a}/{range}` | `r` on group `g` |
| `GET /artifact/{g}/...` | `r` on group `g` |
| `PUT /artifact/{g}/...` | `c` (new) or `w` (overwrite) on group `g` |
| `POST /artifact/{g}/.../yank` | `w` on group `g` |
| `GET /mirror/manifest` | `r` — filtered by visible groups |
| `POST /keys` | `c` on the group in the key's scope |
| `GET /metrics`, `/healthz`, `/readyz` | No auth required |

### 5.3 Mirror Manifest Filtering

`GET /mirror/manifest` currently returns all published artifacts. With v2,
the response is filtered to only include artifacts whose `group_id` the
requesting user has `r` access to. This applies both to local queries and
grid fan-out results.

---

## 6. Grid Propagation

### 6.1 The Problem

When node A fans out to node B on behalf of user Alice, node B needs to
enforce visibility. Three options:

| Option | Mechanism | Pros | Cons |
|--------|-----------|------|------|
| A | Forward Alice's JWT | Simple | All nodes share credential namespace |
| B | Peer-level trust | No user context needed | Coarse-grained |
| C | Scoped claim | Fine-grained, no shared creds | New header |

### 6.2 Recommended: Option C — Scoped Claim

Node A extracts the relevant grants from Alice's JWT and passes them in a
grid-internal header:

```
X-Cookbook-Grid-Grants: com.iridiumfx:r,org.acme:r
X-Cookbook-Grid-Exclude: com.iridiumfx.secret
```

Node B uses these grants to filter its local results before returning them
to node A. The grants are derived (not the full JWT), so:

- No shared credential store needed across nodes
- Peer nodes only see the grants relevant to this request
- Alice's full token never leaves node A

### 6.3 Peer Authentication

Grid peers authenticate to each other via their Ed25519 registry keys (already
stored in the `peers` table as `public_key`). The requesting peer signs the
grid request headers; the receiving peer verifies the signature against the
registered public key. This prevents spoofed grant headers.

---

## 7. Schema Changes

### 7.1 New Table: `policies`

```sql
CREATE TABLE IF NOT EXISTS policies (
    subject     TEXT PRIMARY KEY,
    kind        TEXT NOT NULL DEFAULT 'user',  -- 'user' or 'team'
    pastlet     TEXT NOT NULL,                  -- raw pasta text
    updated_at  TEXT NOT NULL DEFAULT (datetime('now'))
);
```

### 7.2 Modified Table: `credentials`

Add column:
```sql
ALTER TABLE credentials ADD COLUMN kind TEXT NOT NULL DEFAULT 'user';
```

### 7.3 New Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/admin/policies` | List all policies |
| GET | `/admin/policies/{subject}` | Get policy pastlet for subject |
| PUT | `/admin/policies/{subject}` | Upload/replace policy pastlet |
| DELETE | `/admin/policies/{subject}` | Remove policy |
| GET | `/admin/policies/{subject}/effective` | Resolve via alforno and return |

---

## 8. Dependencies

| Dependency | Version | Needed for | Status |
|------------|---------|------------|--------|
| Pasta | #4 (dotted labels) | Bare dotted keys in pastlets, `PASTA_LABEL` for team refs | Vendored, integrated |
| Alforno | #4 (7 features) | `merge: "collect"` for permission OR, conflate mode | Vendored, integrated |
| Basta | #2 (dot sync) | Optional: binary credential storage | Vendored, not yet integrated |

### 8.1 Alforno Integration

Alforno is vendored as a static library (`vendor/alforno/`). At token issue
time, `cookbook_policy_resolve()` builds a dynamic conflate recipe and calls:

```c
/* scan all input pastlets for @grants/@exclude keys to build recipe */
/* (see cookbook_policy.c for full dynamic recipe construction) */
char *recipe = build_dynamic_recipe(all_pastlets, npastlets);

AlfContext *ctx = alf_create(ALF_CONFLATE, &result);
alf_set_recipe(ctx, recipe, strlen(recipe), &result);
free(recipe);

alf_add_input(ctx, user_pastlet, strlen(user_pastlet), &result);
for (int i = 0; i < nteams; i++)
    alf_add_input(ctx, team_pastlets[i], strlen(team_pastlets[i]), &result);

PastaValue *resolved = alf_process(ctx, &result);
// extract @grants (arrays from collect → OR'd) and @exclude sections
// serialize to JSON via serialize_grants_json()
alf_free(ctx);
```

The dynamic recipe contains:
- `@grants` section with `merge: "collect"` and all discovered grant keys
- `@exclude` section with `merge: "replace"` and all discovered exclude keys

### 8.2 Build System

All libraries are statically linked. Alforno uses cookbook's vendored pasta
via `FETCHCONTENT_FULLY_DISCONNECTED`:

```cmake
target_compile_definitions(pasta PUBLIC PASTA_STATIC)
add_subdirectory(vendor/alforno)
target_compile_definitions(alforno PUBLIC ALF_STATIC)
target_link_libraries(cookbook PUBLIC sqlite3 civetweb pasta alforno sodium)
```

---

## 9. Migration Path

### Phase 1: Schema + storage (no enforcement) — DONE
- `policies` table with subject PK, kind, pastlet, updated_at
- `/admin/policies` CRUD endpoints (list, get, put, delete, effective)
- `cookbook_policy_resolve()` with alforno conflate + merge:"collect"
- `cookbook_auth_check()` with hierarchical prefix matching + exclude
- 44 new tests (258 → 302)

### Phase 2: JWT v2 + alforno resolution — DONE
- `cookbook_jwt_create_v2()` embeds resolved grants/exclude in JWT payload
- `cookbook_jwt_verify()` extracts grants/exclude from v2 tokens
- `POST /auth/token` calls policy resolver → JWT v2 (v1 fallback if no policy)
- v1 JWTs still decode correctly (backward compat verified)
- Submodules updated: Pasta #4 (dotted labels), Alforno #4 (7 features), Basta #2
- Build system overhauled: all-static linking, PUBLIC compile definitions
- 40 new tests (302 → 342)

### Phase 3: Enforcement — NEXT
- Wire `cookbook_auth_check()` into all request handlers
- Mirror manifest filtering by visibility
- Grid grant propagation headers

### Phase 4: Grid peer auth
- Peer request signing with Ed25519
- `X-Cookbook-Grid-Grants` header enforcement
- Peer key exchange via `/admin/peers` update

---

## 10. Open Questions

1. ~~**Grant merging semantics**~~: **RESOLVED** — Alforno's `merge: "collect"`
   directive solves this. Same-key collisions produce arrays; cookbook ORs all
   permission characters together. Input order no longer matters. Verified by
   `test_policy_resolve_collect()`: user `"r"` + team `"cw"` → `"rcw"`.

2. **Wildcard grants**: Should `*: "r"` mean "read everything"? Useful for
   admin accounts but dangerous. Alternative: reserved `@admin` role.

3. **Group ownership**: Currently `groups` table has `owner_sub`. Should
   ownership be expressed in the policy pastlet instead?

4. **Token refresh**: How does the client know to refresh when policies change?
   Options: short TTL, push notification, or explicit revocation endpoint.

5. ~~**Alforno maturity**~~: **RESOLVED** — Alforno #4 shipped with 7 features
   including `merge: "collect"`, `merge: "deep"`, conditional `when` sections,
   validation pass, `@include` directive, scatter, and gather. Vendored at
   commit 0bee5ee and fully integrated.

6. **Basta for policies**: Should policy pastlets support blobs (for embedding
   keys/certs)? Basta #2 is vendored but not yet integrated into the build.
   Would require `ALF_USE_BASTA` compile toggle.

---

## Appendix A: Worked Example

### Setup

```
; alice.pasta
@identity {
    subject: "alice",
    kind: "user",
    teams: [core]
}
@grants {
    com.iridiumfx: "crwd"
}

; bob.pasta
@identity {
    subject: "bob",
    kind: "user",
    teams: [external]
}
@grants {
    org.acme: "r"
}

; core-team.pasta
@identity {
    team_id: "core",
    kind: "team"
}
@grants {
    com.iridiumfx: "crwd",
    com.iridiumfx.internal: "crwd"
}
@exclude {
    com.iridiumfx.secret: true
}

; external-team.pasta
@identity {
    team_id: "external",
    kind: "team"
}
@grants {
    com.iridiumfx.sdk: "r"
}
```

### Alice's Effective Permissions

```
alforno conflate --recipe <dynamic> alice.pasta core-team.pasta
```

Collected output (before OR):
```
@grants {
    com.iridiumfx: ["crwd", "crwd"],
    com.iridiumfx.internal: "crwd"
}
@exclude {
    com.iridiumfx.secret: true
}
```

After OR: `com.iridiumfx: "crwd"`, `com.iridiumfx.internal: "crwd"`

Alice can:
- Read/write/create/delete anything under `com.iridiumfx.*`
- **Except** `com.iridiumfx.secret.*` (excluded)

### Bob's Effective Permissions

```
alforno conflate --recipe <dynamic> bob.pasta external-team.pasta
```

Result (after OR):
```
@grants {
    org.acme: "r",
    com.iridiumfx.sdk: "r"
}
```

Bob can:
- Read anything under `org.acme.*`
- Read anything under `com.iridiumfx.sdk.*`
- Nothing else

### Grid Scenario

Alice connects to grid node `west-1`. She requests
`GET /artifact/com/iridiumfx/pasta/1.0.0/pasta-1.0.0.tar`. The artifact
is on `east-1`.

1. `west-1` validates Alice's JWT v2, extracts grants
2. `west-1` fans out to `east-1` with:
   ```
   GET /grid/artifact/com/iridiumfx/pasta/1.0.0/pasta-1.0.0.tar
   X-Cookbook-Via: west-1
   X-Cookbook-Hop-Count: 1
   X-Cookbook-Grid-Grants: com.iridiumfx:crwd
   X-Cookbook-Grid-Exclude: com.iridiumfx.secret
   ```
3. `east-1` checks: `com.iridiumfx.pasta` matches grant `com.iridiumfx:crwd`,
   not excluded → **allowed**
4. `east-1` returns artifact, `west-1` proxies to Alice
