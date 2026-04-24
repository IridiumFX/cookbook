/* t4/db/db_storage.c — vtable adapters for kv and dbtree backends. */

#include "apennines/t4/db/db_storage.h"
#include "apennines/t3/db/kv.h"
#include "apennines/t3/db/dbtree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 *  Common iter shape
 *
 *  db_storage_iter is an opaque type; both backends embed their own
 *  state plus a discriminator so iter_next/iter_destroy can dispatch.
 * ================================================================ */

/* For btree, the iterator snapshots all matching (key, value) pairs up
 * front at iter_create time. This matches kv_iter's semantics (which is
 * stable against concurrent writes since kv is append-only) and makes
 * the engine's CREATE-INDEX scan safe against the writes it does inside
 * the loop — without the snapshot, a cursor-based iter gets invalidated
 * by page splits triggered by those same writes. Memory cost = sum of
 * matching (k+v) sizes; acceptable for the engine's use cases, all of
 * which are bounded by table or index size. */
typedef struct bt_entry {
    u8  *k;  u64 kl;
    u8  *v;  u64 vl;
} bt_entry;

struct db_storage_iter {
    u32 kind;           /* 0 = kv, 1 = btree */
    /* kv path */
    kv_iter *kv_it;
    /* btree path: pre-loaded snapshot */
    bt_entry *bt_entries;
    u64       bt_count;
    u64       bt_pos;
};

/* ================================================================
 *  Backend 0 — kv_store
 * ================================================================ */

static unsigned long kv_vt_open(void **handle, const char *path) {
    kv_store *s = NULL;
    unsigned long rc = kv_open(&s, path);
    if (rc) return rc;
    *handle = s;
    return 0;
}
static unsigned long kv_vt_close(void *handle) {
    return kv_close((kv_store *)handle);
}
static unsigned long kv_vt_put(void *handle,
                                const u8 *key, u64 klen,
                                const u8 *val, u64 vlen) {
    return kv_put((kv_store *)handle, key, klen, val, vlen);
}
static unsigned long kv_vt_get(u8 **out, u64 *out_len,
                                void *handle,
                                const u8 *key, u64 klen) {
    return kv_get(out, out_len, (kv_store *)handle, key, klen);
}
static unsigned long kv_vt_del(void *handle, const u8 *key, u64 klen) {
    return kv_delete((kv_store *)handle, key, klen);
}
static unsigned long kv_vt_iter_create(db_storage_iter **out, void *handle,
                                        const u8 *prefix, u64 plen) {
    db_storage_iter *it = (db_storage_iter *)calloc(1, sizeof(db_storage_iter));
    if (!it) return 1;
    it->kind = 0;
    unsigned long rc = kv_iter_create(&it->kv_it, (kv_store *)handle, prefix, plen);
    if (rc) { free(it); return rc; }
    *out = it;
    return 0;
}
static unsigned long kv_vt_compact(void *handle) {
    return kv_compact((kv_store *)handle);
}
static unsigned long kv_vt_flush(void *handle) {
    (void)handle;
    return 0;   /* kv appends + flushes per write; no-op */
}

static const db_storage_vt KV_VT = {
    kv_vt_open, kv_vt_close,
    kv_vt_put, kv_vt_get, kv_vt_del,
    kv_vt_iter_create,
    kv_vt_compact, kv_vt_flush
};

/* ================================================================
 *  Backend 1 — dbtree
 * ================================================================ */

static unsigned long bt_vt_open(void **handle, const char *path) {
    dbtree *bt = NULL;
    unsigned long rc = dbtree_open(&bt, path);
    if (rc) return rc;
    *handle = bt;
    return 0;
}
static unsigned long bt_vt_close(void *handle) {
    return dbtree_close((dbtree *)handle);
}
static unsigned long bt_vt_put(void *handle,
                                const u8 *key, u64 klen,
                                const u8 *val, u64 vlen) {
    return dbtree_put((dbtree *)handle, key, klen, val, vlen);
}
static unsigned long bt_vt_get(u8 **out, u64 *out_len,
                                void *handle,
                                const u8 *key, u64 klen) {
    unsigned long rc = dbtree_get(out, out_len, (dbtree *)handle, key, klen);
    /* kv_get returns 5 for not-found; dbtree_get returns 5 as well. */
    return rc;
}
static unsigned long bt_vt_del(void *handle, const u8 *key, u64 klen) {
    unsigned long rc = dbtree_delete((dbtree *)handle, key, klen);
    /* kv_delete returns 3 for not-found; dbtree returns 3. Match. */
    return rc;
}
static int prefix_matches(const u8 *key, u64 klen, const u8 *pfx, u64 plen) {
    if (plen == 0) return 1;
    if (klen < plen) return 0;
    return memcmp(key, pfx, plen) == 0;
}

