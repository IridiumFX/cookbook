#ifndef APENNINES_T4_PASTA_ALFORNO_H
#define APENNINES_T4_PASTA_ALFORNO_H

#include "apennines/export.h"
#include "apennines/types.h"
#include "apennines/t4/pasta/pasta.h"

/* ================================================================
 *  alforno — pasta/basta processor (Italian for "baked").
 *
 *  Four verbs on one or more input pastlets (section-based maps):
 *
 *    aggregate — open union. All sections and fields from all inputs
 *                flow through; duplicates resolve last-write-wins
 *                in input declaration order.
 *
 *    conflate  — recipe-gated. A recipe pastlet declares the output
 *                sections + fields; unknown sections and fields are
 *                dropped. Recipe keys that name a merge strategy or
 *                a required/optional type produce validation errors.
 *
 *    gather    — combine many inputs into one. Precedence selectable:
 *                last-wins (default) or first-found (first non-null
 *                value for each key is kept).
 *
 *    scatter   — split the inputs' sections into a map keyed by
 *                section name, with each section as its own pastlet.
 *                Useful as a reverse operation.
 *
 *  Pre-passes (run before the operation):
 *    1. parameterize — resolve {var} tokens in strings using @vars
 *    2. when-filter  — drop sections whose `when:` tag list doesn't
 *                       intersect the active tag set
 *    3. link resolve — values that are label-refs resolve to the
 *                       named section's content (embedded in place)
 *
 *  @include resolution (filesystem) is NOT handled inside alforno;
 *  callers pre-build the input list.
 *
 *  Lifetime: alforno takes ownership of any pastlet passed to
 *  alforno_add_input / alforno_set_recipe. alforno_destroy frees
 *  its own state + any remaining inputs. The output from
 *  alforno_process is caller-owned.
 * ================================================================ */

typedef enum {
    ALFORNO_OP_AGGREGATE,
    ALFORNO_OP_CONFLATE,
    ALFORNO_OP_GATHER,
    ALFORNO_OP_SCATTER
} alforno_op;

typedef enum {
    ALFORNO_LAST_WINS   = 0,
    ALFORNO_FIRST_FOUND = 1
} alforno_precedence;

typedef struct alforno_ctx alforno_ctx;

APENNINES_API unsigned long alforno_create(alforno_ctx **out, alforno_op op);
APENNINES_API unsigned long alforno_destroy(alforno_ctx *ctx);

/* Set gather precedence. Only meaningful for ALFORNO_OP_GATHER. */
APENNINES_API unsigned long alforno_set_precedence(alforno_ctx *ctx,
                                                     alforno_precedence prec);

/* Set active tags for when-filtering. `tags` are copied; caller
 * retains ownership of the array and strings. */
APENNINES_API unsigned long alforno_set_tags(alforno_ctx *ctx,
                                               const char **tags, u64 count);

/* Set the base directory used for resolving @include paths that
 * are relative. When unset, relative includes resolve against the
 * including file's own directory (or the process cwd for
 * programmatically-added inputs with @include entries). */
APENNINES_API unsigned long alforno_set_base_dir(alforno_ctx *ctx,
                                                   const char *dir);

/* Convenience loader: read `path`, parse it, recursively resolve
 * any @include sections, and add the resulting pastlet(s) as
 * inputs. The same file included via multiple paths in a chain is
 * rejected as a cycle. */
APENNINES_API unsigned long alforno_add_input_file(alforno_ctx *ctx,
                                                     const char *path);

/* Attach a recipe for conflate. Takes ownership. Must be called
 * before process() for conflate ops; other ops ignore it. */
APENNINES_API unsigned long alforno_set_recipe(alforno_ctx *ctx,
                                                 pasta_value *recipe);

/* Add an input pastlet. Takes ownership. */
APENNINES_API unsigned long alforno_add_input(alforno_ctx *ctx,
                                                pasta_value *pastlet);

/* Run the pipeline; *out is the result. On error, *out is set to
 * NULL and a non-zero hatch is returned. */
APENNINES_API unsigned long alforno_process(pasta_value **out,
                                              alforno_ctx *ctx);

#endif /* APENNINES_T4_PASTA_ALFORNO_H */
