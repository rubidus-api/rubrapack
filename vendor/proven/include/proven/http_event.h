#ifndef PROVEN_HTTP_EVENT_H
#define PROVEN_HTTP_EVENT_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/u8str.h"
#include "proven/allocator.h"
#include "proven/net.h"
#include "proven/http.h"
#include "proven/loop.h"
#include "proven/tls.h"

/**
 * @file http_event.h
 * @brief An HTTP/1.1 server driven by events, for many connections on one thread.
 *
 * http_server.h gives a handler a request and lets it read and write with calls that wait.
 * That is the simplest thing to write, and each request being handled occupies a thread - or
 * the whole server, if there is one thread. This unit is the other trade: **nothing here
 * waits.** The server sits on a `proven_loop_t` (loop.h) and tells your functions what
 * happened - a request arrived, a piece of its body, room to write more - and they return at
 * once. One thread can then carry very many connections, because a connection that is doing
 * nothing costs a small struct and no buffer.
 *
 * What you take on in exchange:
 *
 *   - **Your functions must not wait.** A handler that needs a file, a database or a
 *     computation hands it to another thread and answers when the result is posted back to
 *     the loop (proven_loop_post). A response may be sent at any later time; the stream stays
 *     valid until `on_done`.
 *   - **Writing can be refused.** proven_http_stream_write takes what fits under the output
 *     limit and says how much; you offer the rest when `on_writable` is called. That is
 *     backpressure: a slow client makes your producer slow, instead of making the server's
 *     memory grow.
 *   - **Views are short-lived.** The request head given to `on_request`, and the piece given
 *     to `on_body`, are good only until that function returns. Copy what you keep.
 *
 * Everything here is called on the loop's thread, and only there.
 *
 * Hosted only; `PROVEN_NO_NET` leaves it out.
 */

/** @brief One request and its response on a connection. Valid from `on_request` until
 *         `on_done` returns. */
typedef struct proven_http_stream proven_http_stream_t;

/** @brief The server. Opaque. */
typedef struct proven_http_event_server proven_http_event_server_t;
/** Sockets for several loops on one port (RFC-0037): proven_http_event_port_open. */
typedef struct proven_http_event_port proven_http_event_port_t;

typedef struct {
    /** A request head has arrived and passed the server's checks. Required. Respond here, or
     *  remember `stream` and respond later from the loop's thread. */
    void (*on_request)(void *ctx, proven_http_stream_t *stream, const proven_http_request_t *head);
    /** A piece of the request body; `last` with the final one (which may be empty). Optional:
     *  without it a body is read and thrown away. */
    void (*on_body)(void *ctx, proven_http_stream_t *stream, proven_mem_view_t piece, bool last);
    /** Output that was held back has gone: proven_http_stream_write will take more. Optional. */
    void (*on_writable)(void *ctx, proven_http_stream_t *stream);
    /** The exchange is over, called exactly once for every `on_request`: PROVEN_OK when the
     *  whole response was handed to the connection, otherwise why it could not be (the client
     *  left, a timeout, a protocol error). Free what you attached to the stream here. Optional. */
    void (*on_done)(void *ctx, proven_http_stream_t *stream, proven_err_t why);
} proven_http_event_callbacks_t;

/**
 * @brief How a server behaves. Zero-initialise it, set `on.on_request`, and set what you
 *        need: every zero field has the default written beside it.
 */
