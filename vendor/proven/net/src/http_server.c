#include "proven/http_server.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven/time.h"
#include "proven_internal_tls.h"
#include "proven_internal_http_coding.h"
#include "../platform/proven_sys_net_mem.h"
#include <stdatomic.h>

/*
 * The HTTP/1.1 server driver.
 *
 * One thread runs the loop. It owns every connection that is waiting for a request: it
 * accepts, reads what has arrived without blocking, and parses. When a request head is
 * complete the connection changes hands - to the handler, which reads the body and writes the
 * response with ordinary deadline-bounded calls - and the loop does not touch it again until
 * the handler has returned. With no job system the handler runs right there on the loop
 * thread; with one, it runs on a worker and the connection comes back through a lock-free
 * stack and a waker.
 *
 * So there is exactly one owner of a connection at any moment, and the only state two threads
 * share is that stack, a stop flag and a count of handlers in flight.
 */

#define SV_MAX_LISTENERS 4
#define SV_BODY_WINDOW 4096u          /* room after the head for reading a request body */
#define SV_DRAIN_LIMIT 65536u         /* an unread request body larger than this closes the connection */
#define SV_LINGER_MS 1000u            /* how long a connection being closed is still read from, unless the configuration says */
#define SV_WHEEL_SLOTS 1024u          /* timer wheel: this many slots ... */
#define SV_TICK_NS 16000000           /* ... of 16 ms each, so one turn is about 16 s */
#define SV_EVENTS 64u                 /* ready sockets taken from the selector per round */

typedef struct sv_conn proven_sv_conn_t;

struct sv_conn {
    proven_http_server_t *server;
    proven_net_conn_t sock;
    proven_transport_t t;
    proven_net_addr_t peer;
    proven_http_header_t *headers;
    proven_byte_t *buf;                /* the request head, then a window for body bytes */
    proven_size_t cap;
    proven_size_t len;
    proven_byte_t *out;                /* the response head being built */
    proven_size_t out_cap;
    proven_net_deadline_t deadline;    /* while waiting: when to give up on this connection */
    proven_sv_conn_t *next_done;
    proven_sv_conn_t *prev, *next;            /* every connection of the server */
    proven_sv_conn_t *timer_prev, *timer_next;/* the others in the same slot of the timer wheel */
    proven_u64 timer_tick;             /* the tick at which the timer is due */
    proven_sv_conn_t *next_dead;
    bool handling;
    bool lingering;                    /* the response is sent; input is read and thrown away until the client closes */
    bool watched;                      /* registered with the selector */
    bool timed;                        /* on the timer wheel */
    bool dead;                         /* closed; freed at the end of the round */
    bool tls;                          /* `t` is a TLS transport over `sock` */
    bool early;                        /* the request being served arrived as early data (RFC-0027) */

    /* One request. */
    proven_http_request_t req;
    proven_size_t head_size;
    proven_size_t body_pos;
    proven_http_body_t body;
    proven_err_t body_error;
    proven_u64 out_remaining;
    bool body_done;
    bool expect_continue;
    bool head_only;                    /* HEAD, or a status that has no body: body bytes are not sent */
    bool response_begun;
    bool response_ended;
    bool chunked_out;
    bool length_known;
    bool close_after;
    bool io_failed;                    /* a write failed: nothing more can be sent */
    bool detached;                     /* the socket was handed to the handler by an upgrade */
    bool keep;
    bool compress_asked;               /* the handler asked for this response to be compressed */
    proven_http_compressor_t_ *zip;    /* it is: the body goes through this, and `out` holds what comes out */
    proven_byte_t *trailers;           /* a chunked request's trailer section, kept (allocated for the first such request, kept with the connection) */
};

struct proven_http_exchange { proven_sv_conn_t conn; };

/* The server's upgraded connections that are open: shared by the server and each of them, so that a
 * connection closed after the server is gone still has somewhere to say so. The last of them frees it. */
typedef struct {
    proven_allocator_t alloc;
    atomic_size_t open;
    atomic_size_t refs;
} proven_sv_upcount_t;

static void sv_up_release(proven_sv_upcount_t *u) {
    if (atomic_fetch_sub_explicit(&u->refs, 1, memory_order_acq_rel) == 1) u->alloc.free_fn(u->alloc.ctx, u);
}

struct proven_http_server {
    proven_http_server_config_t cfg;
    proven_sv_upcount_t *up;
    proven_net_listener_t listeners[SV_MAX_LISTENERS];
    proven_size_t listener_count;
    proven_net_waker_t waker;
    proven_net_selector_t *selector;
    proven_sv_conn_t *conns;                  /* a list: the server never walks it while serving */
    proven_size_t conn_count;
    proven_sv_conn_t *dead;                   /* closed in this round; freed when its events are done with */
    proven_sv_conn_t *wheel[SV_WHEEL_SLOTS];
    proven_u64 wheel_tick;             /* the last tick the wheel has been advanced to */
    proven_size_t timed_count;
    bool listening;                    /* the listeners are registered with the selector */
    _Atomic(proven_sv_conn_t *) done;
    atomic_bool stop;
    atomic_size_t in_flight;
    /* proven_http_server_shutdown: asked from any thread; carried out by the loop. */
    atomic_bool shutdown_asked;
    atomic_uint shutdown_grace_ms;
    bool draining;                     /* the loop has begun it: no listeners, idle connections closed */
    bool drain_over;                   /* everything has gone, or the deadline has passed */
    bool drain_cut;                    /* the deadline ended something, or left a handler running */
    proven_time_t drain_deadline;
};

static void *sv_alloc(proven_allocator_t a, proven_size_t size) {
    proven_result_mem_mut_t m = a.alloc_fn(a.ctx, size ? size : 1, 16);
    return proven_is_ok(m.err) ? m.value.ptr : (void *)0;
}

static void sv_free(proven_allocator_t a, void *p) {
    if (p) a.free_fn(a.ctx, p);
}

static void sv_unwatch(proven_http_server_t *s, proven_sv_conn_t *c);

// -----------------------------------------------------------------------------
// Responses
// -----------------------------------------------------------------------------

static proven_net_deadline_t sv_write_deadline(const proven_sv_conn_t *c) {
    return proven_net_deadline_in(c->server->cfg.write_timeout_ms);
}

/* Write to the client; a failure means the connection is of no further use. */
static proven_err_t sv_send(proven_sv_conn_t *c, proven_mem_view_t data) {
    proven_result_size_t w = proven_transport_write_all(c->t, data, sv_write_deadline(c));
    if (w.err != PROVEN_OK) { c->io_failed = true; c->close_after = true; }
    return w.err;
}

static bool sv_is_driver_header(proven_u8str_view_t name) {
    return proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("Content-Length")) || proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("Transfer-Encoding")) ||
           proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("Connection")) || proven_u8str_view_eq_ascii_ci(name, PROVEN_LIT("Date"));
}

/* Build the response head in c->out. Nothing is sent, and no state changes, unless it returns
 * PROVEN_OK - so a refused header leaves the handler free to try another response. */
