#include "cookbook_semver.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int cookbook_semver_parse(const char *str, cookbook_semver *out) {
    if (!str || !out) return -1;
    memset(out, 0, sizeof(*out));

    const char *p = str;

    /* major */
    if (!isdigit((unsigned char)*p)) return -1;
    out->major = (int)strtol(p, (char **)&p, 10);

    if (*p != '.') return -1;
    p++;

    /* minor */
    if (!isdigit((unsigned char)*p)) return -1;
    out->minor = (int)strtol(p, (char **)&p, 10);

    if (*p != '.') return -1;
    p++;

    /* patch */
    if (!isdigit((unsigned char)*p)) return -1;
    out->patch = (int)strtol(p, (char **)&p, 10);

    /* pre-release */
    if (*p == '-') {
        p++;
        const char *start = p;
        while (*p && *p != '+') p++;
        size_t len = (size_t)(p - start);
        if (len == 0 || len >= sizeof(out->pre_release)) return -1;
        memcpy(out->pre_release, start, len);
        out->pre_release[len] = '\0';
    }

    /* build metadata */
    if (*p == '+') {
        p++;
        const char *start = p;
        while (*p) p++;
        size_t len = (size_t)(p - start);
        if (len == 0 || len >= sizeof(out->build_meta)) return -1;
        memcpy(out->build_meta, start, len);
        out->build_meta[len] = '\0';
    }

    return (*p == '\0') ? 0 : -1;
}