typedef struct {
    proven_allocator_t alloc;            /**< not valid: the loop's allocator */
    proven_http_event_callbacks_t on;
    void *ctx;                           /**< passed to every callback */
    proven_size_t max_connections;       /**< open at once; further ones wait in the backlog. 0: 10,000 */
    proven_size_t max_head_bytes;        /**< largest request head. 0: 16 KiB */
    proven_size_t max_headers;           /**< most header fields in a request. 0: 64 */
    proven_u64 max_body_bytes;           /**< largest request body. 0: 1 MiB */
    proven_size_t max_buffered_output;   /**< response bytes held for one connection before writes are refused. 0: 64 KiB */
    proven_u32 head_timeout_ms;          /**< from a connection's (or a request's) first byte to the end of the head; with TLS it covers the handshake too. 0: 10 s */
    proven_u32 body_timeout_ms;          /**< between pieces of a request body. 0: 30 s */
    proven_u32 write_timeout_ms;         /**< output held with none of it accepted by the client. 0: 30 s */
    proven_u32 idle_timeout_ms;          /**< an open connection with no request on it. 0: 60 s */
    proven_u32 linger_ms;                /**< how long a connection that is being closed after an answer is still read from, so that the answer is not lost to a reset. It counts against `max_connections` for that long. 0: 1 s */
    const proven_tls_config_t *tls;      /**< NULL: plain HTTP. Otherwise every connection is TLS; must outlive the server. */
    int compress_level;                  /**< for responses that ask to be compressed (proven_http_stream_compress): 1 (fastest) to 9 (smallest); -1 stores without compressing. 0: 6 */
    int compress_window_bits;            /**< and how far back the compressor looks, 9 to 15: each response being streamed compressed holds from about 11 KiB (9) to about 325 KiB (15) until it ends. 0: 15 */
    bool http2;                          /**< true: a connection may be HTTP/2 - over TLS when the handshake chose `h2` (the TLS configuration must offer it, and must not when this is false), in the clear when the client begins with the HTTP/2 preface. Every other connection is HTTP/1.1. The callbacks and the stream functions are the same for both */
    proven_size_t max_concurrent_streams; /**< HTTP/2: requests in progress on one connection. 0: 100 */
    proven_size_t http2_header_table_bytes; /**< HTTP/2: the header compression table in each direction, which an idle connection keeps once it has been used - about one and a half times this, twice over. 0: 4,096, the protocol's default */
    bool http2_no_header_table;          /**< HTTP/2: no such tables at all: an idle connection is then about 1.5 KiB instead of about 12.8, and every request and response head is sent in full */
    proven_size_t http2_window_bytes;    /**< HTTP/2: request-body bytes a client may send on one stream, and on a connection as a whole, before the handler has taken them (flow control, RFC 9113 section 5.2). It is what a request that is paused can make a connection hold, and it bounds how fast a large body can arrive: a client must wait for room each time it has sent this much. 0: it starts at 65,535, the protocol's default, and grows - doubling while the bodies are read as fast as they come, up to 16 MiB per stream and 24 MiB per connection (since v0.75.0, as `http3_window_bytes`; RFC-0019); a body not read stops the growth. A value set here is fixed. At most 2^31-1. proven_http_event_server_set_window lowers either while the server runs */
    bool http2_websocket;                /**< HTTP/2: take WebSocket on an HTTP/2 connection (RFC 8441) - the server says SETTINGS_ENABLE_CONNECT_PROTOCOL, and a client that knows it opens its WebSocket as a stream there instead of a connection of its own. The handler is the same: proven_ws_event_accept answers either kind. Each such WebSocket holds one of `max_concurrent_streams` for its life. Needs `http2` */
    bool compress_zstd;                  /**< offer Zstandard too: a response asked to be compressed goes as `zstd` when the request's `Accept-Encoding` weighs zstd at least as high as gzip (curl and browsers send both at one weight), and as gzip otherwise, as before. Faster than gzip by several times, at about gzip 1's size or less. false: gzip only */
    int compress_zstd_level;             /**< zstd.h's level, 1 (fastest) to 9; 6 is about gzip 6's size at three times its speed, and its tables are 1 MiB for a response streamed. 0: 1 */
    int compress_zstd_window_log;        /**< zstd.h's window, 10 to 23. A response written whole is compressed in place with tables sized to it; one streamed holds the window and 128 KiB as input buffer, 64 KiB of hash table at level 1 (768 KiB at 3), and about 500 KiB for a block, until it ends - about 0.8 MiB at the defaults. 0: 17 (128 KiB) */
    bool http3;                          /**< true: HTTP/3 too (RFC 9114, over QUIC, RFC 9000). Needs `tls`, whose configuration must offer `h3` (and must not when this is false). Each proven_http_event_server_listen then opens a UDP socket on the same address and port as well - a listen whose UDP socket cannot be opened fails - and HTTP/1.1 and HTTP/2 responses carry `Alt-Svc: h3=":PORT"` unless the handler wrote an `Alt-Svc` of its own. The callbacks and the stream functions are the same for all three; WebSocket is not taken over HTTP/3. Of the fields above, `max_connections` counts QUIC connections too; `max_concurrent_streams` is also the request streams a client may open at once on one; `max_head_bytes` is also the largest field section taken; `head_timeout_ms` covers the QUIC handshake; and a QUIC connection on which nothing at all arrives is let go of a second after the larger of `idle_timeout_ms` and `head_timeout_ms` (QUIC's idle timeout). A QUIC connection holds up to `http3_window_bytes` of request bodies that have arrived and not been taken (per stream, and in all), and up to 256 KiB per stream and 1 MiB in all of response bytes sent and not yet acknowledged - that, not `max_buffered_output`, is what bounds writes on it. Connections are told apart by their connection IDs on one socket; proven_http_event_server_adopt takes TCP connections only - several loops share one HTTP/3 port through proven_http_event_port_open (Linux). In a build with PROVEN_NO_HTTP3, proven_http_event_server_create returns PROVEN_ERR_UNSUPPORTED */
    bool http3_retry;                    /**< HTTP/3: answer every client's first packet with a Retry (RFC 9000 section 8.1.2), so that no state is kept for an address until it has shown that it receives there - one round trip more for every connection, for a server under load. Without it no more than three times what an address has sent is sent to it before it is validated, as always */
    proven_size_t http3_window_bytes;    /**< HTTP/3: request-body bytes a client may send on one stream, and on a connection as a whole, before the handler has taken them (QUIC's flow control, RFC 9000 section 4) - as `http2_window_bytes` is for HTTP/2. 0: it starts at 256 KiB per stream and 1 MiB per connection and grows - doubling while the bodies are read as fast as they come, up to 16 MiB per stream and 24 MiB per connection (since v0.71.0, RFC-0019); a body not read stops the growth. A value set here is fixed, as before. At most 2^31-1. proven_http_event_server_set_window lowers either while the server runs */
    proven_size_t http3_header_table_bytes; /**< HTTP/3: the QPACK dynamic table a client's encoder may fill for this server's decoder (RFC 9204; RFC-0032), said in SETTINGS with 16 streams that may wait for its inserts. Made only when a client first uses it: then about twice this per connection. 0: 4,096. 32 to 1 MiB. The same size bounds the table the server's encoder fills for the client (since v0.96.0), when the client allows one: it refers only to entries the client has acknowledged, so a client never waits, and never inserts a field marked sensitive or a short credential */
    bool http3_no_header_table;          /**< HTTP/3: no such table in either direction: every head goes in full, as before v0.95.0 */
} proven_http_event_server_config_t;

