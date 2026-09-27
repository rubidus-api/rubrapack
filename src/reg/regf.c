// src/reg/regf.c - registry hive files (include/rubrapack/regf.h; RFC-0010, RFC-0001 F12).

#include "rubrapack/regf.h"

#include "rubrapack/buf.h"
#include "rubrapack/mem.h"
#include "rubrapack/text.h"

#include <string.h>

extern const size_t rp_upcase_count;
extern const uint16_t rp_upcase_pairs[][2];

uint16_t rp_upcase16(uint16_t c) {
    if (c < 0x61) return c;
    size_t lo = 0, hi = rp_upcase_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (rp_upcase_pairs[mid][0] == c) return rp_upcase_pairs[mid][1];
        if (rp_upcase_pairs[mid][0] < c) lo = mid + 1;
        else hi = mid;
    }
    return c;
}

enum {
    BLOCK = 4096, BIN_HEADER = 32, MAX_KEY_NAME = 255, MAX_VALUE_NAME = 16383, MAX_DATA = 16u << 20, BIG = 16344,
    LIST_MAX = 507,                         // lh entries in one 4 KiB bin (as offreg splits them)
    NK_COMP = 0x20, VK_COMP = 0x0001,
};
#define NO_CELL 0xFFFFFFFFu
#define INLINE 0x80000000u     // value data kept in the value cell

// ---- the tree ------------------------------------------------------------------------------------

typedef struct {
    uint16_t *name;
    size_t    nlen;
    uint32_t  type;
    uint8_t  *data;
    size_t    len;
    uint32_t  vk, dat, db, seglist, *segs;  // cell offsets (layout)
} val_t;

typedef struct key key_t;
struct key {
    uint16_t *name;
    size_t    nlen;
    key_t   **subs;             // sorted by upper-case name
    size_t    nsub, capsub;
    val_t    *vals;             // in the order set
    size_t    nval, capval;
    key_t    *parent;
    uint32_t  nk, list, *lh, vlist; // cell offsets (layout)
    size_t    nlh;
};

struct rp_regf {
    proven_allocator_t alloc;
    key_t              root;
    size_t             keys;
};

