// src/container/zip.c - ZIP as MSIX uses it (include/rubrapack/zip.h; PKWARE APPNOTE 6.3.x,
// the layout observed from Windows' packaging API: docs/research/2026-09-27-p8a-msix-oracle.md).

#include "rubrapack/zip.h"

#include "rubrapack/deflate.h"
#include "rubrapack/mem.h"

#include "proven/hash.h"

#include <stdlib.h>
#include <string.h>

enum { DOS_TIME = 0x0000, DOS_DATE = 0x0021, VERSION = 45, FLAG_DESCRIPTOR = 0x0008 };

void rp_zip_begin(rp_zip_writer_t *w, proven_allocator_t alloc, size_t limit) {
    w->out = rp_buf_new(alloc, limit);
    w->central = rp_buf_new(alloc, limit);
    w->count = 0;
    w->signer_form = false;
}

void rp_zip_add(rp_zip_writer_t *w, const char *name, int method, const uint8_t *data, size_t data_len, uint32_t crc, uint64_t size,
                size_t *lfh_size) {
    size_t nl = strlen(name);
    uint64_t off = w->out.len;
    rp_buf_t *o = &w->out;
    rp_buf_u32le(o, 0x04034B50);
    rp_buf_u16le(o, VERSION);
    rp_buf_u16le(o, FLAG_DESCRIPTOR);
    rp_buf_u16le(o, (uint16_t)method);
    rp_buf_u16le(o, DOS_TIME);
    rp_buf_u16le(o, DOS_DATE);
    rp_buf_u32le(o, 0);                     // CRC and sizes follow in the data descriptor
    rp_buf_u32le(o, 0);
    rp_buf_u32le(o, 0);
    rp_buf_u16le(o, (uint16_t)nl);
    rp_buf_u16le(o, 0);
    rp_buf_put(o, name, nl);
    if (lfh_size) *lfh_size = 30 + nl;
    rp_buf_put(o, data, data_len);
    rp_buf_u32le(o, 0x08074B50);            // ZIP64 data descriptor
    rp_buf_u32le(o, crc);
    rp_buf_u64le(o, data_len);
    rp_buf_u64le(o, size);
    rp_buf_t *c = &w->central;
    if (w->signer_form && data_len < 0xFFFFFFFFu && size < 0xFFFFFFFFu && off < 0xFFFFFFFFu) {
        // The same entry described the ZIP32 way (what Windows' signer writes for small entries).
        rp_buf_u32le(c, 0x02014B50);
        rp_buf_u16le(c, VERSION);
        rp_buf_u16le(c, VERSION);
        rp_buf_u16le(c, FLAG_DESCRIPTOR);
        rp_buf_u16le(c, (uint16_t)method);
        rp_buf_u16le(c, DOS_TIME);
        rp_buf_u16le(c, DOS_DATE);
        rp_buf_u32le(c, crc);
        rp_buf_u32le(c, (uint32_t)data_len);
        rp_buf_u32le(c, (uint32_t)size);
        rp_buf_u16le(c, (uint16_t)nl);
        rp_buf_u16le(c, 0);
        rp_buf_u16le(c, 0);
        rp_buf_u16le(c, 0);
        rp_buf_u16le(c, 0);
        rp_buf_u32le(c, 0);
        rp_buf_u32le(c, (uint32_t)off);
        rp_buf_put(c, name, nl);
        ++w->count;
        return;
    }
    rp_buf_u32le(c, 0x02014B50);
    rp_buf_u16le(c, VERSION);               // made by: 4.5, MS-DOS
    rp_buf_u16le(c, VERSION);
    rp_buf_u16le(c, FLAG_DESCRIPTOR);
    rp_buf_u16le(c, (uint16_t)method);
    rp_buf_u16le(c, DOS_TIME);
    rp_buf_u16le(c, DOS_DATE);
    rp_buf_u32le(c, crc);
    rp_buf_u32le(c, 0xFFFFFFFF);
    rp_buf_u32le(c, 0xFFFFFFFF);
    rp_buf_u16le(c, (uint16_t)nl);
    rp_buf_u16le(c, 28);
    rp_buf_u16le(c, 0);                     // comment
    rp_buf_u16le(c, 0);                     // disk
    rp_buf_u16le(c, 0);                     // internal attributes
    rp_buf_u32le(c, 0);                     // external attributes
    rp_buf_u32le(c, 0xFFFFFFFF);
    rp_buf_put(c, name, nl);
    rp_buf_u16le(c, 0x0001);                // ZIP64 extra: size, compressed size, offset
    rp_buf_u16le(c, 24);
    rp_buf_u64le(c, size);
    rp_buf_u64le(c, data_len);
    rp_buf_u64le(c, off);
    ++w->count;
}

