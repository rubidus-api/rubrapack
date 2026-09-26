// src/msi/db.c - MSI database reader and IDT export (include/rubrapack/msi.h, format notes F2).

#include "rubrapack/buf.h"
#include "rubrapack/mem.h"
#include "rubrapack/msi.h"
#include "rubrapack/text.h"

#include <string.h>

// ---- stream names --------------------------------------------------------------------------

static int pack_value(uint32_t c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A' + 10);
    if (c >= 'a' && c <= 'z') return (int)(c - 'a' + 36);
    if (c == '.') return 62;
    if (c == '_') return 63;
    return -1;
}

static char unpack_value(uint32_t v) {
    static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._";
    return alphabet[v & 63];
}

proven_err_t rp_msi_stream_name(const char *utf8, bool table, uint16_t out[32], size_t *len) {
    if (utf8 == NULL || out == NULL || len == NULL) return PROVEN_ERR_INVALID_ARG;
    size_t n8 = strlen(utf8);
    uint16_t wide[128];
    rp_text_result_t r = rp_utf8_to_utf16((const uint8_t *)utf8, n8, wide, 128);
    if (r.err != PROVEN_OK) return r.err;
    size_t o = 0;
    if (table) out[o++] = 0x4840;
    for (size_t i = 0; i < r.units; ++i) {
        if (o >= 31) return PROVEN_ERR_OUT_OF_BOUNDS;
        int a = pack_value(wide[i]);
        if (a < 0) {
            out[o++] = wide[i];
            continue;
        }
        int b = i + 1 < r.units ? pack_value(wide[i + 1]) : -1;
        if (b >= 0) {
            out[o++] = (uint16_t)(0x3800 + a + (b << 6));
            ++i;
        } else {
            out[o++] = (uint16_t)(0x4800 + a);
        }
    }
    out[o] = 0;
    *len = o;
    return PROVEN_OK;
}

proven_err_t rp_msi_unpack_name(const uint16_t *name, size_t len, char *out, size_t cap, bool *table) {
    if (name == NULL || out == NULL || cap == 0) return PROVEN_ERR_INVALID_ARG;
    uint16_t wide[64];
    size_t w = 0, i = 0;
    bool is_table = len > 0 && name[0] == 0x4840;
    if (is_table) i = 1;
    for (; i < len; ++i) {
        uint16_t c = name[i];
        if (w + 2 > 64) return PROVEN_ERR_OUT_OF_BOUNDS;
        if (c >= 0x3800 && c < 0x4800) {
            wide[w++] = (uint16_t)unpack_value(c - 0x3800u);
            wide[w++] = (uint16_t)unpack_value((c - 0x3800u) >> 6);
        } else if (c >= 0x4800 && c < 0x4840) {
            wide[w++] = (uint16_t)unpack_value(c - 0x4800u);
        } else {
            wide[w++] = c;
        }
    }
    rp_text_result_t r = rp_utf16_to_utf8(wide, w, (uint8_t *)out, cap - 1);
    if (r.err != PROVEN_OK) return r.err;
    out[r.units] = '\0';
    if (table) *table = is_table;
    return PROVEN_OK;
}

// ---- helpers ---------------------------------------------------------------------------------

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Reads a whole stream named by a (packed) MSI name. A missing stream gives NOT_FOUND.
static proven_err_t read_named(const rp_msi_t *msi, const char *name, bool table, uint8_t **data, size_t *len) {
    uint16_t packed[32];
    size_t plen;
    proven_err_t err = rp_msi_stream_name(name, table, packed, &plen);
    if (err != PROVEN_OK) return err;
    uint32_t id;
    err = rp_cfb_find(msi->cfb, 0, packed, plen, &id);
    if (err != PROVEN_OK) return err;
    uint64_t size = msi->cfb->entries[id].size;
    if (size > msi->limits.max_metadata) return PROVEN_ERR_OUT_OF_BOUNDS;
    uint8_t *buf = rp_mem_alloc(msi->alloc, (size_t)size, 1);
    if (buf == NULL) return PROVEN_ERR_NOMEM;
    err = rp_cfb_read(msi->cfb, id, buf, (size_t)size);
    if (err != PROVEN_OK) {
        rp_mem_free(msi->alloc, buf);
        return err;
    }
    *data = buf;
    *len = (size_t)size;
    return PROVEN_OK;
}

