#include "proven/http_event.h"
#include "../platform/proven_sys_net_mem.h"
#include "proven_internal_http_event.h"
#include "proven_internal_http_coding.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET) && !defined(PROVEN_NO_HTTP3)

#include "proven_internal_quic.h"
#include "proven_internal_h3.h"
#include "proven_internal_quic_net.h"
#include "proven/time.h"

/*
 * HTTP/3 in the event-driven server.
 *
 * Unlike HTTP/2 there is no connection record of http_event.c underneath: a QUIC connection is
 * not a socket but a stream of datagrams on a UDP socket that many connections share. So this
 * file has three things of its own. An endpoint (h3e_t) is one UDP socket, opened beside each
 * TCP listener on the same address and port. A driver (h3d_t) is one QUIC connection - the
 * transport (quic_conn.c) and HTTP/3 over it (h3_conn.c), neither of which does any I/O - and
 * its streams. A table from connection ID to driver tells datagrams apart; every ID a
 * connection gives its peer is in it, and the one the client chose for its first packets.
 *
 * A handler is given an proven_h3s_t as its `proven_http_stream_t *`, whose first member is the head
 * http_event.c reads: `kind` sends every call here, as for HTTP/2.
 *
 * Re-entrancy, as in http_event_h2.c: streams and drivers count the entries on the stack and
 * are freed when the outermost leaves. **Events are taken from the engine only from the loop**
 * - a datagram, a timer, the round a driver asked for - never inside a call a handler made: a
 * handler's call sends what it made and asks for a round (`d_mark`), and the round reads the
 * events, on_writable among them. A round is asked for by watching a socket for writability,
 * which comes back at once.
 */

#define H3_DATAGRAM ((proven_size_t)65536)   /* what one receive may hold */
#define H3_OUT ((proven_size_t)1500)         /* what one send may hold: QUIC's largest here is 1,452 */
#define H3_READS 256                         /* datagrams taken from a socket in one wake */
#define H3_BATCH PROVEN_NET_UDP_BATCH_BYTES   /* datagrams of one size to one peer, sent in one call (RFC-0018 S-3) */
#define H3_SENDS 64                          /* datagrams a connection sends in one round */
#define H3_KEYS 8                            /* connection IDs a connection is found by: four issued, the client's first, room */
#define H3_ENDS (EV_MAX_LISTENERS * 2)       /* sockets: those listening, and those kept for connections after stop_listening */

typedef struct h3d h3d_t;
typedef struct h3e h3e_t;

typedef struct h3s {
    proven_ev_head_t h;                       /* first: what http_event.c reads */
    h3d_t *d;                          /* not to be followed once `dead` */
    struct h3s *prev, *next;
    void *user;
    proven_byte_t *out;                /* response body made and not yet taken by the engine: compressed, or what respond could not hand over */
    proven_size_t out_len, out_off, out_cap;
    proven_http_compressor_t_ *zip;
    proven_u64 response_left;          /* body bytes still promised; LENGTH_UNKNOWN when not counted */
    proven_u64 body_seen;
    proven_time_t body_due;            /* when a request whose body is owed has been silent too long; 0: nothing is owed, or the handler paused it */
    proven_u64 id;
    proven_http_method_t method;
    bool active;                       /* on_request was delivered and on_done has not been */
    bool response_begun, response_ended, bodiless, paused, dead;
    bool want_writable, compress_asked;
    proven_u8 compress_coding;
    bool in_done;                      /* the request's end has arrived */
    bool early;                        /* handed to the handler before the handshake was complete: early data (RFC-0021) */
    bool end_asked;                    /* the end goes when `out` has drained */
    bool engine_done;                  /* the engine has this side's end already, or the stream is gone from it */
    bool blocked;                      /* the engine took less than offered: it says when there is room (STREAM_WRITABLE) */
    proven_http_trailers_t_ in_trailers;   /* the request's, from its end until on_done (proven_http_stream_trailers) */
    proven_http_trailers_t_ out_trailers;  /* the response's, sent as the stream's end once `out` has drained */
} proven_h3s_t;

struct h3e {
    struct ev_h3 *x;
    proven_http_event_server_t *server;
    proven_net_udp_t udp;
    proven_loop_udp_t lu;              /* the socket as the loop knows it (RFC-0025) */
    proven_byte_t *rbuf;               /* what one receive takes: the loop's while a receive is in flight */
    proven_net_addr_t from;            /* and where it came from */
    proven_net_addr_t bound;
    proven_size_t users;               /* connections that send from here */
    bool open, closing;                /* closing: stop_listening - no new connection, and closed when its last is over */
    bool sock_closing;                 /* closed, the loop's `closed` not come yet: the slot is not used again before */
    bool orphan;                       /* the server's HTTP/3 is gone: `closed` frees this */
    bool waiting;                      /* a send wait is asked for */
    bool ecn;                          /* the system reads and sets the ECN field here (RFC-0035) */
    proven_u8 rx_ecn;                  /* the ECN field of the datagrams being read */
    proven_u8 pend_ecn;                /* and of those kept */
    proven_size_t pend_len;            /* datagrams the socket would not take: nothing more goes until it has */
    proven_size_t pend_seg;            /* their size (the last may be shorter) */
    proven_net_addr_t pend_to;
    proven_byte_t pend[H3_BATCH];
};

typedef struct { proven_quic_cid_t cid; h3d_t *d; } proven_h3key_t;   /* free: no `d` and length 0; deleted: no `d` and length 255 */

struct ev_h3 {
    proven_http_event_server_t *server;
    proven_quic_config_t qc;
    h3e_t *ends[H3_ENDS];
    h3d_t *conns;
    h3d_t *dirty, *work;               /* connections owed a round: asked for, and being given one */
    proven_h3key_t *keys;
    proven_size_t key_cap, key_used;   /* slots, and slots not free (deleted ones too) */
    proven_loop_after_t after;         /* a round for the connections owed one, from the loop */
    proven_byte_t *batch;              /* a connection's datagrams gathered for one send: H3_BATCH bytes */
    proven_byte_t out[H3_OUT];
    bool stopped;                      /* stop_listening: no new connection */
};

struct h3d {
    proven_http_event_server_t *server;
    struct ev_h3 *x;
    h3e_t *e;                          /* the socket its datagrams go out from */
    h3d_t *prev, *next;
    h3d_t *dprev, *dnext;
    proven_u8 list;                    /* 0: owed no round; 1: in `dirty`; 2: in `work` */
    proven_quic_conn_t *q;
    proven_h3_conn_t *h3;
    proven_net_addr_t peer;
    proven_quic_cid_t keys[H3_KEYS];
    proven_size_t nkeys;
    proven_h3s_t *streams;
    proven_size_t stream_count;
    proven_loop_timer_t timer;
    proven_u32 depth, calls;
    bool dead;                         /* gone from the server; freed when the outermost entry leaves */
    bool closed;                       /* HTTP/3 is over: the QUIC connection is closing, or closed */
    bool closing;                      /* this side has closed it, and waits for the engine to be done */
    bool asked;                        /* a request has arrived */
    proven_u8 bye;                     /* GOAWAY: 0 none, 1 the first of two, 2 the last */
    proven_time_t born, idle_since, write_since, bye_due;
};



static void *h3_alloc(proven_allocator_t a, proven_size_t n) {
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, n, 16);
    if (m.err != PROVEN_OK) return (void *)0;
    proven_net_mem_set(m.value.ptr, 0, n);
    return m.value.ptr;
}

static bool h3_append(proven_allocator_t a, proven_byte_t **buf, proven_size_t *len, proven_size_t *cap, const proven_byte_t *p, proven_size_t n) {
    if (n == 0) return true;
    if (*len + n > *cap) {
        proven_size_t want = *cap ? *cap : 256;
        while (want < *len + n) { if (want > (proven_size_t)-1 / 2) return false; want *= 2; }
        proven_byte_t *q = h3_alloc(a, want);
        if (!q) return false;
        proven_net_mem_copy(q, *buf, *len);
        if (*buf) a.free_fn(a.ctx, *buf);
        *buf = q;
        *cap = want;
    }
    proven_net_mem_copy(*buf + *len, p, n);
    *len += n;
    return true;
}

/* QUIC's clock is microseconds. */
static proven_u64 h3_now_us(void) { return (proven_u64)proven_time_monotonic_now() / 1000u; }

#define h3_qaddr proven_quic_addr_of_
#define h3_naddr proven_net_addr_of_quic_

// -----------------------------------------------------------------------------
// Connection IDs
// -----------------------------------------------------------------------------

static proven_size_t key_hash(const proven_quic_cid_t *c) {
    proven_u64 h = 0xcbf29ce484222325u;
    for (proven_size_t i = 0; i < c->len; ++i) { h ^= c->id[i]; h *= 0x100000001b3u; }
    return (proven_size_t)(h ^ (h >> 29));
}

static bool key_same(const proven_quic_cid_t *a, const proven_quic_cid_t *b) {
    if (a->len != b->len) return false;
    for (proven_size_t i = 0; i < a->len; ++i) if (a->id[i] != b->id[i]) return false;
    return true;
}

static h3d_t *key_find(const struct ev_h3 *x, const proven_quic_cid_t *cid) {
    if (x->key_cap == 0 || cid->len == 0) return (void *)0;
    for (proven_size_t i = key_hash(cid) & (x->key_cap - 1), n = 0; n < x->key_cap; ++n, i = (i + 1) & (x->key_cap - 1)) {
        const proven_h3key_t *k = &x->keys[i];
        if (!k->d && k->cid.len == 0) return (void *)0;
        if (k->d && key_same(&k->cid, cid)) return k->d;
    }
    return (void *)0;
}

static bool key_put(struct ev_h3 *x, const proven_quic_cid_t *cid, h3d_t *d);

