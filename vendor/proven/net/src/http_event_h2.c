#include "proven/http_event.h"
#include "../platform/proven_sys_net_mem.h"
#include "proven_internal_http_event.h"
#include "proven_internal_http_coding.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven/h2.h"
#include "proven/time.h"

/*
 * HTTP/2 in the event-driven server.
 *
 * A connection that turns out to be HTTP/2 is handed over the way a WebSocket is
 * (proven_internal_http_event.h): the connection's record stays the transport - socket, TLS,
 * one timer - and this file owns what travels on it. The protocol itself is
 * proven_h2_conn_t (h2.h); what is here is the driver that h2.h leaves to its caller: bytes
 * in and out, time, and the handler's callbacks.
 *
 * A handler is given the same `proven_http_stream_t *` it gets for HTTP/1.1, but for HTTP/2
 * that pointer is to an proven_h2s_t below - one stream of many - which shares only its first member
 * with the HTTP/1.1 struct. http_event.c reads `kind` there and sends every call here.
 *
 * Re-entrancy, as in http_event.c: a callback may respond, end, abort, pause or resume, and
 * any of those may end the stream or the connection. So a stream and the driver each count
 * the entries on the stack and are freed when the outermost leaves; nothing reads a field
 * after a callback without having looked at `dead`.
 *
 * One queue: the engine's pending output is offered straight to the transport, which holds
 * none of it. `max_buffered_output` is what the engine may hold before body bytes are refused.
 *
 * **on_writable is never called from inside a call the handler made.** A handler writes in a
 * loop and keeps its place in a variable it updates when the write returns; called again from
 * within that write, it would send the same bytes twice, in the wrong order. HTTP/1.1 cannot
 * do this to it (its write never calls back), and so neither may this: while such a call is on
 * the stack (`calls`), a stream that could be told is left as it is and the transport is asked
 * to report itself writable, which brings the round back from the loop.
 */

typedef struct h2d h2d_t;

typedef struct h2s {
    proven_ev_head_t h;                       /* first: what http_event.c reads */
    h2d_t *d;                          /* not to be followed once `dead` */
    struct h2s *prev, *next;
    void *user;
    proven_byte_t *held;               /* request body that arrived while the handler had paused */
    proven_size_t held_len, held_cap;
    proven_byte_t *out;                /* response body made and not yet taken by the engine */
    proven_size_t out_len, out_off, out_cap;
    proven_http_compressor_t_ *zip;
    proven_u64 response_left;          /* body bytes still promised; LENGTH_UNKNOWN when not counted */
    proven_u64 body_seen;
    proven_time_t body_due;            /* when a request whose body is owed has been silent too long; 0: nothing is owed, or the handler paused it */
    proven_u32 id;
    proven_u32 mark;                   /* the driver's round in which this stream was last looked at */
    proven_http_method_t method;
    bool active;                       /* on_request was delivered and on_done has not been */
    bool response_begun, response_ended, bodiless, paused, dead;
    bool want_writable, compress_asked;
    proven_u8 compress_coding;         /* what a response asked to be compressed would go in: identity, gzip or zstd */
    bool in_done;                      /* the request's end has arrived */
    bool held_last;                    /* ... while paused: it is delivered with what is held */
    bool end_asked;                    /* END_STREAM goes when `out` has drained */
    bool engine_done;                  /* the engine has seen this side's end already (a response with no body) */
    bool window_shut;                  /* the engine took less than it was offered: the peer's window, and only the engine knows when it opens */
    bool tunnel;                       /* an extended CONNECT (RFC 8441): what follows the head is a protocol's, not a body */
    bool early;                        /* its request arrived in early data (RFC-0027) */
    /* A WebSocket on this stream, once its handler accepted the extended CONNECT: its object,
     * whose first member is its operations (proven_internal_http_event.h), and its timer. Null
     * when there is none, and from the moment it let go of the stream (close, kill). */
    void *raw;
    proven_loop_timer_t timer;
    bool told_drain;                   /* the WebSocket was told the server is ending */
    proven_size_t held_off;            /* a WebSocket's: how much of `held` it has taken */
    proven_http_trailers_t_ in_trailers;   /* the request's, from its end until on_done (proven_http_stream_trailers) */
    proven_http_trailers_t_ out_trailers;  /* the response's, sent in place of END_STREAM once `out` has drained */
} proven_h2s_t;

struct h2d {
    const proven_http_event_raw_ops_t *ops;      /* first: http_event.c finds the operations here */
    proven_http_stream_t *conn;        /* the transport; not to be followed once `dead` */
    proven_http_event_server_t *server;
    proven_h2_conn_t *engine;
    proven_h2s_t *streams;
    proven_h2s_t *turn;                       /* the stream whose output was moved last in a round: the next round goes on behind it */
    proven_size_t share;               /* while `sharing`: how much more the stream whose turn it is may move */
    bool sharing;
    proven_size_t stream_count;
    proven_u32 depth, round;
    bool dead;                         /* the transport is gone, or was let go of */
    bool pumping;
    bool reading;                      /* input is being worked through: what it makes is written once, at its end */
    bool asked;                        /* a request has arrived: from here on a quiet connection is idle, not slow */
    proven_u32 calls;                  /* calls a handler made that are now on the stack */
    proven_time_t quiet_due;           /* when the connection's own silence - output not moving, nothing asked, nothing open - has lasted too long; 0: none is being measured */
    bool over;                         /* nothing more will be taken: write what is owed and close */
    bool eof;                          /* the peer closed its sending side */
    bool held;                         /* input is left where it is - the connection is not read - until the engine's output is under its bound again */
    bool early_now;                    /* everything the engine has taken so far arrived as early data (RFC-0027) */
};

/* The engine's clock for growing windows: microseconds. */
static proven_u64 h2_now_us(void) { return (proven_u64)proven_time_monotonic_now() / 1000u; }



static void *h2_alloc(proven_allocator_t a, proven_size_t n) {
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, n, 16);
    return m.err == PROVEN_OK ? m.value.ptr : (void *)0;
}

