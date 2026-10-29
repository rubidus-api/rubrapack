// src/msi/transform.c - transforms (include/rubrapack/transform.h, RFC-0016 3).
//
// A transform is a compound file of class {000C1082-0000-0000-C000-000000000046} with a string
// pool of its own and, per changed table, a stream of records in row order (a database stores
// columns one after another; a transform does not). Each record starts with a 16-bit mask:
//   0            deletes the row named by the key columns that follow;
//   odd          inserts (or replaces) a whole row, the high byte being the number of columns;
//   otherwise    updates the columns whose bits are set; the key columns come first, always.
// A new table is a record in _Tables and one per column in _Columns, whose Number is left null
// (msi.dll numbers them in record order); a dropped table is a _Tables delete. A binary cell is the
// marker 1 and a stream "<Table>.<keys>" in the transform. The summary stream names both packages.
// This is the layout msi.dll's GenerateTransform writes (tests/fixtures/mst/ref.mst, T108).

#include "rubrapack/in.h"
#include "rubrapack/mem.h"
#include "rubrapack/lang.h"
#include "rubrapack/suminfo.h"
#include "rubrapack/transform.h"

#include <stdio.h>
#include <string.h>

static bool is_string(uint16_t t) { return (t & RP_MSI_COL_STRING) && (t & RP_MSI_COL_NONBINARY); }
static bool is_null(const rp_msi_cell_t *c) { return c->kind == RP_MSI_NULL || (c->kind == RP_MSI_STR && c->len == 0); }

static int bytes_cmp(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
    int c = memcmp(a, b, an < bn ? an : bn);
    if (c != 0) return c < 0 ? -1 : 1;
    return an < bn ? -1 : an > bn;
}

// An integer as a database stores it, so that keys compare the way msi.dll sorts them.
static uint32_t int_stored(int32_t i, uint16_t type) {
    return (type & RP_MSI_COL_WIDTH) == 4 ? (uint32_t)i ^ 0x80000000u : (uint32_t)((i + 0x8000) & 0xFFFF);
}

static int cell_cmp(const rp_msi_cell_t *a, const rp_msi_cell_t *b, uint16_t type) {
    bool an = is_null(a), bn = is_null(b);
    if (an || bn) return an == bn ? 0 : an ? -1 : 1;
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    if (a->kind == RP_MSI_STR) return bytes_cmp(a->bytes, a->len, b->bytes, b->len);
    if (a->kind == RP_MSI_INT) {
        uint32_t x = int_stored(a->i, type), y = int_stored(b->i, type);
        return x < y ? -1 : x > y;
    }
    return 0;       // binary cells are compared by their streams
}

static int key_cmp(const rp_msi_wcolumn_t *cols, size_t ncols, const rp_msi_cell_t *a, const rp_msi_cell_t *b) {
    for (size_t c = 0; c < ncols; ++c) {
        if (!(cols[c].type & RP_MSI_COL_KEY)) continue;
        int r = cell_cmp(&a[c], &b[c], cols[c].type);
        if (r != 0) return r;
    }
    return 0;
}

// Stable merge sort of row pointers by key.
static bool sort_rows(proven_allocator_t alloc, const rp_msi_wcolumn_t *cols, size_t ncols, const rp_msi_cell_t **v, size_t n) {
    const rp_msi_cell_t **tmp = rp_mem_alloc(alloc, n + 1, sizeof *tmp);
    if (tmp == NULL) return false;
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = key_cmp(cols, ncols, v[j], v[i]) < 0 ? v[j++] : v[i++];
            while (i < mid) tmp[k++] = v[i++];
            while (j < hi) tmp[k++] = v[j++];
        }
        memcpy(v, tmp, n * sizeof *v);
    }
    rp_mem_free(alloc, tmp);
    return true;
}

// "<Table>.<key>.<key>" - the stream of a binary cell (as src/msi/db_write.c names it).
static bool binary_name(const char *table, const rp_msi_wcolumn_t *cols, size_t ncols, const rp_msi_cell_t *row, char *out, size_t cap) {
    size_t o = (size_t)snprintf(out, cap, "%s", table);
    for (size_t k = 0; k < ncols && o < cap; ++k) {
        if (!(cols[k].type & RP_MSI_COL_KEY)) continue;
        const rp_msi_cell_t *key = &row[k];
        if (key->kind == RP_MSI_STR) o += (size_t)snprintf(out + o, cap - o, ".%.*s", (int)key->len, (const char *)key->bytes);
        else o += (size_t)snprintf(out + o, cap - o, ".%ld", key->kind == RP_MSI_INT ? (long)key->i : 0L);
    }
    return o < cap;
}

static proven_err_t read_stream(proven_allocator_t alloc, const rp_cfb_t *cfb, uint32_t storage, const char *name, bool table,
                                size_t max, uint8_t **data, size_t *len) {
    uint16_t packed[32];
    size_t pl;
    uint32_t id;
    *data = NULL;
    *len = 0;
    proven_err_t err = rp_msi_stream_name(name, table, packed, &pl);
    if (err != PROVEN_OK) return err;
    if (rp_cfb_find(cfb, storage, packed, pl, &id) != PROVEN_OK) return PROVEN_ERR_NOT_FOUND;
    if (cfb->entries[id].size > max) return PROVEN_ERR_OUT_OF_BOUNDS;
    size_t n = (size_t)cfb->entries[id].size;
    *data = rp_mem_alloc(alloc, n + 1, 1);
    if (*data == NULL) return PROVEN_ERR_NOMEM;
    err = rp_cfb_read(cfb, id, *data, n);
    if (err != PROVEN_OK) {
        rp_mem_free(alloc, *data);
        *data = NULL;
        return err;
    }
    *len = n;
    return PROVEN_OK;
}

static const rp_msi_wtable_t *find_table(const rp_msi_wdb_t *db, const char *name) {
    for (size_t t = 0; t < db->table_count; ++t) {
        if (strcmp(db->tables[t].name, name) == 0) return &db->tables[t];
    }
    return NULL;
}

// ---- writer ----------------------------------------------------------------------------------

typedef struct {
    const rp_msi_cell_t *cells;     // one row in the table's column layout
    uint32_t             mask;
} rec_t;

typedef struct {
    const char             *name;
    const rp_msi_wcolumn_t *cols;
    size_t                  ncols;
    rec_t                  *recs;
    size_t                  nrec, cap;
} otab_t;

typedef struct {
    char     name[128];
    uint8_t *data;          // owned
    size_t   len;
} ostream_t;

typedef struct {
    proven_allocator_t   alloc;
    const rp_limits_t   *limits;
    const rp_mst_side_t *base, *target;
    otab_t              *tabs;
    size_t               ntab, tabcap;
    ostream_t           *streams;
    size_t               nstream, streamcap;
    bool                 files;
    const char          *why;
    char                *table;
    size_t               table_cap;
} wctx_t;

static proven_err_t refuse(wctx_t *w, const char *table, const char *why) {
    w->why = why;
    if (w->table && w->table_cap) snprintf(w->table, w->table_cap, "%s", table);
    return PROVEN_ERR_UNSUPPORTED;
}

static otab_t *add_tab(wctx_t *w, const char *name, const rp_msi_wcolumn_t *cols, size_t ncols) {
    if (w->ntab == w->tabcap) {
        size_t cap = w->tabcap ? w->tabcap * 2 : 32;
        otab_t *v = rp_mem_alloc(w->alloc, cap, sizeof *v);
        if (v == NULL) return NULL;
        if (w->ntab) memcpy(v, w->tabs, w->ntab * sizeof *v);
        rp_mem_free(w->alloc, w->tabs);
        w->tabs = v;
        w->tabcap = cap;
    }
    otab_t *t = &w->tabs[w->ntab++];
    *t = (otab_t){ .name = name, .cols = cols, .ncols = ncols };
    return t;
}

