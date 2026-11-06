#ifndef PROVEN_WS_CONN_H
#define PROVEN_WS_CONN_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/u8str.h"
#include "proven/allocator.h"
#include "proven/net.h"
#include "proven/ws.h"
#include "proven/http_client.h"
#include "proven/http_server.h"

/**
 * @file ws_conn.h
 * @brief A WebSocket connection: the codec of ws.h driven over a transport.
 *
 * A connection is made in one of three ways - as a client from a URL, as a server from a
 * request inside an HTTP handler, or over a transport you upgraded yourself - and from then on
 * both ends are the same thing: send a message, receive a message, close.
 *
 * What the connection does for you:
 *
 *   - **Pings are answered.** A ping that arrives while you are receiving gets its pong; you
 *     never see it.
 *   - **Messages arrive whole.** However the peer fragmented one, proven_ws_conn_receive gives
 *     it back in one piece, in memory the connection owns, up to `max_message_bytes`.
 *   - **Violations end the connection, with the reason.** A frame the protocol forbids, text
 *     that is not UTF-8, a message past the limit: the peer is sent a close with the code that
 *     says which, and you get the error.
 *   - **Closing is a handshake.** proven_ws_conn_close sends a close and waits, for a bounded
 *     time, for the peer's.
 *
 *   - **Silence is noticed** (since v0.104.0). After `ping_interval_ms` with nothing from the peer a
 *     ping is sent, and after `pong_timeout_ms` more the connection is ended - a peer that went
 *     away without a word (a laptop shut, a NAT that forgot it) does not hold a thread forever.
 *     There is no timer: the ping goes out while proven_ws_conn_receive waits.
 *
 * What it does not do: reconnect (compression is asked for with
 * `compress`, see proven_ws_conn_compressed). And it carries no more
 * protection than the connection under it - a `ws://` connection is readable by anyone on the
 * path. `wss://` is the same over TLS: the HTTP client's `tls_wrap`, the HTTP server's `tls`.
 *
 * **One thread at a time.** A connection is not safe to use from two threads at once - not
 * even one sending while another receives, because receiving answers pings. Give the
 * connection to one thread, or guard it.
 *
 * Hosted only; `PROVEN_NO_NET` leaves it out.
 */

/** @brief A WebSocket connection. Opaque; made by one of the three functions below. */
typedef struct proven_ws_conn proven_ws_conn_t;

/**
 * @brief How a connection behaves. Zero-initialise it and set `alloc`; every zero field has
 *        the default written beside it.
 */
typedef struct {
    proven_allocator_t alloc;           /**< required: the connection, its buffers, and the message being assembled */
    proven_size_t max_message_bytes;    /**< largest message accepted, fragments together; 0: 1 MiB */
    proven_u32 io_timeout_ms;           /**< the limit for each write; 0: 30 s */
    proven_u32 close_timeout_ms;        /**< how long proven_ws_conn_close waits for the peer's close; 0: 5 s */
    proven_u32 ping_interval_ms;        /**< silence from the peer after which a ping is sent, as ws_event's: there is no timer, so it goes out while proven_ws_conn_receive waits - a receive reads no longer than to it - or at the next receive. 0: 30 s */
    proven_u32 pong_timeout_ms;         /**< further silence after which the connection is ended: proven_ws_conn_receive returns PROVEN_ERR_RESET, close code 1006, as when the peer drops it. Any byte from the peer counts as an answer. 0: 10 s */
    bool compress;                      /**< compressed messages (`permessage-deflate`): a client offers it, a server accepts it when offered. See proven_ws_conn_compressed. With it set, do not pass a `Sec-WebSocket-Extensions` header of your own to proven_ws_conn_connect: the connection writes one. false: neither */
    int compress_level;                 /**< 1 (fastest) to 9 (smallest); -1 stores without compressing. 0: 6 */
    int compress_window_bits;           /**< how far back the compressor looks, 9 to 15: about 11 KiB of memory at 9, 28 KiB at 11, 325 KiB at 15 (without takeover only during one send, and less for a message smaller than the window). 0: 11 with takeover, 15 without */
    bool compress_no_takeover;          /**< with `compress`: compress each message on its own, keeping nothing between messages (no context takeover), as before v0.80.0. Short, similar messages then shrink far less, and an idle connection holds no compression state. false: each side may keep its window (see proven_ws_conn_compressed) */
    bool compress_peer_no_takeover;     /**< with `compress` and takeover: this side keeps its own window, and the other side must compress each message on its own - so this side needs no decompressor between messages (about 35 KiB less), and the other side's short, similar messages shrink far less. proven_ws_conn_accept answers `client_no_context_takeover` unasked; proven_ws_conn_connect offers `server_no_context_takeover`, and a server that will not honour it leaves the connection uncompressed. proven_ws_conn_open refuses it (nothing is negotiated there). false: the other side may keep its window too */
} proven_ws_conn_config_t;