static int cmp_names(const uint16_t *a, size_t an, const uint16_t *b, size_t bn) {
    for (size_t i = 0; i < an && i < bn; ++i) {
        uint16_t x = rp_upcase16(a[i]), y = rp_upcase16(b[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return an < bn ? -1 : an > bn;
}

rp_regf_t *rp_regf_new(proven_allocator_t alloc) {
    rp_regf_t *h = rp_mem_alloc(alloc, 1, sizeof *h);
    if (h == NULL) return NULL;
    memset(h, 0, sizeof *h);
    h->alloc = alloc;
    static const uint16_t ROOT[4] = { 'R', 'O', 'O', 'T' };
    h->root.name = rp_mem_alloc(alloc, 4, sizeof *h->root.name);
    if (h->root.name == NULL) {
        rp_mem_free(alloc, h);
        return NULL;
    }
    memcpy(h->root.name, ROOT, sizeof ROOT);
    h->root.nlen = 4;
    h->keys = 1;
    return h;
}

static void free_key(proven_allocator_t a, key_t *k) {
    for (size_t i = 0; i < k->nsub; ++i) {
        free_key(a, k->subs[i]);
        rp_mem_free(a, k->subs[i]);
    }
    for (size_t i = 0; i < k->nval; ++i) {
        rp_mem_free(a, k->vals[i].name);
        rp_mem_free(a, k->vals[i].data);
        rp_mem_free(a, k->vals[i].segs);
    }
    rp_mem_free(a, k->subs);
    rp_mem_free(a, k->vals);
    rp_mem_free(a, k->name);
    rp_mem_free(a, k->lh);
}

void rp_regf_free(rp_regf_t *h) {
    if (h == NULL) return;
    free_key(h->alloc, &h->root);
    rp_mem_free(h->alloc, h);
}

static proven_err_t to16(proven_allocator_t a, const char *s, size_t n, uint16_t **out, size_t *units) {
    rp_text_result_t r = rp_utf8_to_utf16((const uint8_t *)s, n, NULL, 0);
    if (r.err != PROVEN_OK) return PROVEN_ERR_INVALID_ARG;
    uint16_t *w = rp_mem_alloc(a, r.units ? r.units : 1, sizeof *w);
    if (w == NULL) return PROVEN_ERR_NOMEM;
    if (n) r = rp_utf8_to_utf16((const uint8_t *)s, n, w, r.units);
    *out = w;
    *units = r.units;
    return r.err;
}

proven_err_t rp_regf_set(rp_regf_t *h, const char *path, const char *value, uint32_t type, const uint8_t *data, size_t len) {
    if (len > MAX_DATA || (len && data == NULL)) return PROVEN_ERR_INVALID_ARG;
    // No empty name anywhere in the path (checked before anything is created).
    if (path && *path && (path[0] == '\\' || path[strlen(path) - 1] == '\\' || strstr(path, "\\\\"))) return PROVEN_ERR_INVALID_ARG;
    key_t *k = &h->root;
    for (const char *p = path; p && *p;) {
        const char *e = strchr(p, '\\');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == 0) return PROVEN_ERR_INVALID_ARG;
        uint16_t *name;
        size_t units;
        proven_err_t err = to16(h->alloc, p, n, &name, &units);
        if (err != PROVEN_OK || units > MAX_KEY_NAME) {
            if (err == PROVEN_OK) rp_mem_free(h->alloc, name);
            return err == PROVEN_ERR_NOMEM ? err : PROVEN_ERR_INVALID_ARG;
        }
        // Binary search in the sorted subkeys; insert when missing.
        size_t lo = 0, hi = k->nsub;
        int c = 1;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            c = cmp_names(k->subs[mid]->name, k->subs[mid]->nlen, name, units);
            if (c == 0) {
                lo = mid;
                break;
            }
            if (c < 0) lo = mid + 1;
            else hi = mid;
        }
        if (lo < k->nsub && c == 0) {
            rp_mem_free(h->alloc, name);
            k = k->subs[lo];
        } else {
            if (k->nsub == k->capsub) {
                size_t cap = k->capsub ? k->capsub * 2 : 4;
                key_t **ns = rp_mem_alloc(h->alloc, cap, sizeof *ns);
                if (ns == NULL) {
                    rp_mem_free(h->alloc, name);
                    return PROVEN_ERR_NOMEM;
                }
                if (k->nsub) memcpy(ns, k->subs, k->nsub * sizeof *ns);
                rp_mem_free(h->alloc, k->subs);
                k->subs = ns;
                k->capsub = cap;
            }
            key_t *nk = rp_mem_alloc(h->alloc, 1, sizeof *nk);
            if (nk == NULL) {
                rp_mem_free(h->alloc, name);
                return PROVEN_ERR_NOMEM;
            }
            memset(nk, 0, sizeof *nk);
            nk->name = name;
            nk->nlen = units;
            nk->parent = k;
            memmove(k->subs + lo + 1, k->subs + lo, (k->nsub - lo) * sizeof *k->subs);
            k->subs[lo] = nk;
            ++k->nsub;
            ++h->keys;
            k = nk;
        }
        p = e ? e + 1 : p + n;
    }
    if (value == NULL) return PROVEN_OK;
    uint16_t *name;
    size_t units;
    proven_err_t err = to16(h->alloc, value, strlen(value), &name, &units);
    if (err != PROVEN_OK) return err;
    if (units > MAX_VALUE_NAME) {
        rp_mem_free(h->alloc, name);
        return PROVEN_ERR_INVALID_ARG;
    }
    uint8_t *copy = rp_mem_alloc(h->alloc, len ? len : 1, 1);
    if (copy == NULL) {
        rp_mem_free(h->alloc, name);
        return PROVEN_ERR_NOMEM;
    }
    if (len) memcpy(copy, data, len);
    for (size_t i = 0; i < k->nval; ++i) {
        if (cmp_names(k->vals[i].name, k->vals[i].nlen, name, units) == 0) {
            rp_mem_free(h->alloc, name);
            rp_mem_free(h->alloc, k->vals[i].data);
            k->vals[i].data = copy;
            k->vals[i].len = len;
            k->vals[i].type = type;
            return PROVEN_OK;
        }
    }
    if (k->nval == k->capval) {
        size_t cap = k->capval ? k->capval * 2 : 4;
        val_t *nv = rp_mem_alloc(h->alloc, cap, sizeof *nv);
        if (nv == NULL) {
            rp_mem_free(h->alloc, name);
            rp_mem_free(h->alloc, copy);
            return PROVEN_ERR_NOMEM;
        }
        if (k->nval) memcpy(nv, k->vals, k->nval * sizeof *nv);
        rp_mem_free(h->alloc, k->vals);
        k->vals = nv;
        k->capval = cap;
    }
    memset(&k->vals[k->nval], 0, sizeof k->vals[0]);
    k->vals[k->nval++] = (val_t){ .name = name, .nlen = units, .type = type, .data = copy, .len = len };
    return PROVEN_OK;
}

// ---- layout: cell offsets, depth first -------------------------------------------------------------

static bool compressible(const uint16_t *s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (s[i] > 0xFF) return false;
    }
    return true;
}

