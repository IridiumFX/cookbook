#ifndef APENNINES_T3_BTREE_H
#define APENNINES_T3_BTREE_H

#include "apennines/export.h"
#include "apennines/types.h"

/* ================================================================
 *  DBTREE — disk-backed B+-tree
 *
 *  4 KB pages, slotted layout, binary search within pages, LRU page
 *  cache. Leaves chained for ordered iteration. Values too large for
 *  a single page spill onto a chain of overflow pages.
 *
 *  Keys are compared lexicographically (memcmp, with length tiebreak).
 *  Engine-level code encodes numeric keys in big-endian so they sort
 *  correctly under this ordering.
 * ================================================================ */

typedef struct dbtree         dbtree;
typedef struct dbtree_cursor  dbtree_cursor;

/* Seek modes for dbtree_seek. */
typedef enum {
    DBTREE_SEEK_EQ    = 0,    /* exact match; cursor invalid if absent */
    DBTREE_SEEK_GE    = 1,    /* first key >= target */
    DBTREE_SEEK_GT    = 2,    /* first key >  target */
    DBTREE_SEEK_LE    = 3,    /* last  key <= target */
    DBTREE_SEEK_LT    = 4,    /* last  key <  target */
    DBTREE_SEEK_FIRST = 5,    /* first key in tree (key/klen ignored) */
    DBTREE_SEEK_LAST  = 6     /* last  key in tree (key/klen ignored) */
} dbtree_seek_mode;

/* ---- Core ---- */

/* dbtree_open — open or create a dbtree at the given path.
 *   out:   receives handle
 *   path:  file path (null-terminated)
 *
 * Hatches: 1=null out, 2=null path, 3=open/create failed,
 *          4=alloc failure, 5=corrupt file header */
APENNINES_API unsigned long dbtree_open(dbtree **out, const char *path);

/* dbtree_close — flush any dirty pages and close.
 *
 * Hatches: 1=null bt, 2=flush failed */
APENNINES_API unsigned long dbtree_close(dbtree *bt);

/* dbtree_put — insert or replace a key/value pair. Values larger than a
 * page spill onto an overflow chain automatically.
 *
 * Hatches: 1=null bt, 2=null key, 3=null value when val_len>0,
 *          4=alloc failure, 5=write failed */
APENNINES_API unsigned long dbtree_put(dbtree *bt,
                                       const u8 *key, u64 key_len,
                                       const u8 *value, u64 val_len);

/* dbtree_get — retrieve the value for a key. Caller frees *out.
 *
 * Hatches: 1=null out, 2=null out_len, 3=null bt, 4=null key,
 *          5=not found, 6=alloc failure, 7=read failed */
APENNINES_API unsigned long dbtree_get(u8 **out, u64 *out_len,
                                       dbtree *bt,
                                       const u8 *key, u64 key_len);

/* dbtree_delete — remove a key.
 *
 * Hatches: 1=null bt, 2=null key, 3=not found, 4=write failed */
APENNINES_API unsigned long dbtree_delete(dbtree *bt,
                                          const u8 *key, u64 key_len);

/* dbtree_flush — write any dirty pages to the backing file (no fsync).
 *
 * Hatches: 1=null bt, 2=write failed */
APENNINES_API unsigned long dbtree_flush(dbtree *bt);

/* dbtree_sync — flush + fsync the backing file.
 *
 * Hatches: 1=null bt, 2=flush failed, 3=sync failed */
APENNINES_API unsigned long dbtree_sync(dbtree *bt);

/* ---- Cursor / iteration ---- */

/* dbtree_seek — position a new cursor per mode.
 * The cursor holds a reference to the dbtree until closed. Modes
 * FIRST/LAST ignore key/klen.
 *
 * Hatches: 1=null out, 2=null bt, 3=invalid mode,
 *          4=null key when mode needs a target, 5=alloc failure,
 *          6=tree empty / no matching key for mode */
APENNINES_API unsigned long dbtree_seek(dbtree_cursor **out, dbtree *bt,
                                        const u8 *key, u64 key_len,
                                        dbtree_seek_mode mode);

/* dbtree_cursor_next — advance to the next key in ascending order.
 *
 * Hatches: 1=null c, 2=past end of tree */
APENNINES_API unsigned long dbtree_cursor_next(dbtree_cursor *c);

/* dbtree_cursor_prev — step back one key (descending).
 *
 * Hatches: 1=null c, 2=before start of tree */
APENNINES_API unsigned long dbtree_cursor_prev(dbtree_cursor *c);

/* dbtree_cursor_key — current key (valid until next/prev/close).
 *
 * Hatches: 1=null out, 2=null out_len, 3=null c, 4=cursor invalid */
APENNINES_API unsigned long dbtree_cursor_key(const u8 **out, u64 *out_len,
                                              dbtree_cursor *c);

/* dbtree_cursor_value — current value. Reads overflow chain if needed,
 * materialises into a caller-owned buffer (caller frees *out).
 *
 * Hatches: 1=null out, 2=null out_len, 3=null c, 4=cursor invalid,
 *          5=alloc failure, 6=read failed */
APENNINES_API unsigned long dbtree_cursor_value(u8 **out, u64 *out_len,
                                                dbtree_cursor *c);

/* dbtree_cursor_close.
 *
 * Hatches: 1=null c */
APENNINES_API unsigned long dbtree_cursor_close(dbtree_cursor *c);

/* ---- Diagnostics / tunables ---- */

/* dbtree_cache_pages — set the page-cache capacity (in pages, default 64).
 * Must be called immediately after dbtree_open and before any other op.
 *
 * Hatches: 1=null bt, 2=invalid n, 3=alloc failure */
APENNINES_API unsigned long dbtree_cache_pages(dbtree *bt, u32 n);

/* dbtree_stats — retrieve usage counters (any pointer may be NULL to skip).
 *
 * Hatches: 1=null bt */
APENNINES_API unsigned long dbtree_stats(dbtree *bt,
                                         u64 *out_num_pages,
                                         u64 *out_num_keys,
                                         u64 *out_cache_hits,
                                         u64 *out_cache_misses);

#endif /* APENNINES_T3_BTREE_H */
