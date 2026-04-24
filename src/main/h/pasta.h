/*
 * pasta.h — cookbook's handler-facing pasta API.
 *
 * Redirects to the pasta_compat shim, which implements the sibling-project
 * pasta_* / PastaValue API on top of apennines `t4/pasta`. Cookbook code
 * compiles unchanged; the pasta_compat.c wrappers translate each call to
 * the apennines native pasta_value / out-param / hatch-code shape.
 *
 * Also provides basta_* aliases for call sites that went through the
 * rc1-era `basta.h` superset header. Semantically basta is pasta with
 * the added BASTA_BLOB type — cookbook doesn't use blobs, so the alias
 * is a straight rename.
 *
 * History: basta superseded pasta in rc1-era; rc4 switched the backend to
 * apennines' native t4/pasta, with pasta_compat as the thin adapter.
 */
#ifndef PASTA_H
#define PASTA_H

#include "pasta_compat.h"

/* ---- Basta-name aliases (cookbook legacy — rc1 era) ------------------- */

typedef PastaValue  BastaValue;
typedef PastaType   BastaType;
typedef PastaError  BastaError;
typedef PastaResult BastaResult;

#define BASTA_NULL    PASTA_NULL
#define BASTA_BOOL    PASTA_BOOL
#define BASTA_NUMBER  PASTA_NUMBER
#define BASTA_STRING  PASTA_STRING
#define BASTA_ARRAY   PASTA_ARRAY
#define BASTA_MAP     PASTA_MAP
#define BASTA_LABEL   PASTA_LABEL

#define BASTA_OK                   PASTA_OK
#define BASTA_ERR_ALLOC            PASTA_ERR_ALLOC
#define BASTA_ERR_SYNTAX           PASTA_ERR_SYNTAX
#define BASTA_ERR_UNEXPECTED_TOKEN PASTA_ERR_UNEXPECTED_TOKEN
#define BASTA_ERR_UNEXPECTED_EOF   PASTA_ERR_UNEXPECTED_EOF

#define BASTA_PRETTY   PASTA_PRETTY
#define BASTA_COMPACT  PASTA_COMPACT
#define BASTA_SECTIONS PASTA_SECTIONS
#define BASTA_SORTED   PASTA_SORTED

#define basta_parse           pasta_parse
#define basta_parse_cstr      pasta_parse_cstr
#define basta_free            pasta_free
#define basta_type            pasta_type
#define basta_is_null         pasta_is_null
#define basta_get_bool        pasta_get_bool
#define basta_get_number      pasta_get_number
#define basta_get_string      pasta_get_string
#define basta_get_string_len  pasta_get_string_len
#define basta_get_label       pasta_get_label
#define basta_get_label_len   pasta_get_label_len
#define basta_count           pasta_count
#define basta_array_get       pasta_array_get
#define basta_map_get         pasta_map_get
#define basta_map_key         pasta_map_key
#define basta_map_value       pasta_map_value
#define basta_new_null        pasta_new_null
#define basta_new_bool        pasta_new_bool
#define basta_new_number      pasta_new_number
#define basta_new_string      pasta_new_string
#define basta_new_string_len  pasta_new_string_len
#define basta_new_label       pasta_new_label
#define basta_new_label_len   pasta_new_label_len
#define basta_new_array       pasta_new_array
#define basta_new_map         pasta_new_map
#define basta_push            pasta_push
#define basta_set             pasta_set
#define basta_set_len         pasta_set_len
/* basta_write takes an extra out_len param for blob-byte reporting;
 * wrap to match the pasta_write(v, flags) shape that cookbook uses. */
#define basta_write(v, flags) pasta_write((v), (flags))
#define basta_write_fp        pasta_write_fp

#endif /* PASTA_H */
