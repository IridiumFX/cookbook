#include "apennines/t2/string/fmt.h"
#include "apennines/t2/string/str.h"
#include "apennines/t1/buffer/buf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static const char hex_chars[] = "0123456789abcdef";

static int hex_digit_val(u8 c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

unsigned long fmt_u64(buf *out, u64 val) {
    char tmp[21];
    int pos;

    if (!out) return 1;

    if (val == 0) {
        return buf_append(out, (u8 *)"0", 1);
    }

    pos = 20;
    tmp[pos] = '\0';

    while (val > 0) {
        pos--;
        tmp[pos] = '0' + (char)(val % 10);
        val /= 10;
    }

    return buf_append(out, (u8 *)(tmp + pos), (u64)(20 - pos));
}

unsigned long fmt_i64(buf *out, i64 val) {
    u64 uval;

    if (!out) return 1;

    if (val < 0) {
        unsigned long rc = buf_append_byte(out, '-');
        if (rc != 0) return rc;
        /* Handle INT64_MIN safely */
        uval = (u64)(-(val + 1)) + 1;
    } else {
        uval = (u64)val;
    }

    return fmt_u64(out, uval);
}

unsigned long fmt_u64_hex(buf *out, u64 val) {
    char tmp[17];
    int pos;

    if (!out) return 1;

    if (val == 0) {
        return buf_append(out, (u8 *)"0", 1);
    }

    pos = 16;
    tmp[pos] = '\0';

    while (val > 0) {
        pos--;
        tmp[pos] = hex_chars[val & 0xf];
        val >>= 4;
    }

    return buf_append(out, (u8 *)(tmp + pos), (u64)(16 - pos));
}

unsigned long fmt_u64_bin(buf *out, u64 val) {
    char tmp[65];
    int pos;

    if (!out) return 1;

    if (val == 0) {
        return buf_append(out, (u8 *)"0", 1);
    }

    pos = 64;
    tmp[pos] = '\0';

    while (val > 0) {
        pos--;
        tmp[pos] = (val & 1) ? '1' : '0';
        val >>= 1;
    }

    return buf_append(out, (u8 *)(tmp + pos), (u64)(64 - pos));
}

unsigned long fmt_f64(buf *out, double val, u32 precision) {
    char tmp[64];
    int n;

    if (!out) return 1;

    n = snprintf(tmp, sizeof(tmp), "%.*f", (int)precision, val);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return 2;

    return buf_append(out, (u8 *)tmp, (u64)n);
}

unsigned long fmt_bool(buf *out, unsigned long val) {
    if (!out) return 1;

    if (val) {
        return buf_append(out, (u8 *)"true", 4);
    } else {
        return buf_append(out, (u8 *)"false", 5);
    }
}

unsigned long fmt_bytes_hex(buf *out, u8 *data, u64 len) {
    u64 i;
    unsigned long rc;

    if (!out) return 1;
    if (!data && len > 0) return 2;

    for (i = 0; i < len; i++) {
        u8 hi = (u8)hex_chars[(data[i] >> 4) & 0xf];
        u8 lo = (u8)hex_chars[data[i] & 0xf];

        rc = buf_append_byte(out, hi);
        if (rc != 0) return rc;
        rc = buf_append_byte(out, lo);
        if (rc != 0) return rc;
    }

    return 0;
}

unsigned long fmt_write(buf *out, const char *fmt, ...) {
    va_list args;
    const char *p;
    unsigned long rc;

    if (!out) return 1;
    if (!fmt) return 2;

    va_start(args, fmt);

    p = fmt;
    while (*p) {
        if (*p == '%') {
            p++;
            switch (*p) {
                case 'd': {
                    i64 v = va_arg(args, i64);
                    rc = fmt_i64(out, v);
                    if (rc != 0) { va_end(args); return rc; }
                    break;
                }
                case 'u': {
                    u64 v = va_arg(args, u64);
                    rc = fmt_u64(out, v);
                    if (rc != 0) { va_end(args); return rc; }
                    break;
                }
                case 'x': {
                    u64 v = va_arg(args, u64);
                    rc = fmt_u64_hex(out, v);
                    if (rc != 0) { va_end(args); return rc; }
                    break;
                }
                case 's': {
                    str *v = va_arg(args, str *);
                    if (v && v->data && v->len > 0) {
                        rc = buf_append(out, v->data, v->len);
                        if (rc != 0) { va_end(args); return rc; }
                    }
                    break;
                }
                case 'f': {
                    double v = va_arg(args, double);
                    rc = fmt_f64(out, v, 6);
                    if (rc != 0) { va_end(args); return rc; }
                    break;
                }
                case '%': {
                    rc = buf_append_byte(out, '%');
                    if (rc != 0) { va_end(args); return rc; }
                    break;
                }
                case '\0': {
                    /* Trailing % at end of format string */
                    va_end(args);
                    return 0;
                }
                default: {
                    /* Unknown specifier, output literally */
                    rc = buf_append_byte(out, '%');
                    if (rc != 0) { va_end(args); return rc; }
                    rc = buf_append_byte(out, (u8)*p);
                    if (rc != 0) { va_end(args); return rc; }
                    break;
                }
            }
            p++;
        } else {
            rc = buf_append_byte(out, (u8)*p);
            if (rc != 0) { va_end(args); return rc; }
            p++;
        }
    }

    va_end(args);
    return 0;
}

unsigned long parse_u64(u64 *out, u8 *data, u64 len) {
    u64 result;
    u64 i;

    if (!out) return 1;
    if (!data) return 2;
    if (len == 0) return 4;

    result = 0;
    for (i = 0; i < len; i++) {
        u8 c = data[i];
        u64 prev;

        if (c < '0' || c > '9') return 4;

        prev = result;
        result = result * 10 + (u64)(c - '0');

        /* Overflow check */
        if (result < prev) return 3;
    }

    *out = result;
    return 0;
}

unsigned long parse_i64(i64 *out, u8 *data, u64 len) {
    u64 uval;
    unsigned long rc;
    int negative;
    u8 *start;
    u64 start_len;

    if (!out) return 1;
    if (!data) return 2;
    if (len == 0) return 4;

    negative = 0;
    start = data;
    start_len = len;

    if (data[0] == '-') {
        negative = 1;
        start = data + 1;
        start_len = len - 1;
        if (start_len == 0) return 4;
    } else if (data[0] == '+') {
        start = data + 1;
        start_len = len - 1;
        if (start_len == 0) return 4;
    }

    rc = parse_u64(&uval, start, start_len);
    if (rc != 0) return rc;

    if (negative) {
        if (uval > (u64)9223372036854775808ULL) return 3;
        if (uval == (u64)9223372036854775808ULL) {
            *out = (i64)(-9223372036854775807LL - 1);
        } else {
            *out = -(i64)uval;
        }
    } else {
        if (uval > (u64)9223372036854775807ULL) return 3;
        *out = (i64)uval;
    }

    return 0;
}

unsigned long parse_u64_hex(u64 *out, u8 *data, u64 len) {
    u64 result;
    u64 i;

    if (!out) return 1;
    if (!data) return 2;
    if (len == 0) return 4;

    result = 0;
    for (i = 0; i < len; i++) {
        int v = hex_digit_val(data[i]);
        u64 prev;

        if (v < 0) return 4;

        prev = result;
        result = (result << 4) | (u64)v;

        /* Overflow: more than 16 hex digits */
        if (i >= 16) return 3;
    }

    *out = result;
    return 0;
}

unsigned long parse_f64(double *out, u8 *data, u64 len) {
    char *tmp;
    char *end;
    double val;

    if (!out) return 1;
    if (!data) return 2;
    if (len == 0) return 3;

    /* Null-terminate for strtod */
    tmp = (char *)malloc((size_t)(len + 1));
    if (!tmp) return 4;

    memcpy(tmp, data, (size_t)len);
    tmp[len] = '\0';

    val = strtod(tmp, &end);

    if (end == tmp || (u64)(end - tmp) != len) {
        free(tmp);
        return 3;
    }

    free(tmp);
    *out = val;

    return 0;
}

unsigned long parse_bool(unsigned long *out, u8 *data, u64 len) {
    if (!out) return 1;
    if (!data) return 2;

    if (len == 4 && memcmp(data, "true", 4) == 0) {
        *out = 1;
        return 0;
    }

    if (len == 5 && memcmp(data, "false", 5) == 0) {
        *out = 0;
        return 0;
    }

    return 3;
}

unsigned long parse_bytes_hex(u8 *out, u64 *out_len, u8 *data, u64 len) {
    u64 i;
    u64 pos;

    if (!out) return 1;
    if (!out_len) return 2;
    if (!data && len > 0) return 2;

    /* Odd length is invalid */
    if (len % 2 != 0) return 3;

    pos = 0;
    for (i = 0; i < len; i += 2) {
        int hi = hex_digit_val(data[i]);
        int lo = hex_digit_val(data[i + 1]);

        if (hi < 0 || lo < 0) return 4;

        out[pos] = (u8)((hi << 4) | lo);
        pos++;
    }

    *out_len = pos;

    return 0;
}