static proven_err_t sv_build_head(proven_sv_conn_t *c, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count,
                                  proven_u64 content_length, bool vary, proven_http_coding_t coding,
                                  proven_size_t *head_len, bool *chunked, bool *will_close, bool *bodiless) {
    if (status < 200 || status > 999) return PROVEN_ERR_INVALID_ARG;
    if (header_count > 0 && !headers) return PROVEN_ERR_INVALID_ARG;
    proven_mem_mut_t out = { .ptr = c->out, .size = c->out_cap };
    proven_size_t len = 0;

    proven_err_t e = proven_http_write_status_line(out, &len, status, PROVEN_LIT(""));
    proven_byte_t date[PROVEN_HTTP_DATE_SIZE];
    if (e == PROVEN_OK && proven_http_date_format(proven_time_now(), date) == PROVEN_OK) {
        e = proven_http_write_header(out, &len, PROVEN_LIT("Date"), (proven_u8str_view_t){ .ptr = date, .size = sizeof date });
    }
    for (proven_size_t i = 0; i < header_count && e == PROVEN_OK; ++i) {
        if (sv_is_driver_header(headers[i].name)) return PROVEN_ERR_INVALID_ARG;
        e = proven_http_write_header(out, &len, headers[i].name, headers[i].value);
    }
    if (e == PROVEN_OK && vary) e = proven_http_write_header(out, &len, PROVEN_LIT("Vary"), PROVEN_LIT("Accept-Encoding"));
    if (e == PROVEN_OK && coding != PROVEN_HTTP_CODING_IDENTITY) e = proven_http_write_header(out, &len, PROVEN_LIT("Content-Encoding"), proven_http_coding_token_(coding));

    /* 204 and 304 never have a body; a response to HEAD has the headers of one and no bytes. */
    bool no_body_status = status == 204 || status == 304;
    bool is_head = c->req.method == PROVEN_HTTP_HEAD;
    bool use_chunked = false;
    /* A response sent while the client is still waiting for its 100 Continue: whether the body
     * follows is the client's choice, so the connection cannot carry another request. */
    bool close = c->close_after || c->expect_continue || !proven_http_request_keep_alive(&c->req) ||
                 atomic_load_explicit(&c->server->stop, memory_order_relaxed) ||
                 atomic_load_explicit(&c->server->shutdown_asked, memory_order_relaxed);
    if (e == PROVEN_OK && !no_body_status) {
        if (content_length != PROVEN_HTTP_LENGTH_UNKNOWN) {
            e = proven_http_write_header_u64(out, &len, PROVEN_LIT("Content-Length"), content_length);
        } else if (c->req.version_minor >= 1) {
            e = proven_http_write_header(out, &len, PROVEN_LIT("Transfer-Encoding"), PROVEN_LIT("chunked"));
            use_chunked = true;
        } else {
            close = true;              /* an HTTP/1.0 client reads until the connection closes */
        }
    }
    if (e == PROVEN_OK) {
        if (close) e = proven_http_write_header(out, &len, PROVEN_LIT("Connection"), PROVEN_LIT("close"));
        else if (c->req.version_minor == 0) e = proven_http_write_header(out, &len, PROVEN_LIT("Connection"), PROVEN_LIT("keep-alive"));
    }
    if (e == PROVEN_OK) e = proven_http_write_head_end(out, &len);
    if (e != PROVEN_OK) return e;
    *head_len = len;
    *chunked = use_chunked && !is_head;
    *will_close = close;
    *bodiless = no_body_status || is_head;
    return PROVEN_OK;
}

static void sv_mark_begun(proven_sv_conn_t *c, proven_u64 content_length, bool chunked, bool will_close, bool bodiless) {
    c->response_begun = true;
    c->chunked_out = chunked;
    c->head_only = bodiless;
    c->length_known = content_length != PROVEN_HTTP_LENGTH_UNKNOWN;
    c->out_remaining = c->length_known ? content_length : 0;
    if (will_close) c->close_after = true;
}

static proven_http_compress_cfg_t_ sv_compress_cfg(const proven_http_server_t *s) {
    return (proven_http_compress_cfg_t_){ s->cfg.compress_level, s->cfg.compress_window_bits, s->cfg.compress_zstd, s->cfg.compress_zstd_level, s->cfg.compress_zstd_window_log };
}

/* What the handler's request for compression comes to for this response: the coding, or
 * identity for none. */
static void sv_compress_plan(const proven_sv_conn_t *c, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count,
                             bool *vary, proven_http_coding_t *coding) {
    *vary = false;
    *coding = PROVEN_HTTP_CODING_IDENTITY;
    if (!c->compress_asked || (header_count > 0 && !headers)) return;
    *vary = proven_http_compress_needs_vary_(headers, header_count);
    const proven_http_coding_t chosen = proven_http_compress_choose_(c->req.headers, c->req.header_count, c->server->cfg.compress_zstd);
    if (proven_http_compress_applies_(chosen != PROVEN_HTTP_CODING_IDENTITY, status, headers, header_count)) *coding = chosen;
}

void proven_http_exchange_compress(proven_http_exchange_t *exchange) {
    if (exchange && !exchange->conn.response_begun) exchange->conn.compress_asked = true;
}

proven_err_t proven_http_exchange_begin(proven_http_exchange_t *exchange, proven_u16 status,
                                        const proven_http_header_t *headers, proven_size_t header_count,
                                        proven_u64 content_length) {
    if (!exchange) return PROVEN_ERR_INVALID_ARG;
    proven_sv_conn_t *c = &exchange->conn;
    if (c->response_begun) return PROVEN_ERR_INVALID_STATE;
    proven_size_t head_len = 0;
    bool chunked = false, will_close = false, bodiless = false, vary = false;
    proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;
    sv_compress_plan(c, status, headers, header_count, &vary, &coding);
    proven_http_compressor_t_ *zip = (void *)0;
    /* Without the memory for a compressor the response goes as it is. (A response to HEAD
     * needs none: it has the headers and no bytes.) */
    if (coding != PROVEN_HTTP_CODING_IDENTITY && c->req.method != PROVEN_HTTP_HEAD) {
        const proven_http_compress_cfg_t_ cc = sv_compress_cfg(c->server);
        if (proven_http_compressor_create_(c->server->cfg.alloc, coding, &cc, &zip) != PROVEN_OK) coding = PROVEN_HTTP_CODING_IDENTITY;
    }
    /* Compressed, the length on the wire is not the one given: the body goes chunked. The
     * length given is still what the handler has promised to write. */
    proven_err_t e = sv_build_head(c, status, headers, header_count, coding != PROVEN_HTTP_CODING_IDENTITY ? PROVEN_HTTP_LENGTH_UNKNOWN : content_length,
                                   vary, coding, &head_len, &chunked, &will_close, &bodiless);
    if (e != PROVEN_OK || bodiless) {
        proven_http_compressor_destroy_(zip);
        zip = (void *)0;
    }
    if (e != PROVEN_OK) return e;
    sv_mark_begun(c, content_length, chunked, will_close, bodiless);
    c->zip = zip;
    return sv_send(c, (proven_mem_view_t){ .ptr = c->out, .size = head_len });
}

/* One piece of the body as the client is to get it: a chunk, or bare bytes. */
static proven_err_t sv_send_piece(proven_sv_conn_t *c, proven_mem_view_t data) {
    if (data.size == 0) return PROVEN_OK;
    if (!c->chunked_out) return sv_send(c, data);
    proven_byte_t frame[24];
    proven_size_t fl = 0;
    proven_err_t e = proven_http_write_chunk_begin((proven_mem_mut_t){ .ptr = frame, .size = sizeof frame }, &fl, data.size);
    if (e != PROVEN_OK) return e;
    e = sv_send(c, (proven_mem_view_t){ .ptr = frame, .size = fl });
    if (e == PROVEN_OK) e = sv_send(c, data);
    if (e == PROVEN_OK) e = sv_send(c, (proven_mem_view_t){ .ptr = (const proven_byte_t *)"\r\n", .size = 2 });
    return e;
}

/* Body bytes through the compressor. The head has left, so its buffer takes what comes out. */
static proven_err_t sv_zip(proven_sv_conn_t *c, proven_mem_view_t data, proven_deflate_flush_t flush) {
    for (;;) {
        proven_size_t used = 0, made = 0;
        bool done = false;
        proven_err_t e = proven_http_compressor_step_(c->zip, data, &used, (proven_mem_mut_t){ .ptr = c->out, .size = c->out_cap }, &made, flush, &done);
        if (e != PROVEN_OK) { c->close_after = true; return e; }
        data.ptr += used;
        data.size -= used;
        e = sv_send_piece(c, (proven_mem_view_t){ .ptr = c->out, .size = made });
        if (e != PROVEN_OK) return e;
        if (flush == PROVEN_DEFLATE_FLUSH_FINISH ? done : (data.size == 0 && made < c->out_cap)) return PROVEN_OK;
    }
}

proven_err_t proven_http_exchange_write(proven_http_exchange_t *exchange, proven_mem_view_t data) {
    if (!exchange || (data.size > 0 && !data.ptr)) return PROVEN_ERR_INVALID_ARG;
    proven_sv_conn_t *c = &exchange->conn;
    if (!c->response_begun || c->response_ended) return PROVEN_ERR_INVALID_STATE;
    if (c->length_known) {
        if ((proven_u64)data.size > c->out_remaining) return PROVEN_ERR_OUT_OF_BOUNDS;
        c->out_remaining -= data.size;
    }
    if (c->head_only || data.size == 0) return PROVEN_OK;
    if (c->zip) return sv_zip(c, data, PROVEN_DEFLATE_FLUSH_NONE);
    return sv_send_piece(c, data);
}

