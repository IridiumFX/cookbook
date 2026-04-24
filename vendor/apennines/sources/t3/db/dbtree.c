/* t3/db/dbtree.c — disk-backed B+-tree
 *
 * Phase A of the DB read-side improvement (close SELECT gap vs sqlite).
 *
 * Layout on disk: fixed 4 KB pages.
 *   Page 0 is the file header (magic, version, root, free-head, num-pages).
 *   Other pages are one of:
 *     INTERNAL — slot array of (key, child_page) cells + rightmost_child.
 *     LEAF     — slot array of (key, value_or_overflow_ref) cells, chained
 *                via prev_leaf / next_leaf.
 *     OVERFLOW — raw payload continuation, linked via next_overflow.
 *     FREE     — on the free-page list, linked via next_free.
 *
 * Each page has a 24-byte header:
 *   [0..3]   magic (APGP)
 *   [4]      type
 *   [5]      flags
 *   [6..7]   num_slots (or payload_len for OVERFLOW)
 *   [8..9]   free_off (first free byte after slot array)
 *   [10..11] cell_base (cells grow down from this offset)
 *   [12..15] extra1 (rightmost_child | next_leaf | next_overflow | next_free)
 *   [16..19] extra2 (prev_leaf — unused for other page types)
 *   [20..23] reserved
 *
 * Slot array starts at offset 24. Each slot is a u16 offset into the page.
 * Cells grow downward from cell_base; new cells take the lowest available
 * base. Free space = cell_base - (24 + num_slots*2).
 *
 * Key ordering: lexicographic bytes + length tiebreak. Engine-level keys
 * are encoded big-endian for numeric components so they sort correctly.
 *
 * Splits: midpoint by slot count. Separator key propagates up. Root
 * splits allocate a new internal root.
 *
 * Deletes: remove cell + slot. Overflow chains reclaimed. No rebalance
 * on under-occupancy in Phase A (sqlite/innodb do eager rebalance; we
 * accept sparse pages until a consumer pushes on it).
 *
 * Overflow: cells with value_len > inline threshold spill onto a chain
 * of OVERFLOW pages. Leaf cell keeps first OVERFLOW_INLINE_PREFIX bytes
 * as a prefix so most comparisons can short-circuit without chasing.
 *
 * Durability: no internal WAL in Phase A — dbtree_flush/sync write dirty
 * pages; crash mid-split is corrupt. Phase B wraps tree mutations in
 * the engine's existing WAL.
 */

#include "apennines/t3/db/dbtree.h"
#include "apennines/t1/sync/mutex/mutex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- constants ---- */

#define DBTREE_PAGE_SIZE          4096u
#define DBTREE_FILE_MAGIC         0x54425041u    /* "APBT" little-endian */
#define DBTREE_PAGE_MAGIC         0x50475041u    /* "APGP" */
#define DBTREE_VERSION            1u
#define DBTREE_PAGE_HDR_SIZE      24u

#define PAGE_TYPE_HEADER         0u
#define PAGE_TYPE_INTERNAL       1u
#define PAGE_TYPE_LEAF           2u
#define PAGE_TYPE_OVERFLOW       3u
#define PAGE_TYPE_FREE           4u

#define CELL_FLAG_INLINE         0u
#define CELL_FLAG_OVERFLOW       1u

/* Bytes of the value kept inline on the leaf when overflowed. Enables most
 * lookups to short-circuit without chasing the overflow chain. */
#define OVERFLOW_INLINE_PREFIX   32u

/* Cap keys so a handful fit per page. */
#define MAX_KEY_LEN              (DBTREE_PAGE_SIZE / 8)

/* Inline-value threshold: values whose total encoded cell would exceed
 * (page_size / 4) spill to overflow. Keeps at least ~4 small cells per leaf. */
#define INLINE_VALUE_THRESHOLD   ((DBTREE_PAGE_SIZE / 4) - 32u)

#define DEFAULT_CACHE_PAGES      64u

#define NO_IDX                   0xFFFFFFFFu
#define NO_PAGE                  0u

#define MAX_TREE_DEPTH           32

/* ---- types ---- */

typedef struct page_buf {
    u32 page_no;
    int dirty;
    int pinned;
    int in_use;
    u32 lru_prev, lru_next;
    u8  data[DBTREE_PAGE_SIZE];
} page_buf;

typedef struct page_cache {
    page_buf *slots;
    u32       capacity;
    u32       lru_head, lru_tail;
    u64       hits, misses;
} page_cache;

struct dbtree {
    FILE       *fp;
    char       *path;
    page_cache  cache;
    u32         root_page;
    u32         free_head;
    u32         num_pages;
    int         dirty_header;
    u64         num_keys;
    /* Phase D concurrency model:
     *   tree_rwlock — readers (get/seek/cursor_*) take the read side;
     *     writers (put/delete/close) take the write side. Lets
     *     concurrent SELECTs parallelise end-to-end.
     *   cache_mutex — protects page_cache's LRU list, slot reuse, and
     *     pin refcounts. Held only across cache_pin / cache_unpin /
     *     eviction; released while actually reading page bytes.
     *     Writers already hold the tree write-lock exclusively, so
     *     they don't contend on cache_mutex.
     * This split gives parallel readers without per-page latching
     * (crabbing), which is fine for our page count. */
    rwlock      tree_rwlock;
    mutex       cache_mutex;
    int         lock_initialised;
};

struct dbtree_cursor {
    dbtree *bt;
    u32    leaf_page;
    u32    slot_idx;
    int    valid;
    u8    *key_buf;
    u64    key_len;
    u32    key_cap;
};

/* ---- little-endian byte helpers ---- */

static u16 get_u16(const u8 *p) {
    return (u16)p[0] | ((u16)p[1] << 8);
}
static void put_u16(u8 *p, u16 v) {
    p[0] = (u8)(v & 0xff);
    p[1] = (u8)((v >> 8) & 0xff);
}
static u32 get_u32(const u8 *p) {
    return (u32)p[0]
         | ((u32)p[1] << 8)
         | ((u32)p[2] << 16)
         | ((u32)p[3] << 24);
}
static void put_u32(u8 *p, u32 v) {
    p[0] = (u8)(v & 0xff);
    p[1] = (u8)((v >> 8) & 0xff);
    p[2] = (u8)((v >> 16) & 0xff);
    p[3] = (u8)((v >> 24) & 0xff);
}

/* ---- page-header accessors ---- */

static u32  ph_magic      (const u8 *p)       { return get_u32(p + 0); }
static void ph_set_magic  (u8 *p, u32 v)      { put_u32(p + 0, v); }
static u8   ph_type       (const u8 *p)       { return p[4]; }
static void ph_set_type   (u8 *p, u8 v)       { p[4] = v; }
static u16  ph_num_slots  (const u8 *p)       { return get_u16(p + 6); }
static void ph_set_num_slots(u8 *p, u16 v)    { put_u16(p + 6, v); }
static u16  ph_free_off   (const u8 *p)       { return get_u16(p + 8); }
static void ph_set_free_off(u8 *p, u16 v)     { put_u16(p + 8, v); }
static u16  ph_cell_base  (const u8 *p)       { return get_u16(p + 10); }
static void ph_set_cell_base(u8 *p, u16 v)    { put_u16(p + 10, v); }
static u32  ph_extra      (const u8 *p)       { return get_u32(p + 12); }
static void ph_set_extra  (u8 *p, u32 v)      { put_u32(p + 12, v); }
static u32  ph_extra2     (const u8 *p)       { return get_u32(p + 16); }
static void ph_set_extra2 (u8 *p, u32 v)      { put_u32(p + 16, v); }

static u16 slot_get(const u8 *p, u16 i) {
    return get_u16(p + DBTREE_PAGE_HDR_SIZE + i * 2);
}
static void slot_set(u8 *p, u16 i, u16 v) {
    put_u16(p + DBTREE_PAGE_HDR_SIZE + i * 2, v);
}
static u16 page_used_top(const u8 *p) {
    return (u16)(DBTREE_PAGE_HDR_SIZE + ph_num_slots(p) * 2);
}
static u16 page_free_space(const u8 *p) {
    u16 base = ph_cell_base(p);
    u16 top  = page_used_top(p);
    return base > top ? (u16)(base - top) : (u16)0;
}

/* ---- file I/O ---- */

static unsigned long dbtree_read_page(dbtree *bt, u32 page_no, u8 *buf) {
    if (fseek(bt->fp, (long)(page_no * DBTREE_PAGE_SIZE), SEEK_SET) != 0) return 1;
    size_t n = fread(buf, 1, DBTREE_PAGE_SIZE, bt->fp);
    if (n != DBTREE_PAGE_SIZE) return 2;
    return 0;
}

static unsigned long dbtree_write_page(dbtree *bt, u32 page_no, const u8 *buf) {
    if (fseek(bt->fp, (long)(page_no * DBTREE_PAGE_SIZE), SEEK_SET) != 0) return 1;
    size_t n = fwrite(buf, 1, DBTREE_PAGE_SIZE, bt->fp);
    if (n != DBTREE_PAGE_SIZE) return 2;
    return 0;
}

/* ---- page cache ---- */

static unsigned long cache_init(page_cache *c, u32 capacity) {
    c->slots = (page_buf *)calloc(capacity, sizeof(page_buf));
    if (!c->slots) return 1;
    c->capacity = capacity;
    for (u32 i = 0; i < capacity; i++) {
        c->slots[i].lru_prev = c->slots[i].lru_next = NO_IDX;
    }
    c->lru_head = c->lru_tail = NO_IDX;
    c->hits = c->misses = 0;
    return 0;
}

static void cache_free(page_cache *c) {
    free(c->slots);
    c->slots = NULL;
    c->capacity = 0;
}