static bool add_rec(wctx_t *w, otab_t *t, const rp_msi_cell_t *cells, uint32_t mask) {
    if (t->nrec == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 16;
        rec_t *v = rp_mem_alloc(w->alloc, cap, sizeof *v);
        if (v == NULL) return false;
        if (t->nrec) memcpy(v, t->recs, t->nrec * sizeof *v);
        rp_mem_free(w->alloc, t->recs);
        t->recs = v;
        t->cap = cap;
    }
    t->recs[t->nrec++] = (rec_t){ cells, mask };
    return true;
}

// The target's bytes of a binary cell become a stream of the transform.
static proven_err_t add_binary(wctx_t *w, const otab_t *t, const rp_msi_cell_t *row) {
    if (w->nstream == w->streamcap) {
        size_t cap = w->streamcap ? w->streamcap * 2 : 8;
        ostream_t *v = rp_mem_alloc(w->alloc, cap, sizeof *v);
        if (v == NULL) return PROVEN_ERR_NOMEM;
        if (w->nstream) memcpy(v, w->streams, w->nstream * sizeof *v);
        rp_mem_free(w->alloc, w->streams);
        w->streams = v;
        w->streamcap = cap;
    }
    ostream_t *s = &w->streams[w->nstream];
    memset(s, 0, sizeof *s);
    if (!binary_name(t->name, t->cols, t->ncols, row, s->name, sizeof s->name)) return PROVEN_ERR_OUT_OF_BOUNDS;
    proven_err_t err = read_stream(w->alloc, w->target->cfb, 0, s->name, false, (size_t)w->limits->max_output, &s->data, &s->len);
    if (err == PROVEN_OK) w->nstream++;
    return err;
}

// Whether the two packages hold the same bytes for the binary cells of a row (same key in both).
static bool same_binary(wctx_t *w, const char *table, const rp_msi_wcolumn_t *cols, size_t ncols, const rp_msi_cell_t *a,
                        proven_err_t *err) {
    char name[128];
    uint8_t *x = NULL, *y = NULL;
    size_t xn = 0, yn = 0;
    if (!binary_name(table, cols, ncols, a, name, sizeof name)) {
        *err = PROVEN_ERR_OUT_OF_BOUNDS;
        return false;
    }
    *err = read_stream(w->alloc, w->base->cfb, 0, name, false, (size_t)w->limits->max_output, &x, &xn);
    if (*err == PROVEN_OK) *err = read_stream(w->alloc, w->target->cfb, 0, name, false, (size_t)w->limits->max_output, &y, &yn);
    bool same = *err == PROVEN_OK && xn == yn && memcmp(x, y, xn) == 0;
    rp_mem_free(w->alloc, x);
    rp_mem_free(w->alloc, y);
    return same;
}

static bool same_columns(const rp_msi_wtable_t *a, const rp_msi_wtable_t *b) {
    if (a->column_count != b->column_count) return false;
    for (size_t c = 0; c < a->column_count; ++c) {
        if (strcmp(a->columns[c].name, b->columns[c].name) != 0 || a->columns[c].type != b->columns[c].type) return false;
    }
    return true;
}

static proven_err_t insert_row(wctx_t *w, otab_t *o, const rp_msi_cell_t *row) {
    if (!add_rec(w, o, row, 1u | (uint32_t)o->ncols << 8)) return PROVEN_ERR_NOMEM;
    for (size_t c = 0; c < o->ncols; ++c) {
        if (rp_msi_col_binary(o->cols[c].type) && !is_null(&row[c])) {
            proven_err_t err = add_binary(w, o, row);
            if (err != PROVEN_OK) return err;
        }
    }
    return PROVEN_OK;
}

// The records of one table that both packages have.
static proven_err_t diff_table(wctx_t *w, const rp_msi_wtable_t *a, const rp_msi_wtable_t *b) {
    if (!same_columns(a, b)) return refuse(w, b->name, "has other columns in the two packages");
    size_t nc = b->column_count;
    const rp_msi_cell_t **ra = rp_mem_alloc(w->alloc, a->row_count + 1, sizeof *ra);
    const rp_msi_cell_t **rb = rp_mem_alloc(w->alloc, b->row_count + 1, sizeof *rb);
    proven_err_t err = ra && rb ? PROVEN_OK : PROVEN_ERR_NOMEM;
    for (size_t r = 0; err == PROVEN_OK && r < a->row_count; ++r) ra[r] = rp_msi_wrow(a, r);      // same columns as b
    for (size_t r = 0; err == PROVEN_OK && r < b->row_count; ++r) rb[r] = rp_msi_wrow(b, r);
    if (err == PROVEN_OK && (!sort_rows(w->alloc, a->columns, nc, ra, a->row_count) || !sort_rows(w->alloc, b->columns, nc, rb, b->row_count))) {
        err = PROVEN_ERR_NOMEM;
    }
    otab_t *o = NULL;
    size_t i = 0, j = 0;
    while (err == PROVEN_OK && (i < a->row_count || j < b->row_count)) {
        int c = i == a->row_count ? 1 : j == b->row_count ? -1 : key_cmp(b->columns, nc, ra[i], rb[j]);
        uint32_t mask = 0;
        bool record = true;
        if (c == 0) {
            for (size_t k = 0; err == PROVEN_OK && k < nc; ++k) {
                uint16_t type = b->columns[k].type;
                if (type & RP_MSI_COL_KEY) continue;
                bool same;
                if (rp_msi_col_binary(type)) {
                    same = is_null(&ra[i][k]) == is_null(&rb[j][k]);
                    if (same && !is_null(&ra[i][k])) same = same_binary(w, b->name, b->columns, nc, ra[i], &err);
                } else {
                    same = ra[i][k].kind == rb[j][k].kind && cell_cmp(&ra[i][k], &rb[j][k], type) == 0;
                }
                if (!same) {
                    if (k == 0 || k >= 16) err = refuse(w, b->name, "changes a column a transform record cannot name (the first, or past the 16th)");
                    mask |= 1u << k;
                }
            }
            record = mask != 0;
        }
        if (err == PROVEN_OK && record && !w->files && strcmp(b->name, "File") == 0) err = refuse(w, b->name, "changes files; a transform carries no files");
        if (err == PROVEN_OK && record && !w->files && strcmp(b->name, "Media") == 0) err = refuse(w, b->name, "changes the cabinets; a transform carries no files");
        if (err == PROVEN_OK && record && o == NULL) {
            o = add_tab(w, b->name, b->columns, nc);
            if (o == NULL) err = PROVEN_ERR_NOMEM;
        }
        if (err == PROVEN_OK && record) {
            if (c < 0) {
                if (!add_rec(w, o, ra[i], 0)) err = PROVEN_ERR_NOMEM;
            } else if (c > 0) {
                err = insert_row(w, o, rb[j]);
            } else {
                if (!add_rec(w, o, rb[j], mask)) err = PROVEN_ERR_NOMEM;
                for (size_t k = 0; err == PROVEN_OK && k < nc; ++k) {
                    if ((mask & (1u << k)) && rp_msi_col_binary(b->columns[k].type) && !is_null(&rb[j][k])) err = add_binary(w, o, rb[j]);
                }
            }
        }
        if (c <= 0) ++i;
        if (c >= 0) ++j;
    }
    rp_mem_free(w->alloc, ra);
    rp_mem_free(w->alloc, rb);
    return err;
}

