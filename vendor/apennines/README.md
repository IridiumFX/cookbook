# Apennines Modules for Cookbook — TLS 1.3 Building Blocks

Drop folder for cookbook team. Grab what you need, delete the folder when done.

## Contents

24 source files (11 modules) + 9 guide files + this README.

### Module Inventory

| Module | Header | Source | What |
|--------|--------|--------|------|
| cipher | t2/crypto/cipher.h | t2/crypto/cipher.c | AES-128/256-GCM, ChaCha20-Poly1305, AES-128/256-CTR, AES-128/256-CBC |
| ct | t2/crypto/ct.h | t2/crypto/ct.c | Constant-time: memcmp, select, zero, copy_if, is_zero |
| rsa | t2/crypto/rsa.h | t2/crypto/rsa.c | RSA PKCS#1 v1.5 sign/verify, PSS sign/verify, key parse (DER) |
| hash | t2/crypto/hash.h | t2/crypto/hash.c | SHA-256, SHA-512, HMAC-SHA-256, HKDF-SHA-256 (extract + expand) |
| x509 | t2/crypto/x509.h | t2/crypto/x509.c | X.509 DER/PEM cert parse, field extraction, SHA-256 fingerprint, chain verify deferred |
| ec | t2/crypto/ec.h | t2/crypto/ec.c | Ed25519 keygen/sign/verify, Curve25519 (X25519) |
| ecdsa | t2/crypto/ecdsa.h | t2/crypto/ecdsa.c | ECDSA P-256 sign/verify, ECDH P-256 |
| secret | t2/crypto/secret.h | t2/crypto/secret.c | Secure memory: mlock, zeroize-on-free, ct compare |
| asn1_der | t2/encoding/asn1_der.h | t2/encoding/asn1_der.c | DER encoder/decoder, tag/length/value, SEQUENCE/SET, OID, INTEGER, BITSTRING |
| pem | t2/encoding/pem.h | t2/encoding/pem.c | PEM ↔ DER, auto-detect, label extraction |
| bigint | t2/math/bigint/bigint.h | t2/math/bigint/bigint.c | Arbitrary-precision: add/sub/mul/div, modpow, modmul, GCD, from/to hex/bytes |

### Dependencies

All modules depend on `apennines/types.h` and `apennines/export.h` (included).

Inter-module deps:
- `rsa` → `bigint`, `hash`, `asn1_der`
- `x509` → `asn1_der`, `pem`, `hash`
- `ecdsa` → `hash`
- `ec` → `hash`
- `cipher` → standalone (AES tables + ChaCha20 state are self-contained)

### Governing Rule

Every function returns `unsigned long` (0 = success, non-zero = error hatch ID).
First parameter is always the output pointer. See guides for per-function hatch tables.

### For TLS 1.3

With these modules you have:
- **Cipher suites**: AES-128-GCM (mandatory) + ChaCha20-Poly1305 (recommended) via `cipher`
- **Key derivation**: HKDF-SHA-256 (extract + expand) via `hash`
- **Certificate parsing**: X.509 DER/PEM via `x509` + `asn1_der` + `pem`
- **RSA verify**: PKCS#1 v1.5 + PSS via `rsa` + `bigint`
- **ECDHE**: P-256 ECDH via `ecdsa`, X25519 via `ec`
- **Constant-time ops**: via `ct`
- **Secure memory**: via `secret`