static void lru_unlink(page_cache *c, u32 idx) {
    page_buf *b = &c->slots[idx];
    if (b->lru_prev != NO_IDX) c->slots[b->lru_prev].lru_next = b->lru_next;
    else                       c->lru_head = b->lru_next;
    if (b->lru_next != NO_IDX) c->slots[b->lru_next].lru_prev = b->lru_prev;
    else                       c->lru_tail = b->lru_prev;
    b->lru_prev = b->lru_next = NO_IDX;
}

static void lru_push_front(page_cache *c, u32 idx) {
    page_buf *b = &c->slots[idx];
    b->lru_prev = NO_IDX;
    b->lru_next = c->lru_head;
    if (c->lru_head != NO_IDX) c->slots[c->lru_head].lru_prev = idx;
    c->lru_head = idx;
    if (c->lru_tail == NO_IDX) c->lru_tail = idx;
}

/* Must be called with bt->cache_mutex held. */
static unsigned long cache_pin_locked(page_buf **out, dbtree *bt, u32 page_no) {
    page_cache *c = &bt->cache;
    for (u32 i = 0; i < c->capacity; i++) {
        if (c->slots[i].in_use && c->slots[i].page_no == page_no) {
            c->slots[i].pinned++;
            if (c->lru_head != i) {
                lru_unlink(c, i);
                lru_push_front(c, i);
            }
            c->hits++;
            *out = &c->slots[i];
            return 0;
        }
    }
    c->misses++;
    u32 target = NO_IDX;
    for (u32 i = 0; i < c->capacity; i++) {
        if (!c->slots[i].in_use) { target = i; break; }
    }
    if (target == NO_IDX) {
        for (u32 idx = c->lru_tail; idx != NO_IDX; idx = c->slots[idx].lru_prev) {
            if (c->slots[idx].pinned == 0) { target = idx; break; }
        }
        if (target == NO_IDX) return 1;
        if (c->slots[target].dirty) {
            unsigned long rc = dbtree_write_page(bt, c->slots[target].page_no, c->slots[target].data);
            if (rc) return 2;
        }
        lru_unlink(c, target);
        c->slots[target].in_use = 0;
    }
    page_buf *b = &c->slots[target];
    unsigned long rc = dbtree_read_page(bt, page_no, b->data);
    if (rc) return 3;
    b->page_no = page_no;
    b->dirty   = 0;
    b->pinned  = 1;
    b->in_use  = 1;
    lru_push_front(c, target);
    *out = b;
    return 0;
}

/* Public wrapper: grab cache_mutex, do the pin, release. Safe to call
 * under either a tree read-lock or write-lock (both are held by the
 * public API wrappers before any cache_pin). */
static unsigned long cache_pin(page_buf **out, dbtree *bt, u32 page_no) {
    if (bt->lock_initialised) mutex_lock(&bt->cache_mutex);
    unsigned long rc = cache_pin_locked(out, bt, page_no);
    if (bt->lock_initialised) mutex_unlock(&bt->cache_mutex);
    return rc;
}

static void cache_unpin(page_buf *b) {
    /* Single-slot decrement; atomic enough on native word. A torn
     * refcount would imply two threads racing on the same page_buf
     * without any tree-level lock — we don't expose that surface. */
    if (b && b->pinned > 0) b->pinned--;
}

static void cache_mark_dirty(page_buf *b) {
    if (b) b->dirty = 1;
}

static unsigned long cache_flush_all(dbtree *bt) {
    page_cache *c = &bt->cache;
    for (u32 i = 0; i < c->capacity; i++) {
        if (c->slots[i].in_use && c->slots[i].dirty) {
            unsigned long rc = dbtree_write_page(bt, c->slots[i].page_no, c->slots[i].data);
            if (rc) return 1;
            c->slots[i].dirty = 0;
        }
    }
    return 0;
}

static void cache_invalidate_all(page_cache *c) {
    for (u32 i = 0; i < c->capacity; i++) {
        c->slots[i].in_use = 0;
        c->slots[i].dirty  = 0;
        c->slots[i].pinned = 0;
        c->slots[i].lru_prev = c->slots[i].lru_next = NO_IDX;
    }
    c->lru_head = c->lru_tail = NO_IDX;
}

/* ---- file header ---- */

static unsigned long write_file_header(dbtree *bt) {
    u8 buf[DBTREE_PAGE_SIZE];
    memset(buf, 0, sizeof(buf));
    put_u32(buf + 0,  DBTREE_FILE_MAGIC);
    put_u32(buf + 4,  DBTREE_VERSION);
    put_u32(buf + 8,  DBTREE_PAGE_SIZE);
    put_u32(buf + 12, bt->root_page);
    put_u32(buf + 16, bt->free_head);
    put_u32(buf + 20, bt->num_pages);
    unsigned long rc = dbtree_write_page(bt, 0, buf);
    if (rc == 0) bt->dirty_header = 0;
    return rc;
}

static unsigned long read_file_header(dbtree *bt) {
    u8 buf[DBTREE_PAGE_SIZE];
    unsigned long rc = dbtree_read_page(bt, 0, buf);
    if (rc) return 1;
    if (get_u32(buf + 0) != DBTREE_FILE_MAGIC) return 2;
    if (get_u32(buf + 4) != DBTREE_VERSION)    return 3;
    if (get_u32(buf + 8) != DBTREE_PAGE_SIZE)  return 4;
    bt->root_page = get_u32(buf + 12);
    bt->free_head = get_u32(buf + 16);
    bt->num_pages = get_u32(buf + 20);
    return 0;
}

/* ---- page alloc / free ---- */

static unsigned long alloc_page(u32 *out_pn, dbtree *bt) {
    if (bt->free_head != NO_PAGE) {
        u32 pn = bt->free_head;
        page_buf *b;
        unsigned long rc = cache_pin(&b, bt, pn);
        if (rc) return rc;
        u32 next_free = ph_extra(b->data);
        bt->free_head = next_free;
        bt->dirty_header = 1;
        memset(b->data, 0, DBTREE_PAGE_SIZE);
        cache_mark_dirty(b);
        cache_unpin(b);
        *out_pn = pn;
        return 0;
    }
    u32 pn = bt->num_pages;
    bt->num_pages++;
    bt->dirty_header = 1;

    u8 zero[DBTREE_PAGE_SIZE];
    memset(zero, 0, sizeof(zero));
    unsigned long rc = dbtree_write_page(bt, pn, zero);
    if (rc) { bt->num_pages--; return rc; }

    /* Pin so caller can init then we keep it cached */
    page_buf *b;
    rc = cache_pin(&b, bt, pn);
    if (rc) return rc;
    cache_unpin(b);
    *out_pn = pn;
    return 0;
}

static unsigned long free_page(dbtree *bt, u32 page_no) {
    page_buf *b;
    unsigned long rc = cache_pin(&b, bt, page_no);
    if (rc) return rc;
    memset(b->data, 0, DBTREE_PAGE_SIZE);
    ph_set_magic(b->data, DBTREE_PAGE_MAGIC);
    ph_set_type(b->data, PAGE_TYPE_FREE);
    ph_set_extra(b->data, bt->free_head);
    cache_mark_dirty(b);
    cache_unpin(b);
    bt->free_head = page_no;
    bt->dirty_header = 1;
    return 0;
}

/* ---- page init ---- */

static void page_init_leaf(u8 *p) {
    memset(p, 0, DBTREE_PAGE_SIZE);
    ph_set_magic(p, DBTREE_PAGE_MAGIC);
    ph_set_type(p, PAGE_TYPE_LEAF);
    ph_set_num_slots(p, 0);
    ph_set_free_off(p, DBTREE_PAGE_HDR_SIZE);
    ph_set_cell_base(p, DBTREE_PAGE_SIZE);
    ph_set_extra(p, NO_PAGE);
    ph_set_extra2(p, NO_PAGE);
}

static void page_init_internal(u8 *p, u32 rightmost) {
    memset(p, 0, DBTREE_PAGE_SIZE);
    ph_set_magic(p, DBTREE_PAGE_MAGIC);
    ph_set_type(p, PAGE_TYPE_INTERNAL);
    ph_set_num_slots(p, 0);
    ph_set_free_off(p, DBTREE_PAGE_HDR_SIZE);
    ph_set_cell_base(p, DBTREE_PAGE_SIZE);
    ph_set_extra(p, rightmost);
    ph_set_extra2(p, NO_PAGE);
}

static void page_init_overflow(u8 *p) {
    memset(p, 0, DBTREE_PAGE_SIZE);
    ph_set_magic(p, DBTREE_PAGE_MAGIC);
    ph_set_type(p, PAGE_TYPE_OVERFLOW);
    ph_set_num_slots(p, 0);     /* payload_len */
    ph_set_free_off(p, 0);
    ph_set_cell_base(p, 0);
    ph_set_extra(p, NO_PAGE);   /* next_overflow */
    ph_set_extra2(p, NO_PAGE);
}

/* ---- leaf cell format ----
 *
 * Inline:
 *   [u16 key_len][key_bytes][u8 flag=0][u16 value_len][value_bytes]
 *
 * Overflow:
 *   [u16 key_len][key_bytes][u8 flag=1]
 *   [u32 total_value_len][u32 first_overflow_page]
 *   [u16 inline_prefix_len][inline_prefix_bytes]
 */

static u16 leaf_cell_key_len(const u8 *cell)     { return get_u16(cell); }
static const u8 *leaf_cell_key(const u8 *cell)   { return cell + 2; }
static u8   leaf_cell_flag(const u8 *cell) {
    u16 klen = get_u16(cell);
    return cell[2 + klen];
}

static u16 leaf_cell_inline_vlen(const u8 *cell) {
    u16 klen = get_u16(cell);
    return get_u16(cell + 2 + klen + 1);
}
static const u8 *leaf_cell_inline_value(const u8 *cell) {
    u16 klen = get_u16(cell);
    return cell + 2 + klen + 1 + 2;
}

