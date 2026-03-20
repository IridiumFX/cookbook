# Cookbook — Configuration Guide

All configuration is via environment variables. No config files needed (set in the `.bat`/`.sh` launcher or system environment).

## Core settings

| Variable | Default | Description |
|----------|---------|-------------|
| `COOKBOOK_PORT` | `8080` | HTTP listen port |
| `COOKBOOK_REGISTRY_ID` | `central` | Registry name (appears in storage paths and discovery) |
| `COOKBOOK_KEY_DIR` | *(none)* | Path to Ed25519 key directory. **Setting this enables authentication.** Auto-generates keys on first run. |

## Database

| Variable | Default | Description |
|----------|---------|-------------|
| `COOKBOOK_DB_URL` | `cookbook.db` | SQLite file path, or a PostgreSQL connection URL |

### SQLite (default)

No setup needed. The database file is created automatically.

```
COOKBOOK_DB_URL=cookbook.db
```

### PostgreSQL

```
COOKBOOK_DB_URL=postgres://user:password@localhost:5432/cookbook
```

Requires PostgreSQL 12+ and libpq at runtime. Schema migrations run automatically on startup.

## Object storage

| Variable | Default | Description |
|----------|---------|-------------|
| `COOKBOOK_STORAGE_TYPE` | `fs` | `fs` (filesystem) or `s3` (S3-compatible) |
| `COOKBOOK_STORAGE_DIR` | `./data/objects` | Filesystem storage root (when type=fs) |

### S3-compatible storage

| Variable | Default | Description |
|----------|---------|-------------|
| `COOKBOOK_S3_BUCKET` | *(required)* | Bucket name |
| `COOKBOOK_S3_REGION` | `us-east-1` | AWS region |
| `COOKBOOK_S3_ACCESS_KEY` | *(required)* | Access key ID |
| `COOKBOOK_S3_SECRET_KEY` | *(required)* | Secret access key |
| `COOKBOOK_S3_ENDPOINT` | *(none)* | Custom endpoint for MinIO etc. (`host:port`) |

S3 uses HTTPS by default (port 443). Custom endpoints on non-443 ports use plain HTTP. AWS Signature V4 signing is built-in (no libcurl).

```
COOKBOOK_STORAGE_TYPE=s3
COOKBOOK_S3_BUCKET=my-artifacts
COOKBOOK_S3_REGION=us-west-2
COOKBOOK_S3_ACCESS_KEY=AKIA...
COOKBOOK_S3_SECRET_KEY=wJal...
```

## Authentication

| Variable | Default | Description |
|----------|---------|-------------|
| `COOKBOOK_KEY_DIR` | *(none)* | Enables auth. Path to keypair directory. |
| `COOKBOOK_JWT_TTL_SEC` | `3600` | JWT token lifetime (seconds) |
| `COOKBOOK_RATE_LIMIT_PER_MIN` | `0` | Per-subject rate limit (0 = unlimited) |

### LDAP backend

| Variable | Default | Description |
|----------|---------|-------------|
| `COOKBOOK_LDAP_URL` | *(none)* | LDAP server URL (`ldap://host:389` or `ldaps://host:636`) |
| `COOKBOOK_LDAP_BASE_DN` | *(none)* | Search base (`ou=users,dc=example,dc=com`) |
| `COOKBOOK_LDAP_USER_ATTR` | `uid` | User attribute for DN construction |

When configured, clients can authenticate with `{"subject":"user","token":"pass","method":"ldap"}`. Cookbook performs a simple bind against the LDAP directory. LDAPS (TLS) is supported.

## Grid federation

| Variable | Default | Description |
|----------|---------|-------------|
| `COOKBOOK_GRID_ENABLED` | `0` | Enable grid federation (`1` = on) |
| `COOKBOOK_GRID_MAX_HOPS` | `3` | Maximum fan-out hop count |
| `COOKBOOK_GRID_PEER_AUTH` | `0` | Require Ed25519 peer signatures (`1` = required) |

When enabled, local misses on `/resolve/` and `/artifact/` fan out to registered peers. HTTPS peer URLs are supported.

## Object cache

| Variable | Default | Description |
|----------|---------|-------------|
| `COOKBOOK_OBJECT_CACHE_TTL_SEC` | `0` | TTL for cached compiled objects (0 = no eviction) |
| `COOKBOOK_MAX_ARTIFACT_MB` | `0` | Maximum upload size in MB (0 = unlimited) |

The object cache (`/objects/` endpoint) stores compiled artifacts for the `now` build system. Expired entries are pruned every 60 seconds by the background reconciliation thread.

## Audit logging

| Variable | Default | Description |
|----------|---------|-------------|
| `COOKBOOK_AUDIT_DIR` | *(none)* | Directory for pasta-format audit logs |

When set, cookbook writes three audit log files:
- `audit-auth.pasta` — token issue/revoke, auth failures (jwt-invalid, denied, bad-credentials, rate-limited)
- `audit-access.pasta` — publish, yank, resolve, object cache operations
- `audit-admin.pasta` — credential, group, and policy CRUD

Each line is a valid pasta document:
```
{ timestamp: "2026-03-19T22:00:00Z", event: "publish", subject: "alice", target: "central/com/example/mylib/1.0.0/mylib.tar", result: "ok" }
```

## Pending artifact cleanup

| Variable | Default | Description |
|----------|---------|-------------|
| `COOKBOOK_PENDING_TIMEOUT_SEC` | `3600` | Stale pending artifact cleanup interval (seconds) |

Artifacts are initially created as `pending` and promoted to `published` after validation passes. The background thread cleans up stale pending artifacts older than this timeout.

## Example: minimal production setup

```sh
export COOKBOOK_PORT=8080
export COOKBOOK_REGISTRY_ID=prod
export COOKBOOK_KEY_DIR=/var/cookbook/keys
export COOKBOOK_DB_URL=postgres://cookbook:secret@db.internal:5432/cookbook
export COOKBOOK_STORAGE_TYPE=s3
export COOKBOOK_S3_BUCKET=my-company-artifacts
export COOKBOOK_S3_REGION=us-east-1
export COOKBOOK_S3_ACCESS_KEY=AKIA...
export COOKBOOK_S3_SECRET_KEY=wJal...
export COOKBOOK_AUDIT_DIR=/var/cookbook/audit
export COOKBOOK_OBJECT_CACHE_TTL_SEC=86400
export COOKBOOK_RATE_LIMIT_PER_MIN=60
cookbook_server
```

## Example: systemd unit file (Linux)

```ini
[Unit]
Description=Cookbook Artifact Registry
After=network.target postgresql.service

[Service]
Type=simple
User=cookbook
WorkingDirectory=/var/cookbook
EnvironmentFile=/etc/cookbook/env
ExecStart=/usr/local/bin/cookbook_server
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```