/** @brief A received message. `data` is memory the connection owns, good until the next
 *         proven_ws_conn_receive, _close or _destroy. */
typedef struct {
    bool text;                          /**< text - UTF-8, already checked - rather than binary */
    proven_mem_view_t data;
} proven_ws_message_t;

/**
 * @brief Connect as a client.
 *
 * Sends the upgrade request through `client` - so its proxy, its timeouts, its credentials and
 * its `tls_wrap` apply - checks the answer, and takes the connection out of the client. The
 * client is not needed afterwards.
 *
 * @param url `ws://host[:port]/path` or `wss://...` (`http://` and `https://` mean the same).
 *        `wss` needs the client's `tls_wrap`; without one it is PROVEN_ERR_UNSUPPORTED.
 * @param headers extra request headers - `Origin`, a token - or NULL.
 * @param protocols the subprotocols to offer, as they go in the header (`chat, superchat`), or
 *        empty. What the server chose is proven_ws_conn_protocol.
 * @param http_status optional: receives the HTTP status of the answer - 101 on success, and
 *        otherwise what the server said instead (401, 404, ...), or 0 if there was no answer.
 *
 * @return PROVEN_ERR_REFUSED when the server answered with something other than 101 - see
 *         `*http_status`; PROVEN_ERR_INVALID_FORMAT when its 101 was not a correct answer to
 *         this handshake (a wrong accept value, a subprotocol or an extension that was not
 *         offered); PROVEN_ERR_INVALID_ARG; PROVEN_ERR_NOMEM; PROVEN_ERR_IO when no random
 *         bytes could be drawn for the key; and the errors of proven_http_client_send.
 */
[[nodiscard]]
proven_err_t proven_ws_conn_connect(proven_http_client_t *client, proven_u8str_view_t url,
                                    const proven_http_header_t *headers, proven_size_t header_count,
                                    proven_u8str_view_t protocols, const proven_ws_conn_config_t *config,
                                    proven_ws_conn_t **out, proven_u16 *http_status);

/**
 * @brief Accept as a server, inside an HTTP handler.
 *
 * Checks that the request is a WebSocket upgrade, answers 101, and takes the connection out of
 * the server (proven_http_exchange_upgrade). The connection then lives as long as you keep it:
 * use it in the handler, or hand it to a thread of your own and return.
 *
 * **Where it runs matters.** With handlers on the loop's thread, a handler that stays to talk
 * WebSocket stops the server for everyone else: hand the connection off and return. With
 * handlers on a job system, a handler that stays occupies one worker for as long as the
 * connection lasts.
 *
 * @param protocol the subprotocol to select - it must be one the request offers
 *        (proven_ws_request_offers) - or empty for none.
 *
 * @return PROVEN_ERR_NOT_FOUND when the request is not a WebSocket upgrade at all: nothing was
 *         sent, answer it as an ordinary request. PROVEN_ERR_UNSUPPORTED (a 426 was sent) for
 *         another protocol version; PROVEN_ERR_INVALID_FORMAT (a 400 was sent) for a malformed
 *         upgrade. PROVEN_ERR_INVALID_ARG for a `protocol` the request does not offer, and
 *         PROVEN_ERR_NOMEM: nothing was sent and you may still respond. PROVEN_ERR_BUSY (a 503 with
 *         `Retry-After: 1` was sent) when the server's `max_upgraded` connections are open. Otherwise the errors of
 *         proven_http_exchange_upgrade - and PROVEN_ERR_OUT_OF_BOUNDS when the client sent more
 *         than 16 KiB of frames along with its handshake: the 101 has gone by then, and the
 *         connection is closed.
 */