/* A larger table - or the same size without its deleted slots. */
static bool key_grow(struct ev_h3 *x) {
    proven_size_t live = 0;
    for (proven_size_t i = 0; i < x->key_cap; ++i) live += x->keys[i].d != (void *)0;
    proven_size_t cap = 64;
    while (cap < (live + 1) * 2) cap *= 2;
    proven_allocator_t a = x->server->alloc;
    proven_h3key_t *keys = h3_alloc(a, cap * sizeof *keys), *old = x->keys;
    if (!keys) return false;
    const proven_size_t old_cap = x->key_cap;
    x->keys = keys; x->key_cap = cap; x->key_used = 0;
    for (proven_size_t i = 0; i < old_cap; ++i) if (old[i].d) (void)key_put(x, &old[i].cid, old[i].d);
    if (old) a.free_fn(a.ctx, old);
    return true;
}

static bool key_put(struct ev_h3 *x, const proven_quic_cid_t *cid, h3d_t *d) {
    if (cid->len == 0 || cid->len > sizeof cid->id) return false;
    if ((x->key_used + 1) * 4 > x->key_cap * 3 && !key_grow(x)) return false;
    proven_size_t at = (proven_size_t)-1;
    for (proven_size_t i = key_hash(cid) & (x->key_cap - 1), n = 0; n < x->key_cap; ++n, i = (i + 1) & (x->key_cap - 1)) {
        proven_h3key_t *k = &x->keys[i];
        if (k->d && key_same(&k->cid, cid)) { k->d = d; return true; }
        if (!k->d && at == (proven_size_t)-1) at = i;
        if (!k->d && k->cid.len == 0) break;
    }
    if (at == (proven_size_t)-1) return false;
    if (x->keys[at].cid.len == 0) x->key_used++;
    x->keys[at] = (proven_h3key_t){ .cid = *cid, .d = d };
    return true;
}

static void key_del(struct ev_h3 *x, const proven_quic_cid_t *cid, const h3d_t *d) {
    if (x->key_cap == 0 || cid->len == 0) return;
    for (proven_size_t i = key_hash(cid) & (x->key_cap - 1), n = 0; n < x->key_cap; ++n, i = (i + 1) & (x->key_cap - 1)) {
        proven_h3key_t *k = &x->keys[i];
        if (!k->d && k->cid.len == 0) return;
        if (k->d == d && key_same(&k->cid, cid)) { k->d = (void *)0; k->cid.len = 255; return; }
    }
}

/* A connection is found by `cid` from now. False when it cannot be (no memory, or no room in
 * its own list): the peer's packets to it are then not delivered, which it survives as loss -
 * it has other IDs - or not, as a connection that went away. */
static bool d_key_add(h3d_t *d, const proven_quic_cid_t *cid) {
    for (proven_size_t i = 0; i < d->nkeys; ++i) if (key_same(&d->keys[i], cid)) return true;
    if (d->nkeys == H3_KEYS || !key_put(d->x, cid, d)) return false;
    d->keys[d->nkeys++] = *cid;
    return true;
}

static void d_key_drop(h3d_t *d, const proven_quic_cid_t *cid) {
    for (proven_size_t i = 0; i < d->nkeys; ++i) {
        if (!key_same(&d->keys[i], cid)) continue;
        key_del(d->x, cid, d);
        d->keys[i] = d->keys[--d->nkeys];
        return;
    }
}

// -----------------------------------------------------------------------------
// Sockets
// -----------------------------------------------------------------------------


static void x_rounds(struct ev_h3 *x);
static void x_after(void *ctx) { x_rounds(ctx); }

/* What is asked of the loop: to be told when a socket takes datagrams again, for each that holds
 * some it would not take; and a round for the connections owed one. */
static void x_rearm(struct ev_h3 *x) {
    for (proven_size_t i = 0; i < H3_ENDS; ++i) {
        h3e_t *e = x->ends[i];
        if (!e || !e->open || e->pend_len == 0 || e->waiting) continue;
        if (proven_loop_udp_send_wait_(x->server->loop, &e->lu) == PROVEN_OK) e->waiting = true;
    }
    if (x->dirty) proven_loop_after_(x->server->loop, &x->after, x_after, x);
}

static void e_close(h3e_t *e) {
    if (!e->open) return;
    proven_loop_udp_close_(e->x->server->loop, &e->lu);   /* the loop stops watching before the close */
    (void)proven_net_udp_close(&e->udp);
    e->open = false;
    e->pend_len = 0;
    e->waiting = false;
    e->sock_closing = true;
    e->server->ending++;
}

/* Datagrams of `seg` bytes (the last may be shorter) out in one call where the platform can. What
 * the socket will not take now is kept, and the socket watched until it will. */
static void e_send_batch(h3e_t *e, proven_net_addr_t to, const proven_byte_t *p, proven_size_t n, proven_size_t seg, proven_u8 ecn) {
    if (!e->open || e->pend_len > 0 || n == 0) return;
    proven_size_t sent = 0;
    const proven_err_t r = proven_net_udp_send_marked_(&e->udp, to, (proven_mem_view_t){ .ptr = p, .size = n }, seg, ecn, &sent);
    if (r != PROVEN_ERR_TIMEOUT || n - sent > sizeof e->pend) return;      /* sent, or lost as UDP may lose it */
    proven_net_mem_copy(e->pend, p + sent, n - sent);
    e->pend_len = n - sent;
    e->pend_seg = seg;
    e->pend_ecn = ecn;
    e->pend_to = to;
    x_rearm(e->x);
}

static void e_send(h3e_t *e, proven_net_addr_t to, const proven_byte_t *p, proven_size_t n) { e_send_batch(e, to, p, n, n, 0); }

static bool h3_addr_same(const proven_net_addr_t *a, const proven_net_addr_t *b) {
    if (a->family != b->family || a->port != b->port || a->scope_id != b->scope_id) return false;
    for (int i = 0; i < 16; ++i) if (a->ip[i] != b->ip[i]) return false;
    return true;
}

// -----------------------------------------------------------------------------
// Lifetimes
// -----------------------------------------------------------------------------

static void d_service(h3d_t *d);
static void d_arm(h3d_t *d);

static void d_enter(h3d_t *d) { d->depth++; }

static void d_leave(h3d_t *d) {
    if (--d->depth > 0 || !d->dead) return;
    proven_allocator_t a = d->server->alloc;
    a.free_fn(a.ctx, d);
}

static void s_enter(proven_h3s_t *s) { s->h.depth++; }

static void s_leave(proven_h3s_t *s) {
    if (--s->h.depth > 0 || !s->dead) return;
    proven_allocator_t a = s->h.server->alloc;
    a.free_fn(a.ctx, s);
}

static void d_unlist(h3d_t *d) {
    if (d->list == 0) return;
    h3d_t **head = d->list == 1 ? &d->x->dirty : &d->x->work;
    if (d->dprev) d->dprev->dnext = d->dnext;
    else *head = d->dnext;
    if (d->dnext) d->dnext->dprev = d->dprev;
    d->dprev = d->dnext = (void *)0;
    d->list = 0;
}

/* The connection is owed a round from the loop: events to read, datagrams to send. */
static void d_mark(h3d_t *d) {
    if (d->dead || d->list == 1) return;
    struct ev_h3 *x = d->x;
    const bool was_empty = !x->dirty;
    d_unlist(d);
    d->dnext = x->dirty;
    if (x->dirty) x->dirty->dprev = d;
    x->dirty = d;
    d->list = 1;
    if (was_empty) x_rearm(x);
}

/* The exchange is over: the handler is told, once. */
static void s_finish(proven_h3s_t *s, proven_err_t why) {
    if (s->dead) return;
    proven_http_event_server_t *sv = s->h.server;
    proven_allocator_t a = sv->alloc;
    h3d_t *d = s->d;
    s_enter(s);
    s->dead = true;
    if (s->prev) s->prev->next = s->next;
    else d->streams = s->next;
    if (s->next) s->next->prev = s->prev;
    d->stream_count--;
    if (s->zip) { proven_http_compressor_destroy_(s->zip); s->zip = (void *)0; }
    if (s->out) { a.free_fn(a.ctx, s->out); s->out = (void *)0; s->out_len = s->out_off = s->out_cap = 0; }
    if (s->active) {
        s->active = false;
        if (sv->cfg.on.on_done) sv->cfg.on.on_done(sv->cfg.ctx, (proven_http_stream_t *)(void *)s, why);
    }
    proven_http_trailers_free_(a, &s->in_trailers);
    proven_http_trailers_free_(a, &s->out_trailers);
    s_leave(s);
}

/* Give up on a stream from this side: the peer is told (`code`), the handler why. */
static void s_reset(proven_h3s_t *s, proven_u64 code, proven_err_t why) {
    if (s->dead) return;
    h3d_t *d = s->d;
    if (!d->dead && !d->closed && !s->engine_done) (void)proven_h3_stream_reset(d->h3, s->id, code);
    s->engine_done = true;
    s_finish(s, why);
}

static void d_end_streams(h3d_t *d, proven_err_t why) {
    while (d->streams) { d->streams->engine_done = true; s_finish(d->streams, why); }
}

/* The connection leaves the server: its streams end, its IDs go, its memory with the last entry. */
static void d_gone(h3d_t *d, proven_err_t why) {
    if (d->dead) return;
    proven_http_event_server_t *sv = d->server;
    struct ev_h3 *x = d->x;
    d_enter(d);
    d_end_streams(d, why);
    d->dead = true;
    d_unlist(d);
    proven_loop_timer_cancel(sv->loop, &d->timer);
    while (d->nkeys > 0) d_key_drop(d, &d->keys[d->nkeys - 1]);
    if (d->prev) d->prev->next = d->next;
    else x->conns = d->next;
    if (d->next) d->next->prev = d->prev;
    proven_h3_conn_destroy(d->h3);
    proven_quic_conn_destroy(d->q);
    d->h3 = (void *)0; d->q = (void *)0;
    sv->h3_count--;
    if (--d->e->users == 0 && d->e->closing) e_close(d->e);
    proven_http_event_gone_(sv);
    d_leave(d);
}

