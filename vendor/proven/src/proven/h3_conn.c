#include "proven_internal_h3.h"
#include "../../platform/proven_sys_mem.h"

#ifndef PROVEN_NO_HTTP3

/*
 * HTTP/3 (RFC 9114) over a borrowed QUIC connection.
 *
 * Every stream is read for exactly what its parser needs next: a frame header a varint at a
 * time, a DATA payload straight into one shared buffer (so each DATA event is one read and
 * nothing of another stream waits behind it), a HEADERS or control payload into a buffer of
 * its own, sized to the frame and bounded, and an unknown frame skipped. A paused stream is
 * simply not read: QUIC's flow control does the rest.
 *
 * What is sent goes straight to the QUIC stream. Only frame headers and header sections that
 * the stream's send buffer could not take at once are held here, and a DATA frame's header is
 * written only for as many bytes as the stream takes now (proven_quic_stream_room), so a
 * frame is never begun that its payload cannot follow.
 *
 * QPACK's dynamic table (RFC-0032), when configured: as an encoder, once the peer's SETTINGS
 * allow one, this side opens its encoder stream, inserts the fields it sends and refers to an
 * entry only once the peer has acknowledged it, so the peer never waits. As a decoder it says the capacity in
 * SETTINGS, opens its decoder stream to acknowledge sections and inserts, feeds the peer's
 * encoder stream to the table, and holds a request stream whose field section needs inserts not
 * yet made - the stream is not read until they come. Without one (RFC-0017 B-4) it opens no
 * decoder stream and takes the peer's streams only for what a table of capacity 0 allows. There is no server push: MAX_PUSH_ID is never sent, so every push is
 * beyond the maximum (H3_ID_ERROR).
 */

#define H3_NO_LENGTH (~(proven_u64)0)
#define H3_READ_BUF 16384u
#define H3_CONTROL_MAX 16384u              /* a control frame's payload (SETTINGS) */

enum { K_REQ = 1, K_UNI_NEW, K_CONTROL, K_QENC, K_QDEC };
enum { RD_TYPE = 0, RD_LEN, RD_PAYLOAD };
enum { RQ_HEAD = 0, RQ_BODY, RQ_TRAILERS, RQ_DONE };     /* what the peer may send next on a request stream */
enum { WS_NONE = 0, WS_HEAD, WS_END };                   /* what this side has sent on it */

typedef struct {
    proven_u64 id;
    int kind;
    /* Reading */
    proven_byte_t vb[8];                    /* a varint being gathered */
    proven_size_t vn;
    int rd;
    proven_u64 ftype, fleft;
    int fmode;                              /* 0: DATA through the shared buffer; 1: buffered; 2: skipped */
    proven_byte_t *fbuf;
    proven_size_t fbuf_n, fbuf_cap;
    bool readable, paused, fin, rx_done;
    int rq;
    proven_u64 length, received;
    bool no_body;                           /* a client's HEAD: the response describes a body it does not carry */
    proven_qpack_stream_t qs;
    /* Writing */
    proven_byte_t *out;                     /* frame bytes the QUIC stream has not taken yet */
    proven_size_t out_off, out_n, out_cap;
    bool out_fin;
    int ws;
    bool tx_done, want_writable;
    bool known;                             /* the application has heard of it */
    bool dead;                              /* ended by an error of its own: let go after the step that found it */
    bool ev_rejected;                       /* a client: STREAM_RESET with H3_REQUEST_REJECTED owed (GOAWAY) */
    int qwait;                              /* its held field section waits for inserts: 1 a head, 2 trailers */
    proven_u64 qric;                        /* how many */
} proven_h3s_t;

struct proven_h3_conn {
    proven_allocator_t alloc;
    proven_size_t held;
    proven_quic_conn_t *q;
    bool server;
    proven_u32 cfg_list, cfg_debt;
    proven_h3s_t **st;
    proven_size_t nst, cap_st, cursor;
    /* This side's control stream */
    bool ctl_open;
    proven_u64 ctl_id;
    proven_byte_t *ctl_out;
    proven_size_t ctl_off, ctl_n, ctl_cap;
    /* The peer's */
    bool have_control, have_qenc, have_qdec, settings_seen;
    proven_u64 peer_list;                   /* the peer's SETTINGS_MAX_FIELD_SECTION_SIZE */
    bool have_max_push;
    proven_u64 max_push;
    /* Goodbye */
    bool goaway_sent, goaway_recv;
    bool goaway_early;                  /* a server's first goodbye of two: the ID said is not the last */
    proven_u64 goaway_sent_id, goaway_recv_id;
    proven_u64 next_peer_req;               /* a server: one above the highest request stream seen */
    proven_u32 debt;
    /* Reading */
    proven_byte_t *rbuf;
    proven_byte_t *hbuf;                    /* header array and scratch for decoded sections */
    proven_size_t hbuf_size, hcap;
    proven_byte_t *stale;                   /* a payload an event pointed into: freed at the next poll */
    proven_size_t stale_cap;
    const bool *sensitive;
    bool failed, closed, close_reported, oom;
    /* QPACK's dynamic table as a decoder (RFC-0032) */
    proven_qpack_dec_t *qdec;
    proven_u32 cfg_qcap, cfg_qblocked, nblocked;
    proven_u64 qknown;                      /* inserts the peer's encoder knows this side has: acknowledged or counted */
    bool dec_open;
    proven_u64 dec_id;
    proven_byte_t *dec_out;
    proven_size_t dec_off, dec_n, dec_cap;
    /* ... and as an encoder */
    proven_u64 peer_qcap;                   /* the peer's QPACK_MAX_TABLE_CAPACITY */
    proven_qpack_enc_t *qenc;
    bool enc_open;
    proven_u64 enc_id;
    proven_byte_t *enc_out;
    proven_size_t enc_off, enc_n, enc_cap;
};

// -----------------------------------------------------------------------------
// Memory
// -----------------------------------------------------------------------------

static void *h3_alloc(proven_h3_conn_t *h, proven_size_t n, proven_size_t align) {
    if (n == 0) n = 1;
    proven_result_mem_mut_t r = h->alloc.alloc_fn(h->alloc.ctx, n, align);
    if (r.err != PROVEN_OK) { h->oom = true; return NULL; }
    h->held += n;
    return r.value.ptr;
}

static void h3_free(proven_h3_conn_t *h, void *p, proven_size_t n) {
    if (!p) return;
    h->alloc.free_fn(h->alloc.ctx, p);
    h->held -= n < 1 ? 1 : n;
}

/* Append to a byte buffer [off, n) of capacity cap, moving its live part to the front or
 * growing it as needed. */
static bool buf_put(proven_h3_conn_t *h, proven_byte_t **p, proven_size_t *off, proven_size_t *n, proven_size_t *cap, const proven_byte_t *src, proven_size_t len) {
    if (*off == *n) *off = *n = 0;
    if (*cap - *n < len) {
        const proven_size_t live = *n - *off;
        proven_size_t want = *cap ? *cap : 64;
        while (want < live + len) want *= 2;
        proven_byte_t *q = *p;
        if (want > *cap) {
            q = (proven_byte_t *)h3_alloc(h, want, 1);
            if (!q) return false;
        }
        proven_sys_mem_move(q, *p + *off, live);
        if (q != *p) { h3_free(h, *p, *cap); *cap = want; }
        *p = q; *off = 0; *n = live;
    }
    proven_sys_mem_copy(*p + *n, src, len);
    *n += len;
    return true;
}

// -----------------------------------------------------------------------------
// The connection's end
// -----------------------------------------------------------------------------

static void h3_fail(proven_h3_conn_t *h, proven_u64 code) {
    if (h->failed) return;
    h->failed = true;
    proven_quic_conn_close(h->q, true, code);
}

// -----------------------------------------------------------------------------
// Streams
// -----------------------------------------------------------------------------

static proven_h3s_t *st_find(const proven_h3_conn_t *h, proven_u64 id) {
    for (proven_size_t i = 0; i < h->nst; ++i) if (h->st[i]->id == id) return h->st[i];
    return NULL;
}

static proven_h3s_t *st_add(proven_h3_conn_t *h, proven_u64 id, int kind) {
    if (h->nst == h->cap_st) {
        const proven_size_t want = h->cap_st ? h->cap_st * 2 : 8;
        proven_h3s_t **n = (proven_h3s_t **)h3_alloc(h, want * sizeof *n, alignof(proven_h3s_t *));
        if (!n) return NULL;
        for (proven_size_t i = 0; i < h->nst; ++i) n[i] = h->st[i];
        h3_free(h, h->st, h->cap_st * sizeof *n);
        h->st = n; h->cap_st = want;
    }
    proven_h3s_t *s = (proven_h3s_t *)h3_alloc(h, sizeof *s, alignof(proven_h3s_t));
    if (!s) return NULL;
    *s = (proven_h3s_t){ .id = id, .kind = kind, .length = H3_NO_LENGTH };
    h->st[h->nst++] = s;
    return s;
}

static void dec_put(proven_h3_conn_t *h, const proven_byte_t *p, proven_size_t n);
static void dec_flush(proven_h3_conn_t *h);