static u32 leaf_cell_overflow_total(const u8 *cell) {
    u16 klen = get_u16(cell);
    return get_u32(cell + 2 + klen + 1);
}
static u32 leaf_cell_overflow_first(const u8 *cell) {
    u16 klen = get_u16(cell);
    return get_u32(cell + 2 + klen + 1 + 4);
}
static u16 leaf_cell_overflow_prefix_len(const u8 *cell) {
    u16 klen = get_u16(cell);
    return get_u16(cell + 2 + klen + 1 + 4 + 4);
}
static const u8 *leaf_cell_overflow_prefix(const u8 *cell) {
    u16 klen = get_u16(cell);
    return cell + 2 + klen + 1 + 4 + 4 + 2;
}

static u16 leaf_cell_size(const u8 *cell) {
    u16 klen = get_u16(cell);
    u8 flag = cell[2 + klen];
    if (flag == CELL_FLAG_INLINE) {
        u16 vlen = get_u16(cell + 2 + klen + 1);
        return (u16)(2 + klen + 1 + 2 + vlen);
    } else {
        u16 prefix = get_u16(cell + 2 + klen + 1 + 4 + 4);
        return (u16)(2 + klen + 1 + 4 + 4 + 2 + prefix);
    }
}

/* Build an inline leaf cell into `out`; returns bytes written. */
static u16 build_leaf_cell_inline(u8 *out,
                                   const u8 *key, u16 klen,
                                   const u8 *val, u16 vlen) {
    u16 p = 0;
    put_u16(out + p, klen); p += 2;
    memcpy(out + p, key, klen); p += klen;
    out[p++] = CELL_FLAG_INLINE;
    put_u16(out + p, vlen); p += 2;
    if (vlen) memcpy(out + p, val, vlen);
    p += vlen;
    return p;
}

static u16 build_leaf_cell_overflow(u8 *out,
                                     const u8 *key, u16 klen,
                                     u32 total_vlen, u32 first_ov_page,
                                     const u8 *prefix, u16 prefix_len) {
    u16 p = 0;
    put_u16(out + p, klen); p += 2;
    memcpy(out + p, key, klen); p += klen;
    out[p++] = CELL_FLAG_OVERFLOW;
    put_u32(out + p, total_vlen); p += 4;
    put_u32(out + p, first_ov_page); p += 4;
    put_u16(out + p, prefix_len); p += 2;
    if (prefix_len) memcpy(out + p, prefix, prefix_len);
    p += prefix_len;
    return p;
}

/* ---- internal cell format ----
 *
 * [u16 key_len][key_bytes][u32 child_page]
 */

static u16 internal_cell_key_len(const u8 *cell)   { return get_u16(cell); }
static const u8 *internal_cell_key(const u8 *cell) { return cell + 2; }
static u32 internal_cell_child(const u8 *cell) {
    u16 klen = get_u16(cell);
    return get_u32(cell + 2 + klen);
}
static u16 internal_cell_size(const u8 *cell) {
    u16 klen = get_u16(cell);
    return (u16)(2 + klen + 4);
}

static u16 build_internal_cell(u8 *out,
                                const u8 *key, u16 klen, u32 child) {
    u16 p = 0;
    put_u16(out + p, klen); p += 2;
    memcpy(out + p, key, klen); p += klen;
    put_u32(out + p, child); p += 4;
    return p;
}

/* ---- key compare ---- */

static int key_cmp(const u8 *a, u16 alen, const u8 *b, u16 blen) {
    u16 m = alen < blen ? alen : blen;
    int c = memcmp(a, b, m);
    if (c != 0) return c < 0 ? -1 : 1;
    if (alen < blen) return -1;
    if (alen > blen) return 1;
    return 0;
}

/* ---- slotted-page primitives ---- */

/* Insert a cell into a page at slot position `slot_pos`.
 * The slot array is expanded; the cell is placed at (cell_base - cell_size). */
static void page_insert_cell(u8 *p, u16 slot_pos,
                              const u8 *cell_bytes, u16 cell_size) {
    u16 nslots = ph_num_slots(p);
    u16 cb     = ph_cell_base(p);
    u16 new_cb = (u16)(cb - cell_size);

    /* Shift slots after slot_pos one step right. */
    for (int i = (int)nslots; i > (int)slot_pos; i--) {
        slot_set(p, (u16)i, slot_get(p, (u16)(i - 1)));
    }
    slot_set(p, slot_pos, new_cb);
    ph_set_num_slots(p, (u16)(nslots + 1));
    ph_set_cell_base(p, new_cb);
    ph_set_free_off(p, (u16)(DBTREE_PAGE_HDR_SIZE + (nslots + 1) * 2));

    memcpy(p + new_cb, cell_bytes, cell_size);
}

/* Remove the slot at `slot_pos`. The cell bytes remain in place (garbage);
 * a compaction pass reclaims space. We compact eagerly on every delete so
 * free_space() is honest. */
static void page_compact(u8 *p) {
    u16 nslots = ph_num_slots(p);
    u8 tmp[DBTREE_PAGE_SIZE];
    memcpy(tmp, p, DBTREE_PAGE_SIZE);

    u16 write_base = DBTREE_PAGE_SIZE;
    u8 type = ph_type(p);
    for (u16 i = 0; i < nslots; i++) {
        u16 off = slot_get(tmp, i);
        u16 cell_size;
        if (type == PAGE_TYPE_LEAF)
            cell_size = leaf_cell_size(tmp + off);
        else
            cell_size = internal_cell_size(tmp + off);
        write_base = (u16)(write_base - cell_size);
        memcpy(p + write_base, tmp + off, cell_size);
        slot_set(p, i, write_base);
    }
    ph_set_cell_base(p, write_base);
}

static void page_delete_slot(u8 *p, u16 slot_pos) {
    u16 nslots = ph_num_slots(p);
    for (u16 i = slot_pos; i + 1 < nslots; i++) {
        slot_set(p, i, slot_get(p, (u16)(i + 1)));
    }
    ph_set_num_slots(p, (u16)(nslots - 1));
    ph_set_free_off(p, (u16)(DBTREE_PAGE_HDR_SIZE + (nslots - 1) * 2));
    page_compact(p);
}

/* ---- leaf search ---- */

/* Binary-search the leaf for key. Returns the slot index where key would
 * be inserted (or where an equal key lives). Sets *found=1 on exact match. */
static u16 leaf_search(const u8 *p, const u8 *key, u16 klen, int *found) {
    u16 lo = 0, hi = ph_num_slots(p);
    *found = 0;
    while (lo < hi) {
        u16 mid = (u16)(lo + (hi - lo) / 2);
        const u8 *cell = p + slot_get(p, mid);
        u16 ck_len = leaf_cell_key_len(cell);
        int cmp = key_cmp(key, klen, leaf_cell_key(cell), ck_len);
        if (cmp == 0) { *found = 1; return mid; }
        if (cmp < 0) hi = mid;
        else         lo = (u16)(mid + 1);
    }
    return lo;
}

/* Internal descent: find first slot i where key < cell[i].key. Descend to
 * cell[i].child. If no such slot, descend to rightmost (ph_extra). Also
 * returns the index we chose (or nslots to indicate rightmost) in *out_slot. */
static u32 internal_descend(const u8 *p, const u8 *key, u16 klen, u16 *out_slot) {
    u16 lo = 0, hi = ph_num_slots(p);
    while (lo < hi) {
        u16 mid = (u16)(lo + (hi - lo) / 2);
        const u8 *cell = p + slot_get(p, mid);
        u16 ck_len = internal_cell_key_len(cell);
        int cmp = key_cmp(key, klen, internal_cell_key(cell), ck_len);
        if (cmp < 0) hi = mid;
        else         lo = (u16)(mid + 1);
    }
    if (out_slot) *out_slot = lo;
    if (lo < ph_num_slots(p)) {
        const u8 *cell = p + slot_get(p, lo);
        return internal_cell_child(cell);
    }
    return ph_extra(p);
}

/* ---- overflow chain ---- */

static unsigned long overflow_chain_alloc(u32 *out_first, dbtree *bt,
                                           const u8 *val, u32 vlen,
                                           u32 skip_prefix) {
    /* Allocate enough pages to hold val[skip_prefix .. vlen). */
    const u32 payload_per_page = DBTREE_PAGE_SIZE - DBTREE_PAGE_HDR_SIZE;
    u32 remaining = vlen - skip_prefix;
    const u8 *src = val + skip_prefix;

    u32 first = NO_PAGE;
    u32 prev  = NO_PAGE;

    while (remaining > 0) {
        u32 pn;
        unsigned long rc = alloc_page(&pn, bt);
        if (rc) return rc;

        page_buf *b;
        rc = cache_pin(&b, bt, pn);
        if (rc) return rc;
        page_init_overflow(b->data);
        u32 chunk = remaining < payload_per_page ? remaining : payload_per_page;
        ph_set_num_slots(b->data, (u16)chunk);
        memcpy(b->data + DBTREE_PAGE_HDR_SIZE, src, chunk);
        src += chunk;
        remaining -= chunk;
        cache_mark_dirty(b);
        cache_unpin(b);

        if (prev == NO_PAGE) {
            first = pn;
        } else {
            page_buf *pb;
            rc = cache_pin(&pb, bt, prev);
            if (rc) return rc;
            ph_set_extra(pb->data, pn);
            cache_mark_dirty(pb);
            cache_unpin(pb);
        }
        prev = pn;
    }
    *out_first = first;
    return 0;
}

