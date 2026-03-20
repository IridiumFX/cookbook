#ifndef APENNINES_T3_KV_H
#define APENNINES_T3_KV_H

#include "apennines/export.h"
#include "apennines/types.h"

/* ================================================================
 *  KV — embedded key-value store
 *
 *  Append-only log + in-memory index (hash table).
 *  Supports iteration, batch writes, snapshots, and compaction.
 * ================================================================ */

typedef struct kv_store kv_store;
typedef struct kv_iter  kv_iter;
typedef struct kv_batch kv_batch;

/* ---- Core ---- */

/* Hatches: 1=null out, 2=null path, 3=open failed, 4=alloc failure,
 *          5=corrupt data file */
APENNINES_API unsigned long kv_open(kv_store **out, const char *path);

/* Hatches: 1=null out, 2=null out_len, 3=null store, 4=null key,
 *          5=not found */
APENNINES_API unsigned long kv_get(u8 **out, u64 *out_len,
                                    kv_store *store,
                                    const u8 *key, u64 key_len);

/* Hatches: 1=null store, 2=null key, 3=null value when val_len>0,
 *          4=write failed */
APENNINES_API unsigned long kv_put(kv_store *store,
                                    const u8 *key, u64 key_len,
                                    const u8 *value, u64 val_len);

/* Hatches: 1=null store, 2=null key, 3=not found */
APENNINES_API unsigned long kv_delete(kv_store *store,
                                       const u8 *key, u64 key_len);

/* Hatches: 1=null store, 2=close failed */
APENNINES_API unsigned long kv_close(kv_store *store);

/* ---- Iterator ---- */

/* Create iterator. If prefix is non-NULL, only keys starting with prefix
 * are returned. prefix=NULL iterates all keys.
 * Hatches: 1=null out, 2=null store, 3=alloc failure */
APENNINES_API unsigned long kv_iter_create(kv_iter **out, kv_store *store,
                                            const u8 *prefix, u64 prefix_len);

/* Hatches: 1=null out_key, 2=null out_key_len, 3=null out_val,
 *          4=null out_val_len, 5=null it, 6=end of iteration */
APENNINES_API unsigned long kv_iter_next(const u8 **out_key, u64 *out_key_len,
                                          const u8 **out_val, u64 *out_val_len,
                                          kv_iter *it);

/* Hatches: 1=null it */
APENNINES_API unsigned long kv_iter_destroy(kv_iter *it);

/* ---- Batch ---- */

/* Hatches: 1=null out, 2=alloc failure */
APENNINES_API unsigned long kv_batch_create(kv_batch **out);

/* Hatches: 1=null batch, 2=null key, 3=alloc failure */
APENNINES_API unsigned long kv_batch_put(kv_batch *batch,
                                          const u8 *key, u64 key_len,
                                          const u8 *value, u64 val_len);

/* Hatches: 1=null batch, 2=null key, 3=alloc failure */
APENNINES_API unsigned long kv_batch_delete(kv_batch *batch,
                                             const u8 *key, u64 key_len);

/* Hatches: 1=null store, 2=null batch, 3=write failed */
APENNINES_API unsigned long kv_batch_commit(kv_store *store, kv_batch *batch);

/* Hatches: 1=null batch */
APENNINES_API unsigned long kv_batch_destroy(kv_batch *batch);

/* ---- Maintenance ---- */

/* Rewrite data file, removing deleted/overwritten entries.
 * Hatches: 1=null store, 2=compaction failed */
APENNINES_API unsigned long kv_compact(kv_store *store);

/* Create a read snapshot (point-in-time consistent view).
 * Hatches: 1=null out, 2=null store, 3=alloc failure */
APENNINES_API unsigned long kv_snapshot(kv_store **out, kv_store *store);

#endif /* APENNINES_T3_KV_H */
