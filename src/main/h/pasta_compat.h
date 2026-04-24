#ifndef PASTA_COMPAT_H
#define PASTA_COMPAT_H

/* ================================================================
 *  Compatibility shim — exposes the sibling ../Pasta project's
 *  public API (pasta_parse, pasta_free, PastaValue, ...) in terms
 *  of our apennines t4/pasta implementation.
 *
 *  Lets us drop in the sibling's pasta_test.c (and the alforno /
 *  basta variants) unchanged and have them exercise our parser,
 *  writer, and value tree, confirming spec compatibility across
 *  ~7k lines of third-party tests.
 *
 *  Internally each sibling-named entry point is implemented as
 *  tpasta_xxx (test-pasta) to avoid linker-level symbol collision
 *  with our apennines API. The macros below rewrite the sibling's
 *  calls. Source files that directly use our apennines pasta API
 *  MUST NOT include this header.
 * ================================================================ */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PASTA_API
#define PASTA_API
#endif

/* Value types — same ordering as sibling pasta.h. */
typedef enum {
    PASTA_NULL = 0,
    PASTA_BOOL,
    PASTA_NUMBER,
    PASTA_STRING,
    PASTA_ARRAY,
    PASTA_MAP,
    PASTA_LABEL
} PastaType;

#define PASTA_NUM_DEC  0
#define PASTA_NUM_HEX  1
#define PASTA_NUM_BIN  2

typedef struct PastaValue PastaValue;

typedef enum {
    PASTA_OK = 0,
    PASTA_ERR_ALLOC,
    PASTA_ERR_SYNTAX,
    PASTA_ERR_UNEXPECTED_TOKEN,
    PASTA_ERR_UNEXPECTED_EOF
} PastaError;

typedef struct {
    PastaError code;
    int        line;
    int        col;
    int        sections;
    char       message[256];
} PastaResult;

#define PASTA_PRETTY   0
#define PASTA_COMPACT  1
#define PASTA_SECTIONS 2
#define PASTA_SORTED   4

/* Declare tpasta_ entry points. */
PASTA_API PastaValue *tpasta_parse(const char *input, size_t len, PastaResult *result);
PASTA_API PastaValue *tpasta_parse_cstr(const char *input, PastaResult *result);
PASTA_API void        tpasta_free(PastaValue *value);
PASTA_API PastaType    tpasta_type(const PastaValue *v);
PASTA_API int          tpasta_is_null(const PastaValue *v);
PASTA_API int          tpasta_get_bool(const PastaValue *v);
PASTA_API double       tpasta_get_number(const PastaValue *v);
PASTA_API int          tpasta_get_number_fmt(const PastaValue *v);
PASTA_API const char  *tpasta_get_string(const PastaValue *v);
PASTA_API size_t       tpasta_get_string_len(const PastaValue *v);
PASTA_API const char  *tpasta_get_label(const PastaValue *v);
PASTA_API size_t       tpasta_get_label_len(const PastaValue *v);
PASTA_API size_t             tpasta_count(const PastaValue *v);
PASTA_API const PastaValue  *tpasta_array_get(const PastaValue *v, size_t index);
PASTA_API const PastaValue  *tpasta_map_get(const PastaValue *v, const char *key);
PASTA_API const char        *tpasta_map_key(const PastaValue *v, size_t index);
PASTA_API const PastaValue  *tpasta_map_value(const PastaValue *v, size_t index);
PASTA_API PastaValue *tpasta_new_null(void);
PASTA_API PastaValue *tpasta_new_bool(int b);
PASTA_API PastaValue *tpasta_new_number(double n);
PASTA_API PastaValue *tpasta_new_number_fmt(double n, int fmt);
PASTA_API PastaValue *tpasta_new_string(const char *s);
PASTA_API PastaValue *tpasta_new_string_len(const char *s, size_t len);
PASTA_API PastaValue *tpasta_new_label(const char *s);
PASTA_API PastaValue *tpasta_new_label_len(const char *s, size_t len);
PASTA_API PastaValue *tpasta_new_array(void);
PASTA_API PastaValue *tpasta_new_map(void);
PASTA_API int tpasta_push(PastaValue *array, PastaValue *item);
PASTA_API int tpasta_set(PastaValue *map, const char *key, PastaValue *value);
PASTA_API int tpasta_set_len(PastaValue *map, const char *key, size_t key_len, PastaValue *value);
PASTA_API char *tpasta_write(const PastaValue *v, int flags);
PASTA_API int   tpasta_write_fp(const PastaValue *v, int flags, void *fp);

/* Rewrite sibling pasta_* calls to our tpasta_*. */
#define pasta_parse               tpasta_parse
#define pasta_parse_cstr          tpasta_parse_cstr
#define pasta_free                tpasta_free
#define pasta_type                tpasta_type
#define pasta_is_null             tpasta_is_null
#define pasta_get_bool            tpasta_get_bool
#define pasta_get_number          tpasta_get_number
#define pasta_get_number_fmt      tpasta_get_number_fmt
#define pasta_get_string          tpasta_get_string
#define pasta_get_string_len      tpasta_get_string_len
#define pasta_get_label           tpasta_get_label
#define pasta_get_label_len       tpasta_get_label_len
#define pasta_count               tpasta_count
#define pasta_array_get           tpasta_array_get
#define pasta_map_get             tpasta_map_get
#define pasta_map_key             tpasta_map_key
#define pasta_map_value           tpasta_map_value
#define pasta_new_null            tpasta_new_null
#define pasta_new_bool            tpasta_new_bool
#define pasta_new_number          tpasta_new_number
#define pasta_new_number_fmt      tpasta_new_number_fmt
#define pasta_new_string          tpasta_new_string
#define pasta_new_string_len      tpasta_new_string_len
#define pasta_new_label           tpasta_new_label
#define pasta_new_label_len       tpasta_new_label_len
#define pasta_new_array           tpasta_new_array
#define pasta_new_map             tpasta_new_map
#define pasta_push                tpasta_push
#define pasta_set                 tpasta_set
#define pasta_set_len             tpasta_set_len
#define pasta_write               tpasta_write
#define pasta_write_fp            tpasta_write_fp

#ifdef __cplusplus
}
#endif

#endif /* PASTA_COMPAT_H */