static unsigned long overflow_chain_read(u8 *dst, u32 dst_len, dbtree *bt, u32 first_page) {
    u32 pn = first_page;
    u32 written = 0;
    while (pn != NO_PAGE && written < dst_len) {
        page_buf *b;
        unsigned long rc = cache_pin(&b, bt, pn);
        if (rc) return rc;
        u32 chunk = ph_num_slots(b->data);
        if (written + chunk > dst_len) chunk = dst_len - written;
        memcpy(dst + written, b->data + DBTREE_PAGE_HDR_SIZE, chunk);
        written += chunk;
        pn = ph_extra(b->data);
        cache_unpin(b);
    }
    return 0;
}

static unsigned long overflow_chain_free(dbtree *bt, u32 first_page) {
    u32 pn = first_page;
    while (pn != NO_PAGE) {
        page_buf *b;
        unsigned long rc = cache_pin(&b, bt, pn);
        if (rc) return rc;
        u32 next = ph_extra(b->data);
        cache_unpin(b);
        rc = free_page(bt, pn);
        if (rc) return rc;
        pn = next;
    }
    return 0;
}

/* ---- path tracking (descent to leaf) ---- */

typedef struct {
    u32 page_no;
    u16 slot;          /* slot we descended through (nslots = rightmost) */
} path_entry;

static unsigned long find_leaf(path_entry *path, u32 *out_depth,
                                dbtree *bt, const u8 *key, u16 klen) {
    u32 pn = bt->root_page;
    u32 depth = 0;
    for (;;) {
        if (depth >= MAX_TREE_DEPTH) return 1;
        page_buf *b;
        unsigned long rc = cache_pin(&b, bt, pn);
        if (rc) return rc;
        u8 type = ph_type(b->data);
        if (type == PAGE_TYPE_LEAF) {
            path[depth].page_no = pn;
            path[depth].slot = 0;
            *out_depth = depth + 1;
            cache_unpin(b);
            return 0;
        }
        if (type != PAGE_TYPE_INTERNAL) { cache_unpin(b); return 2; }
        u16 slot;
        u32 child = internal_descend(b->data, key, klen, &slot);
        path[depth].page_no = pn;
        path[depth].slot = slot;
        cache_unpin(b);
        pn = child;
        depth++;
    }
}

/* ---- insert helpers ---- */

static unsigned long build_leaf_cell_for(dbtree *bt,
                                          u8 *out, u16 *out_size,
                                          const u8 *key, u16 klen,
                                          const u8 *val, u32 vlen) {
    u32 inline_cell_size = 2u + (u32)klen + 1u + 2u + vlen;
    if (vlen <= 0xFFFFu && inline_cell_size <= INLINE_VALUE_THRESHOLD) {
        u16 sz = build_leaf_cell_inline(out, key, klen, val, (u16)vlen);
        *out_size = sz;
        return 0;
    }
    /* Overflow path */
    u16 prefix_len = (u16)(vlen < OVERFLOW_INLINE_PREFIX ? vlen : OVERFLOW_INLINE_PREFIX);
    u32 first_ov = NO_PAGE;
    if (vlen > prefix_len) {
        unsigned long rc = overflow_chain_alloc(&first_ov, bt, val, vlen, prefix_len);
        if (rc) return rc;
    }
    u16 sz = build_leaf_cell_overflow(out, key, klen,
                                       vlen, first_ov,
                                       val, prefix_len);
    *out_size = sz;
    return 0;
}

/* Extract a full leaf key into a caller-provided buffer. Used during splits
 * so we can bubble the separator up (its source cell may move during the
 * split). Caller sizes buf >= klen_max. */
static void copy_leaf_key(u8 *dst, u16 *dst_len,
                           const u8 *page, u16 slot_idx) {
    const u8 *cell = page + slot_get(page, slot_idx);
    u16 klen = leaf_cell_key_len(cell);
    memcpy(dst, leaf_cell_key(cell), klen);
    *dst_len = klen;
}

/* Split a leaf page that cannot fit the new cell. Returns the new right-side
 * page number via *out_right and the separator key (smallest key in right) via
 * *sep_key / *sep_key_len.
 *
 * The cell to insert has been computed by the caller; we insert it into
 * whichever half has room after the midpoint redistribution. */
static unsigned long split_leaf(u32 *out_right,
                                 u8 *sep_key, u16 *sep_key_len,
                                 dbtree *bt, u32 left_pn,
                                 u16 insert_slot, const u8 *new_cell, u16 new_cell_size) {
    /* Allocate right sibling */
    u32 right_pn;
    unsigned long rc = alloc_page(&right_pn, bt);
    if (rc) return rc;

    /* Collect all cells + choose midpoint */
    page_buf *lb;
    rc = cache_pin(&lb, bt, left_pn);
    if (rc) return rc;
    u16 old_nslots = ph_num_slots(lb->data);
    u16 total = (u16)(old_nslots + 1);
    u16 mid = (u16)(total / 2);

    /* Build two local arrays of cell pointers (into left page or pointing at new_cell) */
    typedef struct { const u8 *src; u16 size; } cell_ref;
    cell_ref cells[4096 / 4];  /* at most ~1024 cells; we have far fewer */
    for (u16 i = 0, j = 0; i < total; i++) {
        if (i == insert_slot) {
            cells[i].src  = new_cell;
            cells[i].size = new_cell_size;
        } else {
            u16 src_slot = (u16)(i < insert_slot ? i : i - 1);
            u16 off = slot_get(lb->data, src_slot);
            cells[i].src  = lb->data + off;
            cells[i].size = leaf_cell_size(lb->data + off);
            (void)j;
        }
    }

    /* Separator = first key of right half */
    {
        const u8 *sep_cell = cells[mid].src;
        u16 skl = leaf_cell_key_len(sep_cell);
        memcpy(sep_key, leaf_cell_key(sep_cell), skl);
        *sep_key_len = skl;
    }

    /* Save old left links */
    u32 old_next = ph_extra(lb->data);
    u32 old_prev = ph_extra2(lb->data);

    /* Pin right */
    page_buf *rb;
    rc = cache_pin(&rb, bt, right_pn);
    if (rc) { cache_unpin(lb); return rc; }

    /* Take a snapshot of left cells BEFORE we reinit (since cells[].src may
     * point into lb->data for non-new cells). */
    u8 snapshot[DBTREE_PAGE_SIZE];
    memcpy(snapshot, lb->data, DBTREE_PAGE_SIZE);
    /* Rewire cell_ref pointers that targeted the left page into the snapshot */
    for (u16 i = 0; i < total; i++) {
        if (cells[i].src != new_cell) {
            /* cells[i].src lies inside lb->data; redirect to snapshot */
            cells[i].src = snapshot + (cells[i].src - lb->data);
        }
    }

    /* Reinit left as empty leaf, refill with [0..mid) */
    page_init_leaf(lb->data);
    for (u16 i = 0; i < mid; i++) {
        page_insert_cell(lb->data, i, cells[i].src, cells[i].size);
    }

    /* Init right leaf, refill with [mid..total) */
    page_init_leaf(rb->data);
    for (u16 i = mid, j = 0; i < total; i++, j++) {
        page_insert_cell(rb->data, j, cells[i].src, cells[i].size);
    }

    /* Sibling pointers: left.prev stays, left.next = right, right.prev = left,
     * right.next = old_next. If old_next existed, update its prev. */
    ph_set_extra(lb->data, right_pn);
    ph_set_extra2(lb->data, old_prev);
    ph_set_extra(rb->data, old_next);
    ph_set_extra2(rb->data, left_pn);

    cache_mark_dirty(lb);
    cache_mark_dirty(rb);
    cache_unpin(lb);
    cache_unpin(rb);

    if (old_next != NO_PAGE) {
        page_buf *nb;
        rc = cache_pin(&nb, bt, old_next);
        if (rc) return rc;
        ph_set_extra2(nb->data, right_pn);
        cache_mark_dirty(nb);
        cache_unpin(nb);
    }

    *out_right = right_pn;
    return 0;
}

/* Split an internal page. The new cell (the bubbled-up separator + its right
 * child) is inserted at `insert_slot`. Returns new right-side page and the
 * separator key that must bubble further up. */
static unsigned long split_internal(u32 *out_right,
                                     u8 *sep_key, u16 *sep_key_len,
                                     dbtree *bt, u32 left_pn,
                                     u16 insert_slot, const u8 *new_cell, u16 new_cell_size) {
    u32 right_pn;
    unsigned long rc = alloc_page(&right_pn, bt);
    if (rc) return rc;

    page_buf *lb;
    rc = cache_pin(&lb, bt, left_pn);
    if (rc) return rc;
    u16 old_nslots = ph_num_slots(lb->data);
    u16 total = (u16)(old_nslots + 1);

    typedef struct { const u8 *src; u16 size; u32 child; u16 klen; const u8 *key; } icell;
    icell cells[4096 / 6];
    for (u16 i = 0; i < total; i++) {
        if (i == insert_slot) {
            cells[i].src   = new_cell;
            cells[i].size  = new_cell_size;
            cells[i].child = internal_cell_child(new_cell);
            cells[i].klen  = internal_cell_key_len(new_cell);
            cells[i].key   = internal_cell_key(new_cell);
        } else {
            u16 src_slot = (u16)(i < insert_slot ? i : i - 1);
            u16 off = slot_get(lb->data, src_slot);
            const u8 *c = lb->data + off;
            cells[i].src   = c;
            cells[i].size  = internal_cell_size(c);
            cells[i].child = internal_cell_child(c);
            cells[i].klen  = internal_cell_key_len(c);
            cells[i].key   = internal_cell_key(c);
        }
    }

    /* For internal split the middle key is promoted up and NOT kept in either
     * child (B+-tree semantics: middle key's child becomes rightmost of left). */
    u16 mid = (u16)(total / 2);
    memcpy(sep_key, cells[mid].key, cells[mid].klen);
    *sep_key_len = cells[mid].klen;
    u32 mid_child = cells[mid].child;
    u32 old_rightmost = ph_extra(lb->data);

    page_buf *rb;
    rc = cache_pin(&rb, bt, right_pn);
    if (rc) { cache_unpin(lb); return rc; }

    /* Snapshot so left page re-init doesn't invalidate cells[] pointers */
    u8 snapshot[DBTREE_PAGE_SIZE];
    memcpy(snapshot, lb->data, DBTREE_PAGE_SIZE);
    for (u16 i = 0; i < total; i++) {
        if (cells[i].src != new_cell) {
            cells[i].src = snapshot + (cells[i].src - lb->data);
            cells[i].key = snapshot + (cells[i].key - lb->data);
        }
    }

    /* Left: slots 0..mid-1, rightmost = mid_child */
    page_init_internal(lb->data, mid_child);
    for (u16 i = 0; i < mid; i++) {
        page_insert_cell(lb->data, i, cells[i].src, cells[i].size);
    }
    /* Right: slots mid+1..total-1, rightmost = old_rightmost */
    page_init_internal(rb->data, old_rightmost);
    for (u16 i = (u16)(mid + 1), j = 0; i < total; i++, j++) {
        page_insert_cell(rb->data, j, cells[i].src, cells[i].size);
    }

    cache_mark_dirty(lb);
    cache_mark_dirty(rb);
    cache_unpin(lb);
    cache_unpin(rb);

    *out_right = right_pn;
    return 0;
}