static void st_free(proven_h3_conn_t *h, proven_h3s_t *s) {
    /* A section of it not decoded, which may refer to the table: the encoder is told
     * (RFC 9204 section 4.4.2), so that the entries can be evicted. */
    if (h->qdec && s->kind == K_REQ) {
        const bool unread = s->qwait != 0 || (s->ftype == PROVEN_H3_FRAME_HEADERS && s->rd == RD_PAYLOAD && s->fmode == 1) || (s->rq == RQ_TRAILERS && s->fbuf);
        if (s->qwait) h->nblocked--;
        if (unread && !h->closed) { proven_byte_t b[12]; dec_put(h, b, proven_qpack_put_stream_cancel(b, s->id)); }
    }
    for (proven_size_t i = 0; i < h->nst; ++i) {
        if (h->st[i] != s) continue;
        h->st[i] = h->st[--h->nst];
        break;
    }
    if (h->cursor >= h->nst) h->cursor = 0;
    h3_free(h, s->fbuf, s->fbuf_cap);
    h3_free(h, s->out, s->out_cap);
    h3_free(h, s, sizeof *s);
}

static bool is_bidi(proven_u64 id) { return (id & 2u) == 0; }

/* One more stream ended badly; too many more than end well, and the connection goes. */
static void h3_debt(proven_h3_conn_t *h) {
    if (++h->debt > h->cfg_debt) h3_fail(h, PROVEN_H3_EXCESSIVE_LOAD);
}

/* A stream that has ended both ways is let go. */
static void st_maybe_free(proven_h3_conn_t *h, proven_h3s_t *s) {
    if (s->dead) { st_free(h, s); return; }
    if (s->kind != K_REQ || !s->rx_done || !s->tx_done || s->ev_rejected) return;
    if (s->rq == RQ_DONE && h->debt > 0) h->debt--;
    st_free(h, s);
}

/* Write what is held for a stream, then its end when that is owed. */
static void st_flush(proven_h3_conn_t *h, proven_h3s_t *s) {
    if (s->tx_done) return;
    while (s->out_off < s->out_n) {
        proven_size_t taken = 0;
        const proven_err_t e = proven_quic_stream_write(h->q, s->id, (proven_mem_view_t){ .ptr = s->out + s->out_off, .size = s->out_n - s->out_off }, false, &taken);
        if (e == PROVEN_ERR_NOMEM) { h->oom = true; return; }
        if (e != PROVEN_OK) { s->tx_done = true; return; }
        s->out_off += taken;
        if (taken == 0) return;
    }
    if (s->out_cap > 0) { h3_free(h, s->out, s->out_cap); s->out = NULL; s->out_cap = s->out_off = s->out_n = 0; }
    if (s->out_fin) {
        proven_size_t taken = 0;
        const proven_err_t e = proven_quic_stream_write(h->q, s->id, (proven_mem_view_t){ 0 }, true, &taken);
        if (e == PROVEN_ERR_NOMEM) { h->oom = true; return; }
        s->tx_done = true;
    }
}

/* This side's half has been ended. A server whose response is complete wants no more of the
 * request (RFC 9114 section 4.1). */
static void st_tx_ended(proven_h3_conn_t *h, proven_h3s_t *s) {
    if (h->server && !s->rx_done && s->ws == WS_END) {
        (void)proven_quic_stream_stop(h->q, s->id, PROVEN_H3_NO_ERROR);
        s->rx_done = true;
        s->rq = RQ_DONE;
    }
}

/* An error of one request stream: both halves end with `code`, and the application is told if
 * it knew the stream. The stream is let go by the caller of the step (st_maybe_free). */
static bool st_fault(proven_h3_conn_t *h, proven_h3s_t *s, proven_u64 code, proven_h3_event_t *ev) {
    if (!s->rx_done) (void)proven_quic_stream_stop(h->q, s->id, code);
    if (!s->tx_done) (void)proven_quic_stream_reset(h->q, s->id, code);
    s->rx_done = s->tx_done = true;
    s->dead = true;
    if (s->known) *ev = (proven_h3_event_t){ .kind = PROVEN_H3_EV_STREAM_RESET, .stream_id = s->id, .code = code };
    h3_debt(h);
    return s->known;
}

// -----------------------------------------------------------------------------
// Reading
// -----------------------------------------------------------------------------

/* Read into `p` at most `n` bytes of a stream. */
static proven_size_t st_read(proven_h3_conn_t *h, proven_h3s_t *s, proven_byte_t *p, proven_size_t n) {
    proven_size_t got = 0;
    bool fin = false;
    if (n == 0 && s->fin) return 0;
    const proven_err_t e = proven_quic_stream_read(h->q, s->id, (proven_mem_mut_t){ .ptr = p, .size = n }, &got, &fin);
    if (e != PROVEN_OK) { s->fin = true; return 0; }   /* gone under us (reset): treated as ended */
    if (fin) s->fin = true;
    return got;
}

/* A varint, gathered across reads: 1 complete, 0 not yet. */
static int st_varint(proven_h3_conn_t *h, proven_h3s_t *s, proven_u64 *v) {
    for (;;) {
        const proven_size_t need = s->vn == 0 ? 1 : (proven_size_t)1 << (s->vb[0] >> 6);
        if (s->vn == need) {
            proven_quic_rd_t r = { s->vb, s->vn, true };
            *v = proven_quic_rd_varint(&r);
            s->vn = 0;
            return 1;
        }
        const proven_size_t got = st_read(h, s, s->vb + s->vn, need - s->vn);
        if (got == 0) return 0;
        s->vn += got;
    }
}

static bool name_is(proven_u8str_view_t n, const char *t, proven_size_t len) {
    if (n.size != len) return false;
    for (proven_size_t i = 0; i < len; ++i) if (n.ptr[i] != (proven_byte_t)t[i]) return false;
    return true;
}

static bool content_length(const proven_http_header_t *f, proven_size_t count, proven_u64 *out) {
    *out = H3_NO_LENGTH;
    for (proven_size_t i = 0; i < count; ++i) {
        if (!name_is(f[i].name, "content-length", 14)) continue;
        const proven_u8str_view_t v = f[i].value;
        if (v.size == 0 || v.size > 19) return false;
        proven_u64 n = 0;
        for (proven_size_t k = 0; k < v.size; ++k) {
            if (v.ptr[k] < '0' || v.ptr[k] > '9') return false;
            n = n * 10 + (proven_u64)(v.ptr[k] - '0');
        }
        if (*out != H3_NO_LENGTH && *out != n) return false;
        *out = n;
    }
    return true;
}

static bool is_head(const proven_http_header_t *f, proven_size_t count) {
    for (proven_size_t i = 0; i < count; ++i) if (name_is(f[i].name, ":method", 7)) return name_is(f[i].value, "HEAD", 4);
    return false;
}

/* Decode a field section into the connection's header buffer: 1, 0 too large (a stream
 * error), -1 malformed (QPACK_DECOMPRESSION_FAILED, the connection's). A section that referred to
 * the table is acknowledged. */
static int h3_decode(proven_h3_conn_t *h, proven_u64 stream_id, proven_mem_view_t section, proven_size_t *count) {
    if (!h->hbuf) {
        h->hcap = h->cfg_list / PROVEN_QPACK_ENTRY_OVERHEAD + 1;
        h->hbuf_size = h->hcap * sizeof(proven_http_header_t) + h->cfg_list;
        h->hbuf = (proven_byte_t *)h3_alloc(h, h->hbuf_size, alignof(proven_http_header_t));
        if (!h->hbuf) return 0;
    }
    proven_http_header_t *fields = (proven_http_header_t *)(void *)h->hbuf;
    const proven_mem_mut_t scratch = { .ptr = h->hbuf + h->hcap * sizeof(proven_http_header_t), .size = h->cfg_list };
    proven_u64 ric = 0;
    const proven_err_t e = proven_qpack_decode_dyn(h->qdec, section, fields, NULL, h->hcap, scratch, h->cfg_list, count, &ric);
    if ((e == PROVEN_OK || e == PROVEN_ERR_OUT_OF_BOUNDS) && ric > 0) {
        proven_byte_t b[12];
        dec_put(h, b, proven_qpack_put_section_ack(b, stream_id));
        if (ric > h->qknown) h->qknown = ric;
    }
    if (e == PROVEN_OK) return 1;
    return e == PROVEN_ERR_OUT_OF_BOUNDS ? 0 : -1;
}

/* Whether the stream's held field section needs inserts not yet made: then it is kept, and
 * the stream is not read until they come (`which`: 1 a head, 2 trailers). More such streams
 * than this side allowed is QPACK_DECOMPRESSION_FAILED (RFC 9204 section 2.1.2). */
static bool st_qblock(proven_h3_conn_t *h, proven_h3s_t *s, int which) {
    proven_u64 ric = 0;
    if (!h->qdec || proven_qpack_dec_required(h->qdec, (proven_mem_view_t){ .ptr = s->fbuf, .size = s->fbuf_n }, &ric) != PROVEN_OK) return false;
    if (ric <= proven_qpack_dec_inserts(h->qdec)) return false;
    if (h->nblocked >= h->cfg_qblocked) { h3_fail(h, PROVEN_QPACK_DECOMPRESSION_FAILED); return true; }
    s->qwait = which;
    s->qric = ric;
    h->nblocked++;
    return true;
}