static size_t name_bytes(const uint16_t *s, size_t n) { return compressible(s, n) ? n : 2 * n; }
static uint32_t cell_size(size_t payload) { return (uint32_t)((4 + payload + 7) & ~(size_t)7); }

typedef struct {
    uint64_t bin_start, bin_end, at;    // relative to the first bin
    rp_buf_t bins;                      // [bin start, bin size] pairs, 8 bytes each
    bool     ok;
} lay_t;

static uint32_t place(lay_t *l, uint32_t size) {
    if (l->at + size > l->bin_end) {
        uint64_t bsize = ((uint64_t)BIN_HEADER + size + BLOCK - 1) / BLOCK * BLOCK;
        l->bin_start = l->bin_end;
        l->bin_end = l->bin_start + bsize;
        l->at = l->bin_start + BIN_HEADER;
        uint32_t pair[2] = { (uint32_t)l->bin_start, (uint32_t)bsize };
        rp_buf_put(&l->bins, pair, sizeof pair);
        if (l->bin_end > 0x7FFFFFF0u) l->ok = false;
    }
    uint32_t off = (uint32_t)l->at;
    l->at += size;
    return off;
}

// The security descriptor offreg gives every key of an MSIX Registry.dat: owner and group
// Administrators; SYSTEM and Administrators full control, Everyone and RESTRICTED read, each ACE
// inherited by subkeys (built here from those parts).
static const uint8_t SK_DESC[] = {
    0x01, 0x00, 0x04, 0x80,                             // revision 1, SE_SELF_RELATIVE | SE_DACL_PRESENT
    0x70, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,     // owner at 0x70, group at 0x80
    0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00,     // no SACL, DACL at 0x14
    0x02, 0x00, 0x5C, 0x00, 0x04, 0x00, 0x00, 0x00,     // ACL revision 2, 0x5C bytes, 4 ACEs
    0x00, 0x02, 0x14, 0x00, 0x3F, 0x00, 0x0F, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x12, 0x00, 0x00, 0x00,
    0x00, 0x02, 0x18, 0x00, 0x3F, 0x00, 0x0F, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x20, 0x00, 0x00, 0x00,
    0x20, 0x02, 0x00, 0x00,
    0x00, 0x02, 0x14, 0x00, 0x19, 0x00, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x02, 0x14, 0x00, 0x19, 0x00, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0C, 0x00, 0x00, 0x00,
    0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x20, 0x00, 0x00, 0x00, 0x20, 0x02, 0x00, 0x00,     // owner S-1-5-32-544
    0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x20, 0x00, 0x00, 0x00, 0x20, 0x02, 0x00, 0x00,     // group S-1-5-32-544
};
static const size_t SK_DESC_LEN = sizeof SK_DESC;