static bool is_binary(uint16_t type) {
    return (type & RP_MSI_COL_STRING) && !(type & RP_MSI_COL_NONBINARY);
}

static size_t cell_width(const rp_msi_t *msi, uint16_t type) {
    if (is_binary(type)) return 2;
    if (type & RP_MSI_COL_STRING) return msi->long_refs ? 3 : 2;
    return (type & RP_MSI_COL_WIDTH) == 4 ? 4 : 2;
}

// Decodes a column-major table stream with the given column types.
static proven_err_t decode_rows(const rp_msi_t *msi, const uint8_t *data, size_t len, const uint16_t *types,
                                size_t ncols, rp_msi_rows_t *rows) {
    size_t width = 0;
    for (size_t c = 0; c < ncols; ++c) width += cell_width(msi, types[c]);
    memset(rows, 0, sizeof *rows);
    rows->column_count = ncols;
    if (len == 0) return PROVEN_OK;
    if (width == 0 || len % width != 0) return PROVEN_ERR_INVALID_FORMAT;
    size_t n = len / width;
    if (n > msi->limits.max_entries) return PROVEN_ERR_OUT_OF_BOUNDS;
    rows->cells = rp_mem_alloc(msi->alloc, n * ncols, sizeof *rows->cells);
    if (rows->cells == NULL) return PROVEN_ERR_NOMEM;
    rows->row_count = n;
    size_t off = 0;
    for (size_t c = 0; c < ncols; ++c) {
        size_t w = cell_width(msi, types[c]);
        for (size_t r = 0; r < n; ++r, off += w) {
            rp_msi_value_t *v = &rows->cells[r * ncols + c];
            const uint8_t *p = data + off;
            *v = (rp_msi_value_t){ .kind = RP_MSI_NULL };
            if (is_binary(types[c])) {
                if (rd16(p) != 0) v->kind = RP_MSI_BINARY;
            } else if (types[c] & RP_MSI_COL_STRING) {
                uint32_t id = rd16(p) | (w == 3 ? (uint32_t)p[2] << 16 : 0);
                if (id != 0) {
                    if (id >= msi->string_count || msi->strings[id].refcount == 0) {
                        rp_mem_free(msi->alloc, rows->cells);
                        memset(rows, 0, sizeof *rows);
                        return PROVEN_ERR_INVALID_FORMAT;
                    }
                    v->kind = RP_MSI_STR;
                    v->s = id;
                }
            } else if (w == 2) {
                uint16_t raw = rd16(p);
                if (raw != 0) {
                    v->kind = RP_MSI_INT;
                    v->i = (int32_t)raw - 0x8000;
                }
            } else {
                uint32_t raw = rd32(p);
                if (raw != 0) {
                    v->kind = RP_MSI_INT;
                    v->i = (int32_t)(raw ^ 0x80000000u);
                }
            }
        }
    }
    return PROVEN_OK;
}

static proven_err_t read_table_stream(const rp_msi_t *msi, const char *name, const uint16_t *types, size_t ncols,
                                      rp_msi_rows_t *rows) {
    uint8_t *data = NULL;
    size_t len = 0;
    proven_err_t err = read_named(msi, name, true, &data, &len);
    if (err == PROVEN_ERR_NOT_FOUND) {
        memset(rows, 0, sizeof *rows);
        rows->column_count = ncols;
        return PROVEN_OK;       // an empty table has no stream
    }
    if (err != PROVEN_OK) return err;
    err = decode_rows(msi, data, len, types, ncols, rows);
    rp_mem_free(msi->alloc, data);
    return err;
}

void rp_msi_rows_free(const rp_msi_t *msi, rp_msi_rows_t *rows) {
    if (rows == NULL) return;
    rp_mem_free(msi->alloc, rows->cells);
    memset(rows, 0, sizeof *rows);
}

proven_err_t rp_msi_string(const rp_msi_t *msi, uint32_t id, const uint8_t **bytes, size_t *len) {
    if (msi == NULL || bytes == NULL || len == NULL || id >= msi->string_count) return PROVEN_ERR_INVALID_ARG;
    static const uint8_t empty[1] = { 0 };
    if (id == 0) {
        *bytes = empty;
        *len = 0;
        return PROVEN_OK;
    }
    *bytes = msi->string_data + msi->strings[id].offset;
    *len = msi->strings[id].length;
    return PROVEN_OK;
}