/* Append to a buffer that grows; false when there is no memory. */
static bool h2_append(proven_allocator_t a, proven_byte_t **buf, proven_size_t *len, proven_size_t *cap, const proven_byte_t *p, proven_size_t n) {
    if (n == 0) return true;
    if (*len + n > *cap) {
        proven_size_t want = *cap ? *cap : 256;
        while (want < *len + n) { if (want > (proven_size_t)-1 / 2) return false; want *= 2; }
        proven_byte_t *q = h2_alloc(a, want);
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

// -----------------------------------------------------------------------------
// Lifetimes
// -----------------------------------------------------------------------------

static void d_pump(h2d_t *d);

static void d_enter(h2d_t *d) { d->depth++; }

static void d_leave(h2d_t *d) {
    if (--d->depth > 0 || !d->dead) return;
    proven_allocator_t a = d->server->alloc;
    proven_h2_conn_destroy(d->engine);
    a.free_fn(a.ctx, d);
}

static void s_enter(proven_h2s_t *s) { s->h.depth++; }

static void s_leave(proven_h2s_t *s) {
    if (--s->h.depth > 0 || !s->dead) return;
    proven_allocator_t a = s->h.server->alloc;
    a.free_fn(a.ctx, s);
}

/* The exchange is over: tell the handler, once, and let go of everything but the struct,
 * which goes when the outermost entry leaves. */
static void s_finish(proven_h2s_t *s, proven_err_t why) {
    if (s->dead) return;
    proven_http_event_server_t *sv = s->h.server;
    proven_allocator_t a = sv->alloc;
    h2d_t *d = s->d;
    s_enter(s);
    s->dead = true;
    if (d->turn == s) d->turn = s->prev;
    if (s->prev) s->prev->next = s->next;
    else d->streams = s->next;
    if (s->next) s->next->prev = s->prev;
    d->stream_count--;
    if (!d->dead) proven_h2_conn_stream_set_user(d->engine, s->id, (void *)0);
    if (s->zip) { proven_http_compressor_destroy_(s->zip); s->zip = (void *)0; }
    if (s->held) { a.free_fn(a.ctx, s->held); s->held = (void *)0; s->held_len = s->held_cap = 0; }
    if (s->out) { a.free_fn(a.ctx, s->out); s->out = (void *)0; s->out_len = s->out_off = s->out_cap = 0; }
    proven_loop_timer_cancel(sv->loop, &s->timer);
    if (s->active) {
        s->active = false;
        if (sv->cfg.on.on_done) sv->cfg.on.on_done(sv->cfg.ctx, (proven_http_stream_t *)(void *)s, why);
    }
    proven_http_trailers_free_(a, &s->in_trailers);
    proven_http_trailers_free_(a, &s->out_trailers);
    if (s->raw) {
        void *raw = s->raw;
        s->raw = (void *)0;
        (*(const proven_http_event_raw_ops_t *const *)raw)->on_closed(raw, why);
    }
    s_leave(s);
}

/* Give up on a stream from this side: the peer is told, the handler is told why. */
static void s_reset(proven_h2s_t *s, proven_h2_error_t code, proven_err_t why) {
    if (s->dead) return;
    if (!s->d->dead && !s->engine_done) (void)proven_h2_conn_reset(s->d->engine, s->id, code);
    s_finish(s, why);
}

/* The transport is gone, or is being let go of: every exchange ends here. */
static void d_end_streams(h2d_t *d, proven_err_t why) {
    while (d->streams) s_finish(d->streams, why);
}

/* End the connection at once. */
static void d_kill(h2d_t *d, proven_err_t why) {
    if (d->dead) return;
    proven_http_stream_t *conn = d->conn;
    d_enter(d);
    d->dead = true;
    d_end_streams(d, why);
    proven_http_event_raw_kill_(conn);             /* detaches first: on_closed is not called */
    d_leave(d);
}

// -----------------------------------------------------------------------------
// Time
// -----------------------------------------------------------------------------

static proven_size_t d_queued(const h2d_t *d) {
    return proven_h2_conn_pending_output(d->engine).size + proven_http_event_raw_held_(d->conn);
}

/* A request's body is owed from now: it has `body_timeout_ms` to say something. */
static void s_expect_body(proven_h2s_t *s) {
    s->body_due = s->in_done || s->paused || s->tunnel ? 0 : proven_time_monotonic_now() + (proven_time_t)s->h.server->cfg.body_timeout_ms * 1000000;
}

/* One timer, for whatever the connection is waiting for. Output that does not move and a
 * connection with nothing on it are the connection's silences, measured from its last
 * activity. A request body is each stream's own: the timer is set for the earliest that is
 * due, so a request that has stalled is found though its neighbours are busy. */
static void d_arm(h2d_t *d) {
    if (d->dead) return;
    const proven_http_event_server_config_t *cfg = &d->server->cfg;
    proven_u32 ms = 0;
    if (d_queued(d) > 0) ms = cfg->write_timeout_ms;
    else if (!d->asked) ms = cfg->head_timeout_ms;
    else if (d->stream_count == 0) ms = cfg->idle_timeout_ms;
    const proven_time_t now = proven_time_monotonic_now();
    d->quiet_due = ms ? now + (proven_time_t)ms * 1000000 : 0;
    proven_time_t due = 0;
    for (const proven_h2s_t *s = d->streams; s; s = s->next)
        if (s->body_due != 0 && (due == 0 || s->body_due < due)) due = s->body_due;
    if (due != 0) {
        const proven_time_t left = due > now ? (due - now) / 1000000 + 1 : 1;
        if (ms == 0 || (proven_time_t)ms > left) ms = (proven_u32)left;
    }
    /* With neither, every request is whole and the handlers are thinking: their business. */
    proven_http_event_raw_timer_(d->conn, ms);
}

// -----------------------------------------------------------------------------
// A stream's output
// -----------------------------------------------------------------------------

static proven_size_t s_out_pending(const proven_h2s_t *s) { return s->out_len - s->out_off; }

/* How much more body the engine may be given before it holds `max_buffered_output`. */
/* What one stream moves towards the engine in its turn of a round. Streams that hold output
 * take turns of this much, so that the room under the output bound - which the first to be
 * asked would otherwise fill by itself, round after round - is shared between them.
 * (While a turn lasts d_room answers with what is left of it, to whoever asks: a handler that
 * writes to another stream from inside this one's callback is refused sooner than the bound
 * alone would, and is told when it may write like any other.) */
#define H2_TURN_BYTES ((proven_size_t)65536)

static proven_size_t d_room(const h2d_t *d) {
    const proven_size_t held = proven_h2_conn_pending_output(d->engine).size, bound = d->server->cfg.max_buffered_output;
    const proven_size_t room = held < bound ? bound - held : 0;
    return d->sharing && d->share < room ? d->share : room;
}

/* Move what a stream has made towards the engine; send its end when all of it has gone; tell
 * a handler that was refused that it may write again. */
static void s_push(proven_h2s_t *s) {
    if (s->dead || !s->response_begun) return;
    h2d_t *d = s->d;
    proven_http_event_server_t *sv = s->h.server;
    s_enter(s);
    while (!s->dead && s_out_pending(s) > 0) {
        proven_size_t n = d_room(d);
        if (n == 0) break;
        if (n > s_out_pending(s)) n = s_out_pending(s);
        proven_result_size_t r = proven_h2_conn_send_data(d->engine, s->id, (proven_mem_view_t){ .ptr = s->out + s->out_off, .size = n }, false);
        if (r.err != PROVEN_OK) { s_finish(s, PROVEN_ERR_RESET); break; }
        s->out_off += r.value;
        if (r.value > 0 && s->raw) s->want_writable = true;     /* output moved: the WebSocket may want to know */
        if (d->sharing) d->share -= r.value < d->share ? r.value : d->share;
        if (r.value < n) { s->window_shut = true; break; }      /* the peer's window: the engine says when */
    }
    if (!s->dead && s_out_pending(s) == 0) {
        if (s->out) { sv->alloc.free_fn(sv->alloc.ctx, s->out); s->out = (void *)0; s->out_len = s->out_off = s->out_cap = 0; }
        if (s->end_asked) {
            proven_result_size_t r = s->engine_done            ? (proven_result_size_t){ .err = PROVEN_OK, .value = 0 }
                                     : s->out_trailers.count ? (proven_result_size_t){ .err = proven_h2_conn_send_trailers(d->engine, s->id, s->out_trailers.fields, s->out_trailers.count) }
                                                             : proven_h2_conn_send_data(d->engine, s->id, (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, true);
            s_finish(s, r.err == PROVEN_OK ? PROVEN_OK : PROVEN_ERR_RESET);
        } else if (s->want_writable && !s->window_shut && d_room(d) > 0) {
            if (d->calls > 0) {
                proven_http_event_raw_again_(d->conn);
            } else {
                s->want_writable = false;
                if (s->raw) (*(const proven_http_event_raw_ops_t *const *)s->raw)->on_progress(s->raw);
                else if (sv->cfg.on.on_writable) sv->cfg.on.on_writable(sv->cfg.ctx, (proven_http_stream_t *)(void *)s);
            }
        }
    }
    s_leave(s);
}

// -----------------------------------------------------------------------------
// Requests
// -----------------------------------------------------------------------------

/* Answer a request the server will not pass on, with a status and nothing else. */
static void d_refuse(h2d_t *d, proven_u32 id, const char status[4]) {
    proven_http_header_t h[1] = { { .name = PROVEN_LIT(":status"), .value = proven_u8str_view_from_bytes(status, 3) } };
    if (proven_h2_conn_submit_response(d->engine, id, h, 1, true) != PROVEN_OK) (void)proven_h2_conn_reset(d->engine, id, PROVEN_H2_REFUSED_STREAM);
}

static void d_request(h2d_t *d, const proven_h2_event_t *ev) {
    proven_http_event_server_t *sv = d->server;
    const proven_h2_request_head_t *rq = &ev->request;
    const bool connect = rq->path.size == 0 && rq->scheme.size == 0;
    /* An extended CONNECT (RFC 8441) - which the engine takes only when the server offered it.
     * The handler sees CONNECT, its path as the target, and `:protocol` as the first field:
     * that is what proven_ws_check_request looks for. */
    const bool tunnel = rq->protocol.size > 0;
    /* The fields are copied into the server's array before the handler sees them - its own
     * answer takes the connection, and the event's list is not promised past that. */
    bool has_host = false, has_length = false, bad = false;
    proven_u64 length = 0;
    for (proven_size_t i = 0; i < rq->field_count; ++i) {
        const proven_http_header_t *f = &rq->fields[i];
        if (proven_u8str_view_eq_ascii_ci(f->name, PROVEN_LIT("host"))) {
            /* A request that names two different targets is how one is smuggled past a
             * front end that looked at the other (RFC 9113, section 8.3.1). */
            if (has_host || (rq->authority.size > 0 && !proven_u8str_view_eq_ascii_ci(f->value, rq->authority))) bad = true;
            has_host = true;
        } else if (proven_u8str_view_eq_ascii_ci(f->name, PROVEN_LIT("content-length"))) {
            has_length = true;
            length = 0;
            for (proven_size_t k = 0; k < f->value.size && length <= sv->cfg.max_body_bytes; ++k) length = length * 10 + (proven_u64)(f->value.ptr[k] - '0');
        }
    }
    d->asked = true;
    if (bad) { (void)proven_h2_conn_reset(d->engine, ev->stream_id, PROVEN_H2_PROTOCOL_ERROR); return; }
    /* A request that arrived in early data and is not safe to repeat: 425 Too Early, from here
     * (RFC 8470); the client sends it again after the handshake. */
    const proven_http_method_t m = proven_http_method_from_text(rq->method);
    if (d->early_now && !(ev->end_stream && !tunnel && (m == PROVEN_HTTP_GET || m == PROVEN_HTTP_HEAD || m == PROVEN_HTTP_OPTIONS))) {
        d_refuse(d, ev->stream_id, "425");
        return;
    }
    const proven_size_t add_host = !has_host && rq->authority.size > 0 ? 1 : 0;
    /* A handler tells whether a body is coming from the fields, as HTTP/1.1 taught it to:
     * a length, or "chunked". HTTP/2 may send a body with neither - the frames say where it
     * ends - so one of unannounced length is given the field that means exactly that. */
    const proven_size_t add_chunked = !ev->end_stream && !has_length && !tunnel ? 1 : 0;
    const proven_size_t add_protocol = tunnel ? 1 : 0;
    if (rq->field_count + add_host + add_chunked + add_protocol > sv->cfg.max_headers) { d_refuse(d, ev->stream_id, "431"); return; }
    if (length > sv->cfg.max_body_bytes) { d_refuse(d, ev->stream_id, "413"); return; }

    proven_h2s_t *s = h2_alloc(sv->alloc, sizeof *s);
    if (!s) { d_refuse(d, ev->stream_id, "503"); return; }
    for (proven_size_t i = 0; i < sizeof *s; ++i) ((proven_byte_t *)s)[i] = 0;
    proven_size_t n = 0;
    if (add_protocol) sv->headers[n++] = (proven_http_header_t){ .name = PROVEN_LIT(":protocol"), .value = rq->protocol };
    if (add_host) sv->headers[n++] = (proven_http_header_t){ .name = PROVEN_LIT("host"), .value = rq->authority };
    for (proven_size_t i = 0; i < rq->field_count; ++i) sv->headers[n++] = rq->fields[i];
    if (add_chunked) sv->headers[n++] = (proven_http_header_t){ .name = PROVEN_LIT("transfer-encoding"), .value = PROVEN_LIT("chunked") };
    proven_http_request_t req = {
        .method = proven_http_method_from_text(rq->method), .method_text = rq->method,
        .target = connect ? rq->authority : rq->path,
        .version_minor = 1,                        /* what a handler branches on is 1.0 against 1.1: HTTP/2 is not the older one */
        .headers = sv->headers, .header_count = n,
    };
    s->h = (proven_ev_head_t){ .server = sv, .version_minor = 1, .kind = EV_KIND_H2 };
    s->d = d;
    s->id = ev->stream_id;
    s->method = req.method;
    s->response_left = PROVEN_HTTP_EVENT_LENGTH_UNKNOWN;
    s->compress_coding = (proven_u8)proven_http_compress_choose_(sv->headers, n, sv->cfg.compress_zstd);
    s->in_done = ev->end_stream;
    s->tunnel = tunnel;
    s->early = d->early_now;
    s_expect_body(s);
    s->active = true;
    s->next = d->streams;
    if (d->streams) d->streams->prev = s;
    d->streams = s;
    d->stream_count++;
    proven_h2_conn_stream_set_user(d->engine, s->id, s);
    const bool expects = !ev->end_stream && proven_http_header_has_token(sv->headers, n, PROVEN_LIT("expect"), PROVEN_LIT("100-continue"));

    s_enter(s);
    proven_http_event_request_begin_(sv);
    sv->cfg.on.on_request(sv->cfg.ctx, (proven_http_stream_t *)(void *)s, &req);
    proven_http_event_request_end_(sv);
    if (!s->dead && !d->dead && expects && !s->response_begun) {
        proven_http_header_t go_on[1] = { { .name = PROVEN_LIT(":status"), .value = PROVEN_LIT("100") } };
        (void)proven_h2_conn_submit_response(d->engine, s->id, go_on, 1, false);
    }
    s_leave(s);
}

/* A piece of a request body, or its end. */
static void s_raw_in(proven_h2s_t *s, proven_mem_view_t piece, bool last);

static void d_body(h2d_t *d, proven_h2s_t *s, proven_mem_view_t piece, bool last) {
    proven_http_event_server_t *sv = d->server;
    if (s->raw) { s_raw_in(s, piece, last); return; }
    if (s->tunnel) {
        /* An extended CONNECT whose handler has not taken it yet: what follows is the
         * protocol's, kept - not consumed - for when it does. */
        if (!h2_append(sv->alloc, &s->held, &s->held_len, &s->held_cap, piece.ptr, piece.size)) { s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_NOMEM); return; }
        if (last) s->in_done = true;
        return;
    }
    s->body_seen += piece.size;
    if (!s->tunnel && s->body_seen > sv->cfg.max_body_bytes) {
        if (!s->response_begun) {
            /* A complete answer to an unfinished request tells the client to stop sending. */
            proven_http_header_t h[1] = { { .name = PROVEN_LIT(":status"), .value = PROVEN_LIT("413") } };
            if (proven_h2_conn_submit_response(d->engine, s->id, h, 1, true) == PROVEN_OK) s->engine_done = true;
        }
        s_reset(s, PROVEN_H2_CANCEL, PROVEN_ERR_OUT_OF_BOUNDS);
        return;
    }
    if (last) s->in_done = true;
    s_expect_body(s);                              /* it spoke: the wait for its next piece begins again */
    if (s->paused) {
        /* The handler said "not now", and on HTTP/1.1 that means no call until it resumes.
         * Here the bytes have already arrived, so they are kept - at most one window of
         * them, since the peer is not told they were consumed and can send no more. */
        if (!h2_append(sv->alloc, &s->held, &s->held_len, &s->held_cap, piece.ptr, piece.size)) { s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_NOMEM); return; }
        if (last) s->held_last = true;
        return;
    }
    const proven_u32 id = s->id;
    s_enter(s);
    if ((piece.size > 0 || last) && sv->cfg.on.on_body) sv->cfg.on.on_body(sv->cfg.ctx, (proven_http_stream_t *)(void *)s, piece, last);
    s_leave(s);
    if (!d->dead) proven_h2_conn_consumed(d->engine, id, piece.size);      /* nothing, if the stream has ended */
}

static void d_event(h2d_t *d, const proven_h2_event_t *ev) {
    proven_h2s_t *s = ev->user;
    switch (ev->kind) {
        case PROVEN_H2_EVENT_HEADERS: d_request(d, ev); break;
        case PROVEN_H2_EVENT_DATA:
            if (s) d_body(d, s, ev->data, ev->end_stream);
            else proven_h2_conn_consumed(d->engine, ev->stream_id, ev->data.size);
            break;
        case PROVEN_H2_EVENT_TRAILERS:             /* kept for proven_http_stream_trailers, then the body's end */
            if (s && proven_http_trailers_keep_(d->server->alloc, &s->in_trailers, ev->fields, ev->field_count, true, false) != PROVEN_OK) {
                s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_NOMEM);
                break;
            }
            if (s) d_body(d, s, (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, true);
            break;
        case PROVEN_H2_EVENT_STREAM_RESET: if (s) { s->engine_done = true; s_finish(s, PROVEN_ERR_RESET); } break;
        case PROVEN_H2_EVENT_STREAM_WRITABLE: if (s) { s->window_shut = false; s_push(s); } break;
        case PROVEN_H2_EVENT_CLOSED:
            d->over = true;
            d_end_streams(d, PROVEN_ERR_RESET);
            break;
        default: break;                            /* GOAWAY, PONG: the engine has done what they ask */
    }
}

// -----------------------------------------------------------------------------
// The round: output to the transport, and what that makes due
// -----------------------------------------------------------------------------

/* The engine's bound on output: twice the server's. */
static proven_size_t h2_engine_bound(const proven_http_event_server_t *sv) {
    return sv->cfg.max_buffered_output < 512 ? 1024
         : sv->cfg.max_buffered_output > ((proven_size_t)1 << 29) ? (proven_size_t)1 << 30 : sv->cfg.max_buffered_output * 2;
}

/* Whether this side holds as much output as it may - the engine's and what the loop holds for
 * the connection: the transport is not taking it. (The loop's share counts too: on io_uring every
 * send waits in the loop until the next entry into the kernel, and the engine alone would then stop
 * at its room for bodies, short of the bound, while requests kept being taken - RFC-0030 D-3.) */
static bool d_full(const h2d_t *d) {
    return d_queued(d) >= h2_engine_bound(d->server);
}

static void d_pump(h2d_t *d) {
    /* While a read is being worked through, what it makes is held back and written when the read
     * is done: a hundred requests that came in one read are answered in one write, not a
     * hundred. Only while little is held, though - past a quarter of the bound it leaves at
     * once, as it always did, so that a response's head never finds the engine full of the
     * bodies before it. h2_on_input pumps when the read is done. */
    if (d->pumping || d->dead) return;
    if (d->reading && d_queued(d) < d->server->cfg.max_buffered_output / 4) return;
    d->pumping = true;
    d_enter(d);
    for (;;) {
        bool progress = false;
        while (!d->dead) {
            /* Offered even when there is nothing: that withdraws a request to be called back. */
            proven_mem_view_t out = proven_h2_conn_pending_output(d->engine);
            proven_size_t n = proven_http_event_raw_offer_(d->conn, out);     /* a dead transport comes back through on_closed */
            if (d->dead || n == 0) break;
            proven_h2_conn_output_sent(d->engine, n);
            progress = true;
        }
        if (d->dead) break;
        /* What sending made due: a stream the engine had cut short. */
        for (;;) {
            proven_size_t used = 0;
            proven_h2_event_t ev;
            if (proven_h2_conn_feed(d->engine, (proven_mem_view_t){ .ptr = (void *)0, .size = 0 }, &used, &ev) != PROVEN_OK || ev.kind == PROVEN_H2_EVENT_NONE) break;
            d_event(d, &ev);
            progress = true;
            if (d->dead) break;
        }
        if (d->dead) break;
        /* And what this driver itself held back for the output bound: each stream that holds
         * some has a turn of H2_TURN_BYTES, beginning behind the one that had the last turn.
         * (From the list's head every time, the newest streams took all the room there was
         * and an older one waited for as long as they had something to send.) A handler's
         * callback may end any stream, so the list is searched again after each. */
        d->round++;
        for (bool again = true; again && !d->dead;) {
            again = false;
            proven_h2s_t *const first = d->turn && d->turn->next ? d->turn->next : d->streams;
            for (proven_h2s_t *s = first; s; ) {
                proven_h2s_t *const after = s->next ? s->next : d->streams;
                if (!(s->mark == d->round || s->window_shut || !(s_out_pending(s) > 0 || s->end_asked || s->want_writable))) {
                    s->mark = d->round;
                    if (!(d_room(d) == 0 && !(s->end_asked && s_out_pending(s) == 0))) {
                        const proven_size_t before = proven_h2_conn_pending_output(d->engine).size;
                        d->turn = s;
                        d->sharing = true; d->share = H2_TURN_BYTES;
                        s_push(s);
                        d->sharing = false;
                        if (!d->dead && proven_h2_conn_pending_output(d->engine).size != before) progress = true;
                        again = true;
                        break;
                    }
                }
                s = after == first ? (void *)0 : after;
            }
        }
        if (d->dead || !progress) break;
    }
    if (!d->dead) {
        /* Nothing more to take and nothing left to write: the connection is finished with. */
        if (d->eof && d->stream_count == 0) d->over = true;
        if (d->over && d_queued(d) == 0) {
            proven_http_stream_t *conn = d->conn;
            d->dead = true;
            proven_http_event_raw_close_(conn);    /* detached; closes politely */
        } else {
            d_arm(d);
        }
    }
    d->pumping = false;
    /* Input that was left for want of room is taken up again when there is some: the
     * connection is read again, and what it had already read is worked through now. */
    if (!d->dead && d->held && !d->reading && !d_full(d)) {
        d->held = false;
        proven_http_event_raw_pause_(d->conn, false);
    }
    d_leave(d);
}

// -----------------------------------------------------------------------------
// What the connection's record asks of its owner
// -----------------------------------------------------------------------------

static proven_size_t h2_on_input(void *raw, proven_mem_mut_t in) {
    h2d_t *d = raw;
    d_enter(d);
    proven_size_t pos = 0;
    /* How much of this input arrived early: a request whose HEADERS ended inside it did. */
    const proven_size_t early = proven_http_event_early_ahead_(d->conn);
    for (;;) {
        d->reading = true;
        /* Nothing is taken while the engine's output is at its bound - which is while the
         * client is not reading. What a request needs first is room for its answer's head,
         * and a client that asks without reading must not be able to make this side hold
         * an answer for everything it sends: the rest stays in the connection's buffer, and
         * behind that in the socket. */
        while (!d->dead && !d->over && pos < in.size && !d_full(d)) {
            proven_size_t used = 0;
            proven_h2_event_t ev;
            proven_err_t e = proven_h2_conn_feed(d->engine, (proven_mem_view_t){ .ptr = in.ptr + pos, .size = in.size - pos }, &used, &ev);
            if (e != PROVEN_OK) { d_kill(d, e == PROVEN_ERR_NOMEM ? PROVEN_ERR_NOMEM : PROVEN_ERR_RESET); break; }
            pos += used;
            d->early_now = used > 0 ? pos <= early : d->early_now;
            if (ev.kind != PROVEN_H2_EVENT_NONE) d_event(d, &ev);
            else if (pos == in.size) break;
        }
        d->reading = false;
        d_pump(d);
        /* The write that ends a read may have made the room that was missing. */
        if (d->dead || d->over || pos == in.size || d_full(d)) break;
    }
    const bool dead = d->dead, over = d->over;
    if (!dead && !over && pos < in.size && !d->held) {
        d->held = true;
        proven_http_event_raw_pause_(d->conn, true);
    }
    d_leave(d);
    return dead || over ? in.size : pos;           /* what was not taken is offered again when the connection is read again */
}

static void s_raw_feed(proven_h2s_t *s);

/* Each WebSocket stream that has input it can take now - after a resume, or input that came
 * before it was accepted. A callback may end any stream: while the one fed is still open its
 * `next` is live, otherwise the walk starts again from the head. */
static void d_raw_feed_all(h2d_t *d) {
    for (proven_h2s_t *s = d->streams; s && !d->dead;) {
        if (s->dead || !s->raw || s->paused || s->held_off == s->held_len) { s = s->next; continue; }
        s_enter(s);
        s_raw_feed(s);
        const bool open = !s->dead;
        proven_h2s_t *next = open ? s->next : (void *)0;
        s_leave(s);
        s = open ? next : d->streams;
    }
}

static void h2_on_progress(void *raw) {
    h2d_t *d = raw;
    if (d->dead) return;
    d_enter(d);
    /* The connection's output moved: a WebSocket that counts it (proven_http_event_h2_buffered_)
     * and was refused is asked again in this round's turns. With sends through io_uring the
     * output stays in the loop until the next round, so a stream's own output having moved
     * earlier is not the last news it needs. */
    for (proven_h2s_t *s = d->streams; s; s = s->next) if (s->raw && !s->dead) s->want_writable = true;
    d_raw_feed_all(d);
    d_pump(d);
    d_leave(d);
}

static void h2_on_timer(void *raw) {
    h2d_t *d = raw;
    if (d->dead) return;
    d_enter(d);
    /* Requests whose bodies have stopped arriving, first: they end, the others go on. */
    const proven_time_t now = proven_time_monotonic_now();
    for (bool again = true; again;) {
        again = false;
        for (proven_h2s_t *s = d->streams; s; s = s->next) {
            if (s->body_due == 0 || s->body_due > now) continue;
            s_reset(s, PROVEN_H2_CANCEL, PROVEN_ERR_TIMEOUT);
            again = true;
            break;
        }
    }
    if (d->dead) { d_leave(d); return; }
    if (d->quiet_due == 0 || now < d->quiet_due) {
        d_pump(d);                                 /* a stream's hour, not the connection's: the round sets the timer again */
    } else if (d_queued(d) > 0 || !d->asked) {
        /* The client is not taking what it asked for, or never asked. */
        d_kill(d, PROVEN_ERR_TIMEOUT);
    } else if (d->stream_count == 0) {
        proven_h2_conn_shutdown(d->engine, PROVEN_H2_NO_ERROR);            /* idle: say goodbye */
        d_pump(d);
    } else {
        d_pump(d);
    }
    d_leave(d);
}

static void h2_on_closed(void *raw, proven_err_t why) {
    h2d_t *d = raw;
    if (d->dead) return;
    d_enter(d);
    d->dead = true;
    d_end_streams(d, why == PROVEN_OK ? PROVEN_ERR_RESET : why);
    d_leave(d);
}

/* The client will send nothing more. What it asked for in full is still answered. */
static bool h2_on_eof(void *raw) {
    h2d_t *d = raw;
    if (d->dead) return false;
    d_enter(d);
    d->eof = true;
    for (bool again = true; again;) {
        again = false;
        for (proven_h2s_t *s = d->streams; s; s = s->next) {
            if (s->in_done) continue;
            s_reset(s, PROVEN_H2_CANCEL, PROVEN_ERR_RESET);
            again = true;
            break;
        }
    }
    const bool keep = !d->dead && (d->stream_count > 0 || d_queued(d) > 0);
    if (keep) d_pump(d);
    const bool still = keep && !d->dead;
    d_leave(d);
    return still;
}

static void h2_on_shutdown(void *raw) {
    h2d_t *d = raw;
    if (d->dead) return;
    d_enter(d);
    proven_h2_conn_shutdown(d->engine, PROVEN_H2_NO_ERROR);
    d_pump(d);
    d_leave(d);
}

/* The server is ending: each WebSocket on this connection is told, once (it closes with 1001). */
static void h2_on_drain(void *raw) {
    h2d_t *d = raw;
    if (d->dead) return;
    d_enter(d);
    for (proven_h2s_t *s = d->streams; s && !d->dead;) {
        if (s->dead || !s->raw || s->told_drain) { s = s->next; continue; }
        s->told_drain = true;
        const proven_http_event_raw_ops_t *ops = *(const proven_http_event_raw_ops_t *const *)s->raw;
        if (!ops->on_drain) { s = s->next; continue; }
        s_enter(s);
        ops->on_drain(s->raw);
        const bool open = !s->dead;
        proven_h2s_t *next = open ? s->next : (void *)0;
        s_leave(s);
        s = open ? next : d->streams;
    }
    d_pump(d);
    d_leave(d);
}

static const proven_http_event_raw_ops_t h2_ops = {
    .on_input = h2_on_input, .on_progress = h2_on_progress, .on_timer = h2_on_timer, .on_closed = h2_on_closed,
    .on_eof = h2_on_eof, .on_shutdown = h2_on_shutdown, .on_drain = h2_on_drain,
};

/* RFC 9113, section 9.2: TLS 1.2 at least, and of 1.2 only the suites that keep forward
 * secrecy and authenticate what they encrypt. (TLS 1.3 has no others.) */
static bool h2_tls_adequate(const proven_tls_conn_t *tls) {
    const proven_u16 version = proven_tls_version(tls), suite = proven_tls_cipher_suite(tls);
    if (version >= 0x0304) return true;
    if (version < 0x0303) return false;
    return suite == 0xc02b || suite == 0xc02c || suite == 0xc02f || suite == 0xc030 ||
           suite == 0xcca8 || suite == 0xcca9 || suite == 0xccaa || suite == 0x009e || suite == 0x009f;
}

bool proven_http_event_h2_owns_(const void *raw) {
    return raw && *(const proven_http_event_raw_ops_t *const *)raw == &h2_ops;
}

proven_err_t proven_http_event_h2_take_(proven_http_stream_t *conn) {
    proven_http_event_server_t *sv = conn->h.server;
    proven_u32 list = sv->cfg.max_head_bytes > ((proven_size_t)1 << 20) ? (proven_u32)1 << 20 : (proven_u32)sv->cfg.max_head_bytes;
    if (!sv->h2buf) {
        sv->h2buf_size = proven_h2_header_buffer_size(list);
        sv->h2buf = h2_alloc(sv->alloc, sv->h2buf_size);
        if (!sv->h2buf) return PROVEN_ERR_NOMEM;
    }
    h2d_t *d = h2_alloc(sv->alloc, sizeof *d);
    if (!d) return PROVEN_ERR_NOMEM;
    for (proven_size_t i = 0; i < sizeof *d; ++i) ((proven_byte_t *)d)[i] = 0;
    proven_h2_config_t cfg = {
        .alloc = sv->alloc, .server = true,
        .max_concurrent_streams = (proven_u32)(sv->cfg.max_concurrent_streams > 100000 ? 100000 : sv->cfg.max_concurrent_streams),
        .max_header_list_bytes = list,
        .initial_window = (proven_u32)sv->cfg.http2_window_bytes,
        .enable_connect_protocol = sv->cfg.http2_websocket,
        /* Twice the server's bound: the first half is for bodies, which this driver stops
         * offering at the bound; the rest is room for response heads and the protocol's own
         * answers, so that neither is refused merely because a body filled the first. */
        .max_buffered_output = h2_engine_bound(sv),
        .header_buffer = { .ptr = sv->h2buf, .size = sv->h2buf_size },
        .set_header_table = true, .header_table_bytes = (proven_u32)sv->cfg.http2_header_table_bytes,
    };
    if (proven_h2_conn_create(&cfg, &d->engine) != PROVEN_OK) { sv->alloc.free_fn(sv->alloc.ctx, d); return PROVEN_ERR_NOMEM; }
    if (sv->streams_now < cfg.max_concurrent_streams) proven_h2_conn_open_with_streams_(d->engine, sv->streams_now);
    /* No window configured: it starts at the protocol's and grows while bodies are read, to HTTP/3's caps (RFC-0019 section 10). */
    if (sv->cfg.http2_window_bytes == 0) proven_h2_conn_grow_(d->engine, 16384, 24576, h2_now_us);
    if (sv->window_now) proven_h2_conn_cap_window_(d->engine, sv->window_now);   /* proven_http_event_server_set_window */
    proven_h2_conn_heads_past_bound_(d->engine);
    d->ops = &h2_ops;
    d->conn = conn;
    d->server = sv;
    conn->h.state = EV_RAW;
    conn->raw = d;
    conn->paused = false;
    d_enter(d);
    if (conn->tls && !h2_tls_adequate(conn->tls)) proven_h2_conn_shutdown(d->engine, PROVEN_H2_INADEQUATE_SECURITY);
    d_pump(d);                                     /* the server's SETTINGS */
    d_leave(d);
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// What a handler calls, for a stream of an HTTP/2 connection
// -----------------------------------------------------------------------------

static proven_h2s_t *h2s_of(proven_http_stream_t *stream) { return (proven_h2s_t *)(void *)stream; }
static const proven_h2s_t *h2s_of_const(const proven_http_stream_t *stream) { return (const proven_h2s_t *)(const void *)stream; }

void proven_http_event_h2_compress_(proven_http_stream_t *stream) {
    proven_h2s_t *s = h2s_of(stream);
    if (!s->dead && s->active && !s->response_begun) s->compress_asked = true;
}

static bool h2_is_driver_header(proven_u8str_view_t name) {
    return proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("content-length")) || proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("transfer-encoding")) ||
           proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("connection")) || proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("date"));
}

