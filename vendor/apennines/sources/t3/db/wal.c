#include "apennines/t3/db/wal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <pthread.h>
#endif

/* ------------------------------------------------------------------ */
/*  Platform mutex for thread safety                                   */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
typedef CRITICAL_SECTION wal_mutex;
static void wal_mtx_init(wal_mutex *m)    { InitializeCriticalSection(m); }
static void wal_mtx_lock(wal_mutex *m)    { EnterCriticalSection(m); }
static void wal_mtx_unlock(wal_mutex *m)  { LeaveCriticalSection(m); }
static void wal_mtx_destroy(wal_mutex *m) { DeleteCriticalSection(m); }
#else
typedef pthread_mutex_t wal_mutex;
static void wal_mtx_init(wal_mutex *m)    { pthread_mutex_init(m, NULL); }
static void wal_mtx_lock(wal_mutex *m)    { pthread_mutex_lock(m); }
static void wal_mtx_unlock(wal_mutex *m)  { pthread_mutex_unlock(m); }
static void wal_mtx_destroy(wal_mutex *m) { pthread_mutex_destroy(m); }
#endif

/* ------------------------------------------------------------------ */
/*  Platform file abstraction                                          */
/*                                                                    */
/*  On Windows: native Win32 HANDLE via CreateFileA / ReadFile /       */
/*  WriteFile / SetFilePointerEx / FlushFileBuffers. Bypasses MSVCRT   */
/*  which was the source of the STATUS_INVALID_HANDLE crash cookbook   */
/*  reported under civetweb worker-thread load — MSVCRT's FILE*        */
/*  state-sharing across threads races under fseek+fread/fwrite pairs  */
/*  even with per-FILE internal locking, and `_commit(_fileno(fp))`    */
/*  can see a freed fd mid-close. CreateFile + native HANDLE has no    */
/*  hidden shared state and each op atomically consults kernel state.  */
/*                                                                    */
/*  On POSIX: FILE* stays. Not broken, no reason to churn.             */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
typedef HANDLE wal_fd;
#define WAL_FD_INVALID INVALID_HANDLE_VALUE
#else
typedef FILE * wal_fd;
#define WAL_FD_INVALID NULL
#endif