/* Whether the QUIC connection is over: its closing or draining period too (RFC 9000 section 10.2). */
static bool d_over(const h3d_t *d) {
    proven_quic_stats_t st;
    proven_quic_conn_stats(d->q, &st);
    return st.closed;
}

/* The datagrams the engine has for the peer, a round's worth at most - a CONNECTION_CLOSE among
 * them while it is closing. */
static void d_send(h3d_t *d) {
    if (d->dead) return;
    struct ev_h3 *x = d->x;
    /* Gathered while they are one size, go to one address and carry one ECN mark (RFC-0018 S-3,
     * RFC-0035): a different address or mark or a larger datagram (a PMTU probe) sends what is
     * gathered first; a shorter one ends the batch; so do the platform's limits and the end of
     * the round. */
    proven_size_t at = 0, seg = 0;
    int count = 0, k = 0;
    proven_net_addr_t bto = d->peer;
    proven_u8 bmark = 0;
    for (; k < H3_SENDS; ++k) {
        if (at == 0 && (d->e->pend_len > 0 || !d->e->open)) { d_mark(d); return; }
        if (H3_BATCH - at < H3_OUT) { e_send_batch(d->e, bto, x->batch, at, seg, bmark); at = 0; count = 0; --k; continue; }
        proven_size_t len = 0;
        proven_quic_addr_t to;
        proven_u8 mark = 0;
        if (proven_quic_conn_send(d->q, h3_now_us(), (proven_mem_mut_t){ .ptr = x->batch + at, .size = H3_OUT }, &len, &to, d->e->ecn ? &mark : NULL) != PROVEN_OK || len == 0) break;
        d->peer = h3_naddr(&to);
        if (at > 0 && (len > seg || mark != bmark || !h3_addr_same(&d->peer, &bto))) {
            e_send_batch(d->e, bto, x->batch, at, seg, bmark);
            /* The socket would not take all of it: this datagram is lost as UDP may lose it, and
             * nothing more is asked of the engine until the socket takes what is kept. */
            if (d->e->pend_len > 0) { d_mark(d); return; }
            proven_net_mem_move(x->batch, x->batch + at, len);
            at = 0; count = 0;
        }
        if (at == 0) { seg = len; bto = d->peer; bmark = mark; }
        at += len;
        if (len < seg || ++count == PROVEN_NET_UDP_BATCH_COUNT) { e_send_batch(d->e, bto, x->batch, at, seg, bmark); at = 0; count = 0; }
    }
    if (at > 0) e_send_batch(d->e, bto, x->batch, at, seg, bmark);
    if (k == H3_SENDS) d_mark(d);       /* more, next round: the other connections have theirs first */
}

/* What a handler's call made: sent now, and the events it causes read in a round of the loop's. */
static void d_after_call(h3d_t *d) {
    if (d->dead) return;
    d_send(d);
    d_mark(d);
}

/* Close the connection from this side: every exchange ends with `why`, the peer is told, and
 * the driver goes when the engine says the connection is over. */
static void d_close(h3d_t *d, proven_err_t why) {
    if (d->dead || d->closing) return;
    d->closing = true;
    d_end_streams(d, why);
    if (!d->closed) proven_quic_conn_close(d->q, true, PROVEN_H3_NO_ERROR);
    d_mark(d);
}

// -----------------------------------------------------------------------------
// A stream's output
// -----------------------------------------------------------------------------

static proven_size_t s_out_pending(const proven_h3s_t *s) { return s->out_len - s->out_off; }

