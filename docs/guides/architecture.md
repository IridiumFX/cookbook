# Cookbook — Architecture Guide

## Overview

Cookbook is a C11 artifact registry HTTP server designed for portability to the Nova OS. All protocol implementations are zero-dependency pure C11 over thin platform abstractions.

```
┌─────────────────────────────────────────────────────┐
│                  cookbook_server                      │
│  (civetweb HTTP server, handler routing, auth)       │
├──────────────┬──────────────┬───────────────────────┤
│ cookbook_auth │ cookbook_tls  │ cookbook_policy         │
│ JWT, Ed25519 │ TLS 1.3      │ alforno conflate       │
│ Argon2id     │ AES-GCM      │ grants/exclude         │
├──────────────┼──────────────┼───────────────────────┤
│ cookbook_db   │ cookbook_store│ cookbook_grid           │
│ SQLite | PG  │ FS | S3      │ peer federation        │
├──────────────┴──────────────┴───────────────────────┤
│              cookbook_socket (platform TCP)            │
│              Win32 / POSIX / Nova                     │
└─────────────────────────────────────────────────────┘
```

## Platform abstraction layers

Only these files contain platform-specific code. Everything else is pure C11.

### `cookbook_socket.c` — TCP + TLS

**Swap this file for Nova.** Provides:
- `cookbook_sock_connect(host, port, timeout)` — TCP connect
- `cookbook_sock_send / recv / recv_partial` — data transfer
- `cookbook_sock_close` — cleanup
- `cookbook_sock_connect_tls` — TCP + TLS 1.3 handshake
- `cookbook_tls_sock_send / recv / close` — TLS I/O

Current implementation: `winsock2.h` (Windows), BSD sockets (POSIX).

**Consumers** (zero raw socket calls — all go through this layer):
- `cookbook_grid.c` — HTTP client for grid federation (plain + HTTPS)
- `cookbook_store_s3.c` — S3 object store (plain + HTTPS)
- `cookbook_ldap.c` — LDAP simple bind (plain + LDAPS)
- `cookbook_tls.c` — TLS 1.3 handshake (uses socket for TCP, provides TLS)

### `cookbook_ed25519.c` — CSPRNG

Uses `BCryptGenRandom` (Windows) or `/dev/urandom` (POSIX) for key generation. Swap the `cookbook_csprng()` function for Nova's entropy source.

### Apennines `entropy.c` — CSPRNG

Same pattern: `BCryptGenRandom` / `/dev/urandom`. Swap for Nova.

### `civetweb.c` — HTTP server

Uses threads and sockets. On Nova, replace with Nova's HTTP server or adapt civetweb's socket layer.

## Module dependency graph

```
cookbook_server
  ├── cookbook_auth (JWT create/verify, credential hash)
  │     ├── cookbook_ed25519 (native Ed25519, SHA-512)
  │     └── libsodium (Argon2id, HMAC-SHA256 for S3)
  ├── cookbook_policy (alforno conflate, pasta pastlets)
  │     ├── alforno (config merging)
  │     └── libbasta (pasta/basta parsing)
  ├── cookbook_tls (TLS 1.3 client)
  │     └── apennines (AES-GCM, X25519, HKDF, RSA, X.509)
  ├── cookbook_ldap (LDAP simple bind, BER encoding)
  ├── cookbook_grid (grid federation HTTP client)
  ├── cookbook_db (metadata)
  │     ├── cookbook_db_sqlite (SQLite backend)
  │     └── cookbook_db_postgres (PostgreSQL backend, optional)
  ├── cookbook_store (object storage)
  │     ├── cookbook_store_fs (filesystem backend)
  │     └── cookbook_store_s3 (S3 backend)
  ├── cookbook_sha256 (SHA-256)
  ├── cookbook_semver (semver parsing + range evaluation)
  └── cookbook_socket (platform TCP + TLS wrapper)
```

## Vendored dependencies

| Library | Files | License | Nova portable? |
|---------|-------|---------|---------------|
| libbasta | 4 .c + 2 .h | MIT | Yes (pure C) |
| alforno | 5 .c + 2 .h | MIT | Yes (pure C) |
| SQLite | 1 .c + 1 .h | Public domain | Yes (pure C) |
| civetweb | 1 .c + 1 .h + .inl | MIT | Needs socket adapter |
| libsodium | ~100 .c | ISC | Mostly pure C, some asm |
| apennines | 14 .c + 14 .h | — | Yes (pure C, swap entropy) |

