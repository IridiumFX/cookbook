#ifndef COOKBOOK_SEMVER_H
#define COOKBOOK_SEMVER_H

#include "cookbook.h"
#include <stddef.h>

typedef struct {
    int   major;
    int   minor;
    int   patch;
    char  pre_release[64];
    char  build_meta[64];
} cookbook_semver;

/* Parse a version string like "1.3.0-beta.1+build.42".
   Returns 0 on success, -1 on malformed input. */
COOKBOOK_API int cookbook_semver_parse(const char *str, cookbook_semver *out);

/* Compare two versions per SemVer 2.0 precedence.
   Returns <0, 0, or >0 (like strcmp). Build metadata is ignored. */
COOKBOOK_API int cookbook_semver_compare(const cookbook_semver *a,
                                        const cookbook_semver *b);

/* Range types matching the now spec §6.10 */
typedef enum {
    COOKBOOK_RANGE_EXACT,     /* 1.3.0      */
    COOKBOOK_RANGE_CARET,     /* ^1.3.0     */
    COOKBOOK_RANGE_TILDE,     /* ~1.3.0     */
    COOKBOOK_RANGE_WILDCARD,  /* 1.* or *   */
    COOKBOOK_RANGE_BOUNDED    /* [1.0.0,2.0.0) Maven-style */
} cookbook_range_type;

typedef struct {
    cookbook_range_type type;
    cookbook_semver     lower;
    cookbook_semver     upper;
    int                lower_inclusive; /* for BOUNDED */
    int                upper_inclusive; /* for BOUNDED */
} cookbook_range;

/* Parse a range string. Returns 0 on success, -1 on error. */
COOKBOOK_API int cookbook_range_parse(const char *str, cookbook_range *out);

/* Test whether a version satisfies a range. Returns 1 if yes, 0 if no. */
COOKBOOK_API int cookbook_range_satisfies(const cookbook_range *range,
                                         const cookbook_semver *ver);

#endif /* COOKBOOK_SEMVER_H */
