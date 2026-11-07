// src/crypto/der.c - DER reader and writer, BER to DER (include/rubrapack/der.h), on proven/der.h.

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

// proven_der_from_ber: definite lengths in their shortest form, constructed OCTET STRINGs joined,
// nesting to PROVEN_DER_BER_MAX_DEPTH (rubrapack's own depth limit, 64). It is measured first,
// so `limit` is checked before anything is allocated.
proven_err_t rp_ber_to_der(proven_allocator_t alloc, const uint8_t *in, size_t len, size_t limit, uint8_t **out,
                           size_t *out_len) {
    proven_mem_view_t ber = { in, len };
    size_t n = 0;
    proven_err_t e = proven_der_from_ber(ber, (proven_mem_mut_t){ NULL, 0 }, &n);
    if (e != PROVEN_OK && e != PROVEN_ERR_OUT_OF_BOUNDS) return PROVEN_ERR_INVALID_FORMAT;
    if (n > limit) return PROVEN_ERR_OUT_OF_BOUNDS;
    uint8_t *d = rp_mem_alloc(alloc, n ? n : 1, 1);
    if (d == NULL) return PROVEN_ERR_NOMEM;
    if (proven_der_from_ber(ber, (proven_mem_mut_t){ d, n }, &n) != PROVEN_OK) {
        rp_mem_free(alloc, d);
        return PROVEN_ERR_INVALID_FORMAT;
    }
    *out = d;
    *out_len = n;
    return PROVEN_OK;
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