/* Fields that describe an HTTP/1.1 connection. A handler written for HTTP/1.1 may set them;
 * on HTTP/2 they are not sent, rather than the response being refused for them. */
static bool h2_is_hop_header(proven_u8str_view_t name) {
    return proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("keep-alive")) || proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("proxy-connection")) || proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("upgrade"));
}

/* The response head, to the engine. `length`: the body's size on the wire, or LENGTH_UNKNOWN. */
static proven_err_t s_head(proven_h2s_t *s, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count,
                           proven_u64 length, bool vary, proven_http_coding_t coding, bool end_stream) {
    proven_http_event_server_t *sv = s->h.server;
    h2d_t *d = s->d;
    if (status < 200 || status > 999 || (header_count > 0 && !headers)) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t i = 0; i < header_count; ++i) if (h2_is_driver_header(headers[i].name)) return PROVEN_ERR_INVALID_ARG;
    /* Names go out in lowercase, as HTTP/2 requires of them: written into the server's head
     * buffer, which is where an HTTP/1.1 head would have been built. */
    proven_http_header_t small[40], *list = small;
    if (header_count + 6 > sizeof small / sizeof small[0]) {
        list = h2_alloc(sv->alloc, (header_count + 6) * sizeof *list);
        if (!list) return PROVEN_ERR_NOMEM;
    }
    proven_byte_t code[3] = { (proven_byte_t)('0' + status / 100), (proven_byte_t)('0' + status / 10 % 10), (proven_byte_t)('0' + status % 10) };
    proven_byte_t date[PROVEN_HTTP_DATE_SIZE], digits[20];
    proven_size_t n = 0, at = 0, nd = 0;
    proven_err_t e = PROVEN_OK;
    list[n++] = (proven_http_header_t){ .name = PROVEN_LIT(":status"), .value = { .ptr = code, .size = 3 } };
    if (proven_http_date_format(proven_time_now(), date) == PROVEN_OK) list[n++] = (proven_http_header_t){ .name = PROVEN_LIT("date"), .value = { .ptr = date, .size = sizeof date } };
    for (proven_size_t i = 0; i < header_count && e == PROVEN_OK; ++i) {
        if (h2_is_hop_header(headers[i].name)) continue;
        if (at + headers[i].name.size > sv->cfg.max_head_bytes) { e = PROVEN_ERR_OUT_OF_BOUNDS; break; }
        for (proven_size_t k = 0; k < headers[i].name.size; ++k) sv->hbuf[at + k] = proven_ascii_lower(headers[i].name.ptr[k]);
        list[n++] = (proven_http_header_t){ .name = { .ptr = sv->hbuf + at, .size = headers[i].name.size }, .value = headers[i].value };
        at += headers[i].name.size;
    }
    const bool no_body_status = status == 204 || status == 304;
    if (e == PROVEN_OK) {
        if (vary) list[n++] = (proven_http_header_t){ .name = PROVEN_LIT("vary"), .value = PROVEN_LIT("accept-encoding") };
        if (coding != PROVEN_HTTP_CODING_IDENTITY) list[n++] = (proven_http_header_t){ .name = PROVEN_LIT("content-encoding"), .value = proven_http_coding_token_(coding) };
#if !defined(PROVEN_NO_HTTP3)
        proven_byte_t alt[16];
        const proven_size_t alt_n = sv->h3 && !d->dead ? proven_http_event_alt_svc_(sv, d->conn->local_port, headers, header_count, alt) : 0;
        if (alt_n > 0) list[n++] = (proven_http_header_t){ .name = PROVEN_LIT("alt-svc"), .value = { .ptr = alt, .size = alt_n } };
#endif
        if (!no_body_status && length != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) {
            proven_u64 v = length;
            do { digits[sizeof digits - 1 - nd++] = (proven_byte_t)('0' + v % 10); v /= 10; } while (v > 0);
            list[n++] = (proven_http_header_t){ .name = PROVEN_LIT("content-length"), .value = { .ptr = digits + sizeof digits - nd, .size = nd } };
        }
        s->bodiless = no_body_status || s->method == PROVEN_HTTP_HEAD;
        e = proven_h2_conn_submit_response(d->engine, s->id, list, n, end_stream || s->bodiless);
    }
    if (list != small) sv->alloc.free_fn(sv->alloc.ctx, list);
    if (e == PROVEN_ERR_INVALID_ARG || e == PROVEN_ERR_OUT_OF_BOUNDS) { s->bodiless = false; return e; }      /* the handler's list: it may try another */
    if (e != PROVEN_OK) {
        /* No room, no memory, or the stream has gone: there is no response to go on with. */
        s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_RESET);
        return PROVEN_ERR_RESET;
    }
    s->response_begun = true;
    if (end_stream || s->bodiless) s->engine_done = true;
    return PROVEN_OK;
}

