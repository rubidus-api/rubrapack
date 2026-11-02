#include "proven/ws_conn.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven/random.h"
#include "proven/utf.h"
#include "proven/url.h"
#include "proven_internal_ws.h"

/*
 * The WebSocket connection driver. The frames are ws.c's business and the opening handshake's
 * HTTP is the client's and the server's; what is here is the sequence: read, feed the decoder,
 * act on what it reports, and frame what is sent.
 */

#define WC_READ_SIZE 16384u
#define WC_SEND_SIZE 4096u
#define WC_DEFAULT_MAX_MESSAGE (1024u * 1024u)
#define WC_LINGER_MS 1000u             /* how long a refused peer's remaining bytes are read away */
#define WC_DEFAULT_PING_MS 30000u      /* as ws_event's */
#define WC_DEFAULT_PONG_MS 10000u

struct proven_ws_conn {
    proven_ws_conn_config_t cfg;
    proven_transport_t t;
    proven_ws_decoder_t decoder;
    proven_chacha_rng_t masks;          /* a client's mask keys */
    proven_byte_t *message;             /* the message being assembled */
    proven_size_t message_len;
    proven_size_t message_cap;
    proven_size_t read_pos;
    proven_size_t read_len;
    proven_u64 pongs;
    /* Pings on silence: when the peer was last heard from, and when this side's ping went (while
     * `pinged`). There is no timer: a receive or a close reads no longer than to the next of these. */
    proven_net_deadline_t heard, pinged_at;
    bool pinged;
    proven_err_t failed;
    proven_u16 close_code;
    proven_u8 close_reason_len;
    proven_u8 protocol_len;
    bool is_server;
    bool message_text;
    bool sending;                       /* a message begun with send_part is open */
    bool close_sent;
    bool close_received;
    bool deflate;                       /* permessage-deflate was agreed */
    bool message_compressed;            /* the message being received is compressed */
    bool inflating;                     /* `inflate` holds a decompressor */
    bool zip_begun;                     /* the first frame of the compressed message being sent has left */
    bool plain;                         /* proven_ws_conn_set_compress(false): messages that begin now go as they are */
    bool zipping;                       /* the message being sent goes through `zip` */
    bool send_takeover;                 /* this side keeps `zip` between messages */
    bool recv_takeover;                 /* the peer may keep its window: `kept` reads every message */
    bool zip_off;                       /* nothing is compressed from now on: the peer allows no window this side has, or a compressed message failed half-sent under takeover */
    proven_u8 zip_tail_len;
    proven_byte_t zip_tail[4];
    proven_deflate_t *zip;              /* the message being sent goes through this, and under takeover every message */
    proven_inflate_t *kept;             /* under the peer's takeover: the decompressor of every message */
    proven_ws_inflate_t_ inflate;
    proven_byte_t close_reason[PROVEN_WS_MAX_CONTROL];
    proven_byte_t protocol[64];
    proven_byte_t read_buf[WC_READ_SIZE];
    proven_byte_t send_buf[WC_SEND_SIZE];
};

static proven_u8str_view_t wc_lit(const char *s) {
    proven_size_t n = 0;
    while (s[n] != '\0') n++;
    return (proven_u8str_view_t){ .ptr = (const proven_byte_t *)s, .size = n };
}

static proven_net_deadline_t wc_write_deadline(const proven_ws_conn_t *c) {
    return proven_net_deadline_in(c->cfg.io_timeout_ms);
}

// -----------------------------------------------------------------------------
// Sending
// -----------------------------------------------------------------------------

/* One frame. A client's payload goes through send_buf to be masked; a server's goes as it is. */
static proven_err_t wc_send_frame(proven_ws_conn_t *c, bool fin, proven_u8 opcode, bool rsv1, proven_mem_view_t data) {
    proven_ws_frame_t f = { .fin = fin, .opcode = opcode, .masked = !c->is_server, .length = data.size, .rsv1 = rsv1 };
    if (f.masked) proven_chacha_rng_fill(&c->masks, f.mask, sizeof f.mask);
    proven_size_t n = 0;
    proven_err_t e = proven_ws_frame_write((proven_mem_mut_t){ .ptr = c->send_buf, .size = sizeof c->send_buf }, &n, &f);
    if (e != PROVEN_OK) return e;

    proven_size_t done = 0;
    for (;;) {
        /* The header and as much payload as fits leave together: a small frame is one write. */
        proven_size_t room = sizeof c->send_buf - n;
        proven_size_t take = data.size - done < room ? data.size - done : room;
        for (proven_size_t i = 0; i < take; ++i) c->send_buf[n + i] = data.ptr[done + i];
        if (f.masked) proven_ws_mask((proven_mem_mut_t){ .ptr = c->send_buf + n, .size = take }, f.mask, done);
        proven_result_size_t w = proven_transport_write_all(c->t, (proven_mem_view_t){ .ptr = c->send_buf, .size = n + take }, wc_write_deadline(c));
        if (w.err != PROVEN_OK) { c->failed = w.err; return w.err; }
        done += take;
        n = 0;
        if (done == data.size) return PROVEN_OK;
    }
}

