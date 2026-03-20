#include "apennines/t3/db/wal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/*  WAL structs                                                       */
/* ------------------------------------------------------------------ */

struct wal {
    FILE *fp;
    u64   next_seq;
    char  path[512];
};

struct wal_iter {
    FILE *fp;
    u64   next_seq;
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
static int read_entry_header(u64 *seq, u32 *data_len, u32 *crc, FILE *fp) {
    u8 hdr[WAL_HDR_SIZE];

    if (fread(hdr, 1, WAL_HDR_SIZE, fp) != WAL_HDR_SIZE)
        return 1;

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

    w->fp = fopen(path, "a+b");
    if (!w->fp) {
        free(w);
        return 3;
    }

    /* copy path */
    strncpy(w->path, path, sizeof(w->path) - 1);
    w->path[sizeof(w->path) - 1] = '\0';

    /* scan existing entries to find max sequence number */
    max_seq = 0;
    if (fseek(w->fp, 0, SEEK_SET) == 0) {
        for (;;) {
            u64 seq;
            u32 data_len, crc;

            if (read_entry_header(&seq, &data_len, &crc, w->fp))
                break;

            if (seq > max_seq)
                max_seq = seq;

            /* skip past the data payload */
            if (fseek(w->fp, (long)data_len, SEEK_CUR) != 0) {
                fclose(w->fp);
                free(w);
                return 5;
            }
        }
    }

    w->next_seq = max_seq + 1;

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

    seq = w->next_seq;
    crc = crc32_compute(data, len);

    write_u64_le(hdr,      seq);
    write_u32_le(hdr + 8,  (u32)len);
    write_u32_le(hdr + 12, crc);

    /* ensure we are at end of file (append mode should do this,
       but be explicit after any read-seeking) */
    if (fseek(w->fp, 0, SEEK_END) != 0)
        return 4;

    if (fwrite(hdr, 1, WAL_HDR_SIZE, w->fp) != WAL_HDR_SIZE)
        return 4;

    if (len > 0) {
        if (fwrite(data, 1, (size_t)len, w->fp) != (size_t)len)
            return 4;
    }

    if (fflush(w->fp) != 0)
        return 4;

    w->next_seq = seq + 1;
    *out_seq = seq;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  wal_read                                                          */
/* ------------------------------------------------------------------ */

unsigned long wal_read(wal_entry *out, wal *w, u64 seq) {
    u64 entry_seq;
    u32 data_len, stored_crc;

    if (!out) return 1;
    if (!w)   return 2;

    /* seek to beginning and scan */
    if (fseek(w->fp, 0, SEEK_SET) != 0)
        return 4;

    for (;;) {
        if (read_entry_header(&entry_seq, &data_len, &stored_crc, w->fp))
            return 3; /* seq not found — reached end of file */

        if (entry_seq == seq) {
            u8  *payload;
            u32  check_crc;

            payload = (u8 *)malloc(data_len > 0 ? data_len : 1);
            if (!payload) return 4;

            if (data_len > 0) {
                if (fread(payload, 1, data_len, w->fp) != data_len) {
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
            return 0;
        }

        /* skip this entry's data */
        if (fseek(w->fp, (long)data_len, SEEK_CUR) != 0)
            return 4;
    }
}

/* ------------------------------------------------------------------ */
/*  wal_iter_create                                                   */
/* ------------------------------------------------------------------ */

unsigned long wal_iter_create(wal_iter **out, wal *w, u64 start_seq) {
    wal_iter *it;

    if (!out) return 1;
    if (!w)   return 2;

    it = (wal_iter *)calloc(1, sizeof(wal_iter));
    if (!it) return 3;

    it->fp = fopen(w->path, "rb");
    if (!it->fp) {
        free(it);
        return 3;
    }

    it->next_seq = start_seq;

    /* if start_seq > 0, advance past entries before start_seq */
    if (start_seq > 0) {
        for (;;) {
            u64 entry_seq;
            u32 data_len, crc;
            long pos = ftell(it->fp);

            if (read_entry_header(&entry_seq, &data_len, &crc, it->fp)) {
                /* reached EOF — start_seq not found, position at end */
                break;
            }

            if (entry_seq >= start_seq) {
                /* rewind to start of this entry */
                fseek(it->fp, pos, SEEK_SET);
                break;
            }

            /* skip data payload */
            if (fseek(it->fp, (long)data_len, SEEK_CUR) != 0)
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

    if (!out) return 1;
    if (!it)  return 2;

    if (read_entry_header(&entry_seq, &data_len, &stored_crc, it->fp))
        return 3; /* end of log */

    payload = (u8 *)malloc(data_len > 0 ? data_len : 1);
    if (!payload) return 4;

    if (data_len > 0) {
        if (fread(payload, 1, data_len, it->fp) != data_len) {
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

    if (it->fp)
        fclose(it->fp);

    free(it);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  wal_truncate                                                      */
/* ------------------------------------------------------------------ */

unsigned long wal_truncate(wal *w, u64 before_seq) {
    FILE *tmp_fp;
    char  tmp_path[520];
    u8    hdr[WAL_HDR_SIZE];

    if (!w) return 1;

    /* build temporary file path */
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", w->path);

    tmp_fp = fopen(tmp_path, "wb");
    if (!tmp_fp) return 2;

    /* scan original file, copy entries with seq >= before_seq */
    if (fseek(w->fp, 0, SEEK_SET) != 0) {
        fclose(tmp_fp);
        remove(tmp_path);
        return 2;
    }

    for (;;) {
        u64  entry_seq;
        u32  data_len, crc;
        u8  *payload;

        if (read_entry_header(&entry_seq, &data_len, &crc, w->fp))
            break;

        if (entry_seq >= before_seq) {
            /* re-encode header */
            write_u64_le(hdr,      entry_seq);
            write_u32_le(hdr + 8,  data_len);
            write_u32_le(hdr + 12, crc);

            if (fwrite(hdr, 1, WAL_HDR_SIZE, tmp_fp) != WAL_HDR_SIZE) {
                fclose(tmp_fp);
                remove(tmp_path);
                return 2;
            }

            if (data_len > 0) {
                payload = (u8 *)malloc(data_len);
                if (!payload) {
                    fclose(tmp_fp);
                    remove(tmp_path);
                    return 2;
                }

                if (fread(payload, 1, data_len, w->fp) != data_len) {
                    free(payload);
                    fclose(tmp_fp);
                    remove(tmp_path);
                    return 2;
                }

                if (fwrite(payload, 1, data_len, tmp_fp) != data_len) {
                    free(payload);
                    fclose(tmp_fp);
                    remove(tmp_path);
                    return 2;
                }

                free(payload);
            }
        } else {
            /* skip data for entries we are truncating */
            if (fseek(w->fp, (long)data_len, SEEK_CUR) != 0) {
                fclose(tmp_fp);
                remove(tmp_path);
                return 2;
            }
        }
    }

    fclose(tmp_fp);
    fclose(w->fp);

    /* replace original with truncated version */
    if (remove(w->path) != 0) return 2;
    if (rename(tmp_path, w->path) != 0) return 2;

    /* reopen the file */
    w->fp = fopen(w->path, "a+b");
    if (!w->fp) return 2;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  wal_sync                                                          */
/* ------------------------------------------------------------------ */

unsigned long wal_sync(wal *w) {
    int fd;

    if (!w) return 1;

    if (fflush(w->fp) != 0)
        return 2;

#ifdef _WIN32
    fd = _fileno(w->fp);
    if (fd < 0 || _commit(fd) != 0)
        return 2;
#else
    fd = fileno(w->fp);
    if (fd < 0 || fdatasync(fd) != 0)
        return 2;
#endif

    return 0;
}

/* ------------------------------------------------------------------ */
/*  wal_close                                                         */
/* ------------------------------------------------------------------ */

unsigned long wal_close(wal *w) {
    if (!w) return 1;

    if (w->fp) {
        if (fclose(w->fp) != 0) {
            free(w);
            return 2;
        }
    }

    free(w);
    return 0;
}
