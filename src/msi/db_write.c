// src/msi/db_write.c - MSI database writer (include/rubrapack/msi.h, format notes F2).

#include "rubrapack/buf.h"
#include "rubrapack/mem.h"
#include "rubrapack/msi.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const uint8_t *p;
    size_t         n;
} str_t;

static int str_cmp(str_t a, str_t b) {
    int c = memcmp(a.p, b.p, a.n < b.n ? a.n : b.n);
    if (c != 0) return c < 0 ? -1 : 1;
    return a.n < b.n ? -1 : a.n > b.n;
}

// Stable merge sort of strings (by bytes).
static bool sort_strs(proven_allocator_t alloc, str_t *v, size_t n) {
    str_t *tmp = rp_mem_alloc(alloc, n, sizeof *tmp);
    if (tmp == NULL) return false;
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = str_cmp(v[j], v[i]) < 0 ? v[j++] : v[i++];
            while (i < mid) tmp[k++] = v[i++];
            while (j < hi) tmp[k++] = v[j++];
        }
        memcpy(v, tmp, n * sizeof *v);
    }
    rp_mem_free(alloc, tmp);
    return true;
}

typedef struct {
    proven_allocator_t alloc;
    str_t             *uniq;        // sorted distinct strings; id = index + 1
    uint32_t          *refs;
    size_t             count;
    bool               long_refs;
} pool_t;

static uint32_t string_id(const pool_t *pool, str_t s) {
    if (s.n == 0) return 0;
    size_t lo = 0, hi = pool->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = str_cmp(pool->uniq[mid], s);
        if (c == 0) return (uint32_t)(mid + 1);
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return 0;   // not reached: every string was collected first
}

static str_t cstr(const char *s) { return (str_t){ (const uint8_t *)s, s ? strlen(s) : 0 }; }

static bool is_binary(uint16_t type) { return (type & RP_MSI_COL_STRING) && !(type & RP_MSI_COL_NONBINARY); }