void rp_zip_add_plain(rp_zip_writer_t *w, const char *name, int method, const uint8_t *data, size_t data_len, uint32_t crc, uint64_t size) {
    size_t nl = strlen(name);
    uint64_t off = w->out.len;
    rp_buf_t *o = &w->out, *c = &w->central;
    rp_buf_u32le(o, 0x04034B50);
    rp_buf_u16le(o, 20);
    rp_buf_u16le(o, 0);
    rp_buf_u16le(o, (uint16_t)method);
    rp_buf_u16le(o, DOS_TIME);
    rp_buf_u16le(o, DOS_DATE);
    rp_buf_u32le(o, crc);
    rp_buf_u32le(o, (uint32_t)data_len);
    rp_buf_u32le(o, (uint32_t)size);
    rp_buf_u16le(o, (uint16_t)nl);
    rp_buf_u16le(o, 0);
    rp_buf_put(o, name, nl);
    rp_buf_put(o, data, data_len);
    rp_buf_u32le(c, 0x02014B50);
    rp_buf_u16le(c, VERSION);
    rp_buf_u16le(c, 20);
    rp_buf_u16le(c, 0);
    rp_buf_u16le(c, (uint16_t)method);
    rp_buf_u16le(c, DOS_TIME);
    rp_buf_u16le(c, DOS_DATE);
    rp_buf_u32le(c, crc);
    rp_buf_u32le(c, (uint32_t)data_len);
    rp_buf_u32le(c, (uint32_t)size);
    rp_buf_u16le(c, (uint16_t)nl);
    rp_buf_u16le(c, 0);
    rp_buf_u16le(c, 0);
    rp_buf_u16le(c, 0);
    rp_buf_u16le(c, 0);
    rp_buf_u32le(c, 0);
    rp_buf_u32le(c, (uint32_t)off);
    rp_buf_put(c, name, nl);
    ++w->count;
}

// The ZIP64 end record, its locator and the end record for a central directory of `count` entries
// and `cd_size` bytes at `cd_off`.
static void put_tail(rp_buf_t *o, uint64_t count, uint64_t cd_size, uint64_t cd_off, bool disks_zero) {
    uint64_t z64 = cd_off + cd_size;
    rp_buf_u32le(o, 0x06064B50);            // ZIP64 end of central directory
    rp_buf_u64le(o, 44);
    rp_buf_u16le(o, VERSION);
    rp_buf_u16le(o, VERSION);
    rp_buf_u32le(o, 0);
    rp_buf_u32le(o, 0);
    rp_buf_u64le(o, count);
    rp_buf_u64le(o, count);
    rp_buf_u64le(o, cd_size);
    rp_buf_u64le(o, cd_off);
    rp_buf_u32le(o, 0x07064B50);            // locator
    rp_buf_u32le(o, 0);
    rp_buf_u64le(o, z64);
    rp_buf_u32le(o, 1);
    rp_buf_u32le(o, 0x06054B50);            // end record: everything in the ZIP64 records
    rp_buf_u16le(o, disks_zero ? 0 : 0xFFFF);
    rp_buf_u16le(o, disks_zero ? 0 : 0xFFFF);
    rp_buf_u16le(o, 0xFFFF);
    rp_buf_u16le(o, 0xFFFF);
    rp_buf_u32le(o, 0xFFFFFFFF);
    rp_buf_u32le(o, 0xFFFFFFFF);
    rp_buf_u16le(o, 0);
}

