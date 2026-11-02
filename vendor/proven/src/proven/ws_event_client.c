#include "proven/ws_event.h"
#include "../../platform/proven_sys_mem.h"
#include "proven_internal_http_event.h"
#include "proven_internal_ws.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven/random.h"
#include "proven/time.h"
#include "proven/url.h"

/*
 * A WebSocket connection made by this side, on a loop.
 *
 * ws_event.c is the protocol - frames, messages, pings, the close - and reaches its connection
 * through a table of six calls (proven_ws_transport_t_). For a server that table is the
 * event-driven server's connection record. This file is the other one: a socket on the loop,
 * a TLS engine for `wss`, an output queue, one timer - and, before any of that is a
 * WebSocket, the HTTP/1.1 request that asks for one and the answer that grants it.
 *
 * States: connecting; the TLS handshake; the upgrade (the request is on its way, the answer
 * is being read); open; and, after the protocol has let go of the connection politely,
 * finishing - what is queued leaves, this side says it will send no more, and the peer is
 * given a moment to close.
 *
 * Until it is open the timer is this file's (connect_timeout_ms); from then on it is the
 * protocol's.
 *
 * Callbacks call back in. So, as everywhere on the loop: the record counts the entries on the
 * stack (`depth`), is marked `dead` when the socket has gone, and is freed when the outermost
 * entry leaves; nothing reads a field after a call out without having looked at `dead`.
 */

#define WSC_READ_BYTES 16384u
#define WSC_FINISH_MS 1000u

typedef enum { WSC_CONNECTING, WSC_HANDSHAKE, WSC_UPGRADE, WSC_OPEN, WSC_FINISHING } proven_wsc_state_t;

typedef struct {
    proven_byte_t *p;
    proven_size_t len, off, cap;
} proven_wsc_buf_t;

typedef struct wsc {
    proven_loop_t *loop;
    proven_allocator_t alloc;
    proven_ws_stream_t *ws;            /* the protocol's object; null once it has let go, or has been told the end */
    proven_net_conn_t sock;
    proven_loop_sock_t ls;             /* the connection as the loop knows it (RFC-0025) */
    proven_loop_timer_t timer;
    proven_tls_conn_t *tls;
    proven_wsc_buf_t out;                     /* bytes for the peer that the transport has not taken: the request head, then frames */
    proven_wsc_buf_t in;                      /* the answer's head while it arrives; then input the protocol would not take yet */
    proven_byte_t *rbuf;               /* what one read holds (and, for TLS, what it decrypts to) */
    void (*on_open)(void *ctx, proven_ws_stream_t *ws, proven_u8str_view_t protocol);
    void *ctx;
    proven_size_t max_head;
    proven_u32 depth;
    proven_wsc_state_t state;
    bool attached, sock_gone, recv_armed, dead, paused, more_out, peer_eof, offered_deflate, offered_takeover, offered_peer_none;
    proven_byte_t key[PROVEN_WS_KEY_SIZE];
    proven_size_t protocols_len;
    proven_byte_t protocols[256];      /* the offer, to check the answer against */
} proven_wsc_t;

static proven_u8str_view_t wsc_lit_n(const char *s, proven_size_t n) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = n }; }
#define wsc_lit(s) wsc_lit_n((s), sizeof(s) - 1)

static const proven_http_event_raw_ops_t *wsc_ops(const proven_wsc_t *c) { return *(const proven_http_event_raw_ops_t *const *)(const void *)c->ws; }

static void wsc_enter(proven_wsc_t *c) { c->depth++; }

static void wsc_free(proven_wsc_t *c) {
    proven_allocator_t a = c->alloc;
    if (c->out.p) a.free_fn(a.ctx, c->out.p);
    if (c->in.p) a.free_fn(a.ctx, c->in.p);
    if (c->rbuf) a.free_fn(a.ctx, c->rbuf);
    a.free_fn(a.ctx, c);
}

static void wsc_leave(proven_wsc_t *c) {
    if (--c->depth > 0 || !c->dead) return;
    if (c->attached && !c->sock_gone) return;      /* wsc_sock_closed frees it */
    wsc_free(c);
}