// Copies a string into a NUL-terminated buffer (table and column names).
static proven_err_t string_cstr(const rp_msi_t *msi, uint32_t id, char *out, size_t cap) {
    const uint8_t *p;
    size_t n;
    proven_err_t err = rp_msi_string(msi, id, &p, &n);
    if (err != PROVEN_OK) return err;
    if (n + 1 > cap || memchr(p, 0, n) != NULL) return PROVEN_ERR_INVALID_FORMAT;
    memcpy(out, p, n);
    out[n] = '\0';
    return PROVEN_OK;
}

// ---- open ----------------------------------------------------------------------------------

void rp_msi_close(rp_msi_t *msi) {
    if (msi == NULL) return;
    rp_mem_free(msi->alloc, msi->string_data);
    rp_mem_free(msi->alloc, msi->strings);
    for (size_t t = 0; t < msi->table_count; ++t) rp_mem_free(msi->alloc, msi->tables[t].columns);
    rp_mem_free(msi->alloc, msi->tables);
    memset(msi, 0, sizeof *msi);
}

#define FAIL(e, msg) \
    do { \
        err = (e); \
        if (why) *why = (msg); \
        goto fail; \
    } while (0)

proven_err_t rp_msi_open(rp_msi_t *msi, proven_allocator_t alloc, const rp_cfb_t *cfb, const rp_limits_t *limits,
                         const char **why) {
    proven_err_t err = PROVEN_OK;
    uint8_t *pool = NULL;
    size_t pool_len = 0;
    rp_msi_rows_t trows = { 0 }, crows = { 0 };
    if (why) *why = NULL;
    if (msi == NULL || cfb == NULL || limits == NULL) return PROVEN_ERR_INVALID_ARG;
    memset(msi, 0, sizeof *msi);
    msi->alloc = alloc;
    msi->cfb = cfb;
    msi->limits = *limits;

    // String pool.
    err = read_named(msi, "_StringPool", true, &pool, &pool_len);
    if (err != PROVEN_OK) FAIL(err == PROVEN_ERR_NOT_FOUND ? PROVEN_ERR_INVALID_FORMAT : err, "no string pool");
    err = read_named(msi, "_StringData", true, &msi->string_data, &msi->string_data_len);
    if (err != PROVEN_OK) FAIL(err == PROVEN_ERR_NOT_FOUND ? PROVEN_ERR_INVALID_FORMAT : err, "no string data");
    if (pool_len < 4) FAIL(PROVEN_ERR_INVALID_FORMAT, "string pool header missing");
    uint32_t header = rd32(pool);
    msi->long_refs = (header & 0x80000000u) != 0;
    msi->codepage = header & 0x7FFFFFFFu;

    size_t count = 1;       // id 0
    for (size_t i = 4; i < pool_len; count++) {
        if (pool_len - i < 4) FAIL(PROVEN_ERR_INVALID_FORMAT, "string pool entry cut short");
        uint16_t length = rd16(pool + i), refs = rd16(pool + i + 2);
        i += 4;
        if (length == 0 && refs != 0) {
            if (pool_len - i < 4) FAIL(PROVEN_ERR_INVALID_FORMAT, "long string length missing");
            i += 4;
        }
    }
    if (count > (msi->long_refs ? 0xFFFFFFu : 0xFFFFu) + 1) FAIL(PROVEN_ERR_INVALID_FORMAT, "too many strings");
    msi->strings = rp_mem_alloc(alloc, count, sizeof *msi->strings);
    if (msi->strings == NULL) FAIL(PROVEN_ERR_NOMEM, "out of memory");
    msi->string_count = count;
    msi->strings[0] = (rp_msi_string_t){ 0 };
    uint64_t offset = 0;
    for (size_t i = 4, id = 1; i < pool_len; ++id) {
        uint32_t length = rd16(pool + i);
        uint16_t refs = rd16(pool + i + 2);
        i += 4;
        if (length == 0 && refs != 0) {     // long string: the u32 byte length follows
            length = rd32(pool + i);
            i += 4;
        }
        // Bit 15 of the reference word marks a string with non-ASCII bytes (format notes F2).
        msi->strings[id] = (rp_msi_string_t){ .offset = (uint32_t)offset, .length = length,
                                              .refcount = (uint16_t)(refs & 0x7FFF), .non_ascii = (refs & 0x8000) != 0 };
        if (length != 0 && msi->strings[id].refcount == 0) FAIL(PROVEN_ERR_INVALID_FORMAT, "string without references");
        offset += length;
        if (offset > msi->string_data_len) FAIL(PROVEN_ERR_INVALID_FORMAT, "string lengths exceed the string data");
    }
    if (offset != msi->string_data_len) FAIL(PROVEN_ERR_INVALID_FORMAT, "string data longer than the pool says");

    // _Tables and _Columns.
    static const uint16_t tables_types[] = { RP_MSI_COL_KEY | RP_MSI_COL_STRING | RP_MSI_COL_NONBINARY | 64 };
    static const uint16_t columns_types[] = {
        RP_MSI_COL_KEY | RP_MSI_COL_STRING | RP_MSI_COL_NONBINARY | 64,
        RP_MSI_COL_KEY | 2,
        RP_MSI_COL_STRING | RP_MSI_COL_NONBINARY | 64,
        2,
    };
    err = read_table_stream(msi, "_Tables", tables_types, 1, &trows);
    if (err != PROVEN_OK) FAIL(err, "_Tables is unreadable");
    err = read_table_stream(msi, "_Columns", columns_types, 4, &crows);
    if (err != PROVEN_OK) FAIL(err, "_Columns is unreadable");

    msi->table_count = trows.row_count;
    msi->tables = rp_mem_alloc(alloc, trows.row_count, sizeof *msi->tables);
    if (msi->tables == NULL) FAIL(PROVEN_ERR_NOMEM, "out of memory");
    memset(msi->tables, 0, trows.row_count * sizeof *msi->tables);
    for (size_t t = 0; t < trows.row_count; ++t) {
        rp_msi_value_t name = trows.cells[t];
        if (name.kind != RP_MSI_STR) FAIL(PROVEN_ERR_INVALID_FORMAT, "_Tables row without a name");
        msi->tables[t].name = name.s;
        size_t n = 0;
        for (size_t r = 0; r < crows.row_count; ++r) {
            if (crows.cells[r * 4].kind == RP_MSI_STR && crows.cells[r * 4].s == name.s) ++n;
        }
        if (n == 0) FAIL(PROVEN_ERR_INVALID_FORMAT, "table without columns");
        msi->tables[t].columns = rp_mem_alloc(alloc, n, sizeof *msi->tables[t].columns);
        if (msi->tables[t].columns == NULL) FAIL(PROVEN_ERR_NOMEM, "out of memory");
        memset(msi->tables[t].columns, 0, n * sizeof *msi->tables[t].columns);
        msi->tables[t].column_count = n;
        size_t filled = 0;
        for (size_t r = 0; r < crows.row_count; ++r) {
            const rp_msi_value_t *row = &crows.cells[r * 4];
            if (row[0].kind != RP_MSI_STR || row[0].s != name.s) continue;
            if (row[1].kind != RP_MSI_INT || row[1].i < 1 || (size_t)row[1].i > n || row[2].kind != RP_MSI_STR ||
                row[3].kind != RP_MSI_INT) {
                FAIL(PROVEN_ERR_INVALID_FORMAT, "bad _Columns row");
            }
            rp_msi_column_t *col = &msi->tables[t].columns[row[1].i - 1];
            if (col->name != 0) FAIL(PROVEN_ERR_INVALID_FORMAT, "column number repeated");
            col->name = row[2].s;
            col->type = (uint16_t)row[3].i;
            ++filled;
        }
        if (filled != n) FAIL(PROVEN_ERR_INVALID_FORMAT, "column numbers are not 1..n");
    }

    rp_mem_free(alloc, pool);
    rp_msi_rows_free(msi, &trows);
    rp_msi_rows_free(msi, &crows);
    return PROVEN_OK;

fail:
    rp_mem_free(alloc, pool);
    rp_msi_rows_free(msi, &trows);
    rp_msi_rows_free(msi, &crows);
    rp_msi_close(msi);
    return err;
}