static unsigned long bt_vt_iter_create(db_storage_iter **out, void *handle,
                                        const u8 *prefix, u64 plen) {
    db_storage_iter *it = (db_storage_iter *)calloc(1, sizeof(db_storage_iter));
    if (!it) return 1;
    it->kind = 1;

    dbtree_cursor *cur = NULL;
    unsigned long rc;
    if (prefix && plen > 0) {
        rc = dbtree_seek(&cur, (dbtree *)handle, prefix, plen, DBTREE_SEEK_GE);
    } else {
        rc = dbtree_seek(&cur, (dbtree *)handle, NULL, 0, DBTREE_SEEK_FIRST);
    }
    if (rc == 6) { *out = it; return 0; }   /* empty match set */
    if (rc)      { free(it); return rc; }

    /* Pre-load all matching entries. Snapshots the current state so
     * subsequent puts (which may split pages) don't invalidate the iter. */
    u64 cap = 64;
    bt_entry *entries = (bt_entry *)calloc(cap, sizeof(bt_entry));
    if (!entries) { dbtree_cursor_close(cur); free(it); return 2; }
    u64 n = 0;
    for (;;) {
        const u8 *k; u64 kl;
        if (dbtree_cursor_key(&k, &kl, cur) != 0) break;
        if (prefix && plen > 0 && !prefix_matches(k, kl, prefix, plen)) break;
        if (n == cap) {
            u64 nc = cap * 2;
            bt_entry *tmp = (bt_entry *)realloc(entries, nc * sizeof(bt_entry));
            if (!tmp) { dbtree_cursor_close(cur); free(entries); free(it); return 2; }
            memset(tmp + cap, 0, (nc - cap) * sizeof(bt_entry));
            entries = tmp;
            cap = nc;
        }
        /* Own copies */
        u8 *kcp = (u8 *)malloc(kl ? kl : 1);
        if (!kcp) { dbtree_cursor_close(cur); free(entries); free(it); return 2; }
        memcpy(kcp, k, kl);
        u8 *vbuf = NULL; u64 vl = 0;
        if (dbtree_cursor_value(&vbuf, &vl, cur) != 0) { free(kcp); break; }
        entries[n].k = kcp; entries[n].kl = kl;
        entries[n].v = vbuf; entries[n].vl = vl;
        n++;
        if (dbtree_cursor_next(cur) != 0) break;
    }
    dbtree_cursor_close(cur);
    it->bt_entries = entries;
    it->bt_count = n;
    it->bt_pos = 0;
    *out = it;
    return 0;
}

static unsigned long bt_iter_next_impl(const u8 **out_k, u64 *out_kl,
                                       const u8 **out_v, u64 *out_vl,
                                       db_storage_iter *it) {
    if (it->bt_pos >= it->bt_count) return 6;
    bt_entry *e = &it->bt_entries[it->bt_pos++];
    *out_k = e->k;  *out_kl = e->kl;
    *out_v = e->v;  *out_vl = e->vl;
    return 0;
}
static unsigned long bt_vt_compact(void *handle) {
    (void)handle;
    return 0;    /* btree has no append-only log to compact */
}
static unsigned long bt_vt_flush(void *handle) {
    return dbtree_flush((dbtree *)handle);
}

static const db_storage_vt BTREE_VT = {
    bt_vt_open, bt_vt_close,
    bt_vt_put, bt_vt_get, bt_vt_del,
    bt_vt_iter_create,
    bt_vt_compact, bt_vt_flush
};

/* ---- Standalone iter dispatch ---- */

APENNINES_API unsigned long db_storage_iter_next(const u8 **k, u64 *kl,
                                                  const u8 **v, u64 *vl,
                                                  db_storage_iter *it) {
    if (!it) return 1;
    if (it->kind == 0) return kv_iter_next(k, kl, v, vl, it->kv_it);
    return bt_iter_next_impl(k, kl, v, vl, it);
}

APENNINES_API unsigned long db_storage_iter_destroy(db_storage_iter *it) {
    if (!it) return 1;
    if (it->kind == 0) {
        if (it->kv_it) kv_iter_destroy(it->kv_it);
    } else {
        for (u64 i = 0; i < it->bt_count; i++) {
            free(it->bt_entries[i].k);
            free(it->bt_entries[i].v);
        }
        free(it->bt_entries);
    }
    free(it);
    return 0;
}

/* ================================================================
 *  Factory
 * ================================================================ */

APENNINES_API unsigned long db_storage_get_vt(const db_storage_vt **out_vt, u32 backend) {
    if (!out_vt) return 1;
    switch (backend) {
    case DB_STORAGE_HASHKV: *out_vt = &KV_VT;    return 0;
    case DB_STORAGE_BTREE:  *out_vt = &BTREE_VT; return 0;
    default: return 2;
    }
}