static void s_compress_plan(const proven_h2s_t *s, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count, bool *vary,
                            proven_http_coding_t *coding) {
    *vary = false;
    *coding = PROVEN_HTTP_CODING_IDENTITY;
    if (!s->compress_asked || (header_count > 0 && !headers)) return;
    *vary = proven_http_compress_needs_vary_(headers, header_count);
    if (proven_http_compress_applies_(s->compress_coding != PROVEN_HTTP_CODING_IDENTITY, status, headers, header_count)) *coding = (proven_http_coding_t)s->compress_coding;
}

proven_err_t proven_http_event_h2_begin_(proven_http_stream_t *stream, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count,
                                         proven_u64 content_length) {
    proven_h2s_t *s = h2s_of(stream);
    if (s->dead || !s->active || s->response_begun) return PROVEN_ERR_INVALID_STATE;
    proven_http_event_server_t *sv = s->h.server;
    h2d_t *d = s->d;
    bool vary = false;
    proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;
    s_compress_plan(s, status, headers, header_count, &vary, &coding);
    proven_http_compressor_t_ *zip = (void *)0;
    if (coding != PROVEN_HTTP_CODING_IDENTITY && s->method != PROVEN_HTTP_HEAD) {
        if (!sv->zbuf) sv->zbuf = h2_alloc(sv->alloc, PROVEN_HTTP_CODING_PIECE);
        const proven_http_compress_cfg_t_ cc = proven_http_event_compress_cfg_(&sv->cfg);
        if (!sv->zbuf || proven_http_compressor_create_(sv->alloc, coding, &cc, &zip) != PROVEN_OK) coding = PROVEN_HTTP_CODING_IDENTITY;
    }
    s_enter(s);
    d_enter(d);
    d->calls++;
    proven_err_t e = s_head(s, status, headers, header_count, coding != PROVEN_HTTP_CODING_IDENTITY ? PROVEN_HTTP_EVENT_LENGTH_UNKNOWN : content_length,
                            vary, coding, false);
    if (e != PROVEN_OK || s->dead) {
        proven_http_compressor_destroy_(zip);
    } else {
        s->zip = zip;
        s->response_left = s->bodiless ? PROVEN_HTTP_EVENT_LENGTH_UNKNOWN : content_length;
    }
    d_pump(d);
    /* The stream may have ended in there (on_done has then been called): the caller must not
     * go on to write to it. */
    if (e == PROVEN_OK && s->dead) e = PROVEN_ERR_RESET;
    d->calls--;
    d_leave(d);
    s_leave(s);
    return e;
}