static bool lay_key(proven_allocator_t a, lay_t *l, key_t *k, bool root, uint32_t *sk) {
    k->nk = place(l, cell_size(76 + name_bytes(k->name, k->nlen)));
    if (root) *sk = place(l, cell_size(20 + SK_DESC_LEN));
    if (k->nsub) {
        k->nlh = (k->nsub + LIST_MAX - 1) / LIST_MAX;
        k->lh = rp_mem_alloc(a, k->nlh, sizeof *k->lh);
        if (k->lh == NULL) return false;
        if (k->nlh > 1) k->list = place(l, cell_size(4 + 4 * k->nlh));
        for (size_t i = 0; i < k->nlh; ++i) {
            size_t n = k->nsub - i * LIST_MAX < LIST_MAX ? k->nsub - i * LIST_MAX : LIST_MAX;
            k->lh[i] = place(l, cell_size(4 + 8 * n));
        }
        if (k->nlh == 1) k->list = k->lh[0];
    }
    if (k->nval) {
        k->vlist = place(l, cell_size(4 * k->nval));
        for (size_t i = 0; i < k->nval; ++i) {
            val_t *v = &k->vals[i];
            v->vk = place(l, cell_size(20 + name_bytes(v->name, v->nlen)));
            if (v->len > BIG) {
                size_t nseg = (v->len + BIG - 1) / BIG;
                v->segs = rp_mem_alloc(a, nseg, sizeof *v->segs);
                if (v->segs == NULL) return false;
                v->db = place(l, cell_size(8));
                v->seglist = place(l, cell_size(4 * nseg));
                // Every segment cell has room for 16344 bytes, the last one too (as offreg writes them;
                // Windows does not read a shorter last segment).
                for (size_t s = 0; s < nseg; ++s) v->segs[s] = place(l, cell_size(BIG));
            } else if (v->len > 4) {
                v->dat = place(l, cell_size(v->len));
            }
        }
    }
    for (size_t i = 0; i < k->nsub; ++i) {
        if (!lay_key(a, l, k->subs[i], false, sk)) return false;
    }
    return l->ok && l->bins.err == PROVEN_OK;
}

// ---- writing -----------------------------------------------------------------------------------------

static void w16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void w32(uint8_t *p, uint32_t v) {
    w16(p, (uint16_t)v);
    w16(p + 2, (uint16_t)(v >> 16));
}



typedef struct {
    uint8_t *p;             // the bins (file offset 4096 onward)
    uint32_t sk;
} out_t;

// A cell header (negative: in use) at `off`; returns where its payload starts.
static uint8_t *cell(out_t *o, uint32_t off, uint32_t size) {
    w32(o->p + off, (uint32_t)-(int32_t)size);
    return o->p + off + 4;
}

static void put_name(uint8_t *p, const uint16_t *s, size_t n) {
    bool comp = compressible(s, n);
    for (size_t i = 0; i < n; ++i) {
        if (comp) p[i] = (uint8_t)s[i];
        else w16(p + 2 * i, s[i]);
    }
}

static uint32_t lh_hash(const uint16_t *s, size_t n) {
    uint32_t h = 0;
    for (size_t i = 0; i < n; ++i) h = h * 37 + rp_upcase16(s[i]);
    return h;
}

