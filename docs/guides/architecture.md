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
| civetweb | 1 .c + 1 .h + .inl | MIT | Replaced by apennines HTTP server on Nova |
| libsodium | ~100 .c | ISC | Mostly pure C, some asm |
| apennines | 28 modules (56 .c + .h) | — | Yes (pure C, swap entropy + socket) |

Apennines modules: T1 (buf, entropy), T2 (cipher, ct, ec, ecdsa, hash, rsa, secret, x509, asn1_der, base, pem, bigint, compress, addr), T3 (pki, http, wal, tcp, threadpool, kv, tls, h2, dns), T4 (http_client, https_client, http_server)

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
| TLS 1.3 (RFC 8446) | `cookbook_tls.c` | ~770 | Client handshake + record layer, AES-GCM + ChaCha20, cert verify |
| LDAP bind+search (RFC 4511) | `cookbook_ldap.c` | ~600 | BER encoding, simple bind, SearchRequest, memberOf extraction |
| OIDC (RFC 6749) | `cookbook_oidc.c` | ~190 | Client credentials via apennines HTTPS client |
| HTTP client | `cookbook_grid.c` | ~300 | Plain TCP + apennines HTTPS client for TLS peers |
| AWS Sig V4 | `cookbook_store_s3.c` | ~250 | S3 request signing (raw sockets, not migrated) |
| Gzip (RFC 1952) | `cookbook_gzip.c` | ~80 | Deflate + gzip framing for HTTP responses |

## Binary deployment (DLLs on Windows)

On Windows, cookbook links statically against all vendored libraries. However, GCC runtime DLLs are needed unless you compile with `-static-libgcc -static-libstdc++`:

| DLL | Source | Required? |
|-----|--------|-----------|
| `libgcc_s_seh-1.dll` | MinGW GCC runtime | Yes (unless fully static) |
| `libwinpthread-1.dll` | MinGW pthreads | Yes (unless fully static) |
| `libpq.dll` | PostgreSQL | Only if using PostgreSQL backend |

To eliminate DLL dependencies entirely:
```sh
cmake --preset default -DCOOKBOOK_STATIC_LINK=ON
```

This produces a 4.2MB self-contained executable with zero MinGW DLLs.

## Dual HTTP server architecture

Cookbook supports two HTTP server backends, selectable at compile time:

**civetweb** (default, battle-tested):
- `#include "civetweb.h"` — standard civetweb API
- Worker thread model, prefix-based URI matching
- Used for all production deployments today

**apennines HTTP server** (Nova-forward, experimental):
- Enable: `cmake -DCOOKBOOK_USE_APENNINES_HTTP=ON`
- `cookbook_http_shim.h` redirects civetweb API to apennines `http_ctx`:
  ```c
  #define mg_connection       shim_connection
  #define mg_get_request_info shim_get_request_info
  #define mg_printf           shim_printf
  ```
- All 24 handlers work unchanged with both servers
- Route dispatch via static table with longest-prefix matching
- Zero code duplication — one handler codebase, two server backends

This produces a single self-contained executable. Test thoroughly — some MinGW versions have issues with fully static builds.

## Nova porting checklist

1. **Swap `cookbook_socket.c`** — replace winsock2/BSD with Nova networking
2. **Swap `entropy.c`** (apennines) — replace BCryptGenRandom/urandom with Nova CSPRNG
3. **Swap CSPRNG in `cookbook_ed25519.c`** — same as above
4. **Build with `-DCOOKBOOK_USE_APENNINES_HTTP=ON`** — uses apennines HTTP server instead of civetweb (Nova-native, all handlers work via shim)
5. **Use KV backend** — `COOKBOOK_DB_URL=cookbook.kv` instead of SQLite
6. **Thread primitives** — replace `CreateThread`/`pthread_create` in `cookbook_server.c` (reconciliation thread) and `CRITICAL_SECTION`/`pthread_mutex` (audit lock, rate lock, WAL lock)
7. **File I/O** — `fopen`/`fwrite`/`fclose` in audit log and WAL; `cookbook_store_fs.c` uses standard C file I/O
8. **Everything else compiles as-is** — pure C11

Steps 1-3 are single-file swaps. Step 4 is already done (flag-gated). Step 5 is already done (auto-detected by `.kv` extension). Steps 6-7 are standard C library calls that Nova's libc should provide.