/* Insert a cell bubbled up from a child split. When inserting at `slot`, the
 * cell form is (key, child). The original child at that slot remains pointing
 * to the left side (it's already correct — it was the page that was split).
 * The new cell's child is the NEW right sibling. */
static unsigned long insert_into_internal(dbtree *bt,
                                           path_entry *path, u32 depth,
                                           u32 level,
                                           const u8 *sep_key, u16 sep_len,
                                           u32 right_child) {
    /* level == depth means we need a new root. */
    if (level == (u32)-1) return 1;  /* should never happen */

    u8 cell[DBTREE_PAGE_SIZE];
    u16 cell_size = build_internal_cell(cell, sep_key, sep_len, right_child);

    for (;;) {
        if (level == 0 && depth == 1) {
            /* Root was a leaf that we just split; create a fresh internal root. */
            u32 new_root;
            unsigned long rc = alloc_page(&new_root, bt);
            if (rc) return rc;
            page_buf *nb;
            rc = cache_pin(&nb, bt, new_root);
            if (rc) return rc;
            page_init_internal(nb->data, right_child);
            page_insert_cell(nb->data, 0, cell, cell_size);
            cache_mark_dirty(nb);
            cache_unpin(nb);
            /* Separator key: smallest key in right child (we received it) */
            /* But we need (key, LEFT child). Fix: rightmost = right_child,
             * and the one cell should be (sep_key, left_child). We passed it
             * the wrong way. Let's redo. */
            /* Actually: cells in internal pages follow the rule cell.child =
             * subtree with keys < cell.key. Left subtree keys are < sep_key,
             * right subtree keys are >= sep_key. So cell should be
             * (sep_key, LEFT child). Rightmost = right child. */
            u32 left_child = path[0].page_no;  /* the original root that got split */

            /* Rewrite: discard what we just built, redo correctly. */
            page_init_internal(nb->data, right_child);
            u8 fixed_cell[DBTREE_PAGE_SIZE];
            u16 fixed_size = build_internal_cell(fixed_cell, sep_key, sep_len, left_child);
            page_insert_cell(nb->data, 0, fixed_cell, fixed_size);
            cache_mark_dirty(nb);

            bt->root_page = new_root;
            bt->dirty_header = 1;
            return 0;
        }

        u32 pn = path[level].page_no;
        u16 insert_slot = path[level].slot;  /* parent slot we descended through */
        page_buf *b;
        unsigned long rc = cache_pin(&b, bt, pn);
        if (rc) return rc;

        /* The bubbled cell should be inserted at position `insert_slot`, BUT
         * with the original cell's left child unchanged. In our layout, we
         * insert a new cell whose child is the LEFT half; the slot at
         * insert_slot (if it existed) previously pointed to the (now split)
         * page as its left child — that child ref is still valid because the
         * left page is reused. So we need the new cell's child to be the
         * LEFT child (the split page), and shift: after insertion, the slot
         * at insert_slot has key=sep_key, child=left_page; slots >= insert_slot+1
         * are shifted right. The rightmost child / slot at insert_slot+1
         * originally pointed somewhere, which is fine. */
        /* Adjust: modify cell to use LEFT child (path[level+1].page_no), but
         * path[level+1] is the page that was split, so it IS the left child. */
        u32 left_child_for_insert = path[level + 1].page_no;
        u8 fixed_cell[DBTREE_PAGE_SIZE];
        u16 fixed_size = build_internal_cell(fixed_cell, sep_key, sep_len, left_child_for_insert);

        /* Does the cell fit? */
        u16 need = (u16)(fixed_size + 2);  /* +2 for slot entry */
        if (page_free_space(b->data) >= need) {
            /* If we descended via rightmost (slot == nslots), the cell goes
             * at position nslots (which is fine — appended). Otherwise at
             * insert_slot. But we must also ensure the child pointer semantics
             * remain correct: before insertion, the subtree for the split
             * page was reachable via either slot[insert_slot].child OR
             * ph_extra (if insert_slot == nslots). After insertion, that
             * reachability must be preserved. */
            u16 nslots_before = ph_num_slots(b->data);
            if (insert_slot == nslots_before) {
                /* We descended via rightmost. The split page was the
                 * rightmost child. After insert at position nslots_before,
                 * the cell key=sep_key, child=left_page. Rightmost becomes
                 * right_child. */
                page_insert_cell(b->data, insert_slot, fixed_cell, fixed_size);
                ph_set_extra(b->data, right_child);
            } else {
                /* Descended via slot[insert_slot] which pointed at left_page.
                 * After insert, we want:
                 *   slot[insert_slot] = (sep_key, left_page)   <-- new
                 *   slot[insert_slot+1] = (old_key, right_child) <-- was old slot[insert_slot] updated
                 * But we only get one insert per call — and we need to replace
                 * the child of the *existing* old slot[insert_slot] with
                 * right_child.
                 * Let's: (a) modify old slot[insert_slot].child = right_child
                 *        (b) insert new cell at slot insert_slot (shifts old one right). */
                u16 off = slot_get(b->data, insert_slot);
                u8 *old_cell = b->data + off;
                u16 old_klen = internal_cell_key_len(old_cell);
                put_u32(old_cell + 2 + old_klen, right_child);
                page_insert_cell(b->data, insert_slot, fixed_cell, fixed_size);
            }
            cache_mark_dirty(b);
            cache_unpin(b);
            return 0;
        }

        /* Doesn't fit: split internal. We first need to apply the
         * "child-of-old-slot := right_child" update logically before splitting.
         * Easiest: mutate the existing cell in-place, then split with the new
         * cell inserted at position insert_slot. */
        u16 nslots_before = ph_num_slots(b->data);
        if (insert_slot == nslots_before) {
            /* Update rightmost-child to right_child, then split inserting
             * at position nslots_before. */
            ph_set_extra(b->data, right_child);
        } else {
            u16 off = slot_get(b->data, insert_slot);
            u8 *old_cell = b->data + off;
            u16 old_klen = internal_cell_key_len(old_cell);
            put_u32(old_cell + 2 + old_klen, right_child);
        }
        cache_mark_dirty(b);

        /* Now split. */
        u8 new_sep[MAX_KEY_LEN];
        u16 new_sep_len = 0;
        u32 new_right;
        cache_unpin(b);
        unsigned long rcs = split_internal(&new_right, new_sep, &new_sep_len,
                                           bt, pn, insert_slot, fixed_cell, fixed_size);
        if (rcs) return rcs;

        if (level == 0) {
            /* Split propagated to root; create a new internal root. */
            u32 new_root;
            rc = alloc_page(&new_root, bt);
            if (rc) return rc;
            page_buf *nb;
            rc = cache_pin(&nb, bt, new_root);
            if (rc) return rc;
            page_init_internal(nb->data, new_right);
            u8 cc[DBTREE_PAGE_SIZE];
            u16 cs = build_internal_cell(cc, new_sep, new_sep_len, pn);
            page_insert_cell(nb->data, 0, cc, cs);
            cache_mark_dirty(nb);
            cache_unpin(nb);
            bt->root_page = new_root;
            bt->dirty_header = 1;
            return 0;
        }

        /* Continue bubbling up with new separator + right_child being the
         * just-created sibling. */
        sep_key = NULL;  /* prevent aliasing */
        memcpy(cell /* reuse buffer */, new_sep, new_sep_len);
        sep_key = cell;
        sep_len = new_sep_len;
        right_child = new_right;
        level--;
        (void)cell_size;  /* recomputed in next loop */
    }
}

/* ---- public: dbtree_put ---- */