/* What a stream has made, towards the engine; its end when all of it has gone. */
static void s_push(proven_h3s_t *s) {
    if (s->dead || !s->response_begun) return;
    h3d_t *d = s->d;
    proven_http_event_server_t *sv = s->h.server;
    s_enter(s);
    while (!s->dead && !s->blocked && s_out_pending(s) > 0) {
        proven_size_t taken = 0;
        const bool fin = s->end_asked && s->out_trailers.count == 0;     /* with trailers, they end it */
        const proven_err_t e = proven_h3_send_data(d->h3, s->id, (proven_mem_view_t){ .ptr = s->out + s->out_off, .size = s_out_pending(s) }, fin, &taken);
        if (e != PROVEN_OK) { s_reset(s, PROVEN_H3_INTERNAL_ERROR, PROVEN_ERR_RESET); break; }
        s->out_off += taken;
        if (taken > 0) d->write_since = 0;
        if (s_out_pending(s) > 0) { s->blocked = true; break; }
        if (fin) s->engine_done = true;
    }
    if (!s->dead && s_out_pending(s) == 0) {
        if (s->out) { sv->alloc.free_fn(sv->alloc.ctx, s->out); s->out = (void *)0; s->out_len = s->out_off = s->out_cap = 0; }
        if (s->end_asked) {
            proven_size_t taken = 0;
            const proven_err_t e = s->engine_done            ? PROVEN_OK
                                 : s->out_trailers.count ? proven_h3_send_trailers(d->h3, s->id, s->out_trailers.fields, s->out_trailers.count)
                                                         : proven_h3_send_data(d->h3, s->id, (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, true, &taken);
            s->engine_done = true;
            s_finish(s, e == PROVEN_OK ? PROVEN_OK : PROVEN_ERR_RESET);
        }
    }
    s_leave(s);
}

/* Handlers that were refused and may write now are told - only from a round, never from
 * inside a call of theirs. A callback may end any stream: the walk starts again after each. */
static void d_tell_writable(h3d_t *d) {
    proven_http_event_server_t *sv = d->server;
    for (bool again = true; again && !d->dead;) {
        again = false;
        for (proven_h3s_t *s = d->streams; s; s = s->next) {
            if (s->dead || !s->want_writable || s->blocked || s_out_pending(s) > 0 || !s->response_begun || s->response_ended) continue;
            s->want_writable = false;
            if (!sv->cfg.on.on_writable) continue;
            s_enter(s);
            sv->cfg.on.on_writable(sv->cfg.ctx, (proven_http_stream_t *)(void *)s);
            s_leave(s);
            again = true;
            break;
        }
    }
}

// -----------------------------------------------------------------------------
// Requests
// -----------------------------------------------------------------------------

static proven_h3s_t *d_stream(const h3d_t *d, proven_u64 id) {
    for (proven_h3s_t *s = d->streams; s; s = s->next) if (s->id == id) return s;
    return (void *)0;
}

/* A request the server will not pass on: a status and nothing else. */
static void d_refuse(h3d_t *d, proven_u64 id, const char status[4]) {
    proven_http_header_t h[1] = { { .name = PROVEN_LIT(":status"), .value = proven_u8str_view_from_bytes(status, 3) } };
    if (proven_h3_submit_response(d->h3, id, h, 1, true) != PROVEN_OK) (void)proven_h3_stream_reset(d->h3, id, PROVEN_H3_REQUEST_REJECTED);
}

static void s_expect_body(proven_h3s_t *s) {
    s->body_due = s->in_done || s->paused ? 0 : proven_time_monotonic_now() + (proven_time_t)s->h.server->cfg.body_timeout_ms * 1000000;
}

static void d_request(h3d_t *d, const proven_h3_event_t *ev) {
    proven_http_event_server_t *sv = d->server;
    const proven_h2_request_head_t *rq = &ev->request;
    const bool connect = rq->path.size == 0 && rq->scheme.size == 0;
    /* The fields are copied into the server's array before the handler sees them, as for HTTP/2. */
    bool has_host = false, has_length = false, bad = false;
    proven_u64 length = 0;
    for (proven_size_t i = 0; i < rq->field_count; ++i) {
        const proven_http_header_t *f = &rq->fields[i];
        if (proven_u8str_view_eq_ascii_ci(f->name, PROVEN_LIT("host"))) {
            /* Two different targets in one request (RFC 9114 section 4.3.1). */
            if (has_host || (rq->authority.size > 0 && !proven_u8str_view_eq_ascii_ci(f->value, rq->authority))) bad = true;
            has_host = true;
        } else if (proven_u8str_view_eq_ascii_ci(f->name, PROVEN_LIT("content-length"))) {
            has_length = true;
            length = 0;
            for (proven_size_t k = 0; k < f->value.size && length <= sv->cfg.max_body_bytes; ++k) length = length * 10 + (proven_u64)(f->value.ptr[k] - '0');
        }
    }
    d->asked = true;
    if (bad) { (void)proven_h3_stream_reset(d->h3, ev->stream_id, PROVEN_H3_MESSAGE_ERROR); return; }
    /* Early data (RFC-0021): before the handshake is complete a request may be a replayed copy.
     * Only what is safe to repeat reaches the handler; the rest is told 425 (RFC 8470), which the
     * client answers by sending it again after the handshake. */
    const bool early = !proven_quic_conn_complete_(d->q);
    if (early) {
        const proven_http_method_t m = proven_http_method_from_text(rq->method);
        const bool safe = (m == PROVEN_HTTP_GET || m == PROVEN_HTTP_HEAD || m == PROVEN_HTTP_OPTIONS) && ev->end_stream;
        if (!safe) { d_refuse(d, ev->stream_id, "425"); return; }
    }
    const proven_size_t add_host = !has_host && rq->authority.size > 0 ? 1 : 0;
    /* A body of unannounced length is given the field that means it, as over HTTP/2. */
    const proven_size_t add_chunked = !ev->end_stream && !has_length ? 1 : 0;
    if (rq->field_count + add_host + add_chunked > sv->cfg.max_headers) { d_refuse(d, ev->stream_id, "431"); return; }
    if (length > sv->cfg.max_body_bytes) { d_refuse(d, ev->stream_id, "413"); return; }

    proven_h3s_t *s = h3_alloc(sv->alloc, sizeof *s);
    if (!s) { d_refuse(d, ev->stream_id, "503"); return; }
    proven_size_t n = 0;
    if (add_host) sv->headers[n++] = (proven_http_header_t){ .name = PROVEN_LIT("host"), .value = rq->authority };
    for (proven_size_t i = 0; i < rq->field_count; ++i) sv->headers[n++] = rq->fields[i];
    if (add_chunked) sv->headers[n++] = (proven_http_header_t){ .name = PROVEN_LIT("transfer-encoding"), .value = PROVEN_LIT("chunked") };
    proven_http_request_t req = {
        .method = proven_http_method_from_text(rq->method), .method_text = rq->method,
        .target = connect ? rq->authority : rq->path,
        .version_minor = 1,
        .headers = sv->headers, .header_count = n,
    };
    s->h = (proven_ev_head_t){ .server = sv, .version_minor = 1, .kind = EV_KIND_H3 };
    s->d = d;
    s->id = ev->stream_id;
    s->method = req.method;
    s->response_left = PROVEN_HTTP_EVENT_LENGTH_UNKNOWN;
    s->compress_coding = (proven_u8)proven_http_compress_choose_(sv->headers, n, sv->cfg.compress_zstd);
    s->in_done = ev->end_stream;
    s->early = early;
    s_expect_body(s);
    s->active = true;
    s->next = d->streams;
    if (d->streams) d->streams->prev = s;
    d->streams = s;
    d->stream_count++;
    const bool expects = !ev->end_stream && proven_http_header_has_token(sv->headers, n, PROVEN_LIT("expect"), PROVEN_LIT("100-continue"));

    s_enter(s);
    d->calls++;                                    /* what the handler does in here is sent when it returns */
    proven_http_event_request_begin_(sv);
    sv->cfg.on.on_request(sv->cfg.ctx, (proven_http_stream_t *)(void *)s, &req);
    proven_http_event_request_end_(sv);
    d->calls--;
    if (!s->dead && !d->dead && expects && !s->response_begun) {
        proven_http_header_t go_on[1] = { { .name = PROVEN_LIT(":status"), .value = PROVEN_LIT("100") } };
        (void)proven_h3_submit_response(d->h3, s->id, go_on, 1, false);
    }
    s_leave(s);
}

static void d_body(h3d_t *d, proven_h3s_t *s, proven_mem_view_t piece, bool last) {
    proven_http_event_server_t *sv = d->server;
    s->body_seen += piece.size;
    if (s->body_seen > sv->cfg.max_body_bytes) {
        if (!s->response_begun) {
            /* A complete answer to an unfinished request tells the client to stop sending. */
            proven_http_header_t h[1] = { { .name = PROVEN_LIT(":status"), .value = PROVEN_LIT("413") } };
            if (proven_h3_submit_response(d->h3, s->id, h, 1, true) == PROVEN_OK) s->engine_done = true;
        }
        s_reset(s, PROVEN_H3_REQUEST_CANCELLED, PROVEN_ERR_OUT_OF_BOUNDS);
        return;
    }
    if (last) s->in_done = true;
    s_expect_body(s);
    if (!(piece.size > 0 || last) || !sv->cfg.on.on_body) return;
    s_enter(s);
    d->calls++;
    sv->cfg.on.on_body(sv->cfg.ctx, (proven_http_stream_t *)(void *)s, piece, last);
    d->calls--;
    s_leave(s);
}

static void d_event(h3d_t *d, const proven_h3_event_t *ev) {
    proven_h3s_t *s = ev->kind == PROVEN_H3_EV_HEADERS || ev->kind == PROVEN_H3_EV_QUIC || ev->kind == PROVEN_H3_EV_CLOSED || ev->kind == PROVEN_H3_EV_GOAWAY
             ? (void *)0 : d_stream(d, ev->stream_id);
    switch (ev->kind) {
        case PROVEN_H3_EV_HEADERS: d_request(d, ev); break;
        case PROVEN_H3_EV_DATA: if (s) d_body(d, s, ev->data, ev->end_stream); break;
        case PROVEN_H3_EV_TRAILERS:                /* kept for proven_http_stream_trailers, then the body's end */
            if (s && proven_http_trailers_keep_(d->server->alloc, &s->in_trailers, ev->fields, ev->field_count, true, false) != PROVEN_OK) {
                s_reset(s, PROVEN_H3_INTERNAL_ERROR, PROVEN_ERR_NOMEM);
                break;
            }
            if (s) d_body(d, s, (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, true);
            break;
        case PROVEN_H3_EV_STREAM_RESET: if (s) { s->engine_done = true; s_finish(s, PROVEN_ERR_RESET); } break;
        case PROVEN_H3_EV_STOP_SENDING:
            /* The client wants no more of the response: what is left of it cannot go. */
            if (s) s_reset(s, PROVEN_H3_REQUEST_CANCELLED, PROVEN_ERR_RESET);
            break;
        case PROVEN_H3_EV_STREAM_WRITABLE:
            if (s) { s->blocked = false; d->write_since = 0; s_push(s); }
            break;
        case PROVEN_H3_EV_QUIC:
            if (ev->quic.kind == PROVEN_QUIC_EV_CID_NEW) (void)d_key_add(d, &ev->quic.cid);
            else if (ev->quic.kind == PROVEN_QUIC_EV_CID_RETIRED) d_key_drop(d, &ev->quic.cid);
            break;
        case PROVEN_H3_EV_CLOSED:
            d->closed = true;
            d_end_streams(d, PROVEN_ERR_RESET);
            break;
        default: break;                            /* GOAWAY from a client: it will ask nothing more, which changes nothing here */
    }
}

// -----------------------------------------------------------------------------
// Rounds and time
// -----------------------------------------------------------------------------

/* What a round does: the server's times and the engine's timer if they are due, its events, the
 * handlers told who may write, and the datagrams that makes. */
static void d_check_time(h3d_t *d);

static void d_service(h3d_t *d) {
    if (d->dead) return;
    d_enter(d);
    d_check_time(d);
    const proven_u64 now = h3_now_us();
    if (proven_quic_conn_deadline(d->q) <= now) proven_quic_conn_timeout(d->q, now);
    proven_h3_event_t ev;
    while (!d->dead && !d->closed && proven_h3_conn_poll(d->h3, &ev)) d_event(d, &ev);
    if (!d->dead && !d->closed) d_tell_writable(d);
    if (!d->dead) d_send(d);
    /* What HTTP/3 reports as closed is still answered while QUIC's closing period lasts: a
     * datagram from the peer then gets the CONNECTION_CLOSE again, not a stateless reset. */
    if (!d->dead && d->closed && d_over(d)) d_gone(d, PROVEN_ERR_RESET);
    else if (!d->dead) d_arm(d);
    d_leave(d);
}

/* Every connection owed a round gets one. Those that ask again while it goes wait for the next. */
static void x_rounds(struct ev_h3 *x) {
    x->work = x->dirty;
    x->dirty = (void *)0;
    for (h3d_t *d = x->work; d; d = d->dnext) d->list = 2;
    while (x->work) {
        h3d_t *d = x->work;
        d_unlist(d);
        d_service(d);
    }
    x_rearm(x);
}

static void d_on_timer(void *ctx);

static proven_time_t t_min(proven_time_t a, proven_time_t b) { return a == 0 ? b : b == 0 ? a : a < b ? a : b; }

/* One timer for the engine's deadline and the server's own: a request that does not come, a
 * connection with nothing on it, output that does not move, a request body that stops, and the
 * second goodbye. */
static void d_arm(h3d_t *d) {
    if (d->dead) return;
    const proven_http_event_server_config_t *cfg = &d->server->cfg;
    const proven_time_t now = proven_time_monotonic_now();
    proven_time_t due = 0;
    if (!d->closing && !d->closed) {
        if (!d->asked) due = d->born + (proven_time_t)cfg->head_timeout_ms * 1000000;
        if (d->asked && d->stream_count == 0 && d->bye == 0) {
            if (d->idle_since == 0) d->idle_since = now;
            due = t_min(due, d->idle_since + (proven_time_t)cfg->idle_timeout_ms * 1000000);
        } else {
            d->idle_since = 0;
        }
        bool waiting = false;
        for (const proven_h3s_t *s = d->streams; s; s = s->next) {
            if (s->blocked || s_out_pending(s) > 0) waiting = true;
            if (s->body_due != 0) due = t_min(due, s->body_due);
        }
        if (!waiting) d->write_since = 0;
        else if (d->write_since == 0) d->write_since = now;
        if (d->write_since != 0) due = t_min(due, d->write_since + (proven_time_t)cfg->write_timeout_ms * 1000000);
        if (d->bye == 1) due = t_min(due, d->bye_due);
    }
    const proven_u64 q = proven_quic_conn_deadline(d->q);
    if (q != UINT64_MAX) due = t_min(due, (proven_time_t)(q * 1000u));
    if (due == 0) { proven_loop_timer_cancel(d->server->loop, &d->timer); return; }
    const proven_time_t left = due > now ? due - now : 0;
    proven_loop_timer_set(d->server->loop, &d->timer, (proven_u32)((left + 999999) / 1000000), d_on_timer, d);
}

/* The last goodbye: the requests seen so far are answered, and the connection closes after them. */
static void d_bye_last(h3d_t *d) {
    if (d->bye == 2 || d->closing) return;
    d->bye = 2;
    if (proven_h3_conn_goaway_step(d->h3, false) != PROVEN_OK) d_close(d, PROVEN_ERR_RESET);
}

/* The first goodbye of two: nothing new after the requests already on their way, which a round
 * trip brings (RFC 9114 section 5.2). A connection whose handshake is not over has nothing to say
 * it on: it is closed. */
static void d_bye(h3d_t *d) {
    if (d->dead || d->closing || d->bye != 0) return;
    if (proven_h3_conn_goaway_step(d->h3, true) != PROVEN_OK) { d_close(d, PROVEN_ERR_RESET); return; }
    d->bye = 1;
    proven_quic_stats_t st;
    proven_quic_conn_stats(d->q, &st);
    const proven_u64 rtt_us = st.srtt_us > 0 ? st.srtt_us : 333000;      /* RFC 9002's first estimate */
    d->bye_due = proven_time_monotonic_now() + (proven_time_t)(rtt_us * 1000u);
    d_mark(d);
}

/* What the server's own clock has made due: a request body that stopped, a request that never
 * came, output that does not move, the second goodbye, a connection idle too long. Looked at in
 * every round, not only when the timer fires: a busy connection's rounds would otherwise move its
 * timer on for ever. */
static void d_check_time(h3d_t *d) {
    if (d->dead || d->closing || d->closed) return;
    const proven_http_event_server_config_t *cfg = &d->server->cfg;
    const proven_time_t now = proven_time_monotonic_now();
    for (bool again = true; again && !d->dead;) {
        again = false;
        for (proven_h3s_t *s = d->streams; s; s = s->next) {
            if (s->body_due == 0 || s->body_due > now) continue;
            s_reset(s, PROVEN_H3_REQUEST_CANCELLED, PROVEN_ERR_TIMEOUT);
            again = true;
            break;
        }
    }
    if (d->dead) return;
    if (!d->asked && now >= d->born + (proven_time_t)cfg->head_timeout_ms * 1000000) d_close(d, PROVEN_ERR_TIMEOUT);
    else if (d->write_since != 0 && now >= d->write_since + (proven_time_t)cfg->write_timeout_ms * 1000000) d_close(d, PROVEN_ERR_TIMEOUT);
    else if (d->bye == 1 && now >= d->bye_due) d_bye_last(d);
    else if (d->bye == 0 && d->idle_since != 0 && d->stream_count == 0 && now >= d->idle_since + (proven_time_t)cfg->idle_timeout_ms * 1000000) {
        d->bye = 1;                                /* idle: nothing is on its way that a first goodbye would spare */
        d_bye_last(d);
    }
}

static void d_on_timer(void *ctx) {
    h3d_t *d = ctx;
    if (d->dead) return;
    d_enter(d);
    d_service(d);
    d_leave(d);
}

// -----------------------------------------------------------------------------
// Datagrams in
// -----------------------------------------------------------------------------

/* A client's first packet that the stateless check let through: a connection. */
static void x_accept(struct ev_h3 *x, h3e_t *e, const proven_quic_accept_t *acc, proven_mem_view_t dg, const proven_quic_cid_t *dcid,
                     const proven_net_addr_t *from) {
    proven_http_event_server_t *sv = x->server;
    if (x->stopped || e->closing || sv->stopped || sv->closed_to_new || sv->conn_count + sv->h3_count >= sv->cfg.max_connections) return;
    h3d_t *d = h3_alloc(sv->alloc, sizeof *d);
    if (!d) return;
    const proven_quic_addr_t qa = h3_qaddr(from);
    if (proven_quic_conn_server_create(&x->qc, acc, dg, &qa, e->rx_ecn, h3_now_us(), &d->q) != PROVEN_OK) { sv->alloc.free_fn(sv->alloc.ctx, d); return; }
    const proven_u32 list = sv->cfg.max_head_bytes > ((proven_size_t)1 << 20) ? (proven_u32)1 << 20 : (proven_u32)sv->cfg.max_head_bytes;
    const proven_h3_config_t hc = { .alloc = sv->alloc, .server = true, .max_field_section_bytes = list, .qpack_capacity = (proven_u32)sv->cfg.http3_header_table_bytes };
    if (proven_h3_conn_create(&hc, d->q, &d->h3) != PROVEN_OK) {
        proven_quic_conn_destroy(d->q);
        sv->alloc.free_fn(sv->alloc.ctx, d);
        return;
    }
    d->server = sv;
    d->x = x;
    d->e = e;
    d->peer = *from;
    d->born = proven_time_monotonic_now();
    if (!d_key_add(d, dcid)) {
        proven_h3_conn_destroy(d->h3);
        proven_quic_conn_destroy(d->q);
        sv->alloc.free_fn(sv->alloc.ctx, d);
        return;
    }
    d->next = x->conns;
    if (x->conns) x->conns->prev = d;
    x->conns = d;
    e->users++;
    sv->h3_count++;
    proven_http_event_gone_(sv);                  /* the TCP listeners stop at `max_connections` too */
    d_mark(d);                                     /* its IDs, and the server's first flight */
}

static void x_datagram(struct ev_h3 *x, h3e_t *e, proven_mem_view_t dg, const proven_net_addr_t *from) {
    proven_quic_hdr_t hd;
    if (!proven_quic_hdr_parse(dg, 8, &hd)) return;
    h3d_t *d = key_find(x, &hd.dcid);
    if (d) {
        if (d->dead) return;
        const proven_quic_addr_t qa = h3_qaddr(from);
        if (proven_quic_conn_receive(d->q, dg, &qa, e->rx_ecn, h3_now_us()) != PROVEN_OK) { d_close(d, PROVEN_ERR_NOMEM); return; }
        d_mark(d);
        return;
    }
    proven_size_t n = 0;
    if (dg.ptr[0] & 0x80) {
        proven_quic_accept_t acc;
        const proven_quic_addr_t qa = h3_qaddr(from);
        const proven_quic_stateless_t what = proven_quic_server_stateless(&x->qc, dg, &qa, h3_now_us(), x->server->cfg.http3_retry,
                                                                          (proven_mem_mut_t){ .ptr = x->out, .size = sizeof x->out }, &n, &acc);
        if (what == PROVEN_QUIC_STATELESS_REPLY && n > 0) e_send(e, *from, x->out, n);
        else if (what == PROVEN_QUIC_STATELESS_ACCEPT) x_accept(x, e, &acc, dg, &hd.dcid, from);
        return;
    }
    /* A short header for a connection this server does not know (any more): a stateless reset,
     * smaller than what came, so that two servers cannot keep one going between them. Not for an
     * ID another loop of the port issued: one that came here while the group changed, whose
     * connection may be alive there (RFC-0037). */
    if (x->qc.cid_loops > 1 && hd.dcid.len > 0 && hd.dcid.id[0] % x->qc.cid_loops != x->qc.cid_loop) return;
    n = proven_quic_stateless_reset(&x->qc, &hd.dcid, dg.size, (proven_mem_mut_t){ .ptr = x->out, .size = sizeof x->out });
    if (n > 0) e_send(e, *from, x->out, n);
}

static void e_received(void *ctx, proven_err_t err, proven_size_t n, proven_size_t seg, proven_u8 ecn) {
    h3e_t *e = ctx;
    if (err != PROVEN_OK || !e->open) return;
    struct ev_h3 *x = e->x;
    e->rx_ecn = ecn;
    /* Several datagrams of one sender coalesced by the system (GRO): each as if alone. */
    const proven_size_t step = seg ? seg : n;
    for (proven_size_t off = 0; off < n && e->open; off += step)
        x_datagram(x, e, (proven_mem_view_t){ .ptr = e->rbuf + off, .size = n - off < step ? n - off : step }, &e->from);
    /* The next receive: armed from here, the loop takes what waits already without a new round.
     * The connections it touched get their round once the datagrams that came together are in. */
    if (e->open) (void)proven_loop_udp_recv_(x->server->loop, &e->lu, (proven_mem_mut_t){ .ptr = e->rbuf, .size = H3_DATAGRAM }, &e->from);
    proven_loop_after_(x->server->loop, &x->after, x_after, x);
}

static void e_writable(void *ctx, proven_err_t err) {
    h3e_t *e = ctx;
    e->waiting = false;
    if (err != PROVEN_OK || !e->open) return;
    if (e->pend_len > 0) {
        proven_size_t sent = 0;
        const proven_err_t r = proven_net_udp_send_marked_(&e->udp, e->pend_to, (proven_mem_view_t){ .ptr = e->pend, .size = e->pend_len }, e->pend_seg, e->pend_ecn, &sent);
        if (r != PROVEN_ERR_TIMEOUT) e->pend_len = 0;
        else if (sent > 0) { proven_net_mem_move(e->pend, e->pend + sent, e->pend_len - sent); e->pend_len -= sent; }
    }
    x_rounds(e->x);
}

static void e_closed(void *ctx) {
    h3e_t *e = ctx;
    proven_http_event_server_t *sv = e->server;
    e->sock_closing = false;
    if (e->orphan) {
        sv->alloc.free_fn(sv->alloc.ctx, e->rbuf);
        sv->alloc.free_fn(sv->alloc.ctx, e);
    }
    proven_http_event_ended_(sv);
}

static const proven_loop_udp_fns_t e_fns = { .received = e_received, .writable = e_writable, .closed = e_closed };

// -----------------------------------------------------------------------------
// What http_event.c asks of HTTP/3
// -----------------------------------------------------------------------------

proven_err_t proven_http_event_h3_create_(proven_http_event_server_t *sv) {
    struct ev_h3 *x = h3_alloc(sv->alloc, sizeof *x);
    if (!x) return PROVEN_ERR_NOMEM;
    x->batch = h3_alloc(sv->alloc, H3_BATCH);
    if (!x->batch) { sv->alloc.free_fn(sv->alloc.ctx, x); return PROVEN_ERR_NOMEM; }
    x->server = sv;
    const proven_u32 streams = sv->cfg.max_concurrent_streams > 100000 ? 100000 : (proven_u32)sv->cfg.max_concurrent_streams;
    x->qc = (proven_quic_config_t){
        .alloc = sv->alloc, .tls = sv->cfg.tls,
        .alpn = { .ptr = (const proven_byte_t *)"h3", .size = 2 },   /* whatever else the configuration offers over TCP (RFC 9114 section 3.1) */
        .max_streams_bidi = streams,
        .max_streams_uni = 16,                     /* a client's control and QPACK streams, and a few it may add */
        .stream_window = (proven_u32)sv->cfg.http3_window_bytes,
        .conn_window = (proven_u32)sv->cfg.http3_window_bytes,
        /* No window configured: it starts at the engine's and grows while bodies are read (RFC-0019 section 9). */
        .stream_window_max = sv->cfg.http3_window_bytes ? 0u : PROVEN_QUIC_STREAM_WINDOW_MAX,
        .conn_window_max = sv->cfg.http3_window_bytes ? 0u : PROVEN_QUIC_CONN_WINDOW_MAX,
        /* QUIC's own idle timeout is for a peer that has gone silent; the server's timeouts end a
         * connection politely first, with a goodbye or a close it can read. */
        .idle_timeout_ms = (sv->cfg.idle_timeout_ms > sv->cfg.head_timeout_ms ? sv->cfg.idle_timeout_ms : sv->cfg.head_timeout_ms) + 1000,
    };
    /* One key for the server's life: Retry tokens and stateless resets of every connection. */
    proven_tls_config_random(sv->cfg.tls, x->qc.static_key, sizeof x->qc.static_key);
    sv->h3 = x;
    return PROVEN_OK;
}

static proven_err_t e_start(proven_http_event_server_t *sv, h3e_t *e, proven_net_addr_t got);

proven_err_t proven_http_event_h3_listen_(proven_http_event_server_t *sv, proven_net_addr_t at) {
    struct ev_h3 *x = sv->h3;
    /* A socket kept for connections after stop_listening serves again, rather than a second
     * one being refused its address. */
    for (proven_size_t i = 0; i < H3_ENDS; ++i) {
        h3e_t *e = x->ends[i];
        if (!e || !e->open || !e->closing || e->bound.family != at.family || e->bound.port != at.port) continue;
        bool same = true;
        for (int k = 0; k < 16; ++k) same = same && e->bound.ip[k] == at.ip[k];
        if (!same) continue;
        e->closing = false;
        x->stopped = false;
        return PROVEN_OK;
    }
    proven_size_t slot = H3_ENDS;
    /* (Not a slot whose socket the loop has not finished with: its receive buffer may still be
     * the loop's.) */
    for (proven_size_t i = 0; i < H3_ENDS && slot == H3_ENDS; ++i) if (!x->ends[i] || (!x->ends[i]->open && !x->ends[i]->sock_closing)) slot = i;
    if (slot == H3_ENDS) return PROVEN_ERR_OUT_OF_BOUNDS;
    h3e_t *e = x->ends[slot];
    if (!e) {
        e = h3_alloc(sv->alloc, sizeof *e);
        if (!e) return PROVEN_ERR_NOMEM;
        e->rbuf = h3_alloc(sv->alloc, H3_DATAGRAM);
        if (!e->rbuf) { sv->alloc.free_fn(sv->alloc.ctx, e); return PROVEN_ERR_NOMEM; }
        x->ends[slot] = e;
    }
    proven_byte_t *rbuf = e->rbuf;
    *e = (h3e_t){ .x = x, .server = sv, .rbuf = rbuf };
    proven_net_addr_t got;
    proven_err_t r = proven_net_udp_open(at, &e->udp, &got);
    if (r != PROVEN_OK) return r;
    return e_start(sv, e, got);
}

proven_err_t proven_http_event_h3_listen_port_(proven_http_event_server_t *sv, proven_net_udp_t *udp, proven_net_addr_t at,
                                               proven_size_t loops, proven_size_t index, const proven_byte_t key[32]) {
    struct ev_h3 *x = sv->h3;
    /* Only a server that has never listened: its connection IDs and its key are the group's from
     * the first connection on. */
    for (proven_size_t i = 0; i < H3_ENDS; ++i) if (x->ends[i]) return PROVEN_ERR_INVALID_STATE;
    h3e_t *e = h3_alloc(sv->alloc, sizeof *e);
    if (!e) return PROVEN_ERR_NOMEM;
    e->rbuf = h3_alloc(sv->alloc, H3_DATAGRAM);
    if (!e->rbuf) { sv->alloc.free_fn(sv->alloc.ctx, e); return PROVEN_ERR_NOMEM; }
    x->ends[0] = e;
    proven_byte_t *rbuf = e->rbuf;
    *e = (h3e_t){ .x = x, .server = sv, .rbuf = rbuf, .udp = *udp };
    *udp = (proven_net_udp_t){0};                  /* the server's now */
    for (int i = 0; i < 32; ++i) x->qc.static_key[i] = key[i];
    x->qc.cid_loops = (proven_u8)loops;
    x->qc.cid_loop = (proven_u8)index;
    return e_start(sv, e, at);
}

/* A socket opened for `e`: what it is asked for, and the loop told of it. */
static proven_err_t e_start(proven_http_event_server_t *sv, h3e_t *e, proven_net_addr_t got) {
    struct ev_h3 *x = sv->h3;
    proven_err_t r;
    (void)proven_net_udp_gro_(&e->udp);
    (void)proven_net_udp_dontfrag_(&e->udp);
    e->ecn = proven_net_udp_ecn_(&e->udp);
    r = proven_loop_udp_attach_(sv->loop, &e->lu, &e->udp, &e_fns, e);
    if (r == PROVEN_OK) r = proven_loop_udp_recv_(sv->loop, &e->lu, (proven_mem_mut_t){ .ptr = e->rbuf, .size = H3_DATAGRAM }, &e->from);
    if (r != PROVEN_OK) {
        if (r != PROVEN_ERR_INVALID_ARG) { e->open = true; e_close(e); }       /* attached: the loop answers `closed` */
        else (void)proven_net_udp_close(&e->udp);
        return r;
    }
    e->bound = got;
    e->open = true;
    x->stopped = false;
    x_rearm(x);
    return PROVEN_OK;
}

void proven_http_event_h3_stop_(proven_http_event_server_t *sv) {
    struct ev_h3 *x = sv->h3;
    if (!x) return;
    x->stopped = true;
    for (proven_size_t i = 0; i < H3_ENDS; ++i) {
        h3e_t *e = x->ends[i];
        if (!e || !e->open) continue;
        e->closing = true;
        if (e->users == 0) e_close(e);
    }
    /* Each connection says goodbye; saying it runs no handler, so one walk does them all. */
    for (h3d_t *d = x->conns; d; d = d->next) d_bye(d);
    x_rearm(x);
}

void proven_http_event_h3_end_all_(proven_http_event_server_t *sv) {
    struct ev_h3 *x = sv->h3;
    if (!x) return;
    while (x->conns) {
        h3d_t *d = x->conns;
        d_enter(d);
        /* The peer is told, once, and the connection goes without waiting for its answer. */
        d->closing = true;
        d_end_streams(d, PROVEN_ERR_RESET);
        if (!d->dead) { proven_quic_conn_close(d->q, true, PROVEN_H3_NO_ERROR); d_send(d); }
        d_gone(d, PROVEN_ERR_RESET);
        d_leave(d);
    }
}

void proven_http_event_h3_destroy_(proven_http_event_server_t *sv) {
    struct ev_h3 *x = sv->h3;
    if (!x) return;
    proven_http_event_h3_end_all_(sv);
    proven_allocator_t a = sv->alloc;
    proven_loop_after_cancel_(sv->loop, &x->after);
    for (proven_size_t i = 0; i < H3_ENDS; ++i) {
        h3e_t *e = x->ends[i];
        if (!e) continue;
        e_close(e);
        if (e->sock_closing) { e->orphan = true; continue; }     /* the loop's `closed` frees it */
        a.free_fn(a.ctx, e->rbuf);
        a.free_fn(a.ctx, e);
    }
    if (x->keys) a.free_fn(a.ctx, x->keys);
    a.free_fn(a.ctx, x->batch);
    proven_mem_wipe((proven_mem_mut_t){ .ptr = x->qc.static_key, .size = sizeof x->qc.static_key });
    a.free_fn(a.ctx, x);
    sv->h3 = (void *)0;
}

proven_u16 proven_http_event_h3_port_(const proven_http_event_server_t *sv, proven_u16 local_port) {
    const struct ev_h3 *x = sv->h3;
    if (!x || x->stopped) return 0;
    proven_u16 any = 0;
    for (proven_size_t i = 0; i < H3_ENDS; ++i) {
        const h3e_t *e = x->ends[i];
        if (!e || !e->open || e->closing) continue;
        if (e->bound.port == local_port) return local_port;
        if (any == 0) any = e->bound.port;
    }
    return any;
}

proven_size_t proven_http_event_alt_svc_(const proven_http_event_server_t *sv, proven_u16 local_port, const proven_http_header_t *headers,
                                         proven_size_t header_count, proven_byte_t out[16]) {
    for (proven_size_t i = 0; i < header_count && headers; ++i) if (proven_u8str_view_eq_ascii_ci(headers[i].name, PROVEN_LIT("alt-svc"))) return 0;
    proven_u16 port = proven_http_event_h3_port_(sv, local_port);
    if (port == 0) return 0;
    proven_byte_t digits[5];
    proven_size_t nd = 0, n = 0;
    do { digits[nd++] = (proven_byte_t)('0' + port % 10); port /= 10; } while (port > 0);
    const char *head = "h3=\":";
    for (; head[n]; ++n) out[n] = (proven_byte_t)head[n];
    while (nd > 0) out[n++] = digits[--nd];
    out[n++] = '"';
    return n;
}

// -----------------------------------------------------------------------------
// What a handler calls, for a stream of an HTTP/3 connection
// -----------------------------------------------------------------------------

static proven_h3s_t *h3s_of(proven_http_stream_t *stream) { return (proven_h3s_t *)(void *)stream; }
static const proven_h3s_t *h3s_of_const(const proven_http_stream_t *stream) { return (const proven_h3s_t *)(const void *)stream; }

/* A call of a handler's on a stream: counted, so that nothing of the round runs inside it, and
 * what it made sent when it is done. */
static void call_begin(proven_h3s_t *s) { s_enter(s); d_enter(s->d); s->d->calls++; }
/* True when the stream is still there after the call's sending, which can end it (on_done has
 * then been called): read before the leave, which may free it. */
static bool call_end(proven_h3s_t *s) {
    h3d_t *d = s->d;
    d->calls--;
    d_after_call(d);
    const bool alive = !s->dead;
    d_leave(d);
    s_leave(s);
    return alive;
}

void proven_http_event_h3_compress_(proven_http_stream_t *stream) {
    proven_h3s_t *s = h3s_of(stream);
    if (!s->dead && s->active && !s->response_begun) s->compress_asked = true;
}

static bool h3_is_driver_header(proven_u8str_view_t name) {
    return proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("content-length")) || proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("transfer-encoding")) ||
           proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("connection")) || proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("date"));
}