proven_err_t proven_http_exchange_flush(proven_http_exchange_t *exchange) {
    if (!exchange) return PROVEN_ERR_INVALID_ARG;
    proven_sv_conn_t *c = &exchange->conn;
    if (!c->response_begun || c->response_ended) return PROVEN_ERR_INVALID_STATE;
    if (c->head_only || !c->zip) return PROVEN_OK;     /* nothing is held back: every write has left already */
    return sv_zip(c, (proven_mem_view_t){ 0 }, PROVEN_DEFLATE_FLUSH_SYNC);
}

static proven_err_t sv_end(proven_sv_conn_t *c, const proven_http_header_t *trailers, proven_size_t count) {
    if (!c->response_begun) return PROVEN_ERR_INVALID_STATE;
    if (c->response_ended) return count > 0 ? PROVEN_ERR_INVALID_STATE : PROVEN_OK;
    if (count > 0) {
        /* Trailers go after the last chunk: a response with a length has nowhere to put them.
         * Checked before anything ends; written at the end, after a compressor's last bytes. */
        if (!c->chunked_out || c->head_only) return PROVEN_ERR_INVALID_STATE;
        for (proven_size_t i = 0; i < count; ++i) if (!proven_http_trailer_allowed(trailers[i].name)) return PROVEN_ERR_INVALID_ARG;
    }
    c->response_ended = true;
    /* Fewer bytes than were promised: the message cannot be completed, and the only honest
     * signal left is to close before its announced end. */
    bool cut_short = c->length_known && c->out_remaining > 0;
    if (cut_short) c->close_after = true;
    if (c->zip) {
        /* A short response is left unfinished inside as well: its compressed stream has no
         * end, and no last chunk says otherwise. */
        proven_err_t e = PROVEN_OK;
        if (!c->io_failed && !cut_short) e = sv_zip(c, (proven_mem_view_t){0}, PROVEN_DEFLATE_FLUSH_FINISH);
        proven_http_compressor_destroy_(c->zip);
        c->zip = (void *)0;
        if (e != PROVEN_OK) return e;
        if (cut_short) return PROVEN_OK;
    }
    if (c->chunked_out && !c->io_failed) {
        if (count > 0) {
            proven_size_t tl = 0;
            proven_err_t e = proven_http_write_trailers((proven_mem_mut_t){ .ptr = c->out, .size = c->out_cap }, &tl, trailers, count);
            if (e != PROVEN_OK) { c->close_after = true; return e; }    /* the body cannot be ended now */
            return sv_send(c, (proven_mem_view_t){ .ptr = c->out, .size = tl });
        }
        return sv_send(c, (proven_mem_view_t){ .ptr = (const proven_byte_t *)"0\r\n\r\n", .size = 5 });
    }
    return PROVEN_OK;
}

proven_err_t proven_http_exchange_end(proven_http_exchange_t *exchange) {
    if (!exchange) return PROVEN_ERR_INVALID_ARG;
    return sv_end(&exchange->conn, (void *)0, 0);
}

proven_err_t proven_http_exchange_end_trailers(proven_http_exchange_t *exchange, const proven_http_header_t *fields, proven_size_t count) {
    if (!exchange || count == 0 || !fields) return PROVEN_ERR_INVALID_ARG;
    return sv_end(&exchange->conn, fields, count);
}

proven_err_t proven_http_exchange_trailers(const proven_http_exchange_t *exchange, proven_http_header_t *fields, proven_size_t cap, proven_size_t *count) {
    if (count) *count = 0;
    if (!exchange || !count || (cap > 0 && !fields)) return PROVEN_ERR_INVALID_ARG;
    const proven_sv_conn_t *c = &exchange->conn;
    if (!c->body_done) return PROVEN_ERR_INVALID_STATE;
    return proven_http_body_trailers(&c->body, fields, cap, count);
}

proven_err_t proven_http_exchange_respond(proven_http_exchange_t *exchange, proven_u16 status,
                                          const proven_http_header_t *headers, proven_size_t header_count,
                                          proven_mem_view_t body) {
    if (!exchange || (body.size > 0 && !body.ptr)) return PROVEN_ERR_INVALID_ARG;
    proven_sv_conn_t *c = &exchange->conn;
    if (c->response_begun) return PROVEN_ERR_INVALID_STATE;
    proven_allocator_t a = c->server->cfg.alloc;
    proven_size_t head_len = 0;
    bool chunked = false, will_close = false, bodiless = false, vary = false;
    proven_http_coding_t coding = PROVEN_HTTP_CODING_IDENTITY;
    sv_compress_plan(c, status, headers, header_count, &vary, &coding);
    /* Compressed first, because the head says how long the result is - for HEAD too, which
     * must announce what GET would send. Too short to gain, no smaller, or no memory: the
     * body goes as it is. */
    proven_byte_t *packed = (void *)0;
    proven_size_t packed_cap = 0, packed_size = 0;
    const proven_http_compress_cfg_t_ cc = sv_compress_cfg(c->server);
    if (coding != PROVEN_HTTP_CODING_IDENTITY && proven_http_compress_all_(a, coding, &cc, body, &packed, &packed_cap, &packed_size) != PROVEN_OK) {
        packed = (void *)0;
    }
    if (packed) body = (proven_mem_view_t){ .ptr = packed, .size = packed_size };
    proven_err_t e = sv_build_head(c, status, headers, header_count, body.size, vary, packed ? coding : PROVEN_HTTP_CODING_IDENTITY,
                                   &head_len, &chunked, &will_close, &bodiless);
    if (e == PROVEN_OK) {
        sv_mark_begun(c, body.size, chunked, will_close, bodiless);
        c->out_remaining = 0;
        c->response_ended = true;

        /* A small response leaves in one write, and so in one packet. */
        if (!bodiless && body.size <= c->out_cap - head_len) {
            proven_net_mem_copy(c->out + head_len, body.ptr, body.size);
            e = sv_send(c, (proven_mem_view_t){ .ptr = c->out, .size = head_len + body.size });
        } else {
            e = sv_send(c, (proven_mem_view_t){ .ptr = c->out, .size = head_len });
            if (e == PROVEN_OK && !bodiless) e = sv_send(c, body);
        }
    }
    if (packed) sv_free(a, packed);
    return e;
}

// -----------------------------------------------------------------------------
// Upgrade: the connection leaves the server
// -----------------------------------------------------------------------------

/* A socket that owns itself: what an upgrade hands out. Closing the transport closes the
 * socket and frees this. */
typedef struct {
    proven_allocator_t alloc;
    proven_net_conn_t sock;
    bool tls;
    proven_transport_t over;               /* with TLS: the TLS transport, which reads and writes `sock` */
    proven_sv_upcount_t *up;               /* the server's count of these, which this one is in */
} proven_sv_owned_t;

static proven_result_size_t sv_owned_read(void *ctx, proven_mem_mut_t dest, proven_net_deadline_t until) {
    proven_sv_owned_t *o = ctx;
    return o->tls ? proven_transport_read(o->over, dest, until) : proven_net_read(&o->sock, dest, until);
}
static proven_result_size_t sv_owned_write(void *ctx, proven_mem_view_t src, proven_net_deadline_t until) {
    proven_sv_owned_t *o = ctx;
    return o->tls ? proven_transport_write(o->over, src, until) : proven_net_write(&o->sock, src, until);
}
static proven_err_t sv_owned_shutdown(void *ctx) {
    proven_sv_owned_t *o = ctx;
    return o->tls ? proven_transport_shutdown(o->over) : proven_net_shutdown_write(&o->sock);
}
static proven_err_t sv_owned_close(void *ctx) {
    proven_sv_owned_t *o = ctx;
    /* Closing the TLS transport says goodbye and closes the socket under it. */
    proven_err_t e = o->tls ? proven_transport_close(o->over) : proven_net_close(&o->sock);
    (void)atomic_fetch_sub_explicit(&o->up->open, 1, memory_order_acq_rel);
    sv_up_release(o->up);
    sv_free(o->alloc, o);
    return e;
}