[[nodiscard]]
proven_err_t proven_ws_conn_accept(proven_http_exchange_t *exchange, proven_u8str_view_t protocol,
                                   const proven_ws_conn_config_t *config, proven_ws_conn_t **out);

/**
 * @brief Make a connection over a transport that is already past its opening handshake.
 *
 * For a transport you upgraded yourself, or a pair of in-memory transports in a test. On
 * success the connection owns the transport and closes it; on failure it is still yours.
 *
 * @param is_server true for the server's end: it expects masked frames and sends unmasked ones.
 * @param early bytes of the WebSocket stream that were read together with the HTTP handshake.
 *        They are copied.
 * No handshake is made here, so nothing is negotiated: `config->compress` says that
 * `permessage-deflate` **was** agreed - with context takeover in both directions, or with none
 * under `compress_no_takeover` - and both ends must say the same. (An end before v0.80.0
 * always means none: give the other end `compress_no_takeover`.) `compress_peer_no_takeover`
 * is refused here: it is an agreement only a handshake can make.
 *
 * @return PROVEN_ERR_INVALID_ARG (also for a compression level or window outside its range, and for `compress_peer_no_takeover` with takeover);
 *         PROVEN_ERR_NOMEM; PROVEN_ERR_OUT_OF_BOUNDS when `early` is larger than the
 *         connection's read buffer (16 KiB); PROVEN_ERR_IO when a client could not draw random
 *         bytes for its masks.
 */
[[nodiscard]]
proven_err_t proven_ws_conn_open(proven_transport_t transport, bool is_server, proven_mem_view_t early,
                                 const proven_ws_conn_config_t *config, proven_ws_conn_t **out);

/**
 * @brief Send a text message.
 * @return PROVEN_ERR_INVALID_ENCODING when `text` is not UTF-8 - nothing is sent, because the
 *         peer would have to end the connection over it. PROVEN_ERR_INVALID_STATE after a
 *         close, or in the middle of a message begun with proven_ws_conn_send_part.
 *         PROVEN_ERR_TIMEOUT or PROVEN_ERR_RESET when the peer does not take it; the
 *         connection is then finished and every later call returns the same error.
 */
[[nodiscard]]
proven_err_t proven_ws_conn_send_text(proven_ws_conn_t *conn, proven_u8str_view_t text);

/** @brief Send a binary message. Errors as proven_ws_conn_send_text, without the encoding check. */
[[nodiscard]]
proven_err_t proven_ws_conn_send_binary(proven_ws_conn_t *conn, proven_mem_view_t data);

/**
 * @brief Send a message in pieces, when it is produced as it goes or its size is not known.
 *
 * The first call begins a message as text or binary; each call sends one fragment; the call
 * with `last` true ends it. `text` is read on the first call only. Text sent this way is NOT
 * checked: the pieces together must be UTF-8, and a character may be cut between two of them.
 * Pings may be sent between pieces; other messages may not.
 */
[[nodiscard]]
proven_err_t proven_ws_conn_send_part(proven_ws_conn_t *conn, bool text, proven_mem_view_t data, bool last);

/**
 * @brief Send a ping with up to 125 bytes of payload. The pong is counted when it arrives
 *        during a proven_ws_conn_receive - see proven_ws_conn_pong_count.
 * @return PROVEN_ERR_INVALID_ARG for a longer payload.
 */
[[nodiscard]]
proven_err_t proven_ws_conn_ping(proven_ws_conn_t *conn, proven_mem_view_t data);

/**
 * @brief Receive the next message.
 *
 * Waits until a whole message has arrived or until `until`. Pings are answered and pongs
 * counted on the way.
 *
 * @return PROVEN_ERR_TIMEOUT when no complete message arrived in time; nothing is lost, call
 *         again. PROVEN_ERR_EOF when the peer closed: its code and reason are
 *         proven_ws_conn_close_code and proven_ws_conn_close_reason, and the close has been
 *         answered. PROVEN_ERR_RESET when the connection ended without a close frame (code
 *         1006). PROVEN_ERR_INVALID_FORMAT, PROVEN_ERR_INVALID_ENCODING and
 *         PROVEN_ERR_OUT_OF_BOUNDS when the peer broke the protocol, sent text that is not
 *         UTF-8, or sent a message past `max_message_bytes`: it has been sent a close saying
 *         so (1002, 1007, 1009), and what it was still sending has been read away for up to a
 *         second so that the close reaches it. After anything but TIMEOUT the connection is
 *         finished.
 */
