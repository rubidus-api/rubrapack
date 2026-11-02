#include "proven/http_event_client.h"
#include "../../platform/proven_sys_mem.h"
#include "proven_internal_http_event_client.h"
#include "proven_internal_loop.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET) && !defined(PROVEN_NO_HTTP3)

#include "proven_internal_quic.h"
#include "proven_internal_h3.h"
#include "proven_internal_quic_net.h"
#include "proven/time.h"
#include "proven/url.h"

/*
 * HTTP/3 in the event-driven client.
 *
 * A request that asks for HTTP/3 goes to the QUIC connection the client keeps for its origin
 * (c3conn_t), made the first time: a UDP socket of its own, the transport (quic_conn.c) and
 * HTTP/3 over it (h3_conn.c), neither of which does any I/O. A request is a small object on it
 * (proven_c3req_t), whose first member is the head http_event_client.c reads.
 *
 * HTTP/3 is what the caller asked for: a server that does not answer over UDP, or refuses `h3`,
 * ends the request with that reason - unless the client has `http3_fallback` (RFC-0031). Then
 * the requests of a QUIC connection that did not come up start again over TCP, as requests of
 * http_event_client.c, and each object the caller holds passes every call on to the request it
 * became (`tcp`) and every callback of that one back as its own. The origin is remembered for
 * five minutes, in which its requests go over TCP from the start.
 *
 * What the event server's driver learned holds here: events are taken from the engine only in a
 * round the loop runs - after a datagram, a timer, or a "call me back" - never inside a call
 * the caller made; every round looks at the times itself (a busy connection would otherwise move
 * its timer on for ever); a connection that HTTP/3 reports closed is kept until QUIC's closing
 * period is over.
 */

#define C3_OUT ((proven_size_t)1500)
#define C3_DATAGRAM ((proven_size_t)65536)
#define C3_READS 256
#define C3_BATCH PROVEN_NET_UDP_BATCH_BYTES
#define C3_SENDS 64
#define C3_COPY_BEHIND 1024u

typedef struct c3conn c3conn_t;

typedef struct c3req {
    proven_cl_head_t h;                       /* first: what http_event_client.c reads */
    c3conn_t *conn;                    /* null while it waits for a connection or a try */
    struct c3req *prev, *next;
    struct c3req *wnext;               /* in the client's list of those that wait for room */
    proven_http_event_client_callbacks_t on;
    void *ctx, *user;
    proven_byte_t *copy;               /* what it was started with, when that did not fit behind the struct */
    proven_u8str_view_t url, method, target;
    proven_http_header_t *headers;
    proven_size_t header_count;
    proven_mem_view_t body;
    proven_u64 body_length;
    proven_byte_t *out;                /* the part of a body in memory the engine did not take yet */
    proven_size_t out_len, out_off;
    proven_http_decoder_t_ *dec;       /* with a piece buffer behind it */
    proven_u64 send_left, got, id;
    proven_time_t due;                 /* response or body timeout; 0: none running */
    proven_time_t wait_due;            /* waiting for room, or for a try: when it ends */
    proven_loop_timer_t wait_timer;
    proven_u32 tries;
    proven_net_addr_t to;              /* the origin, kept for a try on another connection */
    proven_u16 port;
    proven_u8 host_len;
    bool has_id, dead, streamed, end_asked, engine_done, paused, want_writable, blocked;
    bool got_head, head_only, host_is_ipv6, query_alone, waiting_room, delayed;
    bool sent_early;                   /* submitted before the handshake was complete: early data (RFC-0021) */
    bool early_again;                  /* sent again once after a 425 or a refusal: never early, never a third time */
    proven_http_event_request_t *tcp;  /* RFC-0031: the request over TCP this became; every call goes to it */
    bool body_over;                    /* the response's end has arrived (proven_http_event_request_trailers) */
    proven_http_trailers_t_ in_trailers;   /* the response's, until the struct goes - over TCP too, copied at its end */
    proven_http_trailers_t_ out_trailers;  /* the request's, sent as the stream's end once `out` has drained */
    proven_byte_t host[256];
} proven_c3req_t;

struct c3conn {
    proven_http_event_client_t *client;
    c3conn_t *prev, *next;
    proven_net_udp_t udp;
    proven_loop_udp_t lu;              /* the socket as the loop knows it (RFC-0025) */
    proven_byte_t *rbuf;               /* what one receive takes: the loop's while a receive is in flight */
    proven_net_addr_t from;            /* and where it came from */
    proven_loop_timer_t timer;
    proven_loop_after_t after;
    proven_quic_conn_t *q;
    proven_h3_conn_t *h3;
    proven_tls_session_t session;
    proven_u64 session_mark;           /* what it was given (RFC-0020) */
    proven_u64 session_stored;         /* what it last stored, or was given */
    proven_c3req_t *reqs;
    proven_size_t count;
    proven_net_addr_t to;
    proven_time_t born, idle_since, write_since;
    proven_u32 depth, calls;
    proven_u16 port;
    proven_u8 host_len;
    bool attached, sock_closed, sock_gone, waiting;   /* given to the loop; closed; `closed` come; a send wait asked for */
    bool dead, up, closed, over, client_going, bye;
    bool early;                        /* 0-RTT is open: requests that are safe to repeat may go before the handshake (RFC-0021) */
    bool restarted;                    /* started again in the version a Version Negotiation named (RFC-0036) */
    proven_err_t send_failed;          /* RFC-0031: the system said the server cannot be reached; the round ends the connection */
    proven_err_t closed_why;           /* what the close means for a request that was never answered */
    proven_size_t pend_len;            /* datagrams the socket would not take */
    proven_size_t pend_seg;            /* their size (the last may be shorter) */
    bool ecn;                          /* the system reads and sets the ECN field on this socket (RFC-0035) */
    proven_u8 pend_ecn;                /* the mark of the datagrams kept */
    proven_byte_t pend[C3_BATCH];
    proven_byte_t host[256];
};

static proven_u8str_view_t c3_lit_n(const char *s, proven_size_t n) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = n }; }
#define c3_lit(s) c3_lit_n((s), sizeof(s) - 1)

static proven_byte_t c3_lower(proven_byte_t c) { return (c >= 'A' && c <= 'Z') ? (proven_byte_t)(c + 32) : c; }


static void *c3_alloc(proven_allocator_t a, proven_size_t n) {
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, n, 16);
    if (m.err != PROVEN_OK) return (void *)0;
    proven_sys_mem_set(m.value.ptr, 0, n);
    return m.value.ptr;
}

static proven_u64 c3_now_us(void) { return (proven_u64)proven_time_monotonic_now() / 1000u; }

static proven_http_event_request_t *c3_handle(proven_c3req_t *r) { return (proven_http_event_request_t *)(void *)r; }
static proven_c3req_t *c3_of(proven_http_event_request_t *request) { return (proven_c3req_t *)(void *)request; }
static const proven_c3req_t *c3_of_const(const proven_http_event_request_t *request) { return (const proven_c3req_t *)(const void *)request; }

static bool c3_same_addr(const proven_net_addr_t *a, const proven_net_addr_t *b) {
    if (a->family != b->family || a->port != b->port) return false;
    for (proven_size_t i = 0; i < sizeof a->ip; ++i) if (a->ip[i] != b->ip[i]) return false;
    return true;
}

// -----------------------------------------------------------------------------
// Lifetimes
// -----------------------------------------------------------------------------

static void c_round(c3conn_t *c);
static void c_arm(c3conn_t *c);
static void r_place(proven_c3req_t *r);

static void c_enter(c3conn_t *c) { c->depth++; }

static void c_free(c3conn_t *c) {
    proven_allocator_t a = c->client->alloc;
    if (c->rbuf) a.free_fn(a.ctx, c->rbuf);
    a.free_fn(a.ctx, c);
}

static void c_leave(c3conn_t *c) {
    if (--c->depth > 0 || !c->dead) return;
    if (c->attached && !c->sock_gone) return;     /* c_sock_closed frees it */
    c_free(c);
}

static void r_enter(proven_c3req_t *r) { r->h.depth++; }

static void r_leave(proven_c3req_t *r) {
    if (--r->h.depth > 0 || !r->dead) return;
    proven_allocator_t a = r->h.client->alloc;
    proven_http_trailers_free_(a, &r->in_trailers);
    proven_http_trailers_free_(a, &r->out_trailers);
    a.free_fn(a.ctx, r);
}

static void r_free_parts(proven_c3req_t *r) {
    proven_allocator_t a = r->h.client->alloc;
    if (r->copy) { a.free_fn(a.ctx, r->copy); r->copy = (void *)0; }
    if (r->out) { a.free_fn(a.ctx, r->out); r->out = (void *)0; r->out_len = r->out_off = 0; }
    if (r->dec) { proven_http_decoder_free_(r->dec); a.free_fn(a.ctx, r->dec); r->dec = (void *)0; }
}

static void r_unlink(proven_c3req_t *r) {
    c3conn_t *c = r->conn;
    if (!c) return;
    if (r->prev) r->prev->next = r->next;
    else c->reqs = r->next;
    if (r->next) r->next->prev = r->prev;
    r->prev = r->next = (void *)0;
    c->count--;
    r->conn = (void *)0;
}

static void r_attach(proven_c3req_t *r, c3conn_t *c) {
    proven_c3req_t *last = c->reqs;
    while (last && last->next) last = last->next;
    r->conn = c;
    r->prev = last;
    r->next = (void *)0;
    if (last) last->next = r;
    else c->reqs = r;
    c->count++;
}

static void r_undelay(proven_c3req_t *r) {
    if (!r->delayed) return;
    proven_http_event_client_t *cl = r->h.client;
    proven_c3req_t *before = (void *)0;
    for (proven_c3req_t *i = cl->h3_delayed; i && i != r; i = i->wnext) before = i;
    if (before) before->wnext = r->wnext;
    else cl->h3_delayed = r->wnext;
    r->wnext = (void *)0;
    r->delayed = false;
}