proven_err_t proven_http_exchange_upgrade(proven_http_exchange_t *exchange, proven_u8str_view_t protocol,
                                          const proven_http_header_t *headers, proven_size_t header_count,
                                          proven_transport_t *out, proven_mem_view_t *early) {
    if (out) *out = (proven_transport_t){0};
    if (early) *early = (proven_mem_view_t){0};
    if (!exchange || !out || !early || protocol.size == 0 || (header_count > 0 && !headers)) return PROVEN_ERR_INVALID_ARG;
    proven_sv_conn_t *c = &exchange->conn;
    /* Only a request with no body left to read can change protocols: after the 101 the bytes
     * on the connection belong to the new one. */
    if (c->response_begun || c->detached || !c->body_done || c->req.version_minor < 1) return PROVEN_ERR_INVALID_STATE;
    /* A place among the upgraded connections, taken now and given back if the 101 does not go. */
    proven_sv_upcount_t *const up = c->server->up;
    if (atomic_fetch_add_explicit(&up->open, 1, memory_order_acq_rel) >= c->server->cfg.max_upgraded) {
        (void)atomic_fetch_sub_explicit(&up->open, 1, memory_order_acq_rel);
        return PROVEN_ERR_BUSY;
    }

    proven_mem_mut_t head = { .ptr = c->out, .size = c->out_cap };
    proven_size_t len = 0;
    proven_err_t e = proven_http_write_status_line(head, &len, 101, PROVEN_LIT(""));
    proven_byte_t date[PROVEN_HTTP_DATE_SIZE];
    if (e == PROVEN_OK && proven_http_date_format(proven_time_now(), date) == PROVEN_OK) {
        e = proven_http_write_header(head, &len, PROVEN_LIT("Date"), (proven_u8str_view_t){ .ptr = date, .size = sizeof date });
    }
    if (e == PROVEN_OK) e = proven_http_write_header(head, &len, PROVEN_LIT("Upgrade"), protocol);
    if (e == PROVEN_OK) e = proven_http_write_header(head, &len, PROVEN_LIT("Connection"), PROVEN_LIT("Upgrade"));
    for (proven_size_t i = 0; i < header_count && e == PROVEN_OK; ++i) {
        if (sv_is_driver_header(headers[i].name) || proven_u8str_view_eq_ascii_ci(headers[i].name, PROVEN_LIT("Upgrade"))) { e = PROVEN_ERR_INVALID_ARG; break; }
        e = proven_http_write_header(head, &len, headers[i].name, headers[i].value);
    }
    if (e == PROVEN_OK) e = proven_http_write_head_end(head, &len);
    proven_sv_owned_t *owned = e == PROVEN_OK ? sv_alloc(c->server->cfg.alloc, sizeof *owned) : (void *)0;
    if (e == PROVEN_OK && !owned) e = PROVEN_ERR_NOMEM;
    if (e == PROVEN_OK) {
        c->response_begun = true;
        c->response_ended = true;
        e = sv_send(c, (proven_mem_view_t){ .ptr = c->out, .size = len });
        if (e != PROVEN_OK) sv_free(c->server->cfg.alloc, owned);
    }
    if (e != PROVEN_OK) { (void)atomic_fetch_sub_explicit(&up->open, 1, memory_order_acq_rel); return e; }
    (void)atomic_fetch_add_explicit(&up->refs, 1, memory_order_acq_rel);

    /* With handlers on the loop's thread the connection is still registered with the selector;
     * it must leave before the socket does. (On a worker it was taken out at dispatch.) */
    sv_unwatch(c->server, c);
    owned->alloc = c->server->cfg.alloc;
    owned->sock = c->sock;
    owned->tls = c->tls;
    owned->over = c->t;
    owned->up = up;
    /* The socket has a new home; the TLS transport must read and write it there. */
    if (c->tls) proven_tls_transport_set_plain_(owned->over, proven_net_conn_transport(&owned->sock));
    c->sock = (proven_net_conn_t){0};
    c->t = (proven_transport_t){0};
    c->tls = false;
    c->detached = true;
    c->close_after = true;
    out->ctx = owned;
    out->read_fn = sv_owned_read;
    out->write_fn = sv_owned_write;
    out->shutdown_fn = sv_owned_shutdown;
    out->close_fn = sv_owned_close;
    *early = (proven_mem_view_t){ .ptr = c->buf + c->body_pos, .size = c->len - c->body_pos };
    return PROVEN_OK;
}

/* The response for a request that never reaches a handler, written without waiting: it is a
 * few dozen bytes into an empty socket buffer, and the loop thread must not be held by a
 * client that will not read. */
static void sv_reject(proven_sv_conn_t *c, proven_u16 status) {
    proven_byte_t m[160];
    proven_mem_mut_t out = { .ptr = m, .size = sizeof m };
    proven_size_t len = 0;
    proven_err_t e = proven_http_write_status_line(out, &len, status, PROVEN_LIT(""));
    if (e == PROVEN_OK) e = proven_http_write_header(out, &len, PROVEN_LIT("Content-Length"), PROVEN_LIT("0"));
    if (e == PROVEN_OK) e = proven_http_write_header(out, &len, PROVEN_LIT("Connection"), PROVEN_LIT("close"));
    if (e == PROVEN_OK) e = proven_http_write_head_end(out, &len);
    if (e == PROVEN_OK) (void)proven_transport_write(c->t, (proven_mem_view_t){ .ptr = m, .size = len }, PROVEN_NET_DONT_WAIT);
}

// -----------------------------------------------------------------------------
// Reading a request body
// -----------------------------------------------------------------------------

const proven_http_request_t *proven_http_exchange_request(const proven_http_exchange_t *exchange) {
    return exchange ? &exchange->conn.req : (void *)0;
}

bool proven_http_exchange_early(const proven_http_exchange_t *exchange) { return exchange && exchange->conn.early; }

proven_net_addr_t proven_http_exchange_peer(const proven_http_exchange_t *exchange) {
    proven_net_addr_t none = {0};
    return exchange ? exchange->conn.peer : none;
}

static proven_result_size_t sv_read_body(proven_sv_conn_t *c, proven_mem_mut_t dest) {
    proven_result_size_t res = { .err = PROVEN_OK, .value = 0 };
    if (c->body_error != PROVEN_OK) { res.err = c->body_error; return res; }
    if (c->body_done) { res.err = PROVEN_ERR_EOF; return res; }
    if (dest.size == 0) return res;

    if (c->expect_continue) {
        c->expect_continue = false;
        if (!c->response_begun) {
            static const char go[] = "HTTP/1.1 100 Continue\r\n\r\n";
            proven_err_t e = sv_send(c, (proven_mem_view_t){ .ptr = (const proven_byte_t *)go, .size = sizeof go - 1 });
            if (e != PROVEN_OK) { c->body_error = e; res.err = e; return res; }
        }
    }
    for (;;) {
        if (c->body_pos < c->len) {
            proven_size_t avail = c->len - c->body_pos;
            proven_size_t offer = avail < dest.size ? avail : dest.size;
            proven_size_t used = 0;
            proven_mem_view_t payload;
            bool done = false;
            proven_err_t e = proven_http_body_feed(&c->body, (proven_mem_view_t){ .ptr = c->buf + c->body_pos, .size = offer }, &used, &payload, &done);
            if (e != PROVEN_OK) { c->body_error = e; c->close_after = true; res.err = e; return res; }
            c->body_pos += used;
            if (done) c->body_done = true;
            if (payload.size > 0) {
                proven_net_mem_copy(dest.ptr, payload.ptr, payload.size);     /* every body byte: a block copy */
                res.value = payload.size;
                return res;
            }
            if (done) { res.err = PROVEN_ERR_EOF; return res; }
            continue;
        }
        /* The window is empty: refill it. It starts after the head, whose bytes the request's
         * views still point into. */
        c->body_pos = c->head_size;
        c->len = c->head_size;
        proven_result_size_t r = proven_transport_read(c->t, (proven_mem_mut_t){ .ptr = c->buf + c->len, .size = c->cap - c->len },
                                                       proven_net_deadline_in(c->server->cfg.body_timeout_ms));
        if (r.err != PROVEN_OK) {
            proven_err_t e = r.err == PROVEN_ERR_EOF ? PROVEN_ERR_RESET : r.err;
            c->body_error = e;
            c->close_after = true;
            res.err = e;
            return res;
        }
        c->len += r.value;
    }
}

proven_result_size_t proven_http_exchange_read(proven_http_exchange_t *exchange, proven_mem_mut_t dest) {
    proven_result_size_t res = { .err = PROVEN_ERR_INVALID_ARG, .value = 0 };
    if (!exchange || (dest.size > 0 && !dest.ptr)) return res;
    return sv_read_body(&exchange->conn, dest);
}

// -----------------------------------------------------------------------------
// One request, from dispatch to the connection's fate
// -----------------------------------------------------------------------------

/* Runs on the loop thread, or on a worker: the handler, and then everything that has to be
 * true before the connection can either carry another request or be closed. */