/* Open (or create) for read+append. Seekable for scan on create. */
static unsigned long wal_fd_open_rw(wal_fd *out, const char *path) {
#ifdef _WIN32
    HANDLE h = CreateFileA(
        path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,         /* let other readers peek; we serialise writes via mutex */
        NULL,
        OPEN_ALWAYS,             /* create if missing, open if exists */
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (h == INVALID_HANDLE_VALUE) return 1;
    *out = h;
    return 0;
#else
    FILE *fp = fopen(path, "a+b");
    if (!fp) return 1;
    *out = fp;
    return 0;
#endif
}

/* Open existing for read-only. */
static unsigned long wal_fd_open_ro(wal_fd *out, const char *path) {
#ifdef _WIN32
    HANDLE h = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, /* concurrent appends from the main WAL handle are fine */
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (h == INVALID_HANDLE_VALUE) return 1;
    *out = h;
    return 0;
#else
    FILE *fp = fopen(path, "rb");
    if (!fp) return 1;
    *out = fp;
    return 0;
#endif
}

static unsigned long wal_fd_close(wal_fd fd) {
    if (fd == WAL_FD_INVALID) return 0;
#ifdef _WIN32
    return CloseHandle(fd) ? 0 : 1;
#else
    return fclose(fd) == 0 ? 0 : 1;
#endif
}

/* Read up to `want` bytes; *got receives how many were actually read.
 * Returns 0 always; short reads are not errors — caller checks *got. */
static unsigned long wal_fd_read(wal_fd fd, u8 *buf, u64 want, u64 *got) {
#ifdef _WIN32
    DWORD n = 0;
    if (!ReadFile(fd, buf, (DWORD)want, &n, NULL)) {
        *got = 0;
        return 1;
    }
    *got = (u64)n;
    return 0;
#else
    size_t n = fread(buf, 1, (size_t)want, fd);
    *got = (u64)n;
    /* fread returning short is only an error if ferror; we treat it as EOF */
    return 0;
#endif
}

/* Write all `len` bytes. Partial writes are retried. */
static unsigned long wal_fd_write(wal_fd fd, const u8 *buf, u64 len) {
#ifdef _WIN32
    DWORD written = 0;
    u64   total = 0;
    while (total < len) {
        DWORD chunk = (DWORD)(len - total > 0x40000000u ? 0x40000000u : len - total);
        if (!WriteFile(fd, buf + total, chunk, &written, NULL)) return 1;
        if (written == 0) return 1;
        total += written;
    }
    return 0;
#else
    size_t n = fwrite(buf, 1, (size_t)len, fd);
    return (n == (size_t)len) ? 0 : 1;
#endif
}

static unsigned long wal_fd_seek_set(wal_fd fd, u64 offset) {
#ifdef _WIN32
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    return SetFilePointerEx(fd, li, NULL, FILE_BEGIN) ? 0 : 1;
#else
    return fseek(fd, (long)offset, SEEK_SET) == 0 ? 0 : 1;
#endif
}

static unsigned long wal_fd_seek_cur(wal_fd fd, i64 offset) {
#ifdef _WIN32
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    return SetFilePointerEx(fd, li, NULL, FILE_CURRENT) ? 0 : 1;
#else
    return fseek(fd, (long)offset, SEEK_CUR) == 0 ? 0 : 1;
#endif
}

static unsigned long wal_fd_seek_end(wal_fd fd) {
#ifdef _WIN32
    LARGE_INTEGER li;
    li.QuadPart = 0;
    return SetFilePointerEx(fd, li, NULL, FILE_END) ? 0 : 1;
#else
    return fseek(fd, 0, SEEK_END) == 0 ? 0 : 1;
#endif
}

static unsigned long wal_fd_tell(wal_fd fd, u64 *out) {
#ifdef _WIN32
    LARGE_INTEGER zero, pos;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(fd, zero, &pos, FILE_CURRENT)) return 1;
    *out = (u64)pos.QuadPart;
    return 0;
#else
    long p = ftell(fd);
    if (p < 0) return 1;
    *out = (u64)p;
    return 0;
#endif
}

static unsigned long wal_fd_flush(wal_fd fd) {
#ifdef _WIN32
    return FlushFileBuffers(fd) ? 0 : 1;
#else
    if (fflush(fd) != 0) return 1;
    {
        int d = fileno(fd);
        if (d >= 0) fdatasync(d);
    }
    return 0;
#endif
}

/* ------------------------------------------------------------------ */
/*  WAL structs                                                       */
/* ------------------------------------------------------------------ */

struct wal {
    wal_fd    fp;
    u64       next_seq;
    char      path[512];
    wal_mutex lock;
};