static proven_err_t wc_send_close(proven_ws_conn_t *c, proven_u16 code, proven_u8str_view_t reason) {
    proven_byte_t payload[PROVEN_WS_MAX_CONTROL];
    proven_size_t n = 0;
    proven_err_t e = proven_ws_close_write((proven_mem_mut_t){ .ptr = payload, .size = sizeof payload }, &n, code, reason);
    if (e != PROVEN_OK) return e;
    c->close_sent = true;
    return wc_send_frame(c, true, PROVEN_WS_CLOSE, false, (proven_mem_view_t){ .ptr = payload, .size = n });
}

static proven_err_t wc_can_send(const proven_ws_conn_t *c) {
    if (!c) return PROVEN_ERR_INVALID_ARG;
    if (c->failed != PROVEN_OK) return c->failed;
    if (c->close_sent || c->close_received) return PROVEN_ERR_INVALID_STATE;
    return PROVEN_OK;
}

proven_err_t proven_ws_conn_send_part(proven_ws_conn_t *conn, bool text, proven_mem_view_t data, bool last) {
    proven_err_t e = wc_can_send(conn);
    if (e != PROVEN_OK) return e;
    if (data.size > 0 && !data.ptr) return PROVEN_ERR_INVALID_ARG;
    proven_u8 opcode = (proven_u8)(conn->sending ? PROVEN_WS_CONTINUATION : (text ? PROVEN_WS_TEXT : PROVEN_WS_BINARY));
    if (conn->deflate && !conn->sending) {
        /* A message begins: is it compressed? Not when it is whole and too short to gain, and
         * not when there is no memory for a compressor - then it goes as it is (and, under
         * takeover, the next message tries again: nothing went into a window). */
        bool worth = !conn->plain && !conn->zip_off &&
                     !(last && data.size < (conn->send_takeover ? PROVEN_WS_DEFLATE_MIN_TAKEOVER : PROVEN_WS_DEFLATE_MIN));
        if (worth && !conn->zip &&
            proven_ws_deflate_begin_(conn->cfg.alloc, conn->cfg.compress_level, conn->cfg.compress_window_bits,
                                     last && !conn->send_takeover ? data.size : 0, &conn->zip) != PROVEN_OK) conn->zip = (void *)0;
        conn->zipping = worth && conn->zip;
        conn->zip_tail_len = 0;
        conn->zip_begun = false;
    }
    if (!conn->zipping) {
        e = wc_send_frame(conn, last, opcode, false, data);
        if (e == PROVEN_OK) conn->sending = !last;
        return e;
    }
    /* Compressed: every part is flushed, so that the peer can read it when it arrives, and
     * leaves as one frame for each buffer of output - the first of the message with the bit
     * that says "compressed", the last with FIN. */
    for (;;) {
        proven_byte_t piece[4096];
        proven_size_t used = 0, made = 0;
        bool done = false;
        e = proven_ws_deflate_step_(conn->zip, data, &used, true, last, (proven_mem_mut_t){ .ptr = piece, .size = sizeof piece }, &made,
                                    conn->zip_tail, &conn->zip_tail_len, &done);
        if (e != PROVEN_OK) { conn->failed = e; break; }
        data.ptr += used;
        data.size -= used;
        if (made > 0 || (done && last)) {
            /* A part that gave the compressor nothing new sends nothing; the frame that says
             * what the message is leaves with the first bytes there are. */
            proven_u8 kind = (proven_u8)(conn->zip_begun ? PROVEN_WS_CONTINUATION : (text ? PROVEN_WS_TEXT : PROVEN_WS_BINARY));
            e = wc_send_frame(conn, done && last, kind, !conn->zip_begun, (proven_mem_view_t){ .ptr = piece, .size = made });
            if (e != PROVEN_OK) break;
            conn->zip_begun = true;
        }
        if (done) break;
    }
    if (e != PROVEN_OK || last) {
        /* Under takeover the compressor is the window both sides share; one that failed in
         * the middle of a message no longer matches the peer's, so nothing more is compressed. */
        if (e != PROVEN_OK && conn->send_takeover) conn->zip_off = true;
        if (e != PROVEN_OK || !conn->send_takeover) {
            proven_deflate_destroy(conn->zip);
            conn->zip = (void *)0;
        }
        conn->zipping = false;
    }
    if (e == PROVEN_OK) conn->sending = !last;
    return e;
}

proven_err_t proven_ws_conn_send_binary(proven_ws_conn_t *conn, proven_mem_view_t data) {
    proven_err_t e = wc_can_send(conn);
    if (e != PROVEN_OK) return e;
    if (conn->sending) return PROVEN_ERR_INVALID_STATE;
    return proven_ws_conn_send_part(conn, false, data, true);
}

