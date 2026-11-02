#include "proven/ws_event.h"
#include "../../platform/proven_sys_mem.h"
#include "proven_internal_http_event.h"
#include "proven_internal_ws.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven/time.h"
#include "proven/random.h"

/*
 * WebSocket on a connection of the event-driven server.
 *
 * http_event.c still owns the connection - socket, TLS, output buffer, timer - and hands this
 * file the bytes that arrive (proven_internal_http_event.h). What is here is the protocol: the
 * decoder of ws.h for input, frames for output, and the three things a WebSocket connection
 * can be waiting for - a message, the answer to a ping, the answer to a close.
 *
 * Callbacks may call back in, close and abort included. So, as in http_event.c:
 *   - `conn` is null from the moment the connection is let go of or gone;
 *   - `closed` is set before on_closed is called, and nothing is delivered after it;
 *   - the stream is freed only when the outermost entry leaves (`depth`).
 */

#define WS_DEFAULT_MESSAGE ((proven_u64)1024 * 1024)
#define WS_DEFAULT_BUFFERED ((proven_size_t)64 * 1024)
#define WS_DEFAULT_PING_MS 30000u
#define WS_DEFAULT_PONG_MS 10000u
#define WS_DEFAULT_CLOSE_MS 5000u

/* A compressed message on its way in: made at its first frame, freed at its last. */
typedef struct {
    proven_ws_inflate_t_ m;
    proven_byte_t *held;               /* compressed bytes of a frame, set aside when the handler paused in the middle of it */
    proven_size_t held_len, held_at;
    bool have_held;                    /* there is such a remainder (it may be empty: output still to come) */
    bool held_last;                    /* it ends the message */
    bool started;                      /* a decoded piece has been delivered */
    bool running;                      /* ws_rx_run is on the stack */
    bool text;
    proven_byte_t piece[PROVEN_WS_DEFLATE_PIECE];
} proven_ws_rx_t;

struct proven_ws_stream {
    const proven_http_event_raw_ops_t *ops;      /* first: whatever carries the connection finds the operations here */
    const proven_ws_transport_t_ *tr;            /* and this is how the connection is reached: the server's record, or a client's */
    void *conn;                                  /* what `tr` works on */
    proven_allocator_t alloc;
    proven_ws_event_callbacks_t on;
    void *ctx;
    void *user;
    proven_u64 max_message;
    proven_size_t max_buffered;
    proven_size_t close_left;                    /* after a close was sent: output the client had not taken when last looked */
    proven_u32 ping_ms, pong_ms, close_ms;
    proven_u32 depth;
    proven_net_addr_t peer;
    proven_ws_decoder_t dec;
    proven_u16 close_code;                       /* the code this side sent */
    /* One bit each, so that the two this file gained for compression cost a connection
     * nothing. */
    bool closed : 1, close_sent : 1, in_message : 1, want_writable : 1, pinged : 1, paused : 1;
    bool deflate : 1;                            /* permessage-deflate was agreed: a proven_ws_z_t follows this struct */
    bool raw_paused : 1;                         /* the connection underneath was told to stop reading */
    bool plain : 1;                              /* proven_ws_stream_set_compress(false): messages that begin now go as they are */
    bool client : 1;                             /* this side asked for the connection: its frames are masked, the peer's are not */
    bool opening : 1;                            /* a client's, before the server has accepted the upgrade: nothing may be sent */
    bool may_deflate : 1;                        /* a client's that offered compression: room for a proven_ws_z_t follows this struct, used if the server agrees */
};

/* What a connection that agreed permessage-deflate holds besides: allocated behind the struct
 * above, so that a connection without the extension is not a byte larger for it. */
typedef struct {
    proven_ws_rx_t *rx;                                 /* a compressed message being received */
    proven_deflate_t *zip;                       /* a compressed message being sent in pieces; under takeover, every one */
    proven_inflate_t *kept;                      /* under the peer's takeover: the decompressor of every message */
    proven_byte_t zip_tail[4];
    proven_u8 zip_tail_len;
    proven_i8 level, window_bits;                /* a client's window is the configured one (0 too) until the server answers */
    bool zip_begun : 1;                          /* the first frame of the compressed message being sent has been queued */
    bool zipping : 1;                            /* the message being sent goes through `zip` */
    bool send_takeover : 1;                      /* this side keeps `zip` between messages */
    bool recv_takeover : 1;                      /* the peer may keep its window: `kept` reads every message */
    bool zip_off : 1;                            /* nothing is compressed from now on: the peer allows no window this side has, or a compressed message failed half-sent under takeover */
} proven_ws_z_t;

/* A client's generator of mask keys: behind the struct, after the room for a proven_ws_z_t if it
 * offered compression. A server's connection has neither, and is not a byte larger for them. */
static proven_chacha_rng_t *ws_masks(const proven_ws_stream_t *ws);

/* Only for a connection with `deflate` set. */
static proven_ws_z_t *ws_z(const proven_ws_stream_t *ws) { return (proven_ws_z_t *)(void *)(ws + 1); }
static proven_ws_rx_t *ws_rx(const proven_ws_stream_t *ws) { return ws->deflate ? ws_z(ws)->rx : (proven_ws_rx_t *)0; }
static proven_chacha_rng_t *ws_masks(const proven_ws_stream_t *ws) {
    return (proven_chacha_rng_t *)(void *)((proven_byte_t *)(ws + 1) + (ws->may_deflate ? sizeof(proven_ws_z_t) : 0));
}