static bool wsc_append(proven_wsc_t *c, proven_wsc_buf_t *b, const proven_byte_t *p, proven_size_t n) {
    if (n == 0) return true;
    if (b->off > 0 && b->off == b->len) b->off = b->len = 0;
    if (b->len + n > b->cap) {
        proven_size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + n) {
            if (cap > ((proven_size_t)-1) / 2) return false;
            cap *= 2;
        }
        proven_result_mem_mut_t m = b->p ? c->alloc.realloc_fn(c->alloc.ctx, b->p, b->cap, cap, 1) : c->alloc.alloc_fn(c->alloc.ctx, cap, 1);
        if (m.err != PROVEN_OK) return false;
        b->p = m.value.ptr;
        b->cap = cap;
    }
    proven_sys_mem_copy(b->p + b->len, p, n);
    b->len += n;
    return true;
}

static proven_size_t wsc_pending(const proven_wsc_buf_t *b) { return b->len - b->off; }

/* The socket and everything on the loop go; the record stays until the stack has left it. */
static void wsc_shut(proven_wsc_t *c) {
    if (c->dead) return;
    c->dead = true;
    proven_loop_timer_cancel(c->loop, &c->timer);
    if (c->attached) proven_loop_close(c->loop, &c->ls);      /* the loop stops watching before the close */
    if (c->tls) { proven_tls_conn_destroy(c->tls); c->tls = (void *)0; }
    if (proven_net_conn_is_open(&c->sock)) (void)proven_net_close(&c->sock);
}

/* The connection is gone without the protocol having let go of it: the protocol is told, once. */
static void wsc_gone(proven_wsc_t *c, proven_err_t why) {
    if (c->dead) return;
    proven_ws_stream_t *ws = c->ws;
    const bool open = c->state == WSC_OPEN;
    c->ws = (void *)0;
    wsc_enter(c);
    wsc_shut(c);
    if (ws) {
        if (open) (*(const proven_http_event_raw_ops_t *const *)(const void *)ws)->on_closed(ws, why);
        else proven_ws_event_client_failed_(ws, why);
    }
    wsc_leave(c);
}

/* How much the loop may hold for the connection before the queue here gives it no more. */
#define WSC_HOLD ((proven_size_t)32 * 1024)

/* Ask the loop for input (a receive of zero bytes, left in flight once armed and ignored when
 * it comes while reading is off), and to be told when what it holds has gone - or, with more
 * waiting here, when half the hold has. */
static bool wsc_reading(const proven_wsc_t *c) {
    /* Not read while the protocol is paused, or while it holds input it has not taken: what
     * the peer sends then waits in the socket, and the peer is told so by TCP. */
    return !c->peer_eof && !(c->state == WSC_OPEN && (c->paused || wsc_pending(&c->in) > 0));
}

static void wsc_gone(proven_wsc_t *c, proven_err_t why);

static void wsc_watch(proven_wsc_t *c) {
    if (c->dead || !c->attached || c->state == WSC_CONNECTING) return;     /* connecting: the loop says when */
    if (wsc_reading(c) && !c->recv_armed) {
        if (proven_loop_recv(c->loop, &c->ls, (proven_mem_mut_t){ .ptr = (void *)0, .size = 0 }) != PROVEN_OK) { wsc_gone(c, PROVEN_ERR_RESET); return; }
        c->recv_armed = true;
    }
    if (proven_loop_send_queued(&c->ls) == 0 && !c->more_out) return;
    if (proven_loop_send_wait(c->loop, &c->ls, c->more_out ? WSC_HOLD / 2 : 0) != PROVEN_OK) wsc_gone(c, PROVEN_ERR_RESET);
}

/* Bytes to the loop, which takes them whole. False when the connection ended. */
static bool wsc_put(proven_wsc_t *c, const proven_byte_t *p, proven_size_t n) {
    proven_result_size_t w = proven_loop_send(c->loop, &c->ls, (proven_mem_view_t){ .ptr = p, .size = n });
    if (w.err == PROVEN_OK && w.value == n) return true;
    wsc_gone(c, w.err == PROVEN_ERR_NOMEM ? PROVEN_ERR_NOMEM : PROVEN_ERR_RESET);
    return false;
}

/* What TLS holds, to the loop. False when the connection ended. */
static bool wsc_flush_tls(proven_wsc_t *c) {
    while (c->tls && !c->dead) {
        proven_mem_view_t pending = proven_tls_pending_output(c->tls);
        if (pending.size == 0) break;
        if (!wsc_put(c, pending.ptr, pending.size)) return false;
        proven_tls_output_sent(c->tls, pending.size);
    }
    return !c->dead;
}

/* As much of the queue as the loop takes now: while it holds less than WSC_HOLD for the
 * connection - straight to the socket when nothing waits there. False when the connection
 * ended. */