proven_err_t proven_ws_conn_send_text(proven_ws_conn_t *conn, proven_u8str_view_t text) {
    proven_err_t e = wc_can_send(conn);
    if (e != PROVEN_OK) return e;
    if (conn->sending) return PROVEN_ERR_INVALID_STATE;
    if (text.size > 0 && !text.ptr) return PROVEN_ERR_INVALID_ARG;
    for (proven_size_t pos = 0; pos < text.size;) {
        if (text.ptr[pos] < 0x80) { pos++; continue; }
        proven_utf8_char_t ch = proven_utf8_decode_next(text, pos);
        if (ch.err != PROVEN_OK) return PROVEN_ERR_INVALID_ENCODING;
        pos += ch.len;
    }
    return proven_ws_conn_send_part(conn, true, (proven_mem_view_t){ .ptr = text.ptr, .size = text.size }, true);
}

proven_err_t proven_ws_conn_ping(proven_ws_conn_t *conn, proven_mem_view_t data) {
    proven_err_t e = wc_can_send(conn);
    if (e != PROVEN_OK) return e;
    if (data.size > PROVEN_WS_MAX_CONTROL || (data.size > 0 && !data.ptr)) return PROVEN_ERR_INVALID_ARG;
    return wc_send_frame(conn, true, PROVEN_WS_PING, false, data);
}

// -----------------------------------------------------------------------------
// Receiving
// -----------------------------------------------------------------------------

static proven_err_t wc_message_append(proven_ws_conn_t *c, proven_mem_view_t piece) {
    if (piece.size > c->cfg.max_message_bytes - c->message_len) return PROVEN_ERR_OUT_OF_BOUNDS;
    proven_size_t need = c->message_len + piece.size;
    if (need > c->message_cap) {
        proven_size_t cap = c->message_cap ? c->message_cap : 1024;
        while (cap < need) cap = cap > c->cfg.max_message_bytes / 2 ? c->cfg.max_message_bytes : cap * 2;
        proven_allocator_t a = c->cfg.alloc;
        proven_result_mem_mut_t m = c->message ? a.realloc_fn(a.ctx, c->message, c->message_cap, cap, 16) : a.alloc_fn(a.ctx, cap, 16);
        if (!proven_is_ok(m.err)) return PROVEN_ERR_NOMEM;
        c->message = m.value.ptr;
        c->message_cap = cap;
    }
    for (proven_size_t i = 0; i < piece.size; ++i) c->message[c->message_len + i] = piece.ptr[i];
    c->message_len = need;
    return PROVEN_OK;
}

/* Room for decoded bytes at the end of the message: at least one byte, at most `want`. The
 * buffer may grow a little past the limit, so that the byte that breaks the limit has
 * somewhere to go and is seen. */
static proven_err_t wc_message_room(proven_ws_conn_t *c, proven_size_t want, proven_size_t *room) {
    proven_size_t most = c->cfg.max_message_bytes + 64;
    if (c->message_len >= most) return PROVEN_ERR_OUT_OF_BOUNDS;
    if (c->message_cap - c->message_len < want && c->message_cap < most) {
        proven_size_t cap = c->message_cap ? c->message_cap : 1024;
        while (cap - c->message_len < want && cap < most) cap = cap > most / 2 ? most : cap * 2;
        proven_allocator_t a = c->cfg.alloc;
        proven_result_mem_mut_t m = c->message ? a.realloc_fn(a.ctx, c->message, c->message_cap, cap, 16) : a.alloc_fn(a.ctx, cap, 16);
        if (!proven_is_ok(m.err)) return PROVEN_ERR_NOMEM;
        c->message = m.value.ptr;
        c->message_cap = cap;
    }
    *room = c->message_cap - c->message_len;
    return PROVEN_OK;
}

static void wc_inflate_end(proven_ws_conn_t *c) {
    if (c->inflating) { proven_ws_inflate_end_(&c->inflate); c->inflating = false; }
}

/* A piece of a compressed message, inflated onto the end of the message buffer. `*whole` is
 * set when it was the last piece and everything has come out. */
static proven_err_t wc_message_inflate(proven_ws_conn_t *c, proven_mem_view_t piece, bool last, bool *whole) {
    *whole = false;
    for (;;) {
        proven_size_t room = 0, used = 0, made = 0;
        bool done = false;
        proven_err_t e = wc_message_room(c, 4096, &room);
        if (e == PROVEN_OK) {
            e = proven_ws_inflate_step_(&c->inflate, piece, &used, last, (proven_mem_mut_t){ .ptr = c->message + c->message_len, .size = room }, &made,
                                        c->cfg.max_message_bytes, &done);
        }
        /* Bytes that are not DEFLATE are answered as text that is not text is: the payload
         * is not what its frame says. */
        if (e == PROVEN_ERR_INVALID_FORMAT) e = PROVEN_ERR_INVALID_ENCODING;
        if (e != PROVEN_OK) return e;
        piece.ptr += used;
        piece.size -= used;
        c->message_len += made;
        if (done) { *whole = true; return PROVEN_OK; }
        if (piece.size == 0 && !last && made < room) return PROVEN_OK;     /* the rest is in a later frame */
    }
}

