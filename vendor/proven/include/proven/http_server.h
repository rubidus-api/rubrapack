#ifndef PROVEN_HTTP_SERVER_H
#define PROVEN_HTTP_SERVER_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/u8str.h"
#include "proven/allocator.h"
#include "proven/net.h"
#include "proven/http.h"
#include "proven/job.h"
#include "proven/tls.h"

/**
 * @file http_server.h
 * @brief An HTTP/1.1 server: the loop every server needs, and nothing above it.
 *
 * You give it a function. It accepts connections, reads and checks each request, calls your
 * function with the request, and sends what your function writes. Keep-alive, pipelined
 * requests, `Expect: 100-continue`, `HEAD`, chunked bodies in both directions, the `Date`
 * header, and the error responses for requests that are malformed, too large or too slow are
 * its business, not yours.
 *
 * What it is not: no router, no static-file handler, no middleware, no templates. Those are
 * programs written on top of it. A request path that is to name a file goes through
 * proven_url_path_resolve first - see url.h.
 *
 * **Every wait has a limit.** A client has `head_timeout_ms` to send its request head,
 * `body_timeout_ms` between pieces of its body, and `idle_timeout_ms` between requests; a
 * response write that the client does not accept within `write_timeout_ms` fails. A client that
 * sends half a request and stops costs one connection for one timeout, not the server.
 *
 * **Two ways to run handlers**, chosen by `jobs` in the configuration:
 *
 *   - `jobs` NULL: the handler runs on the thread that runs the loop. Simple, and safe for
 *     handlers that only compute. While a handler runs - or waits for a slow client's body -
 *     no other connection is served.
 *   - `jobs` set: each request is handed to that job system, and the loop goes back to
 *     accepting and reading. The handler reads the request body and writes the response
 *     directly on the connection from its worker thread; the connection belongs to it until it
 *     returns. Handlers then run concurrently and must be written for that.
 *
 * The loop waits on a selector (net.h) and keeps its timeouts on a timer wheel, so what a
 * round costs depends on the connections that have something to do, not on how many are open:
 * thousands of idle connections are held at no cost to the busy ones where the selector is
 * epoll or kqueue. On Windows it is built on an I/O completion port: measured on one Windows 11
 * virtual machine, ten busy connections among 10,000 held were answered as fast as among 1,000.
 * Two limits remain everywhere: each connection holds its buffers from the moment it is
 * accepted, and each request occupies a thread while its handler runs.
 *
 * Plain HTTP unless the config's `tls` is set (tls.h): then every connection is HTTPS, and a
 * handler sees no difference.
 *
 * Hosted only; `PROVEN_NO_NET` leaves it out.
 */

/** @brief A server. Opaque; made by proven_http_server_create. */
typedef struct proven_http_server proven_http_server_t;

/** @brief One request being handled: the argument of a handler. Opaque. */
typedef struct proven_http_exchange proven_http_exchange_t;

/**
 * @brief Your function: answer one request.
 *
 * Read what you need from `exchange`, then send a response with
 * proven_http_exchange_respond, or with proven_http_exchange_begin / _write / _end. Return when
 * done. If you return without having sent anything, the client gets a 500.
 *
 * The exchange, and every view obtained from it, is good only until the handler returns.
 */
typedef void (*proven_http_handler_fn)(void *ctx, proven_http_exchange_t *exchange);

/**
 * @brief How a server behaves. Zero-initialise it, set `alloc` and `handler`, and set what you
 *        need: every zero field has the default written beside it.
 */