proven_err_t rp_msi_find_table(const rp_msi_t *msi, const char *name, size_t *index) {
    if (msi == NULL || name == NULL || index == NULL) return PROVEN_ERR_INVALID_ARG;
    size_t n = strlen(name);
    for (size_t t = 0; t < msi->table_count; ++t) {
        const uint8_t *p;
        size_t len;
        if (rp_msi_string(msi, msi->tables[t].name, &p, &len) == PROVEN_OK && len == n && memcmp(p, name, n) == 0) {
            *index = t;
            return PROVEN_OK;
        }
    }
    return PROVEN_ERR_NOT_FOUND;
}

proven_err_t rp_msi_read_rows(const rp_msi_t *msi, size_t table, rp_msi_rows_t *rows) {
    if (msi == NULL || rows == NULL || table >= msi->table_count) return PROVEN_ERR_INVALID_ARG;
    const rp_msi_table_t *t = &msi->tables[table];
    char name[256];
    proven_err_t err = string_cstr(msi, t->name, name, sizeof name);
    if (err != PROVEN_OK) return err;
    uint16_t types[64];
    if (t->column_count > 64) return PROVEN_ERR_UNSUPPORTED;
    for (size_t c = 0; c < t->column_count; ++c) types[c] = t->columns[c].type;
    return read_table_stream(msi, name, types, t->column_count, rows);
}

