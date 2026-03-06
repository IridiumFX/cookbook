# Proposal: application/x-pasta Content Negotiation for Cookbook

**From**: cookbook registry team
**To**: now spec maintainers
**Re**: now spec section 23 — Pasta media type registration
**Date**: 2026-03-06

---

## Summary

Cookbook is ready to implement content negotiation for Pasta descriptors (roadmap item #8, spec section 8.3). We need the now spec to finalize section 23 so that we can assign the correct media type and serialization rules. This document proposes what cookbook would implement and identifies the decisions we need from the now team.

---

## What cookbook wants to do

When a client requests a descriptor, it currently always receives the raw `now.pasta` file as `text/plain`. We want to support:

1. **`Accept: application/x-pasta`** on `GET /artifact/{group}/{artifact}/{version}/now.pasta`
   - Returns the descriptor in canonical Pasta serialization with the registered media type.

2. **`Accept: application/json`** on the same endpoint
   - Returns an equivalent JSON representation for tooling that doesn't have a Pasta parser.

3. **`Content-Type: application/x-pasta`** on `PUT /artifact/{group}/{artifact}/{version}/now.pasta`
   - Validates that the uploaded body is well-formed Pasta and applies descriptor validation (field checks, version matching, etc.) as today.

4. **Default behavior** (no `Accept` header or `Accept: */*`)
   - Returns `application/x-pasta` (the canonical format), maintaining backwards compatibility since the wire bytes are identical to what we serve today as `text/plain`.

---

## What we need from the now spec

### 1. Media type name

We assume `application/x-pasta` but need confirmation. Alternatives to consider:

| Option | Notes |
|--------|-------|
| `application/x-pasta` | Simple, unregistered experimental type |
| `application/vnd.now.pasta` | Vendor-specific, IANA-registrable |
| `application/pasta` | Requires IANA registration (standards-track) |
| `text/x-pasta` | If Pasta is considered human-readable text (US-ASCII aligns with text/* default charset) |

**Recommendation**: `application/vnd.now.pasta` gives us a clean namespace under the `now` project and is registrable with IANA when the spec stabilizes. We can use `application/x-pasta` as an interim alias.

### 2. Canonical serialization

Pasta supports both compact and pretty-printed output. For content negotiation we need a defined canonical form:

- **Compact** (`pasta_write(val, PASTA_COMPACT)`): Minimal whitespace, deterministic key ordering. Better for checksums and signatures.
- **Pretty** (`pasta_write(val, PASTA_PRETTY)`): Human-readable with indentation.

**Recommendation**: Canonical form should be compact with sorted keys. Pretty-printed should be available via an `Accept` parameter, e.g. `Accept: application/vnd.now.pasta; format=pretty`.

### 3. JSON mapping rules

For `Accept: application/json` responses, we need defined rules for how Pasta types map to JSON:

| Pasta type | JSON mapping |
|------------|-------------|
| String | JSON string |
| Integer | JSON number |
| Float | JSON number |
| Boolean | JSON boolean |
| Null | JSON null |
| List | JSON array |
| Map | JSON object |
| Multi-line string | JSON string (newlines as `\n`) |
| Tagged values (e.g., `@path "..."`) | Needs spec decision |

**Key question**: How should Pasta's tagged values (like `@path`, `@env`, `@glob`) be represented in JSON? Options:

- A. `{"@path": "src/main"}` (special `@`-prefixed key)
- B. `{"_tag": "path", "_value": "src/main"}` (explicit wrapper)
- C. Omit tags entirely (lossy but simple)

**Recommendation**: Option A is the most natural for JSON consumers. Reserve `@`-prefixed keys as tag markers.

### 4. Charset and encoding

- Pasta is US-ASCII. Should the media type include `; charset=us-ascii` explicitly, or is it implied by the registration?
- Cookbook currently accepts descriptors as raw bytes and passes them to libpasta, which handles the ASCII constraint. We need to confirm whether cookbook should enforce ASCII at the HTTP layer (rejecting bytes > 0x7F before parsing) or continue to defer to libpasta.
- For the JSON representation (`Accept: application/json`), the output would be UTF-8 as per JSON spec (RFC 8259). String values that are valid ASCII are trivially valid UTF-8, so no transcoding is needed.
- **Recommendation**: Implied US-ASCII for the Pasta media type. Cookbook will reject non-ASCII input with 400 at the HTTP layer for defense-in-depth.

### 5. Descriptor versioning

If the Pasta format evolves, should the media type carry a version parameter?

- `application/vnd.now.pasta; version=1`
- Or rely on the `spec-version` field inside the descriptor itself?

**Recommendation**: Use the in-document `spec-version` field. Media type versioning adds negotiation overhead and the descriptor already self-describes its format version.

---

## What cookbook will implement (once spec is finalized)

### Server-side changes

1. **Content-Type on GET responses**: Serve `now.pasta` with `Content-Type: application/vnd.now.pasta` (or whatever is decided) instead of `text/plain`.

2. **Accept header parsing**: Parse the `Accept` header on descriptor GET requests. Supported types:
   - `application/vnd.now.pasta` (or alias) — return Pasta format
   - `application/json` — return JSON representation
   - `text/plain` — return raw Pasta (backwards compatibility)
   - `*/*` — default to Pasta format

3. **Quality values**: Respect `q=` parameters in Accept for proper negotiation (e.g., `Accept: application/json; q=0.9, application/vnd.now.pasta; q=1.0`).

4. **406 Not Acceptable**: Return 406 if the client requests a type we don't support.

5. **Content-Type on PUT validation**: Accept `Content-Type: application/vnd.now.pasta` and `text/plain` on PUT. Reject uploads with incompatible Content-Type for `now.pasta` files. Reject non-ASCII bytes (> 0x7F) at the HTTP layer before passing to libpasta.

### Implementation estimate

This is straightforward to implement in cookbook. The Pasta library already supports both `PASTA_COMPACT` and `PASTA_PRETTY` serialization. JSON output would be a new ~100-line serializer walking the Pasta value tree. The Accept header parser is ~50 lines. Total: ~200 lines of C, well-contained in `cookbook_server.c`.

### Wire compatibility

The change is backwards-compatible:
- Clients that don't send `Accept` headers get the same bytes as today.
- The only visible change is the `Content-Type` response header moving from `text/plain` to the new media type.
- Clients that need `text/plain` can request it explicitly.

---

## Timeline

We can implement this within a day of the spec section being finalized. No new dependencies are required — libpasta and the existing server infrastructure are sufficient. We'd appreciate a decision on the media type name and JSON mapping rules so we can proceed.