/**
 * @brief Make a server on `loop`. It serves once proven_http_event_server_listen has been
 *        called and the loop runs.
 * @return PROVEN_ERR_INVALID_ARG (no loop, no `on_request`, a TLS config with no certificate,
 *         a compression level or window outside its range, `http3` without `tls`),
 *         PROVEN_ERR_UNSUPPORTED (`http3` in a build without it), PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_err_t proven_http_event_server_create(proven_loop_t *loop, const proven_http_event_server_config_t *config,
                                             proven_http_event_server_t **out);

/** @brief Listen at `at` (up to four addresses per server). `bound` (optional) receives the
 *         address in use, with the port the system chose when `at` asked for port 0. With
 *         `http3`, a UDP socket on the same address and port too: when it cannot be had, the
 *         TCP listener is closed again and the error returned - except that for port 0, when
 *         the UDP port with the number TCP was given is in use, another is tried (up to 17
 *         numbers in all), so that the port is one free for both. */
[[nodiscard]]
proven_err_t proven_http_event_server_listen(proven_http_event_server_t *server, proven_net_addr_t at, proven_net_addr_t *bound);

/**
 * @brief One port for several loops, HTTP/3 included (Linux): a TCP listener per loop, and
 *        with `http3` a UDP socket per loop, all on one address and port with SO_REUSEPORT.
 *
 * The system spreads TCP connections over the listeners. A UDP datagram goes to the socket of
 * the loop that owns its QUIC connection, chosen by a small classic BPF program from the
 * connection ID - not from the client's address, so a client whose address changes is still
 * answered by its loop. Each server takes its share with proven_http_event_server_listen_port.
 * Everything is opened here at once, on the calling thread, in order (the order is what the
 * program counts by). `bound` (optional) receives the address, with the port chosen when `at`
 * asked for port 0 - one free for both, as for proven_http_event_server_listen.
 *
 * Any process of the same user can join a port opened with SO_REUSEPORT. A QUIC connection ID
 * from a port of `loops` loops shows, in its first byte, which loop issued it (log2(loops)
 * bits, the same for the connections of that loop).
 *
 * @return PROVEN_ERR_INVALID_ARG (`loops` not 1 to 255, no allocator, no `out`),
 *         PROVEN_ERR_UNSUPPORTED (not Linux; `http3` in a build without it; a kernel without
 *         SO_REUSEPORT or its programs), PROVEN_ERR_NOMEM, or what opening a socket returned.
 */