/* Fields of an HTTP/1.1 connection: not sent over HTTP/3 (RFC 9114 section 4.2), rather than
 * the response refused for them. */
static bool h3_is_hop_header(proven_u8str_view_t name) {
    return proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("keep-alive")) || proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("proxy-connection")) || proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("upgrade"));
}

static proven_err_t s_head(proven_h3s_t *s, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count,
                           proven_u64 length, bool vary, proven_http_coding_t coding, bool end_stream) {
    proven_http_event_server_t *sv = s->h.server;
    h3d_t *d = s->d;
    if (status < 200 || status > 999 || (header_count > 0 && !headers)) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < header_count; ++i) if (h3_is_driver_header(headers[i].name)) return PROVEN_ERR_INVALID_ARG;
    if (d->dead || d->closed || d->closing) { s_finish(s, PROVEN_ERR_RESET); return PROVEN_ERR_RESET; }
    proven_http_header_t small[40], *list = small;
    if (header_count + 5 > sizeof small / sizeof small[0]) {
        list = h3_alloc(sv->alloc, (header_count + 5) * sizeof *list);
        if (!list) return PROVEN_ERR_NOMEM;
    }
    proven_byte_t code[3] = { (proven_byte_t)('0' + status / 100), (proven_byte_t)('0' + status / 10 % 10), (proven_byte_t)('0' + status % 10) };
    proven_byte_t date[PROVEN_HTTP_DATE_SIZE], digits[20];
    proven_size_t n = 0, at = 0, nd = 0;
    proven_err_t e = PROVEN_OK;
    list[n++] = (proven_http_header_t){ .name = PROVEN_LIT(":status"), .value = { .ptr = code, .size = 3 } };
    if (proven_http_date_format(proven_time_now(), date) == PROVEN_OK) list[n++] = (proven_http_header_t){ .name = PROVEN_LIT("date"), .value = { .ptr = date, .size = sizeof date } };
    for (proven_size_t i = 0; i < header_count && e == PROVEN_OK; ++i) {
        if (h3_is_hop_header(headers[i].name)) continue;
        if (at + headers[i].name.size > sv->cfg.max_head_bytes) { e = PROVEN_ERR_OUT_OF_BOUNDS; break; }
        for (proven_size_t k = 0; k < headers[i].name.size; ++k) sv->hbuf[at + k] = proven_ascii_lower(headers[i].name.ptr[k]);
        list[n++] = (proven_http_header_t){ .name = { .ptr = sv->hbuf + at, .size = headers[i].name.size }, .value = headers[i].value };
        at += headers[i].name.size;
    }
    const bool no_body_status = status == 204 || status == 304;
    if (e == PROVEN_OK) {
        if (vary) list[n++] = (proven_http_header_t){ .name = PROVEN_LIT("vary"), .value = PROVEN_LIT("accept-encoding") };
        if (coding != PROVEN_HTTP_CODING_IDENTITY) list[n++] = (proven_http_header_t){ .name = PROVEN_LIT("content-encoding"), .value = proven_http_coding_token_(coding) };
        if (!no_body_status && length != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) {
            proven_u64 v = length;
            do { digits[sizeof digits - 1 - nd++] = (proven_byte_t)('0' + v % 10); v /= 10; } while (v > 0);
            list[n++] = (proven_http_header_t){ .name = PROVEN_LIT("content-length"), .value = { .ptr = digits + sizeof digits - nd, .size = nd } };
        }
        s->bodiless = no_body_status || s->method == PROVEN_HTTP_HEAD;
        e = proven_h3_submit_response(d->h3, s->id, list, n, end_stream || s->bodiless);
    }
    if (list != small) sv->alloc.free_fn(sv->alloc.ctx, list);
    if (e == PROVEN_ERR_INVALID_ARG || e == PROVEN_ERR_OUT_OF_BOUNDS) { s->bodiless = false; return e; }      /* the handler's list: it may try another */
    if (e != PROVEN_OK) {
        s_reset(s, PROVEN_H3_INTERNAL_ERROR, PROVEN_ERR_RESET);
        return PROVEN_ERR_RESET;
    }
    s->response_begun = true;
    if (end_stream || s->bodiless) s->engine_done = true;
    return PROVEN_OK;
}

