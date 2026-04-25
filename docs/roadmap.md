# cookbook — Roadmap

**Version**: 1.0-rc5
**Status as of**: 2026-03-22
**Spec reference**: cookbook-architecture-M1.docx (d3 final)

---

## Current capabilities

| Capability | Status | Notes |
|------------|--------|-------|
| `GET /resolve/{group}/{artifact}/{range}` | Done | ^, ~, *, exact, [a,b) Maven-style ranges |
| `GET /artifact/{group}/{artifact}/{version}/{filename}` | Done | Serves archives, descriptors, sha256, sig files |
| `PUT /artifact/{group}/{artifact}/{version}/{filename}` | Done | Stores files, parses now.pasta, registers metadata |
| `GET /healthz` | Done | Liveness probe |
| Immutability | Done | Duplicate PUT returns 409 |
| Semver parsing and comparison | Done | SemVer 2.0 with pre-release precedence |
| SQLite metadata | Done | Full §4.1 schema: groups, artifacts, artifact_semver, publisher_keys |
| Filesystem object store | Done | §4.2 layout, compatible with `now cache --mirror` output |
| Pasta descriptor validation | Done | Parses now.pasta on publish, extracts group/artifact/version |
| Server binary | Done | Configurable via env vars (COOKBOOK_PORT, COOKBOOK_DB_URL, etc.) |
| CI pipeline | Defined | Linux, macOS, FreeBSD, Windows (not yet tested in CI) |

## Vendored dependencies

| Dependency | Version | License | Purpose |
|------------|---------|---------|---------|
| libbasta | Basta #2 (git submodule) | MIT | Pasta superset — text + binary blobs. Replaces libpasta (compat header at `src/main/h/pasta.h`) |
| alforno | Alforno #4 (git submodule) | MIT | Config merging — merge:"collect", conflate, scatter/gather. Built with `ALF_USE_BASTA` |
| SQLite | 3.49.1 | Public domain | Dev/CI metadata backend |
| civetweb | 1.16 | MIT | HTTP server |
| apennines | 28 vendored modules | MIT | Argon2id (RFC 9106), HMAC-SHA256, TLS 1.3, PKI, compress, WAL, HTTP — full crypto + net stack |

*Note: cookbook is now libsodium-free. Ed25519 is a native RFC 8032 implementation. Argon2id credential hashing, HMAC-SHA256 (S3 Sig V4), and `ct_memzero` all come from apennines — one less cross-platform C dependency to port to Nova.*

## Optional dependencies (system-provided)

| Dependency | License | Purpose | Status |
|------------|---------|---------|--------|
| libpq | PostgreSQL License | PostgreSQL metadata backend | Optional; stub when absent |

*Note: S3 support uses raw sockets + apennines HMAC-SHA256 for AWS Signature V4 (no libcurl).*

---

## All M1 spec gaps — RESOLVED

All 29 original gaps from the M1 spec are implemented and tested.

---

## Implementation phases — all complete

### Phase A — Correctness ✓
### Phase B — Metadata completeness ✓
### Phase C — Auth and signing ✓
### Phase D — Production backends ✓
### Phase E — Content negotiation ✓

### Phase F — Feature gaps ✓

- **F1**: Yank reason (`yank_reason TEXT`, `POST .../yank` body, `X-Now-Yank-Reason` header)
- **F2**: Resolve yank visibility (`?include_yanked=true`, yanked/reason fields in response)
- **F3**: Credential verification (`Authorization: Basic`, Argon2id hash, `credentials` table)

### Phase G — Grid federation ✓

- **G1**: Peers table (`peer_id`, `url`, `mode`, `priority`, `public_key`, `enabled`)
- **G2**: Raw-socket HTTP client for peer communication
- **G3**: Loop detection (`X-Cookbook-Via` breadcrumb, `X-Cookbook-Hop-Count` limit)
- **G4**: Grid-internal endpoints (`/grid/resolve/`, `/grid/artifact/`, `/grid/manifest`)
- **G5**: Grid-aware mirror manifest (`?grid=true` aggregation across peers)
- Fan-out: resolve → iterate peers, artifact → redirect/proxy mode, manifest → merge

### Auth v2 — Policy-based access control ✓

- **Phase 1**: `policies` table, CRUD endpoints, alforno conflate resolver, `cookbook_auth_check()`
- **Phase 2**: JWT v2 (embedded grants/exclude), merge:"collect" wiring, v1 backward compat
- **Phase 3**: Per-handler enforcement on all endpoints, mirror visibility filtering, grid grant propagation
- **Phase 4**: Grid peer Ed25519 request signing, replay prevention, `COOKBOOK_GRID_PEER_AUTH` mode

