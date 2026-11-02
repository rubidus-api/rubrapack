#ifndef PROVEN_INTERNAL_WS_H
#define PROVEN_INTERNAL_WS_H

#include "proven/deflate.h"
#include "proven/ws.h"
#include "proven/ws_event.h"

/*
 * Internal: what the two WebSocket drivers share for permessage-deflate (RFC 7692), so that
 * the rules exist once. Not public API.
 *
 * Without context takeover a message is compressed on its own, and all of this lives from the
 * first frame of a message to its last. With it, a direction that takes over keeps its
 * compressor or its decompressor (`kept`) from one message to the next, and the rest still
 * lives for one message.
 */

/* The largest piece a driver inflates into or compresses out of in one step. */
#define PROVEN_WS_DEFLATE_PIECE 16384u

/* Messages shorter than this are sent as they are: there is nothing in them to refer back to. */
#define PROVEN_WS_DEFLATE_MIN 64u
/* With takeover a message refers back to the ones before it, and only those shorter than this
 * mostly grow (measured over messages of 1 to 40 bytes: 8 sent the fewest bytes). They are
 * sent as they are and kept out of the compressor, as the peer keeps them out of its window. */
#define PROVEN_WS_DEFLATE_MIN_TAKEOVER 8u
/* The compressor's window under takeover when the configuration leaves it 0: about 28 KiB held
 * against about 324 KiB at 15, for nearly the same size on short messages. */
#define PROVEN_WS_DEFLATE_TAKEOVER_BITS 11

/* The UTF-8 check of ws.c, for text that was inflated by a driver. */
[[nodiscard]]
proven_err_t proven_ws_utf8_feed_(proven_byte_t held[4], proven_u8 *held_len, const proven_byte_t *p, proven_size_t n);

/* A compressed message being received. Zero-initialise; proven_ws_inflate_begin_ for each
 * message, proven_ws_inflate_end_ after it (or when it is abandoned). */
typedef struct {
    proven_inflate_t *z;
    proven_u64 total;                  /* decoded bytes so far */
    proven_byte_t held[4];             /* a character cut by the end of a decoded piece */
    proven_u8 held_len;
    proven_u8 tail;                    /* how many of the four closing bytes have been fed */
    bool text;
    bool kept;                         /* `z` is the connection's, kept between messages: not freed here */
} proven_ws_inflate_t_;

/* `kept`: null for a decompressor of this message only; otherwise the connection's, made here
 * the first time (`*kept` null) and used from then on. */
[[nodiscard]]
proven_err_t proven_ws_inflate_begin_(proven_ws_inflate_t_ *m, proven_allocator_t alloc, bool text, proven_inflate_t **kept);

/* Decode: takes some of `in` (the compressed bytes of one piece of the message), fills some
 * of `out`. With `last` - no more compressed bytes will come - the four bytes the sender left
 * off are fed after `in`, and `*done` is set once everything has come out. Call again while
 * it consumes or produces. PROVEN_ERR_INVALID_FORMAT for bytes that are not DEFLATE,
 * PROVEN_ERR_INVALID_ENCODING for text that is not UTF-8, PROVEN_ERR_OUT_OF_BOUNDS when the
 * decoded message passes `max_bytes` (0: no limit). */
[[nodiscard]]
proven_err_t proven_ws_inflate_step_(proven_ws_inflate_t_ *m, proven_mem_view_t in, proven_size_t *consumed, bool last,
                                     proven_mem_mut_t out, proven_size_t *produced, proven_u64 max_bytes, bool *done);

void proven_ws_inflate_end_(proven_ws_inflate_t_ *m);

/* A message being sent compressed. The window is no larger than `size_hint` needs (0: not
 * known - the whole of `window_bits`). */
[[nodiscard]]
proven_err_t proven_ws_deflate_begin_(proven_allocator_t alloc, int level, int window_bits, proven_size_t size_hint, proven_deflate_t **out);

/* Compress: takes some of `in`, fills some of `out`. With `flush`, what was given so far is
 * made decodable; with `last` as well, the four closing bytes are left off (`*tail` carries
 * that between calls: zero it for each message). `*done` is set when everything asked for has
 * come out; call again with more room until then. */
[[nodiscard]]
proven_err_t proven_ws_deflate_step_(proven_deflate_t *z, proven_mem_view_t in, proven_size_t *consumed, bool flush, bool last,
                                     proven_mem_mut_t out, proven_size_t *produced, proven_byte_t tail[4], proven_u8 *tail_len, bool *done);

/* How ws_event.c reaches the connection a WebSocket is on: the event-driven server's record
 * (ws_event.c itself), or a client's (ws_event_client.c). `send` queues `a` then `b` and
 * sends what goes at once - false when the connection is gone. `timer` fires on_timer in so
 * many milliseconds (0 cancels). `close` and `kill` let go of the connection - politely, with
 * what is queued flushed and a moment's wait, or at once - and no operation is called after
 * either. */
typedef struct {
    bool (*send)(void *conn, proven_mem_view_t a, proven_mem_view_t b);
    void (*timer)(void *conn, proven_u32 ms);
    void (*pause)(void *conn, bool paused);
    void (*close)(void *conn);
    void (*kill)(void *conn);
    proven_size_t (*buffered)(const void *conn);
} proven_ws_transport_t_;

/* In ws_event.c, for ws_event_client.c: the protocol's object for a connection this side is
 * making. It is `opening` - nothing may be sent - until the server has accepted the upgrade.
 * `config` is checked as proven_ws_event_accept checks it. */
struct proven_ws_stream;
[[nodiscard]]
proven_err_t proven_ws_event_client_new_(proven_allocator_t alloc, const proven_ws_event_config_t *config, const proven_ws_transport_t_ *transport,
                                         void *conn, proven_net_addr_t peer, struct proven_ws_stream **out);
/* The upgrade was accepted, with compression (`terms`, what was agreed) or without (null):
 * frames from here on. */
void proven_ws_event_client_opened_(struct proven_ws_stream *ws, const proven_ws_deflate_terms_t *terms);
/* The connection was not made, or the upgrade not granted, and is gone: on_closed, code 0. */
void proven_ws_event_client_failed_(struct proven_ws_stream *ws, proven_err_t why);
/* It was never started: freed, and nobody is told. */
void proven_ws_event_client_discard_(struct proven_ws_stream *ws);

#endif /* PROVEN_INTERNAL_WS_H */
