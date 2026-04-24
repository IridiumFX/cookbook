#include "apennines/t1/buffer/utf8.h"

unsigned long utf8_len_from_byte(u64 *out, u8 lead) {
    if (!out) return 1;
    if ((lead & 0x80) == 0x00) {
        *out = 1;
    } else if ((lead & 0xE0) == 0xC0) {
        *out = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        *out = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        *out = 4;
    } else {
        return 2;
    }
    return 0;
}

unsigned long utf8_decode_one(u32 *codepoint, u64 *advance, u8 *data, u64 len) {
    if (!codepoint) return 1;
    if (!advance) return 2;
    if (!data) return 3;
    if (len == 0) return 3;

    u8 lead = data[0];
    u64 seq_len;
    u32 cp;

    if ((lead & 0x80) == 0x00) {
        seq_len = 1;
        cp = lead;
    } else if ((lead & 0xE0) == 0xC0) {
        seq_len = 2;
        cp = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        seq_len = 3;
        cp = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        seq_len = 4;
        cp = lead & 0x07;
    } else {
        return 4;
    }

    if (seq_len > len) return 5;

    for (u64 i = 1; i < seq_len; i++) {
        if ((data[i] & 0xC0) != 0x80) return 6;
        cp = (cp << 6) | (data[i] & 0x3F);
    }

    /* Reject overlong encodings */
    if (seq_len == 2 && cp < 0x80) return 6;
    if (seq_len == 3 && cp < 0x800) return 6;
    if (seq_len == 4 && cp < 0x10000) return 6;

    /* Reject surrogates and out-of-range */
    if (cp >= 0xD800 && cp <= 0xDFFF) return 6;
    if (cp > 0x10FFFF) return 6;

    *codepoint = cp;
    *advance = seq_len;
    return 0;
}

unsigned long utf8_encode_one(u64 *bytes_written, u8 *out, u64 out_cap, u32 codepoint) {
    if (!bytes_written) return 1;
    if (!out) return 2;

    /* Reject surrogates and out-of-range */
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return 3;
    if (codepoint > 0x10FFFF) return 3;

    if (codepoint <= 0x7F) {
        if (out_cap < 1) return 4;
        out[0] = (u8)codepoint;
        *bytes_written = 1;
    } else if (codepoint <= 0x7FF) {
        if (out_cap < 2) return 4;
        out[0] = (u8)(0xC0 | (codepoint >> 6));
        out[1] = (u8)(0x80 | (codepoint & 0x3F));
        *bytes_written = 2;
    } else if (codepoint <= 0xFFFF) {
        if (out_cap < 3) return 4;
        out[0] = (u8)(0xE0 | (codepoint >> 12));
        out[1] = (u8)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (u8)(0x80 | (codepoint & 0x3F));
        *bytes_written = 3;
    } else {
        if (out_cap < 4) return 4;
        out[0] = (u8)(0xF0 | (codepoint >> 18));
        out[1] = (u8)(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = (u8)(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = (u8)(0x80 | (codepoint & 0x3F));
        *bytes_written = 4;
    }
    return 0;
}

unsigned long utf8_validate(u64 *invalid_offset, u8 *data, u64 len) {
    if (!invalid_offset) return 1;
    if (!data) return 2;

    u64 i = 0;
    while (i < len) {
        u8 lead = data[i];
        u64 seq_len;
        u32 cp;

        if ((lead & 0x80) == 0x00) {
            seq_len = 1;
            cp = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            seq_len = 2;
            cp = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            seq_len = 3;
            cp = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            seq_len = 4;
            cp = lead & 0x07;
        } else {
            *invalid_offset = i;
            return 3;
        }

        if (i + seq_len > len) {
            *invalid_offset = i;
            return 3;
        }

        for (u64 j = 1; j < seq_len; j++) {
            if ((data[i + j] & 0xC0) != 0x80) {
                *invalid_offset = i;
                return 3;
            }
            cp = (cp << 6) | (data[i + j] & 0x3F);
        }

        /* Reject overlong encodings */
        if (seq_len == 2 && cp < 0x80) { *invalid_offset = i; return 3; }
        if (seq_len == 3 && cp < 0x800) { *invalid_offset = i; return 3; }
        if (seq_len == 4 && cp < 0x10000) { *invalid_offset = i; return 3; }

        /* Reject surrogates and out-of-range */
        if (cp >= 0xD800 && cp <= 0xDFFF) { *invalid_offset = i; return 3; }
        if (cp > 0x10FFFF) { *invalid_offset = i; return 3; }

        i += seq_len;
    }

    *invalid_offset = len;
    return 0;
}

unsigned long utf8_codepoint_count(u64 *count, u8 *data, u64 len) {
    if (!count) return 1;
    if (!data) return 2;

    u64 n = 0;
    u64 i = 0;
    while (i < len) {
        u8 lead = data[i];
        u64 seq_len;

        if ((lead & 0x80) == 0x00) {
            seq_len = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            seq_len = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            seq_len = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            seq_len = 4;
        } else {
            return 3;
        }

        if (i + seq_len > len) return 3;

        for (u64 j = 1; j < seq_len; j++) {
            if ((data[i + j] & 0xC0) != 0x80) return 3;
        }

        n++;
        i += seq_len;
    }

    *count = n;
    return 0;
}
