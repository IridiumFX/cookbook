# Response: Content Negotiation for Cookbook Registry

**From**: now spec maintainers
**To**: cookbook registry team
**Re**: proposal-pasta-content-negotiation.md
**Date**: 2026-03-07

---

## Overall Assessment

The proposal is well-structured and mostly aligned with where the spec is heading. We agree on the general direction: serve Pasta as the canonical format, support JSON as an integration format, use `Accept` headers, and keep the change backwards-compatible.

We disagree on a few specifics. This document addresses each of the five decisions requested, then raises two additional concerns.

---

## Decision 1: Media Type Name

**Proposal recommends**: `application/vnd.now.pasta` with `application/x-pasta` as interim alias.

**Our position**: Use `application/pasta` as the target, with `application/x-pasta` during the pre-registration period. Do not use `application/vnd.now.pasta`.

**Rationale**:

Pasta is an independent serialisation format — it is not owned by the `now` project. It has its own repository, its own MIT license, and its own API surface (`pasta.h`). Putting `now` in the media type creates a false ownership claim and would need to be changed if Pasta gains adoption outside the `now` ecosystem (which is the goal — see pico_http's MIT spin-off as precedent).

The `vnd.` tree is for vendor-specific formats. Pasta is a general-purpose data format in the same category as JSON, TOML, and YAML. None of those use vendor prefixes (`application/json`, `application/toml`, `application/yaml`). Pasta should follow the same convention.

The path:

| Phase | Media type | When |
|-------|-----------|------|
| Now (pre-IANA) | `application/x-pasta` | Cookbook beta, `now` v0.x |
| Post-registration | `application/pasta` | After IANA submission |

The `x-` prefix is deprecated by RFC 6648 but still widely understood and appropriate for an experimental period. When Pasta is mature enough for IANA registration, we drop the prefix. There is no intermediate `vnd.` step.

Cookbook should implement `application/x-pasta` now and treat `application/pasta` as an alias it already accepts (but does not emit until registration is complete).

---

## Decision 2: Canonical Serialisation

**Proposal recommends**: Compact with sorted keys as canonical form. Pretty via `Accept` parameter.

**Our position**: Agree on compact-with-sorted-keys as canonical. Disagree on the `Accept` parameter mechanism for pretty-printing.

The canonical form must be deterministic for checksums and signatures. This means:

1. **Compact mode** (`PASTA_COMPACT` flag in libpasta)
2. **Sorted keys** — lexicographic sort of map keys at all nesting levels
3. **No comments** — canonical form strips comments
4. **Normalised numbers** — no leading zeros, no trailing zeros after decimal point
5. **Minimal quoting** — keys that are valid bare identifiers are unquoted

This canonical form is what the registry stores, what SHA-256 checksums cover, and what signatures sign.

**On pretty-printing**: Do not use `Accept` parameters (`format=pretty`). The HTTP `Accept` header negotiates the *type* of content, not its formatting. Pretty-printing is a presentation concern, not a content type distinction. Instead:

- `GET /artifact/.../now.pasta` — always returns canonical (compact) form
- `GET /artifact/.../now.pasta?pretty` — returns pretty-printed form (query parameter)
- The `Content-Type` header is the same in both cases (`application/x-pasta`)

This follows the convention used by APIs like GitHub (`?pretty=true`) and Elasticsearch.

**Requirement for libpasta**: Sorted keys are not currently a libpasta feature. `pasta_write(v, PASTA_COMPACT)` preserves insertion order. We need libpasta to add a `PASTA_SORTED` flag (value `4`, OR-able with `COMPACT`/`PRETTY`). This is a request to the Pasta team — cookbook should not implement its own key-sorting serialiser.

---

## Decision 3: JSON Mapping Rules

**Proposal recommends**: Option A (`{"@path": "src/main"}`) for tagged values.

**Our position**: There are no tagged values. The proposal's premise is incorrect.

Pasta's type system (as implemented in libpasta `pasta.h`) has six types:

| `PastaType` | JSON equivalent |
|-------------|----------------|
| `PASTA_NULL` | `null` |
| `PASTA_BOOL` | `true` / `false` |
| `PASTA_NUMBER` | number |
| `PASTA_STRING` | string |
| `PASTA_ARRAY` | array |
| `PASTA_MAP` | object |

There are no tagged values (`@path`, `@env`, `@glob`) in the Pasta data model. The `@section` keyword in `PASTA_SECTIONS` write mode is a serialisation-level feature of the writer, not a data type — it produces the same in-memory tree as a regular map.

The proposal may be confusing Pasta (the format) with `now`-specific field conventions. If the `now` spec later defines semantic annotations (like marking a string as a path), those would be encoded as regular Pasta map fields (`{ type: "path", value: "src/main" }`) — not as tagged values in the serialisation layer.

**The JSON mapping is therefore trivially 1:1**:

| Pasta | JSON |
|-------|------|
| String | String |
| Number | Number |
| Bool | Boolean |
| Null | null |
| Array | Array |
| Map | Object |

Multiline strings (a Pasta syntactic convenience) become regular JSON strings with literal `\n` escapes. No special cases are needed.

**Recommendation to cookbook**: Implement JSON serialisation as a simple recursive walk of the `PastaValue` tree. This should be ~60 lines, not 100, precisely because there are no edge cases.

---

## Decision 4: Charset and Encoding

**Proposal recommends**: Implied US-ASCII for Pasta, reject non-ASCII at HTTP layer.

**Our position**: Agree in principle, with a refinement.

- `application/x-pasta` implies US-ASCII. Do not add `; charset=us-ascii` to the `Content-Type` — it is noise.
- Cookbook should reject bytes > `0x7F` on `PUT` with a `400 Bad Request` and a clear error message: `{"error": "Non-ASCII byte at offset {n} — Pasta requires US-ASCII input"}`.
- On `GET`, this check is unnecessary — the registry only stores what it accepted on `PUT`.
- `application/json` responses use UTF-8 per RFC 8259. Since all stored data is ASCII, this requires no transcoding.

One addition: Cookbook should also reject the NUL byte (`0x00`) on PUT. A NUL in a Pasta file is never valid and would break C string handling in both libpasta and now.

---

## Decision 5: Descriptor Versioning

**Proposal recommends**: Use in-document `spec-version` field, no media type version parameter.

**Our position**: Agree completely.

Media type version parameters add negotiation complexity that is not justified when the document already self-describes. The `now` spec uses a `spec-version` field in the descriptor for this purpose. If a future Pasta 2.0 is radically incompatible (different syntax, not just new fields), a new media type (`application/x-pasta2`) would be more appropriate than a version parameter.

---

## Additional Concern 1: Scope of Content Negotiation

The proposal covers only the `now.pasta` descriptor endpoint. The spec (section 6.5) says `/resolve/` returns "JSON or Pasta list of available versions matching the range". Content negotiation should apply to all endpoints that return structured data:

| Endpoint | Current | Should support |
|----------|---------|----------------|
| `GET /resolve/{g}/{a}/{range}` | JSON only | JSON + Pasta |
| `GET /artifact/.../now.pasta` | `text/plain` | Pasta + JSON |
| `GET /healthz` | JSON | JSON (no Pasta needed) |
| `POST /auth/token` | JSON | JSON (no Pasta needed) |
| Error responses | JSON | JSON (no Pasta needed) |

For `/resolve/`, the Pasta response would be:

```pasta
{
  versions: [
    { version: "1.2.3", snapshot: false, triples: ["noarch"] },
    { version: "1.2.2", snapshot: false, triples: ["linux-x86_64-gnu"] }
  ]
}
```

This is important because `now` itself will be a Pasta-native client. Having `now procure` parse JSON responses from a Pasta-native registry is an unnecessary impedance mismatch. The `now` client should be able to send `Accept: application/x-pasta` and get Pasta back from all structured endpoints.

JSON should remain the default (for backwards compatibility and for third-party tooling), but Pasta should be available.

---

## Additional Concern 2: `406 Not Acceptable` Behaviour

The proposal mentions returning `406` for unsupported types. We agree, but with a constraint:

- `406` should only be returned when the `Accept` header is present and contains no supported types.
- A missing `Accept` header must never produce `406` — it defaults to the canonical format.
- `Accept: */*` must never produce `406`.

This sounds obvious but is a common implementation mistake. Please add explicit test cases for:

1. No `Accept` header → 200 with Pasta
2. `Accept: */*` → 200 with Pasta
3. `Accept: application/x-pasta` → 200 with Pasta
4. `Accept: application/json` → 200 with JSON
5. `Accept: text/plain` → 200 with Pasta (raw bytes, backwards compat)
6. `Accept: application/xml` → 406
7. `Accept: application/json; q=0.9, application/x-pasta; q=1.0` → 200 with Pasta

---

## Summary of Decisions

| # | Topic | Decision |
|---|-------|---------|
| 1 | Media type | `application/x-pasta` now, `application/pasta` after IANA registration. No `vnd.now.` prefix. |
| 2 | Canonical form | Compact, sorted keys, no comments. Pretty via `?pretty` query param, not `Accept` param. Needs `PASTA_SORTED` flag in libpasta. |
| 3 | JSON mapping | Trivial 1:1 mapping. No tagged values exist in Pasta. |
| 4 | Charset | Implied US-ASCII. Reject `> 0x7F` and `0x00` on PUT with 400. |
| 5 | Versioning | In-document `spec-version` field. No media type version param. |
| 6 | Scope | Apply content negotiation to `/resolve/` as well, not just descriptors. |
| 7 | 406 safety | Never 406 on missing `Accept` or `*/*`. Explicit test matrix provided. |

---

## Action Items

| Owner | Action |
|-------|--------|
| Pasta team | Add `PASTA_SORTED` flag (value `4`) to `pasta_write` for deterministic key ordering |
| Cookbook | Implement `Accept` header parsing with the type hierarchy above |
| Cookbook | Add Pasta serialisation to `/resolve/` endpoint |
| Cookbook | Reject `> 0x7F` and `0x00` bytes on PUT |
| Cookbook | Add the 7-case `Accept` test matrix |
| now team | Update spec Chapter 20 (Serialisation) to reference `application/x-pasta` media type |
| now team | Update `pico_http` to support setting `Accept` headers in requests |

---

## Unblocking Cookbook

Cookbook can proceed immediately with items 1-5 from the decisions table. The `PASTA_SORTED` flag is not a blocker — cookbook can serve unsorted compact form initially and switch to sorted once libpasta ships the flag. The SHA-256 canonical checksums in the lock file should be computed client-side (by `now`) against the sorted form, so cookbook does not need sorted output for correctness — only for consistency when clients compare raw bytes.

Start with `application/x-pasta` on all structured endpoints. We will coordinate the IANA registration when both Pasta and `now` reach v1.0.