static bool wsc_flush(proven_wsc_t *c) {
    if (c->dead) return false;
    if (c->tls && c->state != WSC_HANDSHAKE && c->state != WSC_CONNECTING) {
        /* A record's worth at a time. */
        while (wsc_pending(&c->out) > 0 && proven_loop_send_queued(&c->ls) < WSC_HOLD) {
            if (!wsc_flush_tls(c)) return false;
            const proven_size_t n = wsc_pending(&c->out) < 16384 ? wsc_pending(&c->out) : 16384;
            proven_result_size_t w = proven_tls_write(c->tls, (proven_mem_view_t){ .ptr = c->out.p + c->out.off, .size = n });
            if (w.err != PROVEN_OK) { wsc_gone(c, PROVEN_ERR_RESET); return false; }
            c->out.off += w.value;
            if (w.value == 0) break;
        }
        if (!wsc_flush_tls(c)) return false;
    } else if (!c->tls) {
        const proven_size_t held = proven_loop_send_queued(&c->ls);
        if (wsc_pending(&c->out) > 0 && held < WSC_HOLD) {
            proven_size_t n = WSC_HOLD - held;
            if (n > wsc_pending(&c->out)) n = wsc_pending(&c->out);
            if (!wsc_put(c, c->out.p + c->out.off, n)) return false;
            c->out.off += n;
        }
    } else if (!wsc_flush_tls(c)) return false;
    /* More to give - but not before TLS is up: the handshake's own reads move it, and a wait
     * for room then would be answered every round. */
    c->more_out = wsc_pending(&c->out) > 0 && !(c->tls && (c->state == WSC_HANDSHAKE || c->state == WSC_CONNECTING));
    if (c->out.off == c->out.len) c->out.off = c->out.len = 0;
    return true;
}

static proven_size_t wsc_queued(const proven_wsc_t *c) {
    return wsc_pending(&c->out) + (c->tls ? proven_tls_pending_output(c->tls).size : 0) + (c->attached ? proven_loop_send_queued(&c->ls) : 0);
}

/* After the protocol let go politely: when everything has left, say that no more will come,
 * and wait a moment for the peer to close. */
static void wsc_finishing(proven_wsc_t *c) {
    if (c->dead || c->state != WSC_FINISHING) return;
    if (wsc_queued(c) > 0) { wsc_watch(c); return; }
    if (!c->peer_eof) (void)proven_net_shutdown_write(&c->sock);
    if (c->peer_eof) wsc_shut(c);
    else wsc_watch(c);
}

// -----------------------------------------------------------------------------
// The transport ws_event.c works through
// -----------------------------------------------------------------------------

static bool wsc_tr_send(void *conn, proven_mem_view_t a, proven_mem_view_t b) {
    proven_wsc_t *c = conn;
    if (c->dead) return false;
    wsc_enter(c);
    bool ok = wsc_append(c, &c->out, a.ptr, a.size) && wsc_append(c, &c->out, b.ptr, b.size);
    if (!ok) wsc_gone(c, PROVEN_ERR_NOMEM);
    else if (wsc_flush(c)) wsc_watch(c);
    ok = !c->dead;
    wsc_leave(c);
    return ok;
}

static void wsc_on_timer(void *ctx);

static void wsc_tr_timer(void *conn, proven_u32 ms) {
    proven_wsc_t *c = conn;
    if (c->dead) return;
    if (ms == 0) proven_loop_timer_cancel(c->loop, &c->timer);
    else proven_loop_timer_set(c->loop, &c->timer, ms, wsc_on_timer, c);
}

static void wsc_deliver(proven_wsc_t *c);

static void wsc_tr_pause(void *conn, bool paused) {
    proven_wsc_t *c = conn;
    if (c->dead || c->paused == paused) return;
    c->paused = paused;
    wsc_enter(c);
    if (!paused) wsc_deliver(c);                   /* what was held back comes first */
    if (!c->dead) wsc_watch(c);
    wsc_leave(c);
}

static void wsc_tr_close(void *conn) {
    proven_wsc_t *c = conn;
    if (c->dead) return;
    c->ws = (void *)0;
    c->state = WSC_FINISHING;
    c->in.off = c->in.len = 0;
    wsc_enter(c);
    proven_loop_timer_set(c->loop, &c->timer, WSC_FINISH_MS, wsc_on_timer, c);
    if (wsc_flush(c)) wsc_finishing(c);
    wsc_leave(c);
}