static void write_key(out_t *o, const key_t *k, bool root) {
    size_t nb = name_bytes(k->name, k->nlen);
    uint8_t *p = cell(o, k->nk, cell_size(76 + nb));
    memcpy(p, "nk", 2);
    w16(p + 2, compressible(k->name, k->nlen) ? NK_COMP : 0);
    w32(p + 16, root ? NO_CELL : k->parent->nk);
    w32(p + 20, (uint32_t)k->nsub);
    w32(p + 28, k->nsub ? k->list : NO_CELL);
    w32(p + 32, NO_CELL);                              // volatile subkeys
    w32(p + 36, (uint32_t)k->nval);
    w32(p + 40, k->nval ? k->vlist : NO_CELL);
    w32(p + 44, o->sk);
    w32(p + 48, NO_CELL);                              // class
    size_t maxsub = 0, maxvname = 0, maxvdata = 0;
    for (size_t i = 0; i < k->nsub; ++i) maxsub = k->subs[i]->nlen * 2 > maxsub ? k->subs[i]->nlen * 2 : maxsub;
    for (size_t i = 0; i < k->nval; ++i) {
        maxvname = k->vals[i].nlen * 2 > maxvname ? k->vals[i].nlen * 2 : maxvname;
        maxvdata = k->vals[i].len > maxvdata ? k->vals[i].len : maxvdata;
    }
    w32(p + 52, (uint32_t)maxsub);
    w32(p + 60, (uint32_t)maxvname);
    w32(p + 64, (uint32_t)maxvdata);
    w16(p + 72, (uint16_t)nb);
    put_name(p + 76, k->name, k->nlen);
    if (root) {
        uint8_t *s = cell(o, o->sk, cell_size(20 + SK_DESC_LEN));
        memcpy(s, "sk", 2);
        w32(s + 4, o->sk);
        w32(s + 8, o->sk);
        w32(s + 16, (uint32_t)SK_DESC_LEN);
        memcpy(s + 20, SK_DESC, SK_DESC_LEN);
    }
    if (k->nsub) {
        if (k->nlh > 1) {
            uint8_t *r = cell(o, k->list, cell_size(4 + 4 * k->nlh));
            memcpy(r, "ri", 2);
            w16(r + 2, (uint16_t)k->nlh);
            for (size_t i = 0; i < k->nlh; ++i) w32(r + 4 + 4 * i, k->lh[i]);
        }
        for (size_t i = 0; i < k->nlh; ++i) {
            size_t first = i * LIST_MAX, n = k->nsub - first < LIST_MAX ? k->nsub - first : LIST_MAX;
            uint8_t *l = cell(o, k->lh[i], cell_size(4 + 8 * n));
            memcpy(l, "lh", 2);
            w16(l + 2, (uint16_t)n);
            for (size_t j = 0; j < n; ++j) {
                const key_t *s = k->subs[first + j];
                w32(l + 4 + 8 * j, s->nk);
                w32(l + 8 + 8 * j, lh_hash(s->name, s->nlen));
            }
        }
    }
    if (k->nval) {
        uint8_t *vl = cell(o, k->vlist, cell_size(4 * k->nval));
        for (size_t i = 0; i < k->nval; ++i) {
            const val_t *v = &k->vals[i];
            w32(vl + 4 * i, v->vk);
            size_t vb = name_bytes(v->name, v->nlen);
            uint8_t *q = cell(o, v->vk, cell_size(20 + vb));
            memcpy(q, "vk", 2);
            w16(q + 2, (uint16_t)vb);
            w32(q + 12, v->type);
            w16(q + 16, v->nlen && compressible(v->name, v->nlen) ? VK_COMP : 0);
            put_name(q + 20, v->name, v->nlen);
            if (v->len <= 4) {
                w32(q + 4, (uint32_t)v->len | INLINE);
                memcpy(q + 8, v->data, v->len);
            } else if (v->len <= BIG) {
                w32(q + 4, (uint32_t)v->len);
                w32(q + 8, v->dat);
                memcpy(cell(o, v->dat, cell_size(v->len)), v->data, v->len);
            } else {
                size_t nseg = (v->len + BIG - 1) / BIG;
                w32(q + 4, (uint32_t)v->len);
                w32(q + 8, v->db);
                uint8_t *db = cell(o, v->db, cell_size(8));
                memcpy(db, "db", 2);
                w16(db + 2, (uint16_t)nseg);
                w32(db + 4, v->seglist);
                uint8_t *sl = cell(o, v->seglist, cell_size(4 * nseg));
                for (size_t s = 0; s < nseg; ++s) {
                    size_t take = v->len - s * BIG < BIG ? v->len - s * BIG : BIG;
                    w32(sl + 4 * s, v->segs[s]);
                    memcpy(cell(o, v->segs[s], cell_size(BIG)), v->data + s * BIG, take);
                }
            }
        }
    }
    for (size_t i = 0; i < k->nsub; ++i) write_key(o, k->subs[i], false);
}

