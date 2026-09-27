// src/text/nfc.c - Normalization Form C (include/rubrapack/text.h; UAX #15, RFC-0006 L3).
//
// Decompose every character canonically (Hangul syllables arithmetically, the rest from the UCD
// tables in nfc_tables.c, recursively), put combining marks in canonical order (a stable sort by
// combining class inside each run of non-starters), then compose: a character joins the last
// starter when a primary composite exists and nothing between them blocks it.

#include "nfc_tables.h"
#include "rubrapack/mem.h"
#include "rubrapack/text.h"

#include <string.h>

enum {
    S_BASE = 0xAC00, L_BASE = 0x1100, V_BASE = 0x1161, T_BASE = 0x11A7,
    L_COUNT = 19, V_COUNT = 21, T_COUNT = 28, N_COUNT = V_COUNT * T_COUNT, S_COUNT = L_COUNT * N_COUNT,
};

static uint8_t ccc(uint32_t cp) {
    size_t lo = 0, hi = rp_nfc_ccc_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < rp_nfc_ccc[mid].first) hi = mid;
        else if (cp > rp_nfc_ccc[mid].last) lo = mid + 1;
        else return rp_nfc_ccc[mid].ccc;
    }
    return 0;
}

static const rp_nfc_decomp_t *decomp(uint32_t cp) {
    size_t lo = 0, hi = rp_nfc_decomp_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < rp_nfc_decomp[mid].cp) hi = mid;
        else if (cp > rp_nfc_decomp[mid].cp) lo = mid + 1;
        else return &rp_nfc_decomp[mid];
    }
    return NULL;
}

// The primary composite of a + b, or 0.
static uint32_t compose(uint32_t a, uint32_t b) {
    if (a >= L_BASE && a < L_BASE + L_COUNT && b >= V_BASE && b < V_BASE + V_COUNT) {
        return S_BASE + ((a - L_BASE) * V_COUNT + (b - V_BASE)) * T_COUNT;
    }
    if (a >= S_BASE && a < S_BASE + S_COUNT && (a - S_BASE) % T_COUNT == 0 && b > T_BASE && b < T_BASE + T_COUNT) {
        return a + (b - T_BASE);
    }
    size_t lo = 0, hi = rp_nfc_comp_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const rp_nfc_comp_t *c = &rp_nfc_comp[mid];
        if (a < c->a || (a == c->a && b < c->b)) hi = mid;
        else if (a > c->a || b > c->b) lo = mid + 1;
        else return c->c;
    }
    return 0;
}

typedef struct {
    uint32_t          *v;
    size_t             n, cap;
    proven_allocator_t alloc;
    bool               nomem;
} cps_t;

static void push(cps_t *c, uint32_t cp) {
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 64;
        uint32_t *v = rp_mem_alloc(c->alloc, cap, sizeof *v);
        if (v == NULL) {
            c->nomem = true;
            return;
        }
        if (c->n) memcpy(v, c->v, c->n * sizeof *v);
        rp_mem_free(c->alloc, c->v);
        c->v = v;
        c->cap = cap;
    }
    c->v[c->n++] = cp;
}

static void decompose(cps_t *c, uint32_t cp, int depth) {
    if (cp >= S_BASE && cp < S_BASE + S_COUNT) {
        uint32_t s = cp - S_BASE;
        push(c, L_BASE + s / N_COUNT);
        push(c, V_BASE + (s % N_COUNT) / T_COUNT);
        if (s % T_COUNT) push(c, T_BASE + s % T_COUNT);
        return;
    }
    const rp_nfc_decomp_t *d = depth < 8 ? decomp(cp) : NULL;
    if (d == NULL) {
        push(c, cp);
        return;
    }
    decompose(c, d->a, depth + 1);
    if (d->b) decompose(c, d->b, depth + 1);
}