static void sv_run(proven_sv_conn_t *c) {
    proven_http_server_t *s = c->server;
    s->cfg.handler(s->cfg.handler_ctx, (proven_http_exchange_t *)c);

    /* The handler took the connection away with an upgrade: nothing of it is left here. */
    if (c->detached) { c->keep = false; return; }

    if (!c->response_begun) {
        /* The handler sent nothing. If reading the body failed, that is the reason and it has
         * its own status; otherwise the handler is at fault. */
        proven_u16 status = 500;
        if (c->body_error == PROVEN_ERR_TIMEOUT) status = 408;
        else if (c->body_error == PROVEN_ERR_OUT_OF_BOUNDS) status = 413;
        else if (c->body_error == PROVEN_ERR_INVALID_FORMAT) status = 400;
        c->close_after = c->close_after || c->body_error != PROVEN_OK;
        if (c->body_error != PROVEN_ERR_RESET) {
            (void)proven_http_exchange_respond((proven_http_exchange_t *)c, status, (void *)0, 0, (proven_mem_view_t){0});
        }
    } else if (!c->response_ended) {
        (void)proven_http_exchange_end((proven_http_exchange_t *)c);
    }
    if (c->zip) {                       /* ended before it was begun to be written, or never ended */
        proven_http_compressor_destroy_(c->zip);
        c->zip = (void *)0;
    }

    /* The next request starts where this one's body ends, so what the handler did not read
     * has to be read off the connection - within reason. */
    if (!c->close_after && !c->body_done && c->expect_continue) {
        /* The client asked before sending its body and was never told to go on. It may send
         * the body anyway or may not; waiting to find out would hold this thread for nothing. */
        c->close_after = true;
    }
    if (!c->close_after && !c->body_done) {
        proven_byte_t sink[1024];
        proven_size_t total = 0;
        while (!c->body_done && total <= SV_DRAIN_LIMIT) {
            proven_result_size_t r = sv_read_body(c, (proven_mem_mut_t){ .ptr = sink, .size = sizeof sink });
            if (r.err != PROVEN_OK) break;
            total += r.value;
        }
        if (!c->body_done) c->close_after = true;
    }
    c->keep = !c->close_after && c->body_done && proven_http_request_keep_alive(&c->req) &&
              !atomic_load_explicit(&s->stop, memory_order_relaxed) && !atomic_load_explicit(&s->shutdown_asked, memory_order_relaxed);
}

static void sv_job(void *arg) {
    proven_sv_conn_t *c = arg;
    proven_http_server_t *s = c->server;
    sv_run(c);
    /* Hand the connection back: push, wake, and only then count down. The loop may free the
     * connection as soon as it is on the stack, so nothing touches it after the push; and
     * destroy may free the server as soon as the count reaches zero, so the count is the last
     * thing this thread touches. */
    proven_sv_conn_t *head = atomic_load_explicit(&s->done, memory_order_relaxed);
    do {
        c->next_done = head;
    } while (!atomic_compare_exchange_weak_explicit(&s->done, &head, c, memory_order_release, memory_order_relaxed));
    proven_net_waker_wake(&s->waker);
    atomic_fetch_sub_explicit(&s->in_flight, 1, memory_order_release);
}

// -----------------------------------------------------------------------------
// Timers: a hashed wheel, so that an idle connection costs nothing until its time comes
// -----------------------------------------------------------------------------

static void sv_timer_cancel(proven_http_server_t *s, proven_sv_conn_t *c) {
    if (!c->timed) return;
    proven_size_t slot = (proven_size_t)(c->timer_tick % SV_WHEEL_SLOTS);
    if (c->timer_prev) c->timer_prev->timer_next = c->timer_next;
    else s->wheel[slot] = c->timer_next;
    if (c->timer_next) c->timer_next->timer_prev = c->timer_prev;
    c->timer_prev = (void *)0;
    c->timer_next = (void *)0;
    c->timed = false;
    s->timed_count--;
}

/* Give the connection `ms` from now; the wheel calls sv_expired when that has passed. */
static void sv_wait_for(proven_http_server_t *s, proven_sv_conn_t *c, proven_u32 ms) {
    sv_timer_cancel(s, c);
    c->deadline = proven_net_deadline_in(ms);
    /* Rounded up to a tick, and never into a slot the wheel has already passed. */
    proven_u64 tick = (proven_u64)c->deadline / SV_TICK_NS + 1;
    if (tick <= s->wheel_tick) tick = s->wheel_tick + 1;
    c->timer_tick = tick;
    proven_size_t slot = (proven_size_t)(tick % SV_WHEEL_SLOTS);
    c->timer_prev = (void *)0;
    c->timer_next = s->wheel[slot];
    if (c->timer_next) c->timer_next->timer_prev = c;
    s->wheel[slot] = c;
    c->timed = true;
    s->timed_count++;
}

/* When the next timer may be due: the time of the first slot that holds anything. A slot can
 * hold timers of a later turn, so this may be early - which costs one look - but never late. */
static proven_net_deadline_t sv_next_timer(const proven_http_server_t *s) {
    if (s->timed_count == 0) return PROVEN_NET_NO_DEADLINE;
    for (proven_u64 t = s->wheel_tick + 1; t <= s->wheel_tick + SV_WHEEL_SLOTS; ++t) {
        if (s->wheel[t % SV_WHEEL_SLOTS]) return (proven_net_deadline_t)(t * SV_TICK_NS);
    }
    return PROVEN_NET_NO_DEADLINE;
}

// -----------------------------------------------------------------------------
// Watching, closing
// -----------------------------------------------------------------------------

static void sv_close_conn(proven_http_server_t *s, proven_sv_conn_t *c);

/* Register the connection with the selector, to be told when it has something to read. */
static bool sv_watch(proven_http_server_t *s, proven_sv_conn_t *c) {
    if (c->watched) return true;
    if (proven_net_selector_add(s->selector, proven_net_conn_handle(&c->sock), PROVEN_NET_READABLE, c) != PROVEN_OK) {
        sv_close_conn(s, c);
        return false;
    }
    c->watched = true;
    return true;
}

static void sv_unwatch(proven_http_server_t *s, proven_sv_conn_t *c) {
    if (!c->watched) return;
    (void)proven_net_selector_remove(s->selector, proven_net_conn_handle(&c->sock));
    c->watched = false;
}

/* The listeners are watched only while there is room for another connection: a client beyond
 * the limit then waits in the backlog instead of waking the loop for nothing. */
static void sv_update_listeners(proven_http_server_t *s) {
    bool want = s->conn_count < s->cfg.max_connections && !s->draining;
    if (want == s->listening) return;
    for (proven_size_t i = 0; i < s->listener_count; ++i) {
        proven_net_handle_t h = proven_net_listener_handle(&s->listeners[i]);
        if (want) (void)proven_net_selector_add(s->selector, h, PROVEN_NET_READABLE, &s->listeners[i]);
        else (void)proven_net_selector_remove(s->selector, h);
    }
    s->listening = want;
}

/* Close a connection. Its memory stays until the end of the round, because an event for it
 * may already have been taken from the selector and still be waiting to be looked at. */
static void sv_close_conn(proven_http_server_t *s, proven_sv_conn_t *c) {
    if (c->dead) return;
    c->dead = true;
    sv_timer_cancel(s, c);
    sv_unwatch(s, c);                       /* before the close: see proven_net_selector_remove */
    if (c->tls) proven_tls_transport_close_now_(c->t);        /* a goodbye if it goes out at once; closes the socket */
    else (void)proven_net_close(&c->sock);
    if (c->prev) c->prev->next = c->next;
    else s->conns = c->next;
    if (c->next) c->next->prev = c->prev;
    s->conn_count--;
    c->next_dead = s->dead;
    s->dead = c;
    sv_update_listeners(s);
}

static void sv_free_dead(proven_http_server_t *s) {
    while (s->dead) {
        proven_sv_conn_t *c = s->dead;
        s->dead = c->next_dead;
        if (c->trailers) sv_free(s->cfg.alloc, c->trailers);
        sv_free(s->cfg.alloc, c);
    }
}

/*
 * Close a connection the polite way: say that nothing more will be sent, then keep reading -
 * and discarding - until the client closes or a short time has passed.
 *
 * Closing at once would be simpler and wrong. A socket closed while it holds unread input
 * answers with a reset, and a reset makes the peer's system throw away what it had received
 * and not yet handed to the program - which is the very response that was just sent. So a
 * client told "413, too large" in the middle of its upload would see a broken connection
 * instead of the 413.
 */