typedef struct {
    proven_allocator_t alloc;           /**< required. With `jobs` set it is called from several threads and must be thread-safe (the heap allocator is). */
    proven_http_handler_fn handler;     /**< required */
    void *handler_ctx;
    proven_job_sys_t *jobs;             /**< NULL: handlers run on the loop's thread. Must outlive the server. */
    proven_size_t max_connections;      /**< connections open at once; further ones wait in the backlog. 0: 64 */
    proven_size_t max_head_bytes;       /**< largest request head (and response head). 0: 16 KiB */
    proven_size_t max_headers;          /**< most header fields in a request. 0: 64 */
    proven_u64 max_body_bytes;          /**< largest request body. 0: 1 MiB */
    proven_u32 head_timeout_ms;         /**< from a request's first byte to the end of its head. 0: 10 s */
    proven_u32 body_timeout_ms;         /**< for each read of a request body. 0: 30 s */
    proven_u32 write_timeout_ms;        /**< for each write of a response. 0: 30 s */
    proven_u32 idle_timeout_ms;         /**< an open connection with no request on it. 0: 60 s */
    proven_u32 linger_ms;               /**< how long a connection that is being closed after an answer is still read from, so that the answer is not lost to a reset (a refusal in the middle of an upload, mostly). It counts against `max_connections` for that long: a burst of refused requests holds those places. 0: 1 s */
    proven_size_t max_upgraded;         /**< connections taken out of the server by proven_http_exchange_upgrade (WebSocket, through proven_ws_conn_accept) that are open at once, counted until each is closed - from any thread, even after the server is gone. At the limit an upgrade is PROVEN_ERR_BUSY and proven_ws_conn_accept answers 503 with `Retry-After: 1`. 0: 1,024 */
    const proven_tls_config_t *tls;     /**< NULL: plain HTTP. Otherwise every connection is TLS under this config, which must outlive the server. The handshake shares `head_timeout_ms` with the first request's head. */
    int compress_level;                 /**< for responses a handler asks to have compressed (proven_http_exchange_compress): 1 (fastest) to 9 (smallest); -1 stores without compressing. 0: 6 */
    int compress_window_bits;           /**< and how far back the compressor looks, 9 to 15: its memory while such a response is being written runs from about 11 KiB at 9 to about 325 KiB at 15. 0: 15 */
    bool compress_zstd;                 /**< offer Zstandard too: a response asked to be compressed goes as `zstd` when the request's `Accept-Encoding` weighs zstd at least as high as gzip (curl and browsers send both at one weight), and as gzip otherwise, as before. Faster than gzip by several times, at about gzip 1's size or less. false: gzip only */
    int compress_zstd_level;            /**< zstd.h's level, 1 (fastest) to 9; 6 is about gzip 6's size at three times its speed, and its tables are 1 MiB for a response streamed. 0: 1 */
    int compress_zstd_window_log;       /**< zstd.h's window, 10 to 23. A response written whole is compressed in place with tables sized to it; one streamed holds the window and 128 KiB as input buffer, 64 KiB of hash table at level 1 (768 KiB at 3), and about 500 KiB for a block, until it ends - about 0.8 MiB at the defaults. 0: 17 (128 KiB) */
} proven_http_server_config_t;

/**
 * @brief Make a server. It does not listen yet.
 * @return PROVEN_ERR_INVALID_ARG for an invalid allocator, no handler, or a compression level
 *         or window outside its range; PROVEN_ERR_NOMEM;
 *         PROVEN_ERR_BUSY when the system is out of sockets.
 */
[[nodiscard]]
proven_err_t proven_http_server_create(const proven_http_server_config_t *config, proven_http_server_t **out);

/**
 * @brief Listen at `at`. May be called up to four times, for several addresses - an IPv4 and
 *        an IPv6 one, say. `bound` (optional) receives the address actually bound, so that
 *        port 0 can be used and the chosen port learned.
 * @return the errors of proven_net_listen; PROVEN_ERR_OUT_OF_BOUNDS for a fifth listener.
 */
[[nodiscard]]
proven_err_t proven_http_server_listen(proven_http_server_t *server, proven_net_addr_t at, proven_net_addr_t *bound);

/**
 * @brief Serve until proven_http_server_stop is called.
 * @return PROVEN_OK after a stop; another error when the loop itself cannot continue.
 */
[[nodiscard]]
proven_err_t proven_http_server_run(proven_http_server_t *server);

/**
 * @brief Do one round of the loop: wait until something is ready or until `until`, and handle
 *        what is. For a program that has a loop of its own, and for tests.
 * @return PROVEN_OK when something was handled; PROVEN_ERR_TIMEOUT when nothing was ready.
 */
[[nodiscard]]
proven_err_t proven_http_server_poll(proven_http_server_t *server, proven_net_deadline_t until);

/** @brief Make proven_http_server_run return. Safe from any thread, and from a handler. */
void proven_http_server_stop(proven_http_server_t *server);