/* The stream's buffered payload is now the event's: kept until the next poll. */
static proven_mem_view_t st_take_payload(proven_h3_conn_t *h, proven_h3s_t *s) {
    h3_free(h, h->stale, h->stale_cap);
    h->stale = s->fbuf; h->stale_cap = s->fbuf_cap;
    const proven_mem_view_t v = { .ptr = s->fbuf, .size = s->fbuf_n };
    s->fbuf = NULL; s->fbuf_cap = s->fbuf_n = 0;
    return v;
}

/* The peer's side of a request stream ended at a frame boundary. */
static bool req_end(proven_h3_conn_t *h, proven_h3s_t *s, proven_h3_event_t *ev) {
    if (s->rq == RQ_HEAD) {
        /* Ended before a request, or before a final response (RFC 9114 section 4.1). */
        return st_fault(h, s, PROVEN_H3_REQUEST_INCOMPLETE, ev);
    }
    if (s->rq == RQ_DONE) { s->rx_done = true; return false; }
    if (!s->no_body && s->length != H3_NO_LENGTH && s->received != s->length) return st_fault(h, s, PROVEN_H3_MESSAGE_ERROR, ev);
    if (s->rq == RQ_TRAILERS) {
        if (st_qblock(h, s, 2)) return false;
        proven_size_t count = 0;
        const proven_mem_view_t section = st_take_payload(h, s);
        const int d = h3_decode(h, s->id, section, &count);
        if (d < 0) { h3_fail(h, PROVEN_QPACK_DECOMPRESSION_FAILED); return false; }
        const proven_http_header_t *fields = (const proven_http_header_t *)(void *)h->hbuf;
        if (d == 0 || proven_h2_trailers_check(fields, count) != PROVEN_OK) { const bool known = st_fault(h, s, PROVEN_H3_MESSAGE_ERROR, ev); if (known) ev->too_large = d == 0; return known; }
        *ev = (proven_h3_event_t){ .kind = PROVEN_H3_EV_TRAILERS, .stream_id = s->id, .end_stream = true, .fields = fields, .field_count = count };
    } else {
        *ev = (proven_h3_event_t){ .kind = PROVEN_H3_EV_DATA, .stream_id = s->id, .end_stream = true };
    }
    s->rq = RQ_DONE;
    s->rx_done = true;
    return true;
}

/* A complete HEADERS frame on a request stream. */

/* RFC 9114 section 4.3.1: a request for a scheme with an authority - http, https - carries
 * `:authority` or a Host field. (HTTP/2's rules, which the rest of the head follows, only advise it.) */
static bool has_authority(const proven_h2_request_head_t *head) {
    if (head->authority.size > 0) return true;
    const proven_u8str_view_t sc = head->scheme;
    const bool web = name_is(sc, "http", 4) || name_is(sc, "https", 5);
    if (!web) return true;
    for (proven_size_t i = 0; i < head->field_count; ++i)
        if (name_is(head->fields[i].name, "host", 4) && head->fields[i].value.size > 0) return true;
    return false;
}

static bool req_headers(proven_h3_conn_t *h, proven_h3s_t *s, proven_h3_event_t *ev) {
    if (s->rq == RQ_BODY) {
        /* Trailers: checked and given out when the stream ends, as nothing may follow them. */
        if (!s->no_body && s->length != H3_NO_LENGTH && s->received != s->length) return st_fault(h, s, PROVEN_H3_MESSAGE_ERROR, ev);
        s->rq = RQ_TRAILERS;
        return s->fin ? req_end(h, s, ev) : false;
    }
    if (st_qblock(h, s, 1)) return false;
    proven_size_t count = 0;
    const proven_mem_view_t section = st_take_payload(h, s);
    const int d = h3_decode(h, s->id, section, &count);
    if (d < 0) { h3_fail(h, PROVEN_QPACK_DECOMPRESSION_FAILED); return false; }
    if (h->oom) return false;
    const proven_http_header_t *fields = (const proven_http_header_t *)(void *)h->hbuf;
    if (d == 0) { const bool known = st_fault(h, s, PROVEN_H3_MESSAGE_ERROR, ev); if (known) ev->too_large = true; return known; }
    if (h->server) {
        proven_h2_request_head_t head;
        proven_u64 length = H3_NO_LENGTH;
        if (proven_h2_request_head(fields, count, &head) != PROVEN_OK || head.protocol.size > 0
            || !content_length(head.fields, head.field_count, &length) || !has_authority(&head)) return st_fault(h, s, PROVEN_H3_MESSAGE_ERROR, ev);
        /* A request that is also the stream's end carries no body. */
        if (s->fin && length != H3_NO_LENGTH && length != 0) return st_fault(h, s, PROVEN_H3_MESSAGE_ERROR, ev);
        s->length = length;
        s->rq = s->fin ? RQ_DONE : RQ_BODY;
        s->rx_done = s->fin;
        s->known = true;
        *ev = (proven_h3_event_t){ .kind = PROVEN_H3_EV_HEADERS, .stream_id = s->id, .request = head, .end_stream = s->fin };
        return true;
    }
    proven_u16 status = 0;
    const proven_http_header_t *rest = NULL;
    proven_size_t rest_count = 0;
    if (proven_h2_response_head(fields, count, &status, &rest, &rest_count) != PROVEN_OK || status == 101) return st_fault(h, s, PROVEN_H3_MESSAGE_ERROR, ev);
    if (status < 200) {
        if (s->fin) return st_fault(h, s, PROVEN_H3_MESSAGE_ERROR, ev);   /* the stream ends on an interim response */
    } else {
        proven_u64 length = H3_NO_LENGTH;
        if (!content_length(rest, rest_count, &length)) return st_fault(h, s, PROVEN_H3_MESSAGE_ERROR, ev);
        if (status == 204 || status == 304) s->no_body = true;
        if (s->fin && !s->no_body && length != H3_NO_LENGTH && length != 0) return st_fault(h, s, PROVEN_H3_MESSAGE_ERROR, ev);
        s->length = length;
        s->rq = s->fin ? RQ_DONE : RQ_BODY;
        s->rx_done = s->fin;
    }
    *ev = (proven_h3_event_t){ .kind = PROVEN_H3_EV_HEADERS, .stream_id = s->id, .status = status, .fields = rest, .field_count = rest_count, .end_stream = s->fin };
    return true;
}

/* A frame's header was read on a request stream: is the frame allowed, and how is it read? */
static int req_frame_start(proven_h3_conn_t *h, proven_h3s_t *s, proven_h3_event_t *ev, bool *told) {
    switch (s->ftype) {
    case PROVEN_H3_FRAME_DATA:
        if (s->rq != RQ_BODY) { h3_fail(h, PROVEN_H3_FRAME_UNEXPECTED); return -1; }
        s->fmode = 0;
        return 0;
    case PROVEN_H3_FRAME_HEADERS:
        if (s->rq == RQ_TRAILERS) { h3_fail(h, PROVEN_H3_FRAME_UNEXPECTED); return -1; }
        /* Twice the limit can hold no section that is accepted. */
        if (s->fleft > (proven_u64)h->cfg_list * 2) { *told = st_fault(h, s, PROVEN_H3_MESSAGE_ERROR, ev); return -1; }
        s->fmode = 1;
        return 0;
    case PROVEN_H3_FRAME_PUSH_PROMISE:
        h3_fail(h, h->server ? PROVEN_H3_FRAME_UNEXPECTED : PROVEN_H3_ID_ERROR);
        return -1;
    case PROVEN_H3_FRAME_CANCEL_PUSH: case PROVEN_H3_FRAME_SETTINGS: case PROVEN_H3_FRAME_GOAWAY: case PROVEN_H3_FRAME_MAX_PUSH_ID:
    case 0x02: case 0x06: case 0x08: case 0x09:            /* HTTP/2's, reserved (RFC 9114 section 7.2.8) */
        h3_fail(h, PROVEN_H3_FRAME_UNEXPECTED);
        return -1;
    default:
        s->fmode = 2;
        return 0;
    }
}

/* Read a payload that is buffered whole: true when it is complete. */
static bool st_fill(proven_h3_conn_t *h, proven_h3s_t *s) {
    if (!s->fbuf && s->fleft > 0) {
        s->fbuf = (proven_byte_t *)h3_alloc(h, (proven_size_t)s->fleft, 1);
        if (!s->fbuf) return false;
        s->fbuf_cap = (proven_size_t)s->fleft;
        s->fbuf_n = 0;
    }
    while (s->fleft > 0) {
        const proven_size_t got = st_read(h, s, s->fbuf + s->fbuf_n, (proven_size_t)s->fleft);
        if (got == 0) return false;
        s->fbuf_n += got;
        s->fleft -= got;
    }
    return true;
}