static unsigned long dbtree_put_impl(dbtree *bt,
                                       const u8 *key, u64 key_len,
                                       const u8 *value, u64 val_len) {
    if (!bt) return 1;
    if (!key) return 2;
    if (val_len > 0 && !value) return 3;
    if (key_len == 0 || key_len > MAX_KEY_LEN) return 6;
    if (val_len > 0xFFFFFFFFu) return 7;

    /* Build the leaf cell (allocates overflow chain as needed). */
    u8 cell_buf[DBTREE_PAGE_SIZE];
    u16 cell_size = 0;
    unsigned long rc = build_leaf_cell_for(bt, cell_buf, &cell_size,
                                            key, (u16)key_len,
                                            value, (u32)val_len);
    if (rc) return 4;

    /* If cell doesn't fit even on an empty page, reject. */
    u16 max_cell = (u16)(DBTREE_PAGE_SIZE - DBTREE_PAGE_HDR_SIZE - 2);
    if (cell_size > max_cell) return 8;

    /* Descend to leaf. */
    path_entry path[MAX_TREE_DEPTH];
    u32 depth = 0;
    rc = find_leaf(path, &depth, bt, key, (u16)key_len);
    if (rc) return 5;

    u32 leaf_pn = path[depth - 1].page_no;
    page_buf *lb;
    rc = cache_pin(&lb, bt, leaf_pn);
    if (rc) return 5;

    /* Replace-if-exists: delete old cell (and its overflow chain) first. */
    int found = 0;
    u16 pos = leaf_search(lb->data, key, (u16)key_len, &found);
    if (found) {
        const u8 *old_cell = lb->data + slot_get(lb->data, pos);
        u32 old_ov = NO_PAGE;
        if (leaf_cell_flag(old_cell) == CELL_FLAG_OVERFLOW) {
            old_ov = leaf_cell_overflow_first(old_cell);
        }
        page_delete_slot(lb->data, pos);
        cache_mark_dirty(lb);
        if (old_ov != NO_PAGE) {
            cache_unpin(lb);
            unsigned long rcf = overflow_chain_free(bt, old_ov);
            if (rcf) return 9;
            rc = cache_pin(&lb, bt, leaf_pn);
            if (rc) return 5;
        }
        bt->num_keys--;
    }

    /* Try to insert into the leaf. */
    u16 need = (u16)(cell_size + 2);
    if (page_free_space(lb->data) >= need) {
        page_insert_cell(lb->data, pos, cell_buf, cell_size);
        cache_mark_dirty(lb);
        cache_unpin(lb);
        bt->num_keys++;
        return 0;
    }

    /* Leaf needs to split. */
    cache_unpin(lb);

    u8 sep_key[MAX_KEY_LEN];
    u16 sep_len = 0;
    u32 new_right;
    rc = split_leaf(&new_right, sep_key, &sep_len,
                    bt, leaf_pn, pos, cell_buf, cell_size);
    if (rc) return 10;
    bt->num_keys++;

    /* Bubble separator up. Special case: tree had a single leaf as root. */
    if (depth == 1) {
        /* Root-was-leaf split: create internal root. */
        u32 new_root;
        rc = alloc_page(&new_root, bt);
        if (rc) return 11;
        page_buf *nb;
        rc = cache_pin(&nb, bt, new_root);
        if (rc) return 11;
        page_init_internal(nb->data, new_right);
        u8 cc[DBTREE_PAGE_SIZE];
        u16 cs = build_internal_cell(cc, sep_key, sep_len, leaf_pn);
        page_insert_cell(nb->data, 0, cc, cs);
        cache_mark_dirty(nb);
        cache_unpin(nb);
        bt->root_page = new_root;
        bt->dirty_header = 1;
        return 0;
    }

    /* Propagate up through internal parents. The split was at the leaf level
     * (depth-1); its parent is at level depth-2. */
    return insert_into_internal(bt, path, depth, depth - 2,
                                 sep_key, sep_len, new_right);
}

/* ---- public: dbtree_get ---- */

static unsigned long dbtree_get_impl(u8 **out, u64 *out_len,
                                       dbtree *bt,
                                       const u8 *key, u64 key_len) {
    if (!out) return 1;
    if (!out_len) return 2;
    if (!bt) return 3;
    if (!key) return 4;
    if (key_len == 0 || key_len > MAX_KEY_LEN) return 5;

    path_entry path[MAX_TREE_DEPTH];
    u32 depth = 0;
    unsigned long rc = find_leaf(path, &depth, bt, key, (u16)key_len);
    if (rc) return 7;

    page_buf *b;
    rc = cache_pin(&b, bt, path[depth - 1].page_no);
    if (rc) return 7;
    int found = 0;
    u16 pos = leaf_search(b->data, key, (u16)key_len, &found);
    if (!found) { cache_unpin(b); return 5; }

    const u8 *cell = b->data + slot_get(b->data, pos);
    u8 flag = leaf_cell_flag(cell);
    if (flag == CELL_FLAG_INLINE) {
        u16 vlen = leaf_cell_inline_vlen(cell);
        u8 *buf = (u8 *)malloc(vlen ? vlen : 1);
        if (!buf) { cache_unpin(b); return 6; }
        if (vlen) memcpy(buf, leaf_cell_inline_value(cell), vlen);
        *out = buf;
        *out_len = vlen;
        cache_unpin(b);
        return 0;
    } else {
        u32 total = leaf_cell_overflow_total(cell);
        u32 first_ov = leaf_cell_overflow_first(cell);
        u16 prefix_len = leaf_cell_overflow_prefix_len(cell);
        u8 *buf = (u8 *)malloc(total ? total : 1);
        if (!buf) { cache_unpin(b); return 6; }
        if (prefix_len) memcpy(buf, leaf_cell_overflow_prefix(cell), prefix_len);
        cache_unpin(b);
        if (total > prefix_len) {
            rc = overflow_chain_read(buf + prefix_len, total - prefix_len, bt, first_ov);
            if (rc) { free(buf); return 7; }
        }
        *out = buf;
        *out_len = total;
        return 0;
    }
}

/* ---- public: dbtree_delete ---- */

static unsigned long dbtree_delete_impl(dbtree *bt,
                                          const u8 *key, u64 key_len) {
    if (!bt) return 1;
    if (!key) return 2;
    if (key_len == 0 || key_len > MAX_KEY_LEN) return 3;

    path_entry path[MAX_TREE_DEPTH];
    u32 depth = 0;
    unsigned long rc = find_leaf(path, &depth, bt, key, (u16)key_len);
    if (rc) return 4;

    page_buf *b;
    rc = cache_pin(&b, bt, path[depth - 1].page_no);
    if (rc) return 4;
    int found = 0;
    u16 pos = leaf_search(b->data, key, (u16)key_len, &found);
    if (!found) { cache_unpin(b); return 3; }

    const u8 *cell = b->data + slot_get(b->data, pos);
    u32 old_ov = NO_PAGE;
    if (leaf_cell_flag(cell) == CELL_FLAG_OVERFLOW) {
        old_ov = leaf_cell_overflow_first(cell);
    }
    page_delete_slot(b->data, pos);
    cache_mark_dirty(b);
    cache_unpin(b);

    if (old_ov != NO_PAGE) {
        rc = overflow_chain_free(bt, old_ov);
        if (rc) return 4;
    }
    bt->num_keys--;
    return 0;
}

/* ---- public: dbtree_open / close ---- */

static unsigned long init_empty_tree(dbtree *bt) {
    /* page 0 is file header; write zero-filled first, then header,
     * then allocate page 1 as an empty leaf root. */
    u8 zero[DBTREE_PAGE_SIZE];
    memset(zero, 0, sizeof(zero));
    bt->num_pages = 1;  /* page 0 reserved */
    bt->free_head = NO_PAGE;
    bt->root_page = 0;  /* will set after alloc */
    unsigned long rc = dbtree_write_page(bt, 0, zero);
    if (rc) return rc;

    u32 root;
    rc = alloc_page(&root, bt);
    if (rc) return rc;
    page_buf *b;
    rc = cache_pin(&b, bt, root);
    if (rc) return rc;
    page_init_leaf(b->data);
    cache_mark_dirty(b);
    cache_unpin(b);
    bt->root_page = root;
    bt->dirty_header = 1;
    return write_file_header(bt);
}

/* Phase D lock helpers. Reads take the rwlock's read side — multiple
 * concurrent readers allowed; writes take the write side for exclusive
 * access. Cache-internal serialisation (LRU, pin/unpin, eviction,
 * file I/O) is handled separately by cache_mutex inside cache_pin. */
#define BT_RLOCK(bt)   do { if ((bt)->lock_initialised) rwlock_read_lock (&(bt)->tree_rwlock); } while (0)
#define BT_RUNLOCK(bt) do { if ((bt)->lock_initialised) rwlock_read_unlock(&(bt)->tree_rwlock); } while (0)
#define BT_WLOCK(bt)   do { if ((bt)->lock_initialised) rwlock_write_lock (&(bt)->tree_rwlock); } while (0)
#define BT_WUNLOCK(bt) do { if ((bt)->lock_initialised) rwlock_write_unlock(&(bt)->tree_rwlock); } while (0)

APENNINES_API unsigned long dbtree_open(dbtree **out, const char *path) {
    if (!out) return 1;
    if (!path) return 2;

    dbtree *bt = (dbtree *)calloc(1, sizeof(dbtree));
    if (!bt) return 4;

    /* Try to open existing; if fails, create new. */
    FILE *fp = fopen(path, "r+b");
    int created = 0;
    if (!fp) {
        fp = fopen(path, "w+b");
        if (!fp) { free(bt); return 3; }
        created = 1;
    }
    bt->fp = fp;
    size_t plen = strlen(path);
    bt->path = (char *)malloc(plen + 1);
    if (!bt->path) { fclose(fp); free(bt); return 4; }
    memcpy(bt->path, path, plen + 1);

    unsigned long rc = cache_init(&bt->cache, DEFAULT_CACHE_PAGES);
    if (rc) { fclose(fp); free(bt->path); free(bt); return 4; }

    if (rwlock_create(&bt->tree_rwlock) != 0) {
        cache_free(&bt->cache);
        fclose(fp); free(bt->path); free(bt);
        return 4;
    }
    if (mutex_create(&bt->cache_mutex) != 0) {
        rwlock_destroy(&bt->tree_rwlock);
        cache_free(&bt->cache);
        fclose(fp); free(bt->path); free(bt);
        return 4;
    }
    bt->lock_initialised = 1;

    if (created) {
        rc = init_empty_tree(bt);
        if (rc) {
            cache_free(&bt->cache);
            fclose(fp);
            free(bt->path);
            free(bt);
            return 3;
        }
    } else {
        /* Check file size; if empty (0 bytes), treat as fresh. */
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        if (sz < (long)DBTREE_PAGE_SIZE) {
            rc = init_empty_tree(bt);
            if (rc) {
                cache_free(&bt->cache);
                fclose(fp);
                free(bt->path);
                free(bt);
                return 3;
            }
        } else {
            rc = read_file_header(bt);
            if (rc) {
                cache_free(&bt->cache);
                fclose(fp);
                free(bt->path);
                free(bt);
                return 5;
            }
        }
    }

    /* Compute num_keys by scanning leaves (could persist it in header, TODO) */
    bt->num_keys = 0;
    {
        u32 pn = bt->root_page;
        for (;;) {
            page_buf *b;
            if (cache_pin(&b, bt, pn) != 0) break;
            u8 t = ph_type(b->data);
            if (t == PAGE_TYPE_LEAF) {
                /* Walk the leaf chain forward */
                cache_unpin(b);
                u32 lp = pn;
                while (lp != NO_PAGE) {
                    page_buf *lb;
                    if (cache_pin(&lb, bt, lp) != 0) break;
                    bt->num_keys += ph_num_slots(lb->data);
                    u32 next = ph_extra(lb->data);
                    cache_unpin(lb);
                    lp = next;
                }
                break;
            }
            /* Go to leftmost child */
            u32 child;
            if (ph_num_slots(b->data) > 0) {
                const u8 *cell = b->data + slot_get(b->data, 0);
                child = internal_cell_child(cell);
            } else {
                child = ph_extra(b->data);
            }
            cache_unpin(b);
            pn = child;
        }
    }

    *out = bt;
    return 0;
}