static void sv_linger(proven_http_server_t *s, proven_sv_conn_t *c) {
    if (c->detached || c->io_failed || atomic_load_explicit(&s->stop, memory_order_relaxed)) { sv_close_conn(s, c); return; }
    if (c->tls) proven_tls_transport_shutdown_now_(c->t);
    (void)proven_net_shutdown_write(&c->sock);
    c->lingering = true;
    c->len = 0;
    if (sv_watch(s, c)) sv_wait_for(s, c, s->cfg.linger_ms);
}

/* A connection's time is up: a request that stopped half-way gets a 408 and a moment to hear
 * it; a connection that was merely idle, or was already being closed, is closed. */
static void sv_expired(proven_http_server_t *s, proven_sv_conn_t *c) {
    if (!c->lingering && c->len > 0) {
        sv_reject(c, 408);
        sv_linger(s, c);
    } else {
        sv_close_conn(s, c);
    }
}

/* Move the wheel to `now`, expiring what is due. Returns whether anything was. */
static bool sv_timers_advance(proven_http_server_t *s, proven_time_t now) {
    proven_u64 target = (proven_u64)now / SV_TICK_NS;
    if (target <= s->wheel_tick) return false;
    bool any = false;
    if (s->timed_count > 0) {
        /* After a long sleep every slot is visited once; more turns would find nothing new. */
        proven_u64 from = target - s->wheel_tick > SV_WHEEL_SLOTS ? target - SV_WHEEL_SLOTS + 1 : s->wheel_tick + 1;
        for (proven_u64 t = from; t <= target && s->timed_count > 0; ++t) {
            proven_sv_conn_t *c = s->wheel[t % SV_WHEEL_SLOTS];
            while (c) {
                proven_sv_conn_t *next = c->timer_next;
                if (c->timer_tick <= target) {
                    /* Off the wheel first; what happens to it next may put it back on. Its
                     * neighbour `next` cannot be removed by that: only `c` is touched. */
                    sv_timer_cancel(s, c);
                    s->wheel_tick = t - 1;          /* so that a re-armed timer lands ahead */
                    sv_expired(s, c);
                    any = true;
                }
                c = next;
            }
        }
    }
    s->wheel_tick = target;
    return any;
}

static void sv_reset_request(proven_sv_conn_t *c) {
    c->req = (proven_http_request_t){0};
    c->head_size = 0;
    c->body_pos = 0;
    c->body_error = PROVEN_OK;
    c->out_remaining = 0;
    c->body_done = false;
    c->expect_continue = false;
    c->head_only = false;
    c->response_begun = false;
    c->response_ended = false;
    c->chunked_out = false;
    c->length_known = false;
    c->close_after = false;
    c->io_failed = false;
    c->detached = false;
    c->keep = false;
    c->compress_asked = false;
}

/*
 * Parse what a waiting connection has, and act on it. Returns true when the connection is
 * still open and waiting (for more bytes, or for a worker to bring it back), false when it was
 * closed or is being closed. Loops, so that pipelined requests already in the buffer are served
 * in turn.
 */
static bool sv_service(proven_http_server_t *s, proven_sv_conn_t *c) {
    for (;;) {
        /* A TLS transport may hold input the socket will never announce again: a second
         * request that arrived in the same segment as the first. Take it now. */
        while (c->tls && c->len < c->cap && proven_tls_transport_buffered_(c->t)) {
            proven_result_size_t r = proven_transport_read(c->t, (proven_mem_mut_t){ .ptr = c->buf + c->len, .size = c->cap - c->len }, PROVEN_NET_DONT_WAIT);
            if (r.err == PROVEN_ERR_TIMEOUT || r.err == PROVEN_ERR_AGAIN) break;
            if (r.err != PROVEN_OK) { sv_close_conn(s, c); return false; }
            if (r.value == 0) break;
            c->len += r.value;
        }
        if (c->len == 0) {
            sv_wait_for(s, c, s->cfg.idle_timeout_ms);
            return true;
        }
        proven_size_t head = 0;
        proven_err_t e = proven_http_parse_request((proven_mem_view_t){ .ptr = c->buf, .size = c->len }, c->headers, s->cfg.max_headers,
                                                   s->cfg.max_head_bytes, &c->req, &head);
        if (e == PROVEN_ERR_NEED_MORE) return true;
        proven_u16 refuse = 0;
        proven_http_framing_t framing = {0};
        if (e == PROVEN_ERR_OUT_OF_BOUNDS) refuse = 431;
        else if (e == PROVEN_ERR_UNSUPPORTED) refuse = 505;
        else if (e != PROVEN_OK) refuse = 400;
        if (!refuse) {
            e = proven_http_request_framing(&c->req, &framing);
            if (e == PROVEN_ERR_UNSUPPORTED) refuse = 501;
            else if (e != PROVEN_OK) refuse = 400;
        }
        if (!refuse) {
            e = proven_http_body_init(&c->body, framing, s->cfg.max_body_bytes);
            if (e != PROVEN_OK) refuse = 413;
        }
        /* A chunked body's trailers are kept for the handler (proven_http_exchange_trailers). */
        if (!refuse && framing.kind == PROVEN_HTTP_BODY_CHUNKED) {
            if (!c->trailers) c->trailers = sv_alloc(s->cfg.alloc, PROVEN_HTTP_MAX_TRAILER_BYTES);
            if (!c->trailers || proven_http_body_keep_trailers(&c->body, (proven_mem_mut_t){ .ptr = c->trailers, .size = PROVEN_HTTP_MAX_TRAILER_BYTES }) != PROVEN_OK) refuse = 500;
        }
        /* HTTP/1.1 requires Host; without it a request cannot be routed, and a server that
         * guesses is the back end a smuggled request is aimed at. */
        if (!refuse && c->req.version_minor >= 1 && proven_http_header_count(c->headers, c->req.header_count, PROVEN_LIT("Host")) != 1) refuse = 400;
        if (refuse) {
            sv_reject(c, refuse);
            sv_linger(s, c);
            return false;
        }

        c->head_size = head;
        c->body_pos = head;
        c->body_done = framing.kind == PROVEN_HTTP_BODY_NONE || (framing.kind == PROVEN_HTTP_BODY_LENGTH && framing.length == 0);
        /* Early data (RFC-0027): a head that ends among the bytes that arrived early came early.
         * The buffer begins with this request. Only one safe to repeat reaches the handler;
         * anything else is answered 425 Too Early, and the client sends it again (RFC 8470). */
        c->early = false;
        if (c->tls) {
            proven_size_t early = 0, total = 0;
            proven_tls_read_counts_(proven_tls_transport_conn(c->t), &early, &total);
            c->early = early > 0 && c->len <= total && total - c->len + head <= early;
        }
        if (c->early && !(c->body_done && (c->req.method == PROVEN_HTTP_GET || c->req.method == PROVEN_HTTP_HEAD || c->req.method == PROVEN_HTTP_OPTIONS))) {
            sv_reject(c, 425);
            sv_linger(s, c);
            return false;
        }
        c->expect_continue = c->req.version_minor >= 1 && !c->body_done &&
                             proven_http_header_has_token(c->headers, c->req.header_count, PROVEN_LIT("Expect"), PROVEN_LIT("100-continue"));
        c->handling = true;
        sv_timer_cancel(s, c);         /* the handler's own reads and writes have their own limits */

        if (s->cfg.jobs) {
            /* The connection changes hands: this loop must not be told about it meanwhile. */
            sv_unwatch(s, c);
            atomic_fetch_add_explicit(&s->in_flight, 1, memory_order_relaxed);
            if (proven_job_submit_ex(s->cfg.jobs, sv_job, c) != PROVEN_OK) {
                atomic_fetch_sub_explicit(&s->in_flight, 1, memory_order_relaxed);
                c->handling = false;
                sv_reject(c, 503);
                sv_linger(s, c);
                return false;
            }
            return true;               /* it comes back through the done stack */
        }

        sv_run(c);
        c->handling = false;
        if (!c->keep) { sv_linger(s, c); return false; }
        /* What follows the body in the buffer is the next request. */
        proven_size_t rest = c->len - c->body_pos;
        proven_net_mem_move(c->buf, c->buf + c->body_pos, rest);
        c->len = rest;
        sv_reset_request(c);
        sv_wait_for(s, c, rest ? s->cfg.head_timeout_ms : s->cfg.idle_timeout_ms);
    }
}