/**
 * @brief End the server gracefully. Safe from any thread, and from a handler; returns at once,
 *        and the loop carries it out.
 *
 * From the next round: the listeners are closed; a connection with nothing of a request on it
 * is closed; one where a request has begun to arrive, or is with its handler, finishes it -
 * the response says `Connection: close` - and closes. proven_http_server_run returns when no
 * connection is left (PROVEN_OK), or when `grace_ms` has passed (PROVEN_ERR_TIMEOUT): then
 * every connection the loop holds is closed. **A connection whose handler is still running is
 * the handler's** and is not touched; it closes when the handler returns, and
 * proven_http_server_destroy waits for that as always. 0: close what can be closed now.
 *
 * A connection handed over by proven_http_exchange_upgrade is not the server's: a handler that
 * keeps one (a WebSocket) looks at proven_http_server_shutting_down and closes it - with 1001,
 * "going away", for a WebSocket. A shutdown is final; proven_http_server_stop during it makes
 * run return at once, as always.
 *
 * @return PROVEN_ERR_INVALID_ARG for no server; PROVEN_ERR_INVALID_STATE when it was already
 *         asked.
 */
[[nodiscard]]
proven_err_t proven_http_server_shutdown(proven_http_server_t *server, proven_u32 grace_ms);

/** @brief Whether proven_http_server_shutdown has been called. Safe from any thread: for a
 *         handler that keeps a connection (an upgraded one) and should let it go. */
[[nodiscard]]
bool proven_http_server_shutting_down(const proven_http_server_t *server);

/** @brief For a program that calls proven_http_server_poll itself: whether the shutdown is
 *         over, and (`why`, optional) how - PROVEN_OK or PROVEN_ERR_TIMEOUT, as run returns.
 *         On the loop's thread. */
[[nodiscard]]
bool proven_http_server_drained(const proven_http_server_t *server, proven_err_t *why);

/**
 * @brief Close every connection and listener and free the server.
 *
 * With a job system, handlers that are running are waited for: each returns when its handler
 * does. Do not call this from a handler.
 */
void proven_http_server_destroy(proven_http_server_t *server);

/** @brief How many connections are open right now. For the loop's own thread. */
[[nodiscard]]
proven_size_t proven_http_server_connection_count(const proven_http_server_t *server);

// -----------------------------------------------------------------------------
// Inside a handler
// -----------------------------------------------------------------------------

/** @brief The request: method, target, headers. Views into memory the exchange owns. */
[[nodiscard]]
const proven_http_request_t *proven_http_exchange_request(const proven_http_exchange_t *exchange);

/** @brief The client's address. */
[[nodiscard]]
proven_net_addr_t proven_http_exchange_peer(const proven_http_exchange_t *exchange);

/**
 * @brief Whether the request arrived as TLS early data (0-RTT), before the handshake was over.
 *
 * Only with a `tls` configuration that has `early_data`, and only for a request that is safe to
 * repeat - GET, HEAD or OPTIONS without a body: anything else that arrives early is answered
 * 425 Too Early by the server itself, and the client sends it again after the handshake (RFC
 * 8470). Early data can be replayed by someone who saw it (manual chapter 14): a handler whose
 * GET changes something answers 425 to an early one, and the client sends it again.
 */
[[nodiscard]]
bool proven_http_exchange_early(const proven_http_exchange_t *exchange);

/**
 * @brief Read more of the request body into `dest`.
 *
 * `.err` is PROVEN_ERR_EOF when the body is complete (at once, for a request without one). A
 * request that announced `Expect: 100-continue` is sent its `100 Continue` by the first call -
 * so a handler that refuses a request by its headers alone never invites the body.
 *
 * @return PROVEN_ERR_TIMEOUT when the client sent nothing for `body_timeout_ms`;
 *         PROVEN_ERR_OUT_OF_BOUNDS when the body passes `max_body_bytes`;
 *         PROVEN_ERR_INVALID_FORMAT for malformed chunk framing; PROVEN_ERR_RESET when the
 *         client went away. After any of these, send an error response or just return: the
 *         server answers 408, 413 or 400 for you if you sent nothing, and closes.
 */