static void wsc_tr_kill(void *conn) {
    proven_wsc_t *c = conn;
    if (c->dead) return;
    c->ws = (void *)0;
    wsc_enter(c);
    wsc_shut(c);
    wsc_leave(c);
}

static proven_size_t wsc_tr_buffered(const void *conn) {
    const proven_wsc_t *c = conn;
    return c->dead ? 0 : wsc_queued(c);
}

static const proven_ws_transport_t_ wsc_transport = { wsc_tr_send, wsc_tr_timer, wsc_tr_pause, wsc_tr_close, wsc_tr_kill, wsc_tr_buffered };

// -----------------------------------------------------------------------------
// Input
// -----------------------------------------------------------------------------

/* Input that is waiting, to the protocol: as much as it takes. */
static void wsc_deliver(proven_wsc_t *c) {
    while (!c->dead && c->ws && c->state == WSC_OPEN && !c->paused && wsc_pending(&c->in) > 0) {
        const proven_size_t n = wsc_pending(&c->in);
        const proven_size_t used = wsc_ops(c)->on_input(c->ws, (proven_mem_mut_t){ .ptr = c->in.p + c->in.off, .size = n });
        if (c->dead || !c->ws) return;
        c->in.off += used < n ? used : n;
        if (used < n) break;
    }
    if (!c->dead && c->in.off == c->in.len) c->in.off = c->in.len = 0;
}

/* The answer to the upgrade, when all of its head is here. */
static void wsc_answer(proven_wsc_t *c) {
    proven_http_header_t headers[64];
    proven_http_response_t head;
    proven_size_t head_size = 0;
    proven_err_t e = proven_http_parse_response((proven_mem_view_t){ .ptr = c->in.p, .size = c->in.len }, headers, 64, c->max_head, &head, &head_size);
    if (e == PROVEN_ERR_AGAIN) return;             /* not all of it yet */
    if (e != PROVEN_OK) { wsc_gone(c, PROVEN_ERR_INVALID_FORMAT); return; }
    /* Anything but 101 is the server saying no, in whatever words: nothing is followed and
     * nothing is answered. */
    if (head.status != 101) { wsc_gone(c, PROVEN_ERR_REFUSED); return; }
    proven_u8str_view_t chosen = { 0 };
    bool deflate = false;
    proven_ws_deflate_terms_t terms = { .server_window_bits = 15, .client_window_bits = 15 };
    const proven_u8str_view_t key = { .ptr = c->key, .size = sizeof c->key }, offered = { .ptr = c->protocols, .size = c->protocols_len };
    if (c->offered_peer_none) e = proven_ws_check_response_peer_no_takeover(&head, key, offered, &chosen, &deflate, &terms);
    else if (c->offered_takeover) e = proven_ws_check_response_takeover(&head, key, offered, &chosen, &deflate, &terms);
    else if (c->offered_deflate) e = proven_ws_check_response_deflate(&head, key, offered, &chosen, &deflate);
    else e = proven_ws_check_response(&head, key, offered, &chosen);
    if (e != PROVEN_OK) { wsc_gone(c, PROVEN_ERR_INVALID_FORMAT); return; }
    /* Open. The timer is the protocol's from here; what came behind the head is frames. */
    proven_byte_t name[256];
    const proven_size_t name_len = chosen.size < sizeof name ? chosen.size : sizeof name;
    for (proven_size_t i = 0; i < name_len; ++i) name[i] = chosen.ptr[i];
    proven_loop_timer_cancel(c->loop, &c->timer);
    c->state = WSC_OPEN;
    c->in.off = head_size;
    if (c->in.off == c->in.len) c->in.off = c->in.len = 0;
    proven_ws_stream_t *ws = c->ws;
    proven_ws_event_client_opened_(ws, deflate ? &terms : (void *)0);
    if (c->on_open && !c->dead && c->ws) c->on_open(c->ctx, ws, (proven_u8str_view_t){ .ptr = name, .size = name_len });
    if (!c->dead && c->ws) wsc_deliver(c);
}

