# Cookbook — Administration Guide

## First-time setup

After installation, the server starts with auth disabled and no credentials. To set up a production instance:

### 1. Start without auth (bootstrap)

```sh
# don't set COOKBOOK_KEY_DIR — auth is disabled
cookbook_server
```

### 2. Create the first admin credential

```sh
curl -X PUT http://localhost:8080/admin/credentials \
  -H "Content-Type: application/json" \
  -d '{"subject":"admin","token":"your-secret-token","groups":"*"}'
```

The `*` group grants access to all group prefixes.

### 3. Create an admin policy (for v2 tokens with full permissions)

```sh
curl -X PUT http://localhost:8080/admin/policies/admin \
  -H "Content-Type: application/x-pasta" \
  -d '@identity { sub: "admin", kind: "user" } @grants { *: "crwd" }'
```

### 4. Restart with auth enabled

Stop the server, set `COOKBOOK_KEY_DIR`, and restart. The keypair is auto-generated on first run.

### 5. Verify auth works

```sh
# get a token
curl -X POST http://localhost:8080/auth/token \
  -H "Content-Type: application/json" \
  -d '{"subject":"admin","token":"your-secret-token"}'

# use the token
curl http://localhost:8080/admin/groups \
  -H "Authorization: Bearer eyJ..."
```

---

## Credential management

### Create a credential

```sh
curl -X PUT http://localhost:8080/admin/credentials \
  -H "Content-Type: application/json" \
  -d '{"subject":"alice","token":"secret123","groups":"com.example,org.acme"}'
```

Tokens are hashed with Argon2id before storage — the plaintext is never stored.

### List credentials

```sh
curl http://localhost:8080/admin/credentials
```

Returns: `{"credentials":[{"subject":"alice","groups":"com.example,org.acme","created_at":"2026-03-19 17:12:26"}]}`

### Revoke a credential

```sh
curl -X POST http://localhost:8080/admin/credentials/alice/revoke
```

Sets `revoked_at` — the credential can no longer be used to obtain tokens.

### Delete a credential

```sh
curl -X DELETE http://localhost:8080/admin/credentials/alice
```

Hard delete from the database.

---

## Group management

Groups are auto-created when an artifact is first published to a group prefix. Admin endpoints allow explicit management.

### Create a group

```sh
curl -X PUT http://localhost:8080/admin/groups \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"group_id":"com.iridiumfx","description":"IridiumFX components"}'
```

### List groups

```sh
curl http://localhost:8080/admin/groups
```

### Update a group

```sh
curl -X PATCH http://localhost:8080/admin/groups/com/iridiumfx \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"owner":"bob","description":"Updated description"}'
```

Note: URL path uses `/` for `.` separators (`com/iridiumfx` maps to `com.iridiumfx`).

### Delete a group

```sh
curl -X DELETE http://localhost:8080/admin/groups/com/iridiumfx \
  -H "Authorization: Bearer $TOKEN"
```

Fails with 409 if artifacts still reference the group. Remove or yank all artifacts first.

---

## Policy management (Auth v2)

Policies define fine-grained permissions using pasta pastlets with alforno merge semantics.

### Create/update a policy

```sh
curl -X PUT http://localhost:8080/admin/policies/alice \
  -H "Content-Type: application/x-pasta" \
  -d '@identity { sub: "alice", kind: "user", teams: ["dev"] }
@grants { com.example: "crwd", org.acme: "r" }
@exclude { com.example.internal: true }'
```

Permission characters: `c` (create/publish), `r` (read/resolve), `w` (write/update), `d` (delete).

### View resolved permissions

```sh
curl http://localhost:8080/admin/policies/alice/effective
```

Returns the resolved grants after merging user + team policies via alforno conflate.

### Delete a policy

```sh
curl -X DELETE http://localhost:8080/admin/policies/alice
```

---

## Token management

### Issue a token

```sh
curl -X POST http://localhost:8080/auth/token \
  -H "Content-Type: application/json" \
  -d '{"subject":"alice","token":"secret123"}'
```

Returns v2 JWT if a policy exists for the subject, otherwise v1.

### Revoke a token

```sh
curl -X POST http://localhost:8080/auth/revoke \
  -H "Content-Type: application/json" \
  -d '{"token":"eyJ..."}'
```

Revocation is persistent — survives server restarts (stored in DB).

---

## Audit logs

When `COOKBOOK_AUDIT_DIR` is set, three log files are written:

| File | Events |
|------|--------|
| `audit-auth.pasta` | token-issue, token-revoke, jwt-invalid, denied, bad-credentials, rate-limited |
| `audit-access.pasta` | publish, yank, resolve, objects (stored, duplicate, validation-failed) |
| `audit-admin.pasta` | credential-create/revoke/delete, group-created/updated/deleted, policy-put/delete |

Each line is a valid pasta document. Parse with `basta_parse_cstr()` or any pasta reader.

### Monitoring for auth failures

```sh
# watch for denied/failed auth in real time
tail -f data/audit/audit-auth.pasta
```

---

## Grid federation

### Add a peer

```sh
curl -X PUT http://localhost:8080/admin/peers \
  -H "Content-Type: application/json" \
  -d '{"peer_id":"east-1","name":"East Region","url":"https://east-1.internal:8080","mode":"proxy","priority":100}'
```

Modes: `redirect` (307 to peer) or `proxy` (fetch and relay).

### Enable peer authentication

Set `COOKBOOK_GRID_PEER_AUTH=1`. Register each peer's Ed25519 public key:

```sh
curl -X PUT http://localhost:8080/admin/peers \
  -H "Content-Type: application/json" \
  -d '{"peer_id":"east-1","name":"East","url":"https://east-1:8080","public_key":"abcdef..."}'
```

---

## Backup and recovery

### SQLite

```sh
# hot backup (WAL mode safe)
sqlite3 cookbook.db ".backup backup.db"
```

### PostgreSQL

```sh
pg_dump -Fc cookbook > cookbook.dump
pg_restore -d cookbook cookbook.dump
```

### Object store (filesystem)

The `data/objects/` directory is a flat file store. Back up with rsync or similar:

```sh
rsync -a data/objects/ /backup/cookbook-objects/
```

### Keys

The `keys/` directory contains the registry's Ed25519 keypair. **Back up securely** — losing the private key means all issued JWTs become unverifiable and a new keypair must be generated (invalidating all existing tokens).