/* The peer did something the connection cannot continue after: tell it why, once, and stop. */
static proven_err_t wc_fail(proven_ws_conn_t *c, proven_err_t e) {
    if (!c->close_sent && c->failed == PROVEN_OK &&
        wc_send_close(c, proven_ws_close_code_for(e), (proven_u8str_view_t){0}) == PROVEN_OK) {
        /* The peer may be in the middle of sending - a frame past the limit, say. Closing the
         * socket with that unread would answer with a reset, and the reset would destroy the
         * close frame just sent before the peer read it. So: stop sending, and read the rest
         * away for a moment, until the peer closes or the moment is over. */
        (void)proven_transport_shutdown(c->t);
        proven_net_deadline_t until = proven_net_deadline_in(WC_LINGER_MS);
        for (;;) {
            proven_result_size_t r = proven_transport_read(c->t, (proven_mem_mut_t){ .ptr = c->read_buf, .size = sizeof c->read_buf }, until);
            if (r.err != PROVEN_OK) break;
        }
        c->read_pos = 0;
        c->read_len = 0;
    }
    wc_inflate_end(c);
    c->failed = e;
    return e;
}

/*
 * Read and decode until a message is complete (returns PROVEN_OK with *out set), the peer
 * closes (PROVEN_ERR_EOF), or something ends the wait. With `discard` set, messages are
 * dropped and pings are not answered: that is the state after this side has sent its close.
 */
static proven_err_t wc_pump(proven_ws_conn_t *c, proven_net_deadline_t until, proven_ws_message_t *out, bool discard) {
    for (;;) {
        if (c->close_received) return PROVEN_ERR_EOF;
        if (c->failed != PROVEN_OK) return c->failed;
        if (c->read_pos == c->read_len) {
            c->read_pos = 0;
            c->read_len = 0;
            /* Read no longer than to this side's next ping, or to the end of the wait for its pong. */
            const proven_net_deadline_t due = c->pinged ? c->pinged_at + (proven_time_t)c->cfg.pong_timeout_ms * 1000000
                                                        : c->heard + (proven_time_t)c->cfg.ping_interval_ms * 1000000;
            const bool watch = !discard && until != PROVEN_NET_DONT_WAIT && due < until;
            proven_result_size_t r = proven_transport_read(c->t, (proven_mem_mut_t){ .ptr = c->read_buf, .size = sizeof c->read_buf }, watch ? due : until);
            if (r.err == PROVEN_ERR_TIMEOUT && watch && proven_net_deadline_in(0) >= due) {
                if (c->pinged) {
                    /* No byte at all for a ping's interval and the pong's: the peer is gone without a word. */
                    c->close_code = PROVEN_WS_CLOSE_ABNORMAL;
                    c->failed = PROVEN_ERR_RESET;
                    return c->failed;
                }
                proven_err_t e = wc_send_frame(c, true, PROVEN_WS_PING, false, (proven_mem_view_t){0});
                if (e != PROVEN_OK) return e;
                c->pinged = true;
                c->pinged_at = proven_net_deadline_in(0);
                continue;
            }
            if (r.err == PROVEN_ERR_TIMEOUT) return PROVEN_ERR_TIMEOUT;
            if (r.err != PROVEN_OK) {
                /* The connection ended with no close frame: an abnormal closure. */
                c->close_code = PROVEN_WS_CLOSE_ABNORMAL;
                c->failed = r.err == PROVEN_ERR_EOF ? PROVEN_ERR_RESET : r.err;
                return c->failed;
            }
            c->read_len = r.value;
            /* Any byte from the peer answers a ping (as ws_event). */
            c->heard = proven_net_deadline_in(0);
            c->pinged = false;
        }
        proven_size_t used = 0;
        proven_ws_event_t ev;
        proven_err_t e = proven_ws_decoder_feed(&c->decoder, (proven_mem_mut_t){ .ptr = c->read_buf + c->read_pos, .size = c->read_len - c->read_pos }, &used, &ev);
        c->read_pos += used;
        if (e != PROVEN_OK) return wc_fail(c, e);
        switch (ev.kind) {
            case PROVEN_WS_EVENT_NONE:
                break;
            case PROVEN_WS_EVENT_DATA:
                if (discard) break;
                if (ev.first) {
                    c->message_len = 0;
                    c->message_text = ev.text;
                    c->message_compressed = ev.compressed;
                    wc_inflate_end(c);
                    if (ev.compressed) {
                        e = proven_ws_inflate_begin_(&c->inflate, c->cfg.alloc, ev.text, c->recv_takeover ? &c->kept : (void *)0);
                        if (e != PROVEN_OK) return wc_fail(c, e);
                        c->inflating = true;
                    }
                }
                if (c->message_compressed) {
                    bool whole = false;
                    e = wc_message_inflate(c, ev.data, ev.last, &whole);
                    if (e != PROVEN_OK) return wc_fail(c, e);
                    if (!whole) break;
                    wc_inflate_end(c);
                    out->text = c->message_text;
                    out->data = (proven_mem_view_t){ .ptr = c->message, .size = c->message_len };
                    return PROVEN_OK;
                }
                e = wc_message_append(c, ev.data);
                if (e != PROVEN_OK) return wc_fail(c, e);
                if (ev.last) {
                    out->text = c->message_text;
                    out->data = (proven_mem_view_t){ .ptr = c->message, .size = c->message_len };
                    return PROVEN_OK;
                }
                break;
            case PROVEN_WS_EVENT_PING:
                if (!discard && !c->close_sent) {
                    e = wc_send_frame(c, true, PROVEN_WS_PONG, false, ev.data);
                    if (e != PROVEN_OK) return e;
                }
                break;
            case PROVEN_WS_EVENT_PONG:
                c->pongs++;
                break;
            case PROVEN_WS_EVENT_CLOSE:
                c->close_received = true;
                c->close_code = ev.close_code;
                c->close_reason_len = (proven_u8)ev.data.size;
                for (proven_size_t i = 0; i < ev.data.size; ++i) c->close_reason[i] = ev.data.ptr[i];
                /* Answer with the same code, as the RFC asks; a close without one is answered
                 * without one. */
                if (!c->close_sent) (void)wc_send_close(c, ev.close_code, (proven_u8str_view_t){0});
                (void)proven_transport_shutdown(c->t);
                return PROVEN_ERR_EOF;
        }
    }
}