/* Bytes in the clear, from the socket or out of TLS. */
static void wsc_plain(proven_wsc_t *c, proven_byte_t *p, proven_size_t n) {
    if (c->dead || n == 0) return;
    if (c->state == WSC_UPGRADE) {
        if (c->in.len + n > c->max_head + WSC_READ_BYTES) { wsc_gone(c, PROVEN_ERR_INVALID_FORMAT); return; }
        if (!wsc_append(c, &c->in, p, n)) { wsc_gone(c, PROVEN_ERR_NOMEM); return; }
        wsc_answer(c);
        return;
    }
    if (c->state != WSC_OPEN || !c->ws) return;    /* finishing: whatever the peer still says is not listened to */
    if (wsc_pending(&c->in) == 0 && !c->paused) {
        /* Straight from the read buffer, and only what is left over is kept. */
        const proven_size_t used = wsc_ops(c)->on_input(c->ws, (proven_mem_mut_t){ .ptr = p, .size = n });
        if (c->dead || !c->ws || used >= n) return;
        p += used; n -= used;
    }
    if (!wsc_append(c, &c->in, p, n)) wsc_gone(c, PROVEN_ERR_NOMEM);
}

static void wsc_send_request(proven_wsc_t *c) {
    c->state = WSC_UPGRADE;
    if (wsc_flush(c)) wsc_watch(c);
}

static void wsc_eof(proven_wsc_t *c) {
    c->peer_eof = true;
    if (c->state == WSC_FINISHING) { wsc_shut(c); return; }
    /* An end with no close frame - or before the upgrade was answered. */
    wsc_gone(c, c->state == WSC_OPEN ? PROVEN_ERR_EOF : PROVEN_ERR_RESET);
}

static void wsc_readable(proven_wsc_t *c) {
    for (int rounds = 0; rounds < 8 && !c->dead && !c->peer_eof; ++rounds) {
        if (c->state == WSC_OPEN && (c->paused || wsc_pending(&c->in) > 0)) break;
        proven_result_size_t rd = proven_loop_read(c->loop, &c->ls, (proven_mem_mut_t){ .ptr = c->rbuf, .size = WSC_READ_BYTES });
        if (rd.err == PROVEN_ERR_TIMEOUT || rd.err == PROVEN_ERR_AGAIN) break;
        if (rd.err == PROVEN_ERR_EOF) { wsc_eof(c); break; }
        if (rd.err != PROVEN_OK) { wsc_gone(c, rd.err == PROVEN_ERR_REFUSED || rd.err == PROVEN_ERR_UNREACHABLE ? rd.err : PROVEN_ERR_RESET); break; }
        if (!c->tls) { wsc_plain(c, c->rbuf, rd.value); continue; }
        proven_size_t at = 0;
        while (at < rd.value && !c->dead) {
            proven_size_t used = 0;
            proven_err_t e = proven_tls_feed(c->tls, (proven_mem_view_t){ .ptr = c->rbuf + at, .size = rd.value - at }, &used);
            at += used;
            if (e != PROVEN_OK) {
                (void)wsc_flush_tls(c);            /* the alert */
                wsc_gone(c, e);
                return;
            }
            if (c->state == WSC_HANDSHAKE && proven_tls_is_established(c->tls)) wsc_send_request(c);
            /* What it decrypted, into the second half of the read buffer. */
            while (!c->dead && c->state != WSC_HANDSHAKE) {
                if (c->state == WSC_OPEN && (c->paused || wsc_pending(&c->in) > 0)) {
                    /* The protocol is not taking: what TLS holds decrypted is moved to the
                     * queue, so that it is there when the protocol is. */
                    proven_result_size_t got = proven_tls_read(c->tls, (proven_mem_mut_t){ .ptr = c->rbuf + WSC_READ_BYTES, .size = WSC_READ_BYTES });
                    if (got.err == PROVEN_ERR_EOF) { wsc_eof(c); break; }
                    if (got.err != PROVEN_OK || got.value == 0) break;
                    if (!wsc_append(c, &c->in, c->rbuf + WSC_READ_BYTES, got.value)) { wsc_gone(c, PROVEN_ERR_NOMEM); break; }
                    continue;
                }
                proven_result_size_t got = proven_tls_read(c->tls, (proven_mem_mut_t){ .ptr = c->rbuf + WSC_READ_BYTES, .size = WSC_READ_BYTES });
                if (got.err == PROVEN_ERR_EOF) { wsc_eof(c); break; }
                if (got.err != PROVEN_OK || got.value == 0) break;
                wsc_plain(c, c->rbuf + WSC_READ_BYTES, got.value);
            }
            if (used == 0) break;
        }
    }
}