static proven_u8str_view_t ws_lit_n(const char *s, proven_size_t n) { return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = n }; }
#define ws_lit(s) ws_lit_n((s), sizeof(s) - 1)

static void ws_enter(proven_ws_stream_t *ws) { ws->depth++; }
static void ws_rx_free(proven_ws_stream_t *ws) {
    proven_ws_rx_t *rx = ws_rx(ws);
    if (!rx) return;
    ws_z(ws)->rx = (void *)0;
    proven_ws_inflate_end_(&rx->m);
    if (rx->held) ws->alloc.free_fn(ws->alloc.ctx, rx->held);
    ws->alloc.free_fn(ws->alloc.ctx, rx);
}

/* What was agreed, onto a connection's compression state: who keeps a window, and this side's
 * window - the configured one (0 is 11 under takeover, 15 without), no larger than the peer
 * allows; smaller than this compressor's smallest, nothing is compressed. */
static void ws_z_agree(proven_ws_z_t *z, bool client, const proven_ws_deflate_terms_t *t) {
    z->send_takeover = client ? t->client_takeover : t->server_takeover;
    z->recv_takeover = client ? t->server_takeover : t->client_takeover;
    int limit = client ? t->client_window_bits : t->server_window_bits;
    int bits = z->window_bits != 0 ? z->window_bits : z->send_takeover ? PROVEN_WS_DEFLATE_TAKEOVER_BITS : 15;
    if (limit < bits) bits = limit;
    if (bits < 9) z->zip_off = true;
    else z->window_bits = (proven_i8)bits;
}

static void ws_leave(proven_ws_stream_t *ws) {
    if (--ws->depth > 0 || !ws->closed) return;
    ws_rx_free(ws);
    if (ws->deflate && ws_z(ws)->zip) proven_deflate_destroy(ws_z(ws)->zip);
    if (ws->deflate && ws_z(ws)->kept) proven_inflate_destroy(ws_z(ws)->kept);
    ws->alloc.free_fn(ws->alloc.ctx, ws);
}

/* The one place on_closed is called from. The connection has been let go of, or is gone. */
static void ws_finish(proven_ws_stream_t *ws, proven_u16 code, proven_err_t why) {
    if (ws->closed) return;
    ws->closed = true;
    ws->conn = (void *)0;
    ws_enter(ws);
    if (ws->on.on_closed) ws->on.on_closed(ws->ctx, ws, code, why);
    ws_leave(ws);
}

/* Let go of the connection and end. `polite` flushes what is queued and waits a moment for
 * the client to close; otherwise the socket is closed now. */
static void ws_end(proven_ws_stream_t *ws, bool polite, proven_u16 code, proven_err_t why) {
    void *conn = ws->conn;
    ws->conn = (void *)0;
    if (conn) {
        ws->tr->timer(conn, 0);
        if (polite) ws->tr->close(conn);
        else ws->tr->kill(conn);
    }
    ws_finish(ws, code, why);
}

/* Queue one frame. PROVEN_ERR_RESET when the connection is gone - on_closed has then been called. */
static proven_err_t ws_frame(proven_ws_stream_t *ws, proven_u8 opcode, bool fin, bool rsv1, proven_mem_view_t data) {
    if (!ws->conn) return PROVEN_ERR_RESET;
    proven_byte_t header[PROVEN_WS_MAX_FRAME_HEADER];
    proven_size_t len = 0;
    proven_ws_frame_t f = { .fin = fin, .opcode = opcode, .masked = ws->client, .length = data.size, .rsv1 = rsv1 };
    if (f.masked) proven_chacha_rng_fill(ws_masks(ws), f.mask, sizeof f.mask);
    proven_err_t e = proven_ws_frame_write((proven_mem_mut_t){ .ptr = header, .size = sizeof header }, &len, &f);
    if (e != PROVEN_OK) return e;
    if (!f.masked) return ws->tr->send(ws->conn, (proven_mem_view_t){ .ptr = header, .size = len }, data) ? PROVEN_OK : PROVEN_ERR_RESET;
    /* A client's payload is masked on its way: a piece at a time through a small buffer, the
     * header with the first. What is queued is queued whole or the connection is gone. */
    proven_byte_t piece[4096];
    proven_size_t done = 0;
    do {
        const proven_size_t n = data.size - done < sizeof piece ? data.size - done : sizeof piece;
        proven_sys_mem_copy(piece, data.ptr + done, n);
        proven_ws_mask((proven_mem_mut_t){ .ptr = piece, .size = n }, f.mask, done);
        if (!ws->conn || !ws->tr->send(ws->conn, (proven_mem_view_t){ .ptr = header, .size = done == 0 ? len : 0 }, (proven_mem_view_t){ .ptr = piece, .size = n })) return PROVEN_ERR_RESET;
        done += n;
    } while (done < data.size);
    return PROVEN_OK;
}

