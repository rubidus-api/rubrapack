#ifndef PROVEN_WS_EVENT_H
#define PROVEN_WS_EVENT_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/u8str.h"
#include "proven/net.h"
#include "proven/http.h"
#include "proven/http_event.h"
#include "proven/loop.h"
#include "proven/tls.h"
#include "proven/allocator.h"
#include "proven/ws.h"

/**
 * @file ws_event.h
 * @brief WebSocket on a loop: a connection that costs no thread, on either side.
 *
 * ws_conn.h gives a WebSocket connection calls that wait, and so a thread for as long as it is
 * open. Here a request to the server of http_event.h is turned into a WebSocket inside
 * `on_request`, and from then on your functions are told what arrived - nothing waits, and a
 * connection that is silent holds its state and no buffer. A program that asks instead of
 * answering makes the same kind of connection with proven_ws_event_connect.
 *
 * The same three rules as http_event.h: your functions must not wait; a send can be refused
 * (PROVEN_ERR_AGAIN, with `on_writable` to say when to try again); and a piece of a message is
 * good only until the function it was given to returns.
 *
 * Messages arrive **in the pieces the network delivered**, with `first` and `last` marking
 * their ends: a message of any size passes through without being assembled or held. A handler
 * that wants whole messages collects the pieces itself, up to a limit of its choosing.
 *
 * Everything here is called on the loop's thread, and only there.
 *
 * Hosted only; `PROVEN_NO_NET` leaves it out.
 */

/** @brief A WebSocket connection. Valid from proven_ws_event_accept, or proven_ws_event_connect, until `on_closed` returns. */
typedef struct proven_ws_stream proven_ws_stream_t;

typedef struct {
    /** A piece of a text or binary message. Required. Text is checked to be UTF-8 across the
     *  whole message; `piece` may end in the middle of a character. */
    void (*on_message)(void *ctx, proven_ws_stream_t *ws, bool text, proven_mem_view_t piece, bool first, bool last);
    /** Output that was held back has gone: a send that was refused will now be taken. Optional. */
    void (*on_writable)(void *ctx, proven_ws_stream_t *ws);
    /** The connection is over, called exactly once. `code` is the close code that ended it -
     *  the peer's, or the one this side sent first - and 1006 when it ended without a close
     *  frame; `why` is PROVEN_OK after a close handshake, otherwise the reason (the peer
     *  vanished, a timeout, a protocol error). Free what you attached here. Optional. */
    void (*on_closed)(void *ctx, proven_ws_stream_t *ws, proven_u16 code, proven_err_t why);
} proven_ws_event_callbacks_t;

/** @brief How one WebSocket connection behaves. Zero-initialise it and set `on.on_message`;
 *         every zero field has the default written beside it. */
typedef struct {
    proven_ws_event_callbacks_t on;
    void *ctx;                             /**< passed to every callback */
    proven_u8str_view_t subprotocol;       /**< named in the answer when not empty. Offer it only if the client did (proven_ws_request_offers). */
    proven_u64 max_message_bytes;          /**< largest message accepted, its pieces together. 0: 1 MiB */
    proven_size_t max_buffered_output;     /**< output held before sends are refused. 0: 64 KiB */
    proven_u32 ping_interval_ms;           /**< silence from the client after which a ping is sent. 0: 30 s */
    proven_u32 pong_timeout_ms;            /**< further silence after which the connection is ended. 0: 10 s */
    proven_u32 close_timeout_ms;           /**< how long to wait for the client's answer to a close, counted from the last output it took. 0: 5 s */
    bool compress;                         /**< accept compressed messages (`permessage-deflate`) when the client offers them. See proven_ws_stream_compressed. false: not */
    int compress_level;                    /**< 1 (fastest) to 9 (smallest); -1 stores without compressing. 0: 6 */
    int compress_window_bits;              /**< how far back the compressor looks, 9 to 15: about 11 KiB of memory at 9, 28 KiB at 11, 325 KiB at 15 (without takeover only during one send, and less for a message smaller than the window). 0: 11 with takeover, 15 without */
    bool compress_no_takeover;             /**< with `compress`: compress each message on its own, keeping nothing between messages (no context takeover), as before v0.80.0. Short, similar messages then shrink far less, and an idle connection holds no compression state. false: each side may keep its window (see proven_ws_stream_compressed) */
    bool compress_peer_no_takeover;        /**< with `compress` and takeover: this side keeps its own window, and the other side must compress each message on its own - so this side needs no decompressor between messages (about 35 KiB less per idle compressed connection), and the other side's short, similar messages shrink far less. A server answers `client_no_context_takeover` unasked; a client offers `server_no_context_takeover`, and a server that will not honour it leaves the connection uncompressed. false: the other side may keep its window too */
} proven_ws_event_config_t;