proven_err_t rp_regf_write(rp_regf_t *h, uint8_t **out, size_t *len) {
    lay_t l = { .bins = rp_buf_new(h->alloc, 1u << 24), .ok = true };
    uint32_t sk = 0;
    if (!lay_key(h->alloc, &l, &h->root, true, &sk)) {
        rp_buf_free(&l.bins);
        return l.ok ? PROVEN_ERR_NOMEM : PROVEN_ERR_OUT_OF_BOUNDS;
    }
    size_t total = BLOCK + (size_t)l.bin_end;
    uint8_t *f = rp_mem_alloc(h->alloc, total, 1);
    if (f == NULL) {
        rp_buf_free(&l.bins);
        return PROVEN_ERR_NOMEM;
    }
    memset(f, 0, total);
    out_t o = { f + BLOCK, sk };
    write_key(&o, &h->root, true);
    w32(f + BLOCK + sk + 4 + 12, (uint32_t)h->keys);    // the security descriptor's reference count
    // Bin headers.
    size_t nbins = l.bins.len / 8;
    for (size_t i = 0; i < nbins; ++i) {
        uint32_t start, size;
        memcpy(&start, l.bins.data + 8 * i, 4);
        memcpy(&size, l.bins.data + 8 * i + 4, 4);
        uint8_t *b = o.p + start;
        memcpy(b, "hbin", 4);
        w32(b + 4, start);
        w32(b + 8, size);
    }
    // Free space: scan every bin for the first byte not covered by a cell.
    for (size_t i = 0; i < nbins; ++i) {
        uint32_t start, size;
        memcpy(&start, l.bins.data + 8 * i, 4);
        memcpy(&size, l.bins.data + 8 * i + 4, 4);
        uint32_t at = start + BIN_HEADER;
        while (at < start + size) {
            int32_t c = (int32_t)((uint32_t)o.p[at] | (uint32_t)o.p[at + 1] << 8 | (uint32_t)o.p[at + 2] << 16 | (uint32_t)o.p[at + 3] << 24);
            if (c >= 0) break;
            at += (uint32_t)-c;
        }
        if (at < start + size) w32(o.p + at, start + size - at);
    }
    // Base block (as offreg writes it, times zero).
    memcpy(f, "regf", 4);
    w32(f + 4, 1);
    w32(f + 8, 1);
    w32(f + 20, 1);                                     // format version 1.5
    w32(f + 24, 5);
    w32(f + 32, 1);
    w32(f + 36, h->root.nk);
    w32(f + 40, (uint32_t)l.bin_end);
    w32(f + 44, 1);
    memcpy(f + 0xB0, "OfRg", 4);                       // offreg's mark, as in Windows' MSIX hives
    w32(f + 0xB4, 1);
    uint32_t sum = 0;
    for (size_t i = 0; i < 508; i += 4) sum ^= (uint32_t)f[i] | (uint32_t)f[i + 1] << 8 | (uint32_t)f[i + 2] << 16 | (uint32_t)f[i + 3] << 24;
    if (sum == 0) sum = 1;
    else if (sum == 0xFFFFFFFF) sum = 0xFFFFFFFE;
    w32(f + 508, sum);
    rp_buf_free(&l.bins);
    *out = f;
    *len = total;
    return PROVEN_OK;
}

// ---- reading -----------------------------------------------------------------------------------------

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

typedef struct {
    proven_allocator_t       alloc;
    const uint8_t           *bins;
    size_t                   blen;
    const rp_regf_visitor_t *v;
    uint64_t                 count, max;
    const char             **why;
    rp_buf_t                 path;
} rd_t;

#define RFAIL(msg) do { *r->why = (msg); return PROVEN_ERR_INVALID_FORMAT; } while (0)

// The payload of the cell at `off` (in use), at least `need` bytes; NULL when it is not one.
static const uint8_t *get(rd_t *r, uint32_t off, size_t need, size_t *avail) {
    if (off & 7 || (size_t)off + 4 > r->blen) return NULL;
    int32_t size = (int32_t)r32(r->bins + off);
    if (size >= 0 || size == INT32_MIN || (size_t)-size < 4 + need || (size_t)off + (size_t)-size > r->blen) return NULL;
    if (avail) *avail = (size_t)-size - 4;
    return r->bins + off + 4;
}

static bool append_name(rd_t *r, const uint8_t *p, size_t nb, bool comp) {
    if (comp) {
        for (size_t i = 0; i < nb; ++i) {
            uint8_t c = p[i];
            if (c < 0x80) rp_buf_byte(&r->path, c);
            else {
                rp_buf_byte(&r->path, (uint8_t)(0xC0 | c >> 6));
                rp_buf_byte(&r->path, (uint8_t)(0x80 | (c & 63)));
            }
        }
        return r->path.err == PROVEN_OK;
    }
    if (nb & 1) return false;
    static uint16_t tmp[16384];                         // (not on the stack: keys nest 512 deep)
    size_t n = nb / 2;
    if (n > 16384) return false;
    for (size_t i = 0; i < n; ++i) tmp[i] = r16(p + 2 * i);
    rp_text_result_t t = rp_utf16_to_utf8(tmp, n, NULL, 0);
    if (t.err != PROVEN_OK) return false;
    size_t at = r->path.len;
    rp_buf_zero(&r->path, t.units);
    if (r->path.err != PROVEN_OK) return false;
    return rp_utf16_to_utf8(tmp, n, r->path.data + at, t.units).err == PROVEN_OK;
}