/* Compare pre-release identifiers per SemVer 2.0 §11 */
static int compare_pre(const char *a, const char *b) {
    int a_empty = (a[0] == '\0');
    int b_empty = (b[0] == '\0');

    /* no pre-release has higher precedence than pre-release */
    if (a_empty && b_empty) return 0;
    if (a_empty) return 1;
    if (b_empty) return -1;

    /* compare dot-separated identifiers */
    while (*a && *b) {
        /* extract one identifier each */
        const char *a_start = a, *b_start = b;
        while (*a && *a != '.') a++;
        while (*b && *b != '.') b++;
        size_t a_len = (size_t)(a - a_start);
        size_t b_len = (size_t)(b - b_start);

        /* check if both are numeric */
        int a_num = 1, b_num = 1;
        for (size_t i = 0; i < a_len; i++)
            if (!isdigit((unsigned char)a_start[i])) { a_num = 0; break; }
        for (size_t i = 0; i < b_len; i++)
            if (!isdigit((unsigned char)b_start[i])) { b_num = 0; break; }

        if (a_num && b_num) {
            long av = strtol(a_start, NULL, 10);
            long bv = strtol(b_start, NULL, 10);
            if (av != bv) return (av < bv) ? -1 : 1;
        } else if (a_num != b_num) {
            /* numeric identifiers have lower precedence */
            return a_num ? -1 : 1;
        } else {
            /* lexicographic compare */
            size_t min_len = a_len < b_len ? a_len : b_len;
            int cmp = memcmp(a_start, b_start, min_len);
            if (cmp != 0) return cmp;
            if (a_len != b_len) return (a_len < b_len) ? -1 : 1;
        }

        if (*a == '.') a++;
        if (*b == '.') b++;
    }

    /* more identifiers = higher precedence */
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

int cookbook_semver_compare(const cookbook_semver *a, const cookbook_semver *b) {
    if (a->major != b->major) return a->major - b->major;
    if (a->minor != b->minor) return a->minor - b->minor;
    if (a->patch != b->patch) return a->patch - b->patch;
    return compare_pre(a->pre_release, b->pre_release);
}

int cookbook_range_parse(const char *str, cookbook_range *out) {
    if (!str || !out) return -1;
    memset(out, 0, sizeof(*out));

    /* skip leading whitespace */
    while (isspace((unsigned char)*str)) str++;

    if (*str == '^') {
        out->type = COOKBOOK_RANGE_CARET;
        return cookbook_semver_parse(str + 1, &out->lower);
    }

    if (*str == '~') {
        out->type = COOKBOOK_RANGE_TILDE;
        return cookbook_semver_parse(str + 1, &out->lower);
    }

    if (*str == '*') {
        out->type = COOKBOOK_RANGE_WILDCARD;
        out->lower.major = -1;
        out->lower.minor = -1;
        return 0;
    }

    /* Maven-style bounded range: [1.0.0,2.0.0) or (1.0.0,2.0.0] */
    if (*str == '[' || *str == '(') {
        out->type = COOKBOOK_RANGE_BOUNDED;
        out->lower_inclusive = (*str == '[');
        str++;

        /* parse lower bound */
        const char *comma = strchr(str, ',');
        if (!comma) return -1;

        char buf[128];
        size_t len = (size_t)(comma - str);
        if (len >= sizeof(buf)) return -1;
        memcpy(buf, str, len);
        buf[len] = '\0';
        if (cookbook_semver_parse(buf, &out->lower) != 0) return -1;

        str = comma + 1;
        while (isspace((unsigned char)*str)) str++;

        /* find closing bracket */
        const char *end = str;
        while (*end && *end != ')' && *end != ']') end++;
        if (!*end) return -1;

        out->upper_inclusive = (*end == ']');

        len = (size_t)(end - str);
        if (len >= sizeof(buf)) return -1;
        memcpy(buf, str, len);
        buf[len] = '\0';
        return cookbook_semver_parse(buf, &out->upper);
    }

    /* wildcard: 1.* or 1.2.* */
    const char *star = strchr(str, '*');
    if (star) {
        out->type = COOKBOOK_RANGE_WILDCARD;
        /* parse partial version before the star */
        if (star == str) {
            out->lower.major = -1;
            out->lower.minor = -1;
        } else {
            /* e.g. "1.*" — parse major */
            out->lower.major = (int)strtol(str, NULL, 10);
            const char *dot = strchr(str, '.');
            if (dot && dot < star && *(dot + 1) != '*') {
                out->lower.minor = (int)strtol(dot + 1, NULL, 10);
            } else {
                out->lower.minor = -1;
            }
        }
        return 0;
    }

    /* exact version */
    out->type = COOKBOOK_RANGE_EXACT;
    return cookbook_semver_parse(str, &out->lower);
}

int cookbook_range_satisfies(const cookbook_range *range,
                             const cookbook_semver *ver) {
    /* pre-release versions only match if the range itself targets
       the same major.minor.patch (SemVer 2.0 range convention) */
    int ver_is_pre = (ver->pre_release[0] != '\0');

    switch (range->type) {
    case COOKBOOK_RANGE_EXACT:
        return cookbook_semver_compare(&range->lower, ver) == 0;

    case COOKBOOK_RANGE_CARET: {
        /* ^1.3.0 means >=1.3.0, <2.0.0
           ^0.3.0 means >=0.3.0, <0.4.0
           ^0.0.3 means >=0.0.3, <0.0.4 */
        if (cookbook_semver_compare(ver, &range->lower) < 0) return 0;
        if (ver_is_pre) {
            if (ver->major != range->lower.major ||
                ver->minor != range->lower.minor ||
                ver->patch != range->lower.patch) return 0;
        }
        if (range->lower.major != 0) {
            return ver->major == range->lower.major;
        } else if (range->lower.minor != 0) {
            return ver->major == 0 && ver->minor == range->lower.minor;
        } else {
            return ver->major == 0 && ver->minor == 0 &&
                   ver->patch == range->lower.patch;
        }
    }

    case COOKBOOK_RANGE_TILDE: {
        /* ~1.3.0 means >=1.3.0, <1.4.0 */
        if (cookbook_semver_compare(ver, &range->lower) < 0) return 0;
        if (ver_is_pre) {
            if (ver->major != range->lower.major ||
                ver->minor != range->lower.minor ||
                ver->patch != range->lower.patch) return 0;
        }
        return ver->major == range->lower.major &&
               ver->minor == range->lower.minor;
    }

    case COOKBOOK_RANGE_WILDCARD: {
        if (ver_is_pre) return 0;
        if (range->lower.major == -1) return 1; /* * matches all */
        if (ver->major != range->lower.major) return 0;
        if (range->lower.minor == -1) return 1; /* 1.* */
        return ver->minor == range->lower.minor;  /* 1.2.* */
    }

    case COOKBOOK_RANGE_BOUNDED: {
        int cmp_lower = cookbook_semver_compare(ver, &range->lower);
        int cmp_upper = cookbook_semver_compare(ver, &range->upper);

        if (range->lower_inclusive) {
            if (cmp_lower < 0) return 0;
        } else {
            if (cmp_lower <= 0) return 0;
        }
        if (range->upper_inclusive) {
            if (cmp_upper > 0) return 0;
        } else {
            if (cmp_upper >= 0) return 0;
        }
        return 1;
    }
    }
    return 0;
}