// Record order as msi.dll writes it: the target's rows (inserts and updates) by key, then the
// deletes by key.
static int rec_cmp(const otab_t *o, const rec_t *a, const rec_t *b) {
    if ((a->mask == 0) != (b->mask == 0)) return a->mask == 0 ? 1 : -1;
    return key_cmp(o->cols, o->ncols, a->cells, b->cells);
}

// Stable merge sort of a table's records (rec_cmp).
static bool sort_recs(proven_allocator_t alloc, otab_t *o) {
    size_t n = o->nrec;
    rec_t *v = o->recs, *tmp = rp_mem_alloc(alloc, n + 1, sizeof *tmp);
    if (tmp == NULL) return false;
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = rec_cmp(o, &v[j], &v[i]) < 0 ? v[j++] : v[i++];
            while (i < mid) tmp[k++] = v[i++];
            while (j < hi) tmp[k++] = v[j++];
        }
        memcpy(v, tmp, n * sizeof *v);
    }
    rp_mem_free(alloc, tmp);
    return true;
}

// ---- string pool ----

typedef struct {
    const uint8_t *p;
    size_t         n;
    uint32_t       refs;
} pstr_t;

typedef struct {
    pstr_t *v;
    size_t  count;
    bool    long_refs;
} pool_t;

static bool present(uint16_t type, size_t c, uint32_t mask) {
    if (mask & 1) return c < (mask >> 8);
    return (type & RP_MSI_COL_KEY) || (mask & (1u << c));
}

static int pstr_cmp(const pstr_t *a, const pstr_t *b) { return bytes_cmp(a->p, a->n, b->p, b->n); }

