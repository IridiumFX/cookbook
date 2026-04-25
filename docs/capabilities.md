# cookbook — Current Capabilities

**Version**: 1.0-rc5
**Last updated**: 2026-04-25
**Phases complete**: A–E (M1 spec), F1–F3 (feature gaps), G1–G5 (grid federation), Auth v2 (Phases 1–4), Auth v2.5, Native Ed25519, Basta migration, Group management, Persistent revocation, Object cache, LDAP, OIDC, TLS 1.3, Build graph cache, Reproducibility, Gzip, Connection pool, KV backend, Audit (3-file split)

---

## HTTP API

### Artifact resolution

```
GET /resolve/{group}/{artifact}/{range}
```

Resolves a semver range to the best matching published version. Supports:

- Exact versions: `1.2.3`
- Caret ranges: `^1.2.3` (compatible with 1.x.y)
- Tilde ranges: `~1.2.3` (compatible with 1.2.x)
- Wildcard ranges: `1.*`, `1.2.*`
- Maven-style interval ranges: `[1.0.0,2.0.0)`, `(1.0.0,1.5.0]`
- Full SemVer 2.0 pre-release precedence

Snapshot versions are excluded from resolution unless `?snapshot=true` is passed.

Supports content negotiation via `Accept` header:
- `Accept: application/x-pasta` — returns Pasta format
- `Accept: application/json` — returns JSON format (default)
- `Accept: */*` or missing — returns Pasta format
- `?pretty` query parameter for pretty-printed Pasta output

Returns `200` with the resolved version, or `404` if no match is found.

### Artifact download

```
GET /artifact/{group}/{artifact}/{version}/{filename}
```

Serves archive files, descriptors (`now.pasta`), SHA-256 checksums (`.sha256`), signatures (`.sig`), and countersignatures (`.countersig`).

Descriptor files (`now.pasta`) support content negotiation via `Accept` header:
- `Accept: application/x-pasta` or `application/pasta` — returns canonical Pasta (compact) with `Content-Type: application/x-pasta`
- `Accept: application/json` — returns JSON representation via recursive `PastaValue` tree walk
- `Accept: text/plain` — returns Pasta (backwards compatibility)
- `Accept: */*` or missing — returns Pasta (default)
- Unsupported types return `406 Not Acceptable`
- `?pretty` query parameter returns pretty-printed Pasta output

Yanked artifacts are still served but include an `X-Now-Yanked: true` response header.

### Artifact publish

```
PUT /artifact/{group}/{artifact}/{version}/{filename}
```

Stores an artifact file into the object store and registers metadata in the database. On publish:

- **Authentication**: Requires a valid `Bearer` JWT (EdDSA/Ed25519). The JWT's `groups` claim must include the artifact's group.
- **Immutability**: Duplicate PUTs to the same coordinate return `409 Conflict`.
- **Max upload size**: Enforced via `COOKBOOK_MAX_ARTIFACT_MB` (returns `413` if exceeded).
- **SHA-256 on ingest**: Computes SHA-256 over the uploaded body and stores a `.sha256` sidecar file.
- **ASCII enforcement**: `now.pasta` uploads are validated at the HTTP layer — bytes > `0x7F` and NUL (`0x00`) are rejected with `400 Bad Request` and a message indicating the byte offset.
- **Descriptor validation**: If the uploaded file is `now.pasta`, it is parsed and validated:
  - Group, artifact, and version fields must match the URL path.
  - Artifact names must be lowercase alphanumeric with hyphens.
  - Version must be valid SemVer 2.0.
  - `output.type` must be a known enum value.
