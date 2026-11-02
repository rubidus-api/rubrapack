#ifndef PROVEN_HTTP_EVENT_CLIENT_H
#define PROVEN_HTTP_EVENT_CLIENT_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/u8str.h"
#include "proven/allocator.h"
#include "proven/net.h"
#include "proven/http.h"
#include "proven/loop.h"
#include "proven/tls.h"
#include "proven/http_event.h"

/**
 * @file http_event_client.h
 * @brief An HTTP client driven by events: many requests in flight on one thread.
 *
 * http_client.h sends a request and waits for the answer; a thousand requests at once would be
 * a thousand threads. This client sits on a `proven_loop_t` (loop.h): a request is started,
 * the call returns at once, and your functions are told what happened - the response head, a
 * piece of its body, the end.
 *
 * **It is deliberately narrow.** There are no redirects, no answers to authentication
 * challenges, no cookies and no proxies here; http_client.h has all of those. And **names
 * are not resolved**:
 * looking a name up can take seconds, and nothing on a loop may wait. Give the address to
 * connect to, having resolved it on another thread (proven_net_resolve from a job, the result
 * posted back with proven_loop_post). A URL whose host is an IP address needs none.
 *
 * **Connections are kept and counted** (HTTP/1.1; HTTP/2 has one connection an origin, see
 * `http2`). A connection whose exchange ended cleanly is kept for the next request to the
 * same origin - scheme, host, port and address - and an origin has at most
 * `max_connections_per_origin` at a time: a request beyond that waits, in the order of
 * starting, and a program that means to keep more than that many requests open to one server
 * must raise the field. A kept connection may have been closed by the server when it is used
 * again: a request lost on one before anything came back is sent once more on a new
 * connection, silently - or, when its body was written in pieces and cannot be, ends with
 * PROVEN_ERR_AGAIN.
 *
 * The rules of the loop apply: your functions must not wait; the views they are given - the
 * response head, a piece of the body - are good only until they return; and everything here
 * is called on the loop's thread, and only there.
 *
 * Hosted only; `PROVEN_NO_NET` leaves it out.
 */

/** @brief The client. Opaque. */
typedef struct proven_http_event_client proven_http_event_client_t;

/** @brief One request and its response. Valid from proven_http_event_client_start until
 *         `on_done` returns. */
typedef struct proven_http_event_request proven_http_event_request_t;

typedef struct {
    /** The response head has arrived. Optional. Interim responses (100, 103) are passed over
     *  and not reported. */
    void (*on_response)(void *ctx, proven_http_event_request_t *request, const proven_http_response_t *head);
    /** A piece of the response body; `last` with the final one (which may be empty). A
     *  response with no body gets no call. Optional: without it the body is thrown away. */
    void (*on_body)(void *ctx, proven_http_event_request_t *request, proven_mem_view_t piece, bool last);
    /** Request-body bytes that were held back have gone: proven_http_event_request_write will
     *  take more. Optional. */
    void (*on_writable)(void *ctx, proven_http_event_request_t *request);
    /** The end, called exactly once for every request that was started: PROVEN_OK when the
     *  whole response arrived, otherwise why not. Required. Free what you attached here.
     *  PROVEN_ERR_AGAIN is a request the server did not act on, which may be started again
     *  (HTTP/2: refused, or behind a goodbye; HTTP/1.1: a body written in pieces, on a kept
     *  connection that turned out to be dead) - `retry_unsent` has the client do that itself,
     *  so many times, before this is said, and with `retry_delay_ms` and `max_connections` a try that finds
     *  the client full, with as many waiting already as may, ends the request with PROVEN_ERR_BUSY; PROVEN_ERR_UNSUPPORTED is a request to a server in the clear that did
     *  not speak HTTP/2 (`http2_cleartext`), which would end the same way again. */
    void (*on_done)(void *ctx, proven_http_event_request_t *request, proven_err_t why);
} proven_http_event_client_callbacks_t;

/** @brief How a client behaves. Zero-initialise it; every zero field has the default written
 *         beside it. */
