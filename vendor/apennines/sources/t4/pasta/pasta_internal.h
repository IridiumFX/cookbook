#ifndef APENNINES_T4_PASTA_INTERNAL_H
#define APENNINES_T4_PASTA_INTERNAL_H

#include "apennines/t4/pasta/pasta.h"

/* Number literal format hints, for writer roundtrip fidelity. */
#define PASTA_NUM_FMT_DEC  0   /* plain decimal */
#define PASTA_NUM_FMT_HEX  1   /* 0xFF */
#define PASTA_NUM_FMT_BIN  2   /* 0b1010 */

struct pasta_value {
    pasta_kind kind;
    u8 num_fmt;                /* one of PASTA_NUM_FMT_* (INTEGER only) */
    union {
        int b;
        i64 integer;
        f64 number;
        struct { u8  *data;  u64 len; } s;        /* STRING */
        struct { u8  *data;  u64 len; } blob;     /* BLOB */
        struct { u8  *name;  u64 len; } label;    /* LABEL_REF */
        struct {
            pasta_value **items;
            u64 count;
            u64 cap;
        } array;
        struct {
            u8  **keys;
            u64 *key_lens;
            pasta_value **vals;
            u64 count;
            u64 cap;
        } map;
    } u;
};

/* Helpers shared across pasta_*.c files. */
unsigned long pasta__map_reserve(pasta_value *m, u64 need);
unsigned long pasta__array_reserve(pasta_value *a, u64 need);

#endif /* APENNINES_T4_PASTA_INTERNAL_H */