/* What reading made due - a pong, the answer to a close - leaves now; then the watch. */
static void wsc_after_io(proven_wsc_t *c) {
    if (c->dead || c->state == WSC_CONNECTING) return;
    const proven_size_t before = wsc_queued(c);
    if (before > 0 && wsc_flush(c) && c->ws && c->state == WSC_OPEN && wsc_queued(c) < before) wsc_ops(c)->on_progress(c->ws);
    if (!c->dead && c->state == WSC_FINISHING) wsc_finishing(c);
    else if (!c->dead) wsc_watch(c);
}

static void wsc_sock_connected(void *ctx, proven_err_t err) {
    proven_wsc_t *c = ctx;
    if (err == PROVEN_ERR_CANCELLED || c->dead || c->state != WSC_CONNECTING) return;
    wsc_enter(c);
    if (err == PROVEN_OK) {
        (void)proven_net_conn_set_nodelay(&c->sock, true);
        if (c->tls) { c->state = WSC_HANDSHAKE; if (wsc_flush_tls(c)) wsc_watch(c); }
        else wsc_send_request(c);
    } else {
        wsc_gone(c, err);
    }
    wsc_leave(c);
}

static void wsc_sock_received(void *ctx, proven_err_t err, proven_size_t n) {
    proven_wsc_t *c = ctx;
    (void)n;
    c->recv_armed = false;
    if (err == PROVEN_ERR_CANCELLED || c->dead) return;
    wsc_enter(c);
    if (wsc_reading(c)) wsc_readable(c);          /* reading was turned off since: it waits in the socket */
    wsc_after_io(c);
    wsc_leave(c);
}

static void wsc_sock_writable(void *ctx, proven_err_t err) {
    proven_wsc_t *c = ctx;
    if (err == PROVEN_ERR_CANCELLED || c->dead) return;
    wsc_enter(c);
    if (err != PROVEN_OK) wsc_gone(c, PROVEN_ERR_RESET);
    else {
        const proven_size_t before = wsc_queued(c);
        if (wsc_flush(c)) {
            if (c->state == WSC_FINISHING) wsc_finishing(c);
            else if (c->ws && c->state == WSC_OPEN && wsc_queued(c) < before) wsc_ops(c)->on_progress(c->ws);
        }
        wsc_after_io(c);
    }
    wsc_leave(c);
}

static void wsc_sock_closed(void *ctx) {
    proven_wsc_t *c = ctx;
    c->sock_gone = true;
    if (c->dead && c->depth == 0) wsc_free(c);
}

static const proven_loop_sock_fns_t wsc_sock_fns = { .received = wsc_sock_received, .writable = wsc_sock_writable, .connected = wsc_sock_connected, .closed = wsc_sock_closed };

static void wsc_on_timer(void *ctx) {
    proven_wsc_t *c = ctx;
    if (c->dead) return;
    wsc_enter(c);
    if (c->state == WSC_FINISHING) wsc_shut(c);                          /* the peer did not close: this side does */
    else if (c->state != WSC_OPEN) wsc_gone(c, PROVEN_ERR_TIMEOUT);      /* not connected, secured and upgraded in time */
    else if (c->ws) wsc_ops(c)->on_timer(c->ws);
    wsc_leave(c);
}

// -----------------------------------------------------------------------------
// Public
// -----------------------------------------------------------------------------

static bool wsc_scheme_is(proven_u8str_view_t s, const char *want, proven_size_t n) {
    if (s.size != n) return false;
    for (proven_size_t i = 0; i < n; ++i) if ((s.ptr[i] | 0x20) != (proven_byte_t)want[i]) return false;
    return true;
}

