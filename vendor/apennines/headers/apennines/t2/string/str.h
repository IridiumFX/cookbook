#ifndef APENNINES_T2_STR_H
#define APENNINES_T2_STR_H

#include "apennines/export.h"
#include "apennines/types.h"

typedef struct {
    u8 *data;
    u64 len;
    u8 owned;
} str;

APENNINES_API unsigned long str_create(str *out, u8 *data, u64 len);
APENNINES_API unsigned long str_from_cstr(str *out, const char *cstr);
APENNINES_API unsigned long str_compare(long *result, str *a, str *b);
APENNINES_API unsigned long str_compare_insensitive(long *result, str *a, str *b);
APENNINES_API unsigned long str_find(u64 *offset, str *haystack, str *needle);
APENNINES_API unsigned long str_rfind(u64 *offset, str *haystack, str *needle);
APENNINES_API unsigned long str_starts_with(unsigned long *result, str *s, str *prefix);
APENNINES_API unsigned long str_ends_with(unsigned long *result, str *s, str *suffix);
APENNINES_API unsigned long str_contains(unsigned long *result, str *s, str *needle);
APENNINES_API unsigned long str_split(str **out_parts, u64 *out_count, str *s, str *delimiter);
APENNINES_API unsigned long str_join(str *out, str *parts, u64 count, str *separator);
APENNINES_API unsigned long str_trim(str *out, str *s);
APENNINES_API unsigned long str_trim_left(str *out, str *s);
APENNINES_API unsigned long str_trim_right(str *out, str *s);
APENNINES_API unsigned long str_to_upper(str *out, str *s);
APENNINES_API unsigned long str_to_lower(str *out, str *s);
APENNINES_API unsigned long str_repeat(str *out, str *s, u64 n);
APENNINES_API unsigned long str_replace(str *out, str *s, str *old_val, str *new_val);
APENNINES_API unsigned long str_replace_all(str *out, str *s, str *old_val, str *new_val);
APENNINES_API unsigned long str_substring(str *out, str *s, u64 offset, u64 len);
APENNINES_API unsigned long str_hash(u64 *out, str *s);
APENNINES_API unsigned long str_clone(str *out, str *s);
APENNINES_API unsigned long str_destroy(str *s);
APENNINES_API unsigned long str_len(u64 *out, str *s);
APENNINES_API unsigned long str_codepoints(u64 *out, str *s);

#endif /* APENNINES_T2_STR_H */