static void r_unroom(proven_c3req_t *r) {
    if (!r->waiting_room) return;
    proven_http_event_client_t *cl = r->h.client;
    proven_c3req_t *before = (void *)0;
    for (proven_c3req_t *i = cl->h3_roomless; i && i != r; i = i->wnext) before = i;
    if (before) before->wnext = r->wnext;
    else cl->h3_roomless = r->wnext;
    if (cl->h3_roomless_last == r) cl->h3_roomless_last = before;
    r->wnext = (void *)0;
    r->waiting_room = false;
    cl->h3_roomless_count--;
}

// -----------------------------------------------------------------------------
// Fallback to TCP (RFC-0031)
// -----------------------------------------------------------------------------

#define C3_GONE 64u                                       /* origins remembered */
#define C3_GONE_NS ((proven_time_t)300 * 1000000000)      /* for five minutes */

typedef struct {
    proven_u64 key;                    /* the origin's hash; 0: free */
    proven_time_t until;
} c3gone_t;

static proven_u64 c3_origin_key(proven_u8str_view_t host, proven_u16 port) {
    proven_u64 h = 1469598103934665603u;
    for (proven_size_t i = 0; i < host.size; ++i) { h ^= c3_lower(host.ptr[i]); h *= 1099511628211u; }
    h ^= port; h *= 1099511628211u;
    return h | 1u;
}

/* Whether the origin's QUIC connection did not come up in the last five minutes. */
static bool c3_gone(const proven_http_event_client_t *cl, proven_u8str_view_t host, proven_u16 port) {
    const c3gone_t *g = cl->h3_gone;
    if (!g) return false;
    const proven_u64 k = c3_origin_key(host, port);
    const proven_time_t now = proven_time_monotonic_now();
    for (proven_size_t i = 0; i < C3_GONE; ++i) if (g[i].key == k && g[i].until > now) return true;
    return false;
}

/* Remembered in its own place, a free one, or the oldest: they all last as long. */
static void c3_remember(proven_http_event_client_t *cl, proven_u8str_view_t host, proven_u16 port) {
    if (!cl->h3_gone) {
        cl->h3_gone = c3_alloc(cl->alloc, C3_GONE * sizeof(c3gone_t));
        if (!cl->h3_gone) return;                  /* not remembered: the next request waits for its handshake again */
    }
    c3gone_t *g = cl->h3_gone;
    const proven_u64 k = c3_origin_key(host, port);
    proven_size_t at = 0;
    for (proven_size_t i = 0; i < C3_GONE; ++i) {
        if (g[i].key == k) { at = i; break; }
        if (g[i].until < g[at].until) at = i;
    }
    g[at] = (c3gone_t){ .key = k, .until = proven_time_monotonic_now() + C3_GONE_NS };
}

/* A connection that did not come up, for a reason TCP may not share: no answer in
 * http3_fallback_ms, an unreachable address or a refused port, `h3` refused. Not a certificate,
 * which TCP would show again; not a connection that came up and was lost. */
static bool c_falls_back(const c3conn_t *c) {
    const proven_http_event_client_t *cl = c->client;
    if (!cl->cfg.http3_fallback || c->up || c->client_going || cl->closing) return false;
    const proven_err_t why = c->closed_why;
    return why == PROVEN_ERR_TIMEOUT || why == PROVEN_ERR_UNREACHABLE || why == PROVEN_ERR_REFUSED || why == PROVEN_ERR_UNSUPPORTED;
}

/* The request over TCP calls back as the object the caller holds. */
static void x_response(void *ctx, proven_http_event_request_t *in, const proven_http_response_t *head) {
    (void)in;
    proven_c3req_t *r = ctx;
    if (r->on.on_response) r->on.on_response(r->ctx, c3_handle(r), head);
}
static void x_body(void *ctx, proven_http_event_request_t *in, proven_mem_view_t piece, bool last) {
    proven_c3req_t *r = ctx;
    if (last) {
        /* The trailers over TCP, copied: the request over TCP is gone by the time on_done returns. */
        proven_http_header_t fields[64];
        proven_size_t n = 0;
        r->body_over = true;
        if (proven_http_event_request_trailers(in, fields, 64, &n) != PROVEN_OK ||
            proven_http_trailers_keep_(r->h.client->alloc, &r->in_trailers, fields, n, false, false) != PROVEN_OK) {
            proven_http_event_request_abort(in);
            return;
        }
    }
    if (r->on.on_body) r->on.on_body(r->ctx, c3_handle(r), piece, last);
}
static void x_writable(void *ctx, proven_http_event_request_t *in) {
    (void)in;
    proven_c3req_t *r = ctx;
    if (r->on.on_writable) r->on.on_writable(r->ctx, c3_handle(r));
}
static void x_done(void *ctx, proven_http_event_request_t *in, proven_err_t why) {
    (void)in;
    proven_c3req_t *r = ctx;
    proven_loop_timer_cancel(r->h.client->loop, &r->wait_timer);
    r->tcp = (void *)0;
    r->dead = true;
    r_enter(r);
    r->on.on_done(r->ctx, c3_handle(r), why);
    r_leave(r);
}
static const proven_http_event_client_callbacks_t x_relay = { x_response, x_body, x_writable, x_done };

/* A body refused before the switch: the caller is told it may write, now to the request over TCP. */
static void x_writable_fire(void *ctx) {
    proven_c3req_t *r = ctx;
    if (r->dead || !r->tcp || !r->on.on_writable) return;
    r_enter(r);
    r->on.on_writable(r->ctx, c3_handle(r));
    r_leave(r);
}

static void r_finish(proven_c3req_t *r, proven_err_t why);

/* Start the request again over TCP, as it was started; the caller keeps its object. Counted once:
 * as the request it now is. */
static void r_fallback(proven_c3req_t *r) {
    proven_http_event_client_t *cl = r->h.client;
    r_unlink(r);
    r_unroom(r);
    r_undelay(r);
    proven_loop_timer_cancel(cl->loop, &r->wait_timer);
    r->due = 0;
    const proven_http_event_request_options_t o = {
        .method = r->method, .url = r->url, .address = &r->to, .headers = r->headers, .header_count = r->header_count,
        .body = r->body, .body_length = r->body_length, .on = x_relay, .ctx = r,
    };
    r_enter(r);
    proven_http_event_request_t *made = (void *)0;
    const proven_err_t e = proven_http_event_client_start(cl, &o, &made);
    if (e != PROVEN_OK) { r_finish(r, e); r_leave(r); return; }
    cl->request_count--;
    r->tcp = made;
    r_free_parts(r);                               /* what it was started with is the new request's now */
    if (r->paused) proven_http_event_request_pause(made);
    /* Ended with trailers before it had a stream: they end it over TCP instead. */
    if (r->end_asked && r->out_trailers.count && proven_http_event_request_end_trailers(made, r->out_trailers.fields, r->out_trailers.count) != PROVEN_OK)
        proven_http_event_request_abort(made);
    if (r->streamed && r->want_writable) proven_loop_timer_set(cl->loop, &r->wait_timer, 0, x_writable_fire, r);
    r_leave(r);
}

/* The end of a request, good or bad: once. */
static void r_finish(proven_c3req_t *r, proven_err_t why) {
    if (r->dead) return;
    c3conn_t *c = r->conn;
    proven_http_event_client_t *cl = r->h.client;
    r_enter(r);
    r->dead = true;
    r_unroom(r);
    r_undelay(r);
    proven_loop_timer_cancel(cl->loop, &r->wait_timer);
    if (c && !c->dead && !c->closed && r->has_id && (!r->engine_done || why != PROVEN_OK || !r->got_head)) {
        /* A response that is not whole, or a request given up: nothing more is to go either way. */
        if (why != PROVEN_OK) (void)proven_h3_stream_reset(c->h3, r->id, PROVEN_H3_REQUEST_CANCELLED);
    }
    /* A ticket the connection has had by now is the next connection's to offer (RFC-0020). */
    if (c && !c->dead && c->q && why == PROVEN_OK)
        proven_http_event_client_session_keep_(cl, (proven_u8str_view_t){ .ptr = c->host, .size = c->host_len }, c->port, true, &c->session, c->session_mark, &c->session_stored,
                                               c->up, proven_quic_conn_resumed(c->q));
    r_unlink(r);
    cl->request_count--;
    r_free_parts(r);
    r->on.on_done(r->ctx, c3_handle(r), why);
    r_leave(r);
}

static void c_call_back(c3conn_t *c);

/* A request the server did not act on: told PROVEN_ERR_AGAIN, or - with retry_unsent - put to
 * the origin again: at once, or after retry_delay_ms; on another connection, made if need be. */
static void r_wait_fire(void *ctx);

static void r_unsent(proven_c3req_t *r) {
    if (r->dead) return;
    proven_http_event_client_t *cl = r->h.client;
    if (cl->closing || r->streamed || r->got_head || r->tries >= cl->cfg.retry_unsent) { r_finish(r, PROVEN_ERR_AGAIN); return; }
    r->tries++;
    r_unlink(r);
    if (r->out) { cl->alloc.free_fn(cl->alloc.ctx, r->out); r->out = (void *)0; r->out_len = r->out_off = 0; }
    r->has_id = r->end_asked = r->engine_done = r->blocked = false;
    r->due = 0;
    const proven_u32 ms = proven_http_event_client_retry_delay_(cl, r->tries - 1);
    if (ms == 0) { r_place(r); return; }
    r->delayed = true;
    r->wnext = cl->h3_delayed;
    cl->h3_delayed = r;
    proven_loop_timer_set(cl->loop, &r->wait_timer, ms, r_wait_fire, r);
}

/* A request sent as early data and not answered as such: once more, never early. On this
 * connection when it is up, else on another. Not counted against retry_unsent. */
static void r_again(proven_c3req_t *r) {
    if (r->dead) return;
    proven_http_event_client_t *cl = r->h.client;
    if (cl->closing) { r_finish(r, PROVEN_ERR_AGAIN); return; }
    r->early_again = true;
    r->sent_early = false;
    r_unlink(r);
    if (r->out) { cl->alloc.free_fn(cl->alloc.ctx, r->out); r->out = (void *)0; r->out_len = r->out_off = 0; }
    r->has_id = r->end_asked = r->engine_done = r->blocked = false;
    r->due = 0;
    r_place(r);
}