/* Advance a request stream: true with an event. */
static bool req_step(proven_h3_conn_t *h, proven_h3s_t *s, proven_h3_event_t *ev) {
    for (;;) {
        if (h->failed || h->oom) return false;
        if (s->qwait) {
            /* A held section: decoded once its inserts have come, the stream read on after it. */
            if (s->qric > proven_qpack_dec_inserts(h->qdec)) return false;
            const int which = s->qwait;
            s->qwait = 0;
            h->nblocked--;
            if (which == 2) return req_end(h, s, ev);
            const bool got_event = req_headers(h, s, ev);
            if (got_event || s->dead || s->rx_done || h->failed || h->oom || s->qwait) return got_event;
            continue;
        }
        if (s->rd == RD_TYPE || s->rd == RD_LEN) {
            proven_u64 v = 0;
            if (!st_varint(h, s, &v)) {
                if (!s->fin) return false;
                if (s->rd == RD_TYPE && s->vn == 0) return req_end(h, s, ev);
                h3_fail(h, PROVEN_H3_FRAME_ERROR);          /* ended inside a frame */
                return false;
            }
            if (s->rd == RD_TYPE) { s->ftype = v; s->rd = RD_LEN; continue; }
            s->fleft = v;
            s->rd = RD_PAYLOAD;
            bool told = false;
            if (req_frame_start(h, s, ev, &told) < 0) return told;
            continue;
        }
        if (s->fmode == 0) {
            if (s->fleft == 0) { s->rd = RD_TYPE; continue; }
            const proven_size_t want = s->fleft < H3_READ_BUF ? (proven_size_t)s->fleft : H3_READ_BUF;
            const proven_size_t got = st_read(h, s, h->rbuf, want);
            if (got == 0) {
                if (s->fin) h3_fail(h, PROVEN_H3_FRAME_ERROR);
                return false;
            }
            s->fleft -= got;
            s->received += got;
            if (!s->no_body && s->length != H3_NO_LENGTH && s->received > s->length) return st_fault(h, s, PROVEN_H3_MESSAGE_ERROR, ev);
            if (s->fleft == 0) s->rd = RD_TYPE;
            if (s->fin && s->fleft > 0) { h3_fail(h, PROVEN_H3_FRAME_ERROR); return false; }
            if (s->fin) {
                /* The last bytes of the stream: the end is checked with them. */
                if (!s->no_body && s->length != H3_NO_LENGTH && s->received != s->length) return st_fault(h, s, PROVEN_H3_MESSAGE_ERROR, ev);
                s->rq = RQ_DONE;
                s->rx_done = true;
            }
            *ev = (proven_h3_event_t){ .kind = PROVEN_H3_EV_DATA, .stream_id = s->id, .data = { .ptr = h->rbuf, .size = got }, .end_stream = s->fin };
            return true;
        }
        if (s->fmode == 2) {
            while (s->fleft > 0) {
                const proven_size_t want = s->fleft < H3_READ_BUF ? (proven_size_t)s->fleft : H3_READ_BUF;
                const proven_size_t got = st_read(h, s, h->rbuf, want);
                if (got == 0) {
                    if (s->fin) h3_fail(h, PROVEN_H3_FRAME_ERROR);
                    return false;
                }
                s->fleft -= got;
            }
            s->rd = RD_TYPE;
            continue;
        }
        if (!st_fill(h, s)) {
            if (s->fin && !h->oom) h3_fail(h, PROVEN_H3_FRAME_ERROR);
            return false;
        }
        s->rd = RD_TYPE;
        const bool got_event = req_headers(h, s, ev);
        if (got_event || s->dead || s->rx_done || h->failed || h->oom) return got_event;
        /* Trailers held until the end: read on for it. */
    }
}

/* A SETTINGS payload (RFC 9114 section 7.2.4). A setting this side knows that comes twice is
 * refused; an unknown one has no meaning either time (a repeat MAY be refused, and is not). */
static bool h3_settings(proven_h3_conn_t *h, proven_mem_view_t p) {
    proven_quic_rd_t r = { p.ptr, p.size, true };
    bool seen[3] = { false, false, false };
    while (r.ok && r.n > 0) {
        const proven_u64 id = proven_quic_rd_varint(&r);
        const proven_u64 value = proven_quic_rd_varint(&r);
        if (!r.ok) { h3_fail(h, PROVEN_H3_FRAME_ERROR); return false; }
        if (id >= 0x02 && id <= 0x05) { h3_fail(h, PROVEN_H3_SETTINGS_ERROR); return false; }   /* HTTP/2's */
        const int k = id == PROVEN_H3_SETTING_QPACK_MAX_TABLE_CAPACITY ? 0 : id == PROVEN_H3_SETTING_MAX_FIELD_SECTION_SIZE ? 1
                    : id == PROVEN_H3_SETTING_QPACK_BLOCKED_STREAMS ? 2 : -1;
        if (k < 0) continue;
        if (seen[k]) { h3_fail(h, PROVEN_H3_SETTINGS_ERROR); return false; }
        seen[k] = true;
        if (k == 1) h->peer_list = value;
        if (k == 0) h->peer_qcap = value;
    }
    return true;
}

static void enc_flush(proven_h3_conn_t *h) {
    while (h->enc_open && h->enc_off < h->enc_n) {
        proven_size_t taken = 0;
        const proven_err_t e = proven_quic_stream_write(h->q, h->enc_id, (proven_mem_view_t){ .ptr = h->enc_out + h->enc_off, .size = h->enc_n - h->enc_off }, false, &taken);
        if (e == PROVEN_ERR_NOMEM) { h->oom = true; return; }
        if (e != PROVEN_OK || taken == 0) return;
        h->enc_off += taken;
    }
}

/* The peer allows a table: this side's encoder uses the smaller of the two sizes, on an encoder
 * stream of its own (RFC-0032). */
static void h3_start_encoder(proven_h3_conn_t *h) {
    /* The section prefix is encoded with the peer's exact maximum (RFC 9204 section 4.5.1.1): one
     * past 32 bits is not one this side can describe, and its table is not used. */
    if (h->qenc || h->cfg_qcap == 0 || h->peer_qcap < 32 || h->peer_qcap > 0xffffffffu || h->failed) return;
    const proven_u32 cap = h->cfg_qcap < h->peer_qcap ? h->cfg_qcap : (proven_u32)h->peer_qcap;
    if (proven_qpack_enc_create(h->alloc, cap, (proven_u32)h->peer_qcap, &h->qenc) != PROVEN_OK) { h->qenc = NULL; return; }
    const proven_err_t e = proven_quic_stream_open(h->q, false, &h->enc_id);
    if (e == PROVEN_ERR_NOMEM) { h->oom = true; return; }
    if (e != PROVEN_OK) { proven_qpack_enc_destroy(h->qenc); h->qenc = NULL; return; }   /* no stream for it: the table is not used */
    h->enc_open = true;
    const proven_byte_t type = PROVEN_H3_STREAM_QPACK_ENCODER;
    if (!buf_put(h, &h->enc_out, &h->enc_off, &h->enc_n, &h->enc_cap, &type, 1)) return;
    enc_flush(h);
}

/* Every request stream at or above a server's GOAWAY was not acted on. */
static void h3_rejected_from(proven_h3_conn_t *h, proven_u64 id) {
    for (proven_size_t i = 0; i < h->nst; ++i) {
        proven_h3s_t *s = h->st[i];
        if (s->kind != K_REQ || s->id < id || s->ev_rejected) continue;
        if (!s->rx_done) (void)proven_quic_stream_stop(h->q, s->id, PROVEN_H3_REQUEST_CANCELLED);
        if (!s->tx_done) (void)proven_quic_stream_reset(h->q, s->id, PROVEN_H3_REQUEST_CANCELLED);
        s->rx_done = s->tx_done = true;
        s->ev_rejected = true;
    }
}

/* A complete control frame. */
static bool ctl_frame(proven_h3_conn_t *h, proven_h3s_t *s, proven_h3_event_t *ev) {
    const proven_mem_view_t p = { .ptr = s->fbuf, .size = s->fbuf_n };
    proven_u64 v = 0;
    if (s->ftype != PROVEN_H3_FRAME_SETTINGS) {
        /* One varint exactly. */
        proven_quic_rd_t r = { p.ptr, p.size, true };
        v = proven_quic_rd_varint(&r);
        if (!r.ok || r.n != 0) { h3_fail(h, PROVEN_H3_FRAME_ERROR); return false; }
    }
    switch (s->ftype) {
    case PROVEN_H3_FRAME_SETTINGS:
        if (h3_settings(h, p)) h3_start_encoder(h);
        return false;
    case PROVEN_H3_FRAME_CANCEL_PUSH:
        h3_fail(h, PROVEN_H3_ID_ERROR);                    /* no push ID was ever allowed */
        return false;
    case PROVEN_H3_FRAME_MAX_PUSH_ID:
        if (h->have_max_push && v < h->max_push) { h3_fail(h, PROVEN_H3_ID_ERROR); return false; }
        h->have_max_push = true;
        h->max_push = v;
        return false;
    case PROVEN_H3_FRAME_GOAWAY:
        if (!h->server && (v & 3u) != 0) { h3_fail(h, PROVEN_H3_ID_ERROR); return false; }
        if (h->goaway_recv && v > h->goaway_recv_id) { h3_fail(h, PROVEN_H3_ID_ERROR); return false; }
        h->goaway_recv = true;
        h->goaway_recv_id = v;
        if (!h->server) h3_rejected_from(h, v);
        *ev = (proven_h3_event_t){ .kind = PROVEN_H3_EV_GOAWAY, .id = v };
        return true;
    default:
        return false;
    }
}

