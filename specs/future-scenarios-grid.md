# Future Scenarios Grid

**Created**: 2026-03-08
**Scope**: Interaction matrix across cookbook, now, pico-http, and pasta
**Baseline**: cookbook M1 Phases A–E complete, pico-http v0.3.0, pasta with PASTA_SORTED

---

## Legend

- **Done** — implemented and tested on both sides
- **Cookbook done** — cookbook endpoint exists and tested; now client not yet wired
- **Spec'd** — in a spec but not yet implemented
- **Gap** — not in any spec yet, but needed
- **In progress** — actively being implemented
- **Resolved** — was a gap, investigation showed it's already handled or not needed

---

## Diary

### 2026-03-08 — Initial grid + gap analysis

Created grid from cookbook M1 capabilities, now-spec-v1.2, pico-http v0.3.0 spec/roadmap.

**Investigation results:**
- **#28 (version listing)**: Resolved — `/resolve/` already returns ALL matching versions
  (not just best match). `dep:updates` can use `/resolve/{g}/{a}/*` to get everything.
  SQL query returns all published non-yanked rows; range filtering is in application code.
- **#24 (advisory API)**: Resolved — not cookbook's responsibility. now spec says advisories
  come from a separate feed (`advisories.now.build/db.pasta`), not the artifact registry.
- **Auth TODO found**: `/auth/token` (line 1672) issues JWTs without credential verification.
  Comment says "auth will be enforced by whatever identity provider is fronting cookbook in
  production." This is an intentional delegation, not a bug — but we should support Basic
  auth against a local credential store for standalone deployments.

### 2026-03-08 — F1 + F2 implemented

**F1 — Yank reason:**
- Added `yank_reason TEXT` column to `artifacts` table (`cookbook_db_migrate.c`)
- `POST /artifact/.../yank` now reads optional `{"reason":"..."}` from body
- `UPDATE` stores reason alongside `yanked = 1` (NULL when no reason given)
- `yanked_check_ctx` extended to carry reason (256 bytes)
- `GET /artifact/...` emits `X-Now-Yank-Reason: <reason>` header alongside `X-Now-Yanked: true`
- Response: `{"status":"yanked","reason":"..."}` when reason provided

**F2 — Resolve yank visibility:**
- `?include_yanked=true` query param on `GET /resolve/`
- Uses different SQL: drops `AND a.yanked = 0` filter, adds `yanked` and `yank_reason` columns
- `resolve_filter_cb` extended: when `include_yanked`, appends `"yanked":true,"yank_reason":"..."` to each yanked version entry
- Non-yanked versions in include_yanked mode have no extra fields (clean JSON)

**Tests:** 219 → 226 (7 new assertions across 2 test functions)
- `test_db_yank_reason`: yank with reason stored, yank without reason stores NULL
- `test_db_resolve_include_yanked`: default excludes yanked, include_yanked returns both, reason preserved

### 2026-03-08 — F3 implemented

**F3 — Credential verification:**
- Added `credentials` table: `subject TEXT PK`, `token_hash TEXT NOT NULL`, `groups TEXT NOT NULL`, `created_at TEXT`, `revoked_at TEXT`
- `POST /auth/token` now checks `Authorization: Basic base64(subject:token)` header
- Token verified against Argon2id hash via `crypto_pwhash_str_verify()` (libsodium)
- Groups sourced from `credentials.groups` column (not from request body)
- Revoked credentials excluded (`revoked_at IS NULL`)
- Falls back to JSON body `{"sub":"...","groups":"..."}` when no Basic header or no matching credential record (backwards compatible)
- Added `cookbook_base64_decode()` for standard base64 (Basic auth uses `+/=`, not `-_`)
- Added `cookbook_credential_hash()` and `cookbook_credential_verify()` to auth API

**Tests:** 226 → 242 (16 new assertions across 4 test functions)
- `test_base64_std_decode`: standard base64 with padding, no-padding, `+/` chars, empty
- `test_credential_hash_verify`: hash + verify roundtrip
- `test_credential_verify_wrong`: wrong token rejected
- `test_credentials_table`: insert, lookup, duplicate PK rejected, revocation excludes