- **Descriptor stripping**: An installed-view descriptor is generated with build-only fields removed.
- **Signature verification**: If a `.sig` file is uploaded, it is verified against the publisher's registered Ed25519 public key.
- **Registry countersign**: The registry signs the artifact content with its own Ed25519 key and stores a `.countersig` sidecar.
- **Triple extraction**: Archive filenames containing a target triple (e.g., `foo-1.0.0-linux-x86_64-gnu.tar.gz`) have OS, architecture, and ABI metadata extracted and stored.
- **Two-phase write**: Artifacts are initially stored as `pending` and promoted to `published` upon successful completion of all validation steps. Stale pending artifacts are cleaned up by a background reconciliation thread.
- **Rate limiting**: Per-subject sliding window rate limiting (configurable via `COOKBOOK_RATE_LIMIT_PER_MIN`).

### Yank

```
POST /artifact/{group}/{artifact}/{version}/{filename}/yank
```

Marks an artifact as yanked. Accepts optional `{"reason":"..."}` JSON body. Yanked artifacts are excluded from version resolution but remain downloadable with `X-Now-Yanked: true` and `X-Now-Yank-Reason: <reason>` headers.

### Authentication

```
POST /auth/token
Authorization: Basic base64(subject:token)
```

Exchanges credentials for a signed JWT. Three authentication methods supported:

**Token** (default): Credentials verified against Argon2id hashes in the `credentials` table. Accepts `Authorization: Basic` header or JSON body `{"subject":"x","token":"y"}`.

**LDAP**: `{"subject":"x","token":"y","method":"ldap"}` — binds against configured LDAP directory. After successful bind, searches for `memberOf` to extract group membership. CN automatically extracted from DN values. Falls back to local credentials if LDAP unreachable.

**OIDC client credentials**: `{"grant_type":"client_credentials","client_id":"x","client_secret":"y"}` — validates against configured OIDC issuer's token endpoint via HTTPS.

**OIDC device code** (interactive):
- `POST /auth/device` — generates device_code + user_code
- `POST /auth/device/token` — client polls until authorized
- `POST /auth/device/verify` — user authenticates with credentials
- When OIDC issuer is configured, proxies to the IdP. Falls back to standalone mode.

**JWT v1** (no policy): `sub`, `groups` (comma-separated), `iat`, `exp`.

**JWT v2** (with policy): `sub`, `grants` (resolved permission map), `exclude` (deny map), `teams`, `v:2`, `iat`, `exp`. Grants are computed once at token issue time via alforno conflate with merge:"collect".

Token lifetime configurable via `COOKBOOK_JWT_TTL_SEC` (default: 3600s). Signed with the registry's Ed25519 key (native implementation). Auth fallback chain: LDAP → local (or local → LDAP) depending on `method` field.

### Publisher key management

```
POST /keys
POST /keys/{id}/revoke
```

Register and revoke Ed25519 public keys for publishers.

### Access policies (Auth v2)

```
GET    /admin/policies                    — list all policies
GET    /admin/policies/{subject}          — get policy pastlet
GET    /admin/policies/{subject}/effective — resolved grants via alforno
PUT    /admin/policies/{subject}          — upload/replace pastlet
DELETE /admin/policies/{subject}          — remove policy
```

Policies are pasta pastlets with `@identity`, `@grants`, and `@exclude` sections. The resolver uses alforno conflate with merge:"collect" to aggregate user + team grants. Permission characters: `c` (create), `r` (read), `w` (write), `d` (delete). Hierarchical prefix matching — grant on `com.iridiumfx` implies access to `com.iridiumfx.pasta`. Excludes override grants (deny-wins).

All request handlers enforce `cookbook_auth_check()` when JWT v2 grants are present.

### Token revocation

```
POST /auth/revoke         — revoke a JWT by its jti claim
```

Accepts `{"token":"eyJ..."}` body. Verifies the JWT, extracts the `jti` claim, and adds it to both an in-memory bounded revocation list (4096 entries) and the `revocations` database table. Expired entries are auto-pruned on insert and on server startup. On restart, non-expired revocations are loaded from the DB, so revoked tokens stay revoked across server restarts.

### Credential management