static void c_end_requests(c3conn_t *c) {
    /* A connection that ended before its handshake did because the server refused the early data
     * it could no longer take (RFC-0021 P-6): its early requests go once more, elsewhere, and the
     * origin's next connection sends none. */
    proven_quic_stats_t st = { 0 };
    if (c->q) proven_quic_conn_stats(c->q, &st);
    const bool refused = !c->up && st.early == 2;
    if (refused) {
        proven_tls_session_early_off_(&c->session);
        proven_http_event_client_session_keep_(c->client, (proven_u8str_view_t){ .ptr = c->host, .size = c->host_len }, c->port, true, &c->session, c->session_mark, &c->session_stored, false, false);
    }
    /* Not come up for a reason TCP may not share: over TCP, and the origin remembered (RFC-0031). */
    const bool fall = c_falls_back(c);
    if (fall) c3_remember(c->client, (proven_u8str_view_t){ .ptr = c->host, .size = c->host_len }, c->port);
    while (c->reqs) {
        proven_c3req_t *r = c->reqs;
        if (refused && r->sent_early && !r->got_head && !r->early_again) { r_again(r); continue; }
        if (fall && !r->got_head && (!r->streamed || !r->has_id)) { r_fallback(r); continue; }
        /* One the server never saw may go again elsewhere - but not when the connection never
         * came up: that is the answer for every request to this origin, and is theirs. */
        if (!r->has_id && c->up && !c->client_going) r_unsent(r);
        else r_finish(r, r->has_id || !c->up ? c->closed_why : PROVEN_ERR_RESET);
    }
}

static void c_close_socket(c3conn_t *c) {
    proven_http_event_client_t *cl = c->client;
    proven_loop_timer_cancel(cl->loop, &c->timer);
    proven_loop_after_cancel_(cl->loop, &c->after);
    if (c->attached && !c->sock_closed) {          /* the loop stops watching before the close */
        c->sock_closed = true;
        cl->ending++;
        proven_loop_udp_close_(cl->loop, &c->lu);
    }
    (void)proven_net_udp_close(&c->udp);
}

/* The connection is finished with: out of the client's list, its requests told, its engines freed. */
static void c_kill(c3conn_t *c, proven_err_t why) {
    if (c->dead) return;
    proven_http_event_client_t *cl = c->client;
    c_enter(c);
    if (!c->closed) c->closed_why = why;
    c->dead = true;
    if (c->prev) c->prev->next = c->next;
    else cl->h3 = c->next;
    if (c->next) c->next->prev = c->prev;
    c_end_requests(c);
    c_close_socket(c);
    const bool established = c->up, resumed = c->q && proven_quic_conn_resumed(c->q);
    proven_h3_conn_destroy(c->h3);
    proven_quic_conn_destroy(c->q);
    c->h3 = (void *)0; c->q = (void *)0;
    proven_http_event_client_session_keep_(cl, (proven_u8str_view_t){ .ptr = c->host, .size = c->host_len }, c->port, true, &c->session, c->session_mark, &c->session_stored, established, resumed);
    proven_mem_wipe((proven_mem_mut_t){ .ptr = c->session.opaque, .size = sizeof c->session.opaque });
    if (!c->client_going) proven_http_event_client_freed_(cl);
    c_leave(c);
}

// -----------------------------------------------------------------------------
// Datagrams
// -----------------------------------------------------------------------------

/* Datagrams are always being received; when the socket would not take some, the loop is asked
 * to say when it will. */
static void c_watch(c3conn_t *c) {
    if (c->dead || !c->attached || c->sock_closed || c->pend_len == 0 || c->waiting) return;
    if (proven_loop_udp_send_wait_(c->client->loop, &c->lu) == PROVEN_OK) c->waiting = true;
}

static void c_after(void *ctx) {
    c3conn_t *c = ctx;
    c_enter(c);
    c_round(c);
    c_leave(c);
}

/* A round for this connection, from the loop, never from where the caller stands. */
static void c_call_back(c3conn_t *c) {
    if (c->dead) return;
    proven_loop_after_(c->client->loop, &c->after, c_after, c);
}

/* Datagrams of `seg` bytes (the last may be shorter) out in one call where the platform can;
 * what the socket will not take now is kept, and nothing more goes until it has. */
static void c_send_batch(c3conn_t *c, const proven_byte_t *p, proven_size_t n, proven_size_t seg, proven_u8 ecn) {
    if (c->pend_len > 0 || n == 0) return;
    proven_size_t sent = 0;
    const proven_err_t e = proven_net_udp_send_marked_(&c->udp, c->to, (proven_mem_view_t){ .ptr = p, .size = n }, seg, ecn, &sent);
    if ((e == PROVEN_ERR_UNREACHABLE || e == PROVEN_ERR_REFUSED) && !c->up) c->send_failed = e;     /* RFC-0031: the round ends it */
    if (e != PROVEN_ERR_TIMEOUT) return;          /* sent, or lost as UDP may lose it */
    proven_sys_mem_copy(c->pend, p + sent, n - sent);
    c->pend_len = n - sent;
    c->pend_seg = seg;
    c->pend_ecn = ecn;
}

static void c_send(c3conn_t *c) {
    if (c->dead) return;
    proven_http_event_client_t *cl = c->client;
    if (!cl->h3_batch) cl->h3_batch = c3_alloc(cl->alloc, C3_BATCH);
    proven_byte_t one[C3_OUT];
    proven_byte_t *buf = cl->h3_batch ? cl->h3_batch : one;     /* without it, one datagram a call */
    const proven_size_t cap = cl->h3_batch ? C3_BATCH : C3_OUT;
    /* Gathered while they are one size and carry one ECN mark (RFC-0018 S-3, RFC-0035): a larger
     * one (a PMTU probe) or another mark sends what is gathered first; a shorter one ends the
     * batch; so do the platform's limits and the round. */
    proven_size_t at = 0, seg = 0;
    proven_u8 bmark = 0;
    int count = 0, k = 0;
    for (; k < C3_SENDS; ++k) {
        if (at == 0 && c->pend_len > 0) break;
        if (cap - at < C3_OUT) { c_send_batch(c, buf, at, seg, bmark); at = 0; count = 0; --k; continue; }
        proven_size_t len = 0;
        proven_quic_addr_t to;
        proven_u8 mark = 0;
        if (proven_quic_conn_send(c->q, c3_now_us(), (proven_mem_mut_t){ .ptr = buf + at, .size = C3_OUT }, &len, &to, c->ecn ? &mark : NULL) != PROVEN_OK || len == 0) break;
        if (at > 0 && (len > seg || mark != bmark)) {
            c_send_batch(c, buf, at, seg, bmark);
            if (c->pend_len > 0) { at = 0; break; }    /* this one is lost as UDP may lose it */
            proven_sys_mem_move(buf, buf + at, len);
            at = 0; count = 0;
        }
        if (at == 0) { seg = len; bmark = mark; }
        at += len;
        if (len < seg || ++count == PROVEN_NET_UDP_BATCH_COUNT) { c_send_batch(c, buf, at, seg, bmark); at = 0; count = 0; }
    }
    if (at > 0) c_send_batch(c, buf, at, seg, bmark);
    if (k == C3_SENDS) c_call_back(c);            /* more, after the other connections have had theirs */
    c_watch(c);
}

static bool c_over(const c3conn_t *c) {
    proven_quic_stats_t st;
    proven_quic_conn_stats(c->q, &st);
    return st.closed;
}

// -----------------------------------------------------------------------------
// Requests out
// -----------------------------------------------------------------------------

static void r_expect(proven_c3req_t *r) {
    const proven_http_event_client_config_t *cfg = &r->h.client->cfg;
    proven_u32 ms = 0;
    if (!r->has_id || r->paused) ms = 0;
    else if (!r->got_head) ms = r->end_asked || !r->streamed ? cfg->response_timeout_ms : 0;
    else ms = cfg->body_timeout_ms;
    r->due = ms ? proven_time_monotonic_now() + (proven_time_t)ms * 1000000 : 0;
}

