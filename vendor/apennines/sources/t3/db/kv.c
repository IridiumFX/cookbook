#include "apennines/t3/db/kv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define KV_BUCKET_COUNT  1024
#define KV_TYPE_PUT      0
#define KV_TYPE_DELETE   1
#define KV_BATCH_INIT    16

/* ------------------------------------------------------------------ */
/*  Internal structs                                                   */
/* ------------------------------------------------------------------ */

typedef struct kv_entry {
    u8  *key;
    u64  key_len;
    u64  file_offset;   /* offset in data file where value starts */
    u64  val_len;
    int  deleted;       /* 1 if tombstone */
    struct kv_entry *next;
} kv_entry;

struct kv_store {
    FILE      *fp;
    char       path[512];
    kv_entry **buckets;
    u64        bucket_count;
    u64        entry_count;
    int        is_snapshot;
};

struct kv_iter {
    kv_store   *store;
    u64         bucket_idx;
    kv_entry   *current;
    u8         *prefix;
    u64         prefix_len;
    u8         *val_buf;
    u64         val_buf_cap;
};

typedef struct {
    int  type;
    u8  *key;
    u64  key_len;
    u8  *val;
    u64  val_len;
} kv_op;

struct kv_batch {
    kv_op *ops;
    u64    count;
    u64    cap;
};

/* ------------------------------------------------------------------ */
/*  Little-endian helpers                                              */
/* ------------------------------------------------------------------ */

static void write_u32_le(u8 *dst, u32 v) {
    dst[0] = (u8)(v);
    dst[1] = (u8)(v >> 8);
    dst[2] = (u8)(v >> 16);
    dst[3] = (u8)(v >> 24);
}

static u32 read_u32_le(const u8 *src) {
    return (u32)src[0]
         | ((u32)src[1] << 8)
         | ((u32)src[2] << 16)
         | ((u32)src[3] << 24);
}

/* ------------------------------------------------------------------ */
/*  FNV-1a hash                                                        */
/* ------------------------------------------------------------------ */