static uint32_t pool_id(const pool_t *pool, const uint8_t *p, size_t n) {
    pstr_t key = { p, n, 0 };
    size_t lo = 0, hi = pool->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = pstr_cmp(&pool->v[mid], &key);
        if (c == 0) return (uint32_t)(mid + 1);
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

static proven_err_t build_pool(wctx_t *w, pool_t *pool) {
    size_t n = 0;
    for (size_t t = 0; t < w->ntab; ++t) n += w->tabs[t].nrec * w->tabs[t].ncols;
    pstr_t *all = rp_mem_alloc(w->alloc, n + 1, sizeof *all), *tmp = rp_mem_alloc(w->alloc, n + 1, sizeof *tmp);
    if (all == NULL || tmp == NULL) {
        rp_mem_free(w->alloc, all);
        rp_mem_free(w->alloc, tmp);
        return PROVEN_ERR_NOMEM;
    }
    size_t k = 0;
    for (size_t t = 0; t < w->ntab; ++t) {
        const otab_t *o = &w->tabs[t];
        for (size_t r = 0; r < o->nrec; ++r) {
            for (size_t c = 0; c < o->ncols; ++c) {
                const rp_msi_cell_t *cell = &o->recs[r].cells[c];
                if (present(o->cols[c].type, c, o->recs[r].mask) && is_string(o->cols[c].type) && !is_null(cell)) {
                    all[k++] = (pstr_t){ cell->bytes, cell->len, 1 };
                }
            }
        }
    }
    for (size_t wd = 1; wd < k; wd *= 2) {
        for (size_t lo = 0; lo < k; lo += 2 * wd) {
            size_t mid = lo + wd < k ? lo + wd : k, hi = lo + 2 * wd < k ? lo + 2 * wd : k;
            size_t i = lo, j = mid, o = lo;
            while (i < mid && j < hi) tmp[o++] = pstr_cmp(&all[j], &all[i]) < 0 ? all[j++] : all[i++];
            while (i < mid) tmp[o++] = all[i++];
            while (j < hi) tmp[o++] = all[j++];
        }
        memcpy(all, tmp, k * sizeof *all);
    }
    rp_mem_free(w->alloc, tmp);
    size_t u = 0;
    for (size_t i = 0; i < k; ++i) {
        if (u > 0 && pstr_cmp(&all[u - 1], &all[i]) == 0) {
            if (all[u - 1].refs < 0x7FFF) all[u - 1].refs++;
        } else {
            all[u++] = all[i];
        }
    }
    pool->v = all;
    pool->count = u;
    if (u > 0xFFFFFF) return PROVEN_ERR_OUT_OF_BOUNDS;
    pool->long_refs = u > 0xFFFF;
    return PROVEN_OK;
}

static void put_ref(rp_buf_t *b, const pool_t *pool, uint32_t id) {
    rp_buf_u16le(b, (uint16_t)id);
    if (pool->long_refs) rp_buf_byte(b, (uint8_t)(id >> 16));
}

static void put_record(rp_buf_t *b, const pool_t *pool, const otab_t *o, const rec_t *rec) {
    rp_buf_u16le(b, (uint16_t)rec->mask);
    for (size_t c = 0; c < o->ncols; ++c) {
        uint16_t type = o->cols[c].type;
        if (!present(type, c, rec->mask)) continue;
        const rp_msi_cell_t *cell = &rec->cells[c];
        if (rp_msi_col_binary(type)) rp_buf_u16le(b, is_null(cell) ? 0 : 1);
        else if (type & RP_MSI_COL_STRING) put_ref(b, pool, is_null(cell) ? 0 : pool_id(pool, cell->bytes, cell->len));
        else if ((type & RP_MSI_COL_WIDTH) == 4) rp_buf_u32le(b, cell->kind == RP_MSI_INT ? int_stored(cell->i, type) : 0);
        else rp_buf_u16le(b, (uint16_t)(cell->kind == RP_MSI_INT ? int_stored(cell->i, type) : 0));
    }
}

// ---- summary ----

static const rp_suminfo_prop_t *prop(const rp_suminfo_t *si, uint32_t pid) {
    for (size_t i = 0; i < si->count; ++i) {
        if (si->props[i].pid == pid) return &si->props[i];
    }
    return NULL;
}

// Property.Value of `name`, or "".
static void property(const rp_msi_wdb_t *db, const char *name, const uint8_t **p, size_t *n) {
    static const uint8_t empty[1] = { 0 };
    *p = empty;
    *n = 0;
    const rp_msi_wtable_t *t = find_table(db, "Property");
    if (t == NULL || t->column_count < 2) return;
    for (size_t r = 0; r < t->row_count; ++r) {
        const rp_msi_cell_t *k = rp_msi_wcell(t, r, 0), *v = rp_msi_wcell(t, r, 1);
        if (k->kind == RP_MSI_STR && k->len == strlen(name) && memcmp(k->bytes, name, k->len) == 0 && v->kind == RP_MSI_STR) {
            *p = v->bytes;
            *n = v->len;
            return;
        }
    }
}

static proven_err_t summary(wctx_t *w, uint32_t flags, uint8_t **out, size_t *len) {
    rp_suminfo_t a = { .count = 0 }, b = { .count = 0 }, s = { .count = 0 };
    if (w->base->db->summary && rp_suminfo_parse(w->base->db->summary, w->base->db->summary_len, &a) != PROVEN_OK) a.count = 0;
    if (w->target->db->summary && rp_suminfo_parse(w->target->db->summary, w->target->db->summary_len, &b) != PROVEN_OK) b.count = 0;
    // PID_REVNUMBER: "{base product code}base version;{target product code}target version;{upgrade code}".
    char rev[512];
    const uint8_t *p[5];
    size_t n[5];
    property(w->base->db, "ProductCode", &p[0], &n[0]);
    property(w->base->db, "ProductVersion", &p[1], &n[1]);
    property(w->target->db, "ProductCode", &p[2], &n[2]);
    property(w->target->db, "ProductVersion", &p[3], &n[3]);
    property(w->base->db, "UpgradeCode", &p[4], &n[4]);
    int rn = snprintf(rev, sizeof rev, "%.*s%.*s;%.*s%.*s%s%.*s", (int)n[0], (const char *)p[0], (int)n[1], (const char *)p[1],
                      (int)n[2], (const char *)p[2], (int)n[3], (const char *)p[3], n[4] ? ";" : "", (int)n[4], (const char *)p[4]);
    if (rn < 0 || (size_t)rn >= sizeof rev) return PROVEN_ERR_OUT_OF_BOUNDS;
    static const uint8_t none[1] = { 0 };
    for (uint32_t pid = 1; pid <= 19; ++pid) {
        rp_suminfo_prop_t v = { .pid = pid };
        const rp_suminfo_prop_t *x;
        switch (pid) {
        case 1: case 2: case 3: case 4: case 5: case 18:
            if ((x = prop(&b, pid)) == NULL) continue;
            v = *x;
            break;
        case 6:
            v.type = RP_VT_LPSTR;
            v.str = none;
            break;
        case 7: case 8:
            if ((x = prop(pid == 7 ? &a : &b, 7)) == NULL) continue;
            v = *x;
            v.pid = pid;
            break;
        case 9:
            v.type = RP_VT_LPSTR;
            v.str = (const uint8_t *)rev;
            v.str_len = (size_t)rn;
            break;
        case 14: {
            const rp_suminfo_prop_t *xa = prop(&a, 14), *xb = prop(&b, 14);
            int32_t m = xa ? xa->i : 0;
            if (xb && xb->i > m) m = xb->i;
            v.type = RP_VT_I4;
            v.i = m ? m : 200;
            break;
        }
        case 16:
            v.type = RP_VT_I4;
            v.i = (int32_t)flags;     // checks in the high word, ignored errors in the low
            break;
        default:
            continue;
        }
        s.props[s.count++] = v;
    }
    return rp_suminfo_write(&s, false, w->alloc, out, len);
}

// ---- the transform ----

static const rp_msi_wcolumn_t tables_cols[] = {
    { "Name", RP_MSI_COL_KEY | RP_MSI_COL_STRING | RP_MSI_COL_NONBINARY | RP_MSI_COL_VALID | 64 },
};
static const rp_msi_wcolumn_t columns_cols[] = {
    { "Table", RP_MSI_COL_KEY | RP_MSI_COL_STRING | RP_MSI_COL_NONBINARY | RP_MSI_COL_VALID | 64 },
    { "Number", RP_MSI_COL_KEY | RP_MSI_COL_VALID | 2 },
    { "Name", RP_MSI_COL_STRING | RP_MSI_COL_NONBINARY | RP_MSI_COL_VALID | 64 },
    { "Type", RP_MSI_COL_VALID | 2 },
};

proven_err_t rp_mst_write(proven_allocator_t alloc, const rp_mst_side_t *base, const rp_mst_side_t *target, const rp_mst_opts_t *opts,
                          const rp_limits_t *limits, uint8_t **out, size_t *len, const char **why, char *table, size_t table_cap) {
    if (base == NULL || target == NULL || opts == NULL || limits == NULL || out == NULL || len == NULL) return PROVEN_ERR_INVALID_ARG;
    wctx_t w = { .alloc = alloc, .limits = limits, .base = base, .target = target, .files = opts->files, .table = table, .table_cap = table_cap };
    uint32_t flags = opts->summary_flags;
    if (why) *why = NULL;
    if (table && table_cap) table[0] = '\0';
    const rp_msi_wdb_t *A = base->db, *B = target->db;
    rp_msi_cell_t *tcells = NULL, *ccells = NULL;
    pool_t pool = { 0 };
    rp_cfb_stream_t *list = NULL;
    uint8_t *sum = NULL;
    size_t sum_len = 0, nlist = 0;
    rp_buf_t *bufs = NULL;
    proven_err_t err = PROVEN_OK;
    if (A->codepage != B->codepage) err = refuse(&w, "", "the two packages have different code pages");

    // _Tables and _Columns: tables the target adds, and those it drops.
    size_t nt = A->table_count + B->table_count, ncol = 0;
    for (size_t t = 0; t < B->table_count; ++t) {
        if (!find_table(A, B->tables[t].name)) ncol += B->tables[t].column_count;
    }
    tcells = rp_mem_alloc(alloc, nt + 1, sizeof *tcells);
    ccells = rp_mem_alloc(alloc, 4 * ncol + 1, sizeof *ccells);
    if (err == PROVEN_OK && (tcells == NULL || ccells == NULL)) err = PROVEN_ERR_NOMEM;
    if (err == PROVEN_OK && (add_tab(&w, "_Tables", tables_cols, 1) == NULL || add_tab(&w, "_Columns", columns_cols, 4) == NULL)) {
        err = PROVEN_ERR_NOMEM;
    }
    size_t ti = 0, ci = 0;
    for (size_t t = 0; err == PROVEN_OK && t < A->table_count; ++t) {
        if (find_table(B, A->tables[t].name)) continue;
        tcells[ti] = rp_msi_str_cell(A->tables[t].name);
        if (!add_rec(&w, &w.tabs[0], &tcells[ti++], 0)) err = PROVEN_ERR_NOMEM;
    }
    for (size_t t = 0; err == PROVEN_OK && t < B->table_count; ++t) {
        const rp_msi_wtable_t *tb = &B->tables[t];
        if (find_table(A, tb->name)) continue;
        tcells[ti] = rp_msi_str_cell(tb->name);
        if (!add_rec(&w, &w.tabs[0], &tcells[ti++], 1u | 1u << 8)) err = PROVEN_ERR_NOMEM;
        for (size_t c = 0; err == PROVEN_OK && c < tb->column_count; ++c) {
            rp_msi_cell_t *row = &ccells[4 * ci++];
            row[0] = rp_msi_str_cell(tb->name);
            row[1] = (rp_msi_cell_t){ .kind = RP_MSI_NULL };
            row[2] = rp_msi_str_cell(tb->columns[c].name);
            row[3] = (rp_msi_cell_t){ .kind = RP_MSI_INT, .i = tb->columns[c].type };
            if (!add_rec(&w, &w.tabs[1], row, 1u | 4u << 8)) err = PROVEN_ERR_NOMEM;
        }
    }

    // Row records, table by table.
    for (size_t t = 0; err == PROVEN_OK && t < B->table_count; ++t) {
        const rp_msi_wtable_t *tb = &B->tables[t], *ta = find_table(A, tb->name);
        if (ta) {
            err = diff_table(&w, ta, tb);
        } else if (tb->row_count > 0) {
            if (!w.files && (strcmp(tb->name, "File") == 0 || strcmp(tb->name, "Media") == 0)) {
                err = refuse(&w, tb->name, "changes files; a transform carries no files");
                break;
            }
            otab_t *o = add_tab(&w, tb->name, tb->columns, tb->column_count);
            if (o == NULL) err = PROVEN_ERR_NOMEM;
            for (size_t r = 0; err == PROVEN_OK && r < tb->row_count; ++r) err = insert_row(&w, o, rp_msi_wrow(tb, r));
        }
    }

    // Records in key order (stable, so _Columns keeps each table's column order).
    for (size_t t = 0; err == PROVEN_OK && t < w.ntab; ++t) {
        if (!sort_recs(alloc, &w.tabs[t])) err = PROVEN_ERR_NOMEM;
    }

    if (err == PROVEN_OK) err = build_pool(&w, &pool);
    if (err == PROVEN_OK) err = summary(&w, flags, &sum, &sum_len);

    // Streams: the pool, one per table with records, the binary cells, the summary.
    size_t cap = w.ntab + w.nstream + 3;
    list = rp_mem_alloc(alloc, cap, sizeof *list);
    bufs = rp_mem_alloc(alloc, w.ntab + 2, sizeof *bufs);
    uint16_t (*names)[32] = rp_mem_alloc(alloc, cap, sizeof *names);
    if (err == PROVEN_OK && (list == NULL || bufs == NULL || names == NULL)) err = PROVEN_ERR_NOMEM;
    size_t nbuf = 0;
    if (err == PROVEN_OK) {
        rp_buf_t *p = &bufs[nbuf++], *d = &bufs[nbuf++];
        *p = rp_buf_new(alloc, limits->max_metadata);
        *d = rp_buf_new(alloc, limits->max_metadata);
        rp_buf_u32le(p, B->codepage | (pool.long_refs ? 0x80000000u : 0));
        for (size_t i = 0; i < pool.count; ++i) {
            bool high = false;
            for (size_t k = 0; k < pool.v[i].n && !high; ++k) high = pool.v[i].p[k] >= 0x80;
            uint16_t word = (uint16_t)(pool.v[i].refs | (high ? 0x8000u : 0));
            if (pool.v[i].n > 0xFFFF) {
                rp_buf_u16le(p, 0);
                rp_buf_u16le(p, word);
                rp_buf_u32le(p, (uint32_t)pool.v[i].n);
            } else {
                rp_buf_u16le(p, (uint16_t)pool.v[i].n);
                rp_buf_u16le(p, word);
            }
            rp_buf_put(d, pool.v[i].p, pool.v[i].n);
        }
        const char *pn[2] = { "_StringPool", "_StringData" };
        for (size_t k = 0; k < 2 && err == PROVEN_OK; ++k) {
            size_t nl;
            err = bufs[k].err != PROVEN_OK ? bufs[k].err : rp_msi_stream_name(pn[k], true, names[nlist], &nl);
            if (err == PROVEN_OK) list[nlist] = (rp_cfb_stream_t){ .name = names[nlist], .name_len = (uint32_t)nl, .data = bufs[k].data, .size = bufs[k].len }, nlist++;
        }
    }
    for (size_t t = 0; err == PROVEN_OK && t < w.ntab; ++t) {
        const otab_t *o = &w.tabs[t];
        if (o->nrec == 0) continue;
        rp_buf_t *b = &bufs[nbuf++];
        *b = rp_buf_new(alloc, limits->max_metadata);
        for (size_t r = 0; r < o->nrec; ++r) put_record(b, &pool, o, &o->recs[r]);
        size_t nl;
        err = b->err != PROVEN_OK ? b->err : rp_msi_stream_name(o->name, true, names[nlist], &nl);
        if (err == PROVEN_OK) list[nlist] = (rp_cfb_stream_t){ .name = names[nlist], .name_len = (uint32_t)nl, .data = b->data, .size = b->len }, nlist++;
    }
    for (size_t s = 0; err == PROVEN_OK && s < w.nstream; ++s) {
        size_t nl;
        err = rp_msi_stream_name(w.streams[s].name, false, names[nlist], &nl);
        if (err == PROVEN_OK) list[nlist] = (rp_cfb_stream_t){ .name = names[nlist], .name_len = (uint32_t)nl, .data = w.streams[s].data, .size = w.streams[s].len }, nlist++;
    }
    if (err == PROVEN_OK) {
        static const char sname[] = "\005SummaryInformation";
        for (size_t i = 0; sname[i]; ++i) names[nlist][i] = (uint8_t)sname[i];
        list[nlist] = (rp_cfb_stream_t){ .name = names[nlist], .name_len = sizeof sname - 1, .data = sum, .size = sum_len };
        nlist++;
        static const uint8_t mst_clsid[16] = { 0x82, 0x10, 0x0C, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
        err = rp_cfb_write(alloc, 9, mst_clsid, list, nlist, limits, out, len);
    }

    if (why) *why = w.why;
    for (size_t i = 0; bufs && i < nbuf; ++i) rp_buf_free(&bufs[i]);
    rp_mem_free(alloc, bufs);
    rp_mem_free(alloc, names);
    rp_mem_free(alloc, list);
    rp_mem_free(alloc, sum);
    rp_mem_free(alloc, pool.v);
    for (size_t s = 0; s < w.nstream; ++s) rp_mem_free(alloc, w.streams[s].data);
    rp_mem_free(alloc, w.streams);
    for (size_t t = 0; t < w.ntab; ++t) rp_mem_free(alloc, w.tabs[t].recs);
    rp_mem_free(alloc, w.tabs);
    rp_mem_free(alloc, tcells);
    rp_mem_free(alloc, ccells);
    return err;
}

// ---- listing ---------------------------------------------------------------------------------

typedef struct {
    uint32_t off, len;
} lstr_t;

typedef struct {
    char             name[64];
    rp_msi_wcolumn_t cols[32];
    char             colnames[32][64];
    size_t           ncols;
} schema_t;

typedef struct {
    proven_allocator_t alloc;
    const rp_cfb_t    *mst;
    uint32_t           storage;     // the transform's storage in mst
    const rp_limits_t *limits;
    uint8_t           *data;
    size_t             data_len;
    lstr_t            *strs;
    size_t             nstr;
    bool               long_refs;
    schema_t          *added;       // tables the transform adds (from its _Columns)
    size_t             nadded, addcap;
} lctx_t;

// The bytes of the transform's string `id` (id < nstr, checked when the record was read).
static const uint8_t *lstr(const lctx_t *l, uint32_t id, size_t *len) {
    static const uint8_t none[1];
    rp_in_t in = rp_in(l->data, l->data_len);
    const uint8_t *p = id < l->nstr ? rp_in_bytes(&in, l->strs[id].off, l->strs[id].len) : NULL;
    *len = p ? l->strs[id].len : 0;
    return p ? p : none;
}

static void put_text(rp_buf_t *b, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < 0x20 || p[i] == 0x7F) {
            char x[5];
            snprintf(x, sizeof x, "\\x%02X", p[i]);
            rp_buf_puts(b, x);
        } else {
            rp_buf_byte(b, p[i]);
        }
    }
}

