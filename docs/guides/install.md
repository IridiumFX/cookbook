# Cookbook — Installation Guide

## Binary install (recommended for non-dev departments)

Cookbook ships as a single executable with no runtime dependencies beyond the OS.

### Windows

1. Create a directory for cookbook:
   ```
   mkdir C:\cookbook
   ```

2. Copy the following files from the build output or release archive:
   ```
   cookbook_server.exe      — the server
   cookbook_start.bat       — launcher script
   ```

3. Copy MinGW runtime DLLs (from your MinGW installation or the release archive). These are GCC runtime libraries under the [GCC Runtime Library Exception](https://www.gnu.org/licenses/gcc-exception-3.1.en.html) (permits redistribution with compiled programs):
   ```
   libgcc_s_seh-1.dll       — GCC runtime (GCC Runtime Library Exception)
   libwinpthread-1.dll      — POSIX threads (MIT/BSD, part of mingw-w64)
   ```

   **To eliminate DLL dependencies entirely**, build with `-static` link flag (see "Build from source"). This produces a single self-contained executable with no external DLLs.

4. (Optional) If using PostgreSQL, copy `libpq.dll` and its dependencies from your PostgreSQL install (PostgreSQL License, permissive):
   ```
   copy "C:\Program Files\PostgreSQL\16\bin\libpq.dll" C:\cookbook\
   ```

5. Double-click `cookbook_start.bat` or run from cmd. On first launch:
   - Creates `keys\` with an Ed25519 keypair (auto-generated)
   - Creates `data\objects\` for artifact storage
   - Creates `data\audit\` for audit logs
   - Creates `cookbook.db` (SQLite database)
   - Starts listening on `http://localhost:8080`

6. Verify:
   ```
   curl http://localhost:8080/readyz
   ```
   Expected: `{"status":"ready","db":"ok","store":"ok"}`

### Linux / macOS / FreeBSD

1. Copy `cookbook_server` to `/usr/local/bin/` or your preferred location.

2. Create data directories:
   ```sh
   mkdir -p /var/cookbook/keys /var/cookbook/data/objects /var/cookbook/data/audit
   ```

3. Run:
   ```sh
   export COOKBOOK_PORT=8080
   export COOKBOOK_KEY_DIR=/var/cookbook/keys
   export COOKBOOK_DB_URL=/var/cookbook/cookbook.db
   export COOKBOOK_STORAGE_DIR=/var/cookbook/data/objects
   export COOKBOOK_AUDIT_DIR=/var/cookbook/data/audit
   cookbook_server
   ```

4. (Optional) Create a systemd service — see the Configuration Guide for a sample unit file.

### Directory layout after first run

```
cookbook/
  cookbook_server.exe          — server binary
  cookbook_start.bat           — launcher (Windows)
  cookbook.db                  — SQLite metadata database
  keys/
    registry.pub              — Ed25519 public key (hex)
    registry.key              — Ed25519 private key (hex, keep secure)
  data/
    objects/                  — artifact file storage
    audit/
      audit-auth.pasta        — authentication events
      audit-access.pasta      — publish/resolve/yank/objects events
      audit-admin.pasta       — credential/group/policy changes
```

---

## Build from source

### Prerequisites

- C11 compiler (GCC 13+, Clang 16+, or MSVC 2022+)
- CMake 3.20+
- Ninja (recommended) or Make
- Git (for submodule checkout)

### Steps

```sh
git clone <repository-url> cookbook
cd cookbook
git submodule update --init --recursive

# configure
cmake --preset default     # debug build
# or: cmake --preset release

# build
cmake --build build

# test
./build/bin/cookbook_test
```

Output binaries are in `build/bin/`:
- `cookbook_server` — the server
- `cookbook_test` — test suite (619 tests)
- `cookbook_stress` — stress test driver
- `cookbook_import` — CLI import tool

### Alternative: build with `now`

If the `now` build tool is available:

```sh
now build          # output: target/bin/cookbook.exe
```

The `now.pasta` descriptor is included in the repository. This produces a 2.6MB executable in ~30 seconds. No CMake required. See the architecture guide for details.

### Build with PostgreSQL support

Install PostgreSQL development headers, then rebuild. CMake auto-detects libpq:

```
-- PostgreSQL found: 16.13
```

If not detected, install `libpq-dev` (Debian/Ubuntu), `postgresql-devel` (RHEL), or PostgreSQL from the official installer (Windows).

### Vendored dependencies

All dependencies are vendored — no package manager needed:

| Library | Version | Purpose |
|---------|---------|---------|
| libbasta | Basta #2 | Text/binary format (pasta superset) |
| alforno | Alforno #4 | Config merging (policy resolution) |
| SQLite | 3.49.1 | Default metadata backend |
| civetweb | 1.16 | HTTP server |
| libsodium | 1.0.21 | Argon2id, HMAC-SHA256 (S3 signing) |
| apennines | T1+T2 | TLS 1.3 crypto (AES-GCM, X25519, RSA, X.509) |