/* Advance the peer's control stream: true with an event. */
static bool ctl_step(proven_h3_conn_t *h, proven_h3s_t *s, proven_h3_event_t *ev) {
    for (;;) {
        if (h->failed || h->oom) return false;
        if (s->rd == RD_TYPE || s->rd == RD_LEN) {
            proven_u64 v = 0;
            if (!st_varint(h, s, &v)) {
                if (s->fin) h3_fail(h, PROVEN_H3_CLOSED_CRITICAL_STREAM);
                return false;
            }
            if (s->rd == RD_TYPE) { s->ftype = v; s->rd = RD_LEN; continue; }
            s->fleft = v;
            s->rd = RD_PAYLOAD;
            if (!h->settings_seen && s->ftype != PROVEN_H3_FRAME_SETTINGS) { h3_fail(h, PROVEN_H3_MISSING_SETTINGS); return false; }
            switch (s->ftype) {
            case PROVEN_H3_FRAME_SETTINGS:
                if (h->settings_seen) { h3_fail(h, PROVEN_H3_FRAME_UNEXPECTED); return false; }
                h->settings_seen = true;
                if (s->fleft > H3_CONTROL_MAX) { h3_fail(h, PROVEN_H3_EXCESSIVE_LOAD); return false; }
                s->fmode = 1;
                break;
            case PROVEN_H3_FRAME_MAX_PUSH_ID:
                if (!h->server) { h3_fail(h, PROVEN_H3_FRAME_UNEXPECTED); return false; }
                [[fallthrough]];
            case PROVEN_H3_FRAME_CANCEL_PUSH: case PROVEN_H3_FRAME_GOAWAY:
                /* Larger than any control frame is taken: the load, as for SETTINGS; within that,
                 * more than one varint: the frame. */
                if (s->fleft > H3_CONTROL_MAX) { h3_fail(h, PROVEN_H3_EXCESSIVE_LOAD); return false; }
                if (s->fleft == 0 || s->fleft > 8) { h3_fail(h, PROVEN_H3_FRAME_ERROR); return false; }
                s->fmode = 1;
                break;
            case PROVEN_H3_FRAME_DATA: case PROVEN_H3_FRAME_HEADERS: case PROVEN_H3_FRAME_PUSH_PROMISE:
            case 0x02: case 0x06: case 0x08: case 0x09:
                h3_fail(h, PROVEN_H3_FRAME_UNEXPECTED);
                return false;
            default:
                s->fmode = 2;
                break;
            }
            continue;
        }
        if (s->fmode == 2) {
            while (s->fleft > 0) {
                const proven_size_t want = s->fleft < H3_READ_BUF ? (proven_size_t)s->fleft : H3_READ_BUF;
                const proven_size_t got = st_read(h, s, h->rbuf, want);
                if (got == 0) {
                    if (s->fin) h3_fail(h, PROVEN_H3_CLOSED_CRITICAL_STREAM);
                    return false;
                }
                s->fleft -= got;
            }
            s->rd = RD_TYPE;
            continue;
        }
        if (!st_fill(h, s)) {
            if (s->fin && !h->oom) h3_fail(h, PROVEN_H3_CLOSED_CRITICAL_STREAM);
            return false;
        }
        s->rd = RD_TYPE;
        const bool got_event = ctl_frame(h, s, ev);
        h3_free(h, s->fbuf, s->fbuf_cap);
        s->fbuf = NULL; s->fbuf_cap = s->fbuf_n = 0;
        if (got_event) return true;
    }
}

/* Advance one of the peer's QPACK streams. */
static void qpack_step(proven_h3_conn_t *h, proven_h3s_t *s) {
    for (;;) {
        const proven_size_t got = st_read(h, s, h->rbuf, H3_READ_BUF);
        if (got == 0) {
            if (s->fin) h3_fail(h, PROVEN_H3_CLOSED_CRITICAL_STREAM);
            return;
        }
        const proven_mem_view_t in = { .ptr = h->rbuf, .size = got };
        const proven_u64 code = s->kind == K_QDEC ? (h->qenc ? proven_qpack_enc_feed(h->qenc, in) : proven_qpack_decoder_stream_feed(&s->qs, in))
                              : h->qdec ? proven_qpack_dec_feed(h->qdec, in) : proven_qpack_encoder_stream_feed(&s->qs, in);
        if (code != 0) { h3_fail(h, code); return; }
        if (s->kind == K_QENC && h->qdec) {
            /* New entries: counted to the encoder, and the streams that waited for them read. */
            const proven_u64 have = proven_qpack_dec_inserts(h->qdec);
            if (have > h->qknown) { proven_byte_t b[12]; dec_put(h, b, proven_qpack_put_insert_count(b, have - h->qknown)); h->qknown = have; }
            for (proven_size_t i = 0; i < h->nst; ++i) if (h->st[i]->qwait && h->st[i]->qric <= have) h->st[i]->readable = true;
        }
        if (s->fin) { h3_fail(h, PROVEN_H3_CLOSED_CRITICAL_STREAM); return; }
    }
}

/* A unidirectional stream of the peer's, its type not yet known. */
static void uni_step(proven_h3_conn_t *h, proven_h3s_t *s) {
    proven_u64 type = 0;
    if (!st_varint(h, s, &type)) {
        /* Ended before its type: tolerated (RFC 9114 section 6.2). */
        if (s->fin) st_free(h, s);
        return;
    }
    switch (type) {
    case PROVEN_H3_STREAM_CONTROL:
        if (h->have_control) { h3_fail(h, PROVEN_H3_STREAM_CREATION_ERROR); return; }
        h->have_control = true;
        s->kind = K_CONTROL;
        return;
    case PROVEN_H3_STREAM_PUSH:
        /* A client's is not allowed at all; to a client, no push ID was ever allowed. */
        h3_fail(h, h->server ? PROVEN_H3_STREAM_CREATION_ERROR : PROVEN_H3_ID_ERROR);
        return;
    case PROVEN_H3_STREAM_QPACK_ENCODER:
        if (h->have_qenc) { h3_fail(h, PROVEN_H3_STREAM_CREATION_ERROR); return; }
        h->have_qenc = true;
        s->kind = K_QENC;
        return;
    case PROVEN_H3_STREAM_QPACK_DECODER:
        if (h->have_qdec) { h3_fail(h, PROVEN_H3_STREAM_CREATION_ERROR); return; }
        h->have_qdec = true;
        s->kind = K_QDEC;
        return;
    default:
        /* Unknown, or reserved to be ignored: not read (section 6.2). */
        (void)proven_quic_stream_stop(h->q, s->id, PROVEN_H3_STREAM_CREATION_ERROR);
        st_free(h, s);
        return;
    }
}

/* Advance a stream that may be read: true with an event. */
static bool st_step(proven_h3_conn_t *h, proven_h3s_t *s, proven_h3_event_t *ev) {
    for (;;) {
        const int kind = s->kind;
        switch (kind) {
        case K_REQ:
            if (s->rx_done) { s->readable = false; return false; }
            return req_step(h, s, ev);
        case K_UNI_NEW: {
            const proven_u64 id = s->id;    /* uni_step may let the stream go */
            uni_step(h, s);
            if (h->failed || !st_find(h, id) || s->kind == K_UNI_NEW) return false;
        }
            continue;                       /* its type is known: read on */
        case K_CONTROL:
            return ctl_step(h, s, ev);
        default:
            qpack_step(h, s);
            return false;
        }
    }
}

// -----------------------------------------------------------------------------
// This side's control stream
// -----------------------------------------------------------------------------

static void ctl_flush(proven_h3_conn_t *h) {
    while (h->ctl_off < h->ctl_n) {
        proven_size_t taken = 0;
        const proven_err_t e = proven_quic_stream_write(h->q, h->ctl_id, (proven_mem_view_t){ .ptr = h->ctl_out + h->ctl_off, .size = h->ctl_n - h->ctl_off }, false, &taken);
        if (e == PROVEN_ERR_NOMEM) { h->oom = true; return; }
        if (e != PROVEN_OK || taken == 0) return;
        h->ctl_off += taken;
    }
}

static bool ctl_put_frame(proven_h3_conn_t *h, proven_u64 type, const proven_byte_t *p, proven_size_t n) {
    proven_byte_t hd[16];
    proven_size_t at = proven_quic_varint_put(type, hd);
    at += proven_quic_varint_put(n, hd + at);
    return buf_put(h, &h->ctl_out, &h->ctl_off, &h->ctl_n, &h->ctl_cap, hd, at) && buf_put(h, &h->ctl_out, &h->ctl_off, &h->ctl_n, &h->ctl_cap, p, n);
}