[[nodiscard]]
proven_err_t proven_http_event_port_open(proven_allocator_t alloc, proven_net_addr_t at, proven_size_t loops, bool http3,
                                         proven_http_event_port_t **out, proven_net_addr_t *bound);

/**
 * @brief Serve loop `index`'s share of `port` - like proven_http_event_server_listen, but with
 *        the port's sockets for that index. Called on the server's loop thread; each index
 *        once. The server must not have listened before, and must have `http3` exactly when
 *        the port has. Its HTTP/3 then uses the port's key for Retry, NEW_TOKEN and stateless
 *        reset tokens, so a token one loop gave is good at another, and sends no stateless
 *        reset for a connection ID another loop issued.
 *
 * Stop the servers of a port together: a socket that closes while others of its port go on
 * changes where the system sends datagrams, and connections of other loops can be lost.
 *
 * @return PROVEN_ERR_INVALID_ARG (`index` past the port's loops, `http3` differing);
 *         PROVEN_ERR_INVALID_STATE (the index taken, a server that has listened or is
 *         shutting down); PROVEN_ERR_NOMEM; or what watching a socket returned - after which
 *         the port's UDP socket for `index` is closed and the port is not to be used further.
 */
[[nodiscard]]
proven_err_t proven_http_event_server_listen_port(proven_http_event_server_t *server, proven_http_event_port_t *port, proven_size_t index);

/** @brief Close the sockets of `port` no server took, and free it. Call it once every
 *         proven_http_event_server_listen_port on it has returned; the servers keep what they
 *         took. Null is accepted. */
void proven_http_event_port_close(proven_http_event_port_t *port);

/**
 * @brief Close every connection and the listeners, and free the server. Exchanges still in
 *        progress get `on_done` with PROVEN_ERR_RESET. The loop is not stopped or freed.
 *        Null is accepted.
 *
 * What the loop has not yet answered - a listener's accept in flight, a connection whose
 * socket it may still use - keeps its memory, and the server's own struct, until the loop's
 * next round, or until proven_loop_destroy: from then on nothing of the server is left.
 */