typedef struct {
    proven_allocator_t alloc;              /**< not valid: the loop's allocator */
    const proven_tls_config_t *tls;        /**< for `https`. NULL: an `https` URL is refused, never fetched in the clear. Must outlive the client. */
    proven_size_t max_head_bytes;          /**< largest response head, and request head. 0: 16 KiB */
    proven_size_t max_headers;             /**< most header fields in a response. 0: 64 */
    proven_u64 max_body_bytes;             /**< largest response body - as sent, and with `decompress` as decoded too. 0: no limit - it is delivered in pieces and never held */
    proven_size_t max_buffered_output;     /**< request-body bytes held before writes are refused. 0: 64 KiB */
    proven_u32 connect_timeout_ms;         /**< to be connected, the TLS handshake included - and, for HTTP/2, the server's SETTINGS; and the wait for a connection: for HTTP/1.1 when the origin has as many as it may, for either protocol when the client has (`max_connections`). 0: 10 s */
    proven_u32 response_timeout_ms;        /**< from the request being sent to the end of the response head. 0: 30 s */
    proven_u32 body_timeout_ms;            /**< between pieces of the response body. 0: 30 s */
    proven_u32 write_timeout_ms;           /**< output held with none of it accepted by the server. 0: 30 s */
    proven_u32 expect_timeout_ms;          /**< for a request with `expect_continue`: how long to wait for the server's word before sending the body anyway. 0: 1 s */
    bool decompress;                       /**< ask for compressed responses (`Accept-Encoding: gzip`, unless the request has its own or a `Range`) and decode a body sent with `Content-Encoding: gzip` or `deflate`: `on_body` gets decoded pieces of at most 16 KiB, the head stays as the server sent it, and a damaged or unfinished stream ends the request with PROVEN_ERR_INVALID_FORMAT. A 206 and other codings are delivered as sent. Costs about 52 KiB while such a response is arriving (for `zstd` and `br`, see `decompress_zstd` and `decompress_brotli`). false: bodies arrive as sent */
    bool http2;                            /**< true: an `https` origin that agrees to it (ALPN `h2`) is spoken to in HTTP/2, on one connection the client keeps for that origin and shares between its requests. The TLS configuration must offer `h2` - and must not when this is false */
    bool http2_cleartext;                  /**< true: every `http` request of this client is HTTP/2 by prior knowledge - there is no way to ask a server in the clear, so say this only of servers known to speak it. A connection that was made and is then lost before the server's SETTINGS ends its requests with PROVEN_ERR_UNSUPPORTED */
    proven_u32 idle_timeout_ms;            /**< how long a connection with no request on it is kept for the next one, in either protocol. 0: 60 s */
    proven_size_t max_waiting_requests;    /**< requests that may wait: over HTTP/2, on one connection, for it to be made or for the server's limit on streams; over HTTP/1.1, in the whole client, for a connection to their origin; and, counted apart, HTTP/2 requests in the whole client that wait for it to have room for a connection (`max_connections`). The next start is PROVEN_ERR_BUSY. 0: 1,000 */
    proven_size_t max_connections_per_origin;  /**< HTTP/1.1: connections to one origin (scheme, host, port and the address connected to) at a time, busy and idle together. A request beyond that waits, in the order of starting, for one to come free. 0: 6 */
    proven_size_t max_kept_connections;    /**< HTTP/1.1: connections kept idle for the next request, in the whole client; the one idle longest is closed to keep another. 0: 8, as every zero here is the default - to keep none, set `no_connection_reuse`. (Named `max_idle_connections` in v0.36.0. That is the name of a field of the blocking client, http_client.h, where 0 keeps none: the two no longer share it.) */
    bool no_connection_reuse;              /**< HTTP/1.1: true sends `Connection: close` and closes every connection when its response has ended, as every version before v0.36.0 did. The limit on connections to an origin applies all the same */
    proven_u32 retry_unsent;               /**< how many times a request that the server did not act on is put to it again by the client, before `on_done` is told PROVEN_ERR_AGAIN: over HTTP/2, a request behind a goodbye (GOAWAY), a refused stream, a connection lost before the request was on it; over HTTP/1.1, a connection lost before any of the request was given to it. At once, with no delay, on another connection where the first is finished with. Never a request whose body is written in pieces, never after `on_response`, never PROVEN_ERR_UNSUPPORTED. The time limits apply to each try; over HTTP/2 a request held in memory stays there until its response begins; and a try that fails for another reason - the new connection cannot be made - ends the request with that reason. 0: never - the caller decides */
    proven_u32 retry_delay_ms;             /**< how long the client waits before each try that `retry_unsent` makes, in either protocol: the same for every try unless `retry_delay_max_ms` is set. During it the request has no connection; `connect_timeout_ms` of the try begins when it ends. Not for the one re-send on a kept HTTP/1.1 connection that turned out to be dead, which is at once. 0: no wait */
    proven_u32 retry_delay_max_ms;         /**< 0: the wait is `retry_delay_ms` before every try. Otherwise the wait doubles with each try of a request - `retry_delay_ms` before its second try, twice that before its third - and is never more than this; less than `retry_delay_ms`, or set with no `retry_delay_ms`, is PROVEN_ERR_INVALID_ARG. Nothing is added at random: requests refused together are tried again together */
    proven_size_t max_connections;         /**< connections of the whole client, HTTP/1.1, HTTP/2 and HTTP/3 (QUIC), busy and idle. A request that would make one more first has an idle connection closed for it (an HTTP/1.1 one, the one idle longest, before an HTTP/2 one); with none idle the request waits, in either protocol - an HTTP/1.1 request as it does for its origin's limit, an HTTP/2 request for an origin with no connection on no connection at all - until a connection ends or has nothing on it, for `connect_timeout_ms` at most (PROVEN_ERR_TIMEOUT), and no more of them than `max_waiting_requests` (PROVEN_ERR_BUSY from proven_http_event_client_start). A place goes to an HTTP/1.1 request that waits before an HTTP/2 one. Less than `max_kept_connections` when both are set is PROVEN_ERR_INVALID_ARG. 0: no limit */
    proven_u32 http2_window_bytes;         /**< HTTP/2: response-body bytes a server may send on one stream, and on a connection as a whole, before `on_body` has taken them (flow control, RFC 9113 section 5.2). It is what a request that is paused can make a connection hold, and it bounds how fast a large body can arrive: the server must wait for room each time it has sent this much. 0: it starts at 65,535, the protocol's default, and grows - doubling while the bodies are read as fast as they come, up to 16 MiB per stream and 24 MiB per connection (since v0.75.0, as `http3_window_bytes`; RFC-0019); a body not read stops the growth. A value set here is fixed. At most 2^31-1 */
    bool decompress_zstd;                  /**< with `decompress`: ask for Zstandard too (`Accept-Encoding: zstd, gzip`) and decode a body sent with `Content-Encoding: zstd` in the same way. Its window may be up to 8 MiB (RFC 9659), so such a response costs up to that and about 400 KiB more while it is arriving, allocated as its output grows. Without `decompress`, PROVEN_ERR_INVALID_ARG. false: `zstd` is not asked for, and a body sent in it anyway is delivered as sent */
    bool decompress_brotli;                /**< with `decompress`: ask for Brotli too (`br` in `Accept-Encoding`) and decode a body sent with `Content-Encoding: br` in the same way. Its window may be up to 16 MiB, so such a response costs up to that and its codes (tens of KiB as encoders write them, 1.3 MiB at most) while it is arriving, allocated as its output grows. Without `decompress`, PROVEN_ERR_INVALID_ARG; in a build with PROVEN_NO_BROTLI, PROVEN_ERR_UNSUPPORTED. false: `br` is not asked for, and a body sent in it anyway is delivered as sent */
    bool http3;                            /**< true: a request that asks for it (`http3` in its options) is sent over HTTP/3 (RFC 9114, over QUIC, RFC 9000) - never one that does not, and never by following a server's `Alt-Svc`: HTTP/2 is chosen by the TLS handshake, HTTP/3 only by the caller. The TLS configuration must offer `h3` - and must not when this is false. One QUIC connection, on a UDP socket of its own, is kept for each origin and shared by its requests, as an HTTP/2 connection is; it counts against `max_connections`. Without `http3_fallback` there is no fallback: a server that does not answer over UDP ends the request with PROVEN_ERR_TIMEOUT after `connect_timeout_ms`, one that refuses `h3` with PROVEN_ERR_UNSUPPORTED, an untrusted one with PROVEN_ERR_UNTRUSTED - and no TCP connection is tried. A write is bounded by QUIC's send buffer (256 KiB per stream, 1 MiB per connection of data not yet acknowledged), not by `max_buffered_output`. `retry_unsent` covers a request the server refused unprocessed (H3_REQUEST_REJECTED, or behind a GOAWAY). In a build with PROVEN_NO_HTTP3, proven_http_event_client_create returns PROVEN_ERR_UNSUPPORTED */
    proven_size_t http3_window_bytes;      /**< HTTP/3: response-body bytes a server may send on one stream, and on a connection as a whole, before `on_body` has taken them (QUIC's flow control) - as `http2_window_bytes` is for HTTP/2. 0: it starts at 256 KiB per stream and 1 MiB per connection and grows - doubling while the bodies are read as fast as they come, up to 16 MiB per stream and 24 MiB per connection (since v0.71.0, RFC-0019); a body not read stops the growth. A value set here is fixed, as before. At most 2^31-1 */
    proven_size_t http3_header_table_bytes; /**< HTTP/3: the QPACK dynamic table a server's encoder may fill for this client's decoder (RFC 9204; RFC-0032), said in SETTINGS with 16 streams that may wait for its inserts. Made only when a server first uses it: then about twice this per connection. 0: 4,096. 32 to 1 MiB. The same size bounds the table the client's encoder fills for the server (since v0.96.0), when the server allows one: it refers only to entries the server has acknowledged, and never inserts a field marked sensitive or a short credential */
    bool http3_no_header_table;          /**< HTTP/3: no such table in either direction: every head goes in full, as before v0.95.0 */
    bool http3_fallback;                   /**< HTTP/3: true lets a request asked over HTTP/3 go over TCP instead when its QUIC connection does not come up (RFC-0031): after `http3_fallback_ms` without a handshake, or at once when the system reports the server's address unreachable or the port refused, or the server refuses `h3`. It goes as the request it would have been without `http3` - HTTP/2 when the client has `http2` and the server agrees, HTTP/1.1 otherwise - and the caller keeps the object it was given; proven_http_event_request_protocol then says 2 or 1. Every request waiting on that connection goes the same way, and for five minutes after, requests to that origin asked over HTTP/3 go over TCP at once (up to 64 origins remembered, the oldest forgotten first). Never on a certificate the client does not trust (that is the request's answer: TCP would show the same certificate), and never once the QUIC connection has come up: a connection lost after that is `retry_unsent`'s matter. The TLS configuration is used as it is, `h3` among what it offers. false: no fallback, as before */
    proven_u32 http3_fallback_ms;          /**< with `http3_fallback`: how long a QUIC handshake may take before the requests waiting on it go over TCP, counted from the first packet. 0: 1 s. More than `connect_timeout_ms` is taken as that */
} proven_http_event_client_config_t;