/* Body bytes through the compressor into the stream's own queue. False when there is no memory. */
static bool s_zip(proven_h2s_t *s, const proven_byte_t *p, proven_size_t n, proven_deflate_flush_t flush) {
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
        if (!h2_append(sv->alloc, &s->out, &s->out_len, &s->out_cap, sv->zbuf, made)) return false;
        if (flush == PROVEN_DEFLATE_FLUSH_FINISH ? done : (n == 0 && made < PROVEN_HTTP_CODING_PIECE)) return true;
    }
}

proven_result_size_t proven_http_event_h2_write_(proven_http_stream_t *stream, proven_mem_view_t data) {
    proven_h2s_t *s = h2s_of(stream);
    if (s->dead || !s->active || !s->response_begun || s->response_ended) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_STATE, .value = 0 };
    if (s->bodiless) return (proven_result_size_t){ .err = PROVEN_OK, .value = data.size };     /* counted, not sent */
    if (s->response_left != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN && data.size > s->response_left) return (proven_result_size_t){ .err = PROVEN_ERR_OUT_OF_BOUNDS, .value = 0 };
    h2d_t *d = s->d;
    s_enter(s);
    d_enter(d);
    d->calls++;
    proven_size_t done = 0;
    bool alive = true;
    for (int tries = 0; alive && done < data.size && tries < 2;) {
        /* What this stream made earlier goes first; while any of that waits, nothing new is taken. */
        if (s_out_pending(s) > 0) { s_push(s); if (s->dead) { alive = false; break; } }
        proven_size_t room = s_out_pending(s) > 0 ? 0 : d_room(d);
        if (room == 0) {
            /* A refusal must mean that the client is not taking more: look once more after
             * offering what is queued to the transport. */
            tries++;
            d_pump(d);
            if (s->dead) alive = false;
            continue;
        }
        proven_size_t n = data.size - done < room ? data.size - done : room;
        if (s->zip) {
            if (n > PROVEN_HTTP_CODING_PIECE) n = PROVEN_HTTP_CODING_PIECE;
            if (!s_zip(s, data.ptr + done, n, PROVEN_DEFLATE_FLUSH_NONE)) { s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_NOMEM); alive = false; break; }
        } else {
            proven_result_size_t r = proven_h2_conn_send_data(d->engine, s->id, (proven_mem_view_t){ .ptr = data.ptr + done, .size = n }, false);
            if (r.err != PROVEN_OK) { s_finish(s, PROVEN_ERR_RESET); alive = false; break; }
            s->window_shut = r.value < n;                  /* the peer's window: on_writable then waits for the engine's word */
            n = r.value;
            if (s->window_shut) { if (s->response_left != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) s->response_left -= n; done += n; break; }
        }
        if (s->response_left != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN) s->response_left -= n;
        done += n;
    }
    if (alive && !s->dead) {
        if (s_out_pending(s) > 0) s_push(s);
        if (!s->dead && done < data.size) s->want_writable = true;
    }
    d_pump(d);
    alive = alive && !s->dead;                     /* after the pump, which may end the stream */
    d->calls--;
    d_leave(d);
    s_leave(s);
    return alive ? (proven_result_size_t){ .err = PROVEN_OK, .value = done } : (proven_result_size_t){ .err = PROVEN_ERR_RESET, .value = 0 };
}