proven_err_t rp_zip_finish(rp_zip_writer_t *w, uint8_t **out, size_t *len) {
    uint64_t cd_off = w->out.len, cd_size = w->central.len;
    rp_buf_put(&w->out, w->central.data, w->central.len);
    rp_buf_free(&w->central);
    put_tail(&w->out, w->count, cd_size, cd_off, w->signer_form);
    return rp_buf_take(&w->out, out, len);
}

void rp_zip_tail(const rp_zip_writer_t *w, rp_buf_t *out) {
    rp_buf_put(out, w->central.data, w->central.len);
    put_tail(out, w->count, w->central.len, w->out.len, w->signer_form);
}

void rp_zip_abort(rp_zip_writer_t *w) {
    rp_buf_free(&w->out);
    rp_buf_free(&w->central);
}

// ---- reading -----------------------------------------------------------------------------------

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) | ((uint64_t)u32(p + 4) << 32); }

#define NO(msg) do { *why = (msg); err = PROVEN_ERR_INVALID_FORMAT; goto out; } while (0)

void rp_zip_entries_free(proven_allocator_t alloc, rp_zip_entry_t *entries, size_t count) {
    for (size_t i = 0; entries && i < count; ++i) rp_mem_free(alloc, entries[i].name);
    rp_mem_free(alloc, entries);
}

static int by_offset(const void *a, const void *b) {
    const rp_zip_entry_t *x = *(const rp_zip_entry_t *const *)a, *y = *(const rp_zip_entry_t *const *)b;
    return x->lfh_off < y->lfh_off ? -1 : x->lfh_off > y->lfh_off;
}

