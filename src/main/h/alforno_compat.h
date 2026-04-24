#ifndef ALFORNO_COMPAT_H
#define ALFORNO_COMPAT_H

/* Shim for the sibling alforno API. Returns PastaValue* (from our
 * pasta_compat shim) so alforno tests can freely mix in pasta_*
 * queries on the result. */

/* Include pasta_compat so PastaValue is defined and pasta_* macros
 * are active inside the sibling test file. */
#include "pasta_compat.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ALF_API
#define ALF_API
#endif

typedef enum {
    ALF_AGGREGATE,
    ALF_CONFLATE,
    ALF_SCATTER,
    ALF_GATHER
} AlfOp;

typedef enum {
    ALF_LAST_WINS,
    ALF_FIRST_FOUND
} AlfPrecedence;

typedef enum {
    ALF_OK = 0,
    ALF_ERR_ALLOC,
    ALF_ERR_PARSE,
    ALF_ERR_NOT_SECTIONS,
    ALF_ERR_MISSING_CONSUMES,
    ALF_ERR_BAD_RECIPE,
    ALF_ERR_UNRESOLVED_VAR,
    ALF_ERR_CYCLE,
    ALF_ERR_VALIDATION,
    ALF_ERR_INCLUDE,
    ALF_ERR_IO
} AlfError;

typedef struct {
    AlfError code;
    int      pass;
    char     section[64];
    char     message[256];
} AlfResult;

typedef struct AlfContext AlfContext;

ALF_API AlfContext *talf_create(AlfOp op, AlfResult *result);
ALF_API int talf_set_precedence(AlfContext *ctx, AlfPrecedence prec, AlfResult *result);
ALF_API int talf_set_tags(AlfContext *ctx, const char **tags, size_t count, AlfResult *result);
ALF_API int talf_set_base_dir(AlfContext *ctx, const char *dir, AlfResult *result);
ALF_API int talf_add_input_file(AlfContext *ctx, const char *path, AlfResult *result);
ALF_API int talf_set_recipe(AlfContext *ctx, const char *src, size_t len, AlfResult *result);
ALF_API int talf_add_input(AlfContext *ctx, const char *src, size_t len, AlfResult *result);
ALF_API PastaValue *talf_process(AlfContext *ctx, AlfResult *result);
ALF_API char *talf_process_to_string(AlfContext *ctx, int flags, AlfResult *result);
ALF_API int talf_scatter_to_dir(AlfContext *ctx, const char *output_dir, const char *ext, AlfResult *result);
ALF_API void talf_free(AlfContext *ctx);

#define alf_create              talf_create
#define alf_set_precedence      talf_set_precedence
#define alf_set_tags            talf_set_tags
#define alf_set_base_dir        talf_set_base_dir
#define alf_add_input_file      talf_add_input_file
#define alf_set_recipe          talf_set_recipe
#define alf_add_input           talf_add_input
#define alf_process             talf_process
#define alf_process_to_string   talf_process_to_string
#define alf_scatter_to_dir      talf_scatter_to_dir
#define alf_free                talf_free

#ifdef __cplusplus
}
#endif

#endif /* ALFORNO_COMPAT_H */
