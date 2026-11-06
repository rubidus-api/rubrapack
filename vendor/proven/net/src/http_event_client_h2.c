#include "proven/http_event_client.h"
#include "../platform/proven_sys_net_mem.h"
#include "proven_internal_http_event_client.h"
#include "proven_internal_tls.h"
#include "proven_internal_loop.h"
#include "proven_internal_http_event.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven/h2.h"
#include "proven/time.h"
#include "proven/url.h"

/*
 * HTTP/2 in the event-driven client.
 *
 * An HTTP/1.1 request of this client is its own connection. HTTP/2 is the opposite bargain -
 * many requests on one connection - so the client keeps a connection for each origin it has
 * spoken HTTP/2 to (c2conn_t) and a request is a small object on it (proven_c2req_t). The protocol is
 * proven_h2_conn_t (h2.h); this file is its driver: the socket, TLS, time, and the callbacks.
 *
 * Whether an origin speaks HTTP/2 is known only when the TLS handshake has chosen (ALPN). A
 * request started before that waits on the connection, holding a copy of what it was started
 * with. If the answer is HTTP/1.1, the origin is remembered as such, and every waiting request
 * starts again as an ordinary HTTP/1.1 request - which the caller goes on knowing by the
 * object it was first given (`fwd`, and `face` on the other side).
 *
 * The rules the server's driver learned apply here unchanged: no callback from inside a call
 * the handler made but its own on_done; the peer's window and this side's bound are different
 * reasons for a refused write; the engine's pending output is the only queue; each request
 * has its own deadline and one timer is set for the earliest.
 */

#define C2_CONNECTING 0     /* the connect has not finished */
#define C2_HANDSHAKE 1      /* connected; TLS has not said which protocol */
#define C2_UP 2             /* HTTP/2 */
#define C2_H1 3             /* this origin speaks HTTP/1.1: remembered, with no connection */
#define C2_MAX_REMEMBERED 64u   /* such memories kept; beyond that the oldest is forgotten, and costs a handshake to learn again */
#define C2_COPY_BEHIND 1024u    /* a request's copied URL, fields and body up to this size share its allocation */

typedef struct c2conn c2conn_t;

typedef struct c2req {
    proven_cl_head_t h;                       /* first: what http_event_client.c reads */
    c2conn_t *conn;                    /* not to be followed once `dead` or forwarded */
    struct c2req *prev, *next;
    proven_http_event_request_t *fwd;  /* the HTTP/1.1 request this became */
    proven_http_event_client_callbacks_t on;
    void *ctx, *user;
    proven_byte_t *copy;               /* what it was started with, until it has been sent or handed on; null when that is small and lies behind this struct */
    proven_u8str_view_t url, method;
    proven_u8str_view_t target;        /* the path and query within `url`: empty for none */
    proven_http_header_t *headers;
    proven_size_t header_count;
    proven_mem_view_t body;
    proven_u64 body_length;
    proven_byte_t *out;                /* request body not yet taken by the engine */
    proven_size_t out_len, out_off, out_cap;
    proven_byte_t *held;               /* response body that arrived while paused, as it arrived */
    proven_size_t held_len, held_cap;
    proven_http_decoder_t_ *dec;       /* with a piece buffer behind it */
    proven_u64 send_left, got;
    proven_time_t due;                 /* 0: no deadline is running */
    proven_u32 id, mark;               /* id 0: not yet on a stream */
    proven_u32 tries;                  /* times it was put to the server again (retry_unsent) */
    bool dead, streamed, end_asked, engine_done, paused, want_writable, window_shut;
    struct c2delay *delay;             /* waiting to be tried again: on no connection, and this says where it goes */
    bool got_head, head_only, held_last, any_decoded;
    bool host_is_ipv6, query_alone;    /* of the URL, read once when the request was started */
    /* 0-RTT over TCP (RFC-0027): it may go as early data (GET, HEAD or OPTIONS with no body); not
     * again (answered 425); it went; what came of it (proven_http_event_client_early_). */
    bool early_ok, no_early, went_early;
    proven_u8 early_result;
    bool body_over;                    /* the response's end has arrived (proven_http_event_request_trailers) */
    proven_http_trailers_t_ in_trailers;   /* the response's, until the struct goes */
    proven_http_trailers_t_ out_trailers;  /* the request's, sent in place of END_STREAM once `out` has drained */
} proven_c2req_t;

struct c2conn {
    proven_http_event_client_t *client;
    c2conn_t *prev, *next;
    proven_net_conn_t sock;
    proven_loop_sock_t ls;             /* the connection as the loop knows it (RFC-0025) */
    proven_loop_timer_t timer;
    proven_loop_after_t after;         /* "call me back": a round, from the loop, when its present round is delivered */
    proven_tls_conn_t *tls;
    proven_tls_session_t session;      /* this connection's copy of the origin's session (RFC-0020) */
    proven_u64 session_mark;           /* what it was given */
    proven_u64 session_stored;         /* what it last stored, or was given */
    proven_h2_conn_t *engine;
    proven_c2req_t *reqs;
    proven_c2req_t *turn;                     /* the request whose output was moved last in a round: the next round goes on behind it */
    proven_size_t share;               /* while `sharing`: how much more the request whose turn it is may move */
    bool sharing;
    proven_size_t count;               /* requests on it, waiting or under way */
    proven_net_addr_t to;
    proven_time_t quiet_due;
    proven_time_t req_due;             /* no later than the earliest hour among its requests (0: none): see c_arm */
    proven_u32 depth, round, calls;
    proven_u16 port;
    proven_u8 state, host_len;
    bool https, dead, pumping, reading, over, peer_eof, more_out;
    bool attached;                     /* given to the loop: freed only after its `closed` */
    bool sock_closed;                  /* proven_loop_close was called */
    bool sock_gone;                    /* and `closed` has come */
    bool recv_armed;                   /* a receive of zero bytes is in flight */
    bool client_going;                 /* ended by proven_http_event_client_destroy, not by anything the server did */
    /* 0-RTT over TCP (RFC-0027): the engine is up before the handshake, and what it wrote went as
     * early data - kept here, to be written again should the server refuse it. */
    bool early;
    proven_byte_t *early_copy;
    proven_size_t early_copy_len, early_copy_cap;
    proven_byte_t host[256];
};

/* The engine's clock for growing windows: microseconds. */
static proven_u64 h2_now_us(void) { return (proven_u64)proven_time_monotonic_now() / 1000u; }



static void *c2_alloc(proven_allocator_t a, proven_size_t n) {
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, n, 16);
    return m.err == PROVEN_OK ? m.value.ptr : (void *)0;
}

