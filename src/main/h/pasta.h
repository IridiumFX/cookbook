/*
 * Pasta compatibility header — maps Pasta API to Basta.
 *
 * Basta is a strict superset of Pasta (adds BASTA_BLOB for binary data).
 * All Pasta documents parse identically through Basta.  This header lets
 * existing code that uses pasta_* names compile and link against libbasta
 * without modification.
 *
 * The only API difference: basta_write() takes an extra size_t *out_len
 * parameter (for blob byte-count reporting).  The macro below passes NULL
 * so the caller signature stays pasta_write(v, flags).
 */
#ifndef PASTA_H
#define PASTA_H

#include "basta.h"

/* ---- Types ---- */
typedef BastaValue   PastaValue;
typedef BastaResult  PastaResult;

/* ---- Error codes ---- */
typedef BastaError   PastaError;
#define PASTA_OK                 BASTA_OK
#define PASTA_ERR_ALLOC          BASTA_ERR_ALLOC
#define PASTA_ERR_SYNTAX         BASTA_ERR_SYNTAX
#define PASTA_ERR_UNEXPECTED_TOKEN BASTA_ERR_UNEXPECTED_TOKEN
#define PASTA_ERR_UNEXPECTED_EOF BASTA_ERR_UNEXPECTED_EOF

/* ---- Value types ---- */
typedef BastaType    PastaType;
#define PASTA_NULL    BASTA_NULL
#define PASTA_BOOL    BASTA_BOOL
#define PASTA_NUMBER  BASTA_NUMBER
#define PASTA_STRING  BASTA_STRING
#define PASTA_ARRAY   BASTA_ARRAY
#define PASTA_MAP     BASTA_MAP
#define PASTA_LABEL   BASTA_LABEL

/* ---- Parsing / lifetime ---- */
#define pasta_parse           basta_parse
#define pasta_parse_cstr      basta_parse_cstr
#define pasta_free            basta_free

/* ---- Writing ---- */
/* basta_write takes an extra out_len param; wrap to match pasta_write(v, flags) */
#define pasta_write(v, flags) basta_write((v), (flags), NULL)
#define pasta_write_fp        basta_write_fp
#define PASTA_PRETTY          BASTA_PRETTY
#define PASTA_COMPACT         BASTA_COMPACT
#define PASTA_SECTIONS        BASTA_SECTIONS
#define PASTA_SORTED          BASTA_SORTED

/* ---- Query ---- */
#define pasta_type            basta_type
#define pasta_is_null         basta_is_null
#define pasta_get_bool        basta_get_bool
#define pasta_get_number      basta_get_number
#define pasta_get_string      basta_get_string
#define pasta_get_string_len  basta_get_string_len
#define pasta_get_label       basta_get_label
#define pasta_get_label_len   basta_get_label_len
#define pasta_count           basta_count
#define pasta_array_get       basta_array_get
#define pasta_map_get         basta_map_get
#define pasta_map_key         basta_map_key
#define pasta_map_value       basta_map_value

/* ---- Building ---- */
#define pasta_new_null        basta_new_null
#define pasta_new_bool        basta_new_bool
#define pasta_new_number      basta_new_number
#define pasta_new_string      basta_new_string
#define pasta_new_string_len  basta_new_string_len
#define pasta_new_label       basta_new_label
#define pasta_new_label_len   basta_new_label_len
#define pasta_new_array       basta_new_array
#define pasta_new_map         basta_new_map
#define pasta_push            basta_push
#define pasta_set             basta_set
#define pasta_set_len         basta_set_len

#endif /* PASTA_H */