static void ws_send_close(proven_ws_stream_t *ws, proven_u16 code, proven_u8str_view_t reason) {
    proven_byte_t payload[PROVEN_WS_MAX_CONTROL];
    proven_size_t len = 0;
    proven_mem_mut_t out = { .ptr = payload, .size = sizeof payload };
    if (code != PROVEN_WS_CLOSE_NO_STATUS && proven_ws_close_write(out, &len, code, reason) != PROVEN_OK) {
        len = 0;
        if (proven_ws_close_write(out, &len, code, ws_lit("")) != PROVEN_OK) len = 0;
    }
    ws->close_sent = true;
    ws->close_code = code;
    if (ws_frame(ws, PROVEN_WS_CLOSE, true, false, (proven_mem_view_t){ .ptr = payload, .size = len }) == PROVEN_OK && ws->conn) {
        ws->close_left = ws->tr->buffered(ws->conn);
    }
}

/* The client broke the protocol, or sent more than is accepted: say so and end. */
static void ws_fail(proven_ws_stream_t *ws, proven_err_t why) {
    proven_u16 code = proven_ws_close_code_for(why);
    if (!ws->close_sent) ws_send_close(ws, code, ws_lit(""));
    if (!ws->closed) ws_end(ws, true, code, why);
}

static void ws_arm(proven_ws_stream_t *ws) {
    if (!ws->conn) return;
    ws->tr->timer(ws->conn, ws->close_sent ? ws->close_ms : ws->pinged ? ws->pong_ms : ws->ping_ms);
}

// -----------------------------------------------------------------------------
// A compressed message, inflated on its way to on_message
// -----------------------------------------------------------------------------

/*
 * Inflate `n` compressed bytes of the open message and deliver what they become, a piece at a
 * time. `*consumed` counts the bytes taken. Returns true when these bytes are done with -
 * all taken, and all they had to give delivered (the whole message, when `last`) - or the
 * connection is over; false when the handler paused first: the caller keeps the rest.
 */
static bool ws_rx_run(proven_ws_stream_t *ws, const proven_byte_t *p, proven_size_t n, bool last, proven_size_t *consumed) {
    proven_ws_rx_t *rx = ws_rx(ws);
    *consumed = 0;
    rx->running = true;
    bool finished = true;
    for (;;) {
        if (ws->closed || !ws->conn) break;
        if (ws->paused) { finished = false; break; }
        proven_size_t used = 0, made = 0;
        bool done = false;
        proven_err_t e = proven_ws_inflate_step_(&rx->m, (proven_mem_view_t){ .ptr = p + *consumed, .size = n - *consumed }, &used, last,
                                                 (proven_mem_mut_t){ .ptr = rx->piece, .size = sizeof rx->piece }, &made, ws->max_message, &done);
        *consumed += used;
        /* Bytes that are not DEFLATE are answered as text that is not text is: the payload is
         * not what its frame says. And a last piece that leaves the decompressor waiting
         * cannot happen - but it must not spin if it does. */
        if (e == PROVEN_ERR_INVALID_FORMAT || (e == PROVEN_OK && used == 0 && made == 0 && !done && last && *consumed == n)) e = PROVEN_ERR_INVALID_ENCODING;
        if (e != PROVEN_OK) { ws_fail(ws, e); break; }
        if (made > 0 || done) {
            bool first = !rx->started;
            rx->started = true;
            if (!ws->close_sent) ws->on.on_message(ws->ctx, ws, rx->text, (proven_mem_view_t){ .ptr = rx->piece, .size = made }, first, done);
            if (done) {
                /* The handler may have ended the connection, but not this: it is freed here. */
                rx->running = false;
                ws_rx_free(ws);
                return true;
            }
            continue;
        }
        if (used == 0 && *consumed == n) break;    /* a frame in the middle of the message, used up */
    }
    rx->running = false;
    return finished;
}

/* The handler paused in the middle of a frame: set what is left of it aside. The decoder has
 * moved past these bytes, so the connection underneath is told they were used. */
static bool ws_rx_hold(proven_ws_stream_t *ws, const proven_byte_t *p, proven_size_t n, bool last) {
    proven_ws_rx_t *rx = ws_rx(ws);
    if (rx->held) { ws->alloc.free_fn(ws->alloc.ctx, rx->held); rx->held = (void *)0; }
    rx->held_len = n;
    rx->held_at = 0;
    rx->held_last = last;
    rx->have_held = true;
    if (n == 0) return true;
    proven_result_mem_mut_t m = ws->alloc.alloc_fn(ws->alloc.ctx, n, 1);
    if (m.err != PROVEN_OK) return false;
    rx->held = m.value.ptr;
    proven_sys_mem_copy(rx->held, p, n);
    return true;
}

// -----------------------------------------------------------------------------
// What http_event.c calls
// -----------------------------------------------------------------------------