### 2026-03-10 — Grid federation (G1–G4) implemented

**New files:**
- `src/main/h/cookbook_grid.h` — peer struct, grid HTTP client API, loop detection
- `src/main/c/cookbook_grid.c` — raw-socket HTTP client, peer loading, URL parsing

**Schema:** Added `peers` table: `peer_id TEXT PK`, `name`, `url UNIQUE`, `mode` (redirect/proxy), `priority`, `enabled`, `public_key`, `last_seen`, `last_status`

**New endpoints (grid-internal, never fan out):**
- `GET /grid/resolve/{g}/{a}/{range}` — local-only resolve, returns `"source":"registry_id"`
- `GET|HEAD /grid/artifact/{path}` — local-only artifact serve / existence check
- `GET /grid/manifest` — local-only mirror manifest

**New endpoint (admin):**
- `GET /admin/peers` — list peers
- `PUT /admin/peers` — add/update peer (JSON body)
- `DELETE /admin/peers/{id}` — remove peer

**Modified endpoints (client-facing, fan out on miss):**
- `GET /resolve/` — on empty local results + grid enabled + no Via header: iterates peers, calls `/grid/resolve/`, merges results
- `GET /artifact/` — on local 404 + grid enabled: redirect mode (HEAD check → 307) or proxy mode (GET → relay body), adds `X-Cookbook-Source` header

**Loop prevention:** `X-Cookbook-Via` (breadcrumb trail), `X-Cookbook-Hop-Count` (max 3 default), `/grid/` endpoints never fan out

**Config:** `COOKBOOK_GRID_ENABLED=1`, `COOKBOOK_GRID_MAX_HOPS=3`

**Tests:** 242 → 258 (16 new assertions across 3 test functions)
- `test_grid_loop_detection`: exact match, chain match, empty, NULL, prefix/substring not match
- `test_grid_peers_table`: insert, duplicate URL rejected, disable
- `test_grid_peer_load`: load 2 enabled (skips disabled), priority ordering, mode parsing

### 2026-03-10 — Auth v2 Phase 1 implemented

**Submodule updates:**
- Pasta submodule updated to Pasta #2 (`PASTA_LABEL`)
- Basta added as submodule at `vendor/basta/` (not yet integrated)
- Alforno added as submodule at `vendor/alforno/` and integrated into CMake build (static lib)

**Schema:** Added `policies` table: `subject TEXT PK, kind TEXT, pastlet TEXT, updated_at TEXT`

**New module: `cookbook_policy.c`/`.h`:**
- `cookbook_policy_put()` / `cookbook_policy_get()` / `cookbook_policy_delete()` — CRUD for policy pastlets
- `cookbook_policy_resolve()` — loads user pastlet, extracts team memberships from `@identity.teams`, loads team pastlets, runs `alf_process()` (aggregate mode), serializes `@grants` and `@exclude` to JSON
- `cookbook_auth_check()` — hierarchical prefix matching with `crwd` permissions + exclude check
- Handles `PASTA_LABEL` team references (bare identifiers) and `PASTA_STRING` (quoted)
- Handles alforno collect mode output: when `@grants` values are arrays (from future `merge: "collect"`), ORs all permission characters together

**New endpoints:**
- `GET /admin/policies` — list all policies (subject, kind, updated_at)
- `GET /admin/policies/{subject}` — return policy pastlet as `application/x-pasta`
- `GET /admin/policies/{subject}/effective` — resolve via alforno and return JSON grants
- `PUT /admin/policies/{subject}` — upload pastlet (validates pasta syntax, extracts kind)
- `DELETE /admin/policies/{subject}` — remove policy