static void s_compress_plan(const proven_h3s_t *s, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count, bool *vary,
                            proven_http_coding_t *coding) {
    *vary = false;
    *coding = PROVEN_HTTP_CODING_IDENTITY;
    if (!s->compress_asked || (header_count > 0 && !headers)) return;
    *vary = proven_http_compress_needs_vary_(headers, header_count);
    if (proven_http_compress_applies_(s->compress_coding != PROVEN_HTTP_CODING_IDENTITY, status, headers, header_count)) *coding = (proven_http_coding_t)s->compress_coding;
}

proven_err_t proven_http_event_h3_begin_(proven_http_stream_t *stream, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count,
                                         proven_u64 content_length) {
    proven_h3s_t *s = h3s_of(stream);
    if (s->dead || !s->active || s->response_begun) return PROVEN_ERR_INVALID_STATE;
    proven_http_event_server_t *sv = s->h.server;
    bool vary = false;
    proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;
    s_compress_plan(s, status, headers, header_count, &vary, &coding);
    proven_http_compressor_t_ *zip = (void *)0;
    if (coding != PROVEN_HTTP_CODING_IDENTITY && s->method != PROVEN_HTTP_HEAD) {
        if (!sv->zbuf) sv->zbuf = h3_alloc(sv->alloc, PROVEN_HTTP_CODING_PIECE);
        const proven_http_compress_cfg_t_ cc = proven_http_event_compress_cfg_(&sv->cfg);
        if (!sv->zbuf || proven_http_compressor_create_(sv->alloc, coding, &cc, &zip) != PROVEN_OK) coding = PROVEN_HTTP_CODING_IDENTITY;
    }
    call_begin(s);
    proven_err_t e = s_head(s, status, headers, header_count, coding != PROVEN_HTTP_CODING_IDENTITY ? PROVEN_HTTP_EVENT_LENGTH_UNKNOWN : content_length,
                            vary, coding, false);
    if (e != PROVEN_OK || s->dead) {
        proven_http_compressor_destroy_(zip);
    } else {
        s->zip = zip;
        s->response_left = s->bodiless ? PROVEN_HTTP_EVENT_LENGTH_UNKNOWN : content_length;
    }
    if (!call_end(s) && e == PROVEN_OK) e = PROVEN_ERR_RESET;     /* the caller must not go on to write */
    return e;
}