void proven_http_event_server_destroy(proven_http_event_server_t *server);

/**
 * @brief Take a connection that was accepted elsewhere: it becomes one of the server's, as if
 *        the server had accepted it (with TLS, the handshake begins).
 *
 * This is how several loops share one listening port: a thread accepts, and hands each
 * connection to one of several servers, each on a loop of its own. **Like everything else
 * here it must be called on the server's loop thread**, so the accepting thread posts it
 * (proven_loop_post) - and a `proven_net_conn_t` is not to be copied while open, so what is
 * posted carries the connection in memory of its own, not a copy on a stack.
 *
 * On PROVEN_OK the server owns the connection and `*conn` is no longer open. On any error it
 * is still yours, to close.
 *
 * @return PROVEN_ERR_INVALID_ARG; PROVEN_ERR_INVALID_STATE when `conn` is not open, or after
 *         proven_http_event_server_stop_listening; PROVEN_ERR_BUSY at `max_connections`;
 *         PROVEN_ERR_NOMEM; or what watching the socket returned.
 */
[[nodiscard]]
proven_err_t proven_http_event_server_adopt(proven_http_event_server_t *server, proven_net_conn_t *conn);

/** @brief Connections open now. */
proven_size_t proven_http_event_server_connections(const proven_http_event_server_t *server);

/**
 * @brief Change `max_concurrent_streams` while the server runs - to shed load, say, and to
 *        take it back: for the connections open now and for those to come.
 *
 * `n` is 1 to the configured value, which stays the ceiling. On the loop's thread.
 *
 * HTTP/2: each connection says the new number to its client (proven_h2_conn_set_max_streams):
 * requests already running go on; until the client acknowledges, it may still begin requests
 * under the number before, and those are taken; after, one over the number is refused with
 * REFUSED_STREAM, which a client may send again. A WebSocket on an HTTP/2 connection holds one
 * of the streams, as before.
 *
 * HTTP/3: what a client was allowed already cannot be taken back (RFC 9000 section 4.6). From
 * now on each request that ends lets the client begin another only while it is under `n`, so
 * a lower number shows once enough requests have ended.
 *
 * To take no new requests at all, end the server: proven_http_event_server_shutdown.
 *
 * @return PROVEN_ERR_INVALID_ARG for a null server, 0, or a value above the configured one.
 */
[[nodiscard]]
proven_err_t proven_http_event_server_set_max_streams(proven_http_event_server_t *server, proven_size_t n);

/**
 * @brief Change while the server runs how many bytes of request body a client may send ahead
 *        of the handler - on one stream, and on one connection as a whole - for the
 *        connections open now and for those to come: a ceiling on that credit, to shed memory
 *        under load and to give it back. HTTP/2 and HTTP/3. On the loop's thread.
 *
 * From now on room is given back only up to `n`: what the client may still send and what the
 * server holds unread, together. Windows that grow (`http2_window_bytes` / `http3_window_bytes`
 * left 0) stop growing at `n`, and grow again past it when `n` is raised; a configured window
 * is the most a protocol gives whatever `n` is. **What was granted stands** - HTTP/3 credit
 * cannot be taken back (RFC 9000 section 4), and an HTTP/2 window set lower holds once the
 * client acknowledges it - so a lower `n` shows as what is outstanding is used up, as a lower
 * proven_http_event_server_set_max_streams does.
 *
 * HTTP/2: each connection says `min(n, its window)` as its initial stream window
 * (proven_h2_conn_set_initial_window). HTTP/3: on a connection open now, a stream the client
 * opens later may still use the window promised in the handshake; connections to come are
 * promised at most `n`.
 *
 * @return PROVEN_ERR_INVALID_ARG for a null server, 0, or more than 2^31-1.
 */
[[nodiscard]]
proven_err_t proven_http_event_server_set_window(proven_http_event_server_t *server, proven_size_t n);

