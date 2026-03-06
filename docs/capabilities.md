# cookbook — Current Capabilities

**Version**: 0.1.0
**Last updated**: 2026-03-06
**Phases complete**: A (Correctness), B (Metadata completeness), C (Auth and signing), D (partial — metrics, mirror, import)

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

Returns `200` with the resolved version, or `404` if no match is found.

### Artifact download

```
GET /artifact/{group}/{artifact}/{version}/{filename}
```

Serves archive files, descriptors (`now.pasta`), SHA-256 checksums (`.sha256`), signatures (`.sig`), and countersignatures (`.countersig`).

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

Marks an artifact as yanked. Yanked artifacts are excluded from version resolution but remain downloadable (with the `X-Now-Yanked: true` header).

### Authentication

```
POST /auth/token
```

Exchanges credentials for a signed JWT. The token is signed with the registry's Ed25519 secret key using the EdDSA algorithm. Token lifetime is configurable via `COOKBOOK_JWT_TTL_SEC` (default: 3600s).

JWT claims include: `sub` (subject), `groups` (comma-separated group list), `iat` (issued-at), `exp` (expiration).

### Publisher key management

```
POST /keys
```

Registers an Ed25519 public key for a publisher. Keys are associated with the authenticated subject.

```
POST /keys/{id}/revoke
```

Revokes a previously registered publisher key.

### Mirror manifest

```
GET /mirror/manifest[?coords=group:artifact:version,...]
```

Returns a JSON manifest listing all published artifacts suitable for mirroring. Without the `coords` query parameter, returns all published non-yanked artifacts. With `coords`, returns only the specified coordinates. Each entry includes the group, artifact, version, and the base storage path for fetching files.

Response format:
```json
{
  "registry": "central",
  "artifacts": [
    {"group":"org.acme","artifact":"core","version":"1.0.0","base_path":"central/org/acme/core/1.0.0"}
  ]
}
```

### Prometheus metrics

```
GET /metrics
```

Returns Prometheus exposition format (text/plain 0.0.4) with counters:

- `cookbook_requests_total` — total HTTP requests
- `cookbook_requests_by_method{method="GET|PUT|POST"}` — requests by method
- `cookbook_responses_by_status{class="2xx|4xx|5xx"}` — responses by class
- `cookbook_artifacts_published_total` — artifacts published
- `cookbook_artifacts_yanked_total` — artifacts yanked
- `cookbook_artifacts_resolved_total` — version resolutions
- `cookbook_auth_tokens_issued_total` — JWT tokens issued
- `cookbook_auth_failures_total` — authentication failures
- `cookbook_bytes_uploaded_total` — total bytes uploaded
- `cookbook_bytes_downloaded_total` — total bytes downloaded

### Health and diagnostics

```
GET /healthz
```

Liveness probe. Returns `200 OK` unconditionally.

```
GET /readyz
```

Readiness probe. Checks database connectivity and object store health. Returns `200` if all backends are healthy, `503` otherwise.

```
GET /.well-known/now-registry-key
```

Returns the registry's Ed25519 public key in hex encoding. Used by clients to verify registry countersignatures.

---

## Data model

### Metadata backend (SQLite)

Full spec section 4.1 schema:

- **groups**: Group registration and ownership.
- **artifacts**: Artifact metadata including group, name, version, status (`pending`/`published`/`yanked`), `pending_since` timestamp for reconciliation.
- **artifact_semver**: Parsed semver components (major, minor, patch, pre-release, build metadata) for efficient range queries.
- **publisher_keys**: Ed25519 public keys registered per publisher subject.

All data operations use parameterized queries (`sqlite3_prepare_v2`) to prevent SQL injection.

### Object store (filesystem)

Spec section 4.2 layout. Objects stored at:

```
{COOKBOOK_STORAGE_DIR}/{group}/{artifact}/{version}/{filename}
```

Compatible with `now cache --mirror` output. Sidecar files (`.sha256`, `.sig`, `.countersig`, stripped descriptors) are stored alongside their parent artifacts.

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

- JWT-based authentication using EdDSA (Ed25519) signatures via libsodium.
- Group-level authorization: JWT `groups` claim must include the target artifact group.
- Per-subject rate limiting with configurable sliding window.

### Cryptographic integrity

- SHA-256 computed on ingest for every uploaded artifact.
- Publisher Ed25519 signature verification on `.sig` uploads.
- Registry Ed25519 countersignature on all published artifacts.
- Registry key pair auto-generated on first run (when `COOKBOOK_KEY_DIR` is set) and persisted as hex files.

---

## Configuration

All configuration is via environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `COOKBOOK_PORT` | `8080` | HTTP listen port |
| `COOKBOOK_REGISTRY_ID` | `central` | Registry identifier |
| `COOKBOOK_DB_URL` | `cookbook.db` | SQLite database path (or future `postgres://` URL) |
| `COOKBOOK_STORAGE_DIR` | `./data/objects` | Filesystem object store root |
| `COOKBOOK_MAX_ARTIFACT_MB` | `0` (unlimited) | Maximum upload size in MB |
| `COOKBOOK_PENDING_TIMEOUT_SEC` | `3600` | Stale pending artifact cleanup interval |
| `COOKBOOK_JWT_TTL_SEC` | `3600` | JWT token lifetime in seconds |
| `COOKBOOK_RATE_LIMIT_PER_MIN` | `0` (unlimited) | Per-subject request rate limit |
| `COOKBOOK_KEY_DIR` | *(none)* | Directory for registry Ed25519 key pair |

---

## Build

- **Language**: C11
- **Build system**: CMake 3.20+ with Ninja
- **Presets**: `default` (Debug), `release` (Release)
- **Platforms**: Windows (MinGW), Linux, macOS, FreeBSD
- **Output**: `libcookbook` shared library + `cookbook_server` executable

### Vendored dependencies

| Dependency | Version | License | Purpose |
|------------|---------|---------|---------|
| libpasta | git submodule | MIT | Pasta descriptor parsing |
| SQLite | 3.49.1 | Public domain | Metadata backend |
| civetweb | 1.16 | MIT | HTTP server |
| libsodium | 1.0.21 | ISC | Ed25519, JWT, HMAC-SHA256 |

---

## Test suite

90 tests covering:

- Semver parsing and range evaluation
- Database operations (raw and parameterized)
- Object store CRUD
- Artifact publish and resolution (HTTP integration tests)
- Immutability enforcement (409 on duplicate PUT)
- SHA-256 (NIST test vectors: empty, "abc", 448-bit message)
- Base64url roundtrip encoding
- JWT create/verify lifecycle
- Ed25519 sign/verify
- Mirror manifest data queries

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

## Not yet implemented (Phase D remainder and E)

- PostgreSQL metadata backend (requires libpq)
- S3 object store backend (requires libcurl)
- `application/x-pasta` content negotiation (blocked on now spec section 23)