## Database abstraction

The `cookbook_db` vtable provides a backend-independent interface:

```c
typedef struct cookbook_db {
    cookbook_db_status (*exec)(struct cookbook_db *, const char *sql);
    cookbook_db_status (*exec_p)(struct cookbook_db *, const char *sql,
                                 const cookbook_db_param *params, int nparams);
    cookbook_db_status (*query)(struct cookbook_db *, const char *sql,
                                cookbook_db_row_cb cb, void *user);
    cookbook_db_status (*query_p)(struct cookbook_db *, const char *sql,
                                  const cookbook_db_param *params, int nparams,
                                  cookbook_db_row_cb cb, void *user);
    void (*close)(struct cookbook_db *);
} cookbook_db;
```

Implementations: `cookbook_db_sqlite.c`, `cookbook_db_postgres.c`.

For Nova: implement a new backend against Nova's storage API, or use SQLite (which is pure C and will compile for Nova).

## Object store abstraction

```c
typedef struct cookbook_store {
    cookbook_store_status (*put)(struct cookbook_store *, const char *key,
                                 const void *data, size_t len);
    cookbook_store_status (*get)(struct cookbook_store *, const char *key,
                                 void **data, size_t *len);
    cookbook_store_status (*exists)(struct cookbook_store *, const char *key);
    cookbook_store_status (*del)(struct cookbook_store *, const char *key);
    void (*close)(struct cookbook_store *);
} cookbook_store;
```

Implementations: `cookbook_store_fs.c` (filesystem), `cookbook_store_s3.c` (S3).

For Nova: implement against Nova's distributed storage.

## In-house protocol implementations

All protocol code is pure C11 — no system library dependencies:

| Protocol | File | Lines | Notes |
|----------|------|-------|-------|
| Ed25519 (RFC 8032) | `cookbook_ed25519.c` | ~2800 | Full keygen/sign/verify |
| SHA-256 | `cookbook_sha256.c` | ~300 | NIST test vectors |
| TLS 1.3 (RFC 8446) | `cookbook_tls.c` | ~500 | Client handshake + record layer |
| LDAP bind (RFC 4511) | `cookbook_ldap.c` | ~350 | BER encoding, simple bind |
| HTTP client | `cookbook_grid.c` | ~300 | Raw HTTP/1.1 for grid federation |
| AWS Sig V4 | `cookbook_store_s3.c` | ~250 | S3 request signing |

## Binary deployment (DLLs on Windows)

On Windows, cookbook links statically against all vendored libraries. However, GCC runtime DLLs are needed unless you compile with `-static-libgcc -static-libstdc++`:

| DLL | Source | Required? |
|-----|--------|-----------|
| `libgcc_s_seh-1.dll` | MinGW GCC runtime | Yes (unless fully static) |
| `libwinpthread-1.dll` | MinGW pthreads | Yes (unless fully static) |
| `libpq.dll` | PostgreSQL | Only if using PostgreSQL backend |

To eliminate DLL dependencies entirely, add to CMake:
```cmake
target_link_options(cookbook_server PRIVATE -static)
```

This produces a single self-contained executable. Test thoroughly — some MinGW versions have issues with fully static builds.

## Nova porting checklist

1. **Swap `cookbook_socket.c`** — replace winsock2/BSD with Nova networking
2. **Swap `entropy.c`** (apennines) — replace BCryptGenRandom/urandom with Nova CSPRNG
3. **Swap CSPRNG in `cookbook_ed25519.c`** — same as above
4. **Adapt or replace civetweb** — Nova HTTP server or port civetweb's socket layer
5. **Thread primitives** — replace `CreateThread`/`pthread_create` in `cookbook_server.c` (reconciliation thread) and `CRITICAL_SECTION`/`pthread_mutex` (audit lock, rate lock)
6. **File I/O** — `fopen`/`fwrite`/`fclose` in audit log; `cookbook_store_fs.c` uses standard C file I/O
7. **Everything else compiles as-is** — pure C11