static proven_size_t ws_on_input(void *raw, proven_mem_mut_t in) {
    proven_ws_stream_t *ws = raw;
    proven_size_t used = 0;
    ws_enter(ws);
    /* Anything at all from the client says it is there - until this side has closed: after
     * that only its answer counts, and a client that keeps talking does not buy more time. */
    if (!ws->close_sent) {
        ws->pinged = false;
        ws_arm(ws);
    }
    while (used < in.size && !ws->closed && !ws->paused && ws->conn) {
        proven_size_t n = 0;
        proven_ws_event_t ev = { 0 };
        proven_err_t e = proven_ws_decoder_feed(&ws->dec, (proven_mem_mut_t){ .ptr = in.ptr + used, .size = in.size - used }, &n, &ev);
        used += n;
        if (e == PROVEN_ERR_EOF) { used = in.size; break; }          /* bytes after a close frame: nothing to do with them */
        if (e != PROVEN_OK) { ws_fail(ws, e); break; }
        if (ev.kind == PROVEN_WS_EVENT_NONE) break;
        if (ev.kind == PROVEN_WS_EVENT_DATA && ev.compressed) {
            if (ev.first) {
                ws_rx_free(ws);
                if (!ws->close_sent) {
                    proven_result_mem_mut_t m = ws->alloc.alloc_fn(ws->alloc.ctx, sizeof(proven_ws_rx_t), alignof(proven_ws_rx_t));
                    if (m.err != PROVEN_OK) { ws_fail(ws, PROVEN_ERR_NOMEM); break; }
                    proven_ws_rx_t *rx = (proven_ws_rx_t *)(void *)m.value.ptr;
                    rx->held = (void *)0;
                    rx->held_len = 0; rx->held_at = 0;
                    rx->have_held = false; rx->held_last = false; rx->started = false; rx->running = false;
                    rx->text = ev.text;
                    proven_ws_z_t *z = ws_z(ws);
                    if (proven_ws_inflate_begin_(&rx->m, ws->alloc, ev.text, z->recv_takeover ? &z->kept : (void *)0) != PROVEN_OK) {
                        ws->alloc.free_fn(ws->alloc.ctx, rx);
                        ws_fail(ws, PROVEN_ERR_NOMEM);
                        break;
                    }
                    ws_z(ws)->rx = rx;
                }
            }
            if (ws_rx(ws)) {
                proven_size_t took = 0;
                if (!ws_rx_run(ws, ev.data.ptr, ev.data.size, ev.last, &took) && !ws->closed && ws_rx(ws)) {
                    if (!ws_rx_hold(ws, ev.data.ptr + took, ev.data.size - took, ev.last)) { ws_fail(ws, PROVEN_ERR_NOMEM); break; }
                }
            }
        } else if (ev.kind == PROVEN_WS_EVENT_DATA) {
            if (!ws->close_sent) ws->on.on_message(ws->ctx, ws, ev.text, ev.data, ev.first, ev.last);
        } else if (ev.kind == PROVEN_WS_EVENT_PING) {
            if (!ws->close_sent) (void)ws_frame(ws, PROVEN_WS_PONG, true, false, ev.data);
        } else if (ev.kind == PROVEN_WS_EVENT_CLOSE) {
            if (ws->close_sent) {
                /* The answer to this side's close: the handshake is complete. */
                ws_end(ws, true, ws->close_code, PROVEN_OK);
            } else {
                proven_u16 code = ev.close_code;
                ws_send_close(ws, code, ws_lit(""));
                if (!ws->closed) ws_end(ws, true, code, PROVEN_OK);
            }
            break;
        }
    }
    /* A handler that paused and resumed inside one call left the connection underneath
     * stopped: nothing was resumed while a message was being delivered. */
    if (ws->raw_paused && !ws->paused && !ws->closed && ws->conn && !(ws_rx(ws) && ws_rx(ws)->have_held)) {
        ws->raw_paused = false;
        ws->tr->pause(ws->conn, false);
    }
    ws_leave(ws);
    return used;
}

static void ws_on_progress(void *raw) {
    proven_ws_stream_t *ws = raw;
    if (ws->closed || !ws->conn) return;
    /* A close waits behind whatever was queued before it: while the client is still taking
     * that, it is not late with its answer. Only output that actually went counts - this is
     * also called when nothing moved. */
    if (ws->close_sent) {
        proven_size_t left = ws->tr->buffered(ws->conn);
        if (left < ws->close_left) { ws->close_left = left; ws_arm(ws); }
        return;
    }
    if (!ws->want_writable) return;
    if (ws->tr->buffered(ws->conn) >= ws->max_buffered) return;
    ws->want_writable = false;
    ws_enter(ws);
    if (ws->on.on_writable) ws->on.on_writable(ws->ctx, ws);
    ws_leave(ws);
}

static void ws_on_timer(void *raw) {
    proven_ws_stream_t *ws = raw;
    if (ws->closed || !ws->conn) return;
    ws_enter(ws);
    if (ws->close_sent) {
        /* The client never answered the close. */
        ws_end(ws, false, ws->close_code, PROVEN_ERR_TIMEOUT);
    } else if (ws->paused) {
        /* The program is not reading; silence proves nothing. */
        ws_arm(ws);
    } else if (!ws->pinged) {
        ws->pinged = true;
        if (ws_frame(ws, PROVEN_WS_PING, true, false, (proven_mem_view_t){ 0 }) == PROVEN_OK) ws_arm(ws);
    } else {
        ws_end(ws, false, PROVEN_WS_CLOSE_ABNORMAL, PROVEN_ERR_TIMEOUT);
    }
    ws_leave(ws);
}

