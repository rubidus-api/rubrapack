// src/msi/index.c - rows by key for binary search (include/rubrapack/msi.h, rp_msi_index_t).

#include "rubrapack/mem.h"
#include "rubrapack/msi.h"

#include <string.h>

static int cmp_key(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
    size_t m = an < bn ? an : bn;
    int r = m ? memcmp(a, b, m) : 0;
    return r ? r : an < bn ? -1 : an > bn;
}

static int cmp_entry(const void *x, const void *y) {
    const rp_msi_key_t *a = x, *b = y;
    int r = cmp_key(a->key, a->len, b->key, b->len);
    return r ? r : a->at < b->at ? -1 : a->at > b->at;
}

proven_err_t rp_msi_index_new(proven_allocator_t alloc, size_t n, rp_msi_index_t *out) {
    *out = (rp_msi_index_t){ 0 };
    out->v = rp_mem_alloc(alloc, n ? n : 1, sizeof *out->v);
    if (out->v == NULL) return PROVEN_ERR_NOMEM;
    memset(out->v, 0, (n ? n : 1) * sizeof *out->v);
    out->n = n;
    return PROVEN_OK;
}

void rp_msi_index_sort(rp_msi_index_t *ix) { rp_sort(ix->v, ix->n, sizeof *ix->v, cmp_entry); }

proven_err_t rp_msi_index(proven_allocator_t alloc, const rp_msi_wtable_t *t, ptrdiff_t col, rp_msi_index_t *out) {
    size_t n = t ? t->row_count : 0;
    proven_err_t err = rp_msi_index_new(alloc, n, out);
    if (err != PROVEN_OK) return err;
    for (size_t r = 0; r < n; ++r) {
        const rp_msi_cell_t *c = rp_msi_wcell(t, r, col);
        out->v[r] = c->kind == RP_MSI_STR ? (rp_msi_key_t){ c->bytes, c->len, r } : (rp_msi_key_t){ NULL, 0, r };
    }
    rp_msi_index_sort(out);
    return PROVEN_OK;
}

size_t rp_msi_index_first(const rp_msi_index_t *ix, const void *key, size_t len) {
    size_t lo = 0, hi = ix->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cmp_key(ix->v[mid].key, ix->v[mid].len, key, len) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

bool rp_msi_index_is(const rp_msi_index_t *ix, size_t pos, const void *key, size_t len) {
    return pos < ix->n && cmp_key(ix->v[pos].key, ix->v[pos].len, key, len) == 0;
}

size_t rp_msi_index_find(const rp_msi_index_t *ix, const void *key, size_t len) {
    size_t pos = rp_msi_index_first(ix, key, len);
    return rp_msi_index_is(ix, pos, key, len) ? ix->v[pos].at : SIZE_MAX;
}

void rp_msi_index_free(proven_allocator_t alloc, rp_msi_index_t *ix) {
    rp_mem_free(alloc, ix->v);
    *ix = (rp_msi_index_t){ 0 };
}