static schema_t *added_schema(lctx_t *l, const uint8_t *name, size_t n) {
    for (size_t i = 0; i < l->nadded; ++i) {
        if (strlen(l->added[i].name) == n && memcmp(l->added[i].name, name, n) == 0) return &l->added[i];
    }
    if (n >= sizeof l->added[0].name) return NULL;
    if (l->nadded == l->addcap) {
        size_t cap = l->addcap ? l->addcap * 2 : 8;
        schema_t *v = rp_mem_alloc(l->alloc, cap, sizeof *v);
        if (v == NULL) return NULL;
        if (l->nadded) memcpy(v, l->added, l->nadded * sizeof *v);
        rp_mem_free(l->alloc, l->added);
        l->added = v;
        l->addcap = cap;
    }
    schema_t *s = &l->added[l->nadded++];
    memset(s, 0, sizeof *s);
    memcpy(s->name, name, n);
    return s;
}

typedef struct {
    uint8_t  kind;          // RP_MSI_*
    int32_t  i;
    uint32_t s;             // string id; binary: the marker
} lval_t;

static proven_err_t list_table(lctx_t *l, const char *name, const rp_msi_wcolumn_t *cols, size_t ncols, const uint8_t *d, size_t n,
                               rp_buf_t *out, const char **why) {
    size_t ref = l->long_refs ? 3 : 2;
    if (ncols > 32) {
        *why = TR_N("a table has more than 32 columns");
        return PROVEN_ERR_INVALID_FORMAT;
    }
    rp_in_t in = rp_in(d, n);
    for (size_t o = 0; o < n;) {
        if (n - o < 2) {
            *why = TR_N("a record is cut short");
            return PROVEN_ERR_INVALID_FORMAT;
        }
        uint32_t mask = rp_in16(&in, o);
        o += 2;
        if ((mask & 1) && (mask >> 8) > ncols) {
            *why = TR_N("a record has more columns than its table");
            return PROVEN_ERR_INVALID_FORMAT;
        }
        lval_t v[32];
        for (size_t c = 0; c < ncols; ++c) {
            v[c] = (lval_t){ .kind = RP_MSI_NULL };
            uint16_t type = cols[c].type;
            if (!present(type, c, mask)) continue;
            size_t w = (type & RP_MSI_COL_STRING) ? (rp_msi_col_binary(type) ? 2 : ref) : (type & RP_MSI_COL_WIDTH) == 4 ? 4 : 2;
            if (n - o < w) {
                *why = TR_N("a record is cut short");
                return PROVEN_ERR_INVALID_FORMAT;
            }
            uint32_t x = w == 4 ? rp_in32(&in, o) : w == 3 ? rp_in16(&in, o) | (uint32_t)rp_in8(&in, o + 2) << 16 : rp_in16(&in, o);
            if (in.bad) {
                *why = TR_N("a record is cut short");
                return PROVEN_ERR_INVALID_FORMAT;
            }
            o += w;
            if (x == 0) continue;
            if (rp_msi_col_binary(type)) v[c] = (lval_t){ .kind = RP_MSI_BINARY, .s = x };
            else if (type & RP_MSI_COL_STRING) {
                if (x >= l->nstr) {
                    *why = TR_N("a record names a string the pool does not have");
                    return PROVEN_ERR_INVALID_FORMAT;
                }
                v[c] = (lval_t){ .kind = RP_MSI_STR, .s = x };
            } else {
                v[c] = (lval_t){ .kind = RP_MSI_INT, .i = w == 4 ? (int32_t)(x ^ 0x80000000u) : (int32_t)x - 0x8000 };
            }
        }
        // A new table's columns, for the records of that table.
        if (strcmp(name, "_Columns") == 0 && v[0].kind == RP_MSI_STR && v[2].kind == RP_MSI_STR && v[3].kind == RP_MSI_INT) {
            size_t tn, cn;
            const uint8_t *tname = lstr(l, v[0].s, &tn), *cname = lstr(l, v[2].s, &cn);
            schema_t *s = added_schema(l, tname, tn);
            if (s && s->ncols < 32 && cn < sizeof s->colnames[0]) {
                memcpy(s->colnames[s->ncols], cname, cn);
                s->colnames[s->ncols][cn] = '\0';
                s->cols[s->ncols] = (rp_msi_wcolumn_t){ s->colnames[s->ncols], (uint16_t)v[3].i };
                s->ncols++;
            }
        }
        rp_buf_puts(out, name);
        rp_buf_puts(out, mask == 0 ? "\tdelete" : (mask & 1) ? "\tinsert" : "\tupdate");
        char sname[256];
        size_t so = (size_t)snprintf(sname, sizeof sname, "%s", name);
        for (size_t c = 0; c < ncols; ++c) {
            if (!(cols[c].type & RP_MSI_COL_KEY)) continue;
            if (v[c].kind == RP_MSI_STR && so < sizeof sname) {
                size_t kn;
                const uint8_t *kv = lstr(l, v[c].s, &kn);
                so += (size_t)snprintf(sname + so, sizeof sname - so, ".%.*s", (int)kn, (const char *)kv);
            } else if (so < sizeof sname) {
                so += (size_t)snprintf(sname + so, sizeof sname - so, ".%ld", v[c].kind == RP_MSI_INT ? (long)v[c].i : 0L);
            }
        }
        for (size_t c = 0; c < ncols; ++c) {
            if (!present(cols[c].type, c, mask)) continue;
            rp_buf_byte(out, '\t');
            if (!(mask & 1) && !(cols[c].type & RP_MSI_COL_KEY)) {
                rp_buf_puts(out, cols[c].name);
                rp_buf_byte(out, '=');
            }
            if (v[c].kind == RP_MSI_STR) {
                size_t sn;
                const uint8_t *sv = lstr(l, v[c].s, &sn);
                put_text(out, sv, sn);
            } else if (v[c].kind == RP_MSI_INT) {
                rp_buf_long(out, v[c].i);
            } else if (v[c].kind == RP_MSI_BINARY) {
                uint16_t packed[32];
                size_t pl;
                uint32_t id;
                rp_buf_puts(out, "[stream ");
                put_text(out, (const uint8_t *)sname, so < sizeof sname ? so : sizeof sname - 1);
                if (rp_msi_stream_name(sname, false, packed, &pl) == PROVEN_OK && rp_cfb_find(l->mst, l->storage, packed, pl, &id) == PROVEN_OK) {
                    rp_buf_puts(out, ", ");
                    rp_buf_long(out, (long long)l->mst->entries[id].size);
                    rp_buf_puts(out, " bytes]");
                } else {
                    rp_buf_puts(out, ", missing]");
                }
            }
        }
        rp_buf_byte(out, '\n');
    }
    return PROVEN_OK;
}