// Collects every string occurrence, then dedupes; refs = occurrences (format notes F2 rule).
static proven_err_t build_pool(const rp_msi_wdb_t *db, pool_t *pool) {
    size_t occ = 0;
    for (size_t t = 0; t < db->table_count; ++t) {
        const rp_msi_wtable_t *tb = &db->tables[t];
        occ += 1 + 2 * tb->column_count;
        for (size_t i = 0; i < tb->row_count * tb->column_count; ++i) {
            if (tb->cells[i].kind == RP_MSI_STR && tb->cells[i].len > 0) ++occ;
        }
    }
    str_t *all = rp_mem_alloc(pool->alloc, occ, sizeof *all);
    if (all == NULL) return PROVEN_ERR_NOMEM;
    size_t n = 0;
    for (size_t t = 0; t < db->table_count; ++t) {
        const rp_msi_wtable_t *tb = &db->tables[t];
        all[n++] = cstr(tb->name);
        for (size_t c = 0; c < tb->column_count; ++c) {
            all[n++] = cstr(tb->name);      // _Columns.Table
            all[n++] = cstr(tb->columns[c].name);
        }
        for (size_t i = 0; i < tb->row_count * tb->column_count; ++i) {
            const rp_msi_cell_t *cell = &tb->cells[i];
            if (cell->kind == RP_MSI_STR && cell->len > 0) all[n++] = (str_t){ cell->bytes, cell->len };
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (all[i].n == 0) {
            rp_mem_free(pool->alloc, all);
            return PROVEN_ERR_INVALID_ARG;     // empty table or column name
        }
    }
    if (!sort_strs(pool->alloc, all, n)) {
        rp_mem_free(pool->alloc, all);
        return PROVEN_ERR_NOMEM;
    }
    size_t u = 0;
    for (size_t i = 0; i < n; ++i) {
        if (u == 0 || str_cmp(all[u - 1], all[i]) != 0) all[u++] = all[i];
    }
    pool->uniq = all;
    pool->count = u;
    pool->refs = rp_mem_alloc(pool->alloc, u, sizeof *pool->refs);
    if (pool->refs == NULL) return PROVEN_ERR_NOMEM;
    memset(pool->refs, 0, u * sizeof *pool->refs);
    // Count occurrences by looking each one up again.
    for (size_t t = 0; t < db->table_count; ++t) {
        const rp_msi_wtable_t *tb = &db->tables[t];
        pool->refs[string_id(pool, cstr(tb->name)) - 1] += 1 + (uint32_t)tb->column_count;
        for (size_t c = 0; c < tb->column_count; ++c) pool->refs[string_id(pool, cstr(tb->columns[c].name)) - 1]++;
        for (size_t i = 0; i < tb->row_count * tb->column_count; ++i) {
            const rp_msi_cell_t *cell = &tb->cells[i];
            if (cell->kind == RP_MSI_STR && cell->len > 0) {
                pool->refs[string_id(pool, (str_t){ cell->bytes, cell->len }) - 1]++;
            }
        }
    }
    for (size_t i = 0; i < u; ++i) {
        if (pool->refs[i] > 0x7FFF) return PROVEN_ERR_UNSUPPORTED;     // 15-bit reference count
    }
    if (u > 0xFFFFFF) return PROVEN_ERR_OUT_OF_BOUNDS;
    pool->long_refs = u > 0xFFFF;
    return PROVEN_OK;
}

static void put_ref(rp_buf_t *b, const pool_t *pool, uint32_t id) {
    rp_buf_u16le(b, (uint16_t)id);
    if (pool->long_refs) rp_buf_byte(b, (uint8_t)(id >> 16));
}

// A row's stored key numbers compare the way the database is sorted.
static uint32_t stored(const pool_t *pool, const rp_msi_cell_t *cell, uint16_t type) {
    if (cell->kind == RP_MSI_STR) return string_id(pool, (str_t){ cell->bytes, cell->len });
    if (cell->kind == RP_MSI_INT) {
        return (type & RP_MSI_COL_WIDTH) == 4 ? (uint32_t)cell->i ^ 0x80000000u : (uint32_t)((cell->i + 0x8000) & 0xFFFF);
    }
    return cell->kind == RP_MSI_BINARY ? 1 : 0;
}

typedef struct {
    const pool_t          *pool;
    const rp_msi_wtable_t *tb;
} row_ctx_t;

static int row_cmp(const row_ctx_t *ctx, size_t a, size_t b) {
    const rp_msi_wtable_t *tb = ctx->tb;
    for (size_t c = 0; c < tb->column_count; ++c) {
        uint16_t type = tb->columns[c].type;
        if (!(type & RP_MSI_COL_KEY)) continue;
        uint32_t x = stored(ctx->pool, &tb->cells[a * tb->column_count + c], type);
        uint32_t y = stored(ctx->pool, &tb->cells[b * tb->column_count + c], type);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

static bool sort_rows(proven_allocator_t alloc, const row_ctx_t *ctx, size_t *idx, size_t n) {
    size_t *tmp = rp_mem_alloc(alloc, n, sizeof *tmp);
    if (tmp == NULL) return false;
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = row_cmp(ctx, idx[j], idx[i]) < 0 ? idx[j++] : idx[i++];
            while (i < mid) tmp[k++] = idx[i++];
            while (j < hi) tmp[k++] = idx[j++];
        }
        memcpy(idx, tmp, n * sizeof *idx);
    }
    rp_mem_free(alloc, tmp);
    return true;
}

// Checks a cell against its column type (RFC 14.3: refuse, do not store something msi.dll would not).
static bool cell_ok(const rp_msi_cell_t *cell, uint16_t type) {
    if (cell->kind == RP_MSI_NULL || (cell->kind == RP_MSI_STR && cell->len == 0)) {
        return (type & RP_MSI_COL_NULLABLE) != 0;
    }
    if (is_binary(type)) return cell->kind == RP_MSI_BINARY;
    if (type & RP_MSI_COL_STRING) return cell->kind == RP_MSI_STR && memchr(cell->bytes, 0, cell->len) == NULL;
    if (cell->kind != RP_MSI_INT) return false;
    if ((type & RP_MSI_COL_WIDTH) == 4) return cell->i != INT32_MIN;
    return cell->i >= -32767 && cell->i <= 32767;
}

typedef struct {
    uint16_t name[32];
    size_t   name_len;
    uint8_t *data;          // owned
    size_t   size;
    const uint8_t *borrowed;
} out_stream_t;

typedef struct {
    proven_allocator_t alloc;
    out_stream_t      *v;
    size_t             count, cap;
} streams_t;

static out_stream_t *add_stream(streams_t *s) {
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 16;
        out_stream_t *v = rp_mem_alloc(s->alloc, cap, sizeof *v);
        if (v == NULL) return NULL;
        if (s->count) memcpy(v, s->v, s->count * sizeof *v);
        rp_mem_free(s->alloc, s->v);
        s->v = v;
        s->cap = cap;
    }
    out_stream_t *o = &s->v[s->count++];
    memset(o, 0, sizeof *o);
    return o;
}