/** @brief One request. Zero-initialise it and set `url` and `on.on_done`. Everything it points
 *         to is copied or used before proven_http_event_client_start returns. */
typedef struct {
    proven_u8str_view_t method;            /**< empty: GET */
    proven_u8str_view_t url;               /**< absolute, `http` or `https`; its host goes into `Host` and, for `https`, is the name the certificate must be for */
    const proven_net_addr_t *address;      /**< where to connect. NULL only when the URL's host is an IP address. Its port is used as given. */
    const proven_http_header_t *headers;   /**< yours; `Host`, `Content-Length`, `Transfer-Encoding` and `Connection` are written for you and may not be here */
    proven_size_t header_count;
    proven_mem_view_t body;                /**< a body held in memory, sent whole */
    proven_u64 body_length;                /**< when `body` is empty: 0 for no body; a length for a body written afterwards with proven_http_event_request_write; PROVEN_HTTP_EVENT_LENGTH_UNKNOWN to write it chunked */
    proven_http_event_client_callbacks_t on;
    void *ctx;                             /**< passed to every callback */
    bool http3;                            /**< send this request over HTTP/3: an `https` URL, on a client with `http3` (otherwise PROVEN_ERR_INVALID_ARG). See `http3` in the client's configuration: no fallback unless it has `http3_fallback` */
    bool expect_continue;                  /**< for a request with a body, over HTTP/1.1 (since v0.106.0): send `Expect: 100-continue` and the head first, and the body - held, or written with proven_http_event_request_write meanwhile - only when the server says `100 Continue`, or says nothing for `expect_timeout_ms`. A server that answers at once instead - 401, 413, 417 - is not sent the body at all: that answer is the response, what is written of the body after it is dropped, and the connection is not kept. Do not set an `Expect` header of your own with it. Over HTTP/2 and HTTP/3 the flag is ignored and the body goes at once (a stream can be reset instead) */
} proven_http_event_request_options_t;

