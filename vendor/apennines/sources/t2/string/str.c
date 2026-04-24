#include "apennines/t2/string/str.h"
#include "apennines/t1/buffer/buf.h"
#include "apennines/t1/buffer/utf8.h"
#include <string.h>
#include <stdlib.h>

static int is_ascii_whitespace(u8 c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static u8 to_lower_byte(u8 c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

static u8 to_upper_byte(u8 c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

unsigned long str_create(str *out, u8 *data, u64 len) {
    u8 *copy;

    if (!out) return 1;
    if (!data) return 2;

    copy = (u8 *)malloc((size_t)len);
    if (!copy) return 3;

    memcpy(copy, data, (size_t)len);
    out->data = copy;
    out->len = len;
    out->owned = 1;

    return 0;
}

unsigned long str_from_cstr(str *out, const char *cstr) {
    u64 len;
    u8 *copy;

    if (!out) return 1;
    if (!cstr) return 2;

    len = (u64)strlen(cstr);
    copy = (u8 *)malloc((size_t)len);
    if (!copy) return 3;

    memcpy(copy, cstr, (size_t)len);
    out->data = copy;
    out->len = len;
    out->owned = 1;

    return 0;
}

unsigned long str_compare(long *result, str *a, str *b) {
    u64 min_len;
    int cmp;

    if (!result) return 1;
    if (!a) return 2;
    if (!b) return 3;

    min_len = a->len < b->len ? a->len : b->len;
    cmp = memcmp(a->data, b->data, (size_t)min_len);

    if (cmp != 0) {
        *result = (long)cmp;
    } else if (a->len < b->len) {
        *result = -1;
    } else if (a->len > b->len) {
        *result = 1;
    } else {
        *result = 0;
    }

    return 0;
}

unsigned long str_compare_insensitive(long *result, str *a, str *b) {
    u64 min_len;
    u64 i;

    if (!result) return 1;
    if (!a) return 2;
    if (!b) return 3;

    min_len = a->len < b->len ? a->len : b->len;

    for (i = 0; i < min_len; i++) {
        u8 ca = to_lower_byte(a->data[i]);
        u8 cb = to_lower_byte(b->data[i]);
        if (ca < cb) { *result = -1; return 0; }
        if (ca > cb) { *result = 1; return 0; }
    }

    if (a->len < b->len) {
        *result = -1;
    } else if (a->len > b->len) {
        *result = 1;
    } else {
        *result = 0;
    }

    return 0;
}

unsigned long str_find(u64 *offset, str *haystack, str *needle) {
    u64 i;

    if (!offset) return 1;
    if (!haystack) return 2;
    if (!needle) return 3;

    if (needle->len == 0) {
        *offset = 0;
        return 0;
    }

    if (needle->len > haystack->len) return 4;

    for (i = 0; i <= haystack->len - needle->len; i++) {
        if (memcmp(haystack->data + i, needle->data, (size_t)needle->len) == 0) {
            *offset = i;
            return 0;
        }
    }

    return 4;
}

unsigned long str_rfind(u64 *offset, str *haystack, str *needle) {
    u64 i;

    if (!offset) return 1;
    if (!haystack) return 2;
    if (!needle) return 3;

    if (needle->len == 0) {
        *offset = haystack->len;
        return 0;
    }

    if (needle->len > haystack->len) return 4;

    for (i = haystack->len - needle->len + 1; i > 0; i--) {
        if (memcmp(haystack->data + (i - 1), needle->data, (size_t)needle->len) == 0) {
            *offset = i - 1;
            return 0;
        }
    }

    return 4;
}

unsigned long str_starts_with(unsigned long *result, str *s, str *prefix) {
    if (!result) return 1;
    if (!s) return 2;
    if (!prefix) return 3;

    if (prefix->len > s->len) {
        *result = 0;
    } else {
        *result = (memcmp(s->data, prefix->data, (size_t)prefix->len) == 0) ? 1 : 0;
    }

    return 0;
}

unsigned long str_ends_with(unsigned long *result, str *s, str *suffix) {
    if (!result) return 1;
    if (!s) return 2;
    if (!suffix) return 3;

    if (suffix->len > s->len) {
        *result = 0;
    } else {
        *result = (memcmp(s->data + s->len - suffix->len, suffix->data, (size_t)suffix->len) == 0) ? 1 : 0;
    }

    return 0;
}

unsigned long str_contains(unsigned long *result, str *s, str *needle) {
    u64 offset;
    unsigned long rc;

    if (!result) return 1;
    if (!s) return 2;
    if (!needle) return 3;

    rc = str_find(&offset, s, needle);
    *result = (rc == 0) ? 1 : 0;

    return 0;
}

unsigned long str_split(str **out_parts, u64 *out_count, str *s, str *delimiter) {
    u64 count;
    u64 start;
    u64 i;
    str *parts;

    if (!out_parts) return 1;
    if (!out_count) return 2;
    if (!s) return 3;
    if (!delimiter) return 4;

    /* First pass: count parts */
    count = 1;
    if (delimiter->len > 0 && s->len > 0) {
        for (i = 0; i <= s->len - delimiter->len; i++) {
            if (memcmp(s->data + i, delimiter->data, (size_t)delimiter->len) == 0) {
                count++;
                i += delimiter->len - 1;
            }
        }
    }

    parts = (str *)malloc(count * sizeof(str));
    if (!parts) return 5;

    /* Second pass: fill parts as views */
    start = 0;
    count = 0;

    if (delimiter->len == 0) {
        parts[0].data = s->data;
        parts[0].len = s->len;
        parts[0].owned = 0;
        count = 1;
    } else {
        for (i = 0; i <= s->len; i++) {
            if (i == s->len || (i + delimiter->len <= s->len &&
                memcmp(s->data + i, delimiter->data, (size_t)delimiter->len) == 0)) {
                parts[count].data = s->data + start;
                parts[count].len = i - start;
                parts[count].owned = 0;
                count++;
                if (i < s->len) {
                    i += delimiter->len - 1;
                    start = i + 1;
                }
            }
        }
    }

    *out_parts = parts;
    *out_count = count;

    return 0;
}

unsigned long str_join(str *out, str *parts, u64 count, str *separator) {
    u64 total;
    u64 i;
    u64 pos;
    u8 *data;

    if (!out) return 1;
    if (!parts && count > 0) return 2;
    if (!separator) return 3;

    if (count == 0) {
        out->data = (u8 *)malloc(0);
        out->len = 0;
        out->owned = 1;
        return 0;
    }

    total = 0;
    for (i = 0; i < count; i++) {
        total += parts[i].len;
        if (i > 0) total += separator->len;
    }

    data = (u8 *)malloc((size_t)total);
    if (!data) return 4;

    pos = 0;
    for (i = 0; i < count; i++) {
        if (i > 0 && separator->len > 0) {
            memcpy(data + pos, separator->data, (size_t)separator->len);
            pos += separator->len;
        }
        if (parts[i].len > 0) {
            memcpy(data + pos, parts[i].data, (size_t)parts[i].len);
            pos += parts[i].len;
        }
    }

    out->data = data;
    out->len = total;
    out->owned = 1;

    return 0;
}

unsigned long str_trim(str *out, str *s) {
    u64 start;
    u64 end;

    if (!out) return 1;
    if (!s) return 2;

    start = 0;
    end = s->len;

    while (start < end && is_ascii_whitespace(s->data[start])) start++;
    while (end > start && is_ascii_whitespace(s->data[end - 1])) end--;

    out->data = s->data + start;
    out->len = end - start;
    out->owned = 0;

    return 0;
}

unsigned long str_trim_left(str *out, str *s) {
    u64 start;

    if (!out) return 1;
    if (!s) return 2;

    start = 0;
    while (start < s->len && is_ascii_whitespace(s->data[start])) start++;

    out->data = s->data + start;
    out->len = s->len - start;
    out->owned = 0;

    return 0;
}

unsigned long str_trim_right(str *out, str *s) {
    u64 end;

    if (!out) return 1;
    if (!s) return 2;

    end = s->len;
    while (end > 0 && is_ascii_whitespace(s->data[end - 1])) end--;

    out->data = s->data;
    out->len = end;
    out->owned = 0;

    return 0;
}

unsigned long str_to_upper(str *out, str *s) {
    u8 *copy;
    u64 i;

    if (!out) return 1;
    if (!s) return 2;

    copy = (u8 *)malloc((size_t)s->len);
    if (!copy && s->len > 0) return 3;

    for (i = 0; i < s->len; i++) {
        copy[i] = to_upper_byte(s->data[i]);
    }

    out->data = copy;
    out->len = s->len;
    out->owned = 1;

    return 0;
}

unsigned long str_to_lower(str *out, str *s) {
    u8 *copy;
    u64 i;

    if (!out) return 1;
    if (!s) return 2;

    copy = (u8 *)malloc((size_t)s->len);
    if (!copy && s->len > 0) return 3;

    for (i = 0; i < s->len; i++) {
        copy[i] = to_lower_byte(s->data[i]);
    }

    out->data = copy;
    out->len = s->len;
    out->owned = 1;

    return 0;
}

unsigned long str_repeat(str *out, str *s, u64 n) {
    u64 total;
    u8 *data;
    u64 i;

    if (!out) return 1;
    if (!s) return 2;

    total = s->len * n;

    data = (u8 *)malloc((size_t)total);
    if (!data && total > 0) return 3;

    for (i = 0; i < n; i++) {
        memcpy(data + i * s->len, s->data, (size_t)s->len);
    }

    out->data = data;
    out->len = total;
    out->owned = 1;

    return 0;
}

unsigned long str_replace(str *out, str *s, str *old_val, str *new_val) {
    u64 pos;
    unsigned long rc;
    u64 total;
    u8 *data;

    if (!out) return 1;
    if (!s) return 2;
    if (!old_val) return 3;
    if (!new_val) return 4;

    rc = str_find(&pos, s, old_val);
    if (rc != 0) {
        return str_clone(out, s);
    }

    total = s->len - old_val->len + new_val->len;
    data = (u8 *)malloc((size_t)total);
    if (!data && total > 0) return 5;

    if (pos > 0) {
        memcpy(data, s->data, (size_t)pos);
    }
    if (new_val->len > 0) {
        memcpy(data + pos, new_val->data, (size_t)new_val->len);
    }
    if (pos + old_val->len < s->len) {
        memcpy(data + pos + new_val->len,
               s->data + pos + old_val->len,
               (size_t)(s->len - pos - old_val->len));
    }

    out->data = data;
    out->len = total;
    out->owned = 1;

    return 0;
}

unsigned long str_replace_all(str *out, str *s, str *old_val, str *new_val) {
    u64 count;
    u64 i;
    u64 total;
    u8 *data;
    u64 src_pos;
    u64 dst_pos;

    if (!out) return 1;
    if (!s) return 2;
    if (!old_val) return 3;
    if (!new_val) return 4;

    if (old_val->len == 0) {
        return str_clone(out, s);
    }

    /* Count occurrences */
    count = 0;
    for (i = 0; i <= s->len - old_val->len; ) {
        if (memcmp(s->data + i, old_val->data, (size_t)old_val->len) == 0) {
            count++;
            i += old_val->len;
        } else {
            i++;
        }
        if (i + old_val->len > s->len + 1) break;
    }

    if (count == 0) {
        return str_clone(out, s);
    }

    total = s->len - count * old_val->len + count * new_val->len;
    data = (u8 *)malloc((size_t)total);
    if (!data && total > 0) return 5;

    src_pos = 0;
    dst_pos = 0;
    while (src_pos <= s->len) {
        if (src_pos + old_val->len <= s->len &&
            memcmp(s->data + src_pos, old_val->data, (size_t)old_val->len) == 0) {
            if (new_val->len > 0) {
                memcpy(data + dst_pos, new_val->data, (size_t)new_val->len);
                dst_pos += new_val->len;
            }
            src_pos += old_val->len;
        } else {
            if (src_pos < s->len) {
                data[dst_pos] = s->data[src_pos];
                dst_pos++;
            }
            src_pos++;
        }
    }

    out->data = data;
    out->len = total;
    out->owned = 1;

    return 0;
}

unsigned long str_substring(str *out, str *s, u64 offset, u64 len) {
    if (!out) return 1;
    if (!s) return 2;
    if (offset + len > s->len) return 3;

    out->data = s->data + offset;
    out->len = len;
    out->owned = 0;

    return 0;
}

unsigned long str_hash(u64 *out, str *s) {
    u64 hash;
    u64 i;

    if (!out) return 1;
    if (!s) return 2;

    /* FNV-1a 64-bit */
    hash = 14695981039346656037ULL;

    for (i = 0; i < s->len; i++) {
        hash ^= (u64)s->data[i];
        hash *= 1099511628211ULL;
    }

    *out = hash;

    return 0;
}

unsigned long str_clone(str *out, str *s) {
    u8 *copy;

    if (!out) return 1;
    if (!s) return 2;

    copy = (u8 *)malloc((size_t)s->len);
    if (!copy && s->len > 0) return 3;

    if (s->len > 0) {
        memcpy(copy, s->data, (size_t)s->len);
    }

    out->data = copy;
    out->len = s->len;
    out->owned = 1;

    return 0;
}

unsigned long str_destroy(str *s) {
    if (!s) return 1;

    if (s->owned && s->data) {
        free(s->data);
    }

    s->data = NULL;
    s->len = 0;
    s->owned = 0;

    return 0;
}

unsigned long str_len(u64 *out, str *s) {
    if (!out) return 1;
    if (!s) return 2;

    *out = s->len;

    return 0;
}

unsigned long str_codepoints(u64 *out, str *s) {
    if (!out) return 1;
    if (!s) return 2;

    return utf8_codepoint_count(out, s->data, s->len);
}