static proven_err_t add_buffer(streams_t *s, const char *name, bool table, rp_buf_t *b) {
    out_stream_t *o = add_stream(s);
    if (o == NULL) {
        rp_buf_free(b);
        return PROVEN_ERR_NOMEM;
    }
    proven_err_t err = rp_msi_stream_name(name, table, o->name, &o->name_len);
    if (err == PROVEN_OK) err = rp_buf_take(b, &o->data, &o->size);
    else rp_buf_free(b);
    return err;
}

proven_err_t rp_msi_write(proven_allocator_t alloc, const rp_msi_wdb_t *db, unsigned sector_shift,
                          const rp_limits_t *limits, uint8_t **out, size_t *len) {
    if (db == NULL || limits == NULL || out == NULL || len == NULL) return PROVEN_ERR_INVALID_ARG;
    pool_t pool = { .alloc = alloc };
    streams_t streams = { .alloc = alloc };
    size_t *order = NULL;
    proven_err_t err = PROVEN_OK;

    // Validate cells and table shape first.
    for (size_t t = 0; t < db->table_count && err == PROVEN_OK; ++t) {
        const rp_msi_wtable_t *tb = &db->tables[t];
        if (tb->column_count == 0 || tb->column_count > 32 || (tb->row_count && tb->cells == NULL)) err = PROVEN_ERR_INVALID_ARG;
        for (size_t i = 0; err == PROVEN_OK && i < tb->row_count * tb->column_count; ++i) {
            if (!cell_ok(&tb->cells[i], tb->columns[i % tb->column_count].type)) err = PROVEN_ERR_INVALID_ARG;
        }
    }
    if (err == PROVEN_OK) err = build_pool(db, &pool);

    // _StringPool / _StringData.
    if (err == PROVEN_OK) {
        rp_buf_t p = rp_buf_new(alloc, limits->max_metadata), d = rp_buf_new(alloc, limits->max_metadata);
        rp_buf_u32le(&p, db->codepage | (pool.long_refs ? 0x80000000u : 0));
        for (size_t i = 0; i < pool.count; ++i) {
            bool high = false;
            for (size_t k = 0; k < pool.uniq[i].n && !high; ++k) high = pool.uniq[i].p[k] >= 0x80;
            uint16_t word = (uint16_t)(pool.refs[i] | (high ? 0x8000u : 0));
            if (pool.uniq[i].n > 0xFFFF) {
                rp_buf_u16le(&p, 0);
                rp_buf_u16le(&p, word);
                rp_buf_u32le(&p, (uint32_t)pool.uniq[i].n);
            } else {
                rp_buf_u16le(&p, (uint16_t)pool.uniq[i].n);
                rp_buf_u16le(&p, word);
            }
            rp_buf_put(&d, pool.uniq[i].p, pool.uniq[i].n);
        }
        err = add_buffer(&streams, "_StringPool", true, &p);
        if (err == PROVEN_OK) err = add_buffer(&streams, "_StringData", true, &d);
        else rp_buf_free(&d);
    }

    // _Tables and _Columns, sorted by table-name id (then column number).
    if (err == PROVEN_OK && db->table_count > 0) {
        order = rp_mem_alloc(alloc, db->table_count, sizeof *order);
        if (order == NULL) err = PROVEN_ERR_NOMEM;
        for (size_t i = 0; err == PROVEN_OK && i < db->table_count; ++i) {
            size_t j = i;
            uint32_t id = string_id(&pool, cstr(db->tables[i].name));
            while (j > 0 && string_id(&pool, cstr(db->tables[order[j - 1]].name)) > id) {
                order[j] = order[j - 1];
                --j;
            }
            order[j] = i;
            if (j > 0 && string_id(&pool, cstr(db->tables[order[j - 1]].name)) == id) err = PROVEN_ERR_INVALID_ARG;
        }
        if (err == PROVEN_OK) {
            rp_buf_t tb = rp_buf_new(alloc, limits->max_metadata);
            for (size_t i = 0; i < db->table_count; ++i) put_ref(&tb, &pool, string_id(&pool, cstr(db->tables[order[i]].name)));
            err = add_buffer(&streams, "_Tables", true, &tb);
        }
        if (err == PROVEN_OK) {
            rp_buf_t cb = rp_buf_new(alloc, limits->max_metadata);
            for (int col = 0; col < 4; ++col) {
                for (size_t i = 0; i < db->table_count; ++i) {
                    const rp_msi_wtable_t *tb = &db->tables[order[i]];
                    for (size_t c = 0; c < tb->column_count; ++c) {
                        if (col == 0) put_ref(&cb, &pool, string_id(&pool, cstr(tb->name)));
                        if (col == 1) rp_buf_u16le(&cb, (uint16_t)((c + 1) + 0x8000));
                        if (col == 2) put_ref(&cb, &pool, string_id(&pool, cstr(tb->columns[c].name)));
                        if (col == 3) rp_buf_u16le(&cb, (uint16_t)(tb->columns[c].type + 0x8000u));
                    }
                }
            }
            err = add_buffer(&streams, "_Columns", true, &cb);
        }
    }

    // User tables and their binary cells.
    for (size_t t = 0; t < db->table_count && err == PROVEN_OK; ++t) {
        const rp_msi_wtable_t *tb = &db->tables[t];
        if (tb->row_count == 0) continue;
        size_t nc = tb->column_count, nr = tb->row_count;
        size_t *idx = rp_mem_alloc(alloc, nr, sizeof *idx);
        if (idx == NULL) {
            err = PROVEN_ERR_NOMEM;
            break;
        }
        for (size_t r = 0; r < nr; ++r) idx[r] = r;
        row_ctx_t ctx = { &pool, tb };
        if (!sort_rows(alloc, &ctx, idx, nr)) err = PROVEN_ERR_NOMEM;
        for (size_t r = 1; err == PROVEN_OK && r < nr; ++r) {
            if (row_cmp(&ctx, idx[r - 1], idx[r]) == 0) err = PROVEN_ERR_INVALID_ARG;     // duplicate key
        }
        rp_buf_t b = rp_buf_new(alloc, limits->max_metadata);
        for (size_t c = 0; err == PROVEN_OK && c < nc; ++c) {
            uint16_t type = tb->columns[c].type;
            for (size_t r = 0; r < nr; ++r) {
                const rp_msi_cell_t *cell = &tb->cells[idx[r] * nc + c];
                uint32_t v = stored(&pool, cell, type);
                if (is_binary(type)) rp_buf_u16le(&b, (uint16_t)v);
                else if (type & RP_MSI_COL_STRING) put_ref(&b, &pool, v);
                else if ((type & RP_MSI_COL_WIDTH) == 4) rp_buf_u32le(&b, cell->kind == RP_MSI_INT ? v : 0);
                else rp_buf_u16le(&b, (uint16_t)(cell->kind == RP_MSI_INT ? v : 0));
            }
        }
        if (err == PROVEN_OK) err = add_buffer(&streams, tb->name, true, &b);
        else rp_buf_free(&b);

        for (size_t r = 0; err == PROVEN_OK && r < nr; ++r) {
            for (size_t c = 0; c < nc; ++c) {
                const rp_msi_cell_t *cell = &tb->cells[r * nc + c];
                if (cell->kind != RP_MSI_BINARY) continue;
                char name[128];
                size_t o = (size_t)snprintf(name, sizeof name, "%s", tb->name);
                for (size_t k = 0; k < nc && o < sizeof name; ++k) {
                    if (!(tb->columns[k].type & RP_MSI_COL_KEY)) continue;
                    const rp_msi_cell_t *key = &tb->cells[r * nc + k];
                    if (key->kind == RP_MSI_STR) {
                        o += (size_t)snprintf(name + o, sizeof name - o, ".%.*s", (int)key->len, (const char *)key->bytes);
                    } else {
                        o += (size_t)snprintf(name + o, sizeof name - o, ".%ld", (long)key->i);
                    }
                }
                if (o >= sizeof name) {
                    err = PROVEN_ERR_OUT_OF_BOUNDS;
                    break;
                }
                out_stream_t *s = add_stream(&streams);
                if (s == NULL) {
                    err = PROVEN_ERR_NOMEM;
                    break;
                }
                err = rp_msi_stream_name(name, false, s->name, &s->name_len);
                s->borrowed = cell->bytes;
                s->size = cell->len;
            }
        }
        rp_mem_free(alloc, idx);
    }

    for (size_t k = 0; err == PROVEN_OK && k < db->stream_count; ++k) {
        out_stream_t *s = add_stream(&streams);
        if (s == NULL) {
            err = PROVEN_ERR_NOMEM;
            break;
        }
        err = rp_msi_stream_name(db->streams[k].name, false, s->name, &s->name_len);
        s->borrowed = db->streams[k].data;
        s->size = db->streams[k].len;
    }

    if (err == PROVEN_OK && db->summary != NULL) {
        out_stream_t *s = add_stream(&streams);
        if (s == NULL) {
            err = PROVEN_ERR_NOMEM;
        } else {
            static const char sname[] = "\005SummaryInformation";
            for (size_t i = 0; sname[i]; ++i) s->name[i] = (uint8_t)sname[i];
            s->name_len = sizeof sname - 1;
            s->borrowed = db->summary;
            s->size = db->summary_len;
        }
    }

    if (err == PROVEN_OK) {
        rp_cfb_stream_t *list = rp_mem_alloc(alloc, streams.count, sizeof *list);
        if (list == NULL) {
            err = PROVEN_ERR_NOMEM;
        } else {
            for (size_t i = 0; i < streams.count; ++i) {
                const out_stream_t *s = &streams.v[i];
                list[i] = (rp_cfb_stream_t){ s->name, s->name_len, s->data ? s->data : s->borrowed, s->size };
            }
            static const uint8_t msi_clsid[16] = { 0x84, 0x10, 0x0C, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
            err = rp_cfb_write(alloc, sector_shift, msi_clsid, list, streams.count, limits, out, len);
            rp_mem_free(alloc, list);
        }
    }

    for (size_t i = 0; i < streams.count; ++i) rp_mem_free(alloc, streams.v[i].data);
    rp_mem_free(alloc, streams.v);
    rp_mem_free(alloc, order);
    rp_mem_free(alloc, pool.uniq);
    rp_mem_free(alloc, pool.refs);
    return err;
}