/* Body bytes through the compressor into the stream's own queue. False when there is no memory. */
static bool s_zip(proven_h3s_t *s, const proven_byte_t *p, proven_size_t n, proven_deflate_flush_t flush) {
    proven_http_event_server_t *sv = s->h.server;
    for (;;) {
        proven_size_t used = 0, made = 0;
        bool done = false;
        proven_err_t e = proven_http_compressor_step_(s->zip, (proven_mem_view_t){ .ptr = p, .size = n }, &used,
                                                      (proven_mem_mut_t){ .ptr = sv->zbuf, .size = PROVEN_HTTP_CODING_PIECE }, &made, flush, &done);
        if (e != PROVEN_OK) return false;
        if (used) p += used;
        n -= used;
        if (s->out_off > 0 && s->out_off == s->out_len) s->out_off = s->out_len = 0;
        if (!h3_append(sv->alloc, &s->out, &s->out_len, &s->out_cap, sv->zbuf, made)) return false;
        if (flush == PROVEN_DEFLATE_FLUSH_FINISH ? done : (n == 0 && made < PROVEN_HTTP_CODING_PIECE)) return true;
    }
}

proven_result_size_t proven_http_event_h3_write_(proven_http_stream_t *stream, proven_mem_view_t data) {
    proven_h3s_t *s = h3s_of(stream);
    if (s->dead || !s->active || !s->response_begun || s->response_ended) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_STATE, .value = 0 };
    if (s->bodiless) return (proven_result_size_t){ .err = PROVEN_OK, .value = data.size };     /* counted, not sent */
    if (s->response_left != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN && data.size > s->response_left) return (proven_result_size_t){ .err = PROVEN_ERR_OUT_OF_BOUNDS, .value = 0 };
    h3d_t *d = s->d;
    call_begin(s);
    proven_size_t done = 0;
    if (s_out_pending(s) > 0) s_push(s);
    while (!s->dead && !s->blocked && s_out_pending(s) == 0 && done < data.size) {
        proven_size_t n = data.size - done;
        if (s->zip) {
            if (n > PROVEN_HTTP_CODING_PIECE) n = PROVEN_HTTP_CODING_PIECE;
            if (!s_zip(s, data.ptr + done, n, PROVEN_DEFLATE_FLUSH_NONE)) { s_reset(s, PROVEN_H3_INTERNAL_ERROR, PROVEN_ERR_NOMEM); break; }
            s_push(s);
        } else {
            proven_size_t taken = 0;
            if (proven_h3_send_data(d->h3, s->id, (proven_mem_view_t){ .ptr = data.ptr + done, .size = n }, false, &taken) != PROVEN_OK) { s_reset(s, PROVEN_H3_INTERNAL_ERROR, PROVEN_ERR_RESET); break; }
            if (taken > 0) d->write_since = 0;
            if (taken < n) s->blocked = true;      /* the engine says when there is room */
            n = taken;
        }
        if (s->response_left != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) s->response_left -= n;
        done += n;
    }
    if (!s->dead && done < data.size) s->want_writable = true;
    const bool alive = call_end(s);
    return alive ? (proven_result_size_t){ .err = PROVEN_OK, .value = done } : (proven_result_size_t){ .err = PROVEN_ERR_RESET, .value = 0 };
}