// Next code point of valid UTF-8.
static uint32_t next_cp(const uint8_t *s, size_t *i) {
    uint8_t b = s[*i];
    if (b < 0x80) {
        *i += 1;
        return b;
    }
    if (b < 0xE0) {
        uint32_t cp = (uint32_t)(b & 0x1F) << 6 | (s[*i + 1] & 0x3Fu);
        *i += 2;
        return cp;
    }
    if (b < 0xF0) {
        uint32_t cp = (uint32_t)(b & 0x0F) << 12 | (uint32_t)(s[*i + 1] & 0x3F) << 6 | (s[*i + 2] & 0x3Fu);
        *i += 3;
        return cp;
    }
    uint32_t cp = (uint32_t)(b & 0x07) << 18 | (uint32_t)(s[*i + 1] & 0x3F) << 12 | (uint32_t)(s[*i + 2] & 0x3F) << 6 |
                  (s[*i + 3] & 0x3Fu);
    *i += 4;
    return cp;
}

static size_t put_utf8(uint8_t *o, uint32_t cp) {
    if (cp < 0x80) {
        o[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (uint8_t)(0xC0 | cp >> 6);
        o[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (uint8_t)(0xE0 | cp >> 12);
        o[1] = (uint8_t)(0x80 | (cp >> 6 & 0x3F));
        o[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (uint8_t)(0xF0 | cp >> 18);
    o[1] = (uint8_t)(0x80 | (cp >> 12 & 0x3F));
    o[2] = (uint8_t)(0x80 | (cp >> 6 & 0x3F));
    o[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
}

proven_err_t rp_nfc(proven_allocator_t alloc, const uint8_t *src, size_t len, uint8_t **out, size_t *out_len) {
    if ((src == NULL && len) || out == NULL || out_len == NULL) return PROVEN_ERR_INVALID_ARG;
    if (rp_utf8_validate(src, len).err != PROVEN_OK) return PROVEN_ERR_INVALID_ENCODING;
    cps_t c = { .alloc = alloc };
    for (size_t i = 0; i < len && !c.nomem;) decompose(&c, next_cp(src, &i), 0);
    // Canonical order: a stable insertion sort by class inside each run of non-starters.
    for (size_t i = 1; i < c.n && !c.nomem; ++i) {
        uint8_t k = ccc(c.v[i]);
        if (k == 0) continue;
        for (size_t j = i; j > 0; --j) {
            uint8_t p = ccc(c.v[j - 1]);
            if (p == 0 || p <= k) break;
            uint32_t t = c.v[j];
            c.v[j] = c.v[j - 1];
            c.v[j - 1] = t;
        }
    }
    // Composition (the UAX #15 reference algorithm).
    size_t n = 0;
    if (c.n && !c.nomem) {
        size_t starter = 0;
        int last = ccc(c.v[0]) ? 256 : 0;
        n = 1;
        for (size_t i = 1; i < c.n; ++i) {
            uint32_t ch = c.v[i];
            int k = ccc(ch);
            uint32_t composite = compose(c.v[starter], ch);
            if (composite && (last < k || last == 0)) {
                c.v[starter] = composite;
                continue;
            }
            if (k == 0) starter = n;
            last = k;
            c.v[n++] = ch;
        }
    }
    uint8_t *o = c.nomem ? NULL : rp_mem_alloc(alloc, n * 4 + 1, 1);
    if (o == NULL) {
        rp_mem_free(alloc, c.v);
        return PROVEN_ERR_NOMEM;
    }
    size_t w = 0;
    for (size_t i = 0; i < n; ++i) w += put_utf8(o + w, c.v[i]);
    o[w] = 0;
    rp_mem_free(alloc, c.v);
    *out = o;
    *out_len = w;
    return PROVEN_OK;
}

bool rp_is_nfc(proven_allocator_t alloc, const uint8_t *src, size_t len) {
    bool ascii = true;
    for (size_t i = 0; i < len && ascii; ++i) ascii = src[i] < 0x80;
    if (ascii) return true;
    uint8_t *o;
    size_t n;
    if (rp_nfc(alloc, src, len, &o, &n) != PROVEN_OK) return false;
    bool same = n == len && memcmp(o, src, len) == 0;
    rp_mem_free(alloc, o);
    return same;
}