static void ws_on_closed(void *raw, proven_err_t why) {
    proven_ws_stream_t *ws = raw;
    /* An end of stream with no close frame is a client that vanished, whatever it meant. */
    if (why == PROVEN_ERR_EOF || why == PROVEN_OK) why = PROVEN_ERR_RESET;
    ws_finish(ws, ws->close_sent ? ws->close_code : PROVEN_WS_CLOSE_ABNORMAL, why);
}

/* The server is ending: goodbye with 1001, and the answer is waited for as after any close
 * (close_ms, or the server's deadline, whichever comes first). */
static void ws_on_drain(void *raw) {
    proven_ws_stream_close((proven_ws_stream_t *)raw, PROVEN_WS_CLOSE_GOING_AWAY, PROVEN_LIT("going away"));
}

static const proven_http_event_raw_ops_t ws_ops = {
    .on_input = ws_on_input, .on_progress = ws_on_progress, .on_timer = ws_on_timer, .on_closed = ws_on_closed,
    .on_drain = ws_on_drain,
};

/* The server's connection record, as a transport. */
static bool ws_sv_send(void *conn, proven_mem_view_t a, proven_mem_view_t b) { return proven_http_event_raw_send_(conn, a, b); }
static void ws_sv_timer(void *conn, proven_u32 ms) { proven_http_event_raw_timer_(conn, ms); }
static void ws_sv_pause(void *conn, bool paused) { proven_http_event_raw_pause_(conn, paused); }
static void ws_sv_close(void *conn) { proven_http_event_raw_close_(conn); }
static void ws_sv_kill(void *conn) { proven_http_event_raw_kill_(conn); }
static proven_size_t ws_sv_buffered(const void *conn) { return proven_http_stream_buffered(conn); }
static const proven_ws_transport_t_ ws_sv_transport = { ws_sv_send, ws_sv_timer, ws_sv_pause, ws_sv_close, ws_sv_kill, ws_sv_buffered };

// -----------------------------------------------------------------------------
// Public
// -----------------------------------------------------------------------------

static bool ws_is_token(proven_u8str_view_t s) {
    for (proven_size_t i = 0; i < s.size; ++i) {
        proven_byte_t c = s.ptr[i];
        bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        bool mark = c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' ||
                    c == '-' || c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
        if (!alnum && !mark) return false;
    }
    return true;
}