proven_err_t proven_http_event_h2_flush_(proven_http_stream_t *stream) {
    proven_h2s_t *s = h2s_of(stream);
    if (s->dead || !s->active || !s->response_begun || s->response_ended) return PROVEN_ERR_INVALID_STATE;
    if (!s->zip) return PROVEN_OK;
    h2d_t *d = s->d;
    s_enter(s);
    d_enter(d);
    d->calls++;
    if (!s_zip(s, (void *)0, 0, PROVEN_DEFLATE_FLUSH_SYNC)) s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_NOMEM);
    d_pump(d);
    const bool alive = !s->dead;
    d->calls--;
    d_leave(d);
    s_leave(s);
    return alive ? PROVEN_OK : PROVEN_ERR_RESET;
}

proven_err_t proven_http_event_h2_trailers_(const proven_http_stream_t *stream, proven_http_header_t *fields, proven_size_t cap, proven_size_t *count) {
    const proven_h2s_t *s = (const proven_h2s_t *)(const void *)stream;
    if (!s->in_done) return PROVEN_ERR_INVALID_STATE;
    return proven_http_trailers_give_(&s->in_trailers, fields, cap, count);
}

proven_err_t proven_http_event_h2_end_(proven_http_stream_t *stream, const proven_http_header_t *trailers, proven_size_t count) {
    proven_h2s_t *s = h2s_of(stream);
    if (s->dead || !s->active || !s->response_begun || s->response_ended) return PROVEN_ERR_INVALID_STATE;
    if (count > 0) {
        /* As over HTTP/1.1: after a body begun with no length, and not to HEAD. */
        if (s->response_left != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN || s->bodiless || s->engine_done) return PROVEN_ERR_INVALID_STATE;
        if (!proven_http_trailers_valid_(trailers, count)) return PROVEN_ERR_INVALID_ARG;
        const proven_err_t e = proven_http_trailers_keep_(s->h.server->alloc, &s->out_trailers, trailers, count, false, true);
        if (e != PROVEN_OK) return e;
    }
    h2d_t *d = s->d;
    proven_err_t result = PROVEN_OK;
    s_enter(s);
    d_enter(d);
    d->calls++;
    if (s->response_left != PROVEN_HTTP_EVENT_LENGTH_UNKNOWN && s->response_left != 0) {
        /* Less was written than promised. The client must not take what it has for the whole. */
        s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_INVALID_FORMAT);
        result = PROVEN_ERR_INVALID_FORMAT;
    } else {
        s->response_ended = true;
        if (s->zip) {
            if (!s_zip(s, (void *)0, 0, PROVEN_DEFLATE_FLUSH_FINISH)) { s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_NOMEM); result = PROVEN_ERR_RESET; }
            else { proven_http_compressor_destroy_(s->zip); s->zip = (void *)0; }
        }
        if (!s->dead) {
            s->end_asked = true;
            s_push(s);                             /* the end goes with, or after, what is queued */
        }
    }
    d_pump(d);
    d->calls--;
    d_leave(d);
    s_leave(s);
    return result;
}