static proven_err_t read_key(rd_t *r, uint32_t off, int depth) {
    if (depth > 512) RFAIL("the hive's keys nest deeper than 512");
    if (++r->count > r->max) {
        *r->why = "more keys and values than allowed";
        return PROVEN_ERR_OUT_OF_BOUNDS;
    }
    size_t av;
    const uint8_t *p = get(r, off, 76, &av);
    if (p == NULL || memcmp(p, "nk", 2) != 0) RFAIL("a key cell is not a key");
    size_t nb = r16(p + 72);
    if (76 + nb > av || (nb == 0 && depth > 0)) RFAIL("a key's name does not fit its cell");
    size_t base = r->path.len;
    if (depth > 0) {
        if (base) rp_buf_byte(&r->path, '\\');
        if (!append_name(r, p + 76, nb, r16(p + 2) & NK_COMP)) RFAIL("a key name is not valid text");
    }
    size_t here = r->path.len;                          // this key's path, without the NUL
    rp_buf_byte(&r->path, 0);
    if (r->path.err != PROVEN_OK) return r->path.err;
    const char *path = (const char *)r->path.data;
    if (r->v && r->v->key) r->v->key(r->v->ctx, depth ? path : "");
    uint32_t nval = r32(p + 36), vlist = r32(p + 40), nsub = r32(p + 20), list = r32(p + 28);
    if (nval) {
        size_t lav;
        const uint8_t *vl = get(r, vlist, (size_t)nval * 4, &lav);
        if (vl == NULL || nval > lav / 4) RFAIL("a value list does not fit its cell");
        for (uint32_t i = 0; i < nval; ++i) {
            if (++r->count > r->max) {
                *r->why = "more keys and values than allowed";
                return PROVEN_ERR_OUT_OF_BOUNDS;
            }
            size_t vav;
            const uint8_t *q = get(r, r32(vl + 4 * i), 20, &vav);
            if (q == NULL || memcmp(q, "vk", 2) != 0) RFAIL("a value cell is not a value");
            size_t vnb = r16(q + 2);
            if (20 + vnb > vav) RFAIL("a value's name does not fit its cell");
            size_t save = r->path.len;
            if (!append_name(r, q + 20, vnb, r16(q + 16) & VK_COMP)) RFAIL("a value name is not valid text");
            rp_buf_byte(&r->path, 0);
            if (r->path.err != PROVEN_OK) return r->path.err;
            const char *name = (const char *)r->path.data + save;
            uint32_t dsize = r32(q + 4), doff = r32(q + 8), type = r32(q + 12);
            uint8_t inl[4];
            const uint8_t *data = NULL;
            uint8_t *big = NULL;
            size_t dl = dsize & ~INLINE;
            if (dsize & INLINE) {
                if (dl > 4) RFAIL("an inline value longer than 4 bytes");
                w32(inl, doff);
                data = inl;
            } else if (dl > MAX_DATA) {
                RFAIL("a value longer than 16 MiB");
            } else if (dl) {
                size_t dav;
                const uint8_t *d = get(r, doff, 8, &dav);
                if (d && dl > BIG && memcmp(d, "db", 2) == 0) {
                    size_t nseg = r16(d + 2), sav;
                    const uint8_t *sl = get(r, r32(d + 4), nseg * 4, &sav);
                    if (sl == NULL || nseg != (dl + BIG - 1) / BIG) RFAIL("a big value's segment list is damaged");
                    big = rp_mem_alloc(r->alloc, dl, 1);
                    if (big == NULL) return PROVEN_ERR_NOMEM;
                    for (size_t s = 0; s < nseg; ++s) {
                        size_t take = dl - s * BIG < BIG ? dl - s * BIG : BIG, gav;
                        const uint8_t *g = get(r, r32(sl + 4 * s), take, &gav);
                        if (g == NULL) {
                            rp_mem_free(r->alloc, big);
                            RFAIL("a big value's segment is damaged");
                        }
                        memcpy(big + s * BIG, g, take);
                    }
                    data = big;
                } else {
                    d = get(r, doff, dl, &dav);
                    if (d == NULL) RFAIL("a value's data does not fit its cell");
                    data = d;
                }
            }
            if (r->v && r->v->value) r->v->value(r->v->ctx, path, name, type, data, dl);
            rp_mem_free(r->alloc, big);
            r->path.len = save;
            path = (const char *)r->path.data;
        }
    }
    if (nsub) {
        size_t lav;
        const uint8_t *l = get(r, list, 4, &lav);
        if (l == NULL) RFAIL("a subkey list is damaged");
        // An ri index of lh/lf/li lists, or one such list.
        const uint8_t *ri = NULL;
        uint32_t nl = 1;
        if (memcmp(l, "ri", 2) == 0) {
            ri = l;
            nl = r16(l + 2);
            if (4 + 4 * (size_t)nl > lav) RFAIL("a subkey index is damaged");
        }
        uint64_t seen = 0;
        for (uint32_t i = 0; i < nl; ++i) {
            const uint8_t *s = ri ? get(r, r32(ri + 4 + 4 * i), 4, &lav) : get(r, list, 4, &lav);
            if (s == NULL) RFAIL("a subkey list is damaged");
            bool lhf = memcmp(s, "lh", 2) == 0 || memcmp(s, "lf", 2) == 0, li = memcmp(s, "li", 2) == 0;
            size_t n = r16(s + 2), step = lhf ? 8 : 4;
            if ((!lhf && !li) || 4 + step * n > lav) RFAIL("a subkey list is damaged");
            for (size_t j = 0; j < n; ++j) {
                r->path.len = here;
                proven_err_t err = read_key(r, r32(s + 4 + step * j), depth + 1);
                if (err != PROVEN_OK) return err;
                ++seen;
            }
        }
        if (seen != nsub) RFAIL("a key's subkey count does not match its lists");
    }
    r->path.len = base;
    return PROVEN_OK;
}