proven_err_t proven_ws_event_accept(proven_http_stream_t *stream, const proven_http_request_t *head,
                                    const proven_ws_event_config_t *config, proven_ws_stream_t **out) {
    if (out) *out = (void *)0;
    if (!stream || !head || !config || !out || !config->on.on_message || !ws_is_token(config->subprotocol)) return PROVEN_ERR_INVALID_ARG;
    if (config->compress_level < -1 || config->compress_level > 9 ||
        (config->compress_window_bits != 0 && (config->compress_window_bits < 9 || config->compress_window_bits > 15))) return PROVEN_ERR_INVALID_ARG;
    if (PROVEN_HTTP_EVENT_IS_H3_(stream)) return PROVEN_ERR_INVALID_STATE;        /* WebSocket over HTTP/3 (RFC 9220) is not taken */
    proven_u8str_view_t key;
    proven_err_t e = proven_ws_check_request(head, &key);
    if (e != PROVEN_OK) return e;
    /* Over HTTP/2 there is no key, and nothing to compute from it (RFC 8441, section 5). */
    const bool h2 = PROVEN_HTTP_EVENT_IS_H2_(stream);
    proven_byte_t accept[PROVEN_WS_ACCEPT_SIZE];
    if (!h2) {
        e = proven_ws_accept_key(key, accept);
        if (e != PROVEN_OK) return e;
    }

    /* Compression, when this side wants it and the client offered a form of it that can be met. */
    proven_byte_t extension[PROVEN_WS_DEFLATE_ANSWER_MAX];
    proven_size_t extension_len = 0;
    proven_ws_deflate_terms_t terms = { 0 };
    proven_mem_mut_t room = { .ptr = extension, .size = sizeof extension };
    bool deflate = false;
    if (config->compress && config->compress_no_takeover) {
        int window_bits = 0;
        deflate = proven_ws_deflate_accept(head, room, &extension_len, &window_bits) == PROVEN_OK;
        terms = (proven_ws_deflate_terms_t){ .server_window_bits = window_bits, .client_window_bits = 15 };
    } else if (config->compress) {
        deflate = (config->compress_peer_no_takeover ? proven_ws_deflate_accept_peer_no_takeover(head, room, &extension_len, &terms)
                                                     : proven_ws_deflate_accept_takeover(head, room, &extension_len, &terms)) == PROVEN_OK;
    }

    /* Over HTTP/2 (RFC 8441) the answer is 200 with the same two optional fields, and no
     * Upgrade, Connection or Sec-WebSocket-Accept: the stream is the connection. */
    proven_http_header_t h2_fields[2];
    proven_size_t h2_count = 0;
    if (h2) {
        if (config->subprotocol.size > 0) h2_fields[h2_count++] = (proven_http_header_t){ .name = ws_lit("sec-websocket-protocol"), .value = config->subprotocol };
        if (deflate) h2_fields[h2_count++] = (proven_http_header_t){ .name = ws_lit("sec-websocket-extensions"), .value = { .ptr = extension, .size = extension_len } };
    }

    /* Over HTTP/1.1, the 101 as text. */
    proven_byte_t text[640];
    proven_mem_mut_t answer = { .ptr = text, .size = sizeof text };
    proven_size_t len = 0;
    if (!h2) {
        e = proven_http_write_status_line(answer, &len, 101, ws_lit(""));
        proven_byte_t date[PROVEN_HTTP_DATE_SIZE];
        if (e == PROVEN_OK && proven_http_date_format(proven_time_now(), date) == PROVEN_OK) {
            e = proven_http_write_header(answer, &len, ws_lit("Date"), (proven_u8str_view_t){ .ptr = date, .size = sizeof date });
        }
        if (e == PROVEN_OK) e = proven_http_write_header(answer, &len, ws_lit("Upgrade"), ws_lit("websocket"));
        if (e == PROVEN_OK) e = proven_http_write_header(answer, &len, ws_lit("Connection"), ws_lit("Upgrade"));
        if (e == PROVEN_OK) e = proven_http_write_header(answer, &len, ws_lit("Sec-WebSocket-Accept"), (proven_u8str_view_t){ .ptr = accept, .size = sizeof accept });
        if (e == PROVEN_OK && config->subprotocol.size > 0) e = proven_http_write_header(answer, &len, ws_lit("Sec-WebSocket-Protocol"), config->subprotocol);
        if (e == PROVEN_OK && deflate) e = proven_http_write_header(answer, &len, ws_lit("Sec-WebSocket-Extensions"), (proven_u8str_view_t){ .ptr = extension, .size = extension_len });
        if (e == PROVEN_OK) e = proven_http_write_head_end(answer, &len);
        if (e != PROVEN_OK) return e == PROVEN_ERR_OUT_OF_BOUNDS ? PROVEN_ERR_INVALID_ARG : e;      /* a subprotocol too long to name */
    }

    proven_allocator_t alloc = proven_http_event_raw_allocator_(stream);
    proven_result_mem_mut_t m = alloc.alloc_fn(alloc.ctx, sizeof(proven_ws_stream_t) + (deflate ? sizeof(proven_ws_z_t) : 0), 16);
    if (m.err != PROVEN_OK) return PROVEN_ERR_NOMEM;
    proven_ws_stream_t *ws = (proven_ws_stream_t *)(void *)m.value.ptr;
    *ws = (proven_ws_stream_t){
        .ops = &ws_ops, .tr = &ws_sv_transport, .alloc = alloc, .on = config->on, .ctx = config->ctx,
        .max_message = config->max_message_bytes ? config->max_message_bytes : WS_DEFAULT_MESSAGE,
        .max_buffered = config->max_buffered_output ? config->max_buffered_output : WS_DEFAULT_BUFFERED,
        .ping_ms = config->ping_interval_ms ? config->ping_interval_ms : WS_DEFAULT_PING_MS,
        .pong_ms = config->pong_timeout_ms ? config->pong_timeout_ms : WS_DEFAULT_PONG_MS,
        .close_ms = config->close_timeout_ms ? config->close_timeout_ms : WS_DEFAULT_CLOSE_MS,
        .peer = proven_http_stream_peer(stream),
        .deflate = deflate,
    };
    if (deflate) {
        *ws_z(ws) = (proven_ws_z_t){ .level = (proven_i8)config->compress_level, .window_bits = (proven_i8)config->compress_window_bits };
        ws_z_agree(ws_z(ws), false, &terms);
    }
    proven_ws_decoder_init(&ws->dec, true, ws->max_message);
    if (deflate) proven_ws_decoder_allow_compressed(&ws->dec);

    e = h2 ? proven_http_event_h2_raw_begin_(stream, ws, h2_fields, h2_count)
           : proven_http_event_raw_begin_(stream, ws, (proven_mem_view_t){ .ptr = text, .size = len });
    if (e != PROVEN_OK) { alloc.free_fn(alloc.ctx, ws); return e; }
    ws->conn = stream;
    ws_arm(ws);
    *out = ws;
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// A connection this side makes (ws_event_client.c is its transport)
// -----------------------------------------------------------------------------

proven_err_t proven_ws_event_client_new_(proven_allocator_t alloc, const proven_ws_event_config_t *config, const proven_ws_transport_t_ *transport,
                                         void *conn, proven_net_addr_t peer, proven_ws_stream_t **out) {
    *out = (void *)0;
    /* (`subprotocol` is a server's answer; a client's offer is in the options. Set here it
     * would do nothing, which is a mistake to be told about and not a wish to be ignored.) */
    if (!config || !config->on.on_message || config->subprotocol.size != 0) return PROVEN_ERR_INVALID_ARG;
    if (config->compress_level < -1 || config->compress_level > 9 ||
        (config->compress_window_bits != 0 && (config->compress_window_bits < 9 || config->compress_window_bits > 15))) return PROVEN_ERR_INVALID_ARG;
    proven_byte_t seed[PROVEN_CHACHA_SEED_SIZE];
    if (!proven_random_bytes(seed, sizeof seed)) return PROVEN_ERR_IO;
    /* Behind the struct: room for the compression state, if it was offered, and the
     * generator of mask keys - neither of which a server's connection carries. */
    const proven_size_t z_bytes = config->compress ? sizeof(proven_ws_z_t) : 0;
    proven_result_mem_mut_t m = alloc.alloc_fn(alloc.ctx, sizeof(proven_ws_stream_t) + z_bytes + sizeof(proven_chacha_rng_t), 16);
    if (m.err != PROVEN_OK) return PROVEN_ERR_NOMEM;
    proven_ws_stream_t *ws = (proven_ws_stream_t *)(void *)m.value.ptr;
    *ws = (proven_ws_stream_t){
        .ops = &ws_ops, .tr = transport, .conn = conn, .alloc = alloc, .on = config->on, .ctx = config->ctx,
        .max_message = config->max_message_bytes ? config->max_message_bytes : WS_DEFAULT_MESSAGE,
        .max_buffered = config->max_buffered_output ? config->max_buffered_output : WS_DEFAULT_BUFFERED,
        .ping_ms = config->ping_interval_ms ? config->ping_interval_ms : WS_DEFAULT_PING_MS,
        .pong_ms = config->pong_timeout_ms ? config->pong_timeout_ms : WS_DEFAULT_PONG_MS,
        .close_ms = config->close_timeout_ms ? config->close_timeout_ms : WS_DEFAULT_CLOSE_MS,
        .peer = peer, .client = true, .opening = true, .may_deflate = config->compress,
    };
    if (config->compress) {
        *ws_z(ws) = (proven_ws_z_t){ .level = (proven_i8)config->compress_level, .window_bits = (proven_i8)config->compress_window_bits };
    }
    proven_chacha_rng_seed(ws_masks(ws), seed);
    proven_ws_decoder_init(&ws->dec, false, ws->max_message);
    *out = ws;
    return PROVEN_OK;
}

void proven_ws_event_client_opened_(proven_ws_stream_t *ws, const proven_ws_deflate_terms_t *terms) {
    ws->opening = false;
    if (terms && ws->may_deflate) {
        ws->deflate = true;
        ws_z_agree(ws_z(ws), true, terms);
        proven_ws_decoder_allow_compressed(&ws->dec);
    }
    ws_arm(ws);
}

void proven_ws_event_client_failed_(proven_ws_stream_t *ws, proven_err_t why) {
    ws_finish(ws, 0, why);
}

void proven_ws_event_client_discard_(proven_ws_stream_t *ws) {
    ws->alloc.free_fn(ws->alloc.ctx, ws);
}

/* Whether a data or ping frame may be queued now. */
static proven_err_t ws_may_send(proven_ws_stream_t *ws) {
    if (!ws) return PROVEN_ERR_INVALID_ARG;
    if (ws->closed || !ws->conn) return PROVEN_ERR_RESET;
    if (ws->close_sent || ws->opening) return PROVEN_ERR_INVALID_STATE;
    if (ws->tr->buffered(ws->conn) >= ws->max_buffered) { ws->want_writable = true; return PROVEN_ERR_AGAIN; }
    return PROVEN_OK;
}

proven_err_t proven_ws_stream_send_piece(proven_ws_stream_t *ws, bool text, proven_mem_view_t data, bool first, bool last) {
    if (ws && !ws->closed && ws->conn && !ws->close_sent && first == ws->in_message) return PROVEN_ERR_INVALID_STATE;
    if (data.size > 0 && !data.ptr) return PROVEN_ERR_INVALID_ARG;
    proven_err_t e = ws_may_send(ws);
    if (e != PROVEN_OK) return e;
    ws_enter(ws);
    proven_ws_z_t *z = ws->deflate ? ws_z(ws) : (proven_ws_z_t *)0;
    if (z && first) {
        /* A message begins: is it compressed? Not when it is whole and too short to gain, and
         * not when there is no memory for a compressor - then it goes as it is (and, under
         * takeover, the next message tries again: nothing went into a window). */
        if (z->zip && !z->send_takeover) { proven_deflate_destroy(z->zip); z->zip = (void *)0; }
        bool worth = !ws->plain && !z->zip_off &&
                     !(last && data.size < (z->send_takeover ? PROVEN_WS_DEFLATE_MIN_TAKEOVER : PROVEN_WS_DEFLATE_MIN));
        if (worth && !z->zip &&
            proven_ws_deflate_begin_(ws->alloc, z->level, z->window_bits, last && !z->send_takeover ? data.size : 0, &z->zip) != PROVEN_OK) z->zip = (void *)0;
        z->zipping = worth && z->zip;
        z->zip_tail_len = 0;
        z->zip_begun = false;
    }
    if (!z || !z->zipping) {
        e = ws_frame(ws, first ? (text ? PROVEN_WS_TEXT : PROVEN_WS_BINARY) : PROVEN_WS_CONTINUATION, last, false, data);
    } else {
        /* Compressed a buffer at a time, each buffer queued as a frame: nothing the size of
         * the message is allocated, and the queue holds what it held - the limit and one
         * message. Every piece is flushed, so that the client can read it when it arrives. */
        for (;;) {
            proven_byte_t piece[4096];
            proven_size_t used = 0, made = 0;
            bool done = false;
            e = proven_ws_deflate_step_(z->zip, data, &used, true, last, (proven_mem_mut_t){ .ptr = piece, .size = sizeof piece }, &made,
                                        z->zip_tail, &z->zip_tail_len, &done);
            if (e != PROVEN_OK) break;
            data.ptr += used;
            data.size -= used;
            if (made > 0 || (done && last)) {
                proven_u8 kind = (proven_u8)(z->zip_begun ? PROVEN_WS_CONTINUATION : (text ? PROVEN_WS_TEXT : PROVEN_WS_BINARY));
                e = ws_frame(ws, kind, done && last, !z->zip_begun, (proven_mem_view_t){ .ptr = piece, .size = made });
                if (e != PROVEN_OK) break;
                z->zip_begun = true;
            }
            if (done) break;
        }
        if (e != PROVEN_OK || last) {
            /* Under takeover the compressor is the window both sides share; one that failed in
             * the middle of a message no longer matches the client's, so nothing more is
             * compressed. */
            if (e != PROVEN_OK && z->send_takeover) z->zip_off = true;
            if (z->zip && (e != PROVEN_OK || !z->send_takeover)) {
                proven_deflate_destroy(z->zip);
                z->zip = (void *)0;
            }
            z->zipping = false;
        }
    }
    if (e == PROVEN_OK) ws->in_message = !last;
    ws_leave(ws);
    return e;
}

proven_err_t proven_ws_stream_send(proven_ws_stream_t *ws, bool text, proven_mem_view_t data) {
    return proven_ws_stream_send_piece(ws, text, data, true, true);
}

proven_err_t proven_ws_stream_ping(proven_ws_stream_t *ws, proven_mem_view_t data) {
    if (data.size > PROVEN_WS_MAX_CONTROL) return PROVEN_ERR_OUT_OF_BOUNDS;
    if (data.size > 0 && !data.ptr) return PROVEN_ERR_INVALID_ARG;
    proven_err_t e = ws_may_send(ws);
    if (e != PROVEN_OK) return e;
    ws_enter(ws);
    e = ws_frame(ws, PROVEN_WS_PING, true, false, data);
    ws_leave(ws);
    return e;
}

void proven_ws_stream_close(proven_ws_stream_t *ws, proven_u16 code, proven_u8str_view_t reason) {
    if (!ws || ws->closed || !ws->conn || ws->close_sent) return;
    /* A client's that is not open yet has nobody to say goodbye to: it is given up. */
    if (ws->opening) { proven_ws_stream_abort(ws); return; }
    if (!proven_ws_close_code_is_valid(code)) code = PROVEN_WS_CLOSE_NORMAL;
    ws_enter(ws);
    ws_send_close(ws, code, reason);
    ws_arm(ws);
    ws_leave(ws);
}

void proven_ws_stream_abort(proven_ws_stream_t *ws) {
    if (!ws || ws->closed) return;
    ws_enter(ws);
    ws_end(ws, false, PROVEN_WS_CLOSE_ABNORMAL, PROVEN_ERR_RESET);
    ws_leave(ws);
}

void proven_ws_stream_pause(proven_ws_stream_t *ws) {
    if (!ws || ws->closed || !ws->conn || ws->paused) return;
    ws->paused = true;
    if (!ws->raw_paused) {
        ws->raw_paused = true;
        ws->tr->pause(ws->conn, true);
    }
}

void proven_ws_stream_resume(proven_ws_stream_t *ws) {
    if (!ws || ws->closed || !ws->conn || !ws->paused) return;
    ws->paused = false;
    /* Called from inside the delivery of a compressed message: that delivery goes on by
     * itself, and whoever began it lets the connection read again when it is done. */
    if (ws_rx(ws) && ws_rx(ws)->running) return;
    ws_enter(ws);
    /* What was set aside when the handler paused comes first: it is older than anything the
     * connection has still to give. */
    if (ws_rx(ws) && ws_rx(ws)->have_held) {
        proven_ws_rx_t *rx = ws_rx(ws);
        proven_size_t took = 0;
        bool finished = ws_rx_run(ws, rx->held + rx->held_at, rx->held_len - rx->held_at, rx->held_last, &took);
        if (ws_rx(ws) == rx) {
            rx->held_at += took;
            if (finished) {
                rx->have_held = false;
                if (rx->held) { ws->alloc.free_fn(ws->alloc.ctx, rx->held); rx->held = (void *)0; }
            }
        }
    }
    if (!ws->closed && ws->conn && !ws->paused && ws->raw_paused && !(ws_rx(ws) && ws_rx(ws)->have_held)) {
        ws->raw_paused = false;
        ws->tr->pause(ws->conn, false);
    }
    ws_leave(ws);
}

bool proven_ws_stream_compressed(const proven_ws_stream_t *ws) { return ws && ws->deflate; }

void proven_ws_stream_set_compress(proven_ws_stream_t *ws, bool on) {
    if (ws) ws->plain = !on;
}

void proven_ws_stream_set_user(proven_ws_stream_t *ws, void *user) { if (ws) ws->user = user; }
void *proven_ws_stream_user(const proven_ws_stream_t *ws) { return ws ? ws->user : (void *)0; }
proven_net_addr_t proven_ws_stream_peer(const proven_ws_stream_t *ws) { return ws ? ws->peer : (proven_net_addr_t){ 0 }; }
proven_size_t proven_ws_stream_buffered(const proven_ws_stream_t *ws) { return ws && ws->conn ? ws->tr->buffered(ws->conn) : 0; }

#endif
