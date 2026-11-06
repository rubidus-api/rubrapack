// src/crypto/der.c - DER reader and writer, BER normalizer (include/rubrapack/der.h).

#include "rubrapack/der.h"
#include "rubrapack/mem.h"

#include <string.h>

#include "proven/der.h"

// The reader is proven's (proven/der.h): definite lengths in their shortest form, one-byte tags,
// nothing past the input. These keep the cursor shape the callers walk with.

static proven_mem_view_t pv(rp_der_span_t s) { return (proven_mem_view_t){ s.p, s.n }; }

bool rp_der_read(rp_der_span_t *s, rp_der_t *e) {
    proven_mem_view_t in = pv(*s);
    proven_der_item_t it;
    if (s->n == 0 || proven_der_read(&in, &it) != PROVEN_OK) return false;
    e->tag = it.tag;
    e->val = (rp_der_span_t){ it.content.ptr, it.content.size };
    e->whole = (rp_der_span_t){ it.whole.ptr, it.whole.size };
    *s = (rp_der_span_t){ in.ptr, in.size };
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
    return e->tag == RP_DER_OID && proven_der_oid_is(pv(e->val), (proven_mem_view_t){ oid, len });
}

bool rp_der_uint(const rp_der_t *e, rp_der_span_t *out) {
    proven_mem_view_t in = pv(e->whole), m;
    if (proven_der_read_uint(&in, &m) != PROVEN_OK) return false;
    *out = (rp_der_span_t){ m.ptr, m.size };
    return true;
}

bool rp_der_small(const rp_der_t *e, uint64_t *v) {
    proven_mem_view_t in = pv(e->whole);
    proven_u64 x;
    if (proven_der_read_u64(&in, &x) != PROVEN_OK) return false;
    *v = x;
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
    bool ok = ber_children(c, p + head, n - head, indefinite, len, &kids, &u, depth, join) && kids.err == PROVEN_OK;
    if (ok) {
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
//
// Each element is encoded by proven's writer (proven/der.h) straight into the end of `b`: the
// room it needs is made first, exactly, so a buffer's limit is reached only by what is written.

// Bytes of an element with `n` content bytes.
static size_t elem_size(size_t n) {
    size_t h = 2;
    for (size_t x = n; n >= 0x80 && x; x >>= 8) ++h;
    return h + n;
}

static bool open_tail(rp_buf_t *b, size_t room, proven_der_writer_t *w, size_t *at) {
    *at = b->len;
    rp_buf_zero(b, room);
    if (b->err != PROVEN_OK || b->len != *at + room) return false;
    proven_der_writer_init(w, (proven_mem_mut_t){ b->data + *at, room });
    return true;
}

static void close_tail(rp_buf_t *b, const proven_der_writer_t *w, size_t at) {
    size_t n = 0;
    if (proven_der_writer_finish(w, &n) != PROVEN_OK) {
        if (b->err == PROVEN_OK) b->err = PROVEN_ERR_INVALID_STATE;
        n = 0;
    }
    b->len = at + n;
}

void rp_der_put(rp_buf_t *b, uint8_t tag, const void *v, size_t n) {
    proven_der_writer_t w;
    size_t at;
    if (!open_tail(b, elem_size(n), &w, &at)) return;
    proven_der_put(&w, tag, (proven_mem_view_t){ n ? v : (const void *)"", n });
    close_tail(b, &w, at);
}

void rp_der_put_uint(rp_buf_t *b, const uint8_t *be, size_t n) {
    while (n > 1 && be[0] == 0) {
        ++be;
        --n;
    }
    proven_der_writer_t w;
    size_t at;
    if (!open_tail(b, elem_size(n + 1), &w, &at)) return;
    proven_der_put_uint(&w, (proven_mem_view_t){ n ? be : (const uint8_t *)"", n });
    close_tail(b, &w, at);
}

void rp_der_put_small(rp_buf_t *b, uint64_t v) {
    proven_der_writer_t w;
    size_t at;
    if (!open_tail(b, elem_size(9), &w, &at)) return;
    proven_der_put_u64(&w, v);
    close_tail(b, &w, at);
}

void rp_der_put_oid(rp_buf_t *b, const uint8_t *oid, size_t n) { rp_der_put(b, RP_DER_OID, oid, n); }

void rp_der_put_null(rp_buf_t *b) { rp_der_put(b, RP_DER_NULL, NULL, 0); }

void rp_der_wrap(rp_buf_t *b, uint8_t tag, rp_buf_t *child) {
    if (child->err != PROVEN_OK && b->err == PROVEN_OK) b->err = child->err;
    rp_der_put(b, tag, child->data, child->len);
    rp_buf_free(child);
}

// X.690 11.6 order, which proven_der_end_set_of puts the elements in.
void rp_der_set_of(rp_buf_t *b, uint8_t tag, const rp_der_span_t *elems, size_t n) {
    size_t total = 0;
    for (size_t i = 0; i < n; ++i) total += elems[i].n;
    proven_der_writer_t w;
    size_t at;
    if (!open_tail(b, elem_size(total), &w, &at)) return;
    proven_der_begin(&w, tag);
    for (size_t i = 0; i < n; ++i) proven_der_put_raw(&w, (proven_mem_view_t){ elems[i].p, elems[i].n });
    proven_der_end_set_of(&w);
    close_tail(b, &w, at);
}