/* Connections that workers have finished with. */
static bool sv_collect_done(proven_http_server_t *s) {
    proven_sv_conn_t *c = atomic_exchange_explicit(&s->done, (proven_sv_conn_t *)0, memory_order_acquire);
    bool any = c != (void *)0;
    while (c) {
        proven_sv_conn_t *next = c->next_done;
        c->next_done = (void *)0;
        c->handling = false;
        /* A stop that came after the worker decided to keep the connection still wins: nothing
         * new is dispatched once the server is stopping. */
        if (!c->keep || atomic_load_explicit(&s->stop, memory_order_acquire) || s->draining) {
            sv_linger(s, c);
        } else {
            proven_size_t rest = c->len - c->body_pos;
            proven_net_mem_move(c->buf, c->buf + c->body_pos, rest);
            c->len = rest;
            sv_reset_request(c);
            if (!sv_watch(s, c)) { c = next; continue; }
            sv_wait_for(s, c, rest ? s->cfg.head_timeout_ms : s->cfg.idle_timeout_ms);
            if (rest || c->tls) (void)sv_service(s, c);
        }
        c = next;
    }
    return any;
}

static void sv_accept(proven_http_server_t *s, proven_net_listener_t *l) {
    while (s->conn_count < s->cfg.max_connections) {
        proven_net_conn_t sock;
        proven_net_addr_t peer;
        if (proven_net_accept(l, PROVEN_NET_DONT_WAIT, &sock, &peer) != PROVEN_OK) return;

        /* One allocation per connection: the struct, the header array, the request buffer and
         * the response-head buffer. */
        proven_size_t headers_bytes = s->cfg.max_headers * sizeof(proven_http_header_t);
        proven_size_t cap = s->cfg.max_head_bytes + SV_BODY_WINDOW;
        proven_size_t out_cap = s->cfg.max_head_bytes;
        proven_sv_conn_t *c = sv_alloc(s->cfg.alloc, sizeof *c + headers_bytes + cap + out_cap);
        if (!c) { (void)proven_net_close(&sock); return; }
        *c = (proven_sv_conn_t){0};
        c->server = s;
        c->sock = sock;
        c->t = proven_net_conn_transport(&c->sock);
        if (s->cfg.tls) {
            /* The handshake is not run here: it happens inside the reads, as the client's
             * bytes arrive, so that a slow client holds up nobody else. */
            proven_transport_t tls;
            if (proven_tls_transport_server_lazy_(c->t, s->cfg.tls, &tls) != PROVEN_OK) { (void)proven_net_close(&sock); sv_free(s->cfg.alloc, c); return; }
            c->t = tls;
            c->tls = true;
            if (s->cfg.tls->early_data) proven_tls_server_take_early_(proven_tls_transport_conn(tls));   /* RFC-0027 */
        }
        c->peer = peer;
        c->headers = (proven_http_header_t *)(void *)(c + 1);
        c->buf = (proven_byte_t *)(c + 1) + headers_bytes;
        c->cap = cap;
        c->out = c->buf + cap;
        c->out_cap = out_cap;
        (void)proven_net_conn_set_nodelay(&c->sock, true);
        c->next = s->conns;
        if (s->conns) s->conns->prev = c;
        s->conns = c;
        s->conn_count++;
        /* A new connection is expected to say something promptly: it gets the head timeout,
         * not the idle one. */
        if (sv_watch(s, c)) sv_wait_for(s, c, s->cfg.head_timeout_ms);
    }
    sv_update_listeners(s);
}

/* A waiting connection became readable: take what is there, without waiting, and look at it. */
static void sv_readable(proven_http_server_t *s, proven_sv_conn_t *c) {
    if (c->lingering) {
        /* Whatever arrives now is thrown away; the end of it is what is being waited for. */
        proven_result_size_t r = proven_transport_read(c->t, (proven_mem_mut_t){ .ptr = c->buf, .size = c->cap }, PROVEN_NET_DONT_WAIT);
        if (r.err != PROVEN_OK && r.err != PROVEN_ERR_TIMEOUT) sv_close_conn(s, c);
        return;
    }
    if (c->len == c->cap) { sv_reject(c, 431); sv_linger(s, c); return; }
    bool first = c->len == 0;
    /* Through the transport: with TLS this is where the handshake advances, and it reports
     * "nothing yet" until the first request bytes have been decrypted. */
    proven_result_size_t r = proven_transport_read(c->t, (proven_mem_mut_t){ .ptr = c->buf + c->len, .size = c->cap - c->len }, PROVEN_NET_DONT_WAIT);
    if (r.err == PROVEN_ERR_TIMEOUT || r.err == PROVEN_ERR_AGAIN) return;   /* readiness that turned out to be nothing */
    if (r.err != PROVEN_OK) { sv_close_conn(s, c); return; } /* EOF between requests is how a client leaves */
    if (first) sv_wait_for(s, c, s->cfg.head_timeout_ms);
    c->len += r.value;
    (void)sv_service(s, c);
}

// -----------------------------------------------------------------------------
// The server object and its loop
// -----------------------------------------------------------------------------

proven_err_t proven_http_server_create(const proven_http_server_config_t *config, proven_http_server_t **out) {
    if (out) *out = (void *)0;
    if (!config || !out || !proven_alloc_is_valid(config->alloc) || !config->handler) return PROVEN_ERR_INVALID_ARG;
    proven_http_server_t *s = sv_alloc(config->alloc, sizeof *s);
    if (!s) return PROVEN_ERR_NOMEM;
    *s = (proven_http_server_t){0};
    s->cfg = *config;
    const proven_http_compress_cfg_t_ cc = sv_compress_cfg(s);
    if (!proven_http_compress_cfg_valid_(&cc)) {
        sv_free(config->alloc, s);
        return PROVEN_ERR_INVALID_ARG;
    }
    if (s->cfg.max_connections == 0) s->cfg.max_connections = 64;
    if (s->cfg.max_head_bytes == 0) s->cfg.max_head_bytes = PROVEN_HTTP_DEFAULT_MAX_HEAD;
    if (s->cfg.max_head_bytes < 256) s->cfg.max_head_bytes = 256;
    if (s->cfg.max_headers == 0) s->cfg.max_headers = 64;
    if (s->cfg.max_body_bytes == 0) s->cfg.max_body_bytes = 1024 * 1024;
    if (s->cfg.head_timeout_ms == 0) s->cfg.head_timeout_ms = 10000;
    if (s->cfg.body_timeout_ms == 0) s->cfg.body_timeout_ms = 30000;
    if (s->cfg.write_timeout_ms == 0) s->cfg.write_timeout_ms = 30000;
    if (s->cfg.idle_timeout_ms == 0) s->cfg.idle_timeout_ms = 60000;
    if (s->cfg.linger_ms == 0) s->cfg.linger_ms = SV_LINGER_MS;
    if (s->cfg.max_upgraded == 0) s->cfg.max_upgraded = 1024;
    s->up = sv_alloc(config->alloc, sizeof *s->up);
    if (!s->up) { sv_free(config->alloc, s); return PROVEN_ERR_NOMEM; }
    s->up->alloc = config->alloc;
    atomic_init(&s->up->open, 0);
    atomic_init(&s->up->refs, 1);
    atomic_init(&s->done, (proven_sv_conn_t *)0);
    atomic_init(&s->stop, false);
    atomic_init(&s->in_flight, 0);
    atomic_init(&s->shutdown_asked, false);
    atomic_init(&s->shutdown_grace_ms, 0u);

    if (s->cfg.max_head_bytes > PROVEN_SIZE_MAX / 4 || s->cfg.max_headers > PROVEN_SIZE_MAX / (4 * sizeof(proven_http_header_t))) {
        sv_up_release(s->up);
        sv_free(config->alloc, s);
        return PROVEN_ERR_OVERFLOW;
    }
    s->wheel_tick = (proven_u64)proven_time_monotonic_now() / SV_TICK_NS;
    proven_err_t e = proven_net_selector_create(config->alloc, &s->selector);
    if (e == PROVEN_OK) {
        e = proven_net_waker_open(&s->waker);
        if (e == PROVEN_OK) e = proven_net_selector_add(s->selector, proven_net_waker_handle(&s->waker), PROVEN_NET_READABLE, &s->waker);
        if (e != PROVEN_OK) {
            proven_net_waker_close(&s->waker);
            proven_net_selector_destroy(s->selector);
        }
    }
    if (e != PROVEN_OK) {
        sv_up_release(s->up);
        sv_free(config->alloc, s);
        return e;
    }
    *out = s;
    return PROVEN_OK;
}

