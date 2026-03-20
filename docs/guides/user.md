# Cookbook — User Guide

## Authentication

### Get a token

```sh
curl -X POST http://localhost:8080/auth/token \
  -H "Content-Type: application/json" \
  -d '{"subject":"alice","token":"my-secret"}'
```

Response:
```json
{"token":"eyJ...","expires_in":3600,"version":2}
```

Use the token on all subsequent requests:
```sh
export TOKEN="eyJ..."
```

### LDAP authentication

If the registry has LDAP configured:
```sh
curl -X POST http://localhost:8080/auth/token \
  -H "Content-Type: application/json" \
  -d '{"subject":"alice","token":"ldap-password","method":"ldap"}'
```

### Discover registry capabilities

```sh
curl http://localhost:8080/.well-known/now-registry
```

Returns supported auth methods, endpoints, and content types.

---

## Publishing artifacts

### Upload a now.pasta descriptor

```sh
curl -X PUT http://localhost:8080/artifact/com/example/mylib/1.0.0/now.pasta \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/x-pasta" \
  --data-binary @now.pasta
```

The descriptor is validated on upload:
- Must be valid pasta (US-ASCII only, no NUL bytes)
- Group, artifact, version must match the URL path
- Artifact names must be lowercase alphanumeric with hyphens
- Version must be valid SemVer 2.0

### Upload an archive

```sh
curl -X PUT http://localhost:8080/artifact/com/example/mylib/1.0.0/mylib-1.0.0-linux-amd64-gnu.tar.gz \
  -H "Authorization: Bearer $TOKEN" \
  --data-binary @mylib-1.0.0-linux-amd64-gnu.tar.gz
```

SHA-256 is computed on ingest. Duplicate uploads return `409 Conflict` (immutability).

### Upload a signature

```sh
curl -X PUT http://localhost:8080/artifact/com/example/mylib/1.0.0/mylib-1.0.0.tar.gz.sig \
  -H "Authorization: Bearer $TOKEN" \
  --data-binary @mylib-1.0.0.tar.gz.sig
```

### Upload a reproducibility attestation

```sh
curl -X PUT http://localhost:8080/artifact/com/example/mylib/1.0.0/mylib-1.0.0.repro \
  -H "Authorization: Bearer $TOKEN" \
  --data-binary @mylib-1.0.0.repro
```

The `.repro` file must be valid pasta with `format` and `artifact_hash` fields.

---

## Resolving versions

### Exact version

```sh
curl http://localhost:8080/resolve/com.example/mylib/1.0.0 \
  -H "Authorization: Bearer $TOKEN"
```

### Semver range

```sh
# caret: compatible with 1.x.y
curl http://localhost:8080/resolve/com.example/mylib/%5E1.0.0

# tilde: compatible with 1.2.x
curl http://localhost:8080/resolve/com.example/mylib/~1.2.0

# wildcard
curl http://localhost:8080/resolve/com.example/mylib/1.*

# Maven interval
curl http://localhost:8080/resolve/com.example/mylib/%5B1.0.0,2.0.0%29
```

Note: `^` and `[` must be URL-encoded (`%5E`, `%5B`).

### Include yanked versions

```sh
curl "http://localhost:8080/resolve/com.example/mylib/1.*?include_yanked=true"
```

### Content negotiation

```sh
# pasta format (default)
curl http://localhost:8080/resolve/com.example/mylib/1.0.0 \
  -H "Accept: application/x-pasta"

# JSON format
curl http://localhost:8080/resolve/com.example/mylib/1.0.0 \
  -H "Accept: application/json"

# pretty-printed pasta
curl "http://localhost:8080/resolve/com.example/mylib/1.0.0?pretty"
```

---

## Downloading artifacts

```sh
# download archive
curl -o mylib.tar.gz \
  http://localhost:8080/artifact/com/example/mylib/1.0.0/mylib-1.0.0-linux-amd64-gnu.tar.gz \
  -H "Authorization: Bearer $TOKEN"

# download descriptor
curl http://localhost:8080/artifact/com/example/mylib/1.0.0/now.pasta \
  -H "Authorization: Bearer $TOKEN"

# download SHA-256
curl http://localhost:8080/artifact/com/example/mylib/1.0.0/mylib-1.0.0.tar.gz.sha256 \
  -H "Authorization: Bearer $TOKEN"
```

Yanked artifacts are still downloadable but include `X-Now-Yanked: true` and `X-Now-Yank-Reason` headers.

---

## Yanking artifacts

```sh
curl -X POST http://localhost:8080/artifact/com/example/mylib/1.0.0/now.pasta/yank \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"reason":"Security vulnerability CVE-2026-1234"}'
```

Yanked artifacts are excluded from version resolution (unless `?include_yanked=true`).

---

## Object cache (for now build system)

### Store a compiled object

```sh
curl -X PUT http://localhost:8080/objects/abc123def456.o \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @output.o
```

### Retrieve a cached object

```sh
curl -o output.o http://localhost:8080/objects/abc123def456.o
```

### Check existence

```sh
curl -I http://localhost:8080/objects/abc123def456.o
# 200 = exists, 404 = not cached
```

---

## Bulk import (air-gapped environments)

Use `cookbook_import` for importing artifacts from a local directory:

```sh
cookbook_import -u http://localhost:8080 -t $TOKEN ./mirror-data/
```

Options:
- `-u, --url` — registry URL (default: `http://localhost:8080`)
- `-t, --token` — bearer JWT
- `-d, --dry-run` — list files without uploading
- `-v, --verbose` — print each file

The source directory must follow the mirror layout:
```
mirror-data/com/example/mylib/1.0.0/now.pasta
mirror-data/com/example/mylib/1.0.0/mylib-1.0.0.tar.gz
```

---

## Health checks

```sh
# liveness (always 200)
curl http://localhost:8080/healthz

# readiness (checks DB + store)
curl http://localhost:8080/readyz

# registry public key
curl http://localhost:8080/.well-known/now-registry-key
```

## Metrics

```sh
curl http://localhost:8080/metrics
```

Returns Prometheus exposition format with request, auth, publish, resolve, and transfer counters.