// ---- IDT export ------------------------------------------------------------------------------

typedef struct {
    const rp_msi_t       *msi;
    const rp_msi_table_t *table;
    const rp_msi_rows_t  *rows;
} sort_ctx_t;

static int compare_values(const rp_msi_t *msi, rp_msi_value_t a, rp_msi_value_t b) {
    if (a.kind != b.kind) return a.kind < b.kind ? -1 : 1;
    if (a.kind == RP_MSI_INT) return a.i < b.i ? -1 : a.i > b.i;
    if (a.kind == RP_MSI_STR) {
        const uint8_t *pa = NULL, *pb = NULL;
        size_t na = 0, nb = 0;
        if (rp_msi_string(msi, a.s, &pa, &na) != PROVEN_OK || rp_msi_string(msi, b.s, &pb, &nb) != PROVEN_OK) return 0;
        int c = memcmp(pa, pb, na < nb ? na : nb);
        if (c != 0) return c < 0 ? -1 : 1;
        return na < nb ? -1 : na > nb;
    }
    return 0;
}

static int compare_rows(const sort_ctx_t *ctx, size_t ra, size_t rb) {
    size_t nc = ctx->table->column_count;
    for (size_t c = 0; c < nc; ++c) {
        if (!(ctx->table->columns[c].type & RP_MSI_COL_KEY)) continue;
        int d = compare_values(ctx->msi, ctx->rows->cells[ra * nc + c], ctx->rows->cells[rb * nc + c]);
        if (d != 0) return d;
    }
    return 0;
}

// Stable bottom-up merge sort of row indices.
static bool sort_rows(const sort_ctx_t *ctx, size_t *idx, size_t n) {
    size_t *tmp = rp_mem_alloc(ctx->msi->alloc, n, sizeof *tmp);
    if (tmp == NULL) return false;
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = compare_rows(ctx, idx[j], idx[i]) < 0 ? idx[j++] : idx[i++];
            while (i < mid) tmp[k++] = idx[i++];
            while (j < hi) tmp[k++] = idx[j++];
        }
        memcpy(idx, tmp, n * sizeof *idx);
    }
    rp_mem_free(ctx->msi->alloc, tmp);
    return true;
}

// String bytes with tab, CR and LF replaced as the archive format requires.
static void put_escaped(rp_buf_t *b, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = p[i];
        rp_buf_byte(b, c == '\t' ? 0x10 : c == '\r' ? 0x11 : c == '\n' ? 0x19 : c);
    }
}

static void put_value(rp_buf_t *b, const rp_msi_t *msi, rp_msi_value_t v) {
    if (v.kind == RP_MSI_INT) {
        rp_buf_long(b, v.i);
    } else if (v.kind == RP_MSI_STR) {
        const uint8_t *p;
        size_t n;
        if (rp_msi_string(msi, v.s, &p, &n) == PROVEN_OK) put_escaped(b, p, n);
    }
}

static bool has_high_bytes(const rp_msi_t *msi, uint32_t id) {
    const uint8_t *p;
    size_t n;
    if (rp_msi_string(msi, id, &p, &n) != PROVEN_OK) return false;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] >= 0x80) return true;
    }
    return false;
}