[[nodiscard]]
proven_err_t proven_ws_conn_receive(proven_ws_conn_t *conn, proven_net_deadline_t until, proven_ws_message_t *out);

/**
 * @brief Close: send a close frame and wait for the peer's.
 *
 * Messages that arrive while waiting are discarded. Safe to call when the peer has already
 * closed, or twice. The connection still has to be destroyed.
 *
 * @param code PROVEN_WS_CLOSE_NORMAL usually; any code proven_ws_close_code_is_valid accepts,
 *        or PROVEN_WS_CLOSE_NO_STATUS for a close frame with no code.
 * @param reason at most 123 bytes of UTF-8, for a person reading a log; may be empty.
 * @return PROVEN_OK when both sides have closed; PROVEN_ERR_TIMEOUT when the peer did not
 *         answer within `close_timeout_ms`; PROVEN_ERR_INVALID_ARG for a code or reason that
 *         may not be sent (nothing is sent); the connection's error if it had already failed.
 */
proven_err_t proven_ws_conn_close(proven_ws_conn_t *conn, proven_u16 code, proven_u8str_view_t reason);

/** @brief Close the transport and free the connection. Without a proven_ws_conn_close first,
 *         the peer sees the connection drop (code 1006). NULL is ignored. */
void proven_ws_conn_destroy(proven_ws_conn_t *conn);

/** @brief The subprotocol in use; empty when none was agreed. Good until destroy. */
[[nodiscard]]
proven_u8str_view_t proven_ws_conn_protocol(const proven_ws_conn_t *conn);

/** @brief The peer's close code once proven_ws_conn_receive has returned PROVEN_ERR_EOF;
 *         PROVEN_WS_CLOSE_ABNORMAL (1006) when the connection ended without one; 0 while open. */
[[nodiscard]]
proven_u16 proven_ws_conn_close_code(const proven_ws_conn_t *conn);

/** @brief The reason that came with the peer's close; often empty. Good until destroy. */
[[nodiscard]]
proven_u8str_view_t proven_ws_conn_close_reason(const proven_ws_conn_t *conn);

/**
 * @brief Whether messages on this connection may be compressed: `compress` was set and the
 *        other side agreed.
 *
 * When it is on, nothing changes in how the connection is used. Each side may keep its
 * window from one message to the next (**context takeover**), unless the configuration says
 * `compress_no_takeover` or the other side forbade it: a message then refers back to the
 * messages before it, so short, similar messages shrink to a third or less. A message of 8
 * bytes or more is compressed; shorter ones go as they are. The price is memory for the life
 * of the connection, from its first compressed message: a compressor (about 28 KiB at the
 * default window of 11 bits) and a decompressor (about 35 KiB). Without takeover a message of
 * 64 bytes or more is compressed on its own and nothing is kept between messages. A
 * compressed message that arrives is inflated into the
 * message buffer: `max_message_bytes` bounds the **inflated** size, and a message that would
 * pass it ends the connection with 1009, as an uncompressed one does. Bytes that do not
 * inflate, like text that is not UTF-8, end it with 1007.
 *
 * Raw DEFLATE carries no checksum. A compressed message damaged on the way can inflate to
 * other bytes without an error: use TLS, or check what matters yourself.
 *
 * **Do not turn it on for messages that carry a secret together with text the other side
 * chose** - the length of the compressed message tells an observer of the connection how
 * much the two have in common, TLS or not. With takeover the secret and the chosen text need
 * not be in the same message: earlier messages change a later one's length too.
 */
[[nodiscard]]
bool proven_ws_conn_compressed(const proven_ws_conn_t *conn);

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
void proven_ws_conn_set_compress(proven_ws_conn_t *conn, bool on);

/** @brief How many pongs have arrived. Send a ping, receive for a while, and see whether this
 *         moved: that is how a program finds out a silent connection is dead. */
[[nodiscard]]
proven_u64 proven_ws_conn_pong_count(const proven_ws_conn_t *conn);

#endif /* PROVEN_WS_CONN_H */