struct wal_iter {
    wal_fd fp;
    u64    next_seq;
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

static void write_u64_le(u8 *dst, u64 v) {
    dst[0] = (u8)(v);
    dst[1] = (u8)(v >> 8);
    dst[2] = (u8)(v >> 16);
    dst[3] = (u8)(v >> 24);
    dst[4] = (u8)(v >> 32);
    dst[5] = (u8)(v >> 40);
    dst[6] = (u8)(v >> 48);
    dst[7] = (u8)(v >> 56);
}

static u32 read_u32_le(const u8 *src) {
    return (u32)src[0]
         | ((u32)src[1] << 8)
         | ((u32)src[2] << 16)
         | ((u32)src[3] << 24);
}

static u64 read_u64_le(const u8 *src) {
    return (u64)src[0]
         | ((u64)src[1] << 8)
         | ((u64)src[2] << 16)
         | ((u64)src[3] << 24)
         | ((u64)src[4] << 32)
         | ((u64)src[5] << 40)
         | ((u64)src[6] << 48)
         | ((u64)src[7] << 56);
}

/* ------------------------------------------------------------------ */
/*  CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320)              */
/* ------------------------------------------------------------------ */

static u32 crc32_compute(const u8 *data, u64 len) {
    u32 crc = 0xFFFFFFFFu;
    u64 i;

    for (i = 0; i < len; i++) {
        u32 byte = data[i];
        int bit;
        crc ^= byte;
        for (bit = 0; bit < 8; bit++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320u;
            else
                crc = crc >> 1;
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/*  Entry header size: 8 (seq) + 4 (len) + 4 (crc) = 16 bytes        */
/* ------------------------------------------------------------------ */

#define WAL_HDR_SIZE 16

/* read_entry_header — read the 16-byte header at current file pos.
 * Returns 0 on success, 1 on short read / EOF. */
static int read_entry_header(u64 *seq, u32 *data_len, u32 *crc, wal_fd fp) {
    u8  hdr[WAL_HDR_SIZE];
    u64 got = 0;

    if (wal_fd_read(fp, hdr, WAL_HDR_SIZE, &got) != 0) return 1;
    if (got != WAL_HDR_SIZE) return 1;

    *seq      = read_u64_le(hdr);
    *data_len = read_u32_le(hdr + 8);
    *crc      = read_u32_le(hdr + 12);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  wal_create                                                        */
/* ------------------------------------------------------------------ */

unsigned long wal_create(wal **out, const char *path) {
    wal *w;
    u64  max_seq;

    if (!out)  return 1;
    if (!path) return 2;

    w = (wal *)calloc(1, sizeof(wal));
    if (!w) return 4;

    if (wal_fd_open_rw(&w->fp, path) != 0) {
        free(w);
        return 3;
    }

    strncpy(w->path, path, sizeof(w->path) - 1);
    w->path[sizeof(w->path) - 1] = '\0';

    /* scan existing entries to find max sequence number */
    max_seq = 0;
    if (wal_fd_seek_set(w->fp, 0) == 0) {
        for (;;) {
            u64 seq;
            u32 data_len, crc;

            if (read_entry_header(&seq, &data_len, &crc, w->fp))
                break;

            if (seq > max_seq)
                max_seq = seq;

            if (wal_fd_seek_cur(w->fp, (i64)data_len) != 0) {
                wal_fd_close(w->fp);
                free(w);
                return 5;
            }
        }
    }

    w->next_seq = max_seq + 1;
    wal_mtx_init(&w->lock);

    *out = w;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  wal_append                                                        */
/* ------------------------------------------------------------------ */

unsigned long wal_append(u64 *out_seq, wal *w,
                         const u8 *data, u64 len) {
    u8  hdr[WAL_HDR_SIZE];
    u32 crc;
    u64 seq;

    if (!out_seq)          return 1;
    if (!w)                return 2;
    if (!data && len > 0)  return 3;

    wal_mtx_lock(&w->lock);

    seq = w->next_seq;
    crc = crc32_compute(data, len);

    write_u64_le(hdr,      seq);
    write_u32_le(hdr + 8,  (u32)len);
    write_u32_le(hdr + 12, crc);

    if (wal_fd_seek_end(w->fp) != 0) {
        wal_mtx_unlock(&w->lock);
        return 4;
    }

    if (wal_fd_write(w->fp, hdr, WAL_HDR_SIZE) != 0) {
        wal_mtx_unlock(&w->lock);
        return 4;
    }

    if (len > 0) {
        if (wal_fd_write(w->fp, data, len) != 0) {
            wal_mtx_unlock(&w->lock);
            return 4;
        }
    }

    w->next_seq = seq + 1;
    *out_seq = seq;

    wal_mtx_unlock(&w->lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  wal_read                                                          */
/* ------------------------------------------------------------------ */

unsigned long wal_read(wal_entry *out, wal *w, u64 seq) {
    u64 entry_seq;
    u32 data_len, stored_crc;
    unsigned long rc = 0;

    if (!out) return 1;
    if (!w)   return 2;

    /* Seeking + reading must be atomic relative to concurrent appends. */
    wal_mtx_lock(&w->lock);

    if (wal_fd_seek_set(w->fp, 0) != 0) { rc = 4; goto done; }

    for (;;) {
        if (read_entry_header(&entry_seq, &data_len, &stored_crc, w->fp)) {
            rc = 3; /* seq not found */
            goto done;
        }

        if (entry_seq == seq) {
            u8  *payload;
            u32  check_crc;
            u64  got = 0;

            payload = (u8 *)malloc(data_len > 0 ? data_len : 1);
            if (!payload) { rc = 4; goto done; }

            if (data_len > 0) {
                if (wal_fd_read(w->fp, payload, data_len, &got) != 0 ||
                    got != data_len) {
                    free(payload);
                    rc = 4;
                    goto done;
                }
            }

            check_crc = crc32_compute(payload, data_len);
            if (check_crc != stored_crc) {
                free(payload);
                rc = 5;
                goto done;
            }

            out->seq  = entry_seq;
            out->data = payload;
            out->len  = data_len;
            rc = 0;
            goto done;
        }

        if (wal_fd_seek_cur(w->fp, (i64)data_len) != 0) {
            rc = 4;
            goto done;
        }
    }

done:
    wal_mtx_unlock(&w->lock);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  wal_iter_create                                                   */
/*                                                                    */
/*  Iterators use an independent file handle, so they don't fight     */
/*  with concurrent wal_appends over file position. No lock needed    */
/*  on the iterator path — each iterator is single-threaded by        */
/*  convention (caller owns it).                                       */
/* ------------------------------------------------------------------ */

unsigned long wal_iter_create(wal_iter **out, wal *w, u64 start_seq) {
    wal_iter *it;

    if (!out) return 1;
    if (!w)   return 2;

    it = (wal_iter *)calloc(1, sizeof(wal_iter));
    if (!it) return 3;

    if (wal_fd_open_ro(&it->fp, w->path) != 0) {
        free(it);
        return 3;
    }

    it->next_seq = start_seq;

    /* if start_seq > 0, advance past entries before start_seq */
    if (start_seq > 0) {
        for (;;) {
            u64 entry_seq;
            u32 data_len, crc;
            u64 pos = 0;

            if (wal_fd_tell(it->fp, &pos) != 0) break;

            if (read_entry_header(&entry_seq, &data_len, &crc, it->fp)) {
                /* EOF — start_seq not found, leave iterator positioned at end */
                break;
            }

            if (entry_seq >= start_seq) {
                wal_fd_seek_set(it->fp, pos);
                break;
            }

            if (wal_fd_seek_cur(it->fp, (i64)data_len) != 0)
                break;
        }
    }

    *out = it;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  wal_iter_next                                                     */
/* ------------------------------------------------------------------ */

unsigned long wal_iter_next(wal_entry *out, wal_iter *it) {
    u64 entry_seq;
    u32 data_len, stored_crc;
    u8  *payload;
    u32  check_crc;
    u64  got = 0;

    if (!out) return 1;
    if (!it)  return 2;

    if (read_entry_header(&entry_seq, &data_len, &stored_crc, it->fp))
        return 3; /* end of log */

    payload = (u8 *)malloc(data_len > 0 ? data_len : 1);
    if (!payload) return 4;

    if (data_len > 0) {
        if (wal_fd_read(it->fp, payload, data_len, &got) != 0 ||
            got != data_len) {
            free(payload);
            return 4;
        }
    }

    check_crc = crc32_compute(payload, data_len);
    if (check_crc != stored_crc) {
        free(payload);
        return 5;
    }

    out->seq  = entry_seq;
    out->data = payload;
    out->len  = data_len;

    it->next_seq = entry_seq + 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  wal_iter_destroy                                                  */
/* ------------------------------------------------------------------ */

unsigned long wal_iter_destroy(wal_iter *it) {
    if (!it) return 1;

    if (it->fp != WAL_FD_INVALID)
        wal_fd_close(it->fp);

    free(it);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  wal_truncate                                                      */
/*                                                                    */
/*  Rewrites the file with only entries whose seq >= before_seq.      */
/*  Holds the WAL mutex for the whole duration (close → rename →     */
/*  reopen). Concurrent readers/writers block until it's done.        */
/* ------------------------------------------------------------------ */

unsigned long wal_truncate(wal *w, u64 before_seq) {
    wal_fd tmp_fp;
    char   tmp_path[520];
    u8     hdr[WAL_HDR_SIZE];
    unsigned long rc = 0;

    if (!w) return 1;

    wal_mtx_lock(&w->lock);

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", w->path);

    if (wal_fd_open_rw(&tmp_fp, tmp_path) != 0) { rc = 2; goto done_unlock; }

    if (wal_fd_seek_set(w->fp, 0) != 0) {
        wal_fd_close(tmp_fp);
        remove(tmp_path);
        rc = 2;
        goto done_unlock;
    }

    for (;;) {
        u64  entry_seq;
        u32  data_len, crc;
        u8  *payload;
        u64  got;

        if (read_entry_header(&entry_seq, &data_len, &crc, w->fp))
            break;

        if (entry_seq >= before_seq) {
            write_u64_le(hdr,      entry_seq);
            write_u32_le(hdr + 8,  data_len);
            write_u32_le(hdr + 12, crc);

            if (wal_fd_write(tmp_fp, hdr, WAL_HDR_SIZE) != 0) {
                wal_fd_close(tmp_fp);
                remove(tmp_path);
                rc = 2;
                goto done_unlock;
            }

            if (data_len > 0) {
                payload = (u8 *)malloc(data_len);
                if (!payload) {
                    wal_fd_close(tmp_fp);
                    remove(tmp_path);
                    rc = 2;
                    goto done_unlock;
                }

                got = 0;
                if (wal_fd_read(w->fp, payload, data_len, &got) != 0 ||
                    got != data_len) {
                    free(payload);
                    wal_fd_close(tmp_fp);
                    remove(tmp_path);
                    rc = 2;
                    goto done_unlock;
                }

                if (wal_fd_write(tmp_fp, payload, data_len) != 0) {
                    free(payload);
                    wal_fd_close(tmp_fp);
                    remove(tmp_path);
                    rc = 2;
                    goto done_unlock;
                }

                free(payload);
            }
        } else {
            if (wal_fd_seek_cur(w->fp, (i64)data_len) != 0) {
                wal_fd_close(tmp_fp);
                remove(tmp_path);
                rc = 2;
                goto done_unlock;
            }
        }
    }

    wal_fd_close(tmp_fp);
    wal_fd_close(w->fp);
    w->fp = WAL_FD_INVALID;

    if (remove(w->path) != 0) {
        /* Try to recover by re-opening the original */
        wal_fd_open_rw(&w->fp, w->path);
        rc = 2;
        goto done_unlock;
    }
    if (rename(tmp_path, w->path) != 0) {
        rc = 2;
        goto done_unlock;
    }

    if (wal_fd_open_rw(&w->fp, w->path) != 0) { rc = 2; goto done_unlock; }

done_unlock:
    wal_mtx_unlock(&w->lock);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  wal_sync                                                          */
/* ------------------------------------------------------------------ */

unsigned long wal_sync(wal *w) {
    unsigned long rc;

    if (!w) return 1;

    wal_mtx_lock(&w->lock);
    rc = (wal_fd_flush(w->fp) == 0) ? 0 : 2;
    wal_mtx_unlock(&w->lock);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  wal_close                                                         */
/* ------------------------------------------------------------------ */

unsigned long wal_close(wal *w) {
    unsigned long rc = 0;

    if (!w) return 1;

    wal_mtx_lock(&w->lock);
    if (w->fp != WAL_FD_INVALID) {
        if (wal_fd_close(w->fp) != 0) rc = 2;
        w->fp = WAL_FD_INVALID;
    }
    wal_mtx_unlock(&w->lock);
    wal_mtx_destroy(&w->lock);

    free(w);
    return rc;
}
