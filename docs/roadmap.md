# cookbook — M1 Roadmap

**Status as of**: 2026-03-19
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
| libsodium | 1.0.21 | ISC | Argon2id credential hashing, HMAC-SHA256 for S3 Sig V4 |

*Note: Ed25519 signing uses a native implementation (RFC 8032) — libsodium is no longer used for Ed25519.*

## Optional dependencies (system-provided)

| Dependency | License | Purpose | Status |
|------------|---------|---------|--------|
| libpq | PostgreSQL License | PostgreSQL metadata backend | Optional; stub when absent |

*Note: S3 support uses raw sockets + libsodium HMAC-SHA256 for AWS Signature V4 (no libcurl).*

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
- libsodium retained only for Argon2id and HMAC-SHA256

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

18 modules (36 files) from the apennines project:
- **T1**: buf, entropy
- **T2**: cipher (AES-GCM, ChaCha20-Poly1305), ct, ec (Ed25519, X25519), ecdsa (P-256), hash (SHA-256/512, HMAC, HKDF), rsa (PKCS#1 v1.5, PSS), secret, x509, asn1_der, base, pem, bigint, compress (LZ4, Deflate)
- **T3**: pki (chain verify, CRL, OCSP), http (request/response parse), wal (append-only log)

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

### Test suite

619 unit tests + stress test driver (6 concurrent phases). All passing.

Server verified stable by now team: token auth, LDAP fallback, CLI flow, audit logging all confirmed working end-to-end.