/** @return PROVEN_ERR_INVALID_ARG (no loop; a TLS configuration whose `h2` or `h3` does not
 *         match `http2` or `http3`; `http3` without TLS; `http3_fallback` without `http3`; a
 *         window out of range),
 *         PROVEN_ERR_UNSUPPORTED (`http3` in a build without it), PROVEN_ERR_NOMEM. */
[[nodiscard]]
proven_err_t proven_http_event_client_create(proven_loop_t *loop, const proven_http_event_client_config_t *config,
                                             proven_http_event_client_t **out);

/** @brief End every request still in flight or waiting - each gets `on_done` with
 *         PROVEN_ERR_RESET - close the connections it kept, and free the client. The loop is not stopped or freed. Null is accepted.
 *         Connections whose sockets the loop may still use keep their memory, and the client's struct, until the loop's next
 *         round or proven_loop_destroy (chapter 15, section 5, rule 3). */
void proven_http_event_client_destroy(proven_http_event_client_t *client);

/** @brief Requests started and not ended: those in flight, and those waiting for a connection. */
proven_size_t proven_http_event_client_requests(const proven_http_event_client_t *client);

/**
 * @brief Start a request. Returns at once; what happens to it arrives through the callbacks.
 *
 * When this returns an error the request was not started and no callback is or will be made.
 * When it returns PROVEN_OK, `on_done` will be called exactly once - never inside this call.
 *
 * @param out optional: receives the request, for the calls below.
 * @return PROVEN_ERR_INVALID_ARG for no `on_done`; a URL that is not absolute `http` or
 *         `https`, or that carries credentials; an `https` URL with no TLS configuration; a
 *         host that is a name with no `address` given; a header the client writes itself; a
 *         method that is not a token, or CONNECT. PROVEN_ERR_OUT_OF_BOUNDS when the request
 *         head does not fit `max_head_bytes`. PROVEN_ERR_NOMEM. And what beginning to connect
 *         returned, when it failed at once (PROVEN_ERR_UNREACHABLE, PROVEN_ERR_REFUSED).
 */