proven_err_t proven_http_server_listen(proven_http_server_t *server, proven_net_addr_t at, proven_net_addr_t *bound) {
    if (!server) return PROVEN_ERR_INVALID_ARG;
    if (server->listener_count == SV_MAX_LISTENERS) return PROVEN_ERR_OUT_OF_BOUNDS;
    proven_net_listener_t *l = &server->listeners[server->listener_count];
    proven_err_t e = proven_net_listen(at, 0, l, bound);
    if (e != PROVEN_OK) return e;
    /* Listeners already registered stay so; this one joins them if they are being watched. */
    if (server->listening || server->listener_count == 0) {
        e = proven_net_selector_add(server->selector, proven_net_listener_handle(l), PROVEN_NET_READABLE, l);
        if (e != PROVEN_OK) { (void)proven_net_listener_close(l); return e; }
        server->listening = true;
    }
    server->listener_count++;
    return PROVEN_OK;
}

proven_size_t proven_http_server_connection_count(const proven_http_server_t *server) {
    return server ? server->conn_count : 0;
}

void proven_http_server_stop(proven_http_server_t *server) {
    if (!server) return;
    atomic_store_explicit(&server->stop, true, memory_order_release);
    proven_net_waker_wake(&server->waker);
}

// -----------------------------------------------------------------------------
// The graceful end
// -----------------------------------------------------------------------------

/* Close every connection the loop holds now - not those a handler holds: their sockets are the
 * handler's thread's until it returns. With `idle_only`, only those with nothing of a request. */
static void sv_close_held(proven_http_server_t *s, bool idle_only) {
    for (proven_sv_conn_t *c = s->conns, *next; c; c = next) {
        next = c->next;
        if (c->handling || c->dead) continue;
        if (idle_only && (c->lingering || c->len > 0)) continue;
        sv_close_conn(s, c);
    }
}

/* What a round of the loop does about a shutdown: begin it once it is asked, end it when
 * nothing is left or at the deadline. */
static void sv_drain_step(proven_http_server_t *s) {
    if (s->drain_over || !atomic_load_explicit(&s->shutdown_asked, memory_order_acquire)) return;
    proven_time_t now = proven_time_monotonic_now();
    if (!s->draining) {
        s->draining = true;
        s->drain_deadline = now + (proven_time_t)atomic_load_explicit(&s->shutdown_grace_ms, memory_order_relaxed) * 1000000;
        for (proven_size_t i = 0; i < s->listener_count; ++i) {
            if (s->listening) (void)proven_net_selector_remove(s->selector, proven_net_listener_handle(&s->listeners[i]));
            (void)proven_net_listener_close(&s->listeners[i]);
        }
        s->listener_count = 0;
        s->listening = false;
        sv_close_held(s, true);
    }
    if (s->conn_count == 0) { s->drain_over = true; return; }
    if (now >= s->drain_deadline) {
        s->drain_cut = true;
        sv_close_held(s, false);
        s->drain_over = true;
    }
}

proven_err_t proven_http_server_shutdown(proven_http_server_t *server, proven_u32 grace_ms) {
    if (!server) return PROVEN_ERR_INVALID_ARG;
    atomic_store_explicit(&server->shutdown_grace_ms, grace_ms, memory_order_relaxed);
    if (atomic_exchange_explicit(&server->shutdown_asked, true, memory_order_acq_rel)) return PROVEN_ERR_INVALID_STATE;
    proven_net_waker_wake(&server->waker);
    return PROVEN_OK;
}

bool proven_http_server_shutting_down(const proven_http_server_t *server) {
    return server && atomic_load_explicit(&((proven_http_server_t *)server)->shutdown_asked, memory_order_acquire);
}

bool proven_http_server_drained(const proven_http_server_t *server, proven_err_t *why) {
    if (!server || !server->drain_over) return false;
    if (why) *why = server->drain_cut ? PROVEN_ERR_TIMEOUT : PROVEN_OK;
    return true;
}

proven_err_t proven_http_server_poll(proven_http_server_t *server, proven_net_deadline_t until) {
    if (!server) return PROVEN_ERR_INVALID_ARG;
    proven_http_server_t *s = server;
    bool did = sv_collect_done(s);

    /* The wait ends when a socket is ready, at the caller's deadline, or when the next timer
     * may be due - whichever is first. How many connections are open does not enter into it:
     * the selector reports only the ready ones and the wheel only the expired ones. */
    sv_drain_step(s);
    proven_net_deadline_t wake_at = until;
    proven_net_deadline_t timer = sv_next_timer(s);
    if (timer < wake_at) wake_at = timer;
    if (s->draining && !s->drain_over && (proven_net_deadline_t)s->drain_deadline < wake_at) wake_at = (proven_net_deadline_t)s->drain_deadline;
    if (s->drain_over && !did) did = true;
    /* Work was already done on the way in: look at the sockets, but do not wait for them. */
    if (did) wake_at = PROVEN_NET_DONT_WAIT;

    proven_net_ready_t events[SV_EVENTS];
    proven_size_t n = 0;
    proven_err_t e = proven_net_selector_wait(s->selector, events, SV_EVENTS, wake_at, &n);
    if (e != PROVEN_OK && e != PROVEN_ERR_TIMEOUT) return e;

    for (proven_size_t i = 0; i < n; ++i) {
        void *tag = events[i].tag;
        did = true;
        if (tag == (void *)&s->waker) {
            proven_net_waker_drain(&s->waker);
            (void)sv_collect_done(s);
        } else if ((proven_uintptr_t)tag >= (proven_uintptr_t)&s->listeners[0] && (proven_uintptr_t)tag < (proven_uintptr_t)&s->listeners[SV_MAX_LISTENERS]) {
            sv_accept(s, tag);
        } else {
            /* A connection. It may have been closed by something earlier in this same batch;
             * its memory is still there, marked. */
            proven_sv_conn_t *c = tag;
            if (!c->dead && !c->handling) sv_readable(s, c);
        }
    }

    if (sv_timers_advance(s, proven_time_monotonic_now())) did = true;
    sv_drain_step(s);
    sv_free_dead(s);
    return did ? PROVEN_OK : PROVEN_ERR_TIMEOUT;
}

proven_err_t proven_http_server_run(proven_http_server_t *server) {
    if (!server) return PROVEN_ERR_INVALID_ARG;
    while (!atomic_load_explicit(&server->stop, memory_order_acquire)) {
        proven_err_t e = proven_http_server_poll(server, PROVEN_NET_NO_DEADLINE);
        if (e != PROVEN_OK && e != PROVEN_ERR_TIMEOUT) return e;
        if (server->drain_over) return server->drain_cut ? PROVEN_ERR_TIMEOUT : PROVEN_OK;
    }
    return PROVEN_OK;
}

void proven_http_server_destroy(proven_http_server_t *server) {
    if (!server) return;
    proven_http_server_t *s = server;
    atomic_store_explicit(&s->stop, true, memory_order_release);
    /* Handlers on workers still own their connections: wait for each to come back. */
    for (;;) {
        (void)sv_collect_done(s);
        if (atomic_load_explicit(&s->in_flight, memory_order_acquire) == 0) break;
        proven_net_poll_item_t item = { .handle = proven_net_waker_handle(&s->waker), .want = PROVEN_NET_READABLE };
        proven_size_t ready = 0;
        (void)proven_net_poll(&item, 1, proven_net_deadline_in(5), &ready);
        proven_net_waker_drain(&s->waker);
    }
    (void)sv_collect_done(s);
    while (s->conns) sv_close_conn(s, s->conns);
    sv_free_dead(s);
    for (proven_size_t i = 0; i < s->listener_count; ++i) {
        if (s->listening) (void)proven_net_selector_remove(s->selector, proven_net_listener_handle(&s->listeners[i]));
        (void)proven_net_listener_close(&s->listeners[i]);
    }
    (void)proven_net_selector_remove(s->selector, proven_net_waker_handle(&s->waker));
    proven_net_waker_close(&s->waker);
    proven_net_selector_destroy(s->selector);
    sv_up_release(s->up);
    sv_free(s->cfg.alloc, s);
}

#else
/* A translation unit must not be empty. */
typedef int proven_http_server_unused_t;
#endif