/** @brief Stop accepting - and adopting; connections that are open go on, but an HTTP/1.1
 *         response from now on says `Connection: close` and its connection ends after it, and
 *         an HTTP/2 or HTTP/3 connection says goodbye (GOAWAY) and ends when its open streams
 *         have. (HTTP/3 says it twice, a round trip apart, so that a request already on its way
 *         is still answered - RFC 9114 section 5.2 - and its UDP socket stays open until its
 *         last connection has ended.)
 *         (proven_http_event_server_listen afterwards takes connections again.) To end the
 *         server with a deadline, use proven_http_event_server_shutdown. */
void proven_http_event_server_stop_listening(proven_http_event_server_t *server);

/** @brief Told when a server's shutdown is over: `why` is PROVEN_OK when everything that was
 *         open finished, PROVEN_ERR_TIMEOUT when something was cut at the deadline. */
typedef void (*proven_http_event_drained_fn)(void *ctx, proven_err_t why);

/**
 * @brief End the server gracefully. On the loop's thread; returns at once.
 *
 * From the call: nothing new is taken (as proven_http_event_server_stop_listening); an
 * HTTP/1.1 connection with no request on it is closed now; one where a request has begun to
 * arrive, or is being answered, finishes it, with `Connection: close`, and closes; an HTTP/2
 * connection says goodbye and finishes its open streams; a WebSocket connection is closed with
 * 1001 ("going away") and its peer's answer waited for. When `grace_ms` has passed, whatever
 * is still open is ended: an exchange in progress gets `on_done(PROVEN_ERR_RESET)`, a
 * WebSocket `on_closed`. 0: end it all now.
 *
 * `on_drained(ctx, why)` (optional) is called once, from the loop, when the last connection has
 * gone - on the next round when none was open. It may call proven_loop_stop; it must not
 * destroy the server, which is done after the loop has returned. The deadline bounds
 * connections, not handlers: a handler keeps running until it returns.
 *
 * A shutdown is final: the server takes no connection again (listen and adopt are
 * PROVEN_ERR_INVALID_STATE). proven_http_event_server_destroy during it ends it at once.
 *
 * @return PROVEN_ERR_INVALID_ARG for no server; PROVEN_ERR_INVALID_STATE when it is already
 *         shutting down.
 */
[[nodiscard]]
proven_err_t proven_http_event_server_shutdown(proven_http_event_server_t *server, proven_u32 grace_ms,
                                               proven_http_event_drained_fn on_drained, void *ctx);

/**
 * @brief Ask that the response of this stream be compressed. Call it before the response
 *        begins - in `on_request` or later; afterwards it does nothing.
 *
 * Nothing is compressed unless asked for, response by response, because whether it is safe is
 * a property of the response: **do not ask for one that carries a secret together with text
 * the client chose**. The length of a compressed body tells an observer of the connection how
 * much the two have in common, TLS or not (the attack known as BREACH).
 *
 * The response then carries `Vary: Accept-Encoding` (unless your `Vary` already names it) and,
 * when the request's `Accept-Encoding` allows gzip, is sent as `Content-Encoding: gzip`. It
 * is sent as it is when the client does not accept gzip; when the status is 204, 206 or 304;
 * when your headers have a `Content-Encoding` or a `Content-Range` of their own; when memory
 * for the compressor cannot be had; and, with proven_http_stream_respond, when the body is
 * under 256 bytes or does not get smaller.
 *
 * With proven_http_stream_begin the body leaves chunked whatever length was given (the length
 * is still what you must write). proven_http_stream_write takes input while the compressed
 * output held for the connection is under its limit, and **what it takes may wait inside the
 * compressor** until more is written or the response ends. For a response that the client
 * must see piece by piece, such as an event stream, call proven_http_stream_flush after each
 * piece - or do not compress it. proven_http_stream_buffered counts compressed bytes.
 */
void proven_http_stream_compress(proven_http_stream_t *stream);