/* The handshake is complete: the control stream and SETTINGS, before anything else. */
static void h3_open_control(proven_h3_conn_t *h) {
    if (h->ctl_open) return;
    const proven_err_t e = proven_quic_stream_open(h->q, false, &h->ctl_id);
    if (e == PROVEN_ERR_NOMEM) { h->oom = true; return; }
    /* A peer must let each side open three (RFC 9114 section 6.2). */
    if (e != PROVEN_OK) { h3_fail(h, PROVEN_H3_GENERAL_PROTOCOL_ERROR); return; }
    h->ctl_open = true;
    proven_byte_t type = PROVEN_H3_STREAM_CONTROL, settings[48];
    proven_size_t n = proven_quic_varint_put(PROVEN_H3_SETTING_MAX_FIELD_SECTION_SIZE, settings);
    n += proven_quic_varint_put(h->cfg_list, settings + n);
    if (h->qdec) {
        n += proven_quic_varint_put(PROVEN_H3_SETTING_QPACK_MAX_TABLE_CAPACITY, settings + n);
        n += proven_quic_varint_put(h->cfg_qcap, settings + n);
        n += proven_quic_varint_put(PROVEN_H3_SETTING_QPACK_BLOCKED_STREAMS, settings + n);
        n += proven_quic_varint_put(h->cfg_qblocked, settings + n);
    }
    if (!buf_put(h, &h->ctl_out, &h->ctl_off, &h->ctl_n, &h->ctl_cap, &type, 1) || !ctl_put_frame(h, PROVEN_H3_FRAME_SETTINGS, settings, n)) return;
    ctl_flush(h);
    if (h->qdec) {
        /* The decoder stream, for what the table's encoder must hear. */
        const proven_err_t d = proven_quic_stream_open(h->q, false, &h->dec_id);
        if (d == PROVEN_ERR_NOMEM) { h->oom = true; return; }
        if (d != PROVEN_OK) { h3_fail(h, PROVEN_H3_GENERAL_PROTOCOL_ERROR); return; }
        h->dec_open = true;
        /* Instructions owed before the stream could be opened (0-RTT) go after its type. */
        proven_byte_t *held = h->dec_out;
        const proven_size_t held_off = h->dec_off, held_n = h->dec_n, held_cap = h->dec_cap;
        h->dec_out = NULL; h->dec_off = h->dec_n = h->dec_cap = 0;
        const proven_byte_t dtype = PROVEN_H3_STREAM_QPACK_DECODER;
        if (buf_put(h, &h->dec_out, &h->dec_off, &h->dec_n, &h->dec_cap, &dtype, 1) && held_n > held_off)
            (void)buf_put(h, &h->dec_out, &h->dec_off, &h->dec_n, &h->dec_cap, held + held_off, held_n - held_off);
        h3_free(h, held, held_cap);
        dec_flush(h);
    }
}

// -----------------------------------------------------------------------------
// This side's QPACK decoder stream (RFC-0032)
// -----------------------------------------------------------------------------

static void dec_flush(proven_h3_conn_t *h) {
    while (h->dec_open && h->dec_off < h->dec_n) {
        proven_size_t taken = 0;
        const proven_err_t e = proven_quic_stream_write(h->q, h->dec_id, (proven_mem_view_t){ .ptr = h->dec_out + h->dec_off, .size = h->dec_n - h->dec_off }, false, &taken);
        if (e == PROVEN_ERR_NOMEM) { h->oom = true; return; }
        if (e != PROVEN_OK || taken == 0) return;
        h->dec_off += taken;
    }
}

/* An instruction for the peer's encoder: held until the stream is open and takes it. The type
 * byte goes first, as the stream is opened with the control stream. */
static void dec_put(proven_h3_conn_t *h, const proven_byte_t *p, proven_size_t n) {
    if (!h->qdec || h->failed) return;
    if (!buf_put(h, &h->dec_out, &h->dec_off, &h->dec_n, &h->dec_cap, p, n)) return;
    dec_flush(h);
}

// -----------------------------------------------------------------------------
// Events
// -----------------------------------------------------------------------------

static bool h3_quic_event(proven_h3_conn_t *h, const proven_quic_event_t *q, proven_h3_event_t *ev) {
    proven_h3s_t *s = NULL;
    switch (q->kind) {
    case PROVEN_QUIC_EV_ESTABLISHED:
    case PROVEN_QUIC_EV_EARLY:          /* 0-RTT (RFC-0021): streams may be used before the handshake ends */
        h3_open_control(h);
        *ev = (proven_h3_event_t){ .kind = PROVEN_H3_EV_QUIC, .quic = *q };
        return true;
    case PROVEN_QUIC_EV_STREAM_OPENED:
        if (h->failed || st_find(h, q->stream_id)) return false;
        if (is_bidi(q->stream_id)) {
            if (!h->server) { h3_fail(h, PROVEN_H3_STREAM_CREATION_ERROR); return false; }
            if (q->stream_id >= h->next_peer_req) h->next_peer_req = q->stream_id + 4;
            if (h->goaway_sent && q->stream_id >= h->goaway_sent_id) {
                /* Said goodbye: not acted on, and safe to send again elsewhere. */
                (void)proven_quic_stream_stop(h->q, q->stream_id, PROVEN_H3_REQUEST_REJECTED);
                (void)proven_quic_stream_reset(h->q, q->stream_id, PROVEN_H3_REQUEST_REJECTED);
                return false;
            }
            s = st_add(h, q->stream_id, K_REQ);
        } else {
            s = st_add(h, q->stream_id, K_UNI_NEW);
            if (s) s->tx_done = true;
        }
        if (s) s->readable = true;
        return false;
    case PROVEN_QUIC_EV_STREAM_READABLE:
        s = st_find(h, q->stream_id);
        if (s) s->readable = true;
        return false;
    case PROVEN_QUIC_EV_STREAM_WRITABLE:
        if (h->ctl_open && q->stream_id == h->ctl_id) { ctl_flush(h); return false; }
        if (h->dec_open && q->stream_id == h->dec_id) { dec_flush(h); return false; }
        if (h->enc_open && q->stream_id == h->enc_id) { enc_flush(h); return false; }
        s = st_find(h, q->stream_id);
        if (!s || s->kind != K_REQ) return false;
        st_flush(h, s);
        if (s->tx_done) st_tx_ended(h, s);
        if (s->want_writable && s->out_off == s->out_n && !s->tx_done) {
            s->want_writable = false;
            *ev = (proven_h3_event_t){ .kind = PROVEN_H3_EV_STREAM_WRITABLE, .stream_id = s->id };
            return true;
        }
        st_maybe_free(h, s);
        return false;
    case PROVEN_QUIC_EV_STREAM_RESET:
        s = st_find(h, q->stream_id);
        if (!s) return false;
        if (s->kind == K_CONTROL || s->kind == K_QENC || s->kind == K_QDEC) { h3_fail(h, PROVEN_H3_CLOSED_CRITICAL_STREAM); return false; }
        if (s->kind == K_UNI_NEW) { st_free(h, s); return false; }
        if (s->rx_done) return false;       /* after its end, or after this side stopped it */
        {
            const bool told = s->known;
            if (!s->tx_done) (void)proven_quic_stream_reset(h->q, s->id, PROVEN_H3_REQUEST_CANCELLED);
            if (told) *ev = (proven_h3_event_t){ .kind = PROVEN_H3_EV_STREAM_RESET, .stream_id = s->id, .code = q->code };
            st_free(h, s);
            h3_debt(h);
            return told;
        }
    case PROVEN_QUIC_EV_STOP_SENDING:
        if ((h->ctl_open && q->stream_id == h->ctl_id) || (h->dec_open && q->stream_id == h->dec_id) || (h->enc_open && q->stream_id == h->enc_id)) { h3_fail(h, PROVEN_H3_CLOSED_CRITICAL_STREAM); return false; }
        s = st_find(h, q->stream_id);
        if (!s || s->kind != K_REQ || s->tx_done) return false;
        s->tx_done = true;
        h3_free(h, s->out, s->out_cap);
        s->out = NULL; s->out_cap = s->out_off = s->out_n = 0;
        if (s->known) {
            *ev = (proven_h3_event_t){ .kind = PROVEN_H3_EV_STOP_SENDING, .stream_id = s->id, .code = q->code };
            st_maybe_free(h, s);
            return true;
        }
        st_maybe_free(h, s);
        return false;
    case PROVEN_QUIC_EV_CLOSED:
        h->closed = true;
        h->close_reported = true;
        *ev = (proven_h3_event_t){ .kind = PROVEN_H3_EV_CLOSED, .code = q->code, .app = q->app, .remote = q->remote };
        return true;
    default:
        *ev = (proven_h3_event_t){ .kind = PROVEN_H3_EV_QUIC, .quic = *q };
        return true;
    }
}

static bool h3_any_requests(const proven_h3_conn_t *h) {
    for (proven_size_t i = 0; i < h->nst; ++i) if (h->st[i]->kind == K_REQ) return true;
    return false;
}