/**
 * @brief Inside `on_request`: accept the request as a WebSocket.
 *
 * Checks the request (proven_ws_check_request), answers 101, and turns the connection into a
 * WebSocket. **The HTTP exchange ends here: `on_done(PROVEN_OK)` is called inside this call**,
 * after which `stream` is over and `*out` is what you hold.
 *
 * On any error nothing has been sent and the request is still yours to answer -
 * 426 with `Sec-WebSocket-Version: 13` for PROVEN_ERR_UNSUPPORTED, 400 for
 * PROVEN_ERR_INVALID_FORMAT, and whatever the page is for PROVEN_ERR_NOT_FOUND, which means
 * the request did not ask for a WebSocket at all.
 *
 * @return PROVEN_ERR_INVALID_ARG (no `on_message`, a subprotocol that is not a token, a
 *         compression level or window outside its range);
 *         PROVEN_ERR_INVALID_STATE when not called inside `on_request` for this stream, or a
 *         response was already begun; the errors of proven_ws_check_request; PROVEN_ERR_NOMEM;
 *         PROVEN_ERR_RESET when the client vanished at that moment (`on_done` was called).
 */
[[nodiscard]]
proven_err_t proven_ws_event_accept(proven_http_stream_t *stream, const proven_http_request_t *head,
                                    const proven_ws_event_config_t *config, proven_ws_stream_t **out);

/**
 * @brief What proven_ws_event_connect is given. Zero-initialise it and set `url` and
 *        `ws.on.on_message`.
 */
typedef struct {
    proven_allocator_t alloc;              /**< not valid: the loop's allocator */
    const proven_tls_config_t *tls;        /**< for `wss`. NULL: a `wss` URL is refused, never connected in the clear. Must outlive the connection. */
    proven_u8str_view_t url;               /**< absolute, `ws` or `wss`; its host goes into `Host` and, for `wss`, is the name the certificate must be for */
    const proven_net_addr_t *address;      /**< where to connect. NULL only when the URL's host is an IP address: names are not resolved on a loop (see http_event_client.h). Its port is used as given. */
    const proven_http_header_t *headers;   /**< yours - `Origin`, `Authorization`, a cookie. `Host`, `Upgrade`, `Connection` and the `Sec-WebSocket-` fields are written for you and may not be here */
    proven_size_t header_count;
    proven_u8str_view_t protocols;         /**< the subprotocols offered, as `Sec-WebSocket-Protocol` carries them (`chat, superchat`); empty: none. At most 255 bytes */
    proven_u32 connect_timeout_ms;         /**< to be connected, the TLS handshake included, and for the server to have accepted the upgrade. 0: 10 s */
    proven_size_t max_head_bytes;          /**< largest answer to the upgrade. 0: 16 KiB */
    /** The server accepted: messages may be sent from here on - also from inside this call.
     *  `protocol` is the subprotocol it chose, empty when none; good until this returns.
     *  `ctx` is `ws.ctx`. Optional. */
    void (*on_open)(void *ctx, proven_ws_stream_t *ws, proven_u8str_view_t protocol);
    proven_ws_event_config_t ws;           /**< the callbacks, limits, pings and compression of the connection - what a server gives proven_ws_event_accept. `compress` here offers the extension; `subprotocol` is a server's answer and must be empty (the offer is `protocols`) */
} proven_ws_event_connect_options_t;