/** @brief The length to give proven_http_stream_begin when it is not known in advance. */
#define PROVEN_HTTP_EVENT_LENGTH_UNKNOWN UINT64_MAX

/**
 * @brief Send a whole response: status, your headers, a body held in memory.
 *
 * The body is copied, whatever its size - for a large or generated body use
 * proven_http_stream_begin and proven_http_stream_write, which respect the output limit.
 * `Content-Length`, `Date` and `Connection` are written for you and may not be among
 * `headers`. As after proven_http_stream_end, `on_done` follows - possibly inside this call.
 *
 * @return PROVEN_ERR_INVALID_STATE when a response was already begun or the stream is over;
 *         PROVEN_ERR_INVALID_ARG for a status outside 200-999 or a header the server writes
 *         itself; PROVEN_ERR_OUT_OF_BOUNDS when the head does not fit `max_head_bytes`;
 *         PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_err_t proven_http_stream_respond(proven_http_stream_t *stream, proven_u16 status,
                                        const proven_http_header_t *headers, proven_size_t header_count,
                                        proven_mem_view_t body);

/**
 * @brief Begin a response whose body will follow in pieces.
 * @param content_length the body's size, or PROVEN_HTTP_EVENT_LENGTH_UNKNOWN to send it chunked.
 * @return as proven_http_stream_respond; PROVEN_ERR_RESET when the connection or the stream ended
 *         over it - `on_done` has then been called, inside this call, and the stream may not be
 *         written to.
 */
[[nodiscard]]
proven_err_t proven_http_stream_begin(proven_http_stream_t *stream, proven_u16 status,
                                      const proven_http_header_t *headers, proven_size_t header_count,
                                      proven_u64 content_length);

/**
 * @brief Offer body bytes. **It may take fewer than you offer** - as many as fit under the
 *        output limit, possibly none. Keep the rest and offer it again when `on_writable` is
 *        called.
 * @return the number of bytes taken; PROVEN_ERR_INVALID_STATE before proven_http_stream_begin,
 *         after proven_http_stream_end, or when the stream is over; PROVEN_ERR_OUT_OF_BOUNDS
 *         for more than the Content-Length that was promised; PROVEN_ERR_RESET when the
 *         connection or the stream ended over it - `on_done` has then been called, inside this
 *         call, so neither the stream nor what `on_done` freed may be used after it returns.
 */
[[nodiscard]]
proven_result_size_t proven_http_stream_write(proven_http_stream_t *stream, proven_mem_view_t data);

/**
 * @brief Hand over what a compressed response holds back: everything written so far becomes
 *        readable by the client, and the response goes on.
 *
 * Only a response being compressed holds anything back (see proven_http_stream_compress);
 * for any other this does nothing, successfully. It is what makes a compressed response that
 * the client must see piece by piece possible: write an event, flush. Each flush costs a few
 * bytes and some of the compression, so flush where a reader is waiting and not after every
 * write. What comes out is queued whatever the output limit - no more than the compressor
 * held, which is bounded by its window and not by what you wrote.
 *
 * @return PROVEN_ERR_INVALID_STATE before proven_http_stream_begin, after
 *         proven_http_stream_end, or when the stream is over; PROVEN_ERR_RESET when the
 *         connection or the stream ended over it - `on_done` has then been called, inside this
 *         call, as for proven_http_stream_write.
 */
[[nodiscard]]
proven_err_t proven_http_stream_flush(proven_http_stream_t *stream);

/**
 * @brief Finish the response. `on_done` follows when everything has been handed to the
 *        connection - possibly inside this call.
 * @return PROVEN_ERR_INVALID_STATE when no response was begun or it was already ended;
 *         PROVEN_ERR_INVALID_FORMAT when fewer bytes were written than the Content-Length
 *         promised (the connection is then closed: the client must not take it for whole).
 */
[[nodiscard]]
proven_err_t proven_http_stream_end(proven_http_stream_t *stream);

