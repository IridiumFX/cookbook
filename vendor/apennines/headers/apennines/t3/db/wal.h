#ifndef APENNINES_T3_WAL_H
#define APENNINES_T3_WAL_H

#include "apennines/export.h"
#include "apennines/types.h"

/* ================================================================
 *  WAL — Write-Ahead Log
 *
 *  Append-only log with sequence numbers for crash recovery.
 *  Each entry: [u64 seq][u32 len][u32 crc32][len bytes data]
 * ================================================================ */

typedef struct wal wal;
typedef struct wal_iter wal_iter;

typedef struct {
    u64  seq;           /* sequence number */
    u8  *data;          /* entry data */
    u64  len;           /* entry length */
} wal_entry;

/* wal_create — create or open a WAL at the given file path.
 *   out:    receives WAL handle
 *   path:   file path (null-terminated)
 *
 * Hatches: 1=null out, 2=null path, 3=open/create failed,
 *          4=alloc failure, 5=corrupt log detected */
APENNINES_API unsigned long wal_create(wal **out, const char *path);

/* wal_append — append an entry. Sequence number is auto-assigned.
 *   out_seq:  receives assigned sequence number
 *   w:        WAL handle
 *   data:     entry data
 *   len:      data length
 *
 * Hatches: 1=null out_seq, 2=null w, 3=null data and len > 0,
 *          4=write failed, 5=alloc failure */
APENNINES_API unsigned long wal_append(u64 *out_seq, wal *w,
                                        const u8 *data, u64 len);

/* wal_read — read an entry by sequence number.
 *   out:    receives the entry (caller frees out->data)
 *   w:      WAL handle
 *   seq:    sequence number to read
 *
 * Hatches: 1=null out, 2=null w, 3=seq not found,
 *          4=read failed, 5=CRC mismatch */
APENNINES_API unsigned long wal_read(wal_entry *out, wal *w, u64 seq);

/* wal_iter_create — create an iterator starting at a sequence number.
 *   out:       receives iterator handle
 *   w:         WAL handle
 *   start_seq: starting sequence number (0 = beginning)
 *
 * Hatches: 1=null out, 2=null w, 3=alloc failure */
APENNINES_API unsigned long wal_iter_create(wal_iter **out, wal *w,
                                             u64 start_seq);

/* wal_iter_next — advance iterator and return next entry.
 *   out:    receives next entry (caller frees out->data)
 *   it:     iterator handle
 *
 * Hatches: 1=null out, 2=null it, 3=end of log,
 *          4=read failed, 5=CRC mismatch */
APENNINES_API unsigned long wal_iter_next(wal_entry *out, wal_iter *it);

/* wal_iter_destroy — free iterator.
 *   it:     iterator handle
 *
 * Hatches: 1=null it */
APENNINES_API unsigned long wal_iter_destroy(wal_iter *it);

/* wal_truncate — remove all entries before the given sequence number.
 *   w:           WAL handle
 *   before_seq:  truncate entries with seq < before_seq
 *
 * Hatches: 1=null w, 2=truncation failed */
APENNINES_API unsigned long wal_truncate(wal *w, u64 before_seq);

/* wal_sync — force sync to disk (fsync).
 *   w:    WAL handle
 *
 * Hatches: 1=null w, 2=sync failed */
APENNINES_API unsigned long wal_sync(wal *w);

/* wal_close — close the WAL and free resources.
 *   w:    WAL handle
 *
 * Hatches: 1=null w, 2=close failed */
APENNINES_API unsigned long wal_close(wal *w);

#endif /* APENNINES_T3_WAL_H */