proven_err_t rp_mst_list(proven_allocator_t alloc, const rp_cfb_t *mst, uint32_t storage, const rp_msi_wdb_t *base,
                         const rp_limits_t *limits, rp_buf_t *out, const char **why) {
    static const char *no = "";
    *why = no;
    lctx_t l = { .alloc = alloc, .mst = mst, .storage = storage, .limits = limits };
    uint8_t *pool = NULL, *d = NULL;
    size_t pool_len = 0, dn = 0;
    uint32_t *ids = NULL;
    char (*tnames)[64] = NULL;
    proven_err_t err = read_stream(alloc, mst, storage, "_StringPool", true, limits->max_metadata, &pool, &pool_len);
    if (err == PROVEN_OK) err = read_stream(alloc, mst, storage, "_StringData", true, limits->max_metadata, &l.data, &l.data_len);
    if (err != PROVEN_OK || pool_len < 4) {
        *why = TR_N("no string pool: not a transform");
        err = PROVEN_ERR_INVALID_FORMAT;
    }
    if (err == PROVEN_OK) {
        rp_in_t pin = rp_in(pool, pool_len);
        l.long_refs = (rp_in8(&pin, 3) & 0x80) != 0;
        l.strs = rp_mem_alloc(alloc, pool_len / 4 + 1, sizeof *l.strs);
        if (l.strs == NULL) err = PROVEN_ERR_NOMEM;
        l.nstr = 1;
        uint64_t off = 0;
        for (size_t i = 4; err == PROVEN_OK && i + 4 <= pool_len; i += 4) {
            uint32_t n = rp_in16(&pin, i);
            uint16_t refs = rp_in16(&pin, i + 2);
            if (n == 0 && refs != 0) {
                if (i + 8 > pool_len) break;
                n = rp_in32(&pin, i + 4);
                i += 4;
            }
            if (off + n > l.data_len) {
                *why = TR_N("string lengths exceed the string data");
                err = PROVEN_ERR_INVALID_FORMAT;
                break;
            }
            l.strs[l.nstr++] = (lstr_t){ (uint32_t)off, n };
            off += n;
        }
    }
    // The table streams: _Tables, _Columns, then the others by name.
    size_t count = 0, nt = 0;
    if (err == PROVEN_OK) err = rp_cfb_children(mst, storage, NULL, 0, &count);
    if (err == PROVEN_OK) {
        ids = rp_mem_alloc(alloc, count + 1, sizeof *ids);
        tnames = rp_mem_alloc(alloc, count + 1, sizeof *tnames);
        if (ids == NULL || tnames == NULL) err = PROVEN_ERR_NOMEM;
    }
    if (err == PROVEN_OK) err = rp_cfb_children(mst, storage, ids, count, &count);
    for (size_t i = 0; err == PROVEN_OK && i < count; ++i) {
        const rp_cfb_entry_t *e = &mst->entries[ids[i]];
        bool table = false;
        char name[64];
        if (rp_msi_unpack_name(e->name, e->name_len, name, sizeof name, &table) != PROVEN_OK || !table) continue;
        if (strcmp(name, "_StringPool") == 0 || strcmp(name, "_StringData") == 0) continue;
        size_t k = nt++;
        while (k > 0) {
            int rank_a = strcmp(tnames[k - 1], "_Tables") == 0 ? 0 : strcmp(tnames[k - 1], "_Columns") == 0 ? 1 : 2;
            int rank_b = strcmp(name, "_Tables") == 0 ? 0 : strcmp(name, "_Columns") == 0 ? 1 : 2;
            if (rank_a < rank_b || (rank_a == rank_b && strcmp(tnames[k - 1], name) < 0)) break;
            memcpy(tnames[k], tnames[k - 1], sizeof tnames[0]);
            --k;
        }
        snprintf(tnames[k], sizeof tnames[0], "%s", name);
    }
    for (size_t t = 0; err == PROVEN_OK && t < nt; ++t) {
        const char *name = tnames[t];
        const rp_msi_wcolumn_t *cols = NULL;
        size_t ncols = 0;
        const rp_msi_wtable_t *bt = find_table(base, name);
        schema_t *s = NULL;
        if (strcmp(name, "_Tables") == 0) cols = tables_cols, ncols = 1;
        else if (strcmp(name, "_Columns") == 0) cols = columns_cols, ncols = 4;
        else if (bt) cols = bt->columns, ncols = bt->column_count;
        else if ((s = added_schema(&l, (const uint8_t *)name, strlen(name))) != NULL && s->ncols) cols = s->cols, ncols = s->ncols;
        if (cols == NULL) {
            *why = TR_N("has records for a table neither the base nor the transform defines");
            err = PROVEN_ERR_INVALID_FORMAT;
            break;
        }
        err = read_stream(alloc, mst, storage, name, true, limits->max_metadata, &d, &dn);
        if (err == PROVEN_OK) err = list_table(&l, name, cols, ncols, d, dn, out, why);
        rp_mem_free(alloc, d);
        d = NULL;
    }
    if (err == PROVEN_OK && out->err != PROVEN_OK) err = out->err;
    rp_mem_free(alloc, pool);
    rp_mem_free(alloc, l.data);
    rp_mem_free(alloc, l.strs);
    rp_mem_free(alloc, l.added);
    rp_mem_free(alloc, ids);
    rp_mem_free(alloc, tnames);
    return err;
}

