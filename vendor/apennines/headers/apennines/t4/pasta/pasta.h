#ifndef APENNINES_T4_PASTA_H
#define APENNINES_T4_PASTA_H

#include "apennines/export.h"
#include "apennines/types.h"
#include "apennines/t1/buffer/buf.h"

/* ================================================================
 *  pasta — "Plain And Simple Text Archive" data format.
 *  basta — Pasta superset with binary-blob values.
 *
 *  Wire-compatible with the sibling Pasta (../Pasta) and Basta
 *  (../Basta) projects. Rewritten to the apennines governing rule.
 *
 *  Grammar (brief; see Specs/pasta.txt for full BNF):
 *
 *    pasta       : container-seq | section-seq
 *    section     : "@" label container
 *    container   : "{" members "}" | "[" elements "]"
 *    members     : key ":" value ("," key ":" value)*
 *    elements    : value ("," value)*
 *    value       : container | string | number | boolean | "null"
 *                | "Inf" | "-Inf" | "NaN" | label-ref | blob
 *
 *  Strings: "simple" or triple-quoted """multiline""".
 *  Numbers: decimal, 0x... hex, 0b... binary.
 *  Labels: plain ASCII [a-zA-Z0-9.!#$%&_] or quoted "…".
 *  Comments: ";" to end of line.
 *  Blobs: 0x00 sentinel + u64be length + raw bytes (basta only).
 *
 *  A document using `@` sections parses as a map whose keys are the
 *  section names. This mirrors the sibling project's convention;
 *  PASTA_WRITE_SECTIONS on output re-emits that map in @section
 *  form instead of as a plain map.
 * ================================================================ */

typedef enum {
    PASTA_NULL,
    PASTA_BOOL,
    PASTA_INTEGER,
    PASTA_NUMBER,        /* IEEE 754 f64; covers Inf/-Inf/NaN */
    PASTA_STRING,
    PASTA_LABEL_REF,     /* unresolved reference to a @section */
    PASTA_ARRAY,
    PASTA_MAP,
    PASTA_BLOB
} pasta_kind;

typedef struct pasta_value pasta_value;

/* Parse diagnostics. The parser fills `line`/`col` on failure so
 * consumers can report where in the source the error landed. */
typedef struct {
    unsigned long hatch;     /* 0 on success */
    u64  offset;             /* byte offset into src */
    u32  line;
    u32  col;
    char msg[128];
} pasta_result;

/* ---- Construction ---- */

APENNINES_API unsigned long pasta_new_null(pasta_value **out);
APENNINES_API unsigned long pasta_new_bool(pasta_value **out, int b);
APENNINES_API unsigned long pasta_new_integer(pasta_value **out, i64 v);
APENNINES_API unsigned long pasta_new_number(pasta_value **out, f64 v);

/* Integer constructor with a literal-format hint for writer
 * roundtrip fidelity. `fmt` is one of:
 *   0 = decimal (default)
 *   1 = hex    (emits as 0xFF)
 *   2 = binary (emits as 0b1010)
 * Integers parsed from 0xFF or 0b1010 syntax carry these hints
 * automatically. */
APENNINES_API unsigned long pasta_new_integer_fmt(pasta_value **out,
                                                    i64 v, int fmt);
/* Read the format hint. Returns 0 (DEC) for values without a hint
 * set — e.g. everything that wasn't parsed as hex/bin or created
 * via pasta_new_integer_fmt. */
APENNINES_API unsigned long pasta_number_fmt(int *out, const pasta_value *v);
APENNINES_API unsigned long pasta_new_string(pasta_value **out, const char *s);
APENNINES_API unsigned long pasta_new_string_n(pasta_value **out,
                                                 const u8 *s, u64 n);
APENNINES_API unsigned long pasta_new_label_ref(pasta_value **out,
                                                  const char *name);
APENNINES_API unsigned long pasta_new_array(pasta_value **out);
APENNINES_API unsigned long pasta_new_map(pasta_value **out);
APENNINES_API unsigned long pasta_new_blob(pasta_value **out,
                                             const u8 *data, u64 n);

APENNINES_API unsigned long pasta_value_destroy(pasta_value *v);
APENNINES_API unsigned long pasta_value_clone(pasta_value **out,
                                                const pasta_value *src);

/* ---- Queries ---- */

APENNINES_API unsigned long pasta_kind_of(pasta_kind *out,
                                            const pasta_value *v);
APENNINES_API unsigned long pasta_as_bool(int *out, const pasta_value *v);
APENNINES_API unsigned long pasta_as_integer(i64 *out, const pasta_value *v);
APENNINES_API unsigned long pasta_as_number(f64 *out, const pasta_value *v);
APENNINES_API unsigned long pasta_as_string(const u8 **out_ptr, u64 *out_len,
                                              const pasta_value *v);
APENNINES_API unsigned long pasta_as_label(const u8 **out_ptr, u64 *out_len,
                                             const pasta_value *v);
APENNINES_API unsigned long pasta_as_blob(const u8 **out_ptr, u64 *out_len,
                                            const pasta_value *v);

/* Size of a container (map or array). */
APENNINES_API unsigned long pasta_count(u64 *out, const pasta_value *v);

APENNINES_API unsigned long pasta_array_at(pasta_value **out,
                                             const pasta_value *v, u64 idx);

APENNINES_API unsigned long pasta_map_get(pasta_value **out,
                                            const pasta_value *v,
                                            const char *key);
APENNINES_API unsigned long pasta_map_key(const u8 **out_ptr,
                                            u64 *out_len,
                                            const pasta_value *v,
                                            u64 idx);
APENNINES_API unsigned long pasta_map_value(pasta_value **out,
                                              const pasta_value *v,
                                              u64 idx);

/* ---- Mutation (caller transfers ownership of `val`) ---- */

APENNINES_API unsigned long pasta_array_push(pasta_value *a, pasta_value *val);
APENNINES_API unsigned long pasta_map_put(pasta_value *m,
                                            const char *key, pasta_value *val);
APENNINES_API unsigned long pasta_map_put_n(pasta_value *m,
                                              const u8 *key, u64 key_len,
                                              pasta_value *val);

/* ---- Parse / write ---- */

/* Parse a pasta/basta source buffer into a value tree. Document
 * using @section becomes a map keyed by section name. */
APENNINES_API unsigned long pasta_parse(pasta_value **out,
                                          pasta_result *res,
                                          const u8 *src, u64 len);

/* Flags for pasta_write. */
#define PASTA_WRITE_COMPACT   0x1    /* single-line, minimal whitespace */
#define PASTA_WRITE_SECTIONS  0x2    /* render root map as @name sections */
#define PASTA_WRITE_SORTED    0x4    /* deterministic key order */

APENNINES_API unsigned long pasta_write(buf *out,
                                          const pasta_value *v,
                                          u32 flags);

#endif /* APENNINES_T4_PASTA_H */