### Native Ed25519 ✓

- Full RFC 8032 implementation (~2800 lines) — keygen, sign, verify
- Replaced all libsodium Ed25519 calls in JWT, publisher keys, registry signing, grid peer auth
- Verified against all 5 RFC 8032 test vectors
- libsodium fully removed 2026-04-18 — Argon2id / HMAC-SHA256 / ct_memzero now come from apennines

### Auth v2.5 — Wildcard grants, token revocation, credential management ✓

- **Wildcard grants**: `*: "crwd"` matches any group_id at lowest priority (specific grants override)
- **JWT ID (jti)**: unique per token via atomic counter + PRNG, embedded in all JWTs
- **Token revocation**: `POST /auth/revoke` — in-memory bounded list (4096 entries), auto-prunes expired, revocation checked at verification time
- **Credential management**: `PUT/GET/POST/DELETE /admin/credentials` — full CRUD with Argon2id hashing
- **Content-Type conformance**: `application/x-pasta; charset=US-ASCII` per now spec §20

### Basta migration ✓

- **libpasta removed from build** — libbasta is the sole text/binary format library
- Pasta compatibility header (`src/main/h/pasta.h`) maps all `pasta_*` API → `basta_*`
- Alforno compiled with `ALF_USE_BASTA=ON` (uses its own `alf_backend.h` compat layer)
- `FetchContent_SetPopulated` prevents alforno from re-fetching basta
- Root cause of previous basta segfault: 50+ internal symbol collisions between pasta/basta (`lexer_init`, `parse_array`, `write_value`, etc.) — eliminated by using basta exclusively

### Group management ✓

- `GET /admin/groups` — list all groups, get single group (path slash→dot conversion)
- `PUT /admin/groups` — create group with auth enforcement (`c` permission)
- `PATCH /admin/groups/{group_id}` — update owner or description (`w` permission)
- `DELETE /admin/groups/{group_id}` — remove group, blocked if artifacts still reference it (`d` permission)
- Publish path updated: `owner_sub` uses JWT `claims.sub` instead of hardcoded `"anonymous"`

### Persistent token revocation ✓

- `revocations` table: `jti TEXT PK`, `subject TEXT`, `revoked_at TEXT`, `expires_at INTEGER`
- `POST /auth/revoke` now writes to both in-memory list and database
- On startup: non-expired revocations loaded from DB into memory, expired entries pruned from DB
- Revoked tokens survive server restarts

### Registry discovery ✓

- `GET /.well-known/now-registry` — pasta-format capability document
- Returns: registry_id, auth (enabled/methods/algorithm/public_key), grid, endpoints, content_types
- Enables now's enterprise auth discovery (`now auth:login --registry URL`)

### Audit log ✓

- Pasta-format structured event log: `{ timestamp, event, subject, target, result }`
- Events: publish, yank, resolve, auth (issue/revoke), objects (store), admin (credential/group)
- Thread-safe (mutex + fflush), one entry per line
- Enabled via `COOKBOOK_AUDIT_LOG` env var (file path)

### Object cache ✓

- `GET /objects/{key}` — retrieve cached compiled object
- `HEAD /objects/{key}` — existence check
- `PUT /objects/{key}` — store object (auth: `c` on `_objects` prefix)
- `object_cache` DB table tracks cache_key, store_key, size_bytes, created_at
- TTL eviction via `COOKBOOK_OBJECT_CACHE_TTL_SEC` — background thread prunes expired entries every 60s
- Designed for now's remote compilation cache (`GET/PUT /objects/{sha256_hex}{.o|.obj}`)

### LDAP authentication ✓

- Zero-dep LDAP simple bind over TCP (BER encoding, RFC 4511)
- `ldap://` (plain) and `ldaps://` (TLS) support
- Config: `COOKBOOK_LDAP_URL`, `COOKBOOK_LDAP_BASE_DN`, `COOKBOOK_LDAP_USER_ATTR`
- Wired into `POST /auth/token` via `"method":"ldap"` field

### OIDC authentication ✓

- Client credentials flow: `POST /auth/token` with `grant_type=client_credentials`
- Device code flow: `POST /auth/device` → poll `/auth/device/token` → verify via `/auth/device/verify`
- OIDC token exchange over HTTPS to configured issuer
- Config: `COOKBOOK_OIDC_ISSUER`, `COOKBOOK_OIDC_CLIENT_ID`