static u64 fnv1a(const u8 *data, u64 len) {
    u64 hash = 14695981039346656037ULL;
    u64 i;

    for (i = 0; i < len; i++) {
        hash ^= (u64)data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* ------------------------------------------------------------------ */
/*  Hash table helpers                                                 */
/* ------------------------------------------------------------------ */

static kv_entry *ht_find(kv_store *store, const u8 *key, u64 key_len) {
    u64 idx = fnv1a(key, key_len) % store->bucket_count;
    kv_entry *e = store->buckets[idx];

    while (e) {
        if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0) {
            return e;
        }
        e = e->next;
    }
    return NULL;
}

static unsigned long ht_upsert(kv_store *store, const u8 *key, u64 key_len,
                                u64 file_offset, u64 val_len, int deleted) {
    kv_entry *e = ht_find(store, key, key_len);

    if (e) {
        e->file_offset = file_offset;
        e->val_len     = val_len;
        e->deleted     = deleted;
        return 0;
    }

    e = (kv_entry *)malloc(sizeof(kv_entry));
    if (!e) return 1;

    e->key = (u8 *)malloc(key_len);
    if (!e->key) { free(e); return 1; }

    memcpy(e->key, key, key_len);
    e->key_len     = key_len;
    e->file_offset = file_offset;
    e->val_len     = val_len;
    e->deleted     = deleted;

    {
        u64 idx = fnv1a(key, key_len) % store->bucket_count;
        e->next = store->buckets[idx];
        store->buckets[idx] = e;
    }
    store->entry_count++;
    return 0;
}

static void ht_free(kv_store *store) {
    u64 i;

    if (!store->buckets) return;

    for (i = 0; i < store->bucket_count; i++) {
        kv_entry *e = store->buckets[i];
        while (e) {
            kv_entry *next = e->next;
            free(e->key);
            free(e);
            e = next;
        }
    }
    free(store->buckets);
    store->buckets = NULL;
}

/* ------------------------------------------------------------------ */
/*  Log append helper                                                  */
/* ------------------------------------------------------------------ */

static unsigned long append_entry(kv_store *store, int type,
                                   const u8 *key, u64 key_len,
                                   const u8 *val, u64 val_len,
                                   u64 *out_val_offset) {
    u8 header[9]; /* 1 type + 4 key_len + 4 val_len */
    u64 val_offset;

    header[0] = (u8)type;
    write_u32_le(header + 1, (u32)key_len);
    write_u32_le(header + 5, (u32)val_len);

    if (fwrite(header, 1, 9, store->fp) != 9) return 1;
    if (fwrite(key, 1, (size_t)key_len, store->fp) != (size_t)key_len) return 1;

    /* val_offset is position right after key, where value bytes start */
    val_offset = (u64)ftell(store->fp);

    if (val_len > 0) {
        if (fwrite(val, 1, (size_t)val_len, store->fp) != (size_t)val_len)
            return 1;
    }

    if (fflush(store->fp) != 0) return 1;

    if (out_val_offset) *out_val_offset = val_offset;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Scan log to rebuild index                                          */
/* ------------------------------------------------------------------ */

static unsigned long scan_log(kv_store *store) {
    u8 header[9];

    fseek(store->fp, 0, SEEK_SET);

    for (;;) {
        u8  type;
        u32 key_len, val_len;
        u8 *key;
        u64 val_offset;
        size_t n;

        n = fread(header, 1, 9, store->fp);
        if (n == 0) break;       /* clean EOF */
        if (n != 9) return 1;    /* corrupt */

        type    = header[0];
        key_len = read_u32_le(header + 1);
        val_len = read_u32_le(header + 5);

        if (type > 1) return 1;  /* corrupt type byte */

        key = (u8 *)malloc(key_len);
        if (!key) return 1;

        if (fread(key, 1, key_len, store->fp) != key_len) {
            free(key);
            return 1;
        }

        val_offset = (u64)ftell(store->fp);

        /* Skip over the value bytes */
        if (val_len > 0) {
            if (fseek(store->fp, (long)val_len, SEEK_CUR) != 0) {
                free(key);
                return 1;
            }
        }

        {
            int deleted = (type == KV_TYPE_DELETE) ? 1 : 0;
            unsigned long rc = ht_upsert(store, key, key_len,
                                          val_offset, val_len, deleted);
            free(key);
            if (rc != 0) return 1;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Core API                                                           */
/* ------------------------------------------------------------------ */

unsigned long kv_open(kv_store **out, const char *path) {
    kv_store *store;
    size_t path_len;
    unsigned long rc;

    if (!out)  return 1;
    if (!path) return 2;

    store = (kv_store *)calloc(1, sizeof(kv_store));
    if (!store) return 4;

    path_len = strlen(path);
    if (path_len >= sizeof(store->path)) {
        free(store);
        return 3;
    }
    memcpy(store->path, path, path_len + 1);

    store->fp = fopen(path, "a+b");
    if (!store->fp) {
        free(store);
        return 3;
    }

    store->bucket_count = KV_BUCKET_COUNT;
    store->buckets = (kv_entry **)calloc(store->bucket_count, sizeof(kv_entry *));
    if (!store->buckets) {
        fclose(store->fp);
        free(store);
        return 4;
    }

    rc = scan_log(store);
    if (rc != 0) {
        ht_free(store);
        fclose(store->fp);
        free(store);
        return 5;
    }

    /* Seek to end for appending */
    fseek(store->fp, 0, SEEK_END);

    *out = store;
    return 0;
}

unsigned long kv_get(u8 **out, u64 *out_len,
                      kv_store *store,
                      const u8 *key, u64 key_len) {
    kv_entry *e;
    u8 *buf;

    if (!out)     return 1;
    if (!out_len) return 2;
    if (!store)   return 3;
    if (!key)     return 4;

    e = ht_find(store, key, key_len);
    if (!e || e->deleted) return 5;

    if (e->val_len == 0) {
        buf = (u8 *)malloc(1);
        if (!buf) return 5;
        *out     = buf;
        *out_len = 0;
        return 0;
    }

    buf = (u8 *)malloc((size_t)e->val_len);
    if (!buf) return 5;

    fseek(store->fp, (long)e->file_offset, SEEK_SET);
    if (fread(buf, 1, (size_t)e->val_len, store->fp) != (size_t)e->val_len) {
        free(buf);
        return 5;
    }

    /* Restore file position to end for appending */
    fseek(store->fp, 0, SEEK_END);

    *out     = buf;
    *out_len = e->val_len;
    return 0;
}

unsigned long kv_put(kv_store *store,
                      const u8 *key, u64 key_len,
                      const u8 *value, u64 val_len) {
    u64 val_offset;
    unsigned long rc;

    if (!store) return 1;
    if (!key)   return 2;
    if (val_len > 0 && !value) return 3;

    rc = append_entry(store, KV_TYPE_PUT, key, key_len,
                      value, val_len, &val_offset);
    if (rc != 0) return 4;

    rc = ht_upsert(store, key, key_len, val_offset, val_len, 0);
    if (rc != 0) return 4;

    return 0;
}

unsigned long kv_delete(kv_store *store,
                         const u8 *key, u64 key_len) {
    kv_entry *e;

    if (!store) return 1;
    if (!key)   return 2;

    e = ht_find(store, key, key_len);
    if (!e || e->deleted) return 3;

    {
        unsigned long rc = append_entry(store, KV_TYPE_DELETE,
                                         key, key_len, NULL, 0, NULL);
        if (rc != 0) return 3;
    }

    e->deleted = 1;
    return 0;
}

unsigned long kv_close(kv_store *store) {
    if (!store) return 1;

    if (store->fp) {
        if (fclose(store->fp) != 0) {
            ht_free(store);
            free(store);
            return 2;
        }
    }

    ht_free(store);
    free(store);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Iterator                                                           */
/* ------------------------------------------------------------------ */

unsigned long kv_iter_create(kv_iter **out, kv_store *store,
                              const u8 *prefix, u64 prefix_len) {
    kv_iter *it;

    if (!out)   return 1;
    if (!store) return 2;

    it = (kv_iter *)calloc(1, sizeof(kv_iter));
    if (!it) return 3;

    it->store      = store;
    it->bucket_idx = 0;
    it->current    = store->buckets[0];

    if (prefix && prefix_len > 0) {
        it->prefix = (u8 *)malloc((size_t)prefix_len);
        if (!it->prefix) {
            free(it);
            return 3;
        }
        memcpy(it->prefix, prefix, (size_t)prefix_len);
        it->prefix_len = prefix_len;
    } else {
        it->prefix     = NULL;
        it->prefix_len = 0;
    }

    it->val_buf     = NULL;
    it->val_buf_cap = 0;

    *out = it;
    return 0;
}

unsigned long kv_iter_next(const u8 **out_key, u64 *out_key_len,
                            const u8 **out_val, u64 *out_val_len,
                            kv_iter *it) {
    if (!out_key)     return 1;
    if (!out_key_len) return 2;
    if (!out_val)     return 3;
    if (!out_val_len) return 4;
    if (!it)          return 5;

    for (;;) {
        /* Advance to next valid entry */
        while (!it->current) {
            it->bucket_idx++;
            if (it->bucket_idx >= it->store->bucket_count) return 6;
            it->current = it->store->buckets[it->bucket_idx];
        }

        {
            kv_entry *e = it->current;
            it->current = e->next;

            /* Skip deleted entries */
            if (e->deleted) continue;

            /* Check prefix filter */
            if (it->prefix) {
                if (e->key_len < it->prefix_len) continue;
                if (memcmp(e->key, it->prefix, (size_t)it->prefix_len) != 0)
                    continue;
            }

            /* Read value from file */
            if (e->val_len > it->val_buf_cap) {
                u64 new_cap = e->val_len > 0 ? e->val_len : 1;
                u8 *tmp = (u8 *)realloc(it->val_buf, (size_t)new_cap);
                if (!tmp) return 6;
                it->val_buf     = tmp;
                it->val_buf_cap = new_cap;
            }

            if (e->val_len > 0) {
                fseek(it->store->fp, (long)e->file_offset, SEEK_SET);
                if (fread(it->val_buf, 1, (size_t)e->val_len,
                          it->store->fp) != (size_t)e->val_len) {
                    return 6;
                }
                fseek(it->store->fp, 0, SEEK_END);
            }

            *out_key     = e->key;
            *out_key_len = e->key_len;
            *out_val     = it->val_buf;
            *out_val_len = e->val_len;
            return 0;
        }
    }
}

unsigned long kv_iter_destroy(kv_iter *it) {
    if (!it) return 1;
    free(it->prefix);
    free(it->val_buf);
    free(it);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Batch                                                              */
/* ------------------------------------------------------------------ */

unsigned long kv_batch_create(kv_batch **out) {
    kv_batch *b;

    if (!out) return 1;

    b = (kv_batch *)calloc(1, sizeof(kv_batch));
    if (!b) return 2;

    b->ops = (kv_op *)malloc(KV_BATCH_INIT * sizeof(kv_op));
    if (!b->ops) {
        free(b);
        return 2;
    }

    b->count = 0;
    b->cap   = KV_BATCH_INIT;

    *out = b;
    return 0;
}

static unsigned long batch_add_op(kv_batch *batch, int type,
                                   const u8 *key, u64 key_len,
                                   const u8 *val, u64 val_len) {
    kv_op *op;

    if (batch->count == batch->cap) {
        u64 new_cap = batch->cap * 2;
        kv_op *tmp = (kv_op *)realloc(batch->ops,
                                       (size_t)(new_cap * sizeof(kv_op)));
        if (!tmp) return 3;
        batch->ops = tmp;
        batch->cap = new_cap;
    }

    op = &batch->ops[batch->count];
    op->type    = type;
    op->key_len = key_len;
    op->val_len = val_len;

    op->key = (u8 *)malloc((size_t)key_len);
    if (!op->key) return 3;
    memcpy(op->key, key, (size_t)key_len);

    if (val && val_len > 0) {
        op->val = (u8 *)malloc((size_t)val_len);
        if (!op->val) {
            free(op->key);
            return 3;
        }
        memcpy(op->val, val, (size_t)val_len);
    } else {
        op->val     = NULL;
        op->val_len = 0;
    }

    batch->count++;
    return 0;
}

unsigned long kv_batch_put(kv_batch *batch,
                            const u8 *key, u64 key_len,
                            const u8 *value, u64 val_len) {
    if (!batch) return 1;
    if (!key)   return 2;
    return batch_add_op(batch, KV_TYPE_PUT, key, key_len, value, val_len);
}

unsigned long kv_batch_delete(kv_batch *batch,
                               const u8 *key, u64 key_len) {
    if (!batch) return 1;
    if (!key)   return 2;
    return batch_add_op(batch, KV_TYPE_DELETE, key, key_len, NULL, 0);
}

unsigned long kv_batch_commit(kv_store *store, kv_batch *batch) {
    u64 i;

    if (!store) return 1;
    if (!batch) return 2;

    for (i = 0; i < batch->count; i++) {
        kv_op *op = &batch->ops[i];
        unsigned long rc;

        if (op->type == KV_TYPE_PUT) {
            u64 val_offset;
            rc = append_entry(store, KV_TYPE_PUT,
                              op->key, op->key_len,
                              op->val, op->val_len, &val_offset);
            if (rc != 0) return 3;
            rc = ht_upsert(store, op->key, op->key_len,
                           val_offset, op->val_len, 0);
            if (rc != 0) return 3;
        } else {
            kv_entry *e = ht_find(store, op->key, op->key_len);
            if (e && !e->deleted) {
                rc = append_entry(store, KV_TYPE_DELETE,
                                  op->key, op->key_len, NULL, 0, NULL);
                if (rc != 0) return 3;
                e->deleted = 1;
            }
        }
    }

    return 0;
}

unsigned long kv_batch_destroy(kv_batch *batch) {
    u64 i;

    if (!batch) return 1;

    for (i = 0; i < batch->count; i++) {
        free(batch->ops[i].key);
        free(batch->ops[i].val);
    }
    free(batch->ops);
    free(batch);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Compaction                                                         */
/* ------------------------------------------------------------------ */

unsigned long kv_compact(kv_store *store) {
    char tmp_path[520];
    FILE *tmp_fp;
    kv_store tmp_store;
    u64 i;

    if (!store) return 1;

    snprintf(tmp_path, sizeof(tmp_path), "%s.compact", store->path);

    tmp_fp = fopen(tmp_path, "w+b");
    if (!tmp_fp) return 2;

    /* Set up a temporary store struct for append_entry */
    memset(&tmp_store, 0, sizeof(tmp_store));
    tmp_store.fp = tmp_fp;

    /* Write all non-deleted entries to the temp file */
    for (i = 0; i < store->bucket_count; i++) {
        kv_entry *e = store->buckets[i];
        while (e) {
            if (!e->deleted) {
                u8 *val = NULL;
                u64 val_offset;
                unsigned long rc;

                if (e->val_len > 0) {
                    val = (u8 *)malloc((size_t)e->val_len);
                    if (!val) {
                        fclose(tmp_fp);
                        remove(tmp_path);
                        return 2;
                    }
                    fseek(store->fp, (long)e->file_offset, SEEK_SET);
                    if (fread(val, 1, (size_t)e->val_len, store->fp)
                        != (size_t)e->val_len) {
                        free(val);
                        fclose(tmp_fp);
                        remove(tmp_path);
                        return 2;
                    }
                }

                rc = append_entry(&tmp_store, KV_TYPE_PUT,
                                  e->key, e->key_len,
                                  val, e->val_len, &val_offset);
                free(val);
                if (rc != 0) {
                    fclose(tmp_fp);
                    remove(tmp_path);
                    return 2;
                }

                /* Update the entry's offset for the new file */
                e->file_offset = val_offset;
            }
            e = e->next;
        }
    }

    fclose(tmp_fp);
    fclose(store->fp);

    /* Replace the old file with the compacted one */
    remove(store->path);
    if (rename(tmp_path, store->path) != 0) {
        /* Try to reopen original — best effort recovery */
        store->fp = fopen(store->path, "a+b");
        return 2;
    }

    store->fp = fopen(store->path, "a+b");
    if (!store->fp) return 2;

    fseek(store->fp, 0, SEEK_END);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Snapshot                                                           */
/* ------------------------------------------------------------------ */

unsigned long kv_snapshot(kv_store **out, kv_store *store) {
    kv_store *snap;
    u64 i;

    if (!out)   return 1;
    if (!store) return 2;

    snap = (kv_store *)calloc(1, sizeof(kv_store));
    if (!snap) return 3;

    memcpy(snap->path, store->path, sizeof(store->path));
    snap->is_snapshot  = 1;
    snap->bucket_count = store->bucket_count;
    snap->entry_count  = 0;

    snap->fp = fopen(store->path, "rb");
    if (!snap->fp) {
        free(snap);
        return 3;
    }

    snap->buckets = (kv_entry **)calloc(snap->bucket_count, sizeof(kv_entry *));
    if (!snap->buckets) {
        fclose(snap->fp);
        free(snap);
        return 3;
    }

    /* Deep copy the hash table */
    for (i = 0; i < store->bucket_count; i++) {
        kv_entry *src = store->buckets[i];
        kv_entry **dst_ptr = &snap->buckets[i];

        while (src) {
            kv_entry *copy = (kv_entry *)malloc(sizeof(kv_entry));
            if (!copy) {
                ht_free(snap);
                fclose(snap->fp);
                free(snap);
                return 3;
            }

            copy->key = (u8 *)malloc((size_t)src->key_len);
            if (!copy->key) {
                free(copy);
                ht_free(snap);
                fclose(snap->fp);
                free(snap);
                return 3;
            }

            memcpy(copy->key, src->key, (size_t)src->key_len);
            copy->key_len     = src->key_len;
            copy->file_offset = src->file_offset;
            copy->val_len     = src->val_len;
            copy->deleted     = src->deleted;
            copy->next        = NULL;

            *dst_ptr = copy;
            dst_ptr  = &copy->next;
            snap->entry_count++;

            src = src->next;
        }
    }

    *out = snap;
    return 0;
}