proven_err_t proven_ws_conn_receive(proven_ws_conn_t *conn, proven_net_deadline_t until, proven_ws_message_t *out) {
    if (out) *out = (proven_ws_message_t){0};
    if (!conn || !out) return PROVEN_ERR_INVALID_ARG;
    if (conn->close_sent && !conn->close_received && conn->failed == PROVEN_OK) return PROVEN_ERR_INVALID_STATE;
    return wc_pump(conn, until, out, false);
}

proven_err_t proven_ws_conn_close(proven_ws_conn_t *conn, proven_u16 code, proven_u8str_view_t reason) {
    if (!conn) return PROVEN_ERR_INVALID_ARG;
    if (conn->failed != PROVEN_OK) return conn->failed;
    if (!conn->close_sent) {
        proven_err_t e = wc_send_close(conn, code, reason);
        if (e != PROVEN_OK) return e;
    }
    if (!conn->close_received) {
        proven_ws_message_t ignored;
        proven_err_t e = wc_pump(conn, proven_net_deadline_in(conn->cfg.close_timeout_ms), &ignored, true);
        /* A peer that drops the connection once it has our close has closed, if rudely. */
        if (e == PROVEN_ERR_RESET) { conn->failed = PROVEN_OK; conn->close_received = true; e = PROVEN_ERR_EOF; }
        if (e != PROVEN_ERR_EOF) return e;
    }
    (void)proven_transport_shutdown(conn->t);
    return PROVEN_OK;
}

// -----------------------------------------------------------------------------
// Making and destroying
// -----------------------------------------------------------------------------

static bool wc_config_ok(const proven_ws_conn_config_t *config) {
    return config && proven_alloc_is_valid(config->alloc) && config->compress_level >= -1 && config->compress_level <= 9 &&
           (config->compress_window_bits == 0 || (config->compress_window_bits >= 9 && config->compress_window_bits <= 15));
}