### Socket abstraction layer ✓

- `cookbook_socket.h/.c` — platform-abstracted TCP + TLS
- Single file to swap for Nova OS porting
- All three consumers (grid, S3, LDAP) use shared API — zero raw socket calls

### TLS 1.3 client ✓

- Full handshake: ClientHello → ServerHello → key derivation → encrypted handshake → app data
- Cipher suite: TLS_AES_128_GCM_SHA256 (mandatory)
- Key exchange: X25519 ECDH
- Certificate verification: expiry check, CertificateVerify (RSA-PSS, ECDSA P-256, Ed25519)
- Server Finished: HMAC verify with constant-time comparison
- PKI chain verification via apennines `pki_store_verify` + `COOKBOOK_CA_BUNDLE`
- Wired into: LDAPS, HTTPS grid peers, HTTPS S3, OIDC

### Reproducibility attestation ✓

- `.repro` sidecar files validated on upload (ASCII, valid pasta, format + artifact_hash fields)
- Format: `now-repro-v1` per now team spec

### Gzip compression ✓

- `cookbook_gzip_compress()` — deflate + RFC 1952 gzip framing
- `send_response_gzip()` — auto-compresses responses >256 bytes for `Accept-Encoding: gzip`
- Uses apennines `deflate_compress`

### WAL-backed audit log ✓

- Three WAL files alongside flat pasta files: `audit-{auth,access,admin}.wal`
- CRC-32 integrity per entry, sequence numbers, crash recovery
- Entries written to both flat file (human-readable) and WAL (durable)
- Split by category: auth failures, access operations, admin changes

### Vendored apennines crypto ✓

