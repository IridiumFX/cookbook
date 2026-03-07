# cookbook — M1 Roadmap

**Status as of**: Cookbook #0001 — Initial Check-in
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
| libpasta | git submodule | MIT | Pasta parsing (now.pasta descriptors) |
| SQLite | 3.49.1 | Public domain | Dev/CI metadata backend |
| civetweb | 1.16 | MIT | HTTP server |
| libsodium | 1.0.21 | ISC | Ed25519 signing, JWT, HMAC-SHA256 |

## Optional dependencies (system-provided)

| Dependency | License | Purpose | Status |
|------------|---------|---------|--------|
| libpq | PostgreSQL License | PostgreSQL metadata backend | Optional; stub when absent |

*Note: S3 support was implemented without libcurl — uses raw sockets + libsodium HMAC-SHA256 for AWS Signature V4.*

---

## Gaps — by spec section

### §3 HTTP API

| # | Gap | Spec ref | Difficulty | Blocked by |
|---|-----|----------|------------|------------|
| 1 | `POST /artifact/.../yank` | §3.4 | Easy | — |
| 2 | `GET /readyz` (DB + store health) | §10.2 | Easy | — |
| 3 | `GET /metrics` (Prometheus) | §10.2 | Medium | — |
| 4 | Snapshot policy (exclude from resolve unless `snapshot: true`) | §6.3 | Easy | — |
| 5 | `X-Now-Yanked: true` header on yanked artifact GET | §3.4 | Easy | — |
| 6 | Triple-specific metadata from archive filenames | §3.3 | Medium | — |
| 7 | Max upload size enforcement (`COOKBOOK_MAX_ARTIFACT_MB`) | §10.1 | Easy | — |
| 8 | `Accept: application/x-pasta` content negotiation | §8.3 | Medium | now spec §23 |

### §5 Authentication and Authorization

| # | Gap | Spec ref | Difficulty | Blocked by |
|---|-----|----------|------------|------------|
| 9 | `POST /auth/token` — JWT exchange | §5.1 | Medium | libsodium |
| 10 | Bearer JWT validation on PUT | §5.1 | Medium | libsodium |
| 11 | Group claim checking (JWT `groups` vs artifact group) | §5.2 | Easy | #10 |
| 12 | `POST /keys` — publisher key registration | §5.3 | Easy | — |
| 13 | `POST /keys/{id}/revoke` | §5.3 | Easy | — |

### §7 Signing and Integrity

| # | Gap | Spec ref | Difficulty | Blocked by |
|---|-----|----------|------------|------------|
| 14 | SHA-256 compute on ingest + cross-check | §7.1 | Easy | — |
| 15 | `.sig` verification against publisher Ed25519 key | §7.2 | Medium | libsodium |
| 16 | Registry countersign | §7.3 | Medium | libsodium |
| 17 | `GET /.well-known/now-registry-key` | §7.3 | Easy | — |
| 18 | `now.pasta.sha256` generation | §7.1 | Easy | — |

### §4 Data Model

| # | Gap | Spec ref | Difficulty | Blocked by |
|---|-----|----------|------------|------------|
| 19 | Two-phase write (pending → published) | §4.2 | Medium | — |
| 20 | Reconciliation job for stale pending rows | §4.2 | Medium | #19 |
| 21 | PostgreSQL backend | §4.1 | Medium | libpq |
| 22 | S3 object store backend | §4.2 | Medium | libcurl, libsodium (Sig V4) |

### §8 Descriptor Validation

| # | Gap | Spec ref | Difficulty | Blocked by |
|---|-----|----------|------------|------------|
| 23 | Installed descriptor stripping (remove build-only fields) | §8.2 | Medium | — |
| 24 | Field validation (lowercase artifact, valid semver, output.type enum) | §8.1 | Easy | — |

### §9 Offline and Mirror

| # | Gap | Spec ref | Difficulty | Blocked by |
|---|-----|----------|------------|------------|
| 25 | `GET /mirror/manifest?coords=...` | §9.1 | Medium | — |

### Air-gap Addendum

| # | Gap | Spec ref | Difficulty | Blocked by |
|---|-----|----------|------------|------------|
| 26 | `cookbook-import` CLI tool | §A.4 | Medium | — |

### Security

| # | Gap | Notes | Difficulty | Blocked by |
|---|-----|-------|------------|------------|
| 27 | SQL injection prevention | Switch to parameterized queries (sqlite3_prepare_v2) | Medium | — |
| 28 | Rate limiting | Per JWT-sub | Medium | #10 |
| 29 | Input validation | Group/artifact length limits, path traversal checks | Easy | — |

---

## Recommended implementation order

### Phase A — Correctness (no new dependencies) ✓

1. ~~**#27** SQL injection — parameterized queries~~
2. ~~**#29** Input validation — path traversal, length limits~~
3. ~~**#14** SHA-256 on ingest (vendored FIPS 180-4 implementation)~~
4. ~~**#18** now.pasta.sha256 generation~~
5. ~~**#24** Descriptor field validation~~
6. ~~**#1** Yank endpoint~~
7. ~~**#5** X-Now-Yanked header~~
8. ~~**#4** Snapshot policy~~
9. ~~**#7** Max upload size~~

### Phase B — Metadata completeness ✓

10. ~~**#6** Triple-specific metadata from archive filenames~~
11. ~~**#23** Installed descriptor stripping~~
12. ~~**#19** Two-phase write protocol~~
13. ~~**#20** Reconciliation job~~
14. ~~**#2** Readyz probe~~
15. ~~**#17** /.well-known/now-registry-key~~

### Phase C — Auth and signing (requires libsodium) ✓

16. ~~**Vendor libsodium** (1.0.21, ISC license)~~
17. ~~**#9** POST /auth/token~~
18. ~~**#10** JWT validation on PUT~~
19. ~~**#11** Group claim checking~~
20. ~~**#12** Publisher key registration~~
21. ~~**#13** Key revocation~~
22. ~~**#15** .sig verification on publish~~
23. ~~**#16** Registry countersign~~
24. ~~**#28** Rate limiting~~

### Phase D — Production backends ✓

25. ~~**#21** PostgreSQL backend (optional libpq; stub when unavailable)~~
26. ~~**#22** S3 object store backend (raw sockets + libsodium HMAC-SHA256; no libcurl)~~
27. ~~**#25** Mirror manifest endpoint~~
28. ~~**#3** Prometheus metrics~~
29. ~~**#26** cookbook-import CLI tool~~

### Phase E — Content negotiation ✓

30. ~~**#8** application/x-pasta support~~
    - Proposal: `docs/proposal-pasta-content-negotiation.md`
    - Response: `specs/response-pasta-content-negotiation.md`
    - All 7 decisions from spec response implemented:
      1. Media type: `application/x-pasta` (accepts `application/pasta` alias)
      2. Canonical form: compact + sorted keys (`PASTA_COMPACT | PASTA_SORTED`)
      3. JSON mapping: trivial 1:1 via `pasta_to_json()` (~90 lines)
      4. Charset: US-ASCII enforced on PUT (reject >0x7F and 0x00 with 400)
      5. Versioning: in-document `spec-version` field (no media type param)
      6. Scope: content negotiation on `/resolve/` and `/artifact/.../now.pasta`
      7. 406 safety: only when Accept present with no supported types
    - Pretty-print via `?pretty` query parameter
    - 219 unit tests + stress test driver (4 concurrent phases)