proven_err_t proven_ws_event_connect(proven_loop_t *loop, const proven_ws_event_connect_options_t *o, proven_ws_stream_t **out) {
    if (out) *out = (void *)0;
    if (!loop || !o || !out || !o->url.ptr || (o->header_count > 0 && !o->headers) || o->protocols.size > 255) return PROVEN_ERR_INVALID_ARG;
    proven_url_t u;
    if (proven_url_parse(o->url, &u) != PROVEN_OK || u.host.size == 0 || u.host.size > 255 || u.has_userinfo) return PROVEN_ERR_INVALID_ARG;
    const bool secure = wsc_scheme_is(u.scheme, "wss", 3);
    if (!secure && !wsc_scheme_is(u.scheme, "ws", 2)) return PROVEN_ERR_INVALID_ARG;
    if (secure && !o->tls) return PROVEN_ERR_INVALID_ARG;      /* never in the clear instead */
    const proven_u16 default_port = secure ? 443 : 80, port = u.has_port ? u.port : default_port;
    proven_net_addr_t to;
    if (o->address) to = *o->address;
    else if (proven_net_addr_parse(u.host, port, &to) != PROVEN_OK) return PROVEN_ERR_INVALID_ARG;

    proven_allocator_t alloc = proven_alloc_is_valid(o->alloc) ? o->alloc : proven_loop_allocator(loop);
    proven_result_mem_mut_t m = alloc.alloc_fn(alloc.ctx, sizeof(proven_wsc_t), 16);
    if (m.err != PROVEN_OK) return PROVEN_ERR_NOMEM;
    proven_wsc_t *c = (proven_wsc_t *)(void *)m.value.ptr;
    for (proven_size_t i = 0; i < sizeof *c; ++i) ((proven_byte_t *)c)[i] = 0;
    c->loop = loop;
    c->alloc = alloc;
    c->on_open = o->on_open;
    c->ctx = o->ws.ctx;
    c->max_head = o->max_head_bytes ? o->max_head_bytes : 16384;
    c->offered_deflate = o->ws.compress;
    c->offered_takeover = o->ws.compress && !o->ws.compress_no_takeover;
    c->offered_peer_none = c->offered_takeover && o->ws.compress_peer_no_takeover;
    c->protocols_len = o->protocols.size;
    for (proven_size_t i = 0; i < o->protocols.size; ++i) c->protocols[i] = o->protocols.ptr[i];
    c->depth = 1;                                  /* this call: wsc_leave at the end frees it if it died */
    proven_err_t e = PROVEN_OK;

    /* The request, whole, before anything is connected: what cannot be written is refused now. */
    proven_byte_t random[16];
    if (!proven_random_bytes(random, sizeof random)) e = PROVEN_ERR_IO;
    proven_ws_make_key(random, c->key);
    proven_size_t need = 512 + u.path.size + u.query.size + u.host.size + o->protocols.size;
    for (proven_size_t i = 0; i < o->header_count; ++i) need += o->headers[i].name.size + o->headers[i].value.size + 4;
    proven_result_mem_mut_t rq = e == PROVEN_OK ? alloc.alloc_fn(alloc.ctx, need, 1) : (proven_result_mem_mut_t){ .err = e };
    proven_result_mem_mut_t rb = e == PROVEN_OK ? alloc.alloc_fn(alloc.ctx, 2 * WSC_READ_BYTES, 1) : (proven_result_mem_mut_t){ .err = e };
    if (e == PROVEN_OK && (rq.err != PROVEN_OK || rb.err != PROVEN_OK)) e = PROVEN_ERR_NOMEM;
    if (rq.err == PROVEN_OK) { c->out.p = rq.value.ptr; c->out.cap = need; }
    if (rb.err == PROVEN_OK) c->rbuf = rb.value.ptr;
    if (e == PROVEN_OK) {
        proven_mem_mut_t text = { .ptr = c->out.p, .size = c->out.cap };
        proven_size_t len = 0;
        /* The target: the path, or "/" when the URL has none, and the query with it. */
        proven_byte_t *target = c->rbuf;           /* (free until the first read) */
        proven_size_t tl = 0;
        if (u.path.size + u.query.size + 2 > WSC_READ_BYTES) e = PROVEN_ERR_INVALID_ARG;
        else {
            if (u.path.size == 0) target[tl++] = '/';
            for (proven_size_t i = 0; i < u.path.size; ++i) target[tl++] = u.path.ptr[i];
            if (u.has_query) { target[tl++] = '?'; for (proven_size_t i = 0; i < u.query.size; ++i) target[tl++] = u.query.ptr[i]; }
        }
        if (e == PROVEN_OK) e = proven_http_write_request_line(text, &len, wsc_lit("GET"), (proven_u8str_view_t){ .ptr = target, .size = tl });
        /* Host: the port is written only when it is not the scheme's own. */
        proven_byte_t host[300];
        proven_size_t hl = 0;
        if (u.host_is_ipv6) host[hl++] = '[';
        for (proven_size_t i = 0; i < u.host.size; ++i) host[hl++] = u.host.ptr[i];
        if (u.host_is_ipv6) host[hl++] = ']';
        if (u.has_port && u.port != default_port) {
            proven_byte_t digits[5];
            int dn = 0;
            proven_u16 p = u.port;
            do { digits[dn++] = (proven_byte_t)('0' + p % 10); p /= 10; } while (p);
            host[hl++] = ':';
            while (dn > 0) host[hl++] = digits[--dn];
        }
        if (e == PROVEN_OK) e = proven_http_write_header(text, &len, wsc_lit("Host"), (proven_u8str_view_t){ .ptr = host, .size = hl });
        if (e == PROVEN_OK) e = proven_http_write_header(text, &len, wsc_lit("Upgrade"), wsc_lit("websocket"));
        if (e == PROVEN_OK) e = proven_http_write_header(text, &len, wsc_lit("Connection"), wsc_lit("Upgrade"));
        if (e == PROVEN_OK) e = proven_http_write_header(text, &len, wsc_lit("Sec-WebSocket-Key"), (proven_u8str_view_t){ .ptr = c->key, .size = sizeof c->key });
        if (e == PROVEN_OK) e = proven_http_write_header(text, &len, wsc_lit("Sec-WebSocket-Version"), wsc_lit("13"));
        if (e == PROVEN_OK && o->protocols.size > 0) e = proven_http_write_header(text, &len, wsc_lit("Sec-WebSocket-Protocol"), o->protocols);
        if (e == PROVEN_OK && o->ws.compress) {
            e = proven_http_write_header(text, &len, wsc_lit("Sec-WebSocket-Extensions"),
                                         c->offered_peer_none ? wsc_lit(PROVEN_WS_DEFLATE_OFFER_PEER_NO_TAKEOVER)
                                         : c->offered_takeover ? wsc_lit(PROVEN_WS_DEFLATE_OFFER_TAKEOVER) : wsc_lit(PROVEN_WS_DEFLATE_OFFER));
        }
        for (proven_size_t i = 0; i < o->header_count && e == PROVEN_OK; ++i) {
            /* The seven this call writes are not the caller's to write as well. */
            static const char *const mine[] = { "host", "upgrade", "connection", "sec-websocket-key", "sec-websocket-version", "sec-websocket-protocol", "sec-websocket-extensions" };
            for (proven_size_t k = 0; k < sizeof mine / sizeof mine[0] && e == PROVEN_OK; ++k) {
                proven_size_t n = 0;
                while (mine[k][n]) n++;
                bool same = o->headers[i].name.size == n;
                for (proven_size_t q = 0; same && q < n; ++q) same = (o->headers[i].name.ptr[q] | 0x20) == (proven_byte_t)mine[k][q];
                if (same) e = PROVEN_ERR_INVALID_ARG;
            }
            if (e == PROVEN_OK) e = proven_http_write_header(text, &len, o->headers[i].name, o->headers[i].value);
        }
        if (e == PROVEN_OK) e = proven_http_write_head_end(text, &len);
        if (e == PROVEN_ERR_OUT_OF_BOUNDS) e = PROVEN_ERR_INVALID_ARG;
        c->out.len = len;
    }
    proven_ws_stream_t *ws = (void *)0;
    if (e == PROVEN_OK) e = proven_ws_event_client_new_(alloc, &o->ws, &wsc_transport, c, to, &ws);
    if (e == PROVEN_OK && secure) e = proven_tls_client_create(o->tls, u.host, (void *)0, &c->tls);
    if (e == PROVEN_OK) {
        c->ws = ws;
        /* The loop makes the connection and says how it went from the loop, so that nothing is
         * reported from inside the call that began it. */
        e = proven_loop_connect(loop, &c->ls, to, &c->sock, &wsc_sock_fns, c);
        if (e == PROVEN_OK) proven_loop_send_limit_(&c->ls, (proven_size_t)-1);     /* WSC_HOLD bounds what it gives */
    }
    if (e != PROVEN_OK) {
        /* Not started: nothing is told, and nothing is left. */
        if (ws) proven_ws_event_client_discard_(ws);
        c->ws = (void *)0;
        if (c->tls) { proven_tls_conn_destroy(c->tls); c->tls = (void *)0; }
        if (proven_net_conn_is_open(&c->sock)) (void)proven_net_close(&c->sock);
        c->dead = true;
        wsc_leave(c);
        return e;
    }
    c->attached = true;
    c->state = WSC_CONNECTING;
    proven_loop_timer_set(loop, &c->timer, o->connect_timeout_ms ? o->connect_timeout_ms : 10000, wsc_on_timer, c);
    *out = ws;
    wsc_leave(c);
    return PROVEN_OK;
}

#endif /* !PROVEN_FREESTANDING && !PROVEN_NO_NET */