```
PUT    /admin/credentials — create credential (Argon2id hash)
GET    /admin/credentials — list credentials (subject, groups, dates)
POST   /admin/credentials — revoke credential (sets revoked_at)
DELETE /admin/credentials — remove credential
```

Credentials table: `subject TEXT PK`, `token_hash TEXT`, `groups TEXT`, `created_at`, `revoked_at`.

### Group management

```
GET    /admin/groups              — list all groups
GET    /admin/groups/{group_id}   — get single group
PUT    /admin/groups              — create group (body: group_id, description)
PATCH  /admin/groups/{group_id}   — update owner or description
DELETE /admin/groups/{group_id}   — remove group (blocked if artifacts exist)
```

Groups are auto-created on first artifact publish (owner from JWT `sub`). Admin endpoints allow explicit creation, description, ownership transfer, and removal. URL paths use `/` for `.` separators (e.g., `/admin/groups/com/iridiumfx` → `com.iridiumfx`). Auth enforcement: `c` (create), `w` (update), `d` (delete).

### Mirror manifest

```
GET /mirror/manifest[?coords=...][&grid=true]
```

Returns JSON manifest of published artifacts. With `?grid=true`, fans out to all grid peers and merges results. Filtered by requesting user's visibility when JWT v2 grants are present.

### Grid federation

```
GET /grid/resolve/{g}/{a}/{range}   — local-only resolve (grid-internal)
GET /grid/artifact/{path}           — local-only artifact serve
HEAD /grid/artifact/{path}          — existence check
GET /grid/manifest                  — local-only mirror manifest
```

Grid-internal endpoints never fan out (no cascading). Client-facing `/resolve/` and `/artifact/` fan out to peers on miss when `COOKBOOK_GRID_ENABLED=1`.

**Peer management:**
```
GET    /admin/peers          — list peers
PUT    /admin/peers          — add/update peer (JSON body with optional public_key)
DELETE /admin/peers/{id}     — remove peer
```

**Loop prevention:** `X-Cookbook-Via` breadcrumb trail, `X-Cookbook-Hop-Count` (max configurable, default 3).

**Peer authentication** (`COOKBOOK_GRID_PEER_AUTH=1`): Outbound requests signed with Ed25519 (`X-Cookbook-Grid-Signature`, `X-Cookbook-Grid-Origin`, `X-Cookbook-Timestamp`). Inbound requests verified against registered peer public keys. 300-second replay window.