proven_err_t rp_zip_read(proven_allocator_t alloc, const uint8_t *zip, size_t len, const rp_limits_t *lim, rp_zip_entry_t **entries,
                         size_t *count, const char **why) {
    *entries = NULL;
    *count = 0;
    proven_err_t err = PROVEN_OK;
    rp_zip_entry_t *e = NULL;
    rp_zip_entry_t **order = NULL;
    size_t n = 0;
    if (len < 22) {
        *why = "not a ZIP archive (too short)";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    // The end record: the last one, within the 64 KiB a comment may take.
    size_t eocd = SIZE_MAX;
    for (size_t i = len - 22 + 1; i-- > 0 && len - i <= 22 + 65535;) {
        if (u32(zip + i) == 0x06054B50 && i + 22 + u16(zip + i + 20) == len) {
            eocd = i;
            break;
        }
    }
    if (eocd == SIZE_MAX) {
        *why = "not a ZIP archive (no end record)";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    uint64_t entries_n = u16(zip + eocd + 10), cd_size = u32(zip + eocd + 12), cd_off = u32(zip + eocd + 16);
    if (eocd >= 20 && u32(zip + eocd - 20) == 0x07064B50) {
        uint64_t z = u64(zip + eocd - 20 + 8);
        if (z > len - 56 || u32(zip + z) != 0x06064B50) {
            *why = "a damaged ZIP64 end record";
            return PROVEN_ERR_INVALID_FORMAT;
        }
        entries_n = u64(zip + z + 32);
        cd_size = u64(zip + z + 40);
        cd_off = u64(zip + z + 48);
    }
    if (cd_off > len || cd_size > len - cd_off) {
        *why = "the ZIP central directory lies outside the file";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    if (entries_n > lim->max_entries || entries_n > cd_size / 46) {
        *why = "more ZIP entries than allowed or than the directory holds";
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    e = rp_mem_alloc(alloc, entries_n ? entries_n : 1, sizeof *e);
    if (e == NULL) return PROVEN_ERR_NOMEM;
    memset(e, 0, (entries_n ? entries_n : 1) * sizeof *e);
    const uint8_t *p = zip + cd_off, *end = zip + cd_off + cd_size;
    for (; n < entries_n; ++n) {
        if ((size_t)(end - p) < 46 || u32(p) != 0x02014B50) NO("a damaged ZIP central directory entry");
        size_t nl = u16(p + 28), xl = u16(p + 30), cl = u16(p + 32);
        if ((size_t)(end - p) < 46 + nl + xl + cl) NO("a damaged ZIP central directory entry");
        rp_zip_entry_t *x = &e[n];
        x->method = u16(p + 10);
        x->crc = u32(p + 16);
        x->csize = u32(p + 20);
        x->size = u32(p + 24);
        x->lfh_off = u32(p + 42);
        if (u16(p + 8) & 1) NO("an encrypted ZIP entry");
        // ZIP64 extra: the fields that were 0xFFFFFFFF, in order.
        const uint8_t *xp = p + 46 + nl, *xe = xp + xl;
        while (xe - xp >= 4) {
            size_t id = u16(xp), sz = u16(xp + 2);
            if ((size_t)(xe - xp - 4) < sz) NO("a damaged ZIP extra field");
            if (id == 0x0001) {
                const uint8_t *q = xp + 4, *qe = q + sz;
                if (x->size == 0xFFFFFFFF) {
                    if (qe - q < 8) NO("a damaged ZIP64 extra");
                    x->size = u64(q), q += 8;
                }
                if (x->csize == 0xFFFFFFFF) {
                    if (qe - q < 8) NO("a damaged ZIP64 extra");
                    x->csize = u64(q), q += 8;
                }
                if (x->lfh_off == 0xFFFFFFFF) {
                    if (qe - q < 8) NO("a damaged ZIP64 extra");
                    x->lfh_off = u64(q);
                }
            }
            xp += 4 + sz;
        }
        x->name = rp_mem_alloc(alloc, nl + 1, 1);
        if (x->name == NULL) {
            err = PROVEN_ERR_NOMEM;
            goto out;
        }
        memcpy(x->name, p + 46, nl);
        x->name[nl] = 0;
        if (memchr(x->name, 0, nl)) NO("a ZIP entry name with a NUL byte");
        if (x->method != 0 && x->method != 8) NO("a ZIP entry compressed with a method other than stored or deflate");
        // The local header must agree.
        if (x->lfh_off > len || len - x->lfh_off < 30 || u32(zip + x->lfh_off) != 0x04034B50) NO("a ZIP entry's local header is missing");
        const uint8_t *l = zip + x->lfh_off;
        size_t lnl = u16(l + 26), lxl = u16(l + 28);
        x->lfh_size = 30 + lnl + lxl;
        if (lnl != nl || memcmp(l + 30, x->name, nl) != 0 || u16(l + 8) != x->method) NO("a ZIP entry's local header does not match the directory");
        x->data_off = x->lfh_off + x->lfh_size;
        if (x->data_off > len || x->csize > len - x->data_off) NO("a ZIP entry's data lies outside the file");
        p += 46 + nl + xl + cl;
    }
    // No two entries may share bytes.
    order = rp_mem_alloc(alloc, n ? n : 1, sizeof *order);
    if (order == NULL) {
        err = PROVEN_ERR_NOMEM;
        goto out;
    }
    for (size_t i = 0; i < n; ++i) order[i] = &e[i];
    qsort(order, n, sizeof *order, by_offset);
    for (size_t i = 1; i < n; ++i) {
        if (order[i]->lfh_off < order[i - 1]->data_off + order[i - 1]->csize) NO("two ZIP entries overlap");
    }
    if (n && order[n - 1]->data_off + order[n - 1]->csize > cd_off) NO("a ZIP entry runs into the central directory");
out:
    rp_mem_free(alloc, order);
    if (err != PROVEN_OK) {
        rp_zip_entries_free(alloc, e, n + 1 <= entries_n ? n + 1 : n);
        return err;
    }
    *entries = e;
    *count = n;
    return PROVEN_OK;
}

proven_err_t rp_zip_data(proven_allocator_t alloc, const uint8_t *zip, size_t len, const rp_zip_entry_t *e, uint64_t max, uint8_t **out,
                         const char **why) {
    *out = NULL;
    if (e->size > max || e->size > SIZE_MAX - 1) {
        *why = "a ZIP entry larger than allowed";
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    if (e->data_off > len || e->csize > len - e->data_off) {
        *why = "a ZIP entry's data lies outside the file";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    uint8_t *buf = rp_mem_alloc(alloc, (size_t)e->size + 1, 1);
    if (buf == NULL) return PROVEN_ERR_NOMEM;
    const uint8_t *src = zip + e->data_off;
    if (e->method == 0) {
        if (e->csize != e->size) {
            rp_mem_free(alloc, buf);
            *why = "a stored ZIP entry whose sizes differ";
            return PROVEN_ERR_INVALID_FORMAT;
        }
        memcpy(buf, src, (size_t)e->size);
    } else {
        size_t pos = 0, used = 0;
        proven_err_t err = rp_inflate(src, (size_t)e->csize, buf, (size_t)e->size, &pos, &used);
        if (err != PROVEN_OK || pos != e->size) {
            rp_mem_free(alloc, buf);
            *why = "a ZIP entry does not inflate to its size";
            return PROVEN_ERR_INVALID_FORMAT;
        }
    }
    if (proven_crc32((proven_mem_view_t){ buf, (size_t)e->size }) != e->crc) {
        rp_mem_free(alloc, buf);
        *why = "a ZIP entry's CRC-32 does not match";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    *out = buf;
    return PROVEN_OK;
}

bool rp_zip_central_without(const uint8_t *zip, size_t len, const char *skip, uint64_t skip_lfh_off, rp_buf_t *out, const char **why) {
    // The ZIP64 end record gives the central directory (rp_zip_read has checked the archive).
    if (len < 98) {
        *why = "the archive has no ZIP64 end record";
        return false;
    }
    size_t z = len - 22 - 20 - 56;
    if (u32(zip + len - 22) != 0x06054B50 || u32(zip + z) != 0x06064B50) {
        *why = "the archive does not end in ZIP64 end records as MSIX packages do";
        return false;
    }
    uint64_t cd_size = u64(zip + z + 40), cd_off = u64(zip + z + 48);
    if (cd_off > len || cd_size > len - cd_off) {
        *why = "the central directory lies outside the archive";
        return false;
    }
    size_t sl = strlen(skip), count = 0, kept = 0;
    bool found = false;
    for (uint64_t o = cd_off; o < cd_off + cd_size;) {
        if (o + 46 > cd_off + cd_size || u32(zip + o) != 0x02014B50) {
            *why = "a malformed central directory";
            return false;
        }
        size_t n = u16(zip + o + 28), e = u16(zip + o + 30), c = u16(zip + o + 32), rl = 46 + n + e + c;
        if (o + rl > cd_off + cd_size) {
            *why = "a malformed central directory";
            return false;
        }
        if (n == sl && memcmp(zip + o + 46, skip, sl) == 0) {
            // Windows takes the entry's place from the ZIP32 offset field (measured, RFC-0011): an
            // entry described the ZIP64 way is a hash mismatch there, so it is here.
            if (u32(zip + o + 42) == 0xFFFFFFFF || u32(zip + o + 20) == 0xFFFFFFFF) {
                *why = "the signature entry is described the ZIP64 way; Windows reads its place from the ZIP32 fields";
                return false;
            }
            found = true;
        } else {
            rp_buf_put(out, zip + o, rl);
            kept += rl;
            ++count;
        }
        o += rl;
    }
    if (!found) {
        *why = "the entry is not in the central directory";
        return false;
    }
    // The file's own end records, with counts, sizes and offsets as if the entry did not exist.
    uint8_t tail[98];
    memcpy(tail, zip + z, 98);
    for (int k = 0; k < 2; ++k) {
        for (int b = 0; b < 8; ++b) tail[24 + 8 * k + b] = (uint8_t)((uint64_t)count >> (8 * b));
    }
    for (int b = 0; b < 8; ++b) {
        tail[40 + b] = (uint8_t)((uint64_t)kept >> (8 * b));
        tail[48 + b] = (uint8_t)(skip_lfh_off >> (8 * b));
        tail[56 + 8 + b] = (uint8_t)((skip_lfh_off + kept) >> (8 * b));
    }
    rp_buf_put(out, tail, sizeof tail);
    return out->err == PROVEN_OK;
}