APENNINES_API unsigned long dbtree_close(dbtree *bt) {
    if (!bt) return 1;
    BT_WLOCK(bt);
    unsigned long rc = 0;
    if (bt->dirty_header) {
        unsigned long r2 = write_file_header(bt);
        if (r2) rc = 2;
    }
    unsigned long r3 = cache_flush_all(bt);
    if (r3) rc = 2;
    if (bt->fp) fflush(bt->fp);
    cache_free(&bt->cache);
    if (bt->fp) fclose(bt->fp);
    free(bt->path);
    if (bt->lock_initialised) {
        rwlock_write_unlock(&bt->tree_rwlock);
        rwlock_destroy(&bt->tree_rwlock);
        mutex_destroy(&bt->cache_mutex);
    }
    free(bt);
    return rc;
}

static unsigned long dbtree_flush_impl(dbtree *bt) {
    if (!bt) return 1;
    if (bt->dirty_header) {
        if (write_file_header(bt) != 0) return 2;
    }
    if (cache_flush_all(bt) != 0) return 2;
    if (fflush(bt->fp) != 0) return 2;
    return 0;
}

static unsigned long dbtree_sync_impl(dbtree *bt) {
    if (!bt) return 1;
    unsigned long rc = dbtree_flush(bt);
    if (rc) return 2;
#if defined(_WIN32)
    /* Windows: fflush already pushes to OS; we'd need _commit(fd) for full. */
    if (fflush(bt->fp) != 0) return 3;
#else
    if (fflush(bt->fp) != 0) return 3;
#endif
    return 0;
}

static unsigned long dbtree_cache_pages_impl(dbtree *bt, u32 n) {
    if (!bt) return 1;
    if (n == 0 || n > 65536) return 2;
    /* Only valid on freshly opened dbtree (no dirty pages). */
    unsigned long rc = cache_flush_all(bt);
    if (rc) return 3;
    cache_free(&bt->cache);
    return cache_init(&bt->cache, n);
}

static unsigned long dbtree_stats_impl(dbtree *bt,
                                         u64 *out_num_pages,
                                         u64 *out_num_keys,
                                         u64 *out_cache_hits,
                                         u64 *out_cache_misses) {
    if (!bt) return 1;
    if (out_num_pages)    *out_num_pages    = bt->num_pages;
    if (out_num_keys)     *out_num_keys     = bt->num_keys;
    if (out_cache_hits)   *out_cache_hits   = bt->cache.hits;
    if (out_cache_misses) *out_cache_misses = bt->cache.misses;
    return 0;
}

/* ---- cursor ---- */

static unsigned long cursor_copy_key(dbtree_cursor *c) {
    page_buf *b;
    unsigned long rc = cache_pin(&b, c->bt, c->leaf_page);
    if (rc) { c->valid = 0; return rc; }
    if (c->slot_idx >= ph_num_slots(b->data)) { cache_unpin(b); c->valid = 0; return 1; }
    const u8 *cell = b->data + slot_get(b->data, c->slot_idx);
    u16 klen = leaf_cell_key_len(cell);
    if (klen > c->key_cap) {
        u8 *nb = (u8 *)realloc(c->key_buf, klen);
        if (!nb) { cache_unpin(b); return 2; }
        c->key_buf = nb;
        c->key_cap = klen;
    }
    memcpy(c->key_buf, leaf_cell_key(cell), klen);
    c->key_len = klen;
    cache_unpin(b);
    return 0;
}

/* Find the leftmost leaf page reachable from page `root_pn`. */
static unsigned long descend_leftmost(u32 *out_leaf, dbtree *bt, u32 start) {
    u32 pn = start;
    for (int d = 0; d < MAX_TREE_DEPTH; d++) {
        page_buf *b;
        unsigned long rc = cache_pin(&b, bt, pn);
        if (rc) return rc;
        if (ph_type(b->data) == PAGE_TYPE_LEAF) {
            *out_leaf = pn;
            cache_unpin(b);
            return 0;
        }
        u32 child;
        if (ph_num_slots(b->data) > 0) {
            const u8 *cell = b->data + slot_get(b->data, 0);
            child = internal_cell_child(cell);
        } else {
            child = ph_extra(b->data);
        }
        cache_unpin(b);
        pn = child;
    }
    return 2;
}

static unsigned long descend_rightmost(u32 *out_leaf, dbtree *bt, u32 start) {
    u32 pn = start;
    for (int d = 0; d < MAX_TREE_DEPTH; d++) {
        page_buf *b;
        unsigned long rc = cache_pin(&b, bt, pn);
        if (rc) return rc;
        if (ph_type(b->data) == PAGE_TYPE_LEAF) {
            *out_leaf = pn;
            cache_unpin(b);
            return 0;
        }
        u32 child = ph_extra(b->data);  /* rightmost */
        cache_unpin(b);
        pn = child;
    }
    return 2;
}

/* Forward declarations so dbtree_seek_impl can chain to the cursor
 * advance helpers without bouncing through the public locked wrappers
 * (which would try to re-acquire the already-held mutex). */
static unsigned long dbtree_cursor_next_impl(dbtree_cursor *c);
static unsigned long dbtree_cursor_prev_impl(dbtree_cursor *c);

static unsigned long dbtree_seek_impl(dbtree_cursor **out, dbtree *bt,
                                        const u8 *key, u64 key_len,
                                        dbtree_seek_mode mode) {
    if (!out) return 1;
    if (!bt) return 2;

    dbtree_cursor *c = (dbtree_cursor *)calloc(1, sizeof(dbtree_cursor));
    if (!c) return 5;
    c->bt = bt;
    c->valid = 0;

    if (mode == DBTREE_SEEK_FIRST) {
        u32 leaf;
        if (descend_leftmost(&leaf, bt, bt->root_page) != 0) { free(c); return 6; }
        c->leaf_page = leaf;
        c->slot_idx = 0;
        page_buf *b;
        if (cache_pin(&b, bt, leaf) != 0) { free(c); return 6; }
        u16 n = ph_num_slots(b->data);
        cache_unpin(b);
        if (n == 0) { free(c); return 6; }
        c->valid = 1;
        if (cursor_copy_key(c) != 0) { free(c->key_buf); free(c); return 5; }
        *out = c;
        return 0;
    }
    if (mode == DBTREE_SEEK_LAST) {
        u32 leaf;
        if (descend_rightmost(&leaf, bt, bt->root_page) != 0) { free(c); return 6; }
        c->leaf_page = leaf;
        page_buf *b;
        if (cache_pin(&b, bt, leaf) != 0) { free(c); return 6; }
        u16 n = ph_num_slots(b->data);
        cache_unpin(b);
        if (n == 0) { free(c); return 6; }
        c->slot_idx = (u32)(n - 1);
        c->valid = 1;
        if (cursor_copy_key(c) != 0) { free(c->key_buf); free(c); return 5; }
        *out = c;
        return 0;
    }

    if (!key || key_len == 0 || key_len > MAX_KEY_LEN) { free(c); return 4; }

    path_entry path[MAX_TREE_DEPTH];
    u32 depth = 0;
    if (find_leaf(path, &depth, bt, key, (u16)key_len) != 0) { free(c); return 6; }
    u32 leaf = path[depth - 1].page_no;

    page_buf *b;
    if (cache_pin(&b, bt, leaf) != 0) { free(c); return 6; }
    int found = 0;
    u16 pos = leaf_search(b->data, key, (u16)key_len, &found);
    u16 nslots = ph_num_slots(b->data);
    cache_unpin(b);

    c->leaf_page = leaf;
    c->slot_idx = pos;
    c->valid = 0;

    switch (mode) {
    case DBTREE_SEEK_EQ:
        if (found) { c->valid = 1; }
        else       { free(c); return 6; }
        break;
    case DBTREE_SEEK_GE:
        /* pos is the first key >= key; if pos >= nslots, step to next leaf */
        if (pos < nslots) {
            c->slot_idx = pos;
            c->valid = 1;
        } else {
            c->slot_idx = pos;
            if (dbtree_cursor_next_impl(c) != 0) { free(c); return 6; }
        }
        break;
    case DBTREE_SEEK_GT:
        /* pos is insertion pos; exact match => pos+1; non-match => pos */
        c->slot_idx = (u16)(found ? pos + 1 : pos);
        if (c->slot_idx < nslots) {
            c->valid = 1;
        } else {
            /* step to next leaf */
            c->slot_idx = c->slot_idx - 1 >= nslots ? nslots : c->slot_idx;
            c->valid = 1;
            if (dbtree_cursor_next_impl(c) != 0) { free(c); return 6; }
        }
        break;
    case DBTREE_SEEK_LE:
        if (found) { c->slot_idx = pos; c->valid = 1; }
        else if (pos > 0) { c->slot_idx = (u16)(pos - 1); c->valid = 1; }
        else {
            /* step to prev leaf */
            c->slot_idx = 0;
            c->valid = 1;
            if (dbtree_cursor_prev_impl(c) != 0) { free(c); return 6; }
        }
        break;
    case DBTREE_SEEK_LT:
        if (pos > 0) { c->slot_idx = (u16)(pos - 1); c->valid = 1; }
        else {
            c->slot_idx = 0;
            c->valid = 1;
            if (dbtree_cursor_prev_impl(c) != 0) { free(c); return 6; }
        }
        break;
    default:
        free(c);
        return 3;
    }

    if (c->valid) {
        if (cursor_copy_key(c) != 0) { free(c->key_buf); free(c); return 5; }
    }
    *out = c;
    return 0;
}