static bool c2_append(proven_allocator_t a, proven_byte_t **buf, proven_size_t *len, proven_size_t *cap, const proven_byte_t *p, proven_size_t n) {
    if (n == 0) return true;
    if (*len + n > *cap) {
        proven_size_t want = *cap ? *cap : 256;
        while (want < *len + n) { if (want > (proven_size_t)-1 / 2) return false; want *= 2; }
        proven_byte_t *q = c2_alloc(a, want);
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

static proven_http_event_request_t *c2_handle(proven_c2req_t *r) { return (proven_http_event_request_t *)(void *)r; }
static proven_c2req_t *c2_of(proven_http_event_request_t *request) { return (proven_c2req_t *)(void *)request; }
static const proven_c2req_t *c2_of_const(const proven_http_event_request_t *request) { return (const proven_c2req_t *)(const void *)request; }

// -----------------------------------------------------------------------------
// Lifetimes
// -----------------------------------------------------------------------------

static void c_pump(c2conn_t *c);

static void c_enter(c2conn_t *c) { c->depth++; }

static void c_leave(c2conn_t *c) {
    if (--c->depth > 0 || !c->dead) return;
    if (c->attached && !c->sock_gone) return;     /* c_sock_closed frees it */
    proven_allocator_t a = c->client->alloc;
    proven_h2_conn_destroy(c->engine);
    c->engine = (void *)0;
    a.free_fn(a.ctx, c);
}

static void r_enter(proven_c2req_t *r) { r->h.depth++; }

static void r_free_parts(proven_c2req_t *r) {
    proven_allocator_t a = r->h.client->alloc;
    if (r->copy) { a.free_fn(a.ctx, r->copy); r->copy = (void *)0; }
    if (r->out) { a.free_fn(a.ctx, r->out); r->out = (void *)0; r->out_len = r->out_off = r->out_cap = 0; }
    if (r->held) { a.free_fn(a.ctx, r->held); r->held = (void *)0; r->held_len = r->held_cap = 0; }
    if (r->dec) { proven_http_decoder_free_(r->dec); a.free_fn(a.ctx, r->dec); r->dec = (void *)0; }
}

static void r_leave(proven_c2req_t *r) {
    if (--r->h.depth > 0 || !r->dead) return;
    proven_allocator_t a = r->h.client->alloc;
    proven_http_trailers_free_(a, &r->in_trailers);
    proven_http_trailers_free_(a, &r->out_trailers);
    a.free_fn(a.ctx, r);
}

static void r_unlink(proven_c2req_t *r) {
    c2conn_t *c = r->conn;
    if (!c) return;
    if (c->turn == r) c->turn = r->prev;
    if (r->prev) r->prev->next = r->next;
    else c->reqs = r->next;
    if (r->next) r->next->prev = r->prev;
    r->prev = r->next = (void *)0;
    c->count--;
    r->conn = (void *)0;
}

/* The end of a request, good or bad: once. */
static void r_undelay(proven_c2req_t *r);

static void r_finish(proven_c2req_t *r, proven_err_t why) {
    if (r->dead) return;
    c2conn_t *c = r->conn;
    r_enter(r);
    r->dead = true;
    if (r->delay) r_undelay(r);
    if (c && !c->dead && c->engine && r->id != 0) {
        proven_h2_conn_stream_set_user(c->engine, r->id, (void *)0);
        /* A response that is whole, or an end that is not: either way nothing more is to be
         * sent or taken on this stream. */
        if (!r->engine_done || why != PROVEN_OK) (void)proven_h2_conn_reset(c->engine, r->id, PROVEN_H2_CANCEL);
    }
    /* A ticket the connection has had by now is the next connection's to offer (RFC-0020). */
    if (c && !c->dead && c->https && c->tls && why == PROVEN_OK)
        proven_http_event_client_session_keep_(r->h.client, (proven_u8str_view_t){ .ptr = c->host, .size = c->host_len }, c->port, false, &c->session, c->session_mark, &c->session_stored,
                                               proven_tls_is_established(c->tls), proven_tls_resumed(c->tls));
    r_unlink(r);
    r->h.client->request_count--;
    r_free_parts(r);
    r->on.on_done(r->ctx, c2_handle(r), why);
    r_leave(r);
}

static void r_unsent(proven_c2req_t *r, bool here);

static void c_end_requests(c2conn_t *c, proven_err_t why) {
    /* In the clear a connection is HTTP/2 because its caller said so (http2_cleartext), and the
     * server's settings are the first sign that it is. One that ends without them - the bytes
     * that came were not HTTP/2, or none came - was never an HTTP/2 connection, and "start it
     * again" would be the answer that goes round for ever. Over TLS the handshake agreed on
     * the protocol, so a connection lost there was an HTTP/2 connection, lost.
     * (`engine`: it was connected. A connection that could not be made is not judged here,
     * and neither is one that ends because its client is being destroyed.) */
    const bool never_was = !c->https && c->engine && !c->client_going && !proven_h2_conn_has_peer_settings(c->engine);
    while (c->reqs) {
        proven_c2req_t *r = c->reqs;
        /* One the server never saw may be started again; one it may have acted on may not. */
        if (r->id != 0 || why != PROVEN_ERR_RESET) r_finish(r, why);
        else if (never_was) r_finish(r, PROVEN_ERR_UNSUPPORTED);
        else r_unsent(r, false);
    }
}

static void c_close_socket(c2conn_t *c) {
    proven_http_event_client_t *cl = c->client;
    proven_loop_timer_cancel(cl->loop, &c->timer);
    proven_loop_after_cancel_(cl->loop, &c->after);
    if (c->attached && !c->sock_closed) {          /* the loop stops watching before the close */
        c->sock_closed = true;
        cl->ending++;
        proven_loop_close(cl->loop, &c->ls);
    }
    const bool established = c->tls && proven_tls_is_established(c->tls), resumed = established && proven_tls_resumed(c->tls);
    if (c->tls) { proven_tls_conn_destroy(c->tls); c->tls = (void *)0; }
    if (c->early_copy) { cl->alloc.free_fn(cl->alloc.ctx, c->early_copy); c->early_copy = (void *)0; c->early_copy_len = c->early_copy_cap = 0; }
    if (c->https) {
        proven_http_event_client_session_keep_(cl, (proven_u8str_view_t){ .ptr = c->host, .size = c->host_len }, c->port, false, &c->session, c->session_mark, &c->session_stored, established, resumed);
        proven_mem_wipe((proven_mem_mut_t){ .ptr = (proven_byte_t *)(c->session.opaque), .size = sizeof c->session.opaque });
    }
    if (proven_net_conn_is_open(&c->sock)) (void)proven_net_close(&c->sock);
}

/* The connection is finished with: out of the client's list, its requests told. */
static void c_kill(c2conn_t *c, proven_err_t why) {
    if (c->dead) return;
    proven_http_event_client_t *cl = c->client;
    c_enter(c);
    c->dead = true;
    c_close_socket(c);
    if (c->prev) c->prev->next = c->next;
    else cl->h2 = c->next;
    if (c->next) c->next->prev = c->prev;
    c_end_requests(c, why);
    /* A place in the client is free: an HTTP/1.1 request may have been waiting for one. */
    if (!c->client_going) proven_http_event_client_freed_(cl);
    c_leave(c);
}

// -----------------------------------------------------------------------------
// The transport
// -----------------------------------------------------------------------------

/* What the transport holds for the server: the loop's queue (TLS records go there at once). */
static proven_size_t c_held(const c2conn_t *c) {
    return (c->attached && !c->sock_closed ? proven_loop_send_queued(&c->ls) : 0) + (c->tls ? proven_tls_pending_output(c->tls).size : 0);
}

static proven_size_t c_queued(const c2conn_t *c) {
    return (c->engine ? proven_h2_conn_pending_output(c->engine).size : 0) + c_held(c);
}

/* How much the loop may hold for a connection before an offer takes nothing more. */
#define C2_OFFER_HOLD ((proven_size_t)32 * 1024)

static void c_kill(c2conn_t *c, proven_err_t why);

/* Ask the loop for input (a receive of zero bytes, left in flight once armed), and to be told
 * when what it holds has gone - or, with more to offer, when half the hold has. */
static void c_watch(c2conn_t *c) {
    if (c->dead || !c->attached || c->sock_closed || c->state == C2_CONNECTING) return;
    proven_loop_t *loop = c->client->loop;
    if (!c->peer_eof && !c->recv_armed) {          /* read whatever the requests are doing: the windows bound it */
        if (proven_loop_recv(loop, &c->ls, (proven_mem_mut_t){ .ptr = (void *)0, .size = 0 }) != PROVEN_OK) { c_kill(c, PROVEN_ERR_RESET); return; }
        c->recv_armed = true;
    }
    const proven_size_t held = proven_loop_send_queued(&c->ls);
    if (held == 0 && !c->more_out) return;
    if (proven_loop_send_wait(loop, &c->ls, c->more_out ? C2_OFFER_HOLD / 2 : 0) != PROVEN_OK) c_kill(c, PROVEN_ERR_RESET);
}

static void c_pump(c2conn_t *c);
static void c_after(void *ctx) {
    c2conn_t *c = ctx;
    c_enter(c);
    c_pump(c);
    c_leave(c);
}

/* "Call me back": a round for this connection, run from the loop and never from where the
 * caller stands. Inside the connection's own read nothing is asked for - c_sock_received pumps when
 * the read has been worked through. Otherwise the loop runs the round when it has delivered
 * the one it is in, or before it next sleeps: no change of the socket's watch, no system call. */
static void c_call_back(c2conn_t *c) {
    if (c->dead || c->reading) return;
    proven_loop_after_(c->client->loop, &c->after, c_after, c);
}

/* What TLS holds, to the loop. False when the connection ended. */
static bool c_flush_tls(c2conn_t *c) {
    while (c->tls && !c->dead) {
        proven_mem_view_t pending = proven_tls_pending_output(c->tls);
        if (pending.size == 0) break;
        proven_result_size_t w = proven_loop_send(c->client->loop, &c->ls, pending);
        if (w.err != PROVEN_OK || w.value != pending.size) { c_kill(c, w.err == PROVEN_ERR_NOMEM ? PROVEN_ERR_NOMEM : PROVEN_ERR_RESET); return false; }
        proven_tls_output_sent(c->tls, w.value);
    }
    return !c->dead;
}

/* Give `data` to the transport and say how much it took: while the loop holds less than
 * C2_OFFER_HOLD for the connection - straight to the socket when nothing waits there, the rest
 * held by the loop - and nothing more after. */
static proven_size_t c_offer(c2conn_t *c, proven_mem_view_t data) {
    if (c->dead) return 0;
    proven_size_t taken = 0;
    const proven_size_t held = c_held(c);
    if (data.size > 0 && held < C2_OFFER_HOLD) {
        proven_size_t n = C2_OFFER_HOLD - held;
        if (n > data.size) n = data.size;
        if (c->tls) {
            proven_mem_view_t piece = { .ptr = data.ptr, .size = n < 16384 ? n : 16384 };
            const bool early = c->early && !proven_tls_is_established(c->tls);
            if (early && proven_tls_early_room_(c->tls) == 0) return 0;      /* the rest after the handshake */
            proven_result_size_t w = proven_tls_write(c->tls, piece);
            if (w.err != PROVEN_OK) { c_kill(c, PROVEN_ERR_RESET); return 0; }
            taken = w.value;
            if (early && !c2_append(c->client->alloc, &c->early_copy, &c->early_copy_len, &c->early_copy_cap, data.ptr, taken)) { c_kill(c, PROVEN_ERR_NOMEM); return 0; }
            if (!c_flush_tls(c)) return 0;
        } else {
            proven_result_size_t w = proven_loop_send(c->client->loop, &c->ls, (proven_mem_view_t){ .ptr = data.ptr, .size = n });
            if (w.err != PROVEN_OK || w.value != n) { c_kill(c, w.err == PROVEN_ERR_NOMEM ? PROVEN_ERR_NOMEM : PROVEN_ERR_RESET); return 0; }
            taken = n;
        }
    }
    c->more_out = taken < data.size;
    c_watch(c);
    return taken;
}

// -----------------------------------------------------------------------------
// Time
// -----------------------------------------------------------------------------

static void c_on_timer(void *ctx);

/* Whether the connection is still being made, as time counts it: until the server's settings
 * have come. A server that takes the connection - and the TLS handshake - and then says
 * nothing has connect_timeout_ms to say it, like one that never answered the connect. */
static bool c_connecting(const c2conn_t *c) {
    return c->state != C2_UP || c->early || !proven_h2_conn_has_peer_settings(c->engine);
}

static void c_arm(c2conn_t *c) {
    if (c->dead || c->state == C2_H1) return;
    const proven_http_event_client_config_t *cfg = &c->client->cfg;
    proven_u32 ms = 0;
    if (c_connecting(c)) ms = cfg->connect_timeout_ms;
    else if (c_queued(c) > 0) ms = cfg->write_timeout_ms;
    else if (c->count == 0) ms = cfg->idle_timeout_ms;
    const proven_time_t now = proven_time_monotonic_now();
    /* Connecting is one step with one limit: its clock is not started again by progress. */
    if (c_connecting(c) && c->quiet_due != 0) ms = c->quiet_due > now ? (proven_u32)((c->quiet_due - now) / 1000000 + 1) : 1;
    else c->quiet_due = ms ? now + (proven_time_t)ms * 1000000 : 0;
    /* Not the earliest of its requests' hours, which would be a walk of them all at every
     * round, but a time no later than it: at worst the timer wakes early once, and c_on_timer
     * looks at each request and sets this right. */
    const proven_time_t due = c->req_due;
    if (due != 0) {
        const proven_time_t left = due > now ? (due - now) / 1000000 + 1 : 1;
        if (ms == 0 || (proven_time_t)ms > left) ms = (proven_u32)left;
    }
    if (ms == 0) proven_loop_timer_cancel(c->client->loop, &c->timer);
    else proven_loop_timer_set(c->client->loop, &c->timer, ms, c_on_timer, c);
}

/* What a request is waiting for now, and so how long it may wait. */
static void r_expect(proven_c2req_t *r) {
    const proven_http_event_client_config_t *cfg = &r->h.client->cfg;
    proven_u32 ms = 0;
    if (r->id == 0 || r->paused) ms = 0;                   /* the connection's affair, or the caller's */
    else if (!r->got_head) ms = r->end_asked || !r->streamed ? cfg->response_timeout_ms : 0;
    else ms = cfg->body_timeout_ms;
    r->due = ms ? proven_time_monotonic_now() + (proven_time_t)ms * 1000000 : 0;
    if (r->due != 0 && r->conn && (r->conn->req_due == 0 || r->due < r->conn->req_due)) r->conn->req_due = r->due;
}

// -----------------------------------------------------------------------------
// A request's output
// -----------------------------------------------------------------------------

static proven_size_t r_out_pending(const proven_c2req_t *r) { return r->out_len - r->out_off; }

/* What one request moves towards the engine in its turn of a round. Requests that hold a
 * body take turns of this much, so that the room under the output bound - which the first to
 * be asked would otherwise fill by itself, round after round - is shared between them.
 * (While a turn lasts c_room answers with what is left of it, to whoever asks: a caller that
 * writes to another request from inside this one's callback is refused sooner than the bound
 * alone would, and is told when it may write like any other.) */
#define C2_TURN_BYTES ((proven_size_t)65536)

static proven_size_t c_room(const c2conn_t *c) {
    const proven_size_t held = proven_h2_conn_pending_output(c->engine).size, bound = c->client->cfg.max_buffered_output;
    const proven_size_t room = held < bound ? bound - held : 0;
    return c->sharing && c->share < room ? c->share : room;
}

static void r_push(proven_c2req_t *r) {
    if (r->dead || r->id == 0) return;
    c2conn_t *c = r->conn;
    proven_allocator_t a = r->h.client->alloc;
    r_enter(r);
    while (!r->dead && r_out_pending(r) > 0) {
        proven_size_t n = c_room(c);
        if (n == 0) break;
        if (n > r_out_pending(r)) n = r_out_pending(r);
        proven_result_size_t w = proven_h2_conn_send_data(c->engine, r->id, (proven_mem_view_t){ .ptr = r->out + r->out_off, .size = n }, false);
        if (w.err != PROVEN_OK) { r_finish(r, PROVEN_ERR_RESET); break; }
        r->out_off += w.value;
        if (c->sharing) c->share -= w.value < c->share ? w.value : c->share;
        if (w.value < n) { r->window_shut = true; break; }
    }
    if (!r->dead && r_out_pending(r) == 0) {
        if (r->out) { a.free_fn(a.ctx, r->out); r->out = (void *)0; r->out_len = r->out_off = r->out_cap = 0; }
        if (r->end_asked && !r->engine_done) {
            proven_result_size_t w = r->out_trailers.count ? (proven_result_size_t){ .err = proven_h2_conn_send_trailers(c->engine, r->id, r->out_trailers.fields, r->out_trailers.count) }
                                                           : proven_h2_conn_send_data(c->engine, r->id, (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, true);
            if (w.err != PROVEN_OK) r_finish(r, PROVEN_ERR_RESET);
            else { r->engine_done = true; r_expect(r); }
        } else if (!r->end_asked && r->want_writable && !r->window_shut && c_room(c) > 0) {
            if (c->calls > 0) c_call_back(c);                          /* not from inside the caller's own call: the loop comes back */
            else {
                r->want_writable = false;
                if (r->on.on_writable) r->on.on_writable(r->ctx, c2_handle(r));
            }
        }
    }
    r_leave(r);
}

// -----------------------------------------------------------------------------
// Sending a request
// -----------------------------------------------------------------------------

/* Put a waiting request on a stream. PROVEN_ERR_BUSY: the server's limit - it goes on waiting. */
static proven_err_t r_submit(proven_c2req_t *r) {
    c2conn_t *c = r->conn;
    proven_http_event_client_t *cl = c->client;
    /* The URL was read when the request was started; the connection is its origin. */
    if (r->query_alone) return PROVEN_ERR_INVALID_ARG;
    const proven_u8str_view_t path = r->target.size > 0 ? r->target : PROVEN_LIT("/");
    /* :authority is what Host would have been: the port only when it is not the scheme's own. */
    proven_byte_t authority[300], digits[20];
    proven_size_t al = 0, nd = 0;
    if (r->host_is_ipv6) authority[al++] = '[';
    for (proven_size_t i = 0; i < c->host_len; ++i) authority[al++] = proven_ascii_lower(c->host[i]);
    if (r->host_is_ipv6) authority[al++] = ']';
    if (c->port != (c->https ? 443 : 80)) {
        authority[al++] = ':';
        char d[5]; int dn = 0; proven_u16 p = c->port;
        do { d[dn++] = (char)('0' + p % 10); p /= 10; } while (p);
        while (dn > 0) authority[al++] = (proven_byte_t)d[--dn];
    }
    proven_http_header_t small[40], *list = small;
    if (r->header_count + 6 > sizeof small / sizeof small[0]) {
        list = c2_alloc(cl->alloc, (r->header_count + 6) * sizeof *list);
        if (!list) return PROVEN_ERR_NOMEM;
    }
    proven_size_t n = 0, at = 0;
    proven_err_t e = PROVEN_OK;
    list[n++] = (proven_http_header_t){ .name = PROVEN_LIT(":method"), .value = r->method };
    list[n++] = (proven_http_header_t){ .name = PROVEN_LIT(":scheme"), .value = c->https ? PROVEN_LIT("https") : PROVEN_LIT("http") };
    list[n++] = (proven_http_header_t){ .name = PROVEN_LIT(":authority"), .value = { .ptr = authority, .size = al } };
    list[n++] = (proven_http_header_t){ .name = PROVEN_LIT(":path"), .value = path };
    bool has_accept = false, has_range = false;
    for (proven_size_t i = 0; i < r->header_count; ++i) {
        proven_u8str_view_t name = r->headers[i].name;
        if (proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("accept-encoding"))) has_accept = true;
        if (proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("range"))) has_range = true;
        /* Fields that describe an HTTP/1.1 connection are not sent on this one. */
        if (proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("keep-alive")) || proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("proxy-connection")) || proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("upgrade"))) continue;
        if (proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("te")) && !proven_u8str_view_eq_ascii_ci(r->headers[i].value, PROVEN_LIT("trailers"))) continue;
        if (at + name.size > cl->cfg.max_head_bytes) { e = PROVEN_ERR_OUT_OF_BOUNDS; break; }
        for (proven_size_t k = 0; k < name.size; ++k) cl->hbuf[at + k] = proven_ascii_lower(name.ptr[k]);
        list[n++] = (proven_http_header_t){ .name = { .ptr = cl->hbuf + at, .size = name.size }, .value = r->headers[i].value };
        at += name.size;
    }
    const bool body_method = proven_u8str_view_eq_ascii_ci(r->method, PROVEN_LIT("POST")) || proven_u8str_view_eq_ascii_ci(r->method, PROVEN_LIT("PUT")) || proven_u8str_view_eq_ascii_ci(r->method, PROVEN_LIT("PATCH"));
    if (e == PROVEN_OK) {
        if (cl->cfg.decompress && !has_accept && !has_range) list[n++] = (proven_http_header_t){ .name = PROVEN_LIT("accept-encoding"), .value = proven_http_decoder_accept_(cl->cfg.decompress_zstd, cl->cfg.decompress_brotli) };
        proven_u64 length = r->body.size > 0 ? r->body.size : r->body_length;
        if (r->body.size > 0 || (r->body_length != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN && (r->body_length > 0 || body_method))) {
            do { digits[sizeof digits - 1 - nd++] = (proven_byte_t)('0' + length % 10); length /= 10; } while (length > 0);
            list[n++] = (proven_http_header_t){ .name = PROVEN_LIT("content-length"), .value = { .ptr = digits + sizeof digits - nd, .size = nd } };
        }
        e = proven_h2_conn_submit_request(c->engine, list, n, !r->streamed && r->body.size == 0, &r->id);
    }
    if (list != small) cl->alloc.free_fn(cl->alloc.ctx, list);
    if (e != PROVEN_OK) { r->id = 0; return e; }
    proven_h2_conn_stream_set_user(c->engine, r->id, r);
    r->head_only = proven_u8str_view_eq_ascii_ci(r->method, PROVEN_LIT("HEAD"));
    if (!r->streamed) {
        r->end_asked = true;
        if (r->body.size == 0) r->engine_done = true;
        else {
            /* A body held in memory: what the windows do not take now is kept and follows. */
            proven_result_size_t w = proven_h2_conn_send_data(c->engine, r->id, r->body, true);
            if (w.err != PROVEN_OK) return PROVEN_ERR_RESET;
            if (w.value == r->body.size) r->engine_done = true;
            else {
                r->window_shut = true;
                if (!c2_append(cl->alloc, &r->out, &r->out_len, &r->out_cap, r->body.ptr + w.value, r->body.size - w.value)) return PROVEN_ERR_NOMEM;
            }
        }
    } else {
        r->send_left = r->body_length;
    }
    /* What it was started with has been used - unless it may have to be put to the server
     * again (retry_unsent): then it is kept until the response begins. */
    if (r->copy && (r->streamed || cl->cfg.retry_unsent == 0)) { cl->alloc.free_fn(cl->alloc.ctx, r->copy); r->copy = (void *)0; }
    r_expect(r);
    return PROVEN_OK;
}