proven_err_t rp_regf_read(proven_allocator_t alloc, const uint8_t *hive, size_t len, const rp_limits_t *lim, const rp_regf_visitor_t *v,
                          const char **why) {
    if (len < BLOCK + BLOCK || memcmp(hive, "regf", 4) != 0) {
        *why = "not a registry hive";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    uint32_t sum = 0;
    for (size_t i = 0; i < 508; i += 4) sum ^= r32(hive + i);
    if (sum == 0) sum = 1;
    else if (sum == 0xFFFFFFFF) sum = 0xFFFFFFFE;
    if (sum != r32(hive + 508)) {
        *why = "the hive's base block checksum does not match";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    if (r32(hive + 4) != r32(hive + 8)) {
        *why = "the hive was not closed cleanly (its log would be needed)";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    if (r32(hive + 20) != 1 || r32(hive + 24) < 3 || r32(hive + 24) > 6 || r32(hive + 28) != 0 || r32(hive + 32) != 1) {
        *why = "a hive version or type rubrapack does not read";
        return PROVEN_ERR_UNSUPPORTED;
    }
    size_t blen = r32(hive + 40);
    if (blen > len - BLOCK || blen % BLOCK) {
        *why = "the hive's bins lie outside the file";
        return PROVEN_ERR_INVALID_FORMAT;
    }
    // Every bin must be where its header says, and no cell may cross into the next.
    for (size_t at = 0; at < blen;) {
        const uint8_t *b = hive + BLOCK + at;
        uint32_t size = r32(b + 8);
        if (memcmp(b, "hbin", 4) != 0 || r32(b + 4) != at || size < BLOCK || size % BLOCK || size > blen - at) {
            *why = "a damaged hive bin";
            return PROVEN_ERR_INVALID_FORMAT;
        }
        for (size_t c = at + BIN_HEADER; c < at + size;) {
            int32_t cs = (int32_t)r32(hive + BLOCK + c);
            size_t n = cs < 0 ? (size_t)-(int64_t)cs : (size_t)cs;
            if (n < 8 || n & 7 || n > at + size - c) {
                *why = "a hive cell crosses its bin";
                return PROVEN_ERR_INVALID_FORMAT;
            }
            c += n;
        }
        at += size;
    }
    rd_t r = { alloc, hive + BLOCK, blen, v, 0, lim->max_entries, why, rp_buf_new(alloc, 1u << 20) };
    proven_err_t err = read_key(&r, r32(hive + 36), 0);
    rp_buf_free(&r.path);
    return err;
}