proven_err_t rp_msi_export_idt(const rp_msi_t *msi, size_t table, uint8_t **out, size_t *len) {
    if (msi == NULL || out == NULL || len == NULL || table >= msi->table_count) return PROVEN_ERR_INVALID_ARG;
    const rp_msi_table_t *t = &msi->tables[table];
    rp_msi_rows_t rows;
    proven_err_t err = rp_msi_read_rows(msi, table, &rows);
    if (err != PROVEN_OK) return err;
    size_t nc = t->column_count, nr = rows.row_count;
    size_t *idx = rp_mem_alloc(msi->alloc, nr, sizeof *idx);
    if (idx == NULL) {
        rp_msi_rows_free(msi, &rows);
        return PROVEN_ERR_NOMEM;
    }
    for (size_t r = 0; r < nr; ++r) idx[r] = r;
    sort_ctx_t ctx = { msi, t, &rows };
    if (!sort_rows(&ctx, idx, nr)) err = PROVEN_ERR_NOMEM;

    bool high = has_high_bytes(msi, t->name);
    for (size_t c = 0; c < nc && !high; ++c) high = has_high_bytes(msi, t->columns[c].name);
    for (size_t i = 0; i < nr * nc && !high; ++i) {
        if (rows.cells[i].kind == RP_MSI_STR) high = has_high_bytes(msi, rows.cells[i].s);
    }

    rp_buf_t b = rp_buf_new(msi->alloc, msi->limits.max_metadata);
    for (size_t c = 0; c < nc; ++c) {
        if (c) rp_buf_byte(&b, '\t');
        put_value(&b, msi, (rp_msi_value_t){ .kind = RP_MSI_STR, .s = t->columns[c].name });
    }
    rp_buf_puts(&b, "\r\n");
    for (size_t c = 0; c < nc; ++c) {
        uint16_t type = t->columns[c].type;
        bool nullable = type & RP_MSI_COL_NULLABLE;
        if (c) rp_buf_byte(&b, '\t');
        if (is_binary(type)) {
            rp_buf_puts(&b, nullable ? "V0" : "v0");
        } else if (type & RP_MSI_COL_STRING) {
            bool loc = type & RP_MSI_COL_LOCALIZABLE;
            rp_buf_byte(&b, (uint8_t)(loc ? (nullable ? 'L' : 'l') : (nullable ? 'S' : 's')));
            rp_buf_long(&b, type & RP_MSI_COL_WIDTH);
        } else {
            rp_buf_byte(&b, (uint8_t)(nullable ? 'I' : 'i'));
            rp_buf_long(&b, (type & RP_MSI_COL_WIDTH) == 4 ? 4 : 2);
        }
    }
    rp_buf_puts(&b, "\r\n");
    if (high) {
        rp_buf_long(&b, msi->codepage);
        rp_buf_byte(&b, '\t');
    }
    put_value(&b, msi, (rp_msi_value_t){ .kind = RP_MSI_STR, .s = t->name });
    for (size_t c = 0; c < nc; ++c) {
        if (!(t->columns[c].type & RP_MSI_COL_KEY)) continue;
        rp_buf_byte(&b, '\t');
        put_value(&b, msi, (rp_msi_value_t){ .kind = RP_MSI_STR, .s = t->columns[c].name });
    }
    rp_buf_puts(&b, "\r\n");

    for (size_t k = 0; k < nr && err == PROVEN_OK; ++k) {
        const rp_msi_value_t *row = &rows.cells[idx[k] * nc];
        for (size_t c = 0; c < nc; ++c) {
            if (c) rp_buf_byte(&b, '\t');
            if (row[c].kind == RP_MSI_BINARY) {
                bool first = true;
                for (size_t kc = 0; kc < nc; ++kc) {
                    if (!(t->columns[kc].type & RP_MSI_COL_KEY)) continue;
                    if (!first) rp_buf_byte(&b, '.');
                    put_value(&b, msi, row[kc]);
                    first = false;
                }
                rp_buf_puts(&b, ".ibd");
            } else {
                put_value(&b, msi, row[c]);
            }
        }
        rp_buf_puts(&b, "\r\n");
    }
    rp_mem_free(msi->alloc, idx);
    rp_msi_rows_free(msi, &rows);
    if (err != PROVEN_OK) {
        rp_buf_free(&b);
        return err;
    }
    return rp_buf_take(&b, out, len);
}

proven_err_t rp_msi_export_codepage(const rp_msi_t *msi, uint8_t **out, size_t *len) {
    if (msi == NULL || out == NULL || len == NULL) return PROVEN_ERR_INVALID_ARG;
    rp_buf_t b = rp_buf_new(msi->alloc, 64);
    rp_buf_puts(&b, "\r\n\r\n");
    rp_buf_long(&b, msi->codepage);
    rp_buf_puts(&b, "\t_ForceCodepage\r\n");
    return rp_buf_take(&b, out, len);
}