/**
 * @brief Finish a response begun with PROVEN_HTTP_EVENT_LENGTH_UNKNOWN with trailer fields after
 *        its body (since v0.107.0): over HTTP/1.1 after the last chunk, over HTTP/2 and
 *        HTTP/3 as a HEADERS frame that ends the stream. A client that does not read trailers
 *        passes over them. The fields are copied.
 * @return PROVEN_ERR_INVALID_ARG for no fields, or one proven_http_trailer_allowed refuses (or not a
 *         valid field); PROVEN_ERR_INVALID_STATE for a response with a length, to HEAD, to an
 *         HTTP/1.0 client (it has no chunks), or one not begun or already ended;
 *         PROVEN_ERR_OUT_OF_BOUNDS past PROVEN_HTTP_MAX_TRAILER_BYTES; otherwise as proven_http_stream_end.
 */
[[nodiscard]]
proven_err_t proven_http_stream_end_trailers(proven_http_stream_t *stream, const proven_http_header_t *fields, proven_size_t count);

/**
 * @brief The request's trailer fields (since v0.107.0), once its body has ended - from `on_body`
 *        with `last` until `on_done` returns: views into the server's copy. None for a request
 *        whose body was not chunked (HTTP/1.1) or had none. A field that may not be a trailer
 *        (proven_http_trailer_allowed) is left out. Over HTTP/1.1 a chunked request body holds a
 *        buffer of PROVEN_HTTP_MAX_TRAILER_BYTES for its trailer section while it arrives.
 * @return PROVEN_ERR_INVALID_STATE before the body's end or after `on_done`; PROVEN_ERR_OUT_OF_BOUNDS
 *         for more fields than `cap`.
 */
[[nodiscard]]
proven_err_t proven_http_stream_trailers(const proven_http_stream_t *stream, proven_http_header_t *fields, proven_size_t cap, proven_size_t *count);

/** @brief Give up on this exchange: the connection is closed at once and `on_done` is called
 *         with PROVEN_ERR_RESET. */
void proven_http_stream_abort(proven_http_stream_t *stream);

/** @brief Stop delivering the request body (and stop reading the connection) until
 *         proven_http_stream_resume: backpressure in the other direction. */
void proven_http_stream_pause(proven_http_stream_t *stream);
/** @brief Deliver again. Pieces that were waiting may arrive inside this call. */
void proven_http_stream_resume(proven_http_stream_t *stream);

/** @brief Attach a pointer of yours to the stream, and get it back. */
void proven_http_stream_set_user(proven_http_stream_t *stream, void *user);
void *proven_http_stream_user(const proven_http_stream_t *stream);

/** @brief The client's address. */
proven_net_addr_t proven_http_stream_peer(const proven_http_stream_t *stream);

/** @brief Response bytes held for this connection and not yet accepted by the client. */
proven_size_t proven_http_stream_buffered(const proven_http_stream_t *stream);

/** @brief Which protocol carries this exchange: 1 for HTTP/1.1 (and 1.0), 2 for HTTP/2, 3 for HTTP/3. A
 *         handler seldom needs to know - the calls are the same - but a log may want to say. */
[[nodiscard]]
int proven_http_stream_protocol(const proven_http_stream_t *stream);

/** @brief Whether this request arrived as early data (0-RTT, over HTTP/3, when the TLS
 *         configuration has `early_data`) and is being handled before the handshake is complete.
 *         Such a request may be a copy an attacker replayed: act on it only if doing so twice is
 *         harmless, or answer 425 (Too Early) and the client sends it again after the handshake.
 *         Only GET, HEAD and OPTIONS without a body reach a handler this way; the server answers
 *         425 to anything else that arrives early. Always false over HTTP/1.1 and HTTP/2. */
[[nodiscard]]
bool proven_http_stream_early(const proven_http_stream_t *stream);

#endif /* PROVEN_HTTP_EVENT_H */
