# Cookbook — Integration Guide

## Integrating with the `now` build system

### Registry configuration in now

Add to `~/.now/config.pasta`:
```
registry: "http://localhost:8080"
```

Or set via environment:
```sh
export NOW_REGISTRY=http://localhost:8080
```

### Authentication

```sh
# interactive login
now auth:login --registry http://localhost:8080

# check status
now auth:status

# logout
now auth:logout
```

The `now` client caches JWTs in `~/.now/tokens.pasta` with a 60-second safety margin before expiry.

### Supported auth methods

| Method | Command | Description |
|--------|---------|-------------|
| `token` | `now auth:login --method token` | Credential-based (default) |
| `ldap` | `now auth:login --method ldap` | LDAP directory password |
| `oidc` | `now auth:login --method oidc` | OIDC device code flow |

Registry discovery (`GET /.well-known/now-registry`) tells the client which methods are available.

### Publishing

```sh
now publish --registry http://localhost:8080
```

This uploads the `now.pasta` descriptor, archive, signature, and optional `.repro` attestation to cookbook.

### Resolving dependencies

```sh
now procure
```

Resolves dependency versions against the registry using semver ranges from the project's `now.pasta`.

---

## Remote object cache

The `now` build system can use cookbook as a remote compilation cache.

### Setup in now

Add to `~/.now/config.pasta`:
```
remote_cache: "http://localhost:8080"
```

### How it works

1. Before compiling a source file, `now` computes a cache key: `SHA-256(source_hash + compiler + flags)`
2. `GET /objects/{key}.o` — if 200, skip compilation and use the cached object
3. After successful compilation, `PUT /objects/{key}.o` — store for future use
4. Silent failure: if the cache is unreachable, `now` compiles locally without error

### Auth for cache writes

Cache reads (GET/HEAD) are open. Cache writes (PUT) require a JWT with `c` permission on the `_objects` group. Set up a policy:

```sh
curl -X PUT http://localhost:8080/admin/policies/now-ci \
  -H "Content-Type: application/x-pasta" \
  -d '@identity { sub: "now-ci" } @grants { _objects: "cr" }'
```

### TTL eviction

Set `COOKBOOK_OBJECT_CACHE_TTL_SEC=86400` (24 hours) to auto-prune stale cache entries. The background thread checks every 60 seconds.

---

## Grid federation

Multiple cookbook instances can form a federated mesh where any node can serve artifacts from any peer.

### Architecture

```
┌──────────┐     ┌──────────┐     ┌──────────┐
│ central  │────▶│  east-1  │────▶│  west-1  │
│ (origin) │◀────│  (peer)  │◀────│  (peer)  │
└──────────┘     └──────────┘     └──────────┘
```

### Enable grid on each node

```sh
export COOKBOOK_GRID_ENABLED=1
export COOKBOOK_GRID_MAX_HOPS=3
```

### Register peers

```sh
curl -X PUT http://localhost:8080/admin/peers \
  -H "Content-Type: application/json" \
  -d '{"peer_id":"east-1","name":"East Region","url":"https://east-1.internal:8080","mode":"proxy","priority":100}'
```

Modes:
- `redirect` — client gets a `307` redirect to the peer
- `proxy` — cookbook fetches from the peer and relays the response

### Peer authentication

For production grids, enable Ed25519 request signing:

```sh
export COOKBOOK_GRID_PEER_AUTH=1
```

Register each peer's public key in the `PUT /admin/peers` body. Requests include `X-Cookbook-Grid-Signature`, `X-Cookbook-Grid-Origin`, and `X-Cookbook-Timestamp` headers. 300-second replay window.

### Loop prevention

- `X-Cookbook-Via` — breadcrumb trail of registry IDs
- `X-Cookbook-Hop-Count` — incremented per hop, capped at `COOKBOOK_GRID_MAX_HOPS`
- Grid-internal endpoints (`/grid/resolve/`, `/grid/artifact/`, `/grid/manifest`) never fan out

---

## LDAP integration

Cookbook can authenticate users against an LDAP directory (Active Directory, OpenLDAP, etc.).

### Server configuration

```sh
export COOKBOOK_LDAP_URL=ldap://ldap.example.com:389
export COOKBOOK_LDAP_BASE_DN=ou=users,dc=example,dc=com
export COOKBOOK_LDAP_USER_ATTR=uid
```

For LDAPS (TLS):
```sh
export COOKBOOK_LDAP_URL=ldaps://ldap.example.com:636
```

### Client usage

```sh
curl -X POST http://localhost:8080/auth/token \
  -H "Content-Type: application/json" \
  -d '{"subject":"alice","token":"ldap-password","method":"ldap"}'
```

Cookbook constructs the DN as `{user_attr}={subject},{base_dn}` and performs a simple bind. On success, a JWT is issued.

### Groups

LDAP group membership is not yet mapped automatically. Create policies via `/admin/policies` to assign permissions to LDAP-authenticated users.

---

## Mirror manifest

For offline mirrors or periodic synchronization:

```sh
# list all published artifacts
curl http://localhost:8080/mirror/manifest

# with specific coordinates
curl "http://localhost:8080/mirror/manifest?coords=com.example"

# include grid peers
curl "http://localhost:8080/mirror/manifest?grid=true"
```

Use with `cookbook_import` for air-gapped deployments:

```sh
# on connected machine: download manifest and artifacts
curl http://registry.internal:8080/mirror/manifest > manifest.json
# ... download each artifact from manifest ...

# on air-gapped machine: import
cookbook_import -u http://localhost:8080 -t $TOKEN ./downloaded/
```

---

## Prometheus monitoring

Cookbook exposes metrics at `GET /metrics` in Prometheus exposition format.

### Key metrics

| Metric | Type | Description |
|--------|------|-------------|
| `cookbook_requests_total` | counter | Total HTTP requests |
| `cookbook_artifacts_published_total` | counter | Artifacts published |
| `cookbook_artifacts_resolved_total` | counter | Version resolutions |
| `cookbook_auth_tokens_issued_total` | counter | JWT tokens issued |
| `cookbook_auth_failures_total` | counter | Auth failures |
| `cookbook_bytes_uploaded_total` | counter | Total bytes uploaded |
| `cookbook_bytes_downloaded_total` | counter | Total bytes downloaded |

### Prometheus scrape config

```yaml
scrape_configs:
  - job_name: cookbook
    static_configs:
      - targets: ['localhost:8080']
    metrics_path: /metrics
    scrape_interval: 15s
```