bool proven_h3_conn_poll(proven_h3_conn_t *h, proven_h3_event_t *ev) {
    if (!h || !ev) return false;
    *ev = (proven_h3_event_t){ 0 };
    h3_free(h, h->stale, h->stale_cap);
    h->stale = NULL; h->stale_cap = 0;
    /* With no request open the header buffer is let go: an idle connection keeps the read
     * buffer and its control streams, no more. */
    if (h->hbuf && !h3_any_requests(h)) { h3_free(h, h->hbuf, h->hbuf_size); h->hbuf = NULL; }
    if (h->close_reported) return false;
    for (;;) {
        if (h->oom) h3_fail(h, PROVEN_H3_INTERNAL_ERROR);
        if (!h->failed) {
            for (proven_size_t i = 0; i < h->nst; ++i) {
                proven_h3s_t *s = h->st[i];
                if (!s->ev_rejected) continue;
                s->ev_rejected = false;
                *ev = (proven_h3_event_t){ .kind = PROVEN_H3_EV_STREAM_RESET, .stream_id = s->id, .code = PROVEN_H3_REQUEST_REJECTED };
                const bool told = s->known;
                st_free(h, s);
                if (told) return true;
                i = (proven_size_t)-1;      /* the array moved: from the start */
            }
            /* Streams with something to read, in turn. */
            for (proven_size_t k = 0; k < h->nst && !h->failed; ++k) {
                const proven_size_t i = (h->cursor + k) % h->nst;
                proven_h3s_t *s = h->st[i];
                if (!s->readable || s->paused) continue;
                const proven_u64 id = s->id;
                const bool got = st_step(h, s, ev);
                proven_h3s_t *still = st_find(h, id);
                if (still && !got) still->readable = false;
                if (still) st_maybe_free(h, still);
                if (got) {
                    if (h->nst) h->cursor = (i + 1) % h->nst;
                    return true;
                }
                k = (proven_size_t)-1;      /* streams may have gone: look again from the start */
                if (h->nst == 0) break;
                /* Every pass clears a readable flag or frees a stream, so this ends. */
            }
            /* Goodbye, and nothing left: the close waits until what was sent is acknowledged. */
            if (((h->goaway_sent && !h->goaway_early) || (!h->server && h->goaway_recv)) && !h->failed && !h3_any_requests(h) && !proven_quic_conn_sending(h->q)) h3_fail(h, PROVEN_H3_NO_ERROR);
        }
        proven_quic_event_t q;
        if (!proven_quic_conn_poll(h->q, &q)) return false;
        if (h3_quic_event(h, &q, ev)) return true;
    }
}

// -----------------------------------------------------------------------------
// Sending
// -----------------------------------------------------------------------------

/* A HEADERS frame for `headers` into the stream's held bytes. */
/* `*ric` and `*low`: what the section refers to in the table, for proven_qpack_enc_sent once its
 * stream is known. */
static proven_err_t h3_put_headers(proven_h3_conn_t *h, proven_h3s_t *s, const proven_http_header_t *headers, proven_size_t count, proven_u64 *ric, proven_u64 *low) {
    const bool *sensitive = h->sensitive;
    h->sensitive = NULL;
    *ric = *low = 0;
    proven_u64 list = 0;
    for (proven_size_t i = 0; i < count; ++i) list += (proven_u64)headers[i].name.size + headers[i].value.size + PROVEN_QPACK_ENTRY_OVERHEAD;
    if (list > h->peer_list) return PROVEN_ERR_OUT_OF_BOUNDS;
    const proven_size_t bound = proven_qpack_encode_bound(headers, count) + 8;
    proven_byte_t *tmp = (proven_byte_t *)h3_alloc(h, bound + 9, 1);
    if (!tmp) { h->oom = false; return PROVEN_ERR_NOMEM; }
    proven_size_t len = 9;
    proven_err_t e;
    if (h->qenc) {
        /* The inserts go on the encoder stream; the section refers only to what the peer has. */
        const proven_size_t ibound = proven_qpack_enc_ins_bound(headers, count);
        proven_byte_t *ins = (proven_byte_t *)h3_alloc(h, ibound, 1);
        if (!ins) { h->oom = false; h3_free(h, tmp, bound + 9); return PROVEN_ERR_NOMEM; }
        proven_size_t il = 0;
        e = proven_qpack_encode_dyn(h->qenc, headers, count, sensitive, (proven_mem_mut_t){ .ptr = tmp, .size = bound + 9 }, &len, (proven_mem_mut_t){ .ptr = ins, .size = ibound }, &il, ric, low);
        if (e == PROVEN_OK && il > 0) {
            if (buf_put(h, &h->enc_out, &h->enc_off, &h->enc_n, &h->enc_cap, ins, il)) enc_flush(h);
            else h->oom = true;            /* the peer's table would no longer match this one's: the connection goes */
        }
        h3_free(h, ins, ibound);
    } else {
        e = proven_qpack_encode(headers, count, sensitive, (proven_mem_mut_t){ .ptr = tmp, .size = bound + 9 }, &len);
    }
    if (e == PROVEN_OK) {
        proven_byte_t hd[16];
        proven_size_t at = proven_quic_varint_put(PROVEN_H3_FRAME_HEADERS, hd);
        at += proven_quic_varint_put(len - 9, hd + at);
        if (!buf_put(h, &s->out, &s->out_off, &s->out_n, &s->out_cap, hd, at) || !buf_put(h, &s->out, &s->out_off, &s->out_n, &s->out_cap, tmp + 9, len - 9)) {
            h->oom = false;
            e = PROVEN_ERR_NOMEM;
        }
    }
    h3_free(h, tmp, bound + 9);
    return e;
}

proven_err_t proven_h3_submit_request(proven_h3_conn_t *h, const proven_http_header_t *headers, proven_size_t count, bool end_stream, proven_u64 *stream_id) {
    if (!h || !stream_id || (count > 0 && !headers)) return PROVEN_ERR_INVALID_ARG;
    if (h->server) { h->sensitive = NULL; return PROVEN_ERR_INVALID_ARG; }
    proven_h2_request_head_t head;
    if (proven_h2_request_head(headers, count, &head) != PROVEN_OK || head.protocol.size > 0) { h->sensitive = NULL; return PROVEN_ERR_INVALID_ARG; }
    if (h->failed || h->closed || h->goaway_recv || h->goaway_sent || !h->ctl_open) { h->sensitive = NULL; return PROVEN_ERR_INVALID_STATE; }
    proven_h3s_t probe = { 0 };
    proven_u64 ric = 0, low = 0;
    proven_err_t e = h3_put_headers(h, &probe, headers, count, &ric, &low);
    if (e != PROVEN_OK) { h3_free(h, probe.out, probe.out_cap); return e; }
    proven_u64 id = 0;
    e = proven_quic_stream_open(h->q, true, &id);
    if (e != PROVEN_OK) { h3_free(h, probe.out, probe.out_cap); return e; }
    proven_h3s_t *s = st_add(h, id, K_REQ);
    if (!s) {
        h->oom = false;
        h3_free(h, probe.out, probe.out_cap);
        (void)proven_quic_stream_reset(h->q, id, PROVEN_H3_REQUEST_CANCELLED);
        return PROVEN_ERR_NOMEM;
    }
    s->out = probe.out; s->out_off = probe.out_off; s->out_n = probe.out_n; s->out_cap = probe.out_cap;
    if (!proven_qpack_enc_sent(h->qenc, id, ric, low)) h->oom = true;
    s->known = true;
    s->no_body = is_head(headers, count);
    s->ws = end_stream ? WS_END : WS_HEAD;
    s->out_fin = end_stream;
    st_flush(h, s);
    *stream_id = id;
    return PROVEN_OK;
}

static proven_h3s_t *h3_sendable(proven_h3_conn_t *h, proven_u64 id, proven_err_t *e) {
    proven_h3s_t *s = st_find(h, id);
    if (!s || s->kind != K_REQ || s->ev_rejected) { *e = PROVEN_ERR_NOT_FOUND; return NULL; }
    if (h->failed || h->closed || s->tx_done || s->out_fin) { *e = PROVEN_ERR_INVALID_STATE; return NULL; }
    return s;
}

proven_err_t proven_h3_submit_response(proven_h3_conn_t *h, proven_u64 stream_id, const proven_http_header_t *headers, proven_size_t count, bool end_stream) {
    if (!h || (count > 0 && !headers)) return PROVEN_ERR_INVALID_ARG;
    if (!h->server) { h->sensitive = NULL; return PROVEN_ERR_INVALID_ARG; }
    proven_u16 status = 0;
    const proven_http_header_t *rest = NULL;
    proven_size_t rest_count = 0;
    if (proven_h2_response_head(headers, count, &status, &rest, &rest_count) != PROVEN_OK || status == 101 || (status < 200 && end_stream)) {
        h->sensitive = NULL;
        return PROVEN_ERR_INVALID_ARG;
    }
    proven_err_t e = PROVEN_OK;
    proven_h3s_t *s = h3_sendable(h, stream_id, &e);
    if (!s) { h->sensitive = NULL; return e; }
    if (s->ws != WS_NONE || !s->known) { h->sensitive = NULL; return PROVEN_ERR_INVALID_STATE; }
    proven_u64 ric = 0, low = 0;
    e = h3_put_headers(h, s, headers, count, &ric, &low);
    if (e != PROVEN_OK) return e;
    if (!proven_qpack_enc_sent(h->qenc, s->id, ric, low)) h->oom = true;
    if (status >= 200) {
        s->ws = end_stream ? WS_END : WS_HEAD;
        s->out_fin = end_stream;
    }
    st_flush(h, s);
    if (s->ws == WS_END) st_tx_ended(h, s);
    st_maybe_free(h, s);
    return PROVEN_OK;
}