[[nodiscard]]
proven_err_t proven_http_event_client_start(proven_http_event_client_t *client, const proven_http_event_request_options_t *options,
                                            proven_http_event_request_t **out);

/**
 * @brief Offer request-body bytes, for a request started with a `body_length`. **It may take
 *        fewer than you offer** - as many as fit under `max_buffered_output`, possibly none.
 *        Keep the rest and offer it again when `on_writable` is called. It may be called
 *        before the connection is made: what is taken waits.
 * @return the number of bytes taken; PROVEN_ERR_INVALID_STATE when the request has no body to
 *         write, or it was ended; PROVEN_ERR_OUT_OF_BOUNDS for more than the length promised;
 *         PROVEN_ERR_RESET when the request ended over it (its connection failed, or memory ran
 *         out) - `on_done` has then been called, inside this call, so neither the request nor
 *         what `on_done` freed may be used after it returns.
 */
[[nodiscard]]
proven_result_size_t proven_http_event_request_write(proven_http_event_request_t *request, proven_mem_view_t data);

/**
 * @brief Finish the request body.
 * @return PROVEN_ERR_INVALID_STATE when there was none to finish; PROVEN_ERR_INVALID_FORMAT
 *         when fewer bytes were written than promised - the request is then ended, and
 *         `on_done` is called with that error inside this call.
 */