[[nodiscard]]
proven_result_size_t proven_http_exchange_read(proven_http_exchange_t *exchange, proven_mem_mut_t dest);

/**
 * @brief Ask that the response to this request be compressed. Call it before the response
 *        begins; afterwards it does nothing.
 *
 * Nothing is compressed unless a handler asks, response by response, because whether it is
 * safe is a property of the response: **do not ask for one that carries a secret together
 * with text the client chose** - a session token beside a reflected query, say. The length of
 * a compressed body tells an observer of the connection how much the two have in common,
 * TLS or not (the attack known as BREACH).
 *
 * The response then carries `Vary: Accept-Encoding` (unless your `Vary` already names it) and,
 * when the request's `Accept-Encoding` allows gzip, is sent as `Content-Encoding: gzip`.
 * It is sent as it is when the client does not accept gzip; when the status is 204, 206 or
 * 304; when your headers have a `Content-Encoding` or a `Content-Range` of their own; when
 * memory for the compressor cannot be had; and, with proven_http_exchange_respond, when the
 * body is under 256 bytes or does not get smaller.
 *
 * With proven_http_exchange_begin the body leaves chunked whatever length was given (the
 * length is still what you must write), and **what you write may wait inside the compressor**
 * until more is written or the response ends. For a response that the client must see piece
 * by piece, such as an event stream, call proven_http_exchange_flush after each piece - or do
 * not compress it.
 */
void proven_http_exchange_compress(proven_http_exchange_t *exchange);

/** @brief Pass as `content_length` when the length of the body is not known in advance. */
#define PROVEN_HTTP_LENGTH_UNKNOWN UINT64_MAX

/**
 * @brief Send a complete response: status, headers, and a body held in memory.
 *
 * `Date`, `Content-Length` and `Connection` are written by the server. Giving `Content-Length`,
 * `Transfer-Encoding`, `Connection` or `Date` in `headers` is PROVEN_ERR_INVALID_ARG - two of
 * each is how a response becomes ambiguous - and so is a header name or value that the
 * writers of http.h refuse.
 *
 * @return PROVEN_ERR_INVALID_STATE when a response was already begun;
 *         PROVEN_ERR_OUT_OF_BOUNDS when the head does not fit `max_head_bytes`;
 *         PROVEN_ERR_TIMEOUT or PROVEN_ERR_RESET when the client does not take it.
 *         After PROVEN_ERR_INVALID_ARG or PROVEN_ERR_OUT_OF_BOUNDS nothing was sent and
 *         another response may be tried.
 */
[[nodiscard]]
proven_err_t proven_http_exchange_respond(proven_http_exchange_t *exchange, proven_u16 status,
                                          const proven_http_header_t *headers, proven_size_t header_count,
                                          proven_mem_view_t body);

/**
 * @brief Begin a response whose body will be written in pieces.
 * @param content_length the exact number of body bytes that will follow, or
 *        PROVEN_HTTP_LENGTH_UNKNOWN - the body is then sent chunked (and to an HTTP/1.0 client,
 *        which cannot read that, it runs until the connection closes).
 * @return as proven_http_exchange_respond.
 */
[[nodiscard]]
proven_err_t proven_http_exchange_begin(proven_http_exchange_t *exchange, proven_u16 status,
                                        const proven_http_header_t *headers, proven_size_t header_count,
                                        proven_u64 content_length);

/**
 * @brief Write more of the body of a response that proven_http_exchange_begin started.
 * @return PROVEN_ERR_OUT_OF_BOUNDS when this would pass the length that was announced;
 *         PROVEN_ERR_INVALID_STATE before begin or after end; PROVEN_ERR_TIMEOUT or
 *         PROVEN_ERR_RESET when the client does not take it - give up and return.
 */
[[nodiscard]]
proven_err_t proven_http_exchange_write(proven_http_exchange_t *exchange, proven_mem_view_t data);

/**
 * @brief Hand over what a compressed response holds back: everything written so far is sent
 *        in a form the client can read at once, and the response goes on.
 *
 * Only a response being compressed holds anything back (see proven_http_exchange_compress);
 * for any other this does nothing, successfully. Each flush costs a few bytes and some of the
 * compression: flush where a reader is waiting, not after every write.
 *
 * @return PROVEN_ERR_INVALID_STATE before begin or after end; otherwise as
 *         proven_http_exchange_write.
 */