/**
 * @brief Begin a WebSocket connection to a server, on a loop. Returns at once.
 *
 * The connection that results is a `proven_ws_stream_t`, the type a server's connections
 * are: every call below works on it as it does on one of those, and its messages arrive
 * through the same callbacks. A handler written for one side serves the other. What differs
 * is the beginning. **Nothing may be sent until the server has accepted** - the sending calls
 * answer PROVEN_ERR_INVALID_STATE - and `on_open` says when it has;
 * proven_ws_stream_abort ends the attempt, and so does proven_ws_stream_close.
 *
 * When this returns an error nothing was begun and no callback is or will be made. When it
 * returns PROVEN_OK, **`on_closed` will be called exactly once**, never inside this call -
 * also for a connection that was never made or an upgrade that was refused. The close code
 * is then 0 and `why` says which: PROVEN_ERR_REFUSED (nothing listens there, or the server
 * answered with anything but 101 - no redirect is followed and no challenge answered),
 * PROVEN_ERR_UNREACHABLE, PROVEN_ERR_TIMEOUT (`connect_timeout_ms`),
 * PROVEN_ERR_INVALID_FORMAT (an answer that is not HTTP, or a 101 that is not this
 * handshake's: a wrong `Sec-WebSocket-Accept`, a subprotocol that was not offered, an
 * extension that was not), PROVEN_ERR_RESET, or what the TLS handshake failed with.
 *
 * Frames this side sends are masked, as a client's must be, with keys from a generator seeded
 * from the system when the connection is begun.
 *
 * Not here: names are not resolved; no proxy; not over HTTP/2; no reconnection - begin again.
 *
 * @param out receives the connection. Required.
 * @return PROVEN_ERR_INVALID_ARG for no loop, no `on_message`, a URL that is not absolute
 *         `ws` or `wss` or that carries credentials, a `wss` URL with no TLS configuration, a
 *         host that is a name with no `address`, a header this call writes itself or that
 *         cannot be written, an offer longer than 255 bytes, a `ws.subprotocol` (it is a
 *         server's field), a compression level or window outside its range; PROVEN_ERR_NOMEM; PROVEN_ERR_IO when the system gave no random
 *         bytes; and what beginning to connect returned when it failed at once.
 */
[[nodiscard]]
proven_err_t proven_ws_event_connect(proven_loop_t *loop, const proven_ws_event_connect_options_t *options, proven_ws_stream_t **out);

/**
 * @brief Send one whole message.
 *
 * Taken whole or not at all: when less than `max_buffered_output` is held it is queued
 * (copied) and sent as the client takes it; otherwise nothing is taken and the answer is
 * PROVEN_ERR_AGAIN - keep the message and send it again when `on_writable` is called. The most
 * a connection holds is therefore the limit and one message of your choosing.
 *
 * @return PROVEN_ERR_AGAIN as described; PROVEN_ERR_INVALID_STATE after a close was sent or
 *         received, or in the middle of a message begun with proven_ws_stream_send_piece;
 *         PROVEN_ERR_RESET when the connection is gone.
 */
[[nodiscard]]
proven_err_t proven_ws_stream_send(proven_ws_stream_t *ws, bool text, proven_mem_view_t data);

/**
 * @brief Send one piece of a message that is produced as it goes: `first` with the first
 *        piece, `last` with the final one (which may be empty). Each piece is taken or
 *        refused as proven_ws_stream_send says. Text must be UTF-8 over the whole message;
 *        this side does not check what it sends.
 * @return as proven_ws_stream_send; PROVEN_ERR_INVALID_STATE also for `first` inside a
 *         message, or a piece that is not `first` outside one.
 */
[[nodiscard]]
proven_err_t proven_ws_stream_send_piece(proven_ws_stream_t *ws, bool text, proven_mem_view_t data, bool first, bool last);

/** @brief Send a ping with up to 125 bytes. Pings from the client are answered for you, and
 *         one is sent for you after `ping_interval_ms` of silence; this is for a program that
 *         wants its own. PROVEN_ERR_OUT_OF_BOUNDS for more than 125 bytes; otherwise as
 *         proven_ws_stream_send. */