// ---- diff (RFC-0031 B27) ---------------------------------------------------------------------------

static void diff_text(rp_buf_t *b, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < 0x20 || p[i] == 0x7F) {
            char x[5];
            snprintf(x, sizeof x, "\\x%02X", p[i]);
            rp_buf_puts(b, x);
        } else {
            rp_buf_byte(b, p[i]);
        }
    }
}

static void diff_cell(rp_buf_t *out, const rp_cfb_t *cfb, const char *table, const rp_msi_wcolumn_t *cols, size_t nc, const rp_msi_cell_t *row,
                      size_t c) {
    const rp_msi_cell_t *v = &row[c];
    if (is_null(v)) return;
    if (rp_msi_col_binary(cols[c].type)) {
        char name[128];
        uint16_t packed[32];
        size_t pl;
        uint32_t id;
        rp_buf_puts(out, "[stream ");
        if (!binary_name(table, cols, nc, row, name, sizeof name)) {
            rp_buf_puts(out, table);
            rp_buf_puts(out, "...]");
            return;
        }
        diff_text(out, (const uint8_t *)name, strlen(name));
        if (rp_msi_stream_name(name, false, packed, &pl) == PROVEN_OK && rp_cfb_find(cfb, 0, packed, pl, &id) == PROVEN_OK) {
            rp_buf_puts(out, ", ");
            rp_buf_long(out, (long long)cfb->entries[id].size);
            rp_buf_puts(out, " bytes]");
        } else {
            rp_buf_puts(out, ", missing]");
        }
    } else if (v->kind == RP_MSI_STR) {
        diff_text(out, v->bytes, v->len);
    } else if (v->kind == RP_MSI_INT) {
        rp_buf_long(out, v->i);
    }
}

static void diff_keys(rp_buf_t *out, const rp_cfb_t *cfb, const rp_msi_wtable_t *t, const rp_msi_cell_t *row) {
    for (size_t c = 0; c < t->column_count; ++c) {
        if (!(t->columns[c].type & RP_MSI_COL_KEY)) continue;
        rp_buf_byte(out, '\t');
        diff_cell(out, cfb, t->name, t->columns, t->column_count, row, c);
    }
}

static void diff_row(rp_buf_t *out, const rp_cfb_t *cfb, const rp_msi_wtable_t *t, const rp_msi_cell_t *row, bool insert) {
    rp_buf_puts(out, t->name);
    rp_buf_puts(out, insert ? "\tinsert" : "\tdelete");
    if (!insert) {
        diff_keys(out, cfb, t, row);
    } else {
        for (size_t c = 0; c < t->column_count; ++c) {
            rp_buf_byte(out, '\t');
            diff_cell(out, cfb, t->name, t->columns, t->column_count, row, c);
        }
    }
    rp_buf_byte(out, '\n');
}

static void diff_columns(rp_buf_t *out, const rp_msi_wtable_t *t) {
    for (size_t c = 0; c < t->column_count; ++c) {
        if (c) rp_buf_byte(out, ' ');
        rp_buf_puts(out, t->columns[c].name);
    }
}

