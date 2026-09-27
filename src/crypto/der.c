// src/crypto/der.c - DER reader and writer, BER normalizer (include/rubrapack/der.h).

#include "rubrapack/der.h"
#include "rubrapack/mem.h"

#include <string.h>

// The one place a length is checked against the buffer.
bool rp_der_read(rp_der_span_t *s, rp_der_t *e) {
    const uint8_t *p = s->p;
    size_t n = s->n;
    if (n < 2) return false;
    uint8_t tag = p[0];
    if ((tag & 0x1F) == 0x1F) return false;         // multi-byte tag
    size_t len, head;
    uint8_t l0 = p[1];
    if (l0 < 0x80) {
        len = l0;
        head = 2;
    } else {
        size_t k = l0 & 0x7F;
        if (k == 0 || k > 4 || n < 2 + k || p[2] == 0) return false;     // indefinite, too long, or not minimal
        len = 0;
        for (size_t i = 0; i < k; ++i) len = (len << 8) | p[2 + i];
        if (len < 0x80 || (k > 1 && len < ((size_t)1 << (8 * (k - 1))))) return false;   // a shorter form existed
        head = 2 + k;
    }
    if (len > n - head) return false;
    e->tag = tag;
    e->val = (rp_der_span_t){ p + head, len };
    e->whole = (rp_der_span_t){ p, head + len };
    s->p = p + head + len;
    s->n = n - head - len;
    return true;
}

bool rp_der_get(rp_der_span_t *s, uint8_t tag, rp_der_t *e) {
    rp_der_span_t save = *s;
    if (!rp_der_read(s, e)) return false;
    if (e->tag != tag) {
        *s = save;
        return false;
    }
    return true;
}

bool rp_der_peek(const rp_der_span_t *s, uint8_t *tag) {
    if (s->n == 0) return false;
    *tag = s->p[0];
    return true;
}

bool rp_der_oid_is(const rp_der_t *e, const uint8_t *oid, size_t len) {
    return e->tag == RP_DER_OID && e->val.n == len && memcmp(e->val.p, oid, len) == 0;
}

bool rp_der_uint(const rp_der_t *e, rp_der_span_t *out) {
    if (e->tag != RP_DER_INTEGER || e->val.n == 0) return false;
    const uint8_t *p = e->val.p;
    size_t n = e->val.n;
    if (p[0] & 0x80) return false;                  // negative
    if (n > 1 && p[0] == 0 && !(p[1] & 0x80)) return false;     // not minimal
    if (n > 1 && p[0] == 0) {
        ++p;
        --n;
    }
    *out = (rp_der_span_t){ p, n };
    return true;
}

bool rp_der_small(const rp_der_t *e, uint64_t *v) {
    rp_der_span_t m;
    if (!rp_der_uint(e, &m) || m.n > 8) return false;
    *v = 0;
    for (size_t i = 0; i < m.n; ++i) *v = (*v << 8) | m.p[i];
    return true;
}

// ---- BER to DER ----------------------------------------------------------------------------

typedef struct {
    proven_allocator_t alloc;
    uint32_t           max_depth;
    size_t             limit;
} ber_t;

// Reads one BER element at *p (n bytes available) into `out` as DER; *used gets its size.
static bool ber_elem(const ber_t *c, const uint8_t *p, size_t n, rp_buf_t *out, size_t *used, uint32_t depth);

// The children of a constructed element: until an end-of-contents (indefinite) or `len` bytes.
static bool ber_children(const ber_t *c, const uint8_t *p, size_t n, bool indefinite, size_t len, rp_buf_t *out,
                         size_t *used, uint32_t depth, bool join_strings) {
    size_t off = 0;
    size_t end = indefinite ? n : len;
    while (true) {
        if (indefinite) {
            if (off + 2 <= n && p[off] == 0 && p[off + 1] == 0) {
                *used = off + 2;
                return true;
            }
            if (off >= n) return false;
        } else if (off == end) {
            *used = off;
            return true;
        }
        size_t u;
        if (join_strings) {
            // A piece of a constructed OCTET STRING: its DER form, then only the contents kept.
            rp_buf_t piece = rp_buf_new(c->alloc, c->limit);
            if (!ber_elem(c, p + off, end - off, &piece, &u, depth + 1) || piece.err || piece.len < 2 || piece.data[0] != RP_DER_OCTET_STRING) {
                rp_buf_free(&piece);
                return false;
            }
            rp_der_span_t s = { piece.data, piece.len };
            rp_der_t e;
            bool ok = rp_der_read(&s, &e);
            if (ok) rp_buf_put(out, e.val.p, e.val.n);
            rp_buf_free(&piece);
            if (!ok) return false;
        } else if (!ber_elem(c, p + off, end - off, out, &u, depth + 1)) {
            return false;
        }
        off += u;
    }
}

static void put_len(rp_buf_t *b, size_t n) {
    if (n < 0x80) {
        rp_buf_byte(b, (uint8_t)n);
        return;
    }
    uint8_t tmp[8];
    int k = 0;
    for (size_t v = n; v; v >>= 8) tmp[k++] = (uint8_t)v;
    rp_buf_byte(b, (uint8_t)(0x80 | k));
    while (k) rp_buf_byte(b, tmp[--k]);
}