/* `terms`: what was agreed for the extension, or null when it was not. */
static proven_err_t wc_open(proven_transport_t transport, bool is_server, proven_mem_view_t early,
                            const proven_ws_conn_config_t *config, const proven_ws_deflate_terms_t *terms, proven_ws_conn_t **out) {
    if (out) *out = (void *)0;
    if (!out || !wc_config_ok(config) || !proven_transport_is_valid(transport) ||
        (early.size > 0 && !early.ptr)) return PROVEN_ERR_INVALID_ARG;
    if (early.size > WC_READ_SIZE) return PROVEN_ERR_OUT_OF_BOUNDS;
    proven_result_mem_mut_t m = config->alloc.alloc_fn(config->alloc.ctx, sizeof(proven_ws_conn_t), 16);
    if (!proven_is_ok(m.err)) return PROVEN_ERR_NOMEM;
    proven_ws_conn_t *c = (void *)m.value.ptr;
    /* Field by field: the struct is mostly buffers, and zeroing 20 KiB to use none of it is waste. */
    c->cfg = *config;
    if (c->cfg.max_message_bytes == 0) c->cfg.max_message_bytes = WC_DEFAULT_MAX_MESSAGE;
    if (c->cfg.io_timeout_ms == 0) c->cfg.io_timeout_ms = 30000;
    if (c->cfg.close_timeout_ms == 0) c->cfg.close_timeout_ms = 5000;
    if (c->cfg.ping_interval_ms == 0) c->cfg.ping_interval_ms = WC_DEFAULT_PING_MS;
    if (c->cfg.pong_timeout_ms == 0) c->cfg.pong_timeout_ms = WC_DEFAULT_PONG_MS;
    c->heard = proven_net_deadline_in(0);
    c->pinged_at = 0;
    c->pinged = false;
    c->t = transport;
    c->message = (void *)0;
    c->message_len = 0;
    c->message_cap = 0;
    c->read_pos = 0;
    c->read_len = early.size;
    c->pongs = 0;
    c->failed = PROVEN_OK;
    c->close_code = 0;
    c->close_reason_len = 0;
    c->protocol_len = 0;
    c->is_server = is_server;
    c->message_text = false;
    c->sending = false;
    c->close_sent = false;
    c->close_received = false;
    bool deflate = terms != (void *)0;
    c->deflate = deflate;
    c->message_compressed = false;
    c->inflating = false;
    c->zip_tail_len = 0;
    c->zip_begun = false;
    c->plain = false;
    c->zipping = false;
    c->send_takeover = deflate && (is_server ? terms->server_takeover : terms->client_takeover);
    c->recv_takeover = deflate && (is_server ? terms->client_takeover : terms->server_takeover);
    c->zip_off = false;
    c->zip = (void *)0;
    c->kept = (void *)0;
    if (deflate) {
        /* The window: the configured one (0 is 11 under takeover, 15 without), no larger than
         * the peer allows. Smaller than this compressor's smallest: nothing is compressed. */
        int limit = is_server ? terms->server_window_bits : terms->client_window_bits;
        int bits = c->cfg.compress_window_bits != 0 ? c->cfg.compress_window_bits : c->send_takeover ? PROVEN_WS_DEFLATE_TAKEOVER_BITS : 15;
        if (limit < bits) bits = limit;
        if (bits < 9) c->zip_off = true;
        else c->cfg.compress_window_bits = bits;
    }
    for (proven_size_t i = 0; i < early.size; ++i) c->read_buf[i] = early.ptr[i];
    proven_ws_decoder_init(&c->decoder, is_server, c->cfg.max_message_bytes);
    if (deflate) proven_ws_decoder_allow_compressed(&c->decoder);
    if (!is_server) {
        /* Mask keys must be unpredictable to the application's peer; one draw from the system
         * seeds a generator for the life of the connection. */
        proven_byte_t seed[PROVEN_CHACHA_SEED_SIZE];
        if (!proven_random_bytes(seed, sizeof seed)) {
            config->alloc.free_fn(config->alloc.ctx, c);
            return PROVEN_ERR_IO;
        }
        proven_chacha_rng_seed(&c->masks, seed);
    }
    *out = c;
    return PROVEN_OK;
}

proven_err_t proven_ws_conn_open(proven_transport_t transport, bool is_server, proven_mem_view_t early,
                                 const proven_ws_conn_config_t *config, proven_ws_conn_t **out) {
    /* Nothing was negotiated here, so there is no "the peer" to have agreed to keep nothing. */
    if (config && config->compress && !config->compress_no_takeover && config->compress_peer_no_takeover) {
        if (out) *out = (void *)0;
        return PROVEN_ERR_INVALID_ARG;
    }
    bool takeover = config && !config->compress_no_takeover;
    proven_ws_deflate_terms_t terms = { .server_takeover = takeover, .client_takeover = takeover, .server_window_bits = 15, .client_window_bits = 15 };
    return wc_open(transport, is_server, early, config, config && config->compress ? &terms : (void *)0, out);
}

bool proven_ws_conn_compressed(const proven_ws_conn_t *conn) { return conn && conn->deflate; }

void proven_ws_conn_set_compress(proven_ws_conn_t *conn, bool on) {
    if (conn) conn->plain = !on;
}

void proven_ws_conn_destroy(proven_ws_conn_t *conn) {
    if (!conn) return;
    proven_allocator_t a = conn->cfg.alloc;
    (void)proven_transport_close(conn->t);
    wc_inflate_end(conn);
    if (conn->zip) proven_deflate_destroy(conn->zip);
    if (conn->kept) proven_inflate_destroy(conn->kept);
    if (conn->message) a.free_fn(a.ctx, conn->message);
    a.free_fn(a.ctx, conn);
}

static void wc_set_protocol(proven_ws_conn_t *c, proven_u8str_view_t p) {
    c->protocol_len = (proven_u8)(p.size < sizeof c->protocol ? p.size : sizeof c->protocol);
    for (proven_size_t i = 0; i < c->protocol_len; ++i) c->protocol[i] = p.ptr[i];
}