proven_err_t proven_http_event_h3_flush_(proven_http_stream_t *stream) {
    proven_h3s_t *s = h3s_of(stream);
    if (s->dead || !s->active || !s->response_begun || s->response_ended) return PROVEN_ERR_INVALID_STATE;
    if (!s->zip) return PROVEN_OK;
    call_begin(s);
    if (!s_zip(s, (void *)0, 0, PROVEN_DEFLATE_FLUSH_SYNC)) s_reset(s, PROVEN_H3_INTERNAL_ERROR, PROVEN_ERR_NOMEM);
    else s_push(s);
    const bool alive = call_end(s);
    return alive ? PROVEN_OK : PROVEN_ERR_RESET;
}

proven_err_t proven_http_event_h3_trailers_(const proven_http_stream_t *stream, proven_http_header_t *fields, proven_size_t cap, proven_size_t *count) {
    const proven_h3s_t *s = (const proven_h3s_t *)(const void *)stream;
    if (!s->in_done) return PROVEN_ERR_INVALID_STATE;
    return proven_http_trailers_give_(&s->in_trailers, fields, cap, count);
}

proven_err_t proven_http_event_h3_end_(proven_http_stream_t *stream, const proven_http_header_t *trailers, proven_size_t count) {
    proven_h3s_t *s = h3s_of(stream);
    if (s->dead || !s->active || !s->response_begun || s->response_ended) return PROVEN_ERR_INVALID_STATE;
    if (count > 0) {
        /* As over HTTP/1.1: after a body begun with no length, and not to HEAD. */
        if (s->response_left != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN || s->bodiless || s->engine_done) return PROVEN_ERR_INVALID_STATE;
        if (!proven_http_trailers_valid_(trailers, count)) return PROVEN_ERR_INVALID_ARG;
        const proven_err_t e = proven_http_trailers_keep_(s->h.server->alloc, &s->out_trailers, trailers, count, false, true);
        if (e != PROVEN_OK) return e;
    }
    proven_err_t result = PROVEN_OK;
    call_begin(s);
    if (s->response_left != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN && s->response_left != 0) {
        /* Less was written than promised: the client must not take what it has for the whole. */
        s_reset(s, PROVEN_H3_INTERNAL_ERROR, PROVEN_ERR_INVALID_FORMAT);
        result = PROVEN_ERR_INVALID_FORMAT;
    } else {
        s->response_ended = true;
        if (s->zip) {
            if (!s_zip(s, (void *)0, 0, PROVEN_DEFLATE_FLUSH_FINISH)) { s_reset(s, PROVEN_H3_INTERNAL_ERROR, PROVEN_ERR_NOMEM); result = PROVEN_ERR_RESET; }
            else { proven_http_compressor_destroy_(s->zip); s->zip = (void *)0; }
        }
        if (!s->dead) {
            s->end_asked = true;
            s_push(s);
        }
    }
    call_end(s);
    return result;
}

proven_err_t proven_http_event_h3_respond_(proven_http_stream_t *stream, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count,
                                           proven_mem_view_t body) {
    proven_h3s_t *s = h3s_of(stream);
    if (s->dead || !s->active || s->response_begun) return PROVEN_ERR_INVALID_STATE;
    proven_http_event_server_t *sv = s->h.server;
    h3d_t *d = s->d;
    bool vary = false;
    proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;
    s_compress_plan(s, status, headers, header_count, &vary, &coding);
    proven_byte_t *packed = (void *)0;
    proven_size_t packed_cap = 0, packed_size = 0;
    const proven_http_compress_cfg_t_ cc = proven_http_event_compress_cfg_(&sv->cfg);
    if (coding != PROVEN_HTTP_CODING_IDENTITY && proven_http_compress_all_(sv->alloc, coding, &cc, body, &packed, &packed_cap, &packed_size) != PROVEN_OK) packed = (void *)0;
    if (packed) body = (proven_mem_view_t){ .ptr = packed, .size = packed_size };
    call_begin(s);
    proven_err_t e = s_head(s, status, headers, header_count, body.size, vary, packed ? coding : PROVEN_HTTP_CODING_IDENTITY, body.size == 0);
    if (e == PROVEN_OK && !s->dead) {
        s->response_ended = true;
        s->end_asked = true;
        /* The whole response is this call's contract: what the engine does not take now is kept,
         * and follows as the peer's window opens. */
        if (!s->bodiless && body.size > 0) {
            proven_size_t taken = 0;
            if (proven_h3_send_data(d->h3, s->id, body, true, &taken) != PROVEN_OK) { s_reset(s, PROVEN_H3_INTERNAL_ERROR, PROVEN_ERR_RESET); e = PROVEN_ERR_RESET; }
            else if (taken == body.size) s->engine_done = true;
            else {
                s->blocked = true;
                if (!h3_append(sv->alloc, &s->out, &s->out_len, &s->out_cap, body.ptr + taken, body.size - taken)) { s_reset(s, PROVEN_H3_INTERNAL_ERROR, PROVEN_ERR_NOMEM); e = PROVEN_ERR_NOMEM; }
            }
        }
        if (!s->dead) s_push(s);
    }
    if (packed) sv->alloc.free_fn(sv->alloc.ctx, packed);
    call_end(s);
    return e;
}

void proven_http_event_h3_abort_(proven_http_stream_t *stream) {
    proven_h3s_t *s = h3s_of(stream);
    if (s->dead) return;
    call_begin(s);
    s_reset(s, PROVEN_H3_INTERNAL_ERROR, PROVEN_ERR_RESET);
    call_end(s);
}

void proven_http_event_h3_pause_(proven_http_stream_t *stream) {
    proven_h3s_t *s = h3s_of(stream);
    if (s->dead || !s->active || s->paused) return;
    s->paused = true;
    /* The engine stops reading it, and QUIC's flow control stops the client when the window is full. */
    if (!s->d->dead && !s->d->closed) proven_h3_stream_pause(s->d->h3, s->id, true);
    s_expect_body(s);
    d_arm(s->d);
}

void proven_http_event_h3_resume_(proven_http_stream_t *stream) {
    proven_h3s_t *s = h3s_of(stream);
    if (s->dead || !s->paused) return;
    s->paused = false;
    s_expect_body(s);
    call_begin(s);
    if (!s->d->dead && !s->d->closed) proven_h3_stream_pause(s->d->h3, s->id, false);    /* what waited comes in the round this asks for */
    call_end(s);
}

void proven_http_event_h3_set_user_(proven_http_stream_t *stream, void *user) { h3s_of(stream)->user = user; }
void *proven_http_event_h3_user_(const proven_http_stream_t *stream) { return h3s_of_const(stream)->user; }

proven_net_addr_t proven_http_event_h3_peer_(const proven_http_stream_t *stream) {
    const proven_h3s_t *s = h3s_of_const(stream);
    return s->dead || s->d->dead ? (proven_net_addr_t){ 0 } : s->d->peer;
}

bool proven_http_event_h3_early_(const proven_http_stream_t *stream) { return h3s_of_const(stream)->early; }

proven_size_t proven_http_event_h3_buffered_(const proven_http_stream_t *stream) {
    const proven_h3s_t *s = h3s_of_const(stream);
    return s->dead ? 0 : s_out_pending(s);
}

/* The configured windows, or the engine's defaults, under the ceiling `n` (0: none). */
static void h3_windows(struct ev_h3 *x, const proven_http_event_server_t *sv, proven_u32 n) {
    const proven_u32 w = (proven_u32)sv->cfg.http3_window_bytes;
    const proven_u32 sw = w ? w : 256u * 1024u, cw = w ? w : 1024u * 1024u;
    x->qc.stream_window = n && n < sw ? n : sw;
    x->qc.conn_window = n && n < cw ? n : cw;
    x->qc.stream_window_max = w ? 0u : n && n < PROVEN_QUIC_STREAM_WINDOW_MAX ? n : PROVEN_QUIC_STREAM_WINDOW_MAX;
    x->qc.conn_window_max = w ? 0u : n && n < PROVEN_QUIC_CONN_WINDOW_MAX ? n : PROVEN_QUIC_CONN_WINDOW_MAX;
}

void proven_http_event_h3_set_window_(proven_http_event_server_t *sv, proven_u32 n) {
    struct ev_h3 *x = sv->h3;
    if (!x) return;
    h3_windows(x, sv, n);                          /* connections to come: their transport parameters */
    for (h3d_t *d = x->conns; d; d = d->next) {
        if (d->dead || !d->q) continue;
        proven_quic_conn_cap_window_(d->q, n);
        d_mark(d);
    }
}

void proven_http_event_h3_set_max_streams_(proven_http_event_server_t *sv, proven_u32 n) {
    struct ev_h3 *x = sv->h3;
    if (!x) return;
    x->qc.streams_bidi_now = n;                    /* connections to come */
    for (h3d_t *d = x->conns; d; d = d->next) {
        if (d->dead || !d->q) continue;
        proven_quic_conn_set_max_streams_(d->q, n);
        d_mark(d);                                 /* a raise is said at once */
    }
}

#endif