static unsigned long dbtree_cursor_next_impl(dbtree_cursor *c) {
    if (!c) return 1;
    page_buf *b;
    unsigned long rc = cache_pin(&b, c->bt, c->leaf_page);
    if (rc) { c->valid = 0; return 2; }
    u16 n = ph_num_slots(b->data);
    if (c->slot_idx + 1 < n) {
        c->slot_idx++;
        cache_unpin(b);
        if (cursor_copy_key(c) != 0) { c->valid = 0; return 2; }
        c->valid = 1;
        return 0;
    }
    u32 next = ph_extra(b->data);
    cache_unpin(b);
    while (next != NO_PAGE) {
        rc = cache_pin(&b, c->bt, next);
        if (rc) { c->valid = 0; return 2; }
        u16 nn = ph_num_slots(b->data);
        if (nn > 0) {
            c->leaf_page = next;
            c->slot_idx = 0;
            cache_unpin(b);
            if (cursor_copy_key(c) != 0) { c->valid = 0; return 2; }
            c->valid = 1;
            return 0;
        }
        next = ph_extra(b->data);
        cache_unpin(b);
    }
    c->valid = 0;
    return 2;
}

static unsigned long dbtree_cursor_prev_impl(dbtree_cursor *c) {
    if (!c) return 1;
    if (c->slot_idx > 0) {
        c->slot_idx--;
        if (cursor_copy_key(c) != 0) { c->valid = 0; return 2; }
        c->valid = 1;
        return 0;
    }
    page_buf *b;
    unsigned long rc = cache_pin(&b, c->bt, c->leaf_page);
    if (rc) { c->valid = 0; return 2; }
    u32 prev = ph_extra2(b->data);
    cache_unpin(b);
    while (prev != NO_PAGE) {
        rc = cache_pin(&b, c->bt, prev);
        if (rc) { c->valid = 0; return 2; }
        u16 nn = ph_num_slots(b->data);
        if (nn > 0) {
            c->leaf_page = prev;
            c->slot_idx = (u16)(nn - 1);
            cache_unpin(b);
            if (cursor_copy_key(c) != 0) { c->valid = 0; return 2; }
            c->valid = 1;
            return 0;
        }
        prev = ph_extra2(b->data);
        cache_unpin(b);
    }
    c->valid = 0;
    return 2;
}

APENNINES_API unsigned long dbtree_cursor_key(const u8 **out, u64 *out_len,
                                              dbtree_cursor *c) {
    if (!out) return 1;
    if (!out_len) return 2;
    if (!c) return 3;
    if (!c->valid) return 4;
    *out = c->key_buf;
    *out_len = c->key_len;
    return 0;
}

static unsigned long dbtree_cursor_value_impl(u8 **out, u64 *out_len,
                                                dbtree_cursor *c) {
    if (!out) return 1;
    if (!out_len) return 2;
    if (!c) return 3;
    if (!c->valid) return 4;
    page_buf *b;
    unsigned long rc = cache_pin(&b, c->bt, c->leaf_page);
    if (rc) return 6;
    const u8 *cell = b->data + slot_get(b->data, c->slot_idx);
    u8 flag = leaf_cell_flag(cell);
    if (flag == CELL_FLAG_INLINE) {
        u16 vlen = leaf_cell_inline_vlen(cell);
        u8 *buf = (u8 *)malloc(vlen ? vlen : 1);
        if (!buf) { cache_unpin(b); return 5; }
        if (vlen) memcpy(buf, leaf_cell_inline_value(cell), vlen);
        *out = buf;
        *out_len = vlen;
        cache_unpin(b);
        return 0;
    } else {
        u32 total = leaf_cell_overflow_total(cell);
        u32 first_ov = leaf_cell_overflow_first(cell);
        u16 prefix_len = leaf_cell_overflow_prefix_len(cell);
        u8 *buf = (u8 *)malloc(total ? total : 1);
        if (!buf) { cache_unpin(b); return 5; }
        if (prefix_len) memcpy(buf, leaf_cell_overflow_prefix(cell), prefix_len);
        cache_unpin(b);
        if (total > prefix_len) {
            rc = overflow_chain_read(buf + prefix_len, total - prefix_len, c->bt, first_ov);
            if (rc) { free(buf); return 6; }
        }
        *out = buf;
        *out_len = total;
        return 0;
    }
}

APENNINES_API unsigned long dbtree_cursor_close(dbtree_cursor *c) {
    if (!c) return 1;
    free(c->key_buf);
    free(c);
    return 0;
}

/* ================================================================
 *  Lock-wrapping public entry points (Phase B + D).
 *
 *  Writers take the write side of tree_rwlock (put/delete/flush/sync/
 *  cache_pages). Readers take the read side (get/seek/stats/cursor_*).
 *  Concurrent readers parallelise end-to-end; they serialise only
 *  inside cache_pin on cache_mutex, held briefly across the LRU +
 *  file-I/O critical section.
 * ================================================================ */

APENNINES_API unsigned long dbtree_put(dbtree *bt,
                                       const u8 *key, u64 key_len,
                                       const u8 *value, u64 val_len) {
    if (!bt) return 1;
    BT_WLOCK(bt);
    unsigned long rc = dbtree_put_impl(bt, key, key_len, value, val_len);
    BT_WUNLOCK(bt);
    return rc;
}

APENNINES_API unsigned long dbtree_get(u8 **out, u64 *out_len,
                                       dbtree *bt,
                                       const u8 *key, u64 key_len) {
    if (!bt) return 3;
    BT_RLOCK(bt);
    unsigned long rc = dbtree_get_impl(out, out_len, bt, key, key_len);
    BT_RUNLOCK(bt);
    return rc;
}

APENNINES_API unsigned long dbtree_delete(dbtree *bt,
                                          const u8 *key, u64 key_len) {
    if (!bt) return 1;
    BT_WLOCK(bt);
    unsigned long rc = dbtree_delete_impl(bt, key, key_len);
    BT_WUNLOCK(bt);
    return rc;
}

APENNINES_API unsigned long dbtree_flush(dbtree *bt) {
    if (!bt) return 1;
    BT_WLOCK(bt);
    unsigned long rc = dbtree_flush_impl(bt);
    BT_WUNLOCK(bt);
    return rc;
}

APENNINES_API unsigned long dbtree_sync(dbtree *bt) {
    if (!bt) return 1;
    BT_WLOCK(bt);
    unsigned long rc = dbtree_sync_impl(bt);
    BT_WUNLOCK(bt);
    return rc;
}

APENNINES_API unsigned long dbtree_cache_pages(dbtree *bt, u32 n) {
    if (!bt) return 1;
    BT_WLOCK(bt);
    unsigned long rc = dbtree_cache_pages_impl(bt, n);
    BT_WUNLOCK(bt);
    return rc;
}

APENNINES_API unsigned long dbtree_stats(dbtree *bt,
                                         u64 *out_num_pages,
                                         u64 *out_num_keys,
                                         u64 *out_cache_hits,
                                         u64 *out_cache_misses) {
    if (!bt) return 1;
    BT_RLOCK(bt);
    unsigned long rc = dbtree_stats_impl(bt, out_num_pages, out_num_keys,
                                          out_cache_hits, out_cache_misses);
    BT_RUNLOCK(bt);
    return rc;
}

APENNINES_API unsigned long dbtree_seek(dbtree_cursor **out, dbtree *bt,
                                        const u8 *key, u64 key_len,
                                        dbtree_seek_mode mode) {
    if (!bt) return 2;
    BT_RLOCK(bt);
    unsigned long rc = dbtree_seek_impl(out, bt, key, key_len, mode);
    BT_RUNLOCK(bt);
    return rc;
}

APENNINES_API unsigned long dbtree_cursor_next(dbtree_cursor *c) {
    if (!c) return 1;
    BT_RLOCK(c->bt);
    unsigned long rc = dbtree_cursor_next_impl(c);
    BT_RUNLOCK(c->bt);
    return rc;
}

APENNINES_API unsigned long dbtree_cursor_prev(dbtree_cursor *c) {
    if (!c) return 1;
    BT_RLOCK(c->bt);
    unsigned long rc = dbtree_cursor_prev_impl(c);
    BT_RUNLOCK(c->bt);
    return rc;
}

APENNINES_API unsigned long dbtree_cursor_value(u8 **out, u64 *out_len,
                                                dbtree_cursor *c) {
    if (!c) return 3;
    BT_RLOCK(c->bt);
    unsigned long rc = dbtree_cursor_value_impl(out, out_len, c);
    BT_RUNLOCK(c->bt);
    return rc;
}