/* proven_ws_deflate_accept, with what it agreed as terms: no takeover either way. */
static proven_err_t wc_accept_plain(const proven_http_request_t *req, proven_mem_mut_t out, proven_size_t *len, proven_ws_deflate_terms_t *terms) {
    int bits = 0;
    proven_err_t e = proven_ws_deflate_accept(req, out, len, &bits);
    *terms = (proven_ws_deflate_terms_t){ .server_window_bits = bits, .client_window_bits = 15 };
    return e;
}

proven_err_t proven_ws_conn_accept(proven_http_exchange_t *exchange, proven_u8str_view_t protocol,
                                   const proven_ws_conn_config_t *config, proven_ws_conn_t **out) {
    if (out) *out = (void *)0;
    if (!exchange || !out || !wc_config_ok(config) || protocol.size > 64) return PROVEN_ERR_INVALID_ARG;
    const proven_http_request_t *req = proven_http_exchange_request(exchange);
    proven_u8str_view_t key;
    proven_err_t e = proven_ws_check_request(req, &key);
    if (e == PROVEN_ERR_NOT_FOUND) return e;
    if (e == PROVEN_ERR_UNSUPPORTED) {
        proven_http_header_t h = { wc_lit("Sec-WebSocket-Version"), wc_lit("13") };
        (void)proven_http_exchange_respond(exchange, 426, &h, 1, (proven_mem_view_t){0});
        return e;
    }
    if (e != PROVEN_OK) {
        (void)proven_http_exchange_respond(exchange, 400, (void *)0, 0, (proven_mem_view_t){0});
        return PROVEN_ERR_INVALID_FORMAT;
    }
    if (protocol.size > 0 && !proven_ws_request_offers(req, protocol)) return PROVEN_ERR_INVALID_ARG;

    proven_byte_t accept[PROVEN_WS_ACCEPT_SIZE];
    e = proven_ws_accept_key(key, accept);
    if (e != PROVEN_OK) return e;
    proven_http_header_t headers[3] = { { wc_lit("Sec-WebSocket-Accept"), { .ptr = accept, .size = sizeof accept } } };
    proven_size_t header_count = 1;
    if (protocol.size > 0) headers[header_count++] = (proven_http_header_t){ wc_lit("Sec-WebSocket-Protocol"), protocol };
    /* Compression, when this side wants it and the client offered a form of it that can be met. */
    proven_byte_t extension[PROVEN_WS_DEFLATE_ANSWER_MAX];
    proven_size_t extension_len = 0;
    proven_ws_deflate_terms_t terms;
    proven_mem_mut_t room = { .ptr = extension, .size = sizeof extension };
    bool deflate = config->compress &&
                   (config->compress_no_takeover ? wc_accept_plain(req, room, &extension_len, &terms)
                    : config->compress_peer_no_takeover ? proven_ws_deflate_accept_peer_no_takeover(req, room, &extension_len, &terms)
                                                        : proven_ws_deflate_accept_takeover(req, room, &extension_len, &terms)) == PROVEN_OK;
    if (deflate) headers[header_count++] = (proven_http_header_t){ wc_lit("Sec-WebSocket-Extensions"), { .ptr = extension, .size = extension_len } };
    proven_transport_t t;
    proven_mem_view_t early;
    e = proven_http_exchange_upgrade(exchange, wc_lit("websocket"), headers, header_count, &t, &early);
    if (e == PROVEN_ERR_BUSY) {
        /* The server's `max_upgraded` are open: try again soon. */
        const proven_http_header_t h = { wc_lit("Retry-After"), wc_lit("1") };
        (void)proven_http_exchange_respond(exchange, 503, &h, 1, (proven_mem_view_t){0});
        return e;
    }
    if (e != PROVEN_OK) return e;
    e = wc_open(t, true, early, config, deflate ? &terms : (void *)0, out);
    if (e != PROVEN_OK) { (void)proven_transport_close(t); return e; }
    wc_set_protocol(*out, protocol);
    return PROVEN_OK;
}