proven_err_t proven_http_event_h2_respond_(proven_http_stream_t *stream, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count,
                                           proven_mem_view_t body) {
    proven_h2s_t *s = h2s_of(stream);
    if (s->dead || !s->active || s->response_begun) return PROVEN_ERR_INVALID_STATE;
    proven_http_event_server_t *sv = s->h.server;
    h2d_t *d = s->d;
    bool vary = false;
    proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;
    s_compress_plan(s, status, headers, header_count, &vary, &coding);
    proven_byte_t *packed = (void *)0;
    proven_size_t packed_cap = 0, packed_size = 0;
    const proven_http_compress_cfg_t_ cc = proven_http_event_compress_cfg_(&sv->cfg);
    if (coding != PROVEN_HTTP_CODING_IDENTITY && proven_http_compress_all_(sv->alloc, coding, &cc, body, &packed, &packed_cap, &packed_size) != PROVEN_OK) packed = (void *)0;
    if (packed) body = (proven_mem_view_t){ .ptr = packed, .size = packed_size };
    s_enter(s);
    d_enter(d);
    d->calls++;
    proven_err_t e = s_head(s, status, headers, header_count, body.size, vary, packed ? coding : PROVEN_HTTP_CODING_IDENTITY, body.size == 0);
    if (e == PROVEN_OK && !s->dead) {
        s->response_ended = true;
        s->end_asked = true;
        /* The whole response is this call's contract, whatever the output bound: what the
         * engine does not take now is kept, and follows as the peer's window opens. */
        if (!s->bodiless && body.size > 0) {
            /* Offered only as far as the bound on body bytes: the engine's own bound is twice
             * that, and the other half is for heads - another stream's response must be able
             * to begin while this one's body waits. */
            const proven_size_t room = d_room(d);
            const proven_mem_view_t now = { .ptr = body.ptr, .size = body.size < room ? body.size : room };
            proven_result_size_t r = now.size == body.size ? proven_h2_conn_send_data(d->engine, s->id, body, true)
                                   : now.size > 0 ? proven_h2_conn_send_data(d->engine, s->id, now, false)
                                   : (proven_result_size_t){ .err = PROVEN_OK, .value = 0 };
            if (r.err != PROVEN_OK) { s_finish(s, PROVEN_ERR_RESET); e = PROVEN_ERR_RESET; }
            else if (r.value == body.size) s->engine_done = true;
            else if (!h2_append(sv->alloc, &s->out, &s->out_len, &s->out_cap, body.ptr + r.value, body.size - r.value)) { s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_NOMEM); e = PROVEN_ERR_NOMEM; }
        }
        if (!s->dead) s_push(s);
    }
    if (packed) sv->alloc.free_fn(sv->alloc.ctx, packed);
    d_pump(d);
    d->calls--;
    d_leave(d);
    s_leave(s);
    return e;
}

void proven_http_event_h2_abort_(proven_http_stream_t *stream) {
    proven_h2s_t *s = h2s_of(stream);
    if (s->dead) return;
    h2d_t *d = s->d;
    s_enter(s);
    d_enter(d);
    d->calls++;
    s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_RESET);
    d_pump(d);
    d->calls--;
    d_leave(d);
    s_leave(s);
}

proven_u32 proven_http_event_h2_test_window_(proven_http_stream_t *stream) {
    proven_h2s_t *s = h2s_of(stream);
    return s->dead || !s->d->engine ? 0 : proven_h2_conn_test_window_(s->d->engine, PROVEN_H2_TEST_CAP);
}

void proven_http_event_h2_pause_(proven_http_stream_t *stream) {
    proven_h2s_t *s = h2s_of(stream);
    if (s->dead || !s->active) return;
    s->paused = true;
    s_expect_body(s);
    d_arm(s->d);
}

void proven_http_event_h2_resume_(proven_http_stream_t *stream) {
    proven_h2s_t *s = h2s_of(stream);
    if (s->dead || !s->paused) return;
    proven_http_event_server_t *sv = s->h.server;
    h2d_t *d = s->d;
    s->paused = false;
    s_expect_body(s);
    s_enter(s);
    d_enter(d);
    d->calls++;
    if (s->held_len > 0 || s->held_last) {
        /* What arrived meanwhile, as one piece - in a buffer that is no longer the stream's,
         * so that a handler that pauses again inside the call starts a new one. */
        proven_byte_t *held = s->held;
        const proven_size_t n = s->held_len;
        const bool last = s->held_last;
        const proven_u32 id = s->id;
        s->held = (void *)0; s->held_len = s->held_cap = 0; s->held_last = false;
        if (sv->cfg.on.on_body) sv->cfg.on.on_body(sv->cfg.ctx, stream, (proven_mem_view_t){ .ptr = held, .size = n }, last);
        if (held) sv->alloc.free_fn(sv->alloc.ctx, held);
        if (!d->dead) proven_h2_conn_consumed(d->engine, id, n);
    }
    d_pump(d);
    d->calls--;
    d_leave(d);
    s_leave(s);
}

void proven_http_event_h2_set_user_(proven_http_stream_t *stream, void *user) { h2s_of(stream)->user = user; }
void *proven_http_event_h2_user_(const proven_http_stream_t *stream) { return h2s_of_const(stream)->user; }
proven_net_addr_t proven_http_event_h2_peer_(const proven_http_stream_t *stream) {
    const proven_h2s_t *s = h2s_of_const(stream);
    return s->dead || s->d->dead ? (proven_net_addr_t){ 0 } : s->d->conn->peer;      /* the connection's: a stream has none of its own */
}

proven_size_t proven_http_event_h2_buffered_(const proven_http_stream_t *stream) {
    const proven_h2s_t *s = h2s_of_const(stream);
    if (s->dead) return 0;
    return s_out_pending(s) + d_queued(s->d);
}

// -----------------------------------------------------------------------------
// A WebSocket on a stream (RFC 8441): what ws_event.c's transport asks of one
// -----------------------------------------------------------------------------

/* What arrived for the WebSocket, offered to it - all of it, unless it pauses. Input goes into
 * `held` first: the WebSocket unmasks in place, and an event's data is the engine's. What it
 * took is reported as consumed, so the peer's window opens by that much and no more. */