[[nodiscard]]
proven_err_t proven_ws_stream_ping(proven_ws_stream_t *ws, proven_mem_view_t data);

/**
 * @brief Close: send a close frame with `code` and `reason` (at most 123 bytes of UTF-8), and
 *        end the connection when the client answers or `close_timeout_ms` passes. `on_closed`
 *        follows, later. A code that may not be sent (proven_ws_close_code_is_valid) becomes
 *        1000, and a reason that does not fit is left out. Harmless when a close is already
 *        under way.
 */
void proven_ws_stream_close(proven_ws_stream_t *ws, proven_u16 code, proven_u8str_view_t reason);

/** @brief End the connection at once, with no close frame. `on_closed` is called with 1006
 *         and PROVEN_ERR_RESET, inside this call. */
void proven_ws_stream_abort(proven_ws_stream_t *ws);

/** @brief Stop delivering messages (and stop reading the connection) until
 *         proven_ws_stream_resume. */
void proven_ws_stream_pause(proven_ws_stream_t *ws);
/** @brief Deliver again. Pieces that were waiting may arrive inside this call. */
void proven_ws_stream_resume(proven_ws_stream_t *ws);

/** @brief Attach a pointer of yours to the connection, and get it back. */
void proven_ws_stream_set_user(proven_ws_stream_t *ws, void *user);
void *proven_ws_stream_user(const proven_ws_stream_t *ws);

/** @brief The client's address. */
proven_net_addr_t proven_ws_stream_peer(const proven_ws_stream_t *ws);

/**
 * @brief Whether messages on this connection may be compressed: `compress` was set and the
 *        client offered `permessage-deflate` in a form that could be met.
 *
 * When it is on, the calls are used as before. Each side may keep its window from one message
 * to the next (**context takeover**), unless the configuration says `compress_no_takeover` or
 * the other side forbade it: a message then refers back to the messages before it, so short,
 * similar messages shrink to a third or less. A message of 8 bytes or more is compressed as it
 * is queued; shorter ones go as they are. The price is memory for the life of the connection,
 * from its first compressed message: a compressor (about 28 KiB at the default window of 11
 * bits) and a decompressor (about 35 KiB). Without takeover a message of 64 bytes or more is
 * compressed on its own, and an idle connection holds no compression state. A send is still
 * taken or refused on `max_buffered_output`, and is queued a few kilobytes at a time. A
 * compressed message that arrives is inflated on its way to `on_message`, in pieces of at most
 * 16 KiB with `first` and `last` marking its ends: `max_message_bytes` bounds the **inflated**
 * size (1009 past it), text is checked across the pieces, and bytes that do not inflate end
 * the connection with 1007. While such a message is arriving the connection holds about 51 KiB
 * for it (16 KiB of it with takeover, which keeps the decompressor anyway).
 *
 * Raw DEFLATE carries no checksum: a compressed message damaged on the way can inflate to
 * other bytes without an error. And **do not turn compression on for messages that carry a
 * secret together with text the client chose**: the length of a compressed message tells an
 * observer of the connection how much the two have in common, TLS or not. With takeover the
 * secret and the chosen text need not be in the same message: earlier messages change a later
 * one's length too.
 */
[[nodiscard]]
bool proven_ws_stream_compressed(const proven_ws_stream_t *ws);

/**
 * @brief Say whether the messages this side sends **from now on** are compressed, on a
 *        connection that agreed to compression (on any other this does nothing). They are
 *        until this is called with false.
 *
 * For the message that must not be: one that carries a secret together with text the other
 * side chose (see above), or one that is compressed already and would only cost time. Turn
 * it off, send, turn it on again. A message sent uncompressed is not added to the window. A
 * message already begun in pieces goes on as it began; what arrives is not affected - the
 * other side decides that for itself.
 */
void proven_ws_stream_set_compress(proven_ws_stream_t *ws, bool on);

/** @brief Bytes queued for the client and not yet accepted by it. */
proven_size_t proven_ws_stream_buffered(const proven_ws_stream_t *ws);

#endif /* PROVEN_WS_EVENT_H */