proven_err_t proven_ws_conn_connect(proven_http_client_t *client, proven_u8str_view_t url,
                                    const proven_http_header_t *headers, proven_size_t header_count,
                                    proven_u8str_view_t protocols, const proven_ws_conn_config_t *config,
                                    proven_ws_conn_t **out, proven_u16 *http_status) {
    enum { MAX_EXTRA = 28, URL_MAX = 2048 };
    if (out) *out = (void *)0;
    if (http_status) *http_status = 0;
    if (!client || !out || !wc_config_ok(config) || (header_count > 0 && !headers) ||
        header_count > MAX_EXTRA || url.size < 6 || url.size > URL_MAX - 2 || !url.ptr) return PROVEN_ERR_INVALID_ARG;

    /* ws and wss are http and https with another name: the same port, the same connection. */
    proven_byte_t http_url[URL_MAX];
    proven_size_t ul = 0;
    proven_size_t skip = 0;
    bool is_ws = (url.ptr[0] | 0x20) == 'w' && (url.ptr[1] | 0x20) == 's';
    if (is_ws && url.ptr[2] == ':') { skip = 2; http_url[ul++] = 'h'; http_url[ul++] = 't'; http_url[ul++] = 't'; http_url[ul++] = 'p'; }
    else if (is_ws && (url.ptr[2] | 0x20) == 's' && url.ptr[3] == ':') { skip = 3; http_url[ul++] = 'h'; http_url[ul++] = 't'; http_url[ul++] = 't'; http_url[ul++] = 'p'; http_url[ul++] = 's'; }
    for (proven_size_t i = skip; i < url.size; ++i) http_url[ul++] = url.ptr[i];

    proven_byte_t random[16], key[PROVEN_WS_KEY_SIZE];
    if (!proven_random_bytes(random, sizeof random)) return PROVEN_ERR_IO;
    proven_ws_make_key(random, key);

    proven_http_header_t all[MAX_EXTRA + 4];
    proven_size_t n = 0;
    for (proven_size_t i = 0; i < header_count; ++i) all[n++] = headers[i];
    all[n++] = (proven_http_header_t){ wc_lit("Sec-WebSocket-Key"), { .ptr = key, .size = sizeof key } };
    all[n++] = (proven_http_header_t){ wc_lit("Sec-WebSocket-Version"), wc_lit("13") };
    if (protocols.size > 0) all[n++] = (proven_http_header_t){ wc_lit("Sec-WebSocket-Protocol"), protocols };
    if (config->compress) {
        all[n++] = (proven_http_header_t){ wc_lit("Sec-WebSocket-Extensions"),
                                           config->compress_no_takeover ? wc_lit(PROVEN_WS_DEFLATE_OFFER)
                                           : config->compress_peer_no_takeover ? wc_lit(PROVEN_WS_DEFLATE_OFFER_PEER_NO_TAKEOVER) : wc_lit(PROVEN_WS_DEFLATE_OFFER_TAKEOVER) };
    }

    proven_http_client_request_t req = {
        .method = wc_lit("GET"),
        .url = { .ptr = http_url, .size = ul },
        .headers = all,
        .header_count = n,
        .upgrade = wc_lit("websocket"),
    };
    proven_http_client_response_t resp;
    proven_err_t e = proven_http_client_send(client, &req, &resp);
    if (e != PROVEN_OK) { proven_http_client_finish(&resp); return e; }
    if (http_status) *http_status = resp.status;
    if (resp.status != 101) { proven_http_client_finish(&resp); return PROVEN_ERR_REFUSED; }

    proven_http_response_t head = { .status = resp.status, .version_minor = 1, .headers = (proven_http_header_t *)resp.headers, .header_count = resp.header_count };
    proven_u8str_view_t chosen;
    bool deflate = false;
    /* Only a client that offered the extension may be answered with it. Without takeover, the
     * answer says none either way and this side's window is its own. */
    proven_u8str_view_t sent_key = { .ptr = key, .size = sizeof key };
    proven_ws_deflate_terms_t terms = { .server_window_bits = 15, .client_window_bits = 15 };
    if (config->compress && config->compress_no_takeover) e = proven_ws_check_response_deflate(&head, sent_key, protocols, &chosen, &deflate);
    else if (config->compress && config->compress_peer_no_takeover) e = proven_ws_check_response_peer_no_takeover(&head, sent_key, protocols, &chosen, &deflate, &terms);
    else if (config->compress) e = proven_ws_check_response_takeover(&head, sent_key, protocols, &chosen, &deflate, &terms);
    else e = proven_ws_check_response(&head, sent_key, protocols, &chosen);
    proven_transport_t t = {0};
    proven_mem_view_t early = {0};
    if (e == PROVEN_OK) e = proven_http_client_upgrade(&resp, &t, &early);
    if (e == PROVEN_OK) {
        e = wc_open(t, false, early, config, deflate ? &terms : (void *)0, out);
        if (e != PROVEN_OK) (void)proven_transport_close(t);
        else wc_set_protocol(*out, chosen);
    }
    proven_http_client_finish(&resp);
    return e;
}

proven_u8str_view_t proven_ws_conn_protocol(const proven_ws_conn_t *conn) {
    proven_u8str_view_t none = {0};
    return conn ? (proven_u8str_view_t){ .ptr = conn->protocol, .size = conn->protocol_len } : none;
}

proven_u16 proven_ws_conn_close_code(const proven_ws_conn_t *conn) {
    return conn ? conn->close_code : 0;
}

proven_u8str_view_t proven_ws_conn_close_reason(const proven_ws_conn_t *conn) {
    proven_u8str_view_t none = {0};
    return conn ? (proven_u8str_view_t){ .ptr = conn->close_reason, .size = conn->close_reason_len } : none;
}

proven_u64 proven_ws_conn_pong_count(const proven_ws_conn_t *conn) {
    return conn ? conn->pongs : 0;
}

#else
/* A translation unit must not be empty. */
typedef int proven_ws_conn_unused_t;
#endif