static proven_err_t diff_rows(proven_allocator_t alloc, const rp_mst_side_t *sa, const rp_mst_side_t *sb, const rp_limits_t *limits,
                              const rp_msi_wtable_t *a, const rp_msi_wtable_t *b, rp_buf_t *out) {
    size_t nc = b->column_count;
    const rp_msi_cell_t **ra = rp_mem_alloc(alloc, (a ? a->row_count : 0) + 1, sizeof *ra);
    const rp_msi_cell_t **rb = rp_mem_alloc(alloc, b->row_count + 1, sizeof *rb);
    size_t na = a ? a->row_count : 0;
    proven_err_t err = ra && rb ? PROVEN_OK : PROVEN_ERR_NOMEM;
    for (size_t r = 0; err == PROVEN_OK && r < na; ++r) ra[r] = rp_msi_wrow(a, r);              // same columns as b (the caller)
    for (size_t r = 0; err == PROVEN_OK && r < b->row_count; ++r) rb[r] = rp_msi_wrow(b, r);
    if (err == PROVEN_OK && (!sort_rows(alloc, b->columns, nc, ra, na) || !sort_rows(alloc, b->columns, nc, rb, b->row_count))) err = PROVEN_ERR_NOMEM;
    size_t i = 0, j = 0;
    while (err == PROVEN_OK && (i < na || j < b->row_count)) {
        int c = i == na ? 1 : j == b->row_count ? -1 : key_cmp(b->columns, nc, ra[i], rb[j]);
        if (c < 0) {
            diff_row(out, sa->cfb, a, ra[i], false);
        } else if (c > 0) {
            diff_row(out, sb->cfb, b, rb[j], true);
        } else {
            bool any = false;
            for (size_t k = 0; k < nc; ++k) {
                uint16_t type = b->columns[k].type;
                if (type & RP_MSI_COL_KEY) continue;
                bool same;
                if (rp_msi_col_binary(type)) {
                    same = is_null(&ra[i][k]) == is_null(&rb[j][k]);
                    if (same && !is_null(&ra[i][k])) {
                        char name[128];
                        uint8_t *x = NULL, *y = NULL;
                        size_t xn = 0, yn = 0;
                        same = binary_name(b->name, b->columns, nc, ra[i], name, sizeof name) &&
                               read_stream(alloc, sa->cfb, 0, name, false, (size_t)limits->max_output, &x, &xn) == PROVEN_OK &&
                               read_stream(alloc, sb->cfb, 0, name, false, (size_t)limits->max_output, &y, &yn) == PROVEN_OK &&
                               xn == yn && memcmp(x, y, xn) == 0;
                        rp_mem_free(alloc, x);
                        rp_mem_free(alloc, y);
                    }
                } else {
                    same = is_null(&ra[i][k]) == is_null(&rb[j][k]) && cell_cmp(&ra[i][k], &rb[j][k], type) == 0;
                }
                if (same) continue;
                if (!any) {
                    rp_buf_puts(out, b->name);
                    rp_buf_puts(out, "\tupdate");
                    diff_keys(out, sb->cfb, b, rb[j]);
                    any = true;
                }
                rp_buf_byte(out, '\t');
                rp_buf_puts(out, b->columns[k].name);
                rp_buf_byte(out, '=');
                diff_cell(out, sa->cfb, b->name, b->columns, nc, ra[i], k);
                rp_buf_puts(out, " -> ");
                diff_cell(out, sb->cfb, b->name, b->columns, nc, rb[j], k);
            }
            if (any) rp_buf_byte(out, '\n');
        }
        if (c <= 0) ++i;
        if (c >= 0) ++j;
    }
    rp_mem_free(alloc, ra);
    rp_mem_free(alloc, rb);
    return err;
}

// The summary as "<property>\t<value>" lines (rp_suminfo_export_idt without its three header lines).
static proven_err_t summary_lines(proven_allocator_t alloc, const rp_msi_wdb_t *db, uint8_t **idt, size_t *len, const char **body) {
    *idt = NULL;
    *len = 0;
    *body = "";
    if (db->summary == NULL) return PROVEN_OK;
    rp_suminfo_t si = { .count = 0 };
    proven_err_t err = rp_suminfo_parse(db->summary, db->summary_len, &si);
    uint8_t *raw = NULL;
    size_t rl = 0;
    if (err == PROVEN_OK) err = rp_suminfo_export_idt(&si, alloc, &raw, &rl);
    if (err != PROVEN_OK) return err;
    *idt = rp_mem_alloc(alloc, rl + 1, 1);        // with a terminating NUL, for the lookups below
    if (*idt == NULL) {
        rp_mem_free(alloc, raw);
        return PROVEN_ERR_NOMEM;
    }
    memcpy(*idt, raw, rl);
    (*idt)[rl] = 0;
    *len = rl;
    rp_mem_free(alloc, raw);
    const char *p = (const char *)*idt;
    for (int k = 0; k < 3 && p; ++k) {
        p = memchr(p, '\n', *len - (size_t)(p - (const char *)*idt));
        if (p) ++p;
    }
    *body = p ? p : "";
    return PROVEN_OK;
}

static const char *summary_value(const char *body, const char *pid, size_t *n) {
    size_t pl = strlen(pid);
    for (const char *p = body; *p;) {
        const char *e = strchr(p, '\n');
        size_t ll = e ? (size_t)(e - p) : strlen(p);
        if (ll > pl && memcmp(p, pid, pl) == 0 && p[pl] == '\t') {
            *n = ll - pl - 1;
            if (*n && p[pl + 1 + *n - 1] == '\r') --*n;
            return p + pl + 1;
        }
        p += ll + (e ? 1 : 0);
    }
    return NULL;
}

static void diff_summary(rp_buf_t *out, const char *x, const char *y) {
    for (unsigned pid = 0; pid < 32; ++pid) {
        char k[8];
        snprintf(k, sizeof k, "%u", pid);
        size_t xn = 0, yn = 0;
        const char *a = summary_value(x, k, &xn), *b = summary_value(y, k, &yn);
        if ((a == NULL && b == NULL) || (a && b && xn == yn && memcmp(a, b, xn) == 0)) continue;
        rp_buf_puts(out, "_SummaryInformation\t");
        rp_buf_puts(out, k);
        rp_buf_byte(out, '\t');
        if (a) diff_text(out, (const uint8_t *)a, xn);
        rp_buf_puts(out, " -> ");
        if (b) diff_text(out, (const uint8_t *)b, yn);
        rp_buf_byte(out, '\n');
    }
}

proven_err_t rp_msi_diff(proven_allocator_t alloc, const rp_mst_side_t *a, const rp_mst_side_t *b, const rp_limits_t *limits, rp_buf_t *out) {
    size_t n = a->db->table_count + b->db->table_count, nn = 0;
    const char **names = rp_mem_alloc(alloc, n + 1, sizeof *names);
    if (names == NULL) return PROVEN_ERR_NOMEM;
    for (size_t t = 0; t < a->db->table_count; ++t) names[nn++] = a->db->tables[t].name;
    for (size_t t = 0; t < b->db->table_count; ++t) {
        if (find_table(a->db, b->db->tables[t].name) == NULL) names[nn++] = b->db->tables[t].name;
    }
    for (size_t i = 1; i < nn; ++i) {          // name order (a few dozen tables)
        for (size_t k = i; k > 0 && strcmp(names[k - 1], names[k]) > 0; --k) {
            const char *t = names[k];
            names[k] = names[k - 1];
            names[k - 1] = t;
        }
    }
    proven_err_t err = PROVEN_OK;
    for (size_t k = 0; k < nn && err == PROVEN_OK; ++k) {
        const rp_msi_wtable_t *ta = find_table(a->db, names[k]), *tb = find_table(b->db, names[k]);
        if (tb == NULL) {
            rp_buf_puts(out, names[k]);
            rp_buf_puts(out, "\tdropped table\n");
        } else if (ta == NULL) {
            rp_buf_puts(out, names[k]);
            rp_buf_puts(out, "\tnew table\t");
            diff_columns(out, tb);
            rp_buf_byte(out, '\n');
            err = diff_rows(alloc, a, b, limits, NULL, tb, out);
        } else if (!same_columns(ta, tb)) {
            rp_buf_puts(out, names[k]);
            rp_buf_puts(out, "\tcolumns\t");
            diff_columns(out, ta);
            rp_buf_puts(out, " -> ");
            diff_columns(out, tb);
            rp_buf_byte(out, '\n');
        } else {
            err = diff_rows(alloc, a, b, limits, ta, tb, out);
        }
    }
    rp_mem_free(alloc, names);
    uint8_t *xi = NULL, *yi = NULL;
    size_t xl = 0, yl = 0;
    const char *x = "", *y = "";
    if (err == PROVEN_OK) err = summary_lines(alloc, a->db, &xi, &xl, &x);
    if (err == PROVEN_OK) err = summary_lines(alloc, b->db, &yi, &yl, &y);
    if (err == PROVEN_OK) diff_summary(out, x, y);
    rp_mem_free(alloc, xi);
    rp_mem_free(alloc, yi);
    return err == PROVEN_OK ? out->err : err;
}