/* Requests that were waiting, onto streams, in the order they were started. */
static void c_submit_waiting(c2conn_t *c) {
    /* Not before the server has said what it allows: a burst sent ahead of its settings can
     * exceed its limit on streams, and the surplus comes back refused. */
    if (c->dead || c->state != C2_UP || c->over) return;
    if (c->early) {
        /* Early data (RFC-0027): before the server's settings, one request that is safe to repeat,
         * on the one stream every server allows; the rest wait for the handshake. */
        bool any = false;
        for (proven_c2req_t *r = c->reqs; r; r = r->next) any = any || r->went_early;
        proven_c2req_t *first = (void *)0;
        for (proven_c2req_t *r = c->reqs; r; r = r->next) if (r->id == 0 && !r->dead && r->early_ok && !r->no_early) first = r;
        if (any || !first) return;
        c_enter(c);
        first->went_early = true;
        const proven_err_t e = r_submit(first);
        if (e != PROVEN_OK) { first->went_early = false; if (e != PROVEN_ERR_BUSY) r_finish(first, e); }
        c_leave(c);
        return;
    }
    if (!proven_h2_conn_has_peer_settings(c->engine)) return;
    c_enter(c);
    for (bool again = true; again && !c->dead;) {
        again = false;
        proven_c2req_t *oldest = (void *)0;
        for (proven_c2req_t *r = c->reqs; r; r = r->next) if (r->id == 0 && !r->dead) oldest = r;      /* new ones go to the list's head */
        if (!oldest) break;
        proven_err_t e = r_submit(oldest);
        if (e == PROVEN_ERR_BUSY) break;
        if (e == PROVEN_ERR_INVALID_STATE) {
            /* The connection takes no more requests - it has none of its stream numbers left,
             * if nothing else said so first. The next request to this origin makes another. */
            c->over = true;
            r_unsent(oldest, false);
        } else if (e != PROVEN_OK) {
            r_finish(oldest, e);
        }
        again = true;
    }
    c_leave(c);
}

// -----------------------------------------------------------------------------
// Responses
// -----------------------------------------------------------------------------