**Important finding (resolved by Pasta #4):** Group coordinates with dots (e.g. `com.iridiumfx`) originally required **quoted keys** in pasta. As of Pasta #4 (dotted labels), bare dotted keys are supported and all pastlets have been updated.

**Tests:** 258 → 302 (44 new assertions across 6 test functions)
- `test_policy_crud`: put, get, update, delete, non-existent returns NULL
- `test_policy_resolve`: user + team aggregation, grants and exclude sections present
- `test_auth_check_prefix`: exact match, hierarchical, read-only enforcement, dot-boundary
- `test_auth_check_exclude`: excluded group denied, sub-group of excluded denied
- `test_auth_check_edge_cases`: NULL grants, NULL group, empty grants
- `test_alforno_integration`: alf_create → alf_add_input × 2 → alf_process, last-write-wins verified

### 2026-03-14 — Auth v2 Phase 2 complete + submodule updates + build overhaul

**Submodule updates:**
- **Pasta #4** (dotted labels) — bare keys now support dots (`com.iridiumfx:` instead of `"com.iridiumfx":`). All policy pastlets updated to unquoted dotted keys.
- **Basta #2** (dot sync) — vendored, not yet integrated into build.
- **Alforno #4** (7 new features) — `alf_process_to_string()`, `merge: "deep"`, conditional `when` (tag-based section gating), validation pass, `@include` directive, `ALF_SCATTER`, `ALF_GATHER` with precedence. Critically: `merge: "collect"` now available.

**Build system overhaul:**
- Switched all libraries from shared to static linking (eliminates DLL boundary issues on Windows)
- Added `PASTA_STATIC`, `ALF_STATIC` as PUBLIC compile definitions
- Downstream targets simplified — just link `cookbook`, transitive deps propagate
- Increased test exe stack to 8MB on Windows (`-Wl,--stack,8388608`)

**Auth v2 Phase 2 — JWT v2 + alforno resolution:**
- `cookbook_jwt_create_v2()` — builds JWT payload with `"v":2`, embeds resolved grants/exclude
- `json_extract_object()` — extracts `{...}` sub-objects from JWT payload by key
- `cookbook_jwt_verify()` — detects v2 tokens, extracts grants/exclude into claims struct
- `cookbook_jwt_claims_free()` — frees malloc'd grants_json/exclude_json
- `POST /auth/token` calls `cookbook_policy_resolve()` → JWT v2 (falls back to v1 if no policy)
- v1 backward compat verified

**merge:"collect" wired into policy resolver:**
- `cookbook_policy_resolve()` switched from `ALF_AGGREGATE` to `ALF_CONFLATE`
- Dynamic recipe built at runtime by scanning all input pastlets for grant/exclude keys
- `@grants` section uses `merge: "collect"` — same-key collisions produce arrays
- `serialize_grants_json()` ORs collected permission arrays into single strings
- `@exclude` section uses `merge: "replace"` (last-write-wins for boolean deny flags)

**Bug fix:** Double-free in `test_policy_resolve()` — assertions added after `free(json)` used freed pointer, then another `free(json)` at end caused STATUS_HEAP_CORRUPTION.

**Bug fix:** `cookbook_auth_check()` exclude path updated to handle bare `{...}` maps from JWT v2 extracted claims (not just wrapped `{"exclude":{...}}` format).

**Tests:** 302 → 342 (40 new assertions)
- `test_policy_resolve_collect()`: user `"r"` + team `"cw"` → OR'd `"rcw"`, all three ops allowed, delete denied
- `test_jwt_v2_roundtrip()`: create v2, verify, extract grants/exclude, auth_check enforcement
- `test_jwt_v2_policy_integration()`: end-to-end store → resolve → JWT v2 → verify → enforce
- `test_jwt_v1_v2_compat()`: v1 tokens decode correctly with version=1

### 2026-03-10 — Auth v2 design + alforno feature requests

**Auth v2 proposal drafted** (`specs/cookbook-auth-v2-proposal.md`):
- Pasta-native access policies: user/team profiles and grants as pastlets
- Hierarchical group prefix matching with `crwd` permission characters
- Deny-overrides-allow via `@exclude` sections
- Individual contributor = team of one (uniform model)
- JWT v2 embeds resolved grants (computed once at token issue, not per-request)
- Grid propagation via scoped `X-Cookbook-Grid-Grants` header (Option C)
- 4-phase migration path, backward compatible at every step

**Pasta ecosystem review:**
- **Pasta #2**: `PASTA_LABEL` — bare identifiers in value position, designed for alforno link resolution. Our submodule is one commit behind.
- **Basta**: Pasta + binary blobs (`BASTA_BLOB` via `0x00` sentinel + 8-byte length + payload). Useful for credential/key storage.
- **Alforno**: 3-pass config merging (parameterize → merge → link). Two modes: aggregate (open union) and conflate (recipe contract).

**Feature requests accepted by alforno team:**
1. `merge: "collect"` — per-section merge directive in conflate recipes. Same-key collisions produce arrays of all values instead of last-write-wins. Solves permission OR problem (cookbook post-processes collected arrays).
2. Runtime pasta/basta support — accept both pastlet and bastlet inputs in same `alf_process()` call. Blob values are opaque to all three passes. Allows text policies alongside binary credential storage.

### 2026-03-10 — G5 grid-aware mirror manifest

**G5 — Grid-aware mirror manifest:**
- `GET /mirror/manifest?grid=true` — when grid enabled, fans out to all peers via `/grid/manifest`
- Extracts `"artifacts":[...]` array from each peer response and merges into local result
- Same pattern as resolve fan-out: load peers, iterate, `cookbook_grid_get()`, extract JSON array
- Without `?grid=true`, behavior is unchanged (local-only)
- `now cache:mirror` can now get a complete view of all artifacts across the grid

**Test count unchanged** (258) — grid fan-out tested via loop detection and peer loading unit tests; full manifest aggregation requires integration test with live peers.

### 2026-03-10 — Now #0003 landed (now team)

now team shipped all three priorities in a single commit:
- **P1**: `now procure` wired to pico-http v0.3.0 — auth, downloads, SHA-256 verify, .sig
- **P2**: `now publish` wired — shared auth via `now_auth`, `application/x-pasta` on descriptor PUT
- **P3**: `now yank <g:a:v> --repo URL [--reason "..."]` — POST with optional JSON reason body
- New `now_auth` module: credential loading from `~/.now/credentials.pasta`, JWT exchange via Basic auth

Full client-server integration path is now unblocked. Remaining: `dep:updates` and `cache:mirror` (nice-to-have).

**New actionable items identified (no external dependencies):**

| ID | Item | Rationale | Status |
|----|------|-----------|--------|
| F1 | Yank reason field | DB has `yanked INTEGER` only. now spec error NOW-E0205 says "Yanked reason if available". Need `yank_reason TEXT` column, accept reason in POST body, return in response. | Planned |
| F2 | Resolve yank visibility | `?include_yanked=true` on `/resolve/` so `dep:updates` and `dep:check` can show users which versions are yanked (with reason). Currently yanked versions are silently excluded. | Planned |
| F3 | Credential verification | `/auth/token` should verify `Authorization: Basic` against stored credentials. now spec says `Basic base64({username}:{token})` where token is from `~/.now/credentials.pasta`. Need a `credentials` table. | Planned |

---

## Core Interactions (now → cookbook via pico-http)

| # | Scenario | now command | HTTP | cookbook endpoint | pico-http API | Status |
|---|----------|-------------|------|------------------|---------------|--------|
| 1 | Resolve dependency | `now procure` | `GET /resolve/{g}/{a}/{range}` | Done | `pico_http_get()` | **Done** (Now #0003) |
| 2 | Resolve w/ Pasta conneg | `now procure` | `GET /resolve/...` + `Accept: application/x-pasta` | Done | `pico_http_get()` + custom headers | **Done** (Now #0003) |
| 3 | Download archive | `now procure` | `GET /artifact/{g}/{a}/{v}/{file}` | Done | `pico_http_get_stream()` | **Done** (Now #0003) |
| 4 | Download descriptor | `now procure` | `GET /artifact/.../now.pasta` | Done | `pico_http_get()` | **Done** (Now #0003) |
| 5 | Download .sha256 | `now procure` | `GET /artifact/.../{file}.sha256` | Done | `pico_http_get()` | **Done** (Now #0003) |
| 6 | Download .sig | `now procure` | `GET /artifact/.../{file}.sig` | Done | `pico_http_get()` | **Done** (Now #0003) |
| 7 | Authenticate | `now procure/publish` | `POST /auth/token` | Done (Basic auth + Argon2id) | `pico_http_post()` | **Done** (Now #0003) |
| 8 | Publish archive | `now publish` | `PUT /artifact/{g}/{a}/{v}/{file}` | Done | `pico_http_put()` | **Done** (Now #0003) |
| 9 | Publish .sha256 | `now publish` | `PUT /artifact/.../{file}.sha256` | Done | `pico_http_put()` | **Done** (Now #0003) |
| 10 | Publish .sig | `now publish` | `PUT /artifact/.../{file}.sig` | Done | `pico_http_put()` | **Done** (Now #0003) |
| 11 | Publish descriptor | `now publish` | `PUT /artifact/.../now.pasta` | Done (ASCII enforced) | `pico_http_put()` | **Done** (Now #0003) |
| 12 | Streaming large download | `now procure` | `GET /artifact/...` (large file) | Done | `pico_http_get_stream()` | **Done** (Now #0003) |
| 13 | Streaming large upload | `now publish` | `PUT /artifact/...` (large file) | Done (max_upload_mb) | `pico_http_put()` (buffered) | Works; streaming upload deferred to pico-http v0.4.0 |

---

## Auth and Trust Interactions

| # | Scenario | now command | HTTP | cookbook endpoint | Status |
|---|----------|-------------|------|------------------|--------|
| 14 | Register publisher key | manual/setup | `POST /keys` | Done | Cookbook done |
| 15 | Revoke publisher key | manual/setup | `POST /keys/{id}/revoke` | Done | Cookbook done |
| 16 | Fetch registry pubkey | `now procure` (verify countersig) | `GET /.well-known/now-registry-key` | Done | Cookbook done |
| 17 | Sig verify on publish | `now publish` + `sign: true` | (server-side on PUT) | Done | Cookbook done |
| 18 | Countersig on publish | (automatic) | (server-side on PUT) | Done | Cookbook done |

---

## Ops and Mirror Interactions

| # | Scenario | now command | HTTP | cookbook endpoint | Status |
|---|----------|-------------|------|------------------|--------|
| 19 | Mirror manifest | `now cache:mirror` | `GET /mirror/manifest` | Done | Cookbook done |
| 19b | Grid-aware mirror | `now cache:mirror` | `GET /mirror/manifest?grid=true` | Done (G5) | Cookbook done |
| 20 | Health check | infra/monitoring | `GET /healthz` | Done | Done |
| 21 | Readiness check | infra/monitoring | `GET /readyz` | Done | Done |
| 22 | Metrics scrape | Prometheus | `GET /metrics` | Done | Done |
| 23 | Air-gap import | `cookbook-import` | `PUT /artifact/...` (batch) | Done | N/A (raw sockets) |

---

## Gaps

### Resolved (no action needed)

| # | Scenario | Resolution |
|---|----------|------------|
| 24 | Advisory database API | Not cookbook's responsibility. Advisories come from a separate feed (`advisories.now.build/db.pasta`), not the artifact registry. |
| 28 | Dep updates / version listing | Already works. `/resolve/` returns ALL matching versions. `dep:updates` can use `*` range. |

### Waiting on other teams

| # | Scenario | Who | Notes |
|---|----------|-----|-------|
| 25 | Remote build graph cache | now team | `GET/PUT /graphs/{hash}` — could be cookbook extension or separate service. |
| 26 | Compilation artifact cache (CAC) | now team | `GET/PUT /objects/{hash}` — separate service, same infra pattern. |
| 27 | Yank via now CLI | ~~now team~~ | **Done** (Now #0003) — `now yank <g:a:v> --repo URL [--reason "..."]` |
| 29 | Plugin procure | now team | Same protocol as deps. Cookbook stores plugin descriptors but doesn't validate plugin-specific fields. |
| 30 | Reproducibility metadata | now team + cookbook | `now publish --reproducible` attaches verification results. No storage/retrieval endpoint defined. |

### Actionable (cookbook, no blockers)

| ID | Item | Description | Status |
|----|------|-------------|--------|
| F1 | Yank reason | `yank_reason TEXT` column. `POST .../yank` accepts `{"reason":"..."}` body. `X-Now-Yank-Reason` header on GET. Response includes reason. | **Done** (2026-03-08) |
| F2 | Resolve yank visibility | `?include_yanked=true` on `/resolve/`. Returns yanked versions with `"yanked":true,"yank_reason":"..."` fields. | **Done** (2026-03-08) |
| F3 | Credential verification | `Authorization: Basic base64(sub:token)` in `/auth/token`. `credentials` table with Argon2id hash. Falls back to JSON body when no credential record exists. | **Done** (2026-03-08) |

---

## Cross-Project Dependency Matrix

### cookbook needs from...

| Source | Current | Future |
|--------|---------|--------|
| pasta | `pasta_parse()`, `pasta_write()`, `PASTA_SORTED`, `PASTA_LABEL`, dotted keys | IANA `application/pasta` registration |
| pico-http | *(nothing — cookbook is the server)* | *(nothing)* |
| now | Spec compliance | Yank CLI (#27) |

### now needs from...

| Source | Current | Future |
|--------|---------|--------|
| pasta | `pasta_parse()`, `pasta_write()` | *(stable)* |
| pico-http | `pico_http_get/put/post()`, `pico_http_get_stream()` | `pico_http_put_stream()` (v0.4.0, large uploads) |
| cookbook | All endpoints working | Yank reason (F1), resolve yank visibility (F2), credential verification (F3) |

### pico-http needs from...

| Source | Current | Future |
|--------|---------|--------|
| pasta | *(nothing)* | *(nothing)* |
| cookbook | *(test target only)* | *(nothing)* |
| now | Integration into `now` binary | TLS cert verification requirements (v0.4.0) |

---

## Priority

### Completed (cookbook-side, no blockers)

1. **F1** — Yank reason (schema + endpoint + header) ✓
2. **F2** — Resolve yank visibility (`?include_yanked=true`) ✓
3. **F3** — Credential verification in `/auth/token` ✓
4. **G1–G4** — Grid federation (peers table, raw HTTP client, loop detection, internal endpoints, fan-out on resolve + artifact) ✓
5. **G5** — Grid-aware mirror manifest (`?grid=true` aggregation across peers) ✓

342 unit tests. All passing.

### Completed (now team, Now #0003)

6. Wire pico-http v0.3.0 into `now procure` — auth, download, SHA-256 verify, .sig ✓
7. Wire pico-http v0.3.0 into `now publish` — auth, descriptor, archive, .sig ✓
8. `now yank <g:a:v> --repo URL [--reason "..."]` ✓

### Still waiting on now team (nice-to-have)

9. `now dep:updates` — client UI for `?include_yanked=true` and `*` range
10. `now cache:mirror` — client for `/mirror/manifest?grid=true`

### Auth v2 — Phases 1–2 done, Phase 3 next

11. **Auth v2 spec drafted** — `specs/cookbook-auth-v2-proposal.md` ✓
12. **Pasta #4 submodule** — dotted bare keys, `PASTA_LABEL` ✓
13. **Alforno #4 submodule** — `merge: "collect"` + 6 other features ✓
14. **Basta #2 submodule** — vendored, integration deferred ✓
15. **Build system overhaul** — all-static linking, PUBLIC compile defs ✓
16. **Auth v2 Phase 1** — `policies` table, CRUD endpoints, resolver, auth_check ✓ (302 tests)
17. **Auth v2 Phase 2** — JWT v2, merge:"collect" wiring, v1 compat ✓ (342 tests)
18. **Auth v2 Phase 3** — per-handler enforcement + mirror visibility filtering — **NEXT**
19. **Auth v2 Phase 4** — grid peer auth + scoped grant propagation

### Waiting on other teams (nice-to-have)

20. `now dep:updates` — client UI for `?include_yanked=true` and `*` range
21. `now cache:mirror` — client for `/mirror/manifest?grid=true`

### Can defer

22. IANA `application/pasta` registration — coordination across all three projects
23. Build graph / CAC (#25, #26) — separate infrastructure
24. Reproducibility metadata (#30) — needs design on both sides