proven_err_t proven_h3_send_data(proven_h3_conn_t *h, proven_u64 stream_id, proven_mem_view_t data, bool end_stream, proven_size_t *taken) {
    if (!h || !taken || (data.size > 0 && !data.ptr)) return PROVEN_ERR_INVALID_ARG;
    *taken = 0;
    proven_err_t e = PROVEN_OK;
    proven_h3s_t *s = h3_sendable(h, stream_id, &e);
    if (!s) return e;
    if (s->ws != WS_HEAD) return PROVEN_ERR_INVALID_STATE;
    st_flush(h, s);
    if (h->oom) { h->oom = false; return PROVEN_ERR_NOMEM; }
    if (s->tx_done) return PROVEN_ERR_INVALID_STATE;
    /* An end with no bytes is taken whatever is queued before it: it goes with the last of that. */
    if (data.size > 0 && s->out_off < s->out_n) { s->want_writable = true; return PROVEN_OK; }
    proven_size_t n = 0;
    if (data.size > 0) {
        const proven_u64 room = proven_quic_stream_room(h->q, s->id);
        n = data.size;
        if (room < (proven_u64)n + 1 + proven_quic_varint_len(n)) {
            n = room > 2 + 8 ? (proven_size_t)(room - 1 - 8) : 0;
            if (n > data.size) n = data.size;
        }
        if (n == 0) { s->want_writable = true; return PROVEN_OK; }
        proven_byte_t hd[16];
        proven_size_t at = proven_quic_varint_put(PROVEN_H3_FRAME_DATA, hd);
        at += proven_quic_varint_put(n, hd + at);
        proven_size_t t1 = 0, t2 = 0;
        e = proven_quic_stream_write(h->q, s->id, (proven_mem_view_t){ .ptr = hd, .size = at }, false, &t1);
        if (e == PROVEN_OK && t1 == at) e = proven_quic_stream_write(h->q, s->id, (proven_mem_view_t){ .ptr = data.ptr, .size = n }, end_stream && n == data.size, &t2);
        if (e != PROVEN_OK) return e;
        if (t1 != at || t2 != n) return PROVEN_ERR_INVALID_STATE;   /* room was said and not given: cannot happen */
        if (n < data.size) s->want_writable = true;
        *taken = n;
        if (!(end_stream && n == data.size)) return PROVEN_OK;
        s->ws = WS_END;
        s->tx_done = true;
    } else if (end_stream) {
        s->ws = WS_END;
        s->out_fin = true;
        st_flush(h, s);
    } else {
        return PROVEN_OK;
    }
    st_tx_ended(h, s);
    st_maybe_free(h, s);
    return PROVEN_OK;
}

proven_err_t proven_h3_send_trailers(proven_h3_conn_t *h, proven_u64 stream_id, const proven_http_header_t *headers, proven_size_t count) {
    if (!h || (count > 0 && !headers)) return PROVEN_ERR_INVALID_ARG;
    if (proven_h2_trailers_check(headers, count) != PROVEN_OK) { h->sensitive = NULL; return PROVEN_ERR_INVALID_ARG; }
    proven_err_t e = PROVEN_OK;
    proven_h3s_t *s = h3_sendable(h, stream_id, &e);
    if (!s) { h->sensitive = NULL; return e; }
    if (s->ws != WS_HEAD) { h->sensitive = NULL; return PROVEN_ERR_INVALID_STATE; }
    proven_u64 ric = 0, low = 0;
    e = h3_put_headers(h, s, headers, count, &ric, &low);
    if (e != PROVEN_OK) return e;
    if (!proven_qpack_enc_sent(h->qenc, s->id, ric, low)) h->oom = true;
    s->ws = WS_END;
    s->out_fin = true;
    st_flush(h, s);
    st_tx_ended(h, s);
    st_maybe_free(h, s);
    return PROVEN_OK;
}

void proven_h3_next_sensitive(proven_h3_conn_t *h, const bool *sensitive) {
    if (h) h->sensitive = sensitive;
}

proven_err_t proven_h3_stream_reset(proven_h3_conn_t *h, proven_u64 stream_id, proven_u64 code) {
    if (!h || code > PROVEN_QUIC_VARINT_MAX) return PROVEN_ERR_INVALID_ARG;
    proven_h3s_t *s = st_find(h, stream_id);
    if (!s || s->kind != K_REQ) return PROVEN_ERR_NOT_FOUND;
    if (code == 0) code = PROVEN_H3_REQUEST_CANCELLED;
    if (!s->rx_done) (void)proven_quic_stream_stop(h->q, s->id, code);
    if (!s->tx_done) (void)proven_quic_stream_reset(h->q, s->id, code);
    st_free(h, s);
    return PROVEN_OK;
}

void proven_h3_stream_pause(proven_h3_conn_t *h, proven_u64 stream_id, bool paused) {
    proven_h3s_t *s = h ? st_find(h, stream_id) : NULL;
    if (s) { s->paused = paused; if (!paused) s->readable = true; }
}

proven_err_t proven_h3_conn_goaway_step(proven_h3_conn_t *h, bool early) {
    if (!h) return PROVEN_ERR_INVALID_ARG;
    if (h->failed || h->closed || !h->ctl_open) return PROVEN_ERR_INVALID_STATE;
    early = early && h->server;
    if (h->goaway_sent && (early || !h->goaway_early)) return PROVEN_OK;
    /* A server: the first request stream it will not act on - or, the first time of two, the
     * largest a client may open, so that nothing already on its way is refused (RFC 9114
     * section 5.2). A client: a push ID, and it allows none. */
    const proven_u64 id = !h->server ? 0 : early ? ((proven_u64)1 << 62) - 4 : h->next_peer_req;
    proven_byte_t p[8];
    const proven_size_t n = proven_quic_varint_put(id, p);
    if (!ctl_put_frame(h, PROVEN_H3_FRAME_GOAWAY, p, n)) { h->oom = false; return PROVEN_ERR_NOMEM; }
    h->goaway_sent = true;
    h->goaway_early = early;
    h->goaway_sent_id = id;
    ctl_flush(h);
    return PROVEN_OK;
}

proven_err_t proven_h3_conn_goaway(proven_h3_conn_t *h) { return proven_h3_conn_goaway_step(h, false); }

proven_size_t proven_h3_conn_memory(const proven_h3_conn_t *h) { return h ? h->held : 0; }

// -----------------------------------------------------------------------------
// Making and freeing
// -----------------------------------------------------------------------------

void proven_h3_conn_test_qpack_(const proven_h3_conn_t *h, proven_u64 *enc_inserts, proven_u64 *enc_known, proven_u64 *dec_inserts) {
    *enc_inserts = proven_qpack_enc_inserts(h->qenc);
    *enc_known = proven_qpack_enc_known(h->qenc);
    *dec_inserts = proven_qpack_dec_inserts(h->qdec);
}

proven_err_t proven_h3_conn_create(const proven_h3_config_t *config, proven_quic_conn_t *quic, proven_h3_conn_t **out) {
    if (out) *out = NULL;
    if (!config || !quic || !out || !proven_alloc_is_valid(config->alloc) || config->max_field_section_bytes > ((proven_u32)1 << 20) ||
        (config->qpack_capacity != 0 && (config->qpack_capacity < 32 || config->qpack_capacity > ((proven_u32)1 << 20))) || config->qpack_blocked_streams > 65535u) return PROVEN_ERR_INVALID_ARG;
    proven_result_mem_mut_t m = config->alloc.alloc_fn(config->alloc.ctx, sizeof(proven_h3_conn_t), alignof(proven_h3_conn_t));
    if (m.err != PROVEN_OK) return PROVEN_ERR_NOMEM;
    proven_h3_conn_t *h = (proven_h3_conn_t *)(void *)m.value.ptr;
    *h = (proven_h3_conn_t){ .alloc = config->alloc, .q = quic, .server = config->server,
                             .cfg_list = config->max_field_section_bytes ? config->max_field_section_bytes : 16384,
                             .cfg_debt = config->max_reset_debt ? config->max_reset_debt : 100,
                             .peer_list = ~(proven_u64)0 };
    h->rbuf = (proven_byte_t *)h3_alloc(h, H3_READ_BUF, 1);
    if (!h->rbuf) { config->alloc.free_fn(config->alloc.ctx, h); return PROVEN_ERR_NOMEM; }
    if (config->qpack_capacity != 0) {
        h->cfg_qcap = config->qpack_capacity;
        h->cfg_qblocked = config->qpack_blocked_streams ? config->qpack_blocked_streams : 16;
        if (proven_qpack_dec_create(config->alloc, h->cfg_qcap, &h->qdec) != PROVEN_OK) { proven_h3_conn_destroy(h); return PROVEN_ERR_NOMEM; }
    }
    *out = h;
    return PROVEN_OK;
}

void proven_h3_conn_destroy(proven_h3_conn_t *h) {
    if (!h) return;
    while (h->nst > 0) st_free(h, h->st[h->nst - 1]);
    h3_free(h, h->st, h->cap_st * sizeof(proven_h3s_t *));
    h3_free(h, h->ctl_out, h->ctl_cap);
    h3_free(h, h->dec_out, h->dec_cap);
    proven_qpack_dec_destroy(h->qdec);
    h->qdec = NULL;
    h3_free(h, h->enc_out, h->enc_cap);
    proven_qpack_enc_destroy(h->qenc);
    h->qenc = NULL;
    h3_free(h, h->rbuf, H3_READ_BUF);
    h3_free(h, h->hbuf, h->hbuf_size);
    h3_free(h, h->stale, h->stale_cap);
    const proven_allocator_t a = h->alloc;
    a.free_fn(a.ctx, h);
}

#endif /* PROVEN_NO_HTTP3 */