28 modules (56 files) from the apennines project:
- **T1**: buf, entropy
- **T2**: cipher (AES-GCM, ChaCha20-Poly1305), ct, ec (Ed25519, X25519), ecdsa (P-256), hash (SHA-256/512, HMAC, HKDF), rsa (PKCS#1 v1.5, PSS), secret, x509, asn1_der, base, pem, bigint, compress (LZ4, Deflate), addr
- **T3**: pki (chain verify, CRL, OCSP), http (request/response parse), wal (thread-safe append-only log), tcp, threadpool, kv, tls, h2, dns
- **T4**: http_client, https_client, http_server

### Documentation ✓

Six guides under `docs/guides/`:
- install.md — binary + source install
- configuration.md — all env vars
- admin.md — bootstrap, credentials, groups, policies, audit, backup
- user.md — auth, publish, resolve, download, yank, object cache
- integration.md — now client, remote cache, grid, LDAP, Prometheus
- architecture.md — platform layers, module graph, Nova porting checklist

### Connection pool ✓

- `cookbook_connpool.h/.c` — bounded pool (128 entries, per-host limit, idle timeout)
- Wired into grid federation for plain TCP peer connections
- Thread-safe (mutex-protected), auto-prunes expired connections

### KV store DB backend ✓

- `cookbook_db_kv.c` — third DB backend alongside SQLite and PostgreSQL
- `COOKBOOK_DB_URL=cookbook.kv` selects KV backend (detected by `.kv` extension)
- INSERT, SELECT (by PK + prefix iterate), DELETE, OR IGNORE, CONSTRAINT detection
- Designed for Nova OS where SQLite is overkill

### Crash diagnostics ✓

- Unix signal handler: SIGSEGV, SIGABRT, SIGFPE, SIGBUS → `crash.log`
- Windows SEH handler: ACCESS_VIOLATION, STACK_OVERFLOW → `crash.log`
- Startup sentinel: `startup.log` written as first action in main()

### HTTP server migration ✓

- Dual-path architecture: civetweb (default) + apennines HTTP server (Nova)
- `cookbook_http_shim.h` — macro redirect layer, all 24 handlers work unchanged with both servers
- Route dispatch table with longest-prefix matching
- `COOKBOOK_USE_APENNINES_HTTP=ON` cmake flag to switch

### HTTPS client migration ✓

- OIDC: migrated to apennines `https_client` (net -63 lines)
- Grid federation: TLS peers migrated to `https_client`
- S3: stays raw (AWS Sig V4 signing coupled to HTTP request format — by design)

### Crash diagnostics ✓

- Unix signals: SIGSEGV, SIGABRT, SIGFPE, SIGBUS → `crash.log`
- Windows SEH: ACCESS_VIOLATION, STACK_OVERFLOW → `crash.log`
- Startup sentinel: `startup.log` confirms exe launched

### `now` build system (Phase 2) ✓

- `now.pasta` descriptor: 64 source files, vendored deps via `sources.include`
- `target/bin/cookbook.exe`: 2.2MB (down from 2.6MB after libsodium removal)
- First external project built with `now` — validated 3 bug fixes in the build tool
- Build time: ~30 seconds (vs ~60s CMake with configure)
- Phase 2 complete: cookbook can build with either CMake or `now`

### Test suite

617 unit tests + stress test driver (6 concurrent phases). All passing.

Server verified stable by now team: token auth, LDAP fallback, CLI flow, audit logging, object cache, build graph cache all confirmed working end-to-end.

---

## 1.0-rc1 Summary

All M1 spec gaps resolved. All enterprise features implemented. All known bugs fixed. Backlog empty. Builds with both CMake and `now`.

**62 commits this milestone.** Feature highlights:
- 4 auth methods (token, LDAP with group search, OIDC client credentials, OIDC device code)
- TLS 1.3 with AES-GCM + ChaCha20-Poly1305 + PKI chain verification
- 3 database backends (SQLite, PostgreSQL, KV store)
- Gzip compression, connection pooling, WAL-backed audit (thread-safe)
- Build graph cache, reproducibility attestation, object cache with TTL
- Dual HTTP server architecture (civetweb + apennines, shim layer with 24 routes)
- 28 vendored apennines modules (crypto, TLS, HTTP, KV, WAL, compress)
- 4.2MB fully static single-exe deployment (CMake), 2.6MB via `now`
- 6 documentation guides (install, config, admin, user, integration, architecture)
- Dual build systems: CMake (production) + `now` (Phase 2, Nova-forward)
- Nova OS porting: 5 of 8 checklist items already done at compile time
- Cross-team collaboration: cookbook + now + apennines via pasta mailbox protocol

---

## 1.0-rc2 Summary

Follow-on milestone. Dependency surface shrinks, Phase 3 (civetweb→apennines HTTP swap) goes from "scoped" to "functional". Everything is additive or reductive — no breaking changes, no new user-visible APIs.

**Key deltas vs rc1:**

- **libsodium fully removed.** Argon2id (RFC 9106), HMAC-SHA256, and `ct_memzero` now come from apennines. Format-compatible — existing credential hashes verify unchanged. `vendor/libsodium/` submodule gone. Exe shrinks: CMake 4.2MB → 3.9MB, `now` 2.6MB → 2.2MB. Forward alignment for Nova (one less cross-platform C dependency).
- **Phase 3 functional under flag.** `COOKBOOK_USE_APENNINES_HTTP=ON` now produces a working apennines-HTTP server serving the entire registry surface with zero handler code changes. Shim (`cookbook_http.c` + `cookbook_http_shim.h`, ~350 lines) bridges civetweb's `mg_connection`/`mg_read`/`mg_printf` to apennines' `http_ctx`/streaming API. Flag still OFF by default — civetweb remains production.
- **Inbound HTTPS wired.** New env vars `COOKBOOK_TLS_CERT_PEM` + `COOKBOOK_TLS_KEY_PEM`. PEM files decoded to DER via apennines pem, handed to `http_server_set_tls`. Verified end-to-end with self-signed cert — TLS 1.3 / AES_128_GCM_SHA256 / X25519 / rsa_pss_rsae_sha256 via `openssl s_client 3.5`.
- **Apennines upgrades (vendored):** 000113 (http.c body-length clamp fix), 000115 (http_server TLS termination), 000116 (router splat `*name`), 000117 (listen_async + streaming triplet real, TLS pointer fix), 000118 (WAL Win32 native — closes our March-21 crash report), 000119 (`threadpool_submit_detached` — fixes accept-path UAF), latest tls.c (PKCS#8 key unwrap).
- **New vendored T1 module:** `t1/sync/thread` (dependency of http_server's new listen_async).
- **Tests:** 617/617 pass in both flag-OFF and flag-ON builds. `now` build produces working exe.
- **Cross-team collaboration delta:** 4 apennines fixes shipped in ~24h (splat routing, listen_async, streaming triplet, threadpool UAF) unblocking Phase 3 from "parked indefinitely" to "works in one afternoon".

**What did NOT change:**
- Public HTTP API surface (same endpoints, same semantics)
- Database schema and credential format
- Default build (civetweb path)
- `now.pasta` descriptor structure

Phase 3 flag flip + civetweb removal is deferred to the formal Nova cutover milestone; cookbook 1.0 final will ship on civetweb.

---

## 1.0-rc3 Summary

Nova-alignment milestone. Civetweb is gone; the organization-wide push to apennines-first consolidation officially begins.

**Key deltas vs rc2:**

- **Civetweb removed.** `vendor/civetweb/` deleted, CMake target deleted, `COOKBOOK_USE_APENNINES_HTTP` flag deleted (no longer optional — apennines http_server is the only HTTP transport). `#else` civetweb branches in `cookbook_server.c` removed. The shim (`cookbook_http.c` + `cookbook_http_shim.h`) stays — it's a clean handler-facing adapter layer, transport-agnostic.
- **`vendor/pasta/` submodule removed** (dead weight — superseded by basta in the rc1 era, just hadn't been cleaned up).
- **`now` build: 2.2MB → 2.0MB** after civetweb drop.
- **Tests:** 617/617 pass on default CMake and `now` builds. Live smoke test green (HTTP + HTTPS endpoints all respond correctly).

**Remaining vendored third-party dependencies:**
- `apennines` — our strategic dep, grows
- `basta` + `alforno` (IridiumFX submodules) — apennines now has native `t4/pasta/` + `t4/pasta/alforno.h`; migration scheduled for rc4
- `sqlite` — last pure external; apennines has `t4/db/database.h` but data-format migration is its own milestone
- `libpq` (optional, system-provided) — stays (not in apennines' territory)

**Platform libs (unavoidable):** pthread (Unix), ws2_32 / bcrypt / advapi32 (Windows), libdl (Unix).

**What did NOT change:**
- Public HTTP API surface
- Database schema / credential format
- Environment variable names and semantics

---

## 1.0-rc4 Summary

Apennines-consolidation milestone. basta + alforno submodules are gone, replaced by apennines' native `t4/pasta` + `t4/pasta/alforno`. Platform-lib calls (`BCryptGenRandom`, `pthread_mutex_t`, `CreateThread`) swapped for apennines wrappers so cookbook source is now single-platform-shape and Nova-portable at the call-site level.

**Key deltas vs rc3:**

- **basta submodule removed.** `vendor/basta/` gone. Vendored apennines' `t4/pasta` as the successor (5 files: `pasta.h`, `pasta_parser.c`, `pasta_writer.c`, `pasta_value.c`, `pasta_internal.h`). New apennines-side dependencies also vendored: `t1/buffer/utf8`, `t2/string/fmt`, `t2/string/str`. Our `src/main/h/pasta.h` now redirects to `pasta_compat.h` (our new in-tree adapter) which wraps apennines' native `pasta_*` API to preserve the `PastaValue` / `pasta_parse_cstr` / etc. surface cookbook handlers use. Legacy `basta_*` names in a handful of call sites aliased to `pasta_*` via macros.

- **alforno submodule removed.** `vendor/alforno/` gone. Vendored apennines' `t4/pasta/alforno.{c,h}`. `src/main/h/alforno.h` redirects to `alforno_compat.h`. The `alf_*` / `AlfContext` / `ALF_CONFLATE` API our policy resolver uses is preserved verbatim through the compat wrapper.

- **`vendor/` is now just `apennines + sqlite`.** Sqlite is the last pure external third-party (optional `libpq` doesn't count — it's system-provided, not apennines territory).

- **Platform-lib ride-along:** cookbook source no longer calls `BCryptGenRandom`, `/dev/urandom`, `pthread_mutex_t` / `pthread_mutex_{init,lock,unlock,destroy}`, `CRITICAL_SECTION` / `EnterCriticalSection` / `LeaveCriticalSection`, `pthread_t` / `pthread_create` / `pthread_join`, `CreateThread` / `WaitForSingleObject` / `CloseHandle`, `Sleep` / `nanosleep`. All replaced by apennines wrappers:
    - `BCryptGenRandom` / `/dev/urandom` → `entropy_get_system` (t1/random/entropy) — 2 call sites (cookbook_auth salt, cookbook_ed25519 seed)
    - `pthread_mutex_t` + `CRITICAL_SECTION` → `mutex` (t1/sync/mutex/mutex) — 3 locks (rate_lock, audit_lock, connpool)
    - `pthread_t` + `HANDLE` + thread creation → `thread_handle` + `thread_create` + `thread_join` (t1/sync/thread) — 1 thread (reconcile)
    - `Sleep` / `nanosleep` → `thread_sleep` — server poll + reconcile loop

- **Reconcile thread function signature unified.** Was dual `DWORD WINAPI (LPVOID)` + `void *(void *)` behind `#ifdef _WIN32`; now single `unsigned long (void *)` matching apennines' `thread_fn`.

- **Tests:** 597/597 pass. (-20 vs rc3: the `test_basta_integration` block under `#ifdef COOKBOOK_HAS_BASTA` went away with the basta submodule. Those 20 asserts exercised basta's direct API; they're redundant now that pasta/alforno are apennines-native, and the remaining tests cover the same semantics through cookbook's normal code paths.)

- **`now` build: 2.0MB → 2.1MB** (basta+alforno sources removed but apennines pasta + utf8 + str + fmt + mutex + rng vendored added; roughly balanced, +90KB net).

- **Nova-readiness:** every line of cookbook source that used to have a `#ifdef _WIN32` / `#else` branch for threading or CSPRNG is now a single portable call. Nova cutover will swap the `#ifdef` bodies inside apennines, cookbook source stays untouched.

**What did NOT change:**
- Public HTTP API surface
- Database schema / credential format
- Environment variable names and semantics
- Handler code (PastaValue / pasta_* calls still work unchanged via compat)

---

## 1.0-rc5 Summary

Pre-golden cleanup milestone. The DB story rounds out (4th backend, fully production-ready) and the SQL surface becomes truly backend-portable.

**Key deltas vs rc4:**

- **Apennines DB backend wired** as the 4th `cookbook_db` vtable. Two storage modes selectable via URL prefix:
  - `apennines://<path>` — hash-KV storage. Optimised for INSERT-heavy workloads. Files: `<path>.kv` + `<path>.wal`.
  - `apennines-btree://<path>` — B+-tree storage (apennines 000218+). Files: `<path>.btree` + `<path>.wal`. Beats sqlite on indexed SELECT (~4×), ORDER BY LIMIT (~22×), and SELECT COUNT(*) (O(1) via cached row_count). Still-3× sqlite gap on pure aggregate-scan, down from 34×.
  - Default backend stays sqlite — opt-in for apennines per deployment.

- **SQL is now backend-portable.** The 8 inline `datetime('now')` sites that used to be sqlite-specific (1 column DEFAULT + 7 INSERT/UPDATE values) are now caller-side ISO-8601 binds via the new `cookbook_now_iso(char[20])` helper in `cookbook_db.h`. Same wire format sqlite produced (`"YYYY-MM-DD HH:MM:SS"`), so existing rows stored under sqlite read back unchanged. PostgreSQL backend's `translate_sql()` no longer needs to grow rewrites for `datetime` — neutral SQL flows to all four backends. PG was previously partially broken (would fail at migration on `policies.updated_at DEFAULT (datetime('now'))`); not a regression of recent rc-work but now actually fixable.

- **Apennines vendor refresh** — 15 commits across 000213–000224:
  - `INSERT OR REPLACE` parser support; `datetime`/`date`/`time`/`current_timestamp` evaluator
  - B+-tree storage backend with rwlock + cache_mutex split
  - Query planner: `UPDATE`/`DELETE` via index, `LIKE 'prefix%'` index range, compound `AND` indexed-branch selection, COUNT(*) on index entries, MIN/MAX TEXT correctness
  - SELECT DISTINCT (was parsed-but-ignored — silent gap before 000224)

- **Tidy:** `utc_now()` (audit + grid timestamp helper) collapsed from a `#ifdef _WIN32 / #else` two-implementation function to a single `strftime` call with the existing `gmtime_s`/`gmtime_r` shim. Same output format, less duplicate code.

- **Tests:** 597/597 pass on the default sqlite build. Live smoke green on all three apennines variants (sqlite default + apennines hash-KV + apennines-btree) — INSERT, GET, UPDATE (revoke), JWT issuance, /mirror/manifest (DISTINCT path).

- **Build hygiene:** zero warnings on cookbook source (single pre-existing `__declspec(thread)` warning from apennines on MinGW, harmless).

**What did NOT change:**
- Public HTTP API surface
- Stored DB row format (timestamps remain `"YYYY-MM-DD HH:MM:SS"` — historical sqlite values read back unchanged)
- Handler code
- Default backend (sqlite — flip per deploy if you want apennines)