[[nodiscard]]
proven_err_t proven_http_event_request_end(proven_http_event_request_t *request);

/**
 * @brief Finish a body written with `body_length` PROVEN_HTTP_EVENT_LENGTH_UNKNOWN with trailer
 *        fields after it (since v0.107.0): over HTTP/1.1 after the last chunk, over HTTP/2 and
 *        HTTP/3 as a HEADERS frame that ends the stream - sent once the body before them has gone.
 *        The fields are copied. A server that does not read trailers passes over them.
 * @return PROVEN_ERR_INVALID_ARG for no fields, or one proven_http_trailer_allowed refuses (or not
 *         a valid field); PROVEN_ERR_INVALID_STATE for a body with a length, or none to finish;
 *         PROVEN_ERR_OUT_OF_BOUNDS past PROVEN_HTTP_MAX_TRAILER_BYTES; otherwise as
 *         proven_http_event_request_end.
 */
[[nodiscard]]
proven_err_t proven_http_event_request_end_trailers(proven_http_event_request_t *request, const proven_http_header_t *fields, proven_size_t count);

/**
 * @brief The response's trailer fields (since v0.107.0), once its body has ended - from `on_body`
 *        with `last` (or, for a response with no body, `on_done`) until `on_done` returns: views
 *        into the client's copy. None for an HTTP/1.1 response that was not chunked. A field that
 *        may not be a trailer (proven_http_trailer_allowed) is left out. Over HTTP/1.1 a chunked
 *        response holds a buffer of PROVEN_HTTP_MAX_TRAILER_BYTES for its trailer section.
 * @return PROVEN_ERR_INVALID_STATE before the body's end; PROVEN_ERR_OUT_OF_BOUNDS for more fields
 *         than `cap`.
 */
[[nodiscard]]
proven_err_t proven_http_event_request_trailers(const proven_http_event_request_t *request, proven_http_header_t *fields, proven_size_t cap, proven_size_t *count);

/** @brief Give up: the connection is closed at once and `on_done` is called with
 *         PROVEN_ERR_RESET, inside this call. */
void proven_http_event_request_abort(proven_http_event_request_t *request);

/** @brief Stop delivering the response body (and stop reading the connection) until
 *         proven_http_event_request_resume. */
void proven_http_event_request_pause(proven_http_event_request_t *request);
/** @brief Deliver again. Pieces that were waiting may arrive inside this call. */
void proven_http_event_request_resume(proven_http_event_request_t *request);

/** @brief Attach a pointer of yours to the request, and get it back. */
void proven_http_event_request_set_user(proven_http_event_request_t *request, void *user);
void *proven_http_event_request_user(const proven_http_event_request_t *request);

/** @brief Which protocol carries this request: 3 for HTTP/3 (asked for, so known from the
 *         start - until it falls back to TCP, `http3_fallback`), 2 for HTTP/2, 1 for HTTP/1.1 -
 *         which is also the answer while a request waits for a connection whose protocol is not
 *         yet known. */
[[nodiscard]]
int proven_http_event_request_protocol(const proven_http_event_request_t *request);

#endif /* PROVEN_HTTP_EVENT_CLIENT_H */
