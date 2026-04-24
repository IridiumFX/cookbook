#ifndef APENNINES_T4_DB_STORAGE_H
#define APENNINES_T4_DB_STORAGE_H

#include "apennines/export.h"
#include "apennines/types.h"

/* ================================================================
 *  db_storage — storage-layer vtable for t4/db/database
 *
 *  The engine speaks to the storage layer through this abstraction
 *  so it can be backed by either the hash-indexed append-only log
 *  (t3/db/kv) or the B+-tree (t3/db/dbtree). Same shape, different
 *  ordering + iteration guarantees.
 *
 *  Iterator contract:
 *    - iter_create(handle, prefix, plen): iterator yielding every
 *      key whose bytes begin with `prefix` (prefix=NULL/plen=0 yields
 *      every key). Order is unspecified for kv, lexicographic for
 *      dbtree. Engine code does not currently depend on order within
 *      a prefix, so both backends are valid for all call sites.
 *    - iter_next returns borrowed pointers valid only until the next
 *      iter_next / iter_destroy.
 * ================================================================ */

typedef struct db_storage_iter db_storage_iter;

typedef struct db_storage_vt {
    unsigned long (*open)       (void **handle, const char *path);
    unsigned long (*close)      (void *handle);

    unsigned long (*put)        (void *handle,
                                  const u8 *key, u64 key_len,
                                  const u8 *val, u64 val_len);
    unsigned long (*get)        (u8 **out, u64 *out_len,
                                  void *handle,
                                  const u8 *key, u64 key_len);
    unsigned long (*del)        (void *handle,
                                  const u8 *key, u64 key_len);

    unsigned long (*iter_create) (db_storage_iter **out, void *handle,
                                   const u8 *prefix, u64 prefix_len);

    unsigned long (*compact)    (void *handle);
    unsigned long (*flush)      (void *handle);
} db_storage_vt;

/* Standalone iter_next / iter_destroy dispatch on the iter's internal
 * discriminator, so call sites don't need the db_conn to iterate. */
APENNINES_API unsigned long db_storage_iter_next(const u8 **k, u64 *kl,
                                                  const u8 **v, u64 *vl,
                                                  db_storage_iter *it);
APENNINES_API unsigned long db_storage_iter_destroy(db_storage_iter *it);

/* Backend selection flags (passed to db_open_ex). */
#define DB_STORAGE_HASHKV  0u        /* default: t3/db/kv (append-only log) */
#define DB_STORAGE_BTREE   1u        /* t3/db/dbtree (B+-tree) */

/* Factory: return a vtable for the requested backend.
 * Hatches: 1=null out_vt, 2=unknown backend */
APENNINES_API unsigned long db_storage_get_vt(const db_storage_vt **out_vt, u32 backend);

#endif /* APENNINES_T4_DB_STORAGE_H */