static void s_raw_feed(proven_h2s_t *s) {
    h2d_t *d = s->d;
    s_enter(s);
    while (s->raw && !s->dead && !s->paused && s->held_off < s->held_len) {
        void *raw = s->raw;
        const proven_size_t have = s->held_len - s->held_off;
        proven_size_t took = (*(const proven_http_event_raw_ops_t *const *)raw)->on_input(raw, (proven_mem_mut_t){ .ptr = s->held + s->held_off, .size = have });
        if (s->dead) break;
        if (took > have) took = have;
        s->held_off += took;
        if (!d->dead) proven_h2_conn_consumed(d->engine, s->id, took);
        if (took == 0) break;                      /* it waits for the rest of a frame */
    }
    /* What was taken goes from the front - not after every frame, which would move the rest
     * once a frame: when all of it is taken, or half the buffer is behind it. */
    if (!s->dead && s->held_off > 0 && (s->held_off == s->held_len || s->held_off > s->held_cap / 2)) {
        for (proven_size_t i = s->held_off; i < s->held_len; ++i) s->held[i - s->held_off] = s->held[i];
        s->held_len -= s->held_off;
        s->held_off = 0;
    }
    /* The peer ended its side and everything it sent has been taken: for the WebSocket that is
     * the end of its input, as a closed socket is on HTTP/1.1. This side ends too. */
    if (s->raw && !s->dead && s->in_done && s->held_off == s->held_len) {
        void *raw = s->raw;
        s->raw = (void *)0;
        proven_loop_timer_cancel(s->h.server->loop, &s->timer);
        (*(const proven_http_event_raw_ops_t *const *)raw)->on_closed(raw, PROVEN_ERR_EOF);
        if (!s->dead) { s->end_asked = true; s_push(s); }
    }
    s_leave(s);
}

static void s_raw_in(proven_h2s_t *s, proven_mem_view_t piece, bool last) {
    if (!h2_append(s->h.server->alloc, &s->held, &s->held_len, &s->held_cap, piece.ptr, piece.size)) { s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_NOMEM); return; }
    if (last) s->in_done = true;
    s_raw_feed(s);
}

proven_err_t proven_http_event_h2_raw_begin_(proven_http_stream_t *stream, void *raw, const proven_http_header_t *headers, proven_size_t count) {
    proven_h2s_t *s = h2s_of(stream);
    if (!raw || count > 6 || (count > 0 && !headers)) return PROVEN_ERR_INVALID_ARG;
    if (s->dead || s->d->dead || !s->active || !s->tunnel || s->response_begun) return PROVEN_ERR_INVALID_STATE;
    h2d_t *d = s->d;
    proven_http_event_server_t *sv = s->h.server;
    proven_http_header_t list[7];
    list[0] = (proven_http_header_t){ .name = PROVEN_LIT(":status"), .value = PROVEN_LIT("200") };
    for (proven_size_t i = 0; i < count; ++i) list[i + 1] = headers[i];
    s_enter(s);
    d_enter(d);
    d->calls++;
    /* 200 and no END_STREAM: the stream goes on, as the WebSocket's (RFC 8441, section 5). */
    proven_err_t e = proven_h2_conn_submit_response(d->engine, s->id, list, count + 1, false);
    if (e == PROVEN_OK) {
        s->response_begun = true;
        s->active = false;                         /* the HTTP exchange is over */
        if (sv->cfg.on.on_done) sv->cfg.on.on_done(sv->cfg.ctx, stream, PROVEN_OK);
        d_pump(d);
        if (s->dead || d->dead) e = PROVEN_ERR_RESET;
        else {
            s->user = (void *)0;
            /* Input that came before this is fed from the loop, once the caller has the object.
             * Asked before the owner is set: asking can end the connection, and then the caller
             * learns it from the return, not from the owner's on_closed as well. */
            if (s->held_len > 0 || s->in_done) proven_http_event_raw_again_(d->conn);
            if (s->dead || d->dead) e = PROVEN_ERR_RESET;
            else s->raw = raw;
        }
    } else {
        e = e == PROVEN_ERR_NOMEM ? PROVEN_ERR_NOMEM : PROVEN_ERR_RESET;
    }
    d->calls--;
    d_leave(d);
    s_leave(s);
    return e;
}

bool proven_http_event_h2_raw_send_(proven_http_stream_t *stream, proven_mem_view_t a, proven_mem_view_t b) {
    proven_h2s_t *s = h2s_of(stream);
    if (s->dead || s->d->dead) return false;
    h2d_t *d = s->d;
    proven_allocator_t al = s->h.server->alloc;
    s_enter(s);
    d_enter(d);
    d->calls++;
    /* Both parts are queued together, so that a frame's header and payload stay in order. */
    const bool ok = h2_append(al, &s->out, &s->out_len, &s->out_cap, a.ptr, a.size) && h2_append(al, &s->out, &s->out_len, &s->out_cap, b.ptr, b.size);
    if (!ok) s_reset(s, PROVEN_H2_INTERNAL_ERROR, PROVEN_ERR_NOMEM);
    else s_push(s);
    d_pump(d);
    const bool alive = ok && !s->dead;
    d->calls--;
    d_leave(d);
    s_leave(s);
    return alive;
}

static void s_raw_timer(void *ctx) {
    proven_h2s_t *s = ctx;
    if (s->dead || !s->raw) return;
    h2d_t *d = s->d;
    s_enter(s);
    d_enter(d);
    (*(const proven_http_event_raw_ops_t *const *)s->raw)->on_timer(s->raw);
    d_pump(d);
    d_leave(d);
    s_leave(s);
}

void proven_http_event_h2_raw_timer_(proven_http_stream_t *stream, proven_u32 ms) {
    proven_h2s_t *s = h2s_of(stream);
    if (s->dead) return;
    if (ms == 0) proven_loop_timer_cancel(s->h.server->loop, &s->timer);
    else proven_loop_timer_set(s->h.server->loop, &s->timer, ms, s_raw_timer, s);
}

void proven_http_event_h2_raw_pause_(proven_http_stream_t *stream, bool paused) {
    proven_h2s_t *s = h2s_of(stream);
    if (s->dead || s->paused == paused) return;
    s->paused = paused;
    if (paused || s->held_off == s->held_len) return;
    /* As on HTTP/1.1: what waited is given at once. */
    h2d_t *d = s->d;
    s_enter(s);
    d_enter(d);
    s_raw_feed(s);
    d_pump(d);
    d_leave(d);
    s_leave(s);
}

/* The WebSocket lets go: politely - its output, then END_STREAM - or at once, with a reset.
 * Nothing is called on it after either. */
void proven_http_event_h2_raw_close_(proven_http_stream_t *stream) {
    proven_h2s_t *s = h2s_of(stream);
    if (s->dead) return;
    s->raw = (void *)0;
    proven_loop_timer_cancel(s->h.server->loop, &s->timer);
    h2d_t *d = s->d;
    s_enter(s);
    d_enter(d);
    d->calls++;
    s->end_asked = true;
    s_push(s);
    d_pump(d);
    d->calls--;
    d_leave(d);
    s_leave(s);
}

void proven_http_event_h2_raw_kill_(proven_http_stream_t *stream) {
    proven_h2s_t *s = h2s_of(stream);
    if (s->dead) return;
    s->raw = (void *)0;
    proven_loop_timer_cancel(s->h.server->loop, &s->timer);
    h2d_t *d = s->d;
    s_enter(s);
    d_enter(d);
    d->calls++;
    s_reset(s, PROVEN_H2_CANCEL, PROVEN_OK);
    d_pump(d);
    d->calls--;
    d_leave(d);
    s_leave(s);
}

void proven_http_event_h2_set_window_(proven_http_event_server_t *sv, proven_u32 n) {
    for (proven_http_stream_t *conn = sv->conns, *next; conn; conn = next) {
        next = conn->next;
        if (conn->h.state != EV_RAW || !conn->raw || *(const proven_http_event_raw_ops_t *const *)conn->raw != &h2_ops) continue;
        h2d_t *d = conn->raw;
        if (d->dead) continue;
        d_enter(d);
        proven_h2_conn_cap_window_(d->engine, n);
        d_pump(d);
        d_leave(d);
    }
}

void proven_http_event_h2_set_max_streams_(proven_http_event_server_t *sv, proven_u32 n) {
    for (proven_http_stream_t *conn = sv->conns, *next; conn; conn = next) {
        next = conn->next;                         /* a connection that ends while it is told leaves the list */
        if (conn->h.state != EV_RAW || !conn->raw || *(const proven_http_event_raw_ops_t *const *)conn->raw != &h2_ops) continue;
        h2d_t *d = conn->raw;
        if (d->dead) continue;
        d_enter(d);
        /* After a goodbye there is nothing to change; and a connection that cannot say it keeps
         * the number it has. */
        (void)proven_h2_conn_set_max_streams(d->engine, n);
        d_pump(d);
        d_leave(d);
    }
}

bool proven_http_event_h2_early_(const proven_http_stream_t *stream) {
    const proven_h2s_t *s = (const proven_h2s_t *)(const void *)stream;
    return s && s->early;
}

#endif