[[nodiscard]]
proven_err_t proven_http_exchange_flush(proven_http_exchange_t *exchange);

/**
 * @brief Finish a response begun with proven_http_exchange_begin.
 *
 * Optional: returning from the handler finishes it too. A response that announced a length and
 * wrote fewer bytes cannot be completed, and the connection is closed so that the client sees
 * it was cut short.
 */
proven_err_t proven_http_exchange_end(proven_http_exchange_t *exchange);

/**
 * @brief End a response written in pieces with no length (proven_http_exchange_begin with
 *        PROVEN_HTTP_LENGTH_UNKNOWN, so chunked) with trailer fields after it (since v0.106.0): what a
 *        sender can only know at the end - a checksum, a signature, a status (gRPC's `grpc-status`).
 *        A client that does not read trailers passes over them.
 * @return PROVEN_ERR_INVALID_STATE for a response with a length (or to HEAD), or one already ended;
 *         PROVEN_ERR_INVALID_ARG for no fields, or a field proven_http_trailer_allowed refuses; as
 *         proven_http_exchange_end otherwise.
 */
[[nodiscard]]
proven_err_t proven_http_exchange_end_trailers(proven_http_exchange_t *exchange, const proven_http_header_t *fields, proven_size_t count);

/**
 * @brief The trailer fields that followed a chunked request body (since v0.106.0), once
 *        proven_http_exchange_read has returned PROVEN_ERR_EOF: views good until the handler returns.
 *        None for a body that was not chunked.
 * @return PROVEN_ERR_INVALID_STATE before the body's end; PROVEN_ERR_OUT_OF_BOUNDS for more fields
 *         than `cap`.
 */
[[nodiscard]]
proven_err_t proven_http_exchange_trailers(const proven_http_exchange_t *exchange, proven_http_header_t *fields, proven_size_t cap, proven_size_t *count);

/**
 * @brief Answer `101 Switching Protocols` and take the connection out of the server.
 *
 * For a request that asks to change protocol - `Upgrade: websocket`, say. The server writes the
 * 101 with `Upgrade: <protocol>`, `Connection: Upgrade`, `Date` and your `headers`, and from
 * then on the connection is not its business: `*out` is a transport that owns the socket, to
 * be used for as long as you like - after the handler has returned, from any one thread - and
 * closed by you with proven_transport_close. It no longer counts against `max_connections`,
 * and none of the server's timeouts apply to it; it counts against `max_upgraded` until it is
 * closed.
 *
 * `*early` is whatever the client sent after its request head: the first bytes of the new
 * protocol, if it did not wait for the 101. It is a view into the exchange and dies with it,
 * so hand it over (or copy it) before the handler returns.
 *
 * Whether the request is a well-formed upgrade of the protocol you mean is yours to check
 * first - for WebSocket, proven_ws_check_request; ws_conn.h does both steps.
 *
 * The transport is allocated from the server's allocator and freed when it is closed. If you
 * close it from a thread other than the one the server runs on, that allocator must be
 * thread-safe.
 *
 * @return PROVEN_ERR_INVALID_STATE when a response was already begun, the request has a body
 *         that was not read to its end, or it is HTTP/1.0; PROVEN_ERR_INVALID_ARG for an empty
 *         protocol, or a header the server owns (`Upgrade` included); PROVEN_ERR_NOMEM;
 *         PROVEN_ERR_OUT_OF_BOUNDS; PROVEN_ERR_TIMEOUT or PROVEN_ERR_RESET when the 101 could
 *         not be sent; PROVEN_ERR_BUSY when `max_upgraded` upgraded connections are open. After
 *         INVALID_ARG, NOMEM, OUT_OF_BOUNDS or BUSY nothing was sent and another response may be tried.
 */
[[nodiscard]]
proven_err_t proven_http_exchange_upgrade(proven_http_exchange_t *exchange, proven_u8str_view_t protocol,
                                          const proven_http_header_t *headers, proven_size_t header_count,
                                          proven_transport_t *out, proven_mem_view_t *early);

#endif /* PROVEN_HTTP_SERVER_H */