/* What a request holds of its body, towards the engine; its end when all of it has gone. */
static void r_push(proven_c3req_t *r) {
    if (r->dead || !r->has_id || r->engine_done) return;
    c3conn_t *c = r->conn;
    proven_http_event_client_t *cl = r->h.client;
    if (r->out_off < r->out_len && !r->blocked) {
        proven_size_t taken = 0;
        const bool fin = r->end_asked && r->out_trailers.count == 0;     /* with trailers, they end it */
        const proven_err_t e = proven_h3_send_data(c->h3, r->id, (proven_mem_view_t){ .ptr = r->out + r->out_off, .size = r->out_len - r->out_off }, fin, &taken);
        if (e != PROVEN_OK) { r_finish(r, PROVEN_ERR_RESET); return; }
        r->out_off += taken;
        if (taken > 0) c->write_since = 0;
        if (r->out_off < r->out_len) { r->blocked = true; return; }
        cl->alloc.free_fn(cl->alloc.ctx, r->out);
        r->out = (void *)0; r->out_len = r->out_off = 0;
        if (fin) { r->engine_done = true; r_expect(r); return; }
    }
    if (r->out_len == 0 && r->end_asked && !r->engine_done) {
        proven_size_t taken = 0;
        const proven_err_t e = r->out_trailers.count ? proven_h3_send_trailers(c->h3, r->id, r->out_trailers.fields, r->out_trailers.count)
                                                     : proven_h3_send_data(c->h3, r->id, (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, true, &taken);
        if (e != PROVEN_OK) { r_finish(r, PROVEN_ERR_RESET); return; }
        r->engine_done = true;
        r_expect(r);
    }
}

/* Onto a stream. PROVEN_ERR_BUSY: the server's limit, or not up yet - it waits. */
static proven_err_t r_submit(proven_c3req_t *r) {
    c3conn_t *c = r->conn;
    proven_http_event_client_t *cl = c->client;
    if (r->query_alone) return PROVEN_ERR_INVALID_ARG;
    const proven_u8str_view_t path = r->target.size > 0 ? r->target : c3_lit("/");
    proven_byte_t authority[300], digits[20];
    proven_size_t al = 0, nd = 0;
    if (r->host_is_ipv6) authority[al++] = '[';
    for (proven_size_t i = 0; i < c->host_len; ++i) authority[al++] = c3_lower(c->host[i]);
    if (r->host_is_ipv6) authority[al++] = ']';
    if (c->port != 443) {
        authority[al++] = ':';
        char d[5]; int dn = 0; proven_u16 p = c->port;
        do { d[dn++] = (char)('0' + p % 10); p /= 10; } while (p);
        while (dn > 0) authority[al++] = (proven_byte_t)d[--dn];
    }
    proven_http_header_t small[40], *list = small;
    if (r->header_count + 6 > sizeof small / sizeof small[0]) {
        list = c3_alloc(cl->alloc, (r->header_count + 6) * sizeof *list);
        if (!list) return PROVEN_ERR_NOMEM;
    }
    proven_size_t n = 0, at = 0;
    proven_err_t e = PROVEN_OK;
    list[n++] = (proven_http_header_t){ .name = c3_lit(":method"), .value = r->method };
    list[n++] = (proven_http_header_t){ .name = c3_lit(":scheme"), .value = c3_lit("https") };
    list[n++] = (proven_http_header_t){ .name = c3_lit(":authority"), .value = { .ptr = authority, .size = al } };
    list[n++] = (proven_http_header_t){ .name = c3_lit(":path"), .value = path };
    bool has_accept = false, has_range = false;
    for (proven_size_t i = 0; i < r->header_count; ++i) {
        proven_u8str_view_t name = r->headers[i].name;
        if (proven_u8str_view_eq_ascii_ci(name, c3_lit("accept-encoding"))) has_accept = true;
        if (proven_u8str_view_eq_ascii_ci(name, c3_lit("range"))) has_range = true;
        /* Fields that describe an HTTP/1.1 connection are not sent on this one. */
        if (proven_u8str_view_eq_ascii_ci(name, c3_lit("keep-alive")) || proven_u8str_view_eq_ascii_ci(name, c3_lit("proxy-connection")) || proven_u8str_view_eq_ascii_ci(name, c3_lit("upgrade"))) continue;
        if (proven_u8str_view_eq_ascii_ci(name, c3_lit("te")) && !proven_u8str_view_eq_ascii_ci(r->headers[i].value, c3_lit("trailers"))) continue;
        if (at + name.size > cl->cfg.max_head_bytes) { e = PROVEN_ERR_OUT_OF_BOUNDS; break; }
        for (proven_size_t k = 0; k < name.size; ++k) cl->hbuf[at + k] = c3_lower(name.ptr[k]);
        list[n++] = (proven_http_header_t){ .name = { .ptr = cl->hbuf + at, .size = name.size }, .value = r->headers[i].value };
        at += name.size;
    }
    const bool body_method = proven_u8str_view_eq_ascii_ci(r->method, c3_lit("POST")) || proven_u8str_view_eq_ascii_ci(r->method, c3_lit("PUT")) || proven_u8str_view_eq_ascii_ci(r->method, c3_lit("PATCH"));
    if (e == PROVEN_OK) {
        if (cl->cfg.decompress && !has_accept && !has_range) list[n++] = (proven_http_header_t){ .name = c3_lit("accept-encoding"), .value = proven_http_decoder_accept_(cl->cfg.decompress_zstd, cl->cfg.decompress_brotli) };
        proven_u64 length = r->body.size > 0 ? r->body.size : r->body_length;
        if (r->body.size > 0 || (r->body_length != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN && (r->body_length > 0 || body_method))) {
            do { digits[sizeof digits - 1 - nd++] = (proven_byte_t)('0' + length % 10); length /= 10; } while (length > 0);
            list[n++] = (proven_http_header_t){ .name = c3_lit("content-length"), .value = { .ptr = digits + sizeof digits - nd, .size = nd } };
        }
        e = proven_h3_submit_request(c->h3, list, n, !r->streamed && r->body.size == 0, &r->id);
    }
    if (list != small) cl->alloc.free_fn(cl->alloc.ctx, list);
    if (e == PROVEN_ERR_INVALID_STATE) return PROVEN_ERR_BUSY;          /* the handshake is not over */
    if (e != PROVEN_OK) return e;
    r->has_id = true;
    r->head_only = proven_u8str_view_eq_ascii_ci(r->method, c3_lit("HEAD"));
    if (!r->streamed) {
        r->end_asked = true;
        if (r->body.size == 0) r->engine_done = true;
        else {
            proven_size_t taken = 0;
            if (proven_h3_send_data(c->h3, r->id, r->body, true, &taken) != PROVEN_OK) return PROVEN_ERR_RESET;
            if (taken == r->body.size) r->engine_done = true;
            else {
                r->out = c3_alloc(cl->alloc, r->body.size - taken);
                if (!r->out) return PROVEN_ERR_NOMEM;
                proven_sys_mem_copy(r->out, r->body.ptr + taken, r->body.size - taken);
                r->out_len = r->body.size - taken;
                r->blocked = true;
            }
        }
    } else {
        r->send_left = r->body_length;
    }
    /* What it was started with is kept only while it may have to be put to the server again. */
    if (r->copy && !r->sent_early && (r->streamed || cl->cfg.retry_unsent == 0)) { cl->alloc.free_fn(cl->alloc.ctx, r->copy); r->copy = (void *)0; }
    r_expect(r);
    return PROVEN_OK;
}

/* Tests only, on the client's one thread: any request with its body in memory may go early, so
 * that a server's 425 for what is not safe to repeat can be seen. */
static bool g_c3_early_any;
void proven_http_event_client_h3_test_early_any_(bool on) { g_c3_early_any = on; }

/* What may go as early data (RFC-0021 D-3): GET, HEAD or OPTIONS with no body, never twice. */
static bool r_early_ok(const proven_c3req_t *r) {
    if (g_c3_early_any && !r->early_again && !r->streamed) return true;
    if (r->early_again || r->streamed || r->body.size != 0 || r->body_length != 0) return false;
    const proven_http_method_t m = proven_http_method_from_text(r->method);
    return m == PROVEN_HTTP_GET || m == PROVEN_HTTP_HEAD || m == PROVEN_HTTP_OPTIONS;
}

static void c_submit_waiting(c3conn_t *c) {
    if (c->dead || c->closed || (!c->up && !c->early) || c->over) return;
    for (bool again = true; again && !c->dead;) {
        again = false;
        for (proven_c3req_t *r = c->reqs; r; r = r->next) {
            if (r->has_id || r->dead) continue;
            if (!c->up && !r_early_ok(r)) continue;          /* the rest wait for the handshake */
            r->sent_early = !c->up;
            const proven_err_t e = r_submit(r);
            if (e == PROVEN_ERR_BUSY) return;
            if (e != PROVEN_OK) r_finish(r, e);
            again = true;
            break;
        }
    }
}

// -----------------------------------------------------------------------------
// Responses
// -----------------------------------------------------------------------------

static bool r_deliver(proven_c3req_t *r, proven_mem_view_t piece, bool last) {
    proven_http_event_client_t *cl = r->h.client;
    if (!r->dec) {
        r->got += piece.size;
        if (cl->cfg.max_body_bytes && r->got > cl->cfg.max_body_bytes) { r_finish(r, PROVEN_ERR_OUT_OF_BOUNDS); return false; }
        if ((piece.size > 0 || last) && r->on.on_body) r->on.on_body(r->ctx, c3_handle(r), piece, last);
        return !r->dead;
    }
    proven_byte_t *room = (proven_byte_t *)(void *)(r->dec + 1);
    while (piece.size > 0 && !r->dead) {
        proven_size_t used = 0, made = 0;
        proven_err_t e = proven_http_decoder_step_(r->dec, piece, &used, (proven_mem_mut_t){ .ptr = room, .size = PROVEN_HTTP_CODING_PIECE }, &made);
        if (e != PROVEN_OK) { r_finish(r, e); return false; }
        piece.ptr += used;
        piece.size -= used;
        if (made > 0) {
            r->got += made;
            if (cl->cfg.max_body_bytes && r->got > cl->cfg.max_body_bytes) { r_finish(r, PROVEN_ERR_OUT_OF_BOUNDS); return false; }
            if (r->on.on_body) r->on.on_body(r->ctx, c3_handle(r), (proven_mem_view_t){ .ptr = room, .size = made }, false);
        } else if (used == 0) {
            r_finish(r, PROVEN_ERR_INVALID_FORMAT);
            return false;
        }
    }
    while (!r->dead) {
        proven_size_t used = 0, made = 0;
        proven_err_t e = proven_http_decoder_step_(r->dec, (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, &used, (proven_mem_mut_t){ .ptr = room, .size = PROVEN_HTTP_CODING_PIECE }, &made);
        if (e != PROVEN_OK) { r_finish(r, e); return false; }
        if (made == 0) break;
        r->got += made;
        if (cl->cfg.max_body_bytes && r->got > cl->cfg.max_body_bytes) { r_finish(r, PROVEN_ERR_OUT_OF_BOUNDS); return false; }
        if (r->on.on_body) r->on.on_body(r->ctx, c3_handle(r), (proven_mem_view_t){ .ptr = room, .size = made }, false);
    }
    if (r->dead) return false;
    if (last) {
        proven_err_t e = proven_http_decoder_end_(r->dec);
        if (e != PROVEN_OK) { r_finish(r, e); return false; }
        if (r->on.on_body) r->on.on_body(r->ctx, c3_handle(r), (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, true);
    }
    return !r->dead;
}

static void c_response(c3conn_t *c, proven_c3req_t *r, const proven_h3_event_t *ev) {
    proven_http_event_client_t *cl = c->client;
    if (ev->status < 200 || r->got_head) return;          /* an interim response: not the answer, and not passed on */
    if (ev->status == 425 && r->sent_early && !r->early_again) {
        /* Too Early (RFC 8470): not the answer. Sent again, once, after the handshake. */
        if (!ev->end_stream) (void)proven_h3_stream_reset(c->h3, r->id, PROVEN_H3_REQUEST_CANCELLED);
        r_again(r);
        return;
    }
    if (ev->field_count > cl->cfg.max_headers) { r_finish(r, PROVEN_ERR_OUT_OF_BOUNDS); return; }
    for (proven_size_t i = 0; i < ev->field_count; ++i) cl->headers[i] = ev->fields[i];
    proven_http_response_t res = { .status = ev->status, .version_minor = 1, .headers = cl->headers, .header_count = ev->field_count };
    r->got_head = true;
    if (r->copy) { cl->alloc.free_fn(cl->alloc.ctx, r->copy); r->copy = (void *)0; }
    const bool no_body = ev->end_stream || r->head_only;
    proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;
    if (cl->cfg.decompress && !no_body && proven_http_decoder_wanted_(res.status, res.headers, res.header_count, cl->cfg.decompress_zstd, cl->cfg.decompress_brotli, &coding)) {
        proven_result_mem_mut_t m = cl->alloc.alloc_fn(cl->alloc.ctx, sizeof(proven_http_decoder_t_) + PROVEN_HTTP_CODING_PIECE, alignof(proven_http_decoder_t_));
        if (m.err != PROVEN_OK) { r_finish(r, PROVEN_ERR_NOMEM); return; }
        r->dec = (proven_http_decoder_t_ *)(void *)m.value.ptr;
        proven_http_decoder_begin_(r->dec, cl->alloc, coding);
    }
    r_enter(r);
    if (r->on.on_response) r->on.on_response(r->ctx, c3_handle(r), &res);
    if (!r->dead) {
        if (ev->end_stream || r->head_only) { r->body_over = true; r_finish(r, PROVEN_OK); }
        else r_expect(r);
    }
    r_leave(r);
}

static proven_c3req_t *c_find_req(const c3conn_t *c, proven_u64 id) {
    for (proven_c3req_t *r = c->reqs; r; r = r->next) if (r->has_id && r->id == id) return r;
    return (void *)0;
}

/* What a QUIC close means for a request: a certificate the TLS engine would not take, `h3`
 * refused, or a connection lost. (Its alerts come as 0x100 and the alert's number.) */
/* The QUIC configuration of a connection of this client's. */
static proven_quic_config_t c_qconfig(const proven_http_event_client_t *cl) {
    return (proven_quic_config_t){
        .alloc = cl->alloc, .tls = cl->cfg.tls,
        .alpn = { .ptr = (const proven_byte_t *)"h3", .size = 2 },   /* offered alone, whatever else the configuration offers over TCP (RFC 9114 section 3.1) */
        .stream_window = (proven_u32)cl->cfg.http3_window_bytes, .conn_window = (proven_u32)cl->cfg.http3_window_bytes,
        /* No window configured: it starts at the engine's and grows while bodies are read (RFC-0019 section 9). */
        .stream_window_max = cl->cfg.http3_window_bytes ? 0u : PROVEN_QUIC_STREAM_WINDOW_MAX,
        .conn_window_max = cl->cfg.http3_window_bytes ? 0u : PROVEN_QUIC_CONN_WINDOW_MAX,
        .idle_timeout_ms = (cl->cfg.idle_timeout_ms > cl->cfg.connect_timeout_ms ? cl->cfg.idle_timeout_ms : cl->cfg.connect_timeout_ms) + 1000,
    };
}

/* A Version Negotiation packet named a version this client speaks and not the one it started in
 * (RFC 9368 section 2.1; RFC-0036): the attempt starts again in it, once, on the same socket -
 * the same connection to its requests and to RFC-0031's fallback timer. Not when a request is
 * on a stream already (early data): those end as the close says. */
static bool c_restart(c3conn_t *c) {
    proven_quic_stats_t st;
    proven_quic_conn_stats(c->q, &st);
    if (c->restarted || st.vn_version == 0 || st.vn_version == st.version) return false;
    for (const proven_c3req_t *r = c->reqs; r; r = r->next) if (r->has_id) return false;
    proven_http_event_client_t *cl = c->client;
    proven_quic_config_t qc = c_qconfig(cl);
    qc.version = st.vn_version;
    qc.after_vn = true;
    proven_quic_conn_t *q = (void *)0;
    proven_h3_conn_t *h3 = (void *)0;
    const proven_quic_addr_t qa = proven_quic_addr_of_(&c->to);
    proven_err_t e = proven_quic_conn_client_create(&qc, (proven_u8str_view_t){ .ptr = c->host, .size = c->host_len }, &c->session, &qa, c3_now_us(), &q);
    if (e == PROVEN_OK) {
        const proven_u32 list = cl->cfg.max_head_bytes > ((proven_size_t)1 << 20) ? (proven_u32)1 << 20 : (proven_u32)cl->cfg.max_head_bytes;
        const proven_h3_config_t hc = { .alloc = cl->alloc, .server = false, .max_field_section_bytes = list, .qpack_capacity = (proven_u32)cl->cfg.http3_header_table_bytes };
        e = proven_h3_conn_create(&hc, q, &h3);
    }
    if (e != PROVEN_OK) { proven_quic_conn_destroy(q); return false; }
    proven_h3_conn_destroy(c->h3);
    proven_quic_conn_destroy(c->q);
    c->q = q;
    c->h3 = h3;
    c->restarted = true;
    c->early = false;
    return true;
}

static proven_err_t c_close_reason(const proven_h3_event_t *ev) {
    if (!ev->app && ev->code >= 0x100 && ev->code <= 0x1ff) {
        const proven_u64 alert = ev->code - 0x100;
        if (alert == 42 || alert == 43 || alert == 44 || alert == 45 || alert == 46 || alert == 48 || alert == 49) return PROVEN_ERR_UNTRUSTED;
        if (alert == 120) return PROVEN_ERR_UNSUPPORTED;
    }
    return PROVEN_ERR_RESET;
}

static void c_event(c3conn_t *c, const proven_h3_event_t *ev) {
    proven_c3req_t *r = ev->kind == PROVEN_H3_EV_HEADERS || ev->kind == PROVEN_H3_EV_DATA || ev->kind == PROVEN_H3_EV_TRAILERS || ev->kind == PROVEN_H3_EV_STREAM_RESET ||
                 ev->kind == PROVEN_H3_EV_STOP_SENDING || ev->kind == PROVEN_H3_EV_STREAM_WRITABLE ? c_find_req(c, ev->stream_id) : (void *)0;
    switch (ev->kind) {
        case PROVEN_H3_EV_HEADERS: if (r) c_response(c, r, ev); break;
        case PROVEN_H3_EV_DATA:
            if (r && !r->head_only) {
                r_enter(r);
                if (ev->end_stream) r->body_over = true;
                if (r_deliver(r, ev->data, ev->end_stream)) { if (ev->end_stream) r_finish(r, PROVEN_OK); else r_expect(r); }
                r_leave(r);
            }
            break;
        case PROVEN_H3_EV_TRAILERS:                    /* kept for proven_http_event_request_trailers, then the body's end */
            if (r && proven_http_trailers_keep_(r->h.client->alloc, &r->in_trailers, ev->fields, ev->field_count, true, false) != PROVEN_OK) { r_finish(r, PROVEN_ERR_NOMEM); break; }
            if (r) { r->body_over = true; r_enter(r); if (r_deliver(r, (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, true)) r_finish(r, PROVEN_OK); r_leave(r); }
            break;
        case PROVEN_H3_EV_STREAM_RESET:
            if (r) {
                r->engine_done = true;
                r->has_id = false;                     /* the engine has let it go */
                /* Not acted on: safe to send again (RFC 9114 section 4.1.1). */
                if (ev->code == PROVEN_H3_REQUEST_REJECTED) r_unsent(r);
                else r_finish(r, ev->too_large ? PROVEN_ERR_OUT_OF_BOUNDS : PROVEN_ERR_RESET);   /* a head over max_head_bytes */
            }
            break;
        case PROVEN_H3_EV_STOP_SENDING:
            /* The server wants no more of the body: with H3_NO_ERROR because its response is
             * enough, and that response goes on; anything else is the end. */
            if (r) {
                r->engine_done = true;
                if (ev->code != PROVEN_H3_NO_ERROR) r_finish(r, PROVEN_ERR_RESET);
            }
            break;
        case PROVEN_H3_EV_STREAM_WRITABLE:
            if (r) {
                r->blocked = false;
                c->write_since = 0;
                r_push(r);                             /* and on_writable from the round that follows */
            }
            break;
        case PROVEN_H3_EV_GOAWAY:
            /* No new request here: those that wait go to another connection to this origin. */
            c->over = true;
            for (bool again = true; again && !c->dead;) {
                again = false;
                for (proven_c3req_t *w = c->reqs; w; w = w->next) if (!w->has_id) { r_unsent(w); again = true; break; }
            }
            break;
        case PROVEN_H3_EV_QUIC:
            if (ev->quic.kind == PROVEN_QUIC_EV_ESTABLISHED) { c->up = true; c_submit_waiting(c); }
            else if (ev->quic.kind == PROVEN_QUIC_EV_EARLY) { c->early = true; c_submit_waiting(c); }
            break;
        case PROVEN_H3_EV_CLOSED:
            if (!c->up && c_restart(c)) break;      /* the same attempt, in another version */
            c->closed = true;
            c->over = true;
            c->closed_why = c->up ? PROVEN_ERR_RESET : c_close_reason(ev);
            c_end_requests(c);
            break;
        default: break;
    }
}

// -----------------------------------------------------------------------------
// The round, and time
// -----------------------------------------------------------------------------

/* What the client's own clock has made due, looked at in every round. */
static void c_check_time(c3conn_t *c) {
    if (c->dead || c->closed) return;
    const proven_http_event_client_config_t *cfg = &c->client->cfg;
    const proven_time_t now = proven_time_monotonic_now();
    for (bool again = true; again && !c->dead;) {
        again = false;
        for (proven_c3req_t *r = c->reqs; r; r = r->next) {
            if (r->due == 0 || r->due > now) continue;
            r_finish(r, PROVEN_ERR_TIMEOUT);
            again = true;
            break;
        }
    }
    if (c->dead) return;
    /* RFC-0031: what the system said, or the time a handshake may take before TCP is tried. */
    if (!c->up && cfg->http3_fallback && c->send_failed != PROVEN_OK) { c_kill(c, c->send_failed); return; }
    if (!c->up && cfg->http3_fallback && now >= c->born + (proven_time_t)cfg->http3_fallback_ms * 1000000) { c_kill(c, PROVEN_ERR_TIMEOUT); return; }
    if (!c->up && now >= c->born + (proven_time_t)cfg->connect_timeout_ms * 1000000) { c_kill(c, PROVEN_ERR_TIMEOUT); return; }
    if (c->write_since != 0 && now >= c->write_since + (proven_time_t)cfg->write_timeout_ms * 1000000) { c_kill(c, PROVEN_ERR_TIMEOUT); return; }
    if (c->up && c->count == 0 && !c->bye && c->idle_since != 0 && now >= c->idle_since + (proven_time_t)cfg->idle_timeout_ms * 1000000) {
        /* Kept with nothing on it for as long as it may be: goodbye, and the close follows. */
        c->bye = true;
        c->over = true;
        if (proven_h3_conn_goaway(c->h3) != PROVEN_OK) proven_quic_conn_close(c->q, true, PROVEN_H3_NO_ERROR);
    }
}

static void c_on_timer(void *ctx) {
    c3conn_t *c = ctx;
    if (c->dead) return;
    c_enter(c);
    c_round(c);
    c_leave(c);
}

static proven_time_t t_min(proven_time_t a, proven_time_t b) { return a == 0 ? b : b == 0 ? a : a < b ? a : b; }

static void c_arm(c3conn_t *c) {
    if (c->dead) return;
    const proven_http_event_client_config_t *cfg = &c->client->cfg;
    const proven_time_t now = proven_time_monotonic_now();
    proven_time_t due = 0;
    if (!c->closed) {
        if (!c->up) due = c->born + (proven_time_t)(cfg->http3_fallback ? cfg->http3_fallback_ms : cfg->connect_timeout_ms) * 1000000;
        if (!c->up && cfg->http3_fallback && c->send_failed != PROVEN_OK) due = now;
        bool waiting = false;
        for (const proven_c3req_t *r = c->reqs; r; r = r->next) {
            if (r->due != 0) due = t_min(due, r->due);
            if (r->blocked) waiting = true;
        }
        if (!waiting) c->write_since = 0;
        else if (c->write_since == 0) c->write_since = now;
        if (c->write_since != 0) due = t_min(due, c->write_since + (proven_time_t)cfg->write_timeout_ms * 1000000);
        if (c->up && c->count == 0 && !c->bye) {
            if (c->idle_since == 0) c->idle_since = now;
            due = t_min(due, c->idle_since + (proven_time_t)cfg->idle_timeout_ms * 1000000);
        } else if (c->count > 0) {
            c->idle_since = 0;
        }
    }
    const proven_u64 q = proven_quic_conn_deadline(c->q);
    if (q != UINT64_MAX) due = t_min(due, (proven_time_t)(q * 1000u));
    if (due == 0) { proven_loop_timer_cancel(c->client->loop, &c->timer); return; }
    const proven_time_t left = due > now ? due - now : 0;
    proven_loop_timer_set(c->client->loop, &c->timer, (proven_u32)((left + 999999) / 1000000), c_on_timer, c);
}

static void c_round(c3conn_t *c) {
    if (c->dead) return;
    c_enter(c);
    c_check_time(c);
    if (!c->dead) {
        const proven_u64 now = c3_now_us();
        if (proven_quic_conn_deadline(c->q) <= now) proven_quic_conn_timeout(c->q, now);
        proven_h3_event_t ev;
        while (!c->dead && !c->closed && proven_h3_conn_poll(c->h3, &ev)) c_event(c, &ev);
        if (!c->dead && !c->closed) {
            c_submit_waiting(c);
            /* A body to be written that was refused before its request was on a stream - or
             * after its room came back - is asked for now, from the loop. A callback may end
             * any request: the walk starts again after each. */
            for (bool again = true; again && !c->dead;) {
                again = false;
                for (proven_c3req_t *r = c->reqs; r; r = r->next) {
                    if (r->dead || !r->has_id || !r->streamed || !r->want_writable || r->blocked || r->end_asked || r->engine_done) continue;
                    r->want_writable = false;
                    if (r->on.on_writable) { r_enter(r); r->on.on_writable(r->ctx, c3_handle(r)); r_leave(r); again = true; break; }
                }
            }
            /* Bodies that wait for nothing but a turn. r_push may end a request: the walk starts again. */
            for (bool again = true; again && !c->dead;) {
                again = false;
                for (proven_c3req_t *r = c->reqs; r; r = r->next) {
                    if (!r->has_id || r->blocked || r->engine_done || !(r->out_len > 0 || r->end_asked)) continue;
                    r_push(r);
                    again = true;
                    break;
                }
            }
        }
        if (!c->dead) c_send(c);
        if (!c->dead && !c->up && c->client->cfg.http3_fallback && c->send_failed != PROVEN_OK) c_kill(c, c->send_failed);
        if (!c->dead && c->closed && c_over(c)) c_kill(c, c->closed_why);
        else if (!c->dead) c_arm(c);
    }
    if (!c->dead && c->count == 0 && c->client->cfg.max_connections != 0 && (c->client->wait_head || c->client->h2_roomless || c->client->h3_roomless))
        proven_http_event_client_freed_(c->client);
    c_leave(c);
}

static void c_sock_received(void *ctx, proven_err_t err, proven_size_t n, proven_size_t seg, proven_u8 ecn) {
    c3conn_t *c = ctx;
    if (err != PROVEN_OK && !c->dead && !c->up && c->client->cfg.http3_fallback && (err == PROVEN_ERR_UNREACHABLE || err == PROVEN_ERR_REFUSED)) {
        c_kill(c, err);                            /* RFC-0031: the system says nobody is there */
        return;
    }
    if (err != PROVEN_OK || c->dead) return;
    proven_http_event_client_t *cl = c->client;
    c_enter(c);
    const proven_quic_addr_t qa = proven_quic_addr_of_(&c->from);
    /* Several datagrams of one sender coalesced by the kernel (GRO): each as if alone. */
    const proven_size_t step = seg ? seg : n;
    bool failed = false;
    for (proven_size_t off = 0; off < n && !c->dead; off += step) {
        const proven_size_t k = n - off < step ? n - off : step;
        if (proven_quic_conn_receive(c->q, (proven_mem_view_t){ .ptr = c->rbuf + off, .size = k }, &qa, ecn, c3_now_us()) != PROVEN_OK) { failed = true; break; }
    }
    if (failed) c_kill(c, PROVEN_ERR_NOMEM);
    /* The next receive, armed from here: what waits already is taken without a new round. The
     * connection's round comes when the datagrams that came together are in. */
    if (!c->dead) (void)proven_loop_udp_recv_(cl->loop, &c->lu, (proven_mem_mut_t){ .ptr = c->rbuf, .size = CL_READ_BYTES }, &c->from);
    c_call_back(c);
    c_leave(c);
}

static void c_sock_writable(void *ctx, proven_err_t err) {
    c3conn_t *c = ctx;
    c->waiting = false;
    if (err != PROVEN_OK || c->dead) return;
    c_enter(c);
    if (c->pend_len > 0) {
        proven_size_t sent = 0;
        const proven_err_t e = proven_net_udp_send_marked_(&c->udp, c->to, (proven_mem_view_t){ .ptr = c->pend, .size = c->pend_len }, c->pend_seg, c->pend_ecn, &sent);
        if (e != PROVEN_ERR_TIMEOUT) c->pend_len = 0;
        else if (sent > 0) { proven_sys_mem_move(c->pend, c->pend + sent, c->pend_len - sent); c->pend_len -= sent; }
    }
    c_round(c);
    c_leave(c);
}

static void c_sock_closed(void *ctx) {
    c3conn_t *c = ctx;
    proven_http_event_client_t *cl = c->client;
    c->sock_gone = true;
    if (c->dead && c->depth == 0) c_free(c);
    proven_http_event_client_ended_(cl);
}

static const proven_loop_udp_fns_t c_udp_fns = { .received = c_sock_received, .writable = c_sock_writable, .closed = c_sock_closed };

// -----------------------------------------------------------------------------
// Connections, and placing a request
// -----------------------------------------------------------------------------

static c3conn_t *c_find(proven_http_event_client_t *cl, proven_u8str_view_t host, proven_u16 port, const proven_net_addr_t *to) {
    for (c3conn_t *k = cl->h3; k; k = k->next) {
        if (k->dead || k->over || k->closed || k->port != port || !c3_same_addr(&k->to, to)) continue;
        if (!proven_u8str_view_eq_ascii_ci((proven_u8str_view_t){ .ptr = k->host, .size = k->host_len }, host)) continue;
        return k;
    }
    return (void *)0;
}

static proven_err_t c_open(proven_http_event_client_t *cl, proven_u8str_view_t host, proven_u16 port, const proven_net_addr_t *to, c3conn_t **out) {
    if (!proven_http_event_client_room_(cl)) return PROVEN_ERR_BUSY;
    c3conn_t *c = c3_alloc(cl->alloc, sizeof *c);
    if (!c) return PROVEN_ERR_NOMEM;
    c->client = cl;
    c->port = port;
    c->to = *to;
    c->host_len = (proven_u8)host.size;
    for (proven_size_t i = 0; i < host.size; ++i) c->host[i] = host.ptr[i];
    c->born = proven_time_monotonic_now();
    c->closed_why = PROVEN_ERR_RESET;
    proven_err_t e = proven_net_udp_open(proven_net_addr_any(to->family, 0), &c->udp, (void *)0);
    if (e == PROVEN_OK) { (void)proven_net_udp_gro_(&c->udp); (void)proven_net_udp_dontfrag_(&c->udp); c->ecn = proven_net_udp_ecn_(&c->udp); }
    if (e == PROVEN_OK) {
        const proven_quic_config_t qc = c_qconfig(cl);
        const proven_quic_addr_t qa = proven_quic_addr_of_(to);
        /* An older connection to this origin, closing still, may have had a ticket since its last
         * request ended: it goes to the cache now, before this one is given what is there - a
         * ticket that carried early data once is refused it again (RFC-0021). */
        for (c3conn_t *o = cl->h3; o; o = o->next) {
            if (o == c || o->dead || !o->q || o->port != c->port || !proven_u8str_view_eq_ascii_ci((proven_u8str_view_t){ .ptr = o->host, .size = o->host_len }, host)) continue;
            proven_http_event_client_session_keep_(cl, (proven_u8str_view_t){ .ptr = o->host, .size = o->host_len }, o->port, true, &o->session, o->session_mark,
                                                   &o->session_stored, o->up, proven_quic_conn_resumed(o->q));
        }
        c->session_mark = proven_http_event_client_session_start_(cl, host, c->port, true, &c->session);    /* RFC-0020 */
        c->session_stored = c->session_mark;
        e = proven_quic_conn_client_create(&qc, host, &c->session, &qa, c3_now_us(), &c->q);
        if (e == PROVEN_OK) {
            const proven_u32 list = cl->cfg.max_head_bytes > ((proven_size_t)1 << 20) ? (proven_u32)1 << 20 : (proven_u32)cl->cfg.max_head_bytes;
            const proven_h3_config_t hc = { .alloc = cl->alloc, .server = false, .max_field_section_bytes = list, .qpack_capacity = (proven_u32)cl->cfg.http3_header_table_bytes };
            e = proven_h3_conn_create(&hc, c->q, &c->h3);
        }
        if (e == PROVEN_OK) {
            c->rbuf = c3_alloc(cl->alloc, CL_READ_BYTES);
            if (!c->rbuf) e = PROVEN_ERR_NOMEM;
        }
        if (e == PROVEN_OK) e = proven_loop_udp_attach_(cl->loop, &c->lu, &c->udp, &c_udp_fns, c);
        if (e == PROVEN_OK) {
            c->attached = true;
            e = proven_loop_udp_recv_(cl->loop, &c->lu, (proven_mem_mut_t){ .ptr = c->rbuf, .size = CL_READ_BYTES }, &c->from);
        }
    }
    if (e != PROVEN_OK) {
        proven_h3_conn_destroy(c->h3);
        proven_quic_conn_destroy(c->q);
        c->h3 = (void *)0; c->q = (void *)0;
        if (c->attached) {                         /* the loop's `closed` frees it */
            c->sock_closed = true;
            cl->ending++;
            proven_loop_udp_close_(cl->loop, &c->lu);
            (void)proven_net_udp_close(&c->udp);
            c->dead = true;
        } else {
            (void)proven_net_udp_close(&c->udp);
            c_free(c);
        }
        return e == PROVEN_ERR_NOMEM ? e : PROVEN_ERR_UNREACHABLE;
    }
    c->next = cl->h3;
    if (c->next) c->next->prev = c;
    cl->h3 = c;
    c_call_back(c);                                /* the first flight leaves from the loop */
    *out = c;
    return PROVEN_OK;
}

static void r_room_fire(void *ctx) {
    proven_c3req_t *r = ctx;
    if (r->dead) return;
    r_finish(r, PROVEN_ERR_TIMEOUT);               /* no room for a connection within connect_timeout_ms */
}

/* Onto the connection for its origin, made if need be; or, with no room for one, into the wait. */
static void r_place(proven_c3req_t *r) {
    proven_http_event_client_t *cl = r->h.client;
    const proven_u8str_view_t host = { .ptr = r->host, .size = r->host_len };
    c3conn_t *c = c_find(cl, host, r->port, &r->to);
    proven_err_t e = PROVEN_OK;
    if (!c) e = cl->h3_roomless ? PROVEN_ERR_BUSY : c_open(cl, host, r->port, &r->to, &c);
    if (e == PROVEN_ERR_BUSY && cl->cfg.max_connections != 0) {
        if (cl->h3_roomless_count >= cl->cfg.max_waiting_requests) { r_finish(r, PROVEN_ERR_BUSY); return; }
        r->waiting_room = true;
        if (cl->h3_roomless_last) ((proven_c3req_t *)cl->h3_roomless_last)->wnext = r;
        else cl->h3_roomless = r;
        cl->h3_roomless_last = r;
        cl->h3_roomless_count++;
        proven_loop_timer_set(cl->loop, &r->wait_timer, cl->cfg.connect_timeout_ms, r_room_fire, r);
        return;
    }
    if (e != PROVEN_OK) { r_finish(r, e); return; }
    r_attach(r, c);
    c_call_back(c);
}

static void r_wait_fire(void *ctx) {
    proven_c3req_t *r = ctx;
    if (r->dead) return;
    r_undelay(r);
    r_place(r);
}

// -----------------------------------------------------------------------------
// What http_event_client.c calls
// -----------------------------------------------------------------------------

/* A request asked over HTTP/3 started over TCP at once: the caller gets that request. */
static proven_err_t r_start_tcp(proven_http_event_client_t *cl, const proven_http_event_request_options_t *o, proven_net_addr_t to, proven_http_event_request_t **out) {
    proven_http_event_request_options_t t = *o;
    t.http3 = false;
    t.address = &to;
    return proven_http_event_client_start(cl, &t, out);
}

proven_err_t proven_http_event_client_h3_start_(proven_http_event_client_t *cl, const proven_http_event_request_options_t *o,
                                                const proven_url_t *url, proven_net_addr_t to, proven_http_event_request_t **out) {
    if (out) *out = (void *)0;
    const proven_u8str_view_t host = url->host;
    const proven_u16 port = url->has_port ? url->port : 443;
    if (host.size == 0 || host.size > 255) return PROVEN_ERR_INVALID_ARG;
    const proven_u8str_view_t method = o->method.size ? o->method : c3_lit("GET");
    if (proven_u8str_view_eq_ascii_ci(method, c3_lit("CONNECT"))) return PROVEN_ERR_INVALID_ARG;
    /* An origin whose QUIC connection did not come up lately: over TCP from the start (RFC-0031). */
    if (cl->cfg.http3_fallback && c3_gone(cl, host, port)) return r_start_tcp(cl, o, to, out);
    c3conn_t *c = c_find(cl, host, port, &to);
    if (c) {
        proven_size_t waiting = 0;
        for (const proven_c3req_t *w = c->reqs; w; w = w->next) if (!w->has_id) waiting++;
        if (waiting >= cl->cfg.max_waiting_requests) return PROVEN_ERR_BUSY;
    }
    proven_size_t bytes = o->url.size + method.size + o->header_count * sizeof(proven_http_header_t) + o->body.size;
    for (proven_size_t i = 0; i < o->header_count; ++i) bytes += o->headers[i].name.size + o->headers[i].value.size;
    const bool behind = bytes <= C3_COPY_BEHIND;
    proven_c3req_t *r = c3_alloc(cl->alloc, sizeof *r + (behind ? bytes : 0));
    proven_byte_t *copy = !r ? (void *)0 : behind ? (proven_byte_t *)(r + 1) : c3_alloc(cl->alloc, bytes ? bytes : 1);
    if (!r || !copy) {
        if (r) cl->alloc.free_fn(cl->alloc.ctx, r);
        return PROVEN_ERR_NOMEM;
    }
    r->h = (proven_cl_head_t){ .client = cl, .kind = CL_KIND_H3 };
    r->on = o->on;
    r->ctx = o->ctx;
    r->copy = behind ? (void *)0 : copy;
    r->headers = (proven_http_header_t *)(void *)copy;
    r->header_count = o->header_count;
    proven_size_t at = o->header_count * sizeof(proven_http_header_t);
    #define C3_KEEP(view_ptr, view_size, dst) do { for (proven_size_t k_ = 0; k_ < (view_size); ++k_) copy[at + k_] = (view_ptr)[k_]; (dst).ptr = copy + at; (dst).size = (view_size); at += (view_size); } while (0)
    C3_KEEP(o->url.ptr, o->url.size, r->url);
    if (url->path.size > 0) {
        const proven_size_t from = (proven_size_t)(url->path.ptr - o->url.ptr);
        const proven_size_t to_ = url->has_query ? (proven_size_t)(url->query.ptr + url->query.size - o->url.ptr) : from + url->path.size;
        r->target = (proven_u8str_view_t){ .ptr = r->url.ptr + from, .size = to_ - from };
    } else {
        r->query_alone = url->has_query;
    }
    if (r->query_alone) { if (!behind) cl->alloc.free_fn(cl->alloc.ctx, copy); cl->alloc.free_fn(cl->alloc.ctx, r); return PROVEN_ERR_INVALID_ARG; }
    r->host_is_ipv6 = url->host_is_ipv6;
    C3_KEEP(method.ptr, method.size, r->method);
    for (proven_size_t i = 0; i < o->header_count; ++i) {
        C3_KEEP(o->headers[i].name.ptr, o->headers[i].name.size, r->headers[i].name);
        C3_KEEP(o->headers[i].value.ptr, o->headers[i].value.size, r->headers[i].value);
    }
    C3_KEEP(o->body.ptr, o->body.size, r->body);
    #undef C3_KEEP
    r->body_length = o->body_length;
    r->streamed = o->body.size == 0 && o->body_length != 0;
    r->to = to;
    r->port = port;
    r->host_len = (proven_u8)host.size;
    for (proven_size_t i = 0; i < host.size; ++i) r->host[i] = host.ptr[i];
    if (!c) {
        proven_err_t e = cl->h3_roomless ? PROVEN_ERR_BUSY : c_open(cl, host, port, &to, &c);
        if (e == PROVEN_ERR_BUSY && cl->cfg.max_connections != 0) {
            if (cl->h3_roomless_count >= cl->cfg.max_waiting_requests) e = PROVEN_ERR_BUSY;
            else {
                cl->request_count++;
                if (out) *out = c3_handle(r);
                r->waiting_room = true;
                if (cl->h3_roomless_last) ((proven_c3req_t *)cl->h3_roomless_last)->wnext = r;
                else cl->h3_roomless = r;
                cl->h3_roomless_last = r;
                cl->h3_roomless_count++;
                proven_loop_timer_set(cl->loop, &r->wait_timer, cl->cfg.connect_timeout_ms, r_room_fire, r);
                return PROVEN_OK;
            }
        }
        if (e != PROVEN_OK) {
            r_free_parts(r);
            cl->alloc.free_fn(cl->alloc.ctx, r);
            if (e == PROVEN_ERR_UNREACHABLE && cl->cfg.http3_fallback) return r_start_tcp(cl, o, to, out);    /* no UDP socket to be had */
            return e;
        }
    }
    r_attach(r, c);
    cl->request_count++;
    if (out) *out = c3_handle(r);
    c_call_back(c);                                /* onto a stream in the round, never from here */
    return PROVEN_OK;
}

proven_size_t proven_http_event_client_h3_conns_(const proven_http_event_client_t *cl) {
    proven_size_t n = 0;
    for (const c3conn_t *k = cl->h3; k; k = k->next) if (!k->dead) n++;
    return n;
}

bool proven_http_event_client_h3_close_idle_(proven_http_event_client_t *cl) {
    for (c3conn_t *k = cl->h3; k; k = k->next) {
        if (k->dead || k->count != 0) continue;
        k->client_going = true;
        if (!k->closed) { proven_quic_conn_close(k->q, true, PROVEN_H3_NO_ERROR); c_send(k); }
        c_kill(k, PROVEN_OK);
        return true;
    }
    return false;
}

bool proven_http_event_client_h3_waiting_(const proven_http_event_client_t *cl) { return cl->h3_roomless != (void *)0; }

void proven_http_event_client_h3_freed_(proven_http_event_client_t *cl) {
    if (cl->closing || cl->h3_room_busy) return;
    cl->h3_room_busy = true;
    while (cl->h3_roomless) {
        proven_c3req_t *r = cl->h3_roomless;
        const proven_u8str_view_t host = { .ptr = r->host, .size = r->host_len };
        c3conn_t *c = c_find(cl, host, r->port, &r->to);
        if (!c) {
            const proven_err_t e = c_open(cl, host, r->port, &r->to, &c);
            if (e == PROVEN_ERR_BUSY) break;
            if (e != PROVEN_OK) { r_finish(r, e); continue; }
        }
        r_unroom(r);
        proven_loop_timer_cancel(cl->loop, &r->wait_timer);
        r_attach(r, c);
        c_call_back(c);
    }
    cl->h3_room_busy = false;
}

void proven_http_event_client_h3_destroy_(proven_http_event_client_t *cl) {
    /* Those that wait for a try are on no connection: they may be started again. */
    while (cl->h3_delayed) r_finish(cl->h3_delayed, PROVEN_ERR_AGAIN);
    while (cl->h3_roomless) r_finish(cl->h3_roomless, PROVEN_ERR_RESET);
    while (cl->h3) {
        c3conn_t *c = cl->h3;
        c->client_going = true;
        c->closed_why = PROVEN_ERR_RESET;
        if (!c->closed) { proven_quic_conn_close(c->q, true, PROVEN_H3_NO_ERROR); c_send(c); }
        c_kill(c, PROVEN_ERR_RESET);
    }
    if (cl->h3_batch) { cl->alloc.free_fn(cl->alloc.ctx, cl->h3_batch); cl->h3_batch = (void *)0; }
    if (cl->h3_gone) { cl->alloc.free_fn(cl->alloc.ctx, cl->h3_gone); cl->h3_gone = (void *)0; }
}

void proven_http_event_client_h3_test_forget_(proven_http_event_client_t *client) {
    if (client && client->h3_gone) proven_sys_mem_set(client->h3_gone, 0, C3_GONE * sizeof(c3gone_t));
}

int proven_http_event_client_h3_protocol_(const proven_http_event_request_t *request) {
    const proven_c3req_t *r = c3_of_const(request);
    return r->tcp ? proven_http_event_request_protocol(r->tcp) : 3;
}

static void call_begin(proven_c3req_t *r) { r_enter(r); if (r->conn) { c_enter(r->conn); r->conn->calls++; } }
/* True when the request is still there after the call's sending, which can end it (on_done has
 * then been called): read before the leave, which may free it. */
static bool call_end(proven_c3req_t *r, c3conn_t *c) {
    if (c) { c->calls--; if (!c->dead) { c_send(c); c_call_back(c); } c_leave(c); }
    const bool alive = !r->dead;
    r_leave(r);
    return alive;
}

void proven_http_event_client_h3_test_retire_(proven_http_event_client_t *client) {
    for (c3conn_t *c = client->h3; c; c = c->next) c->over = true;
}

bool proven_http_event_client_h3_test_ticket_new_(proven_http_event_client_t *client) {
    const c3conn_t *c = client->h3;                    /* the newest first */
    return c && !c->dead && proven_quic_conn_ticket_stored_(c->q);
}

int proven_http_event_client_h3_early_(proven_http_event_request_t *request) {
    proven_c3req_t *r = c3_of(request);
    if (r->tcp) return 0;
    if (r->early_again) return 3;
    if (!r->sent_early || !r->conn || !r->conn->q) return 0;
    proven_quic_stats_t st;
    proven_quic_conn_stats(r->conn->q, &st);
    return st.early == 1 ? 1 : st.early == 2 ? 2 : 0;
}

bool proven_http_event_client_h3_resumed_(proven_http_event_request_t *request) {
    proven_c3req_t *r = c3_of(request);
    if (r->tcp) return proven_http_event_request_resumed_(r->tcp);
    return r->conn && r->conn->q && proven_quic_conn_resumed(r->conn->q);
}

proven_result_size_t proven_http_event_client_h3_write_(proven_http_event_request_t *request, proven_mem_view_t data) {
    proven_c3req_t *r = c3_of(request);
    if (r->tcp) return proven_http_event_request_write(r->tcp, data);
    if (r->dead || !r->streamed || r->end_asked) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_STATE, .value = 0 };
    if (r->body_length != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN && data.size > (r->has_id ? r->send_left : r->body_length)) return (proven_result_size_t){ .err = PROVEN_ERR_OUT_OF_BOUNDS, .value = 0 };
    if (!r->has_id || r->blocked || r->engine_done) { r->want_writable = true; return (proven_result_size_t){ .err = PROVEN_OK, .value = 0 }; }
    c3conn_t *c = r->conn;
    call_begin(r);
    proven_size_t taken = 0;
    const proven_err_t e = proven_h3_send_data(c->h3, r->id, data, false, &taken);
    bool alive = true;
    if (e != PROVEN_OK) { r_finish(r, PROVEN_ERR_RESET); alive = false; }
    else {
        if (taken > 0) c->write_since = 0;
        if (r->body_length != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) r->send_left -= taken;
        if (taken < data.size) { r->blocked = true; r->want_writable = true; }
    }
    if (!call_end(r, c)) alive = false;
    return alive ? (proven_result_size_t){ .err = PROVEN_OK, .value = taken } : (proven_result_size_t){ .err = PROVEN_ERR_RESET, .value = 0 };
}

proven_err_t proven_http_event_client_h3_trailers_(const proven_http_event_request_t *request, proven_http_header_t *fields, proven_size_t cap, proven_size_t *count) {
    const proven_c3req_t *r = (const proven_c3req_t *)(const void *)request;
    if (!r->body_over) return PROVEN_ERR_INVALID_STATE;
    return proven_http_trailers_give_(&r->in_trailers, fields, cap, count);
}

proven_err_t proven_http_event_client_h3_end_(proven_http_event_request_t *request, const proven_http_header_t *trailers, proven_size_t count) {
    proven_c3req_t *r = c3_of(request);
    if (r->tcp) {
        r_enter(r);
        const proven_err_t e = count > 0 ? proven_http_event_request_end_trailers(r->tcp, trailers, count) : proven_http_event_request_end(r->tcp);
        r_leave(r);
        return e;
    }
    if (r->dead || !r->streamed || r->end_asked) return PROVEN_ERR_INVALID_STATE;
    if (count > 0) {
        /* As over HTTP/1.1: after a body written with no length. Kept until the body has gone. */
        if (r->body_length != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) return PROVEN_ERR_INVALID_STATE;
        if (!proven_http_trailers_valid_(trailers, count)) return PROVEN_ERR_INVALID_ARG;
        const proven_err_t e = proven_http_trailers_keep_(r->h.client->alloc, &r->out_trailers, trailers, count, false, true);
        if (e != PROVEN_OK) return e;
        if (!r->has_id) {
            /* Not on a stream yet: its HEADERS go without the end, and the trailers after them. */
            r->end_asked = true;
            return PROVEN_OK;
        }
    }
    if (r->body_length != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN && (r->has_id ? r->send_left : r->body_length) != 0) {
        r_finish(r, PROVEN_ERR_INVALID_FORMAT);
        return PROVEN_ERR_INVALID_FORMAT;
    }
    if (!r->has_id) {
        /* Ended before it was ever on a stream: it had no body after all. */
        r->streamed = false;
        r->body_length = 0;
        return PROVEN_OK;
    }
    c3conn_t *c = r->conn;
    call_begin(r);
    r->end_asked = true;
    r_push(r);
    const proven_err_t result = r->dead ? PROVEN_ERR_RESET : PROVEN_OK;
    call_end(r, c);
    return result;
}

void proven_http_event_client_h3_abort_(proven_http_event_request_t *request) {
    proven_c3req_t *r = c3_of(request);
    if (r->tcp) { r_enter(r); proven_http_event_request_abort(r->tcp); r_leave(r); return; }
    if (r->dead) return;
    c3conn_t *c = r->conn;
    call_begin(r);
    r_finish(r, PROVEN_ERR_RESET);
    call_end(r, c);
}

void proven_http_event_client_h3_pause_(proven_http_event_request_t *request) {
    proven_c3req_t *r = c3_of(request);
    if (r->tcp) { proven_http_event_request_pause(r->tcp); return; }
    if (r->dead || r->paused) return;
    r->paused = true;
    r->due = 0;
    if (r->conn && r->has_id && !r->conn->closed) proven_h3_stream_pause(r->conn->h3, r->id, true);
}

void proven_http_event_client_h3_resume_(proven_http_event_request_t *request) {
    proven_c3req_t *r = c3_of(request);
    if (r->tcp) { proven_http_event_request_resume(r->tcp); return; }
    if (r->dead || !r->paused) return;
    r->paused = false;
    r_expect(r);
    if (r->conn && r->has_id && !r->conn->closed) { proven_h3_stream_pause(r->conn->h3, r->id, false); c_call_back(r->conn); }
}

void proven_http_event_client_h3_set_user_(proven_http_event_request_t *request, void *user) { c3_of(request)->user = user; }
void *proven_http_event_client_h3_user_(const proven_http_event_request_t *request) { return c3_of_const(request)->user; }

#endif