static bool ber_elem(const ber_t *c, const uint8_t *p, size_t n, rp_buf_t *out, size_t *used, uint32_t depth) {
    if (depth > c->max_depth || n < 2) return false;
    uint8_t tag = p[0];
    if ((tag & 0x1F) == 0x1F) return false;
    bool constructed = tag & 0x20;
    size_t head, len = 0;
    bool indefinite = false;
    if (p[1] < 0x80) {
        len = p[1];
        head = 2;
    } else if (p[1] == 0x80) {
        if (!constructed) return false;
        indefinite = true;
        head = 2;
    } else {
        size_t k = p[1] & 0x7F;
        if (k > 4 || n < 2 + k) return false;
        for (size_t i = 0; i < k; ++i) len = (len << 8) | p[2 + i];
        head = 2 + k;
    }
    if (!indefinite && len > n - head) return false;
    if (!constructed) {
        rp_buf_byte(out, tag);
        put_len(out, len);
        rp_buf_put(out, p + head, len);
        *used = head + len;
        return out->err == PROVEN_OK;
    }
    bool join = tag == 0x24;            // constructed OCTET STRING -> primitive
    rp_buf_t kids = rp_buf_new(c->alloc, c->limit);
    size_t u;
    bool ok = ber_children(c, p + head, n - head, indefinite, len, &kids, &u, depth, join);
    if (ok && kids.err == PROVEN_OK) {
        rp_buf_byte(out, join ? RP_DER_OCTET_STRING : tag);
        put_len(out, kids.len);
        rp_buf_put(out, kids.data, kids.len);
        *used = head + u;
    }
    rp_buf_free(&kids);
    return ok && out->err == PROVEN_OK;
}

proven_err_t rp_ber_to_der(proven_allocator_t alloc, const uint8_t *in, size_t len, uint32_t max_depth, size_t limit,
                           uint8_t **out, size_t *out_len) {
    ber_t c = { alloc, max_depth, limit };
    rp_buf_t b = rp_buf_new(alloc, limit);
    size_t used;
    if (!ber_elem(&c, in, len, &b, &used, 0) || used != len) {
        rp_buf_free(&b);
        return PROVEN_ERR_INVALID_FORMAT;
    }
    return rp_buf_take(&b, out, out_len);
}

// ---- writing -----------------------------------------------------------------------------

void rp_der_put(rp_buf_t *b, uint8_t tag, const void *v, size_t n) {
    rp_buf_byte(b, tag);
    put_len(b, n);
    if (n) rp_buf_put(b, v, n);
}

void rp_der_put_uint(rp_buf_t *b, const uint8_t *be, size_t n) {
    while (n > 1 && be[0] == 0) {
        ++be;
        --n;
    }
    bool pad = n == 0 || (be[0] & 0x80);
    rp_buf_byte(b, RP_DER_INTEGER);
    put_len(b, n + (pad ? 1 : 0));
    if (pad) rp_buf_byte(b, 0);
    if (n) rp_buf_put(b, be, n);
}

void rp_der_put_small(rp_buf_t *b, uint64_t v) {
    uint8_t tmp[8];
    int k = 0;
    for (uint64_t x = v; x; x >>= 8) tmp[7 - k++] = (uint8_t)x;
    if (k == 0) {
        rp_der_put(b, RP_DER_INTEGER, "\0", 1);
        return;
    }
    rp_der_put_uint(b, tmp + 8 - k, (size_t)k);
}

void rp_der_put_oid(rp_buf_t *b, const uint8_t *oid, size_t n) { rp_der_put(b, RP_DER_OID, oid, n); }

void rp_der_put_null(rp_buf_t *b) { rp_der_put(b, RP_DER_NULL, NULL, 0); }

void rp_der_wrap(rp_buf_t *b, uint8_t tag, rp_buf_t *child) {
    if (child->err != PROVEN_OK && b->err == PROVEN_OK) b->err = child->err;
    rp_der_put(b, tag, child->data, child->len);
    rp_buf_free(child);
}

// X.690 11.6: the encodings in ascending order, compared as octet strings where the shorter one
// is padded with zeros at the end.
static int cmp_set(const rp_der_span_t *a, const rp_der_span_t *b) {
    size_t n = a->n > b->n ? a->n : b->n;
    for (size_t i = 0; i < n; ++i) {
        uint8_t x = i < a->n ? a->p[i] : 0, y = i < b->n ? b->p[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

void rp_der_set_of(rp_buf_t *b, uint8_t tag, const rp_der_span_t *elems, size_t n) {
    size_t *order = rp_mem_alloc(b->alloc, n + 1, sizeof *order);
    if (order == NULL) {
        if (b->err == PROVEN_OK) b->err = PROVEN_ERR_NOMEM;
        return;
    }
    for (size_t i = 0; i < n; ++i) order[i] = i;
    for (size_t i = 1; i < n; ++i) {
        for (size_t j = i; j > 0 && cmp_set(&elems[order[j - 1]], &elems[order[j]]) > 0; --j) {
            size_t t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    }
    size_t total = 0;
    for (size_t i = 0; i < n; ++i) total += elems[i].n;
    rp_buf_byte(b, tag);
    put_len(b, total);
    for (size_t i = 0; i < n; ++i) rp_buf_put(b, elems[order[i]].p, elems[order[i]].n);
    rp_mem_free(b->alloc, order);
}