/* Body bytes to the caller, decoded if they are to be. False when the request ended. */
static bool r_deliver(proven_c2req_t *r, proven_mem_view_t piece, bool last) {
    proven_http_event_client_t *cl = r->h.client;
    if (!r->dec) {
        if ((piece.size > 0 || last) && r->on.on_body) r->on.on_body(r->ctx, c2_handle(r), piece, last);
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
            if (r->got > cl->cfg.max_body_bytes) { r_finish(r, PROVEN_ERR_OUT_OF_BOUNDS); return false; }
            if (r->on.on_body) r->on.on_body(r->ctx, c2_handle(r), (proven_mem_view_t){ .ptr = room, .size = made }, false);
        } else if (used == 0) {
            r_finish(r, PROVEN_ERR_INVALID_FORMAT);        /* input it will not take, and nothing to give */
            return false;
        }
    }
    /* What the decoder still holds of the last input comes out with nothing more put in. */
    while (!r->dead) {
        proven_size_t used = 0, made = 0;
        proven_err_t e = proven_http_decoder_step_(r->dec, (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, &used, (proven_mem_mut_t){ .ptr = room, .size = PROVEN_HTTP_CODING_PIECE }, &made);
        if (e != PROVEN_OK) { r_finish(r, e); return false; }
        if (made == 0) break;
        r->got += made;
        if (r->got > cl->cfg.max_body_bytes) { r_finish(r, PROVEN_ERR_OUT_OF_BOUNDS); return false; }
        if (r->on.on_body) r->on.on_body(r->ctx, c2_handle(r), (proven_mem_view_t){ .ptr = room, .size = made }, false);
    }
    if (r->dead) return false;
    if (last) {
        proven_err_t e = proven_http_decoder_end_(r->dec);
        if (e != PROVEN_OK) { r_finish(r, e); return false; }
        if (r->on.on_body) r->on.on_body(r->ctx, c2_handle(r), (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, true);
    }
    return !r->dead;
}

static void c_body(c2conn_t *c, proven_c2req_t *r, proven_mem_view_t piece, bool last) {
    proven_http_event_client_t *cl = c->client;
    const proven_u32 id = r->id;
    if (last) r->body_over = true;
    if (!r->dec) {
        r->got += piece.size;
        if (r->got > cl->cfg.max_body_bytes) { r_finish(r, PROVEN_ERR_OUT_OF_BOUNDS); return; }
    }
    if (r->paused) {
        /* Kept as it arrived - compressed, if it is - and not reported as consumed, so the
         * server can send at most a window of it. */
        if (!c2_append(cl->alloc, &r->held, &r->held_len, &r->held_cap, piece.ptr, piece.size)) { r_finish(r, PROVEN_ERR_NOMEM); return; }
        if (last) r->held_last = true;
        r->due = 0;
        return;
    }
    r_enter(r);
    const bool alive = r_deliver(r, piece, last);
    if (alive) {
        if (last) r_finish(r, PROVEN_OK);
        else r_expect(r);
    }
    r_leave(r);
    if (!c->dead) proven_h2_conn_consumed(c->engine, id, piece.size);
}

static void c_response(c2conn_t *c, proven_c2req_t *r, const proven_h2_event_t *ev) {
    proven_http_event_client_t *cl = c->client;
    if (ev->status < 200) return;                          /* an interim response: not the answer, and not passed on */
    if (r->got_head) return;
    if (ev->field_count > cl->cfg.max_headers) { r_finish(r, PROVEN_ERR_OUT_OF_BOUNDS); return; }
    for (proven_size_t i = 0; i < ev->field_count; ++i) cl->headers[i] = ev->fields[i];
    proven_http_response_t res = { .status = ev->status, .version_minor = 1, .headers = cl->headers, .header_count = ev->field_count };
    if (ev->status == 425 && r->early_result == 1 && !r->no_early) {
        /* Too Early (RFC 8470): sent once more on this connection, now that its handshake is
         * over; the stream that carried the early copy is let go of. */
        r->no_early = true;
        r->early_result = 3;
        proven_h2_conn_stream_set_user(c->engine, r->id, (void *)0);
        (void)proven_h2_conn_reset(c->engine, r->id, PROVEN_H2_CANCEL);
        r->id = 0;
        r->engine_done = false;
        c_submit_waiting(c);
        return;
    }
    r->got_head = true;
    if (r->copy) { cl->alloc.free_fn(cl->alloc.ctx, r->copy); r->copy = (void *)0; }      /* answered: it will not be sent again */
    const bool no_body = ev->end_stream || r->head_only;
    proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;
    if (cl->cfg.decompress && !no_body && proven_http_decoder_wanted_(res.status, res.headers, res.header_count, cl->cfg.decompress_zstd, cl->cfg.decompress_brotli, &coding)) {
        proven_result_mem_mut_t m = cl->alloc.alloc_fn(cl->alloc.ctx, sizeof(proven_http_decoder_t_) + PROVEN_HTTP_CODING_PIECE, alignof(proven_http_decoder_t_));
        if (m.err != PROVEN_OK) { r_finish(r, PROVEN_ERR_NOMEM); return; }
        r->dec = (proven_http_decoder_t_ *)(void *)m.value.ptr;
        proven_http_decoder_begin_(r->dec, cl->alloc, coding);
    }
    r_enter(r);
    if (r->on.on_response) r->on.on_response(r->ctx, c2_handle(r), &res);
    if (!r->dead) {
        if (ev->end_stream) { r->body_over = true; r_finish(r, PROVEN_OK); }
        else r_expect(r);
    }
    r_leave(r);
}

static void c_event(c2conn_t *c, const proven_h2_event_t *ev) {
    proven_c2req_t *r = ev->user;
    switch (ev->kind) {
        case PROVEN_H2_EVENT_HEADERS: if (r) c_response(c, r, ev); break;
        case PROVEN_H2_EVENT_DATA:
            if (r) c_body(c, r, ev->data, ev->end_stream);
            else proven_h2_conn_consumed(c->engine, ev->stream_id, ev->data.size);
            break;
        case PROVEN_H2_EVENT_TRAILERS:             /* kept for proven_http_event_request_trailers, then the body's end */
            if (r && proven_http_trailers_keep_(c->client->alloc, &r->in_trailers, ev->fields, ev->field_count, true, false) != PROVEN_OK) { r_finish(r, PROVEN_ERR_NOMEM); break; }
            if (r) c_body(c, r, (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, true);
            break;
        case PROVEN_H2_EVENT_STREAM_RESET:
            if (r) {
                r->engine_done = true;
                /* Not acted on: safe to start again. A reset that says the response is complete
                 * (every byte its content-length declared, no END_STREAM): its end. Anything
                 * else: it may have been acted on. */
                if (ev->code == PROVEN_H2_REFUSED_STREAM) r_unsent(r, true);
                else if (ev->code == PROVEN_H2_NO_ERROR && ev->end_stream && r->got_head && !r->dead) c_body(c, r, (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, true);
                else r_finish(r, ev->too_large ? PROVEN_ERR_OUT_OF_BOUNDS : PROVEN_ERR_RESET);   /* a head over max_head_bytes */
            }
            break;
        case PROVEN_H2_EVENT_STREAM_WRITABLE: if (r) { r->window_shut = false; r_push(r); } break;
        case PROVEN_H2_EVENT_GOAWAY:
            /* No new request here: the next one to this origin makes a new connection, and
             * those that were waiting for a stream on this one are told to start again. */
            c->over = true;
            for (bool again = true; again;) {
                again = false;
                for (proven_c2req_t *w = c->reqs; w; w = w->next) if (w->id == 0) { r_unsent(w, false); again = true; break; }
            }
            break;
        case PROVEN_H2_EVENT_CLOSED:
            c->over = true;
            c_end_requests(c, PROVEN_ERR_RESET);
            break;
        default: break;
    }
}

// -----------------------------------------------------------------------------
// The round
// -----------------------------------------------------------------------------

static void c_pump(c2conn_t *c) {
    /* While a read is being worked through nothing is written: the requests that its answers
     * set off leave in one write. c_sock_received pumps when the read is done. */
    if (c->pumping || c->dead || c->reading || c->state != C2_UP) return;
    c->pumping = true;
    c_enter(c);
    for (;;) {
        bool progress = false;
        while (!c->dead) {
            proven_mem_view_t out = proven_h2_conn_pending_output(c->engine);
            proven_size_t n = c_offer(c, out);             /* with nothing to offer: withdraws "call me back" */
            if (c->dead || n == 0) break;
            proven_h2_conn_output_sent(c->engine, n);
            progress = true;
        }
        if (c->dead) break;
        for (;;) {
            proven_size_t used = 0;
            proven_h2_event_t ev;
            if (proven_h2_conn_feed(c->engine, (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, &used, &ev) != PROVEN_OK || ev.kind == PROVEN_H2_EVENT_NONE) break;
            c_event(c, &ev);
            progress = true;
            if (c->dead) break;
        }
        if (c->dead) break;
        /* A stream that ended may have made room under the server's limit. */
        {
            const proven_size_t before = proven_h2_conn_pending_output(c->engine).size;
            c_submit_waiting(c);
            if (!c->dead && proven_h2_conn_pending_output(c->engine).size != before) progress = true;
        }
        if (c->dead) break;
        /* Each request that holds a body has a turn of C2_TURN_BYTES, beginning behind the one
         * that had the last turn. (From the list's head every time, the newest requests took
         * all the room there was and an older one waited for as long as they had something
         * to send.) */
        c->round++;
        for (bool again = true; again && !c->dead;) {
            again = false;
            proven_c2req_t *const first = c->turn && c->turn->next ? c->turn->next : c->reqs;
            for (proven_c2req_t *r = first; r; ) {
                proven_c2req_t *const after = r->next ? r->next : c->reqs;
                if (!(r->mark == c->round || r->id == 0 || r->window_shut || !(r_out_pending(r) > 0 || (r->end_asked && !r->engine_done) || r->want_writable))) {
                    r->mark = c->round;
                    if (!(c_room(c) == 0 && !(r->end_asked && !r->engine_done && r_out_pending(r) == 0))) {
                        const proven_size_t before = proven_h2_conn_pending_output(c->engine).size;
                        c->turn = r;
                        c->sharing = true; c->share = C2_TURN_BYTES;
                        r_push(r);
                        c->sharing = false;
                        if (!c->dead && proven_h2_conn_pending_output(c->engine).size != before) progress = true;
                        again = true;
                        break;
                    }
                }
                r = after == first ? (void *)0 : after;
            }
        }
        if (c->dead || !progress) break;
    }
    if (!c->dead) {
        if (c->peer_eof && c->count == 0) c->over = true;
        if (c->over && c->count == 0 && c_queued(c) == 0) c_kill(c, PROVEN_OK);
        else { c_arm(c); c_watch(c); }
    }
    /* With nothing on it this connection is one that max_connections may close for a request
     * that waits for a place, of either protocol. (It may be this very connection that is
     * closed: the memory holds while this round is open.) */
    if (!c->dead && !c->client_going && c->count == 0 && c_queued(c) == 0 && c->client->cfg.max_connections != 0 && (c->client->wait_head || c->client->h2_roomless || c->client->h3_roomless))
        proven_http_event_client_freed_(c->client);
    c->pumping = false;
    c_leave(c);
}

/* Bytes of the HTTP/2 stream, after TLS if any. */
static void c_input(c2conn_t *c, const proven_byte_t *p, proven_size_t n) {
    proven_size_t pos = 0;
    while (!c->dead && pos < n) {
        proven_size_t used = 0;
        proven_h2_event_t ev;
        proven_err_t e = proven_h2_conn_feed(c->engine, (proven_mem_view_t){ .ptr = p + pos, .size = n - pos }, &used, &ev);
        if (e != PROVEN_OK) { c_kill(c, e == PROVEN_ERR_NOMEM ? PROVEN_ERR_NOMEM : PROVEN_ERR_RESET); return; }
        pos += used;
        if (ev.kind != PROVEN_H2_EVENT_NONE) c_event(c, &ev);
        else if (pos == n) break;
    }
}

// -----------------------------------------------------------------------------
// Which protocol, and what follows from the answer
// -----------------------------------------------------------------------------

/* The connection speaks HTTP/2: make the engine and send what was waiting. */
static void c_up(c2conn_t *c) {
    proven_http_event_client_t *cl = c->client;
    proven_u32 list = cl->cfg.max_head_bytes > ((proven_size_t)1 << 20) ? (proven_u32)1 << 20 : (proven_u32)cl->cfg.max_head_bytes;
    if (!cl->h2buf) {
        cl->h2buf_size = proven_h2_header_buffer_size(list);
        cl->h2buf = c2_alloc(cl->alloc, cl->h2buf_size);
        if (!cl->h2buf) { c_kill(c, PROVEN_ERR_NOMEM); return; }
    }
    proven_h2_config_t cfg = {
        .alloc = cl->alloc, .max_header_list_bytes = list, .initial_window = cl->cfg.http2_window_bytes,
        .max_buffered_output = cl->cfg.max_buffered_output < 512 ? 1024
                             : cl->cfg.max_buffered_output > ((proven_size_t)1 << 29) ? (proven_size_t)1 << 30 : cl->cfg.max_buffered_output * 2,
        .header_buffer = { .ptr = cl->h2buf, .size = cl->h2buf_size },
    };
    if (proven_h2_conn_create(&cfg, &c->engine) != PROVEN_OK) { c_kill(c, PROVEN_ERR_NOMEM); return; }
    /* No window configured: it starts at the protocol's and grows while bodies are read, to HTTP/3's caps (RFC-0019 section 10). */
    if (cl->cfg.http2_window_bytes == 0) proven_h2_conn_grow_(c->engine, 16384, 24576, h2_now_us);
    c->state = C2_UP;
    c->quiet_due = 0;
    c_submit_waiting(c);
    c_pump(c);
}

static void c_fall_back(c2conn_t *c);

/* The handshake of a connection that began HTTP/2 inside it is over (RFC-0027). The server took
 * what went early, or refused it - it never read those bytes, and they go again first, as they
 * were, so that the stream and the header compression stay where both sides think they are. A
 * server that did not agree h2 again cannot have taken them: its requests start again elsewhere. */
static void c_settle_early(c2conn_t *c) {
    proven_http_event_client_t *cl = c->client;
    c->early = false;
    if (!proven_tls_ticket_stored_(c->tls)) proven_tls_session_early_off_(&c->session);    /* a ticket that came since is fresh; a later one replaces it */
    proven_u8str_view_t chosen = proven_tls_alpn(c->tls);
    const bool h2 = chosen.size == 2 && chosen.ptr[0] == 'h' && chosen.ptr[1] == '2';
    const bool taken = h2 && proven_tls_early_(c->tls) == PROVEN_TLS_EARLY_ACCEPTED;
    for (proven_c2req_t *r = c->reqs; r; r = r->next) if (r->went_early) r->early_result = taken ? 1 : 2;
    if (h2 && !taken) {
        for (proven_size_t at = 0; at < c->early_copy_len && !c->dead;) {
            const proven_size_t n = c->early_copy_len - at < 16384 ? c->early_copy_len - at : 16384;
            proven_result_size_t w = proven_tls_write(c->tls, (proven_mem_view_t){ .ptr = c->early_copy + at, .size = n });
            if (w.err != PROVEN_OK || w.value == 0) { c_kill(c, PROVEN_ERR_RESET); break; }
            at += w.value;
        }
    }
    if (c->early_copy) { cl->alloc.free_fn(cl->alloc.ctx, c->early_copy); c->early_copy = (void *)0; c->early_copy_len = c->early_copy_cap = 0; }
    if (c->dead) return;
    if (!h2) { c_fall_back(c); return; }
    if (c_flush_tls(c)) c_pump(c);
}

/* The origin speaks HTTP/1.1. It is remembered as that, and every request that was waiting to
 * find out starts again as an ordinary one, known to its caller by the object it already has. */
static void c_fall_back(c2conn_t *c) {
    proven_http_event_client_t *cl = c->client;
    c_enter(c);
    c_close_socket(c);
    c->state = C2_H1;
    /* What is remembered is bounded: a client that meets many such origins forgets the
     * oldest (the list's tail), which then costs one handshake to find out again. */
    {
        proven_size_t remembered = 0;
        c2conn_t *oldest = (void *)0;
        for (c2conn_t *k = cl->h2; k; k = k->next) if (k->state == C2_H1 && k != c) { remembered++; oldest = k; }
        if (remembered >= C2_MAX_REMEMBERED && oldest) {
            if (oldest->prev) oldest->prev->next = oldest->next;
            else cl->h2 = oldest->next;
            if (oldest->next) oldest->next->prev = oldest->prev;
            oldest->dead = true;
            c_enter(oldest);
            c_leave(oldest);
        }
    }
    while (c->reqs && !c->dead) {
        proven_c2req_t *r = c->reqs;
        /* The oldest first: new ones were put at the head. */
        while (r->next) r = r->next;
        proven_http_event_request_options_t o = {
            .method = r->method, .url = r->url, .address = &c->to, .headers = r->headers, .header_count = r->header_count,
            .body = r->body, .body_length = r->body_length, .on = r->on, .ctx = r->ctx,
        };
        r_unlink(r);
        proven_http_event_request_t *made = (void *)0;
        proven_err_t e = proven_http_event_client_start_h1_(cl, &o, c2_handle(r), &made);
        cl->request_count--;                       /* it is counted once: as the request it now is, or not at all */
        if (e != PROVEN_OK) {
            r->dead = true;
            r_enter(r);
            r_free_parts(r);
            r->on.on_done(r->ctx, c2_handle(r), e);
            r_leave(r);
            continue;
        }
        r->fwd = made;
        made->want_writable = r->want_writable;
        if (r->paused) proven_http_event_request_pause(made);
        /* Ended with trailers before it had a stream: they end it over HTTP/1.1 instead. */
        if (r->end_asked && r->out_trailers.count && proven_http_event_request_end_trailers(made, r->out_trailers.fields, r->out_trailers.count) != PROVEN_OK)
            proven_http_event_request_abort(made);
        r_free_parts(r);                           /* the copy has been used */
    }
    c_leave(c);
}

static void c_readable(c2conn_t *c) {
    proven_http_event_client_t *cl = c->client;
    if (c->peer_eof) return;
    proven_result_size_t rd = proven_loop_read(cl->loop, &c->ls, (proven_mem_mut_t){ .ptr = cl->rbuf, .size = CL_READ_BYTES });
    if (rd.err == PROVEN_ERR_TIMEOUT || rd.err == PROVEN_ERR_AGAIN) return;
    if (rd.err == PROVEN_ERR_EOF) {
        c->peer_eof = true;
        /* What was answered in full has been delivered; nothing else will be. */
        if (c->state != C2_UP) c_kill(c, PROVEN_ERR_RESET);
        else { c_end_requests(c, PROVEN_ERR_RESET); c_pump(c); }
        return;
    }
    if (rd.err != PROVEN_OK) { c_kill(c, rd.err == PROVEN_ERR_REFUSED || rd.err == PROVEN_ERR_UNREACHABLE ? rd.err : PROVEN_ERR_RESET); return; }
    if (!c->tls) { c_input(c, cl->rbuf, rd.value); return; }
    proven_size_t at = 0;
    while (at < rd.value && !c->dead) {
        proven_size_t used = 0;
        proven_err_t e = proven_tls_feed(c->tls, (proven_mem_view_t){ .ptr = cl->rbuf + at, .size = rd.value - at }, &used);
        at += used;
        if (e != PROVEN_OK) {
            (void)c_flush_tls(c);                  /* the alert */
            c_kill(c, e);
            return;
        }
        if (c->state == C2_HANDSHAKE && proven_tls_is_established(c->tls)) {
            proven_u8str_view_t chosen = proven_tls_alpn(c->tls);
            if (chosen.size == 2 && chosen.ptr[0] == 'h' && chosen.ptr[1] == '2') c_up(c);
            else { c_fall_back(c); return; }
            if (c->dead) return;
        }
        if (c->early && proven_tls_is_established(c->tls)) {
            c_settle_early(c);
            if (c->dead || c->state != C2_UP) return;
        }
        while (c->state == C2_UP && !c->dead) {
            proven_result_size_t got = proven_tls_read(c->tls, (proven_mem_mut_t){ .ptr = cl->pbuf, .size = CL_PLAIN_BYTES });
            if (got.err == PROVEN_ERR_EOF) { c->peer_eof = true; c_end_requests(c, PROVEN_ERR_RESET); break; }
            if (got.err != PROVEN_OK) break;
            c_input(c, cl->pbuf, got.value);
        }
        if (c->dead || c->peer_eof) break;
        if (used == 0) break;
    }
}

static void c_sock_connected(void *ctx, proven_err_t err) {
    c2conn_t *c = ctx;
    if (err == PROVEN_ERR_CANCELLED || c->dead || c->state != C2_CONNECTING) return;
    c_enter(c);
    if (err == PROVEN_OK) {
        (void)proven_net_conn_set_nodelay(&c->sock, true);
        if (c->tls) {
            c->state = C2_HANDSHAKE;
            /* With room for early data and a request that may use it, HTTP/2 begins now, inside
             * the ClientHello's flight (RFC-0027). */
            bool any = false;
            for (proven_c2req_t *r = c->reqs; r; r = r->next) any = any || (r->id == 0 && !r->dead && r->early_ok && !r->no_early);
            if (any && proven_tls_early_room_(c->tls) > 0) { c->early = true; c_up(c); }
            if (!c->dead && c_flush_tls(c)) c_watch(c);
        }
        else c_up(c);                              /* in the clear: by prior knowledge */
    } else {
        c_kill(c, err);
    }
    c_leave(c);
}

static void c_sock_received(void *ctx, proven_err_t err, proven_size_t n) {
    c2conn_t *c = ctx;
    (void)n;
    c->recv_armed = false;
    if (err == PROVEN_ERR_CANCELLED || c->dead || c->state == C2_H1 || c->state == C2_CONNECTING) return;
    c_enter(c);
    c->reading = true; c_readable(c); c->reading = false;
    if (!c->dead && c->state == C2_UP) c_pump(c);
    else if (!c->dead && c->state == C2_HANDSHAKE) (void)c_flush_tls(c);
    if (!c->dead) c_watch(c);
    c_leave(c);
}

static void c_sock_writable(void *ctx, proven_err_t err) {
    c2conn_t *c = ctx;
    if (err == PROVEN_ERR_CANCELLED || c->dead || c->state == C2_H1) return;
    c_enter(c);
    if (err != PROVEN_OK) c_kill(c, PROVEN_ERR_RESET);
    else {
        c->more_out = false;
        if (c_flush_tls(c)) {
            if (c->state == C2_UP) c_pump(c);
            else c_watch(c);
        }
    }
    c_leave(c);
}

static void c_sock_closed(void *ctx) {
    c2conn_t *c = ctx;
    proven_http_event_client_t *cl = c->client;
    c->sock_gone = true;
    if (c->dead && c->depth == 0) {
        proven_h2_conn_destroy(c->engine);
        cl->alloc.free_fn(cl->alloc.ctx, c);
    }
    proven_http_event_client_ended_(cl);
}

static const proven_loop_sock_fns_t c_sock_fns = { .received = c_sock_received, .writable = c_sock_writable, .connected = c_sock_connected, .closed = c_sock_closed };

static void c_on_timer(void *ctx) {
    c2conn_t *c = ctx;
    if (c->dead) return;
    c_enter(c);
    const proven_time_t now = proven_time_monotonic_now();
    for (bool again = true; again && !c->dead;) {
        again = false;
        for (proven_c2req_t *r = c->reqs; r; r = r->next) {
            if (r->due == 0 || r->due > now) continue;
            r_finish(r, PROVEN_ERR_TIMEOUT);
            again = true;
            break;
        }
    }
    if (!c->dead) {
        /* The earliest hour among the requests that remain, exactly: c_arm goes by it. */
        c->req_due = 0;
        for (const proven_c2req_t *r = c->reqs; r; r = r->next)
            if (r->due != 0 && (c->req_due == 0 || r->due < c->req_due)) c->req_due = r->due;
        if (c->quiet_due != 0 && now >= c->quiet_due) {
            if (c_connecting(c) || c_queued(c) > 0) c_kill(c, PROVEN_ERR_TIMEOUT);       /* never connected, or the server takes nothing */
            else if (c->count == 0) { proven_h2_conn_shutdown(c->engine, PROVEN_H2_NO_ERROR); c->over = true; c_pump(c); }
            else c_pump(c);
        } else if (c->state == C2_UP) {
            c_pump(c);
        } else {
            c_arm(c);
        }
    }
    c_leave(c);
}

// -----------------------------------------------------------------------------
// An origin's connection, and a request put to it again
// -----------------------------------------------------------------------------

static bool c2_same_addr(const proven_net_addr_t *a, const proven_net_addr_t *b) {
    if (a->family != b->family || a->port != b->port) return false;
    for (proven_size_t i = 0; i < sizeof a->ip; ++i) if (a->ip[i] != b->ip[i]) return false;
    return true;
}

/* What the client has for an origin - scheme, host, port and address: a connection that takes
 * requests, or the memory that the origin speaks HTTP/1.1. `not`: one to pass over. */
static c2conn_t *c_find(proven_http_event_client_t *cl, bool https, proven_u8str_view_t host, proven_u16 port, const proven_net_addr_t *to, const c2conn_t *not) {
    for (c2conn_t *k = cl->h2; k; k = k->next) {
        if (k == not || k->dead || k->over || k->peer_eof || k->https != https || k->port != port || !c2_same_addr(&k->to, to)) continue;
        if (!proven_u8str_view_eq_ascii_ci((proven_u8str_view_t){ .ptr = k->host, .size = k->host_len }, host)) continue;
        return k;
    }
    return (void *)0;
}

/* Begin a connection to an origin. On failure nothing is left of it. */
static proven_err_t c_open(proven_http_event_client_t *cl, bool https, proven_u8str_view_t host, proven_u16 port, const proven_net_addr_t *to, c2conn_t **out) {
    /* max_connections: an idle connection is closed to make room; with none, there is none. */
    if (!proven_http_event_client_room_(cl)) return PROVEN_ERR_BUSY;
    c2conn_t *c = c2_alloc(cl->alloc, sizeof *c);
    if (!c) return PROVEN_ERR_NOMEM;
    for (proven_size_t i = 0; i < sizeof *c; ++i) ((proven_byte_t *)c)[i] = 0;
    c->client = cl;
    c->https = https;
    c->port = port;
    c->to = *to;
    c->host_len = (proven_u8)host.size;
    for (proven_size_t i = 0; i < host.size; ++i) c->host[i] = host.ptr[i];
    c->state = C2_CONNECTING;
    proven_err_t e = PROVEN_OK;
    if (https) {
        c->session_mark = proven_http_event_client_session_start_(cl, host, port, false, &c->session);
        c->session_stored = c->session_mark;
        /* Early data (RFC-0027) is offered with a session made over HTTP/2: what goes early is
         * HTTP/2's preface, settings and a request, which only a server that agrees h2 again reads. */
        e = cl->cfg.tls->early_data && proven_tls_session_alpn_is_(&c->session, PROVEN_LIT("h2")) ? proven_tls_client_create_early_(cl->cfg.tls, host, &c->session, &c->tls)
                                                                                                : proven_tls_client_create(cl->cfg.tls, host, &c->session, &c->tls);
    }
    /* The loop makes the connection and says how it went from the loop, so that nothing is
     * reported from inside the call that began it. */
    if (e == PROVEN_OK) e = proven_loop_connect(cl->loop, &c->ls, c->to, &c->sock, &c_sock_fns, c);
    if (e == PROVEN_OK) proven_loop_send_limit_(&c->ls, (proven_size_t)-1);     /* the engine and C2_OFFER_HOLD bound what it gives */
    if (e != PROVEN_OK) {
        if (c->tls) proven_tls_conn_destroy(c->tls);
        if (proven_net_conn_is_open(&c->sock)) (void)proven_net_close(&c->sock);
        cl->alloc.free_fn(cl->alloc.ctx, c);
        return e;
    }
    c->attached = true;
    c->next = cl->h2;
    if (c->next) c->next->prev = c;
    cl->h2 = c;
    *out = c;
    return PROVEN_OK;
}

/* A request the server did not act on: it was never on a stream, or the stream was refused.
 * Its caller is told PROVEN_ERR_AGAIN and may start it again - or, with retry_unsent, the
 * client does that itself, so many times: the request goes back to waiting, on this connection
 * (`here`: it was one stream that was refused, and the connection goes on) or on another for
 * the same origin, which is made if there is none. Nothing is told of the try that failed.
 * Not a request whose body is written in pieces - the client does not have it to send again -
 * and not while the client is being destroyed. */
/* A request that waits to be tried again (retry_delay_ms). It is on no connection: this
 * says where it is to go, and the client keeps them in the order they are due - the order
 * they came, when the wait is the same for all; r_delay puts each in its place when the wait
 * grows (retry_delay_max_ms). The same record serves a request that waits for the client to
 * have room for a connection (`for_room`), in a list of its own. */
typedef struct c2delay {
    struct c2delay *next;
    proven_c2req_t *r;
    proven_time_t due;
    proven_net_addr_t to;
    proven_u16 port;
    bool https;
    bool for_room;                     /* in the other list: it waits for the client to have room for a connection, and `due` is when it gives up */
    proven_u8 host_len;
    proven_byte_t host[256];
} proven_c2delay_t;

static void c_on_delay(void *ctx);

static void cl_delay_arm(proven_http_event_client_t *cl) {
    const proven_c2delay_t *first = cl->h2_delayed;
    if (!first) { proven_loop_timer_cancel(cl->loop, &cl->h2_delay_timer); return; }
    const proven_time_t now = proven_time_monotonic_now();
    const proven_u64 ms = first->due > now ? (proven_u64)(first->due - now + 999999) / 1000000 : 0;
    proven_loop_timer_set(cl->loop, &cl->h2_delay_timer, ms > 0xffffffffu ? 0xffffffffu : (proven_u32)ms, c_on_delay, cl);
}

/* Out of its list, its record freed: it is to be placed, or it has ended. */
static void r_undelay(proven_c2req_t *r) {
    proven_http_event_client_t *cl = r->h.client;
    proven_c2delay_t *d = r->delay, *before = (void *)0;
    void **head = d->for_room ? &cl->h2_roomless : &cl->h2_delayed, **last = d->for_room ? &cl->h2_roomless_last : &cl->h2_delayed_last;
    for (proven_c2delay_t *i = *head; i && i != d; i = i->next) before = i;
    if (before) before->next = d->next;
    else *head = d->next;
    if (*last == d) *last = before;
    if (d->for_room) cl->h2_roomless_count--;
    r->delay = (void *)0;
    cl->alloc.free_fn(cl->alloc.ctx, d);
}

/* max_connections, and every connection in use: a request for an origin with none waits here,
 * on no connection, until the client may have one more - or until connect_timeout_ms, which
 * is what the wait for a connection is. In the order they came: the wait is the same for
 * all, so the first is the first to give up. */
static void c_on_room_timer(void *ctx);
static void r_attach(proven_c2req_t *r, c2conn_t *to);

static void cl_room_arm(proven_http_event_client_t *cl) {
    const proven_c2delay_t *first = cl->h2_roomless;
    if (!first) { proven_loop_timer_cancel(cl->loop, &cl->h2_room_timer); return; }
    const proven_time_t now = proven_time_monotonic_now();
    const proven_u64 ms = first->due > now ? (proven_u64)(first->due - now + 999999) / 1000000 : 0;
    proven_loop_timer_set(cl->loop, &cl->h2_room_timer, ms > 0xffffffffu ? 0xffffffffu : (proven_u32)ms, c_on_room_timer, cl);
}

static void c_on_room_timer(void *ctx) {
    proven_http_event_client_t *cl = ctx;
    for (;;) {
        proven_c2delay_t *d = cl->h2_roomless;
        if (!d || d->due > proven_time_monotonic_now()) break;
        r_finish(d->r, PROVEN_ERR_TIMEOUT);
    }
    cl_room_arm(cl);
}

/* Into that list. The request is on no connection. PROVEN_ERR_BUSY when as many wait as may;
 * PROVEN_ERR_NOMEM. */
static proven_err_t r_wait_room(proven_c2req_t *r, bool https, proven_u8str_view_t host, proven_u16 port, const proven_net_addr_t *to) {
    proven_http_event_client_t *cl = r->h.client;
    if (cl->h2_roomless_count >= cl->cfg.max_waiting_requests) return PROVEN_ERR_BUSY;
    proven_c2delay_t *d = c2_alloc(cl->alloc, sizeof *d);
    if (!d) return PROVEN_ERR_NOMEM;
    d->next = (void *)0;
    d->r = r;
    d->due = proven_time_monotonic_now() + (proven_time_t)cl->cfg.connect_timeout_ms * 1000000;
    d->to = *to; d->port = port; d->https = https; d->for_room = true; d->host_len = (proven_u8)host.size;
    for (proven_size_t i = 0; i < host.size; ++i) d->host[i] = host.ptr[i];
    r->delay = d;
    if (cl->h2_roomless_last) ((proven_c2delay_t *)cl->h2_roomless_last)->next = d;
    else cl->h2_roomless = d;
    cl->h2_roomless_last = d;
    cl->h2_roomless_count++;
    if (cl->h2_roomless == d) cl_room_arm(cl);
    return PROVEN_OK;
}

bool proven_http_event_client_h2_waiting_(const proven_http_event_client_t *cl) {
    return cl->h2_roomless != (void *)0;
}

/* A place can be had, or a connection was made: each of those that wait, in order, onto a
 * connection to its origin if there is one by now, and otherwise onto a new one while the
 * client has room for it. */
void proven_http_event_client_h2_freed_(proven_http_event_client_t *cl) {
    if (cl->closing || cl->h2_room_busy) return;
    cl->h2_room_busy = true;                       /* (c_open closes an idle connection, which comes back here) */
    bool may_open = true;
    for (proven_c2delay_t *d = cl->h2_roomless, *next; d; d = next) {
        next = d->next;
        proven_c2req_t *r = d->r;
        proven_byte_t name[256];
        for (proven_size_t i = 0; i < d->host_len; ++i) name[i] = d->host[i];
        const proven_u8str_view_t origin = { .ptr = name, .size = d->host_len };
        const bool https = d->https;
        const proven_u16 port = d->port;
        const proven_net_addr_t addr = d->to;
        c2conn_t *to = c_find(cl, https, origin, port, &addr, (void *)0);
        if (to && to->state == C2_H1) { r_finish(r, PROVEN_ERR_AGAIN); next = cl->h2_roomless; continue; }       /* the origin turned out to speak HTTP/1.1: the caller's to start again */
        if (!to) {
            if (!may_open) continue;
            const proven_err_t e = c_open(cl, https, origin, port, &addr, &to);
            if (e == PROVEN_ERR_BUSY) { may_open = false; continue; }
            if (e != PROVEN_OK) { r_finish(r, e); next = cl->h2_roomless; continue; }
        }
        r_undelay(r);
        r_attach(r, to);
        if (to->state == C2_UP) c_call_back(to);
        c_arm(to);
    }
    cl->h2_room_busy = false;
    cl_room_arm(cl);
}

/* As it was when it was started: r_submit does the rest. */
static void r_as_new(proven_c2req_t *r) {
    proven_http_event_client_t *cl = r->h.client;
    if (r->out) { cl->alloc.free_fn(cl->alloc.ctx, r->out); r->out = (void *)0; r->out_len = r->out_off = r->out_cap = 0; }
    r->id = 0;
    r->mark = 0;
    r->due = 0;
    r->end_asked = r->engine_done = r->window_shut = r->head_only = false;
}

/* Onto a connection, behind every request that waits there: the oldest waiting request is
 * the last in the list, and this one is older than any that is there - requests that move
 * together arrive newest first, and so keep their order. */
static void r_attach(proven_c2req_t *r, c2conn_t *to) {
    proven_c2req_t *last = to->reqs;
    while (last && last->next) last = last->next;
    r->conn = to;
    r->prev = last;
    r->next = (void *)0;
    if (last) last->next = r;
    else to->reqs = r;
    to->count++;
}

/* The wait before a try is over for the first of them, perhaps for more. */
static void c_on_delay(void *ctx) {
    proven_http_event_client_t *cl = ctx;
    for (;;) {
        proven_c2delay_t *d = cl->h2_delayed;
        if (!d || d->due > proven_time_monotonic_now()) break;
        proven_c2req_t *r = d->r;
        const proven_u8str_view_t host = { .ptr = d->host, .size = d->host_len };
        const bool https = d->https;
        const proven_u16 port = d->port;
        const proven_net_addr_t addr = d->to;
        proven_byte_t name[256];
        for (proven_size_t i = 0; i < host.size; ++i) name[i] = host.ptr[i];
        r_undelay(r);                              /* (frees the record: the origin is in the copies) */
        const proven_u8str_view_t origin = { .ptr = name, .size = host.size };
        c2conn_t *to = c_find(cl, https, origin, port, &addr, (void *)0);
        proven_err_t why = PROVEN_ERR_AGAIN;
        /* (An origin that has turned out to speak HTTP/1.1 since: the caller's to start again.) */
        if (to && to->state == C2_H1) to = (void *)0;
        else if (!to) {
            why = cl->h2_roomless ? PROVEN_ERR_BUSY : c_open(cl, https, origin, port, &addr, &to);
            /* No room for a connection: it waits for that now, as a request just started would. */
            if (why == PROVEN_ERR_BUSY && cl->cfg.max_connections != 0 && (why = r_wait_room(r, https, origin, port, &addr)) == PROVEN_OK) continue;
            if (why != PROVEN_OK) to = (void *)0;
        }
        if (!to) { r_finish(r, why); continue; }
        r_attach(r, to);
        if (to->state == C2_UP) c_call_back(to);
        c_arm(to);
    }
    cl_delay_arm(cl);
}

/* Off its connection and into the list of those that wait to be tried again. False: there
 * was no memory for the record. */
static bool r_delay(proven_c2req_t *r) {
    proven_http_event_client_t *cl = r->h.client;
    c2conn_t *c = r->conn;
    proven_c2delay_t *d = c2_alloc(cl->alloc, sizeof *d);
    if (!d) return false;
    d->next = (void *)0;
    d->r = r;
    d->due = proven_time_monotonic_now() + (proven_time_t)proven_http_event_client_retry_delay_(cl, r->tries) * 1000000;
    d->to = c->to; d->port = c->port; d->https = c->https; d->for_room = false; d->host_len = c->host_len;
    for (proven_size_t i = 0; i < c->host_len; ++i) d->host[i] = c->host[i];
    r->tries++;
    if (r->id != 0 && !c->dead && c->engine) proven_h2_conn_stream_set_user(c->engine, r->id, (void *)0);
    r_unlink(r);
    r_as_new(r);
    r->delay = d;
    /* In the order they are due: behind every one that is due no later. (With one wait for
     * all that is the end of the list; with a wait that grows, a request on its second try
     * may come before one on its third.) */
    proven_c2delay_t *before = cl->h2_delayed_last;
    if (before && before->due > d->due) {
        /* Not behind the last, then: its place is looked for. */
        before = (void *)0;
        for (proven_c2delay_t *i = cl->h2_delayed; i && i->due <= d->due; i = i->next) before = i;
    }
    d->next = before ? before->next : (proven_c2delay_t *)cl->h2_delayed;
    if (before) before->next = d;
    else cl->h2_delayed = d;
    if (!d->next) cl->h2_delayed_last = d;
    if (cl->h2_delayed == d) cl_delay_arm(cl);
    return true;
}

static void r_unsent(proven_c2req_t *r, bool here) {
    if (r->dead) return;
    c2conn_t *c = r->conn;
    proven_http_event_client_t *cl = r->h.client;
    c2conn_t *to = (void *)0;
    proven_err_t why = PROVEN_ERR_AGAIN;
    bool again = c && !cl->closing && !r->streamed && !r->got_head && r->tries < cl->cfg.retry_unsent;
    if (again && cl->cfg.retry_delay_ms != 0) {
        /* Not at once: it leaves this connection now and is placed when the time has passed -
         * on this one again if it is still there, on another, or on a new one. */
        if (r_delay(r)) return;
        again = false;
        why = PROVEN_ERR_NOMEM;
    }
    if (again) {
        const proven_u8str_view_t host = { .ptr = c->host, .size = c->host_len };
        if (here && !c->dead && !c->over && !c->peer_eof && c->state == C2_UP) to = c;
        else to = c_find(cl, c->https, host, c->port, &c->to, c);
        /* (An origin that has turned out to speak HTTP/1.1 since: the caller's to start again.) */
        if (to && to->state == C2_H1) again = false;
        else if (!to) {
            /* A try that cannot be begun ends the request as that try would have. */
            why = cl->h2_roomless ? PROVEN_ERR_BUSY : c_open(cl, c->https, host, c->port, &c->to, &to);
            if (why == PROVEN_ERR_BUSY && cl->cfg.max_connections != 0) {
                /* No room for a connection: off this one, and it waits for a place. */
                proven_byte_t name[256];
                for (proven_size_t i = 0; i < c->host_len; ++i) name[i] = c->host[i];
                const proven_u8str_view_t origin = { .ptr = name, .size = c->host_len };
                const bool https = c->https;
                const proven_u16 port = c->port;
                const proven_net_addr_t addr = c->to;
                r->tries++;
                if (r->id != 0 && !c->dead && c->engine) proven_h2_conn_stream_set_user(c->engine, r->id, (void *)0);
                r_unlink(r);
                r_as_new(r);
                why = r_wait_room(r, https, origin, port, &addr);
                if (why == PROVEN_OK) return;
            }
            if (why != PROVEN_OK) again = false;
        }
    }
    if (!again) { r_finish(r, why); return; }
    r->tries++;
    if (r->id != 0 && !c->dead && c->engine) proven_h2_conn_stream_set_user(c->engine, r->id, (void *)0);
    r_unlink(r);
    r_as_new(r);
    r_attach(r, to);
    if (to != c) {
        /* Its bytes leave from the loop, never from inside another connection's round. */
        if (to->state == C2_UP) c_call_back(to);
        c_arm(to);
    }
}

// -----------------------------------------------------------------------------
// What http_event_client.c calls
// -----------------------------------------------------------------------------


proven_err_t proven_http_event_client_h2_start_(proven_http_event_client_t *cl, const proven_http_event_request_options_t *o,
                                                bool https, const proven_url_t *url, proven_net_addr_t to,
                                                proven_http_event_request_t **out) {
    if (out) *out = (void *)0;
    const proven_u8str_view_t host = url->host;
    const proven_u16 port = url->has_port ? url->port : proven_url_default_port(url->scheme);
    if (host.size == 0 || host.size > 255) return PROVEN_ERR_INVALID_ARG;
    const proven_u8str_view_t method = o->method.size ? o->method : PROVEN_LIT("GET");
    c2conn_t *c = c_find(cl, https, host, port, &to, (void *)0);
    if (c && c->state == C2_H1) return PROVEN_ERR_UNSUPPORTED;
    if (c) {
        /* Counted only when there could be that many: no more wait than there are requests. */
        proven_size_t waiting = 0;
        if (c->count >= cl->cfg.max_waiting_requests) for (const proven_c2req_t *w = c->reqs; w; w = w->next) if (w->id == 0) waiting++;
        if (waiting >= cl->cfg.max_waiting_requests) return PROVEN_ERR_BUSY;
    }

    /* The request, with a copy of everything it was started with: it may have to wait. */
    proven_size_t bytes = o->url.size + method.size + o->header_count * sizeof(proven_http_header_t) + o->body.size;
    for (proven_size_t i = 0; i < o->header_count; ++i) bytes += o->headers[i].name.size + o->headers[i].value.size;
    /* A small copy lies behind the struct, in the one allocation, and stays for the request's
     * life; a large one - a body, mostly - is its own, and goes back once it has been sent. */
    const bool behind = bytes <= C2_COPY_BEHIND;
    proven_c2req_t *r = c2_alloc(cl->alloc, sizeof *r + (behind ? bytes : 0));
    proven_byte_t *copy = !r ? (void *)0 : behind ? (proven_byte_t *)(r + 1) : c2_alloc(cl->alloc, bytes);
    if (!r || !copy) {
        if (r) cl->alloc.free_fn(cl->alloc.ctx, r);
        return PROVEN_ERR_NOMEM;
    }
    for (proven_size_t i = 0; i < sizeof *r; ++i) ((proven_byte_t *)r)[i] = 0;
    r->h = (proven_cl_head_t){ .client = cl, .kind = CL_KIND_H2 };
    r->on = o->on;
    r->ctx = o->ctx;
    r->copy = behind ? (void *)0 : copy;
    r->headers = (proven_http_header_t *)(void *)copy;     /* first: it is the part that needs alignment */
    r->header_count = o->header_count;
    proven_size_t at = o->header_count * sizeof(proven_http_header_t);
    #define C2_KEEP(view_ptr, view_size, dst) do { for (proven_size_t k_ = 0; k_ < (view_size); ++k_) copy[at + k_] = (view_ptr)[k_]; (dst).ptr = copy + at; (dst).size = (view_size); at += (view_size); } while (0)
    C2_KEEP(o->url.ptr, o->url.size, r->url);
    /* The parsed URL's views lie within the caller's text: the same places in the copy. */
    if (url->path.size > 0) {
        const proven_size_t from = (proven_size_t)(url->path.ptr - o->url.ptr);
        const proven_size_t to_ = url->has_query ? (proven_size_t)(url->query.ptr + url->query.size - o->url.ptr) : from + url->path.size;
        r->target = (proven_u8str_view_t){ .ptr = r->url.ptr + from, .size = to_ - from };
    } else {
        r->query_alone = url->has_query;
    }
    r->host_is_ipv6 = url->host_is_ipv6;
    C2_KEEP(method.ptr, method.size, r->method);
    for (proven_size_t i = 0; i < o->header_count; ++i) {
        C2_KEEP(o->headers[i].name.ptr, o->headers[i].name.size, r->headers[i].name);
        C2_KEEP(o->headers[i].value.ptr, o->headers[i].value.size, r->headers[i].value);
    }
    C2_KEEP(o->body.ptr, o->body.size, r->body);
    #undef C2_KEEP
    r->body_length = o->body_length;
    r->early_ok = proven_http_event_client_may_go_early_(method, o);
    r->streamed = o->body.size == 0 && o->body_length != 0;

    if (!c) {
        /* Behind those that already wait for a place, if any do: a place goes to whoever
         * has waited longest. Otherwise a connection - or, when the client may have no more
         * (max_connections) and none is idle, the wait. */
        proven_err_t e = cl->h2_roomless ? PROVEN_ERR_BUSY : c_open(cl, https, host, port, &to, &c);
        if (e == PROVEN_ERR_BUSY && cl->cfg.max_connections != 0) {
            e = r_wait_room(r, https, host, port, &to);
            if (e == PROVEN_OK) {
                cl->request_count++;
                if (out) *out = c2_handle(r);
                return PROVEN_OK;
            }
        }
        if (e != PROVEN_OK) {
            r_free_parts(r);
            cl->alloc.free_fn(cl->alloc.ctx, r);
            return e;
        }
    }
    r->conn = c;
    r->next = c->reqs;
    if (c->reqs) c->reqs->prev = r;
    c->reqs = r;
    c->count++;
    cl->request_count++;
    if (out) *out = c2_handle(r);
    if (c->state == C2_UP && proven_h2_conn_has_peer_settings(c->engine)) {
        /* Onto a stream now, if the server's limit allows; its bytes leave from the loop. An
         * error here is the caller's to have: the request was not started. */
        proven_err_t e = r_submit(r);
        if (e != PROVEN_OK && e != PROVEN_ERR_BUSY) {
            r_unlink(r);
            cl->request_count--;
            r_free_parts(r);
            cl->alloc.free_fn(cl->alloc.ctx, r);
            if (out) *out = (void *)0;
            /* (It takes no more requests: the same call, made again, makes a new connection.) */
            if (e == PROVEN_ERR_INVALID_STATE) { c->over = true; c_call_back(c); return PROVEN_ERR_AGAIN; }
            return e;
        }
        c_call_back(c);                            /* the round runs from the loop, never from here */
    }
    c_arm(c);
    return PROVEN_OK;
}

proven_size_t proven_http_event_client_h2_conns_(const proven_http_event_client_t *cl) {
    proven_size_t n = 0;
    for (const c2conn_t *k = cl->h2; k; k = k->next) if (!k->dead && k->state != C2_H1) n++;
    return n;
}

bool proven_http_event_client_h2_close_idle_(proven_http_event_client_t *cl) {
    for (c2conn_t *k = cl->h2; k; k = k->next) {
        if (k->dead || k->state != C2_UP || k->count != 0 || c_queued(k) != 0) continue;
        k->client_going = true;                    /* (its place is being taken: nobody is to be served from it) */
        c_kill(k, PROVEN_OK);
        return true;
    }
    return false;
}

void proven_http_event_client_h2_destroy_(proven_http_event_client_t *cl) {
    /* Those that wait to be tried again are on no connection: they may be started again. */
    proven_loop_timer_cancel(cl->loop, &cl->h2_delay_timer);
    while (cl->h2_delayed) r_finish(((proven_c2delay_t *)cl->h2_delayed)->r, PROVEN_ERR_AGAIN);
    /* Nor are those that wait for a place: they were never sent, and end as every request of a client that is destroyed does. */
    proven_loop_timer_cancel(cl->loop, &cl->h2_room_timer);
    while (cl->h2_roomless) r_finish(((proven_c2delay_t *)cl->h2_roomless)->r, PROVEN_ERR_RESET);
    while (cl->h2) {
        c2conn_t *c = cl->h2;
        if (c->state == C2_H1) {
            /* Only a memory of an origin: nothing to close. */
            cl->h2 = c->next;
            if (c->next) c->next->prev = (void *)0;
            c->dead = true;
            c_enter(c);
            c_leave(c);
            continue;
        }
        c->client_going = true;
        c_kill(c, PROVEN_ERR_RESET);
    }
}

bool proven_http_event_client_h2_resumed_(proven_http_event_request_t *request) {
    proven_c2req_t *r = c2_of(request);
    if (r->fwd) return proven_http_event_request_resumed_(r->fwd);
    return r->conn && r->conn->tls && proven_tls_is_established(r->conn->tls) && proven_tls_resumed(r->conn->tls);
}

void proven_http_event_client_h2_forwarded_done_(proven_http_event_request_t *face) {
    proven_c2req_t *r = c2_of(face);
    r->fwd = (void *)0;
    r->dead = true;
    r_enter(r);
    r_leave(r);
}

proven_result_size_t proven_http_event_client_h2_write_(proven_http_event_request_t *request, proven_mem_view_t data) {
    proven_c2req_t *r = c2_of(request);
    if (r->fwd) return proven_http_event_request_write(r->fwd, data);
    if (r->dead || !r->streamed || r->end_asked) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_STATE, .value = 0 };
    if (r->body_length != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN && data.size > (r->id ? r->send_left : r->body_length)) return (proven_result_size_t){ .err = PROVEN_ERR_OUT_OF_BOUNDS, .value = 0 };
    c2conn_t *c = r->conn;
    if (r->id == 0) { r->want_writable = true; return (proven_result_size_t){ .err = PROVEN_OK, .value = 0 }; }     /* not on a stream yet */
    r_enter(r);
    c_enter(c);
    c->calls++;
    proven_size_t done = 0;
    bool alive = true;
    for (int tries = 0; alive && done < data.size && tries < 2;) {
        proven_size_t room = c_room(c);
        if (room == 0) { tries++; c_pump(c); if (r->dead) alive = false; continue; }
        proven_size_t n = data.size - done < room ? data.size - done : room;
        proven_result_size_t w = proven_h2_conn_send_data(c->engine, r->id, (proven_mem_view_t){ .ptr = data.ptr + done, .size = n }, false);
        if (w.err != PROVEN_OK) { r_finish(r, PROVEN_ERR_RESET); alive = false; break; }
        r->window_shut = w.value < n;
        if (r->body_length != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) r->send_left -= w.value;
        done += w.value;
        if (r->window_shut) break;
    }
    if (alive && !r->dead && done < data.size) r->want_writable = true;
    c_pump(c);
    alive = alive && !r->dead;                     /* after the pump, which may end the request */
    c->calls--;
    c_leave(c);
    r_leave(r);
    return alive ? (proven_result_size_t){ .err = PROVEN_OK, .value = done } : (proven_result_size_t){ .err = PROVEN_ERR_RESET, .value = 0 };
}

proven_err_t proven_http_event_client_h2_trailers_(const proven_http_event_request_t *request, proven_http_header_t *fields, proven_size_t cap, proven_size_t *count) {
    const proven_c2req_t *r = (const proven_c2req_t *)(const void *)request;
    if (r->fwd) return proven_http_event_request_trailers(r->fwd, fields, cap, count);
    if (!r->body_over) return PROVEN_ERR_INVALID_STATE;
    return proven_http_trailers_give_(&r->in_trailers, fields, cap, count);
}

proven_err_t proven_http_event_client_h2_end_(proven_http_event_request_t *request, const proven_http_header_t *trailers, proven_size_t count) {
    proven_c2req_t *r = c2_of(request);
    if (r->fwd) return count > 0 ? proven_http_event_request_end_trailers(r->fwd, trailers, count) : proven_http_event_request_end(r->fwd);
    if (r->dead || !r->streamed || r->end_asked) return PROVEN_ERR_INVALID_STATE;
    c2conn_t *c = r->conn;
    proven_err_t result = PROVEN_OK;
    if (count > 0) {
        /* As over HTTP/1.1: after a body written with no length. Kept until the body has gone. */
        if (r->body_length != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) return PROVEN_ERR_INVALID_STATE;
        if (!proven_http_trailers_valid_(trailers, count)) return PROVEN_ERR_INVALID_ARG;
        const proven_err_t e = proven_http_trailers_keep_(r->h.client->alloc, &r->out_trailers, trailers, count, false, true);
        if (e != PROVEN_OK) return e;
        if (!c || r->id == 0) {
            /* Not on a stream yet: its HEADERS go without END_STREAM, and the trailers after them. */
            r->end_asked = true;
            return PROVEN_OK;
        }
    }
    if (!c) {
        /* Waiting for a place: on no connection, so on no stream. */
        if (r->body_length != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN && r->body_length != 0) {
            proven_http_event_client_t *cl = r->h.client;
            r_finish(r, PROVEN_ERR_INVALID_FORMAT);
            cl_room_arm(cl);
            return PROVEN_ERR_INVALID_FORMAT;
        }
        r->streamed = false;
        r->body_length = 0;
        return PROVEN_OK;
    }
    r_enter(r);
    c_enter(c);
    c->calls++;
    if (r->body_length != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN && (r->id ? r->send_left : r->body_length) != 0) {
        /* Less was written than promised: the server would wait for the rest for ever. */
        r_finish(r, PROVEN_ERR_INVALID_FORMAT);
        result = PROVEN_ERR_INVALID_FORMAT;
    } else if (r->id == 0) {
        /* Ended before it was ever on a stream: it had no body after all. */
        r->streamed = false;
        r->body_length = 0;
    } else {
        r->end_asked = true;
        r_push(r);
        if (r->dead) result = PROVEN_ERR_RESET;
    }
    c_pump(c);
    c->calls--;
    c_leave(c);
    r_leave(r);
    return result;
}

void proven_http_event_client_h2_abort_(proven_http_event_request_t *request) {
    proven_c2req_t *r = c2_of(request);
    if (r->fwd) { proven_http_event_request_abort(r->fwd); return; }
    if (r->dead) return;
    c2conn_t *c = r->conn;
    if (!c) {
        /* Waiting to be tried again, or for a place: on no connection. */
        proven_http_event_client_t *cl = r->h.client;
        r_finish(r, PROVEN_ERR_RESET);
        cl_delay_arm(cl);
        cl_room_arm(cl);
        return;
    }
    r_enter(r);
    c_enter(c);
    c->calls++;
    r_finish(r, PROVEN_ERR_RESET);
    c_pump(c);
    c->calls--;
    if (!c->dead && c->state != C2_UP) c_arm(c);
    c_leave(c);
    r_leave(r);
}

void proven_http_event_client_h2_pause_(proven_http_event_request_t *request) {
    proven_c2req_t *r = c2_of(request);
    if (r->fwd) { proven_http_event_request_pause(r->fwd); return; }
    if (r->dead || r->paused) return;
    r->paused = true;
    r->due = 0;
    if (r->conn) c_arm(r->conn);
}

void proven_http_event_client_h2_resume_(proven_http_event_request_t *request) {
    proven_c2req_t *r = c2_of(request);
    if (r->fwd) { proven_http_event_request_resume(r->fwd); return; }
    if (r->dead || !r->paused) return;
    proven_http_event_client_t *cl = r->h.client;
    c2conn_t *c = r->conn;
    r->paused = false;
    if (!c) return;                                /* waiting to be tried again: nothing has arrived */
    r_enter(r);
    c_enter(c);
    c->calls++;
    if (r->held_len > 0 || r->held_last) {
        /* What arrived meanwhile, in a buffer that is no longer the request's: a caller that
         * pauses again inside the call starts a new one. */
        proven_byte_t *held = r->held;
        const proven_size_t n = r->held_len;
        const bool last = r->held_last;
        const proven_u32 id = r->id;
        r->held = (void *)0; r->held_len = r->held_cap = 0; r->held_last = false;
        const bool alive = r_deliver(r, (proven_mem_view_t){ .ptr = held, .size = n }, last);
        if (held) cl->alloc.free_fn(cl->alloc.ctx, held);
        if (alive && last) r_finish(r, PROVEN_OK);
        if (!c->dead && c->engine) proven_h2_conn_consumed(c->engine, id, n);
    }
    if (!r->dead) r_expect(r);
    c_pump(c);
    c->calls--;
    if (!c->dead && c->state != C2_UP) c_arm(c);
    c_leave(c);
    r_leave(r);
}

void proven_http_event_client_h2_set_user_(proven_http_event_request_t *request, void *user) { c2_of(request)->user = user; }
void *proven_http_event_client_h2_user_(const proven_http_event_request_t *request) { return c2_of_const(request)->user; }

bool proven_http_event_client_h2_is_h1_(const proven_http_event_request_t *request) {
    const proven_c2req_t *r = c2_of_const(request);
    return r->fwd != (void *)0 || r->id == 0;
}

/* For tests: see proven_internal_http_event_client.h. */
void proven_http_event_client_h2_test_next_id_(proven_http_event_client_t *cl, proven_u32 next_id) {
    if (!cl) return;
    for (c2conn_t *k = cl->h2; k; k = k->next) if (k->engine) proven_h2_conn_test_next_id_(k->engine, next_id);
}

proven_u32 proven_http_event_client_h2_test_window_(proven_http_event_request_t *request) {
    proven_c2req_t *r = request ? c2_of(request) : (proven_c2req_t *)0;
    if (!r || r->dead || r->id == 0 || !r->conn || !r->conn->engine) return 0;
    return proven_h2_conn_test_window_(r->conn->engine, PROVEN_H2_TEST_CAP);
}

int proven_http_event_client_h2_early_(proven_http_event_request_t *request) {
    const proven_c2req_t *r = (const proven_c2req_t *)(const void *)request;
    return r ? r->early_result : 0;
}

#endif