**Grant propagation:** `X-Cookbook-Grid-Grants` and `X-Cookbook-Grid-Exclude` headers carry scoped claims (derived from user's JWT, not the full token).

### Object cache (compilation artifacts)

```
GET  /objects/{cache_key}    — retrieve cached object
HEAD /objects/{cache_key}    — existence check
PUT  /objects/{cache_key}    — store object (auth: 'c' on _objects)
```

Content-addressable blob cache for compiled artifacts. Cache key is typically a SHA-256 hex string with extension (`.o`, `.obj`). Uses the same object store backend as artifacts. GET/HEAD are open; PUT requires JWT authorization on the `_objects` group prefix. Respects `COOKBOOK_MAX_ARTIFACT_MB` upload limit. Cached objects are tracked in the `object_cache` table with creation timestamps for TTL-based eviction.

### Build graph cache

```
GET  /graphs/{hash}    — retrieve build graph (gzip-compressed)
HEAD /graphs/{hash}    — existence check + X-Graph-Age header
PUT  /graphs/{hash}    — store graph (auth: 'c' on _graphs, max 64KB)
```

Stores project dependency graph snapshots as pasta documents. CI publishes a graph after a successful build; other machines fetch the graph, check if all object keys exist in `/objects/`, and skip compilation if they do. Turns per-file cache lookups into one graph fetch + bulk restore.

### Reproducibility attestation

```
PUT /artifact/{path}/{artifact}.repro    — upload attestation (validated: format + artifact_hash required)
GET /artifact/{path}?repro               — shortcut to fetch .repro sidecar
```

Stores reproducibility metadata alongside artifacts. Format: `now-repro-v1` pasta document with build tool, compiler, flags hash, timestamps, and optional third-party attestations. Upload validated for ASCII, valid pasta, and required fields.

### Gzip compression

Responses larger than 256 bytes are automatically gzip-compressed when the client sends `Accept-Encoding: gzip`. Active on `/resolve/` pasta responses and `/admin/policies/` pastlets. Uses Deflate (RFC 1951) + gzip framing (RFC 1952).

### Audit log

When `COOKBOOK_AUDIT_DIR` is set, cookbook writes three audit log files by category:

- `audit-auth.pasta` — token-issue, token-revoke, jwt-invalid, denied, bad-credentials, rate-limited, ldap-ok, ldap-bind-failed, oidc-failed
- `audit-access.pasta` — publish (ok/duplicate/validation-failed), yank, resolve, objects, graphs
- `audit-admin.pasta` — credential-create/revoke/delete, group-created/updated/deleted, policy-put/delete

Each line is a valid pasta document:

```
{ timestamp: "2026-03-19T22:00:00Z", event: "publish", subject: "alice", target: "central/com/example/mylib/1.0.0/mylib.tar", result: "ok" }
```

Events: `publish`, `yank`, `resolve`, `auth` (token-issue, token-revoke), `objects` (stored), `admin` (credential-create, group-created, group-deleted). Thread-safe (mutex-protected writes with flush). One entry per line — parseable with `basta_parse_cstr()`.

### Prometheus metrics

```
GET /metrics
```

Prometheus exposition format with counters: `cookbook_requests_total`, `cookbook_requests_by_method`, `cookbook_responses_by_status`, `cookbook_artifacts_published_total`, `cookbook_artifacts_yanked_total`, `cookbook_artifacts_resolved_total`, `cookbook_auth_tokens_issued_total`, `cookbook_auth_failures_total`, `cookbook_bytes_uploaded_total`, `cookbook_bytes_downloaded_total`.

### Registry discovery

```
GET /.well-known/now-registry         — registry capabilities (pasta format)
```

Returns a pasta document with registry metadata: `registry_id`, `auth` (enabled, methods, algorithm, public_key), `grid` (enabled, max_hops), `endpoints` (list of available endpoints), `content_types`. Used by now's enterprise auth for registry capability discovery.

### Health and diagnostics

```
GET /healthz                          — liveness probe (200 always)
GET /readyz                           — readiness probe (DB + store health)
GET /.well-known/now-registry-key     — registry Ed25519 public key (hex)
```

---

## Data model

### Metadata backends

Full spec section 4.1 schema:

- **groups**: Group registration and ownership.
- **artifacts**: Artifact metadata including group, name, version, status (`pending`/`published`/`yanked`), `pending_since` timestamp for reconciliation.
- **artifact_semver**: Parsed semver components (major, minor, patch, pre-release, build metadata) for efficient range queries.
- **publisher_keys**: Ed25519 public keys registered per publisher subject.

**SQLite** (default): All data operations use parameterized queries (`sqlite3_prepare_v2`) to prevent SQL injection. WAL mode enabled for concurrent reads.

**PostgreSQL** (optional): Full vtable implementation using libpq. Automatically translates `?N` placeholders to PostgreSQL `$N` syntax and `INSERT OR IGNORE` to `INSERT ... ON CONFLICT DO NOTHING`. Enabled when libpq is found at build time; graceful stub when unavailable. Connect via `COOKBOOK_DB_URL=postgres://user:pass@host:5432/dbname`.

**KV store** (Nova OS): Lightweight alternative using apennines kv (append-only log + FNV-1a hash index). Each table row stored as `table:pk → col=val\tcol=val` pairs. Supports INSERT, SELECT (by PK + prefix iterate), DELETE, CONSTRAINT detection, OR IGNORE. No JOINs or complex queries. Connect via `COOKBOOK_DB_URL=cookbook.kv` (detected by `.kv` extension).

### Object store backends

**Filesystem** (default): Spec section 4.2 layout. Objects stored at:

```
{COOKBOOK_STORAGE_DIR}/{group}/{artifact}/{version}/{filename}
```

Compatible with `now cache --mirror` output. Sidecar files (`.sha256`, `.sig`, `.countersig`, stripped descriptors) are stored alongside their parent artifacts.

**S3-compatible** (optional): Uses AWS Signature V4 over raw sockets with HMAC-SHA256 (libsodium) and SHA-256. No libcurl dependency. Works with AWS S3, MinIO, and other S3-compatible stores. Path-style addressing for custom endpoints; virtual-hosted for AWS. Enable via `COOKBOOK_STORAGE_TYPE=s3` with appropriate `COOKBOOK_S3_*` environment variables.

### Two-phase write protocol

1. Artifact is inserted with status `pending` and a `pending_since` timestamp.
2. After all validation passes (descriptor parsing, signature checks, SHA-256), status is promoted to `published`.
3. A background reconciliation thread runs periodically and cleans up stale pending rows older than `COOKBOOK_PENDING_TIMEOUT_SEC` (default: 3600s).

---

## Security

### Input validation

- Path traversal prevention: all path segments are validated to reject `..`, absolute paths, and null bytes.
- Group/artifact name length limits enforced.
- Group names: dot-separated segments, alphanumeric with hyphens.
- Artifact names: lowercase alphanumeric with hyphens.
- Version strings: validated against SemVer 2.0.

### Authentication and authorization

- JWT-based authentication using EdDSA (Ed25519) signatures (native implementation).
- **v1**: Group-level authorization via `groups` claim.
- **v2**: Fine-grained access via `grants`/`exclude` maps (alforno-resolved policies).
- **Three auth methods**: token (Argon2id), LDAP (simple bind + group search), OIDC (client credentials + device code).
- Auth fallback chains: LDAP ↔ local bidirectional fallback during migration.
- Per-subject rate limiting with configurable sliding window.
- Hierarchical prefix matching with deny-overrides-allow.

### Cryptographic integrity

- **Native Ed25519** (RFC 8032, ~2800 lines) — keygen, sign, verify. No libsodium for Ed25519.
- **TLS 1.3** (RFC 8446) — client handshake with AES-128-GCM + ChaCha20-Poly1305, X25519 ECDH, certificate verification (RSA-PSS, ECDSA P-256, Ed25519), PKI chain verification, HKDF key schedule. Used for LDAPS, HTTPS grid peers, S3 over HTTPS, OIDC.
- SHA-256 computed on ingest for every uploaded artifact.
- Publisher Ed25519 signature verification on `.sig` uploads.
- Registry Ed25519 countersignature on all published artifacts.
- Grid peer request signing with Ed25519 + replay prevention.
- Registry key pair auto-generated on first run (when `COOKBOOK_KEY_DIR` is set) and persisted as hex files.

---

## Configuration

All configuration is via environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `COOKBOOK_PORT` | `8080` | HTTP listen port |
| `COOKBOOK_REGISTRY_ID` | `central` | Registry identifier |
| `COOKBOOK_DB_URL` | `cookbook.db` | SQLite path, or `postgres://` connection URL |
| `COOKBOOK_STORAGE_TYPE` | `fs` | Object store backend: `fs` or `s3` |
| `COOKBOOK_STORAGE_DIR` | `./data/objects` | Filesystem object store root (when type=fs) |
| `COOKBOOK_S3_BUCKET` | *(none)* | S3 bucket name (when type=s3) |
| `COOKBOOK_S3_REGION` | `us-east-1` | S3 region |
| `COOKBOOK_S3_ACCESS_KEY` | *(none)* | S3 access key ID |
| `COOKBOOK_S3_SECRET_KEY` | *(none)* | S3 secret access key |
| `COOKBOOK_S3_ENDPOINT` | *(none)* | Custom S3 endpoint (e.g., `minio:9000`) |
| `COOKBOOK_MAX_ARTIFACT_MB` | `0` (unlimited) | Maximum upload size in MB |
| `COOKBOOK_PENDING_TIMEOUT_SEC` | `3600` | Stale pending artifact cleanup interval |
| `COOKBOOK_JWT_TTL_SEC` | `3600` | JWT token lifetime in seconds |
| `COOKBOOK_RATE_LIMIT_PER_MIN` | `0` (unlimited) | Per-subject request rate limit |
| `COOKBOOK_KEY_DIR` | *(none)* | Directory for registry Ed25519 key pair |
| `COOKBOOK_GRID_ENABLED` | `0` | Enable grid federation (1 = on) |
| `COOKBOOK_GRID_MAX_HOPS` | `3` | Maximum grid fan-out hop count |
| `COOKBOOK_GRID_PEER_AUTH` | `0` | Require Ed25519 peer signatures (1 = required) |
| `COOKBOOK_AUDIT_DIR` | *(none)* | Directory for pasta-format audit logs (3 files) |
| `COOKBOOK_OBJECT_CACHE_TTL_SEC` | `0` (no eviction) | TTL for cached objects; expired entries pruned every 60s |
| `COOKBOOK_LDAP_URL` | *(none)* | LDAP server (`ldap://` or `ldaps://`) |
| `COOKBOOK_LDAP_BASE_DN` | *(none)* | LDAP search base DN |
| `COOKBOOK_LDAP_USER_ATTR` | `uid` | User attribute for DN construction |
| `COOKBOOK_LDAP_GROUP_ATTR` | *(none)* | Group attribute (`memberOf`) for auto-mapping |
| `COOKBOOK_LDAP_GROUP_BASE` | *(base_dn)* | Base DN for group search |
| `COOKBOOK_OIDC_ISSUER` | *(none)* | OIDC issuer URL |
| `COOKBOOK_OIDC_CLIENT_ID` | *(none)* | OIDC client_id |
| `COOKBOOK_CA_BUNDLE` | *(none)* | PEM CA certificate bundle for TLS verification |

---

## Build

- **Language**: C11
- **Build system**: CMake 3.20+ with Ninja
- **Presets**: `default` (Debug), `release` (Release)
- **Platforms**: Windows (MinGW), Linux, macOS, FreeBSD
- **Output**: `libcookbook` static library + `cookbook_server` + `cookbook_import` executables
- **Linking**: All-static (no DLLs) — PUBLIC compile definitions propagate to all consumers

### Vendored dependencies

| Dependency | Version | License | Purpose |
|------------|---------|---------|---------|
| libbasta | Basta #2 (submodule) | MIT | Pasta superset — text + binary blobs. Sole format library |
| alforno | Alforno #4 (submodule) | MIT | Config merging — conflate, merge:"collect" |
| SQLite | 3.49.1 | Public domain | Default metadata backend |
| civetweb | 1.16 | MIT | HTTP server |
| libsodium | 1.0.21 | ISC | Argon2id, HMAC-SHA256 (S3 Sig V4) |
| apennines | T1-T4 (28 modules) | — | TLS 1.3 (AES-GCM, ChaCha20, X25519, HKDF, PKI, X.509, RSA, ECDSA), gzip (Deflate), HTTP client/server, KV store, connection pool, DNS |

### Optional system dependencies

| Dependency | License | Purpose |
|------------|---------|---------|
| libpq | PostgreSQL License | PostgreSQL metadata backend (auto-detected by CMake) |

---

## Test suite

619 unit tests covering:

- Semver parsing, range evaluation, edge cases, comparison details
- Database operations (parameterized queries, yank/status transitions, pending lifecycle)
- Object store CRUD, overwrite semantics, large value roundtrip
- Artifact publish and resolution (HTTP integration tests)
- Immutability enforcement (409 on duplicate PUT)
- SHA-256 (NIST test vectors), Base64url roundtrip (including all 256 byte values)
- JWT v1 create/verify, expired rejection, group boundary matching
- JWT v2 create/verify, grants/exclude extraction, v1/v2 compatibility
- Ed25519 native implementation (RFC 8032 test vectors 1–5, keygen, sign, verify)
- ASCII validation (boundary bytes, UTF-8 rejection, NUL, offset reporting)
- Pasta-to-JSON serialization, Pasta sorted key output
- Policy CRUD, resolve (user + team aggregation), effective permissions
- Auth check: prefix matching, exclude override, edge cases (NULL, empty)
- Alforno integration (conflate, merge:"collect" — permission OR)
- Policy resolve with collect (user "r" + team "cw" → "rcw")
- Credential hash/verify (Argon2id), credentials table, base64 standard decode
- Yank reason storage and retrieval, resolve yank visibility
- Grid loop detection, peers table, peer loading (priority, mode, enabled)
- Grid peer key CRUD, canonical string construction, sign/verify roundtrip
- Grid timestamp validation, peer auth enforcement
- Wildcard grants (specific overrides wildcard, admin full access, exclude interaction)
- Token revocation (add, check, duplicate, expiry prune, full auth flow roundtrip)
- JWT jti uniqueness (atomic counter, v1 and v2 tokens)
- Credential admin lifecycle (insert, verify, lookup, revoke, recreate, delete)
- Basta integration (string/map/blob create, write, parse)
- Mirror manifest, S3 store validation, PostgreSQL stub
- Stress test driver (4 concurrent phases: publish, resolve, get, conneg)

---

## CLI Tools

### cookbook-import

Standalone CLI tool for importing artifacts from a local directory into a cookbook registry. Designed for air-gapped environments (spec section A.4).

```
cookbook-import [options] <source-dir>

Options:
  -u, --url <url>       Registry URL (default: http://localhost:8080)
  -t, --token <token>   Bearer JWT for authentication
  -d, --dry-run         List files without uploading
  -v, --verbose         Print each file as it is uploaded
```

Source directory layout follows the mirror path convention:
```
<source-dir>/<group-path>/<artifact>/<version>/<filename>
```

Automatically skips `.sha256` and `.countersig` sidecar files (the registry generates these on ingest). Existing artifacts (409 responses) are silently skipped.

---

### Stress test driver

```
cookbook_stress [options]

Options:
  -c, --concurrency <n>   Number of concurrent workers (default: 8)
  -n, --requests <n>      Total requests per phase (default: 1000)
  -p, --port <port>       Server port (default: 19080)
  -q, --quiet             Only print summary
```

Starts an in-process cookbook server and runs 4 phases of concurrent HTTP requests:

1. **PUBLISH** — PUT unique `now.pasta` descriptors (concurrent writers)
2. **RESOLVE** — GET `/resolve/` with semver ranges (concurrent readers)
3. **GET** — fetch published descriptors by coordinate
4. **CONNEG** — GET descriptors with varying `Accept` headers (pasta, json, text/plain, */*)

Reports throughput (req/s), status code distribution, and latency percentiles (p50/p95/p99/max).

---

## Known limitations

- **IANA registration**: Using `application/x-pasta` as interim media type. Will switch to `application/pasta` after IANA registration.
- **No token refresh**: Policy changes take effect only on next token issuance. Revocation endpoint (`POST /auth/revoke`) exists for immediate invalidation.
- **Revocation list bounded**: In-memory list capped at 4096 entries (backed by DB for persistence across restarts). Expired entries auto-pruned.
