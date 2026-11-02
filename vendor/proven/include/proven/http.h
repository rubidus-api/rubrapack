#ifndef PROVEN_HTTP_H
#define PROVEN_HTTP_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/u8str.h"
#include "proven/time.h"

/**
 * @file http.h
 * @brief HTTP/1.1 messages: parsing a head, framing a body, and writing both.
 *
 * This is a codec, not a client and not a server. It takes bytes you have and tells you what
 * they say; it writes bytes into memory you supply. It never reads, never writes to a socket,
 * never allocates, and keeps no state between calls except the small body-decoder struct you
 * own. That is what lets the same code sit under a blocking client, an event loop, a test with
 * no network, or a freestanding build.
 *
 * The parser is strict on purpose. An HTTP message is routinely read by more than one program
 * - a proxy in front, a server behind - and wherever two parsers read the same bytes
 * differently, one of them can be shown a request the other does not see. That is request
 * smuggling, and its raw material is leniency. So:
 *
 *   - a line ends in CRLF; a bare LF or a bare CR is refused;
 *   - there is no whitespace between a header name and its colon;
 *   - a header line that starts with a space (obsolete line folding) is refused;
 *   - `Content-Length` appears once, with digits only;
 *   - a message with both `Transfer-Encoding` and `Content-Length` is refused;
 *   - the only transfer coding is `chunked`.
 *
 * Each of these is something real software accepts and real attacks use.
 *
 * What is NOT here: HTTP/2 and HTTP/3; content codings (gzip); cookies;
 * authentication. Trailers after a chunked body are checked and skipped unless asked for
 * (proven_http_body_keep_trailers, since v0.106.0).
 *
 * Not part of the socket layer: `PROVEN_NO_NET` does not remove this header.
 */

// -----------------------------------------------------------------------------
// Heads
// -----------------------------------------------------------------------------

/** @brief One header field: two views into the bytes that were parsed. The value has no
 *         leading or trailing whitespace. */
typedef struct {
    proven_u8str_view_t name;
    proven_u8str_view_t value;
} proven_http_header_t;

typedef enum {
    PROVEN_HTTP_METHOD_OTHER = 0,   /**< a valid token this enum has no name for; see `method_text` */
    PROVEN_HTTP_GET,
    PROVEN_HTTP_HEAD,
    PROVEN_HTTP_POST,
    PROVEN_HTTP_PUT,
    PROVEN_HTTP_DELETE,
    PROVEN_HTTP_CONNECT,
    PROVEN_HTTP_OPTIONS,
    PROVEN_HTTP_TRACE,
    PROVEN_HTTP_PATCH
} proven_http_method_t;

/**
 * @brief A parsed request head. Every view points into the buffer that was parsed, so the
 *        buffer must outlive it and must not move.
 *
 * The event-driven server gives a handler an HTTP/2 request in the same form (version 1.1).
 * An HTTP/2 extended CONNECT (RFC 8441) arrives as method CONNECT, its path as `target`, and a
 * `:protocol` field first in `headers` - a name no HTTP/1.1 request can carry.
 */
typedef struct {
    proven_http_method_t method;
    proven_u8str_view_t method_text;    /**< as written; methods are case-sensitive */
    proven_u8str_view_t target;         /**< as written: `/a?b`, `http://h/a`, `*`, or `host:port` */
    proven_u8 version_minor;            /**< 1 for HTTP/1.1, 0 for HTTP/1.0 */
    proven_http_header_t *headers;      /**< the array you supplied */
    proven_size_t header_count;
} proven_http_request_t;

/** @brief A parsed response head. Views point into the buffer that was parsed. */
typedef struct {
    proven_u16 status;                  /**< 100-999 */
    proven_u8str_view_t reason;         /**< may be empty; means nothing to a program */
    proven_u8 version_minor;
    proven_http_header_t *headers;
    proven_size_t header_count;
} proven_http_response_t;

/** @brief Largest head, in bytes, the parsers accept when you pass 0 for the limit. */
#define PROVEN_HTTP_DEFAULT_MAX_HEAD ((proven_size_t)16384)

/**
 * @brief Parse a request head from the front of `data`.
 *
 * `data` is what you have read so far. The head is the request line and the header lines, up
 * to and including the empty line that ends them; `*head_size` is its length, and whatever
 * follows it in `data` is the start of the body (or of the next request).
 *
 * Nothing is copied. `headers` is an array you supply for the header fields; `out->headers`
 * points at it.
 *
 * Empty lines before the request line are skipped, as RFC 9112 section 2.2 asks of a server:
 * clients have long sent a stray CRLF after a body. They are counted in `*head_size`.
 *
 * @param max_head_bytes the largest head to accept; 0 means PROVEN_HTTP_DEFAULT_MAX_HEAD.
 *
 * @return PROVEN_ERR_NEED_MORE when `data` is a valid beginning that does not yet contain the
 *         whole head: read more and call again with the longer buffer.
 *         PROVEN_ERR_OUT_OF_BOUNDS when the head is larger than `max_head_bytes` - decided as
 *         soon as that many bytes hold no end, so an endless header costs the limit and no
 *         more - or has more fields than `header_cap` (answer 431).
 *         PROVEN_ERR_INVALID_FORMAT when the bytes are not an HTTP/1 request head (answer 400).
 *         PROVEN_ERR_UNSUPPORTED when the version is not HTTP/1.0 or HTTP/1.1 (answer 505).
 *         On any error `*out` and `*head_size` are not to be used.
 */
[[nodiscard]]
proven_err_t proven_http_parse_request(proven_mem_view_t data, proven_http_header_t *headers, proven_size_t header_cap,
                                       proven_size_t max_head_bytes, proven_http_request_t *out, proven_size_t *head_size);

/** @brief Parse a response head from the front of `data`. As proven_http_parse_request. */
[[nodiscard]]
proven_err_t proven_http_parse_response(proven_mem_view_t data, proven_http_header_t *headers, proven_size_t header_cap,
                                        proven_size_t max_head_bytes, proven_http_response_t *out, proven_size_t *head_size);

/**
 * @brief Find the first header field named `name` (ASCII case-insensitive).
 * @return true and `*value` when present; false otherwise.
 */
[[nodiscard]]
bool proven_http_header_find(const proven_http_header_t *headers, proven_size_t count,
                             proven_u8str_view_t name, proven_u8str_view_t *value);

/** @brief How many fields are named `name`. A message may repeat a field; some, like
 *         `Set-Cookie`, routinely do. */
[[nodiscard]]
proven_size_t proven_http_header_count(const proven_http_header_t *headers, proven_size_t count, proven_u8str_view_t name);

/**
 * @brief Whether any field named `name` lists `token` (ASCII case-insensitive) among its
 *        comma-separated values: `Connection: keep-alive, Upgrade` lists `upgrade`.
 */
[[nodiscard]]
bool proven_http_header_has_token(const proven_http_header_t *headers, proven_size_t count,
                                  proven_u8str_view_t name, proven_u8str_view_t token);

/** @brief A content coding: how a body was transformed on top of its media type. */
typedef enum {
    PROVEN_HTTP_CODING_IDENTITY = 0,   /**< none: the bytes are the content */
    PROVEN_HTTP_CODING_GZIP,           /**< `gzip` (and its old name `x-gzip`): RFC 1952 */
    PROVEN_HTTP_CODING_DEFLATE,        /**< `deflate`: by the standard a zlib stream (RFC 1950); in practice sometimes raw DEFLATE */
    PROVEN_HTTP_CODING_OTHER,          /**< anything else, or more than one coding */
    PROVEN_HTTP_CODING_ZSTD,           /**< `zstd`: Zstandard, RFC 8878, with a window of at most 8 MiB (RFC 9659); since v0.57.0, OTHER before */
    PROVEN_HTTP_CODING_BROTLI          /**< `br`: Brotli, RFC 7932; since v0.60.0, OTHER before */
} proven_http_coding_t;

/**
 * @brief What `Content-Encoding` says about a message's body.
 * @return PROVEN_HTTP_CODING_IDENTITY when the field is absent, empty or `identity`;
 *         PROVEN_HTTP_CODING_OTHER for a coding that is none of these, for a list of several,
 *         and for more than one field.
 */
[[nodiscard]]
proven_http_coding_t proven_http_content_coding(const proven_http_header_t *headers, proven_size_t count);

/**
 * @brief Whether a request's `Accept-Encoding` allows a response in `coding` (RFC 9110,
 *        section 12.5.3): the coding is listed with a weight above zero, or it is not listed
 *        and `*` is, with a weight above zero.
 *
 * Without the field nothing but PROVEN_HTTP_CODING_IDENTITY is accepted: the standard lets a
 * server assume any coding then, and this does not. Identity is accepted unless the field
 * rules it out (`identity;q=0`, or `*;q=0` without naming it). A field that is not well formed
 * accepts identity only. PROVEN_HTTP_CODING_OTHER is never accepted.
 */
[[nodiscard]]
bool proven_http_accepts_coding(const proven_http_header_t *headers, proven_size_t count, proven_http_coding_t coding);

/**
 * @brief How much a request's `Accept-Encoding` wants `coding`: its weight (`q`) in
 *        thousandths, 0 to 1000, by the rules of proven_http_accepts_coding - the coding's own
 *        weight, else that of `*`, else 1000 for identity and 0 for the rest. 0 exactly when
 *        proven_http_accepts_coding says no. For choosing between codings a request accepts.
 */
[[nodiscard]]
unsigned proven_http_accept_weight(const proven_http_header_t *headers, proven_size_t count, proven_http_coding_t coding);

/** @brief The method a token names; PROVEN_HTTP_METHOD_OTHER for any other valid token.
 *         Case-sensitive: `get` is not `GET`. */
[[nodiscard]]
proven_http_method_t proven_http_method_from_text(proven_u8str_view_t text);

/** @brief The token for a method; an empty view for PROVEN_HTTP_METHOD_OTHER. */
[[nodiscard]]
proven_u8str_view_t proven_http_method_text(proven_http_method_t method);

/** @brief The standard reason phrase for a status code ("Not Found"); "Unknown" for one this
 *         library has no phrase for. */
[[nodiscard]]
proven_u8str_view_t proven_http_reason_phrase(proven_u16 status);

/**
 * @brief Whether the connection may carry another request after this one.
 *
 * HTTP/1.1 persists unless `Connection: close`; HTTP/1.0 closes unless `Connection: keep-alive`.
 */
[[nodiscard]]
bool proven_http_request_keep_alive(const proven_http_request_t *request);

/** @brief The same question for a response. A response whose body runs until the connection
 *         closes can never be followed by another; ask proven_http_response_framing as well. */
[[nodiscard]]
bool proven_http_response_keep_alive(const proven_http_response_t *response);

// -----------------------------------------------------------------------------
// Bodies
// -----------------------------------------------------------------------------

typedef enum {
    PROVEN_HTTP_BODY_NONE = 0,      /**< no body follows the head */
    PROVEN_HTTP_BODY_LENGTH,        /**< exactly `length` bytes follow */
    PROVEN_HTTP_BODY_CHUNKED,       /**< chunks follow, ended by a zero-size chunk */
    PROVEN_HTTP_BODY_UNTIL_CLOSE    /**< responses only: the body is everything until the peer closes */
} proven_http_body_kind_t;

/** @brief How the body that follows a head is delimited. */
typedef struct {
    proven_http_body_kind_t kind;
    proven_u64 length;              /**< for PROVEN_HTTP_BODY_LENGTH */
} proven_http_framing_t;

/**
 * @brief Decide how a request's body is delimited (RFC 9112 section 6.3).
 *
 * `Transfer-Encoding: chunked` means chunked; `Content-Length` means that many bytes; neither
 * means no body. A request never runs until close.
 *
 * @return PROVEN_ERR_INVALID_FORMAT (answer 400) for both `Transfer-Encoding` and
 *         `Content-Length`; for more than one `Content-Length` field, or one that is not a
 *         plain decimal number or does not fit 64 bits; for `Transfer-Encoding` on an HTTP/1.0
 *         request. PROVEN_ERR_UNSUPPORTED (answer 501) for a transfer coding other than
 *         `chunked`.
 */
[[nodiscard]]
proven_err_t proven_http_request_framing(const proven_http_request_t *request, proven_http_framing_t *out);

/**
 * @brief Decide how a response's body is delimited.
 *
 * @param request_method the method of the request this answers: a response to HEAD has no
 *        body whatever its headers say, and neither has a 2xx response to CONNECT.
 *
 * Responses with status 1xx, 204 and 304 have no body. Otherwise chunked, then
 * `Content-Length`, and with neither the body is everything until the peer closes.
 *
 * @return as proven_http_request_framing.
 */
[[nodiscard]]
proven_err_t proven_http_response_framing(const proven_http_response_t *response, proven_http_method_t request_method,
                                          proven_http_framing_t *out);

/**
 * @brief The state of a body being decoded. Caller-owned; start it with proven_http_body_init.
 *
 * Its fields are exposed only so it can live on the stack. Do not read or write them.
 */
typedef struct {
    proven_u8 kind;
    proven_u8 state;
    proven_u8 size_digits;
    bool done;
    proven_u64 remaining;       /* bytes left in the body, or in the current chunk */
    proven_u64 total;           /* payload bytes delivered so far */
    proven_u64 max_body;
    proven_u32 line_bytes;      /* bytes of chunk-size line or trailer section seen */
    proven_u32 trailer_cap;     /* proven_http_body_keep_trailers: room in `trailers`, where the section is kept */
    proven_byte_t *trailers;
} proven_http_body_t;

/** @brief Largest chunk-size line (size, extensions, CRLF) the decoder accepts. */
#define PROVEN_HTTP_MAX_CHUNK_LINE ((proven_u32)256)
/** @brief Largest trailer section after a chunked body the decoder accepts. */
#define PROVEN_HTTP_MAX_TRAILER_BYTES ((proven_u32)8192)

/**
 * @brief Begin decoding a body.
 * @param max_body_bytes the largest body to accept. There is no default: say how much a peer
 *        may send you. A `Content-Length` above it is refused at once.
 * @return PROVEN_ERR_OUT_OF_BOUNDS when `framing` already announces more than `max_body_bytes`
 *         (answer 413); PROVEN_ERR_INVALID_ARG for a framing kind that does not exist.
 */
[[nodiscard]]
proven_err_t proven_http_body_init(proven_http_body_t *body, proven_http_framing_t framing, proven_u64 max_body_bytes);

/**
 * @brief Feed the decoder bytes that followed the head, and get body bytes back.
 *
 * Consumes some prefix of `in` - `*consumed` says how much - and sets `*payload` to the body
 * bytes that prefix contained, as a view INTO `in` (nothing is copied; for a chunked body the
 * chunk framing is simply stepped over). Call it again with what is left of `in`; when it has
 * consumed everything and `*done` is still false, read more.
 *
 * When `*done` becomes true the body is complete, and bytes of `in` beyond `*consumed` belong
 * to the next message. A body that runs until close is never done: when the peer closes, ask
 * proven_http_body_end.
 *
 * @return PROVEN_ERR_INVALID_FORMAT for malformed chunk framing or trailers;
 *         PROVEN_ERR_OUT_OF_BOUNDS when the body grows past the limit given to
 *         proven_http_body_init, or a chunk-size line or the trailers pass their bounds;
 *         PROVEN_ERR_INVALID_STATE when called again after an error. After any error the
 *         connection cannot be reused: where the next message starts is no longer known.
 */
[[nodiscard]]
proven_err_t proven_http_body_feed(proven_http_body_t *body, proven_mem_view_t in,
                                   proven_size_t *consumed, proven_mem_view_t *payload, bool *done);

/**
 * @brief The peer closed the connection: was the body complete?
 * @return PROVEN_OK when it was (always, for a body that runs until close);
 *         PROVEN_ERR_NEED_MORE when the connection ended in the middle of it - the message is
 *         truncated and must not be treated as whole.
 */
[[nodiscard]]
proven_err_t proven_http_body_end(const proven_http_body_t *body);

/** @brief Body bytes delivered so far. */
[[nodiscard]]
proven_u64 proven_http_body_received(const proven_http_body_t *body);

/**
 * @brief Keep the trailer section of a chunked body (since v0.106.0): the fields sent after the last
 *        chunk (RFC 9112 section 7.1.2), which are otherwise checked and passed over. Call it after
 *        proven_http_body_init, before the body ends. The section is copied into `buf` as it arrives.
 * @return PROVEN_ERR_INVALID_ARG for no buffer. A section larger than `buf` (or than
 *         PROVEN_HTTP_MAX_TRAILER_BYTES) makes proven_http_body_feed fail with PROVEN_ERR_OUT_OF_BOUNDS.
 */
[[nodiscard]]
proven_err_t proven_http_body_keep_trailers(proven_http_body_t *body, proven_mem_mut_t buf);

/**
 * @brief The trailer fields of a body that has ended, read from what proven_http_body_keep_trailers
 *        kept: views into its buffer, in the order sent. None for a body that was not chunked. A field
 *        proven_http_trailer_allowed refuses is left out: it is not taken for what the head said.
 * @return PROVEN_ERR_INVALID_STATE before the end, or without kept trailers; PROVEN_ERR_OUT_OF_BOUNDS
 *         for more fields than `cap`.
 */
[[nodiscard]]
proven_err_t proven_http_body_trailers(const proven_http_body_t *body, proven_http_header_t *fields, proven_size_t cap, proven_size_t *count);

/**
 * @brief Whether a field may be sent as a trailer (RFC 9110 section 6.5.1): not one that frames the
 *        message, routes it, controls the request, authenticates, or says how to process the content
 *        - `Content-Length`, `Transfer-Encoding`, `Trailer`, `Host`, `Connection`, `Keep-Alive`, `TE`,
 *        `Upgrade`, `Expect`, `Max-Forwards`, `Cache-Control`, `Pragma`, `Range`, `Authorization`,
 *        `Proxy-Authorization`, `WWW-Authenticate`, `Proxy-Authenticate`, `Cookie`, `Set-Cookie`,
 *        `Age`, `Date`, `Expires`, `Location`, `Retry-After`, `Vary`, `Content-Encoding`,
 *        `Content-Type`, `Content-Range`, and any pseudo-field. Names compare without case.
 */
[[nodiscard]]
bool proven_http_trailer_allowed(proven_u8str_view_t name);

// -----------------------------------------------------------------------------
// Writing
// -----------------------------------------------------------------------------
//
// Each writer APPENDS to `out` at `*len` and advances `*len`. On any failure `*len` and the
// bytes before it are unchanged, so a message is built by a run of calls and one check of the
// last result is not enough: check each, or the first failure leaves a hole.

/**
 * @brief Append a request line: `GET /path HTTP/1.1` and CRLF.
 * @return PROVEN_ERR_INVALID_ARG when `method` is not a token or `target` is empty or holds a
 *         space or a control character - either would let a caller-supplied string end the
 *         line and start another. PROVEN_ERR_OUT_OF_BOUNDS when it does not fit.
 */
[[nodiscard]]
proven_err_t proven_http_write_request_line(proven_mem_mut_t out, proven_size_t *len,
                                            proven_u8str_view_t method, proven_u8str_view_t target);

/**
 * @brief Append a status line: `HTTP/1.1 404 Not Found` and CRLF.
 * @param reason the phrase to send; an empty view sends the standard one for `status`.
 * @return PROVEN_ERR_INVALID_ARG for a status outside 100-999 or a reason with a control
 *         character in it.
 */
[[nodiscard]]
proven_err_t proven_http_write_status_line(proven_mem_mut_t out, proven_size_t *len,
                                           proven_u16 status, proven_u8str_view_t reason);

/**
 * @brief Append one header field: `Name: value` and CRLF.
 *
 * @return PROVEN_ERR_INVALID_ARG when `name` is not a token, or `value` contains CR, LF, NUL or
 *         another control character (a tab is allowed), or starts or ends with whitespace.
 *         This is the check that stops header injection: a value taken from a request and
 *         written into a response cannot end the field and add one of its own.
 */
[[nodiscard]]
proven_err_t proven_http_write_header(proven_mem_mut_t out, proven_size_t *len,
                                      proven_u8str_view_t name, proven_u8str_view_t value);

/** @brief Append a header field whose value is a decimal number: `Content-Length: 1234`. */
[[nodiscard]]
proven_err_t proven_http_write_header_u64(proven_mem_mut_t out, proven_size_t *len,
                                          proven_u8str_view_t name, proven_u64 value);

/** @brief Append the empty line that ends a head. */
[[nodiscard]]
proven_err_t proven_http_write_head_end(proven_mem_mut_t out, proven_size_t *len);

/**
 * @brief Append the line that starts a chunk of `size` bytes. Follow it with exactly `size`
 *        bytes of data and then proven_http_write_chunk_end.
 * @return PROVEN_ERR_INVALID_ARG for a size of 0: that is the last chunk, which
 *         proven_http_write_last_chunk writes.
 */
[[nodiscard]]
proven_err_t proven_http_write_chunk_begin(proven_mem_mut_t out, proven_size_t *len, proven_u64 size);

/** @brief Append the CRLF that follows a chunk's data. */
[[nodiscard]]
proven_err_t proven_http_write_chunk_end(proven_mem_mut_t out, proven_size_t *len);

/** @brief Append the zero-size chunk and empty trailer section that end a chunked body. */
[[nodiscard]]
proven_err_t proven_http_write_last_chunk(proven_mem_mut_t out, proven_size_t *len);

/** @brief Append the zero-size chunk and a trailer section of `fields` (since v0.106.0).
 *  @return PROVEN_ERR_INVALID_ARG for a field proven_http_trailer_allowed refuses, or one that is not
 *          a valid field; PROVEN_ERR_OUT_OF_BOUNDS. */
[[nodiscard]]
proven_err_t proven_http_write_trailers(proven_mem_mut_t out, proven_size_t *len, const proven_http_header_t *fields, proven_size_t count);

// -----------------------------------------------------------------------------
// Ranges
// -----------------------------------------------------------------------------

/** @brief Pass as `last` to ask for everything from `first` to the end. */
#define PROVEN_HTTP_RANGE_TO_END UINT64_MAX

/**
 * @brief Append a `Range` header asking for bytes `first` through `last`, inclusive:
 *        `Range: bytes=100-199`, or `Range: bytes=100-` with PROVEN_HTTP_RANGE_TO_END.
 * @return PROVEN_ERR_INVALID_ARG when `last` is before `first`.
 */
[[nodiscard]]
proven_err_t proven_http_write_range(proven_mem_mut_t out, proven_size_t *len, proven_u64 first, proven_u64 last);

/**
 * @brief Append a `Content-Range` header for a 206 response: `Content-Range: bytes 100-199/1000`.
 * @return PROVEN_ERR_INVALID_ARG unless `first <= last < total`.
 */
[[nodiscard]]
proven_err_t proven_http_write_content_range(proven_mem_mut_t out, proven_size_t *len,
                                             proven_u64 first, proven_u64 last, proven_u64 total);

/**
 * @brief Read the value of a request's `Range` header against a resource of `size` bytes.
 *
 * Takes one byte range in any of its three spellings - `bytes=100-199`, `bytes=100-` (to the
 * end) and `bytes=-500` (the last 500) - and gives back the first and last byte to send,
 * clamped to the resource.
 *
 * @return PROVEN_ERR_OUT_OF_BOUNDS when the range starts at or past the end, or asks for the
 *         last zero bytes, or `size` is 0 (answer 416);
 *         PROVEN_ERR_UNSUPPORTED for a unit other than `bytes`, or more than one range - a
 *         server may ignore `Range` and send the whole resource;
 *         PROVEN_ERR_INVALID_FORMAT for anything else (ignore the header, as RFC 9110 says).
 */
[[nodiscard]]
proven_err_t proven_http_range_parse(proven_u8str_view_t value, proven_u64 size, proven_u64 *first, proven_u64 *last);

/**
 * @brief Read the value of a response's `Content-Range` header: `bytes 100-199/1000`, or
 *        `bytes 100-199/` followed by `*` when the total is not known.
 * @param total receives the complete length; `*has_total` is false when it was `*`.
 * @return PROVEN_ERR_INVALID_FORMAT when it is not that, or the numbers contradict each other.
 */
[[nodiscard]]
proven_err_t proven_http_content_range_parse(proven_u8str_view_t value, proven_u64 *first, proven_u64 *last,
                                             proven_u64 *total, bool *has_total);

// -----------------------------------------------------------------------------
// Multipart form data
// -----------------------------------------------------------------------------

/** @brief Bytes in a boundary made by proven_http_multipart_boundary, without a NUL. */
#define PROVEN_HTTP_BOUNDARY_SIZE ((proven_size_t)40)

/**
 * @brief Make a boundary for a `multipart/form-data` body from 16 random bytes you supply.
 *
 * A boundary must not occur inside any part. 128 random bits make that as unlikely as a
 * collision of two UUIDs; draw them from proven_random_bytes or a seeded generator. The result
 * is 40 characters, all of which are safe in a header without quoting.
 */
void proven_http_multipart_boundary(const proven_byte_t random[16], proven_byte_t out[PROVEN_HTTP_BOUNDARY_SIZE]);

/** @brief Append the header that announces the body: `Content-Type: multipart/form-data;
 *         boundary=...`. PROVEN_ERR_INVALID_ARG for a boundary that is empty, longer than 70
 *         characters, or holds a character that would need quoting. */
[[nodiscard]]
proven_err_t proven_http_multipart_write_content_type(proven_mem_mut_t out, proven_size_t *len, proven_u8str_view_t boundary);

/**
 * @brief Append the opening of one part: the boundary line, its `Content-Disposition`, an
 *        optional `Content-Type`, and the empty line. The part's bytes follow, written by you,
 *        and then proven_http_multipart_write_part_end.
 *
 * @param name the form field's name.
 * @param filename empty for an ordinary field; for a file, the name to report.
 * @param content_type empty to omit the header (a field's default is text/plain).
 *
 * A double quote, CR or LF in `name` or `filename` is written percent-encoded, as browsers do,
 * so neither can end the quoted string or the header.
 *
 * @return PROVEN_ERR_INVALID_ARG for an empty name, a control character in `content_type`, or a
 *         bad boundary.
 */
[[nodiscard]]
proven_err_t proven_http_multipart_write_part(proven_mem_mut_t out, proven_size_t *len, proven_u8str_view_t boundary,
                                              proven_u8str_view_t name, proven_u8str_view_t filename,
                                              proven_u8str_view_t content_type);

/** @brief Append the CRLF that follows a part's bytes. */
[[nodiscard]]
proven_err_t proven_http_multipart_write_part_end(proven_mem_mut_t out, proven_size_t *len);

/** @brief Append the closing boundary that ends the whole body. */
[[nodiscard]]
proven_err_t proven_http_multipart_write_end(proven_mem_mut_t out, proven_size_t *len, proven_u8str_view_t boundary);

// -----------------------------------------------------------------------------
// Reading multipart bodies (since v0.105.0)
// -----------------------------------------------------------------------------

/*
 * A multipart body (RFC 2046 section 5.1) is parts, each a small head of fields and then bytes,
 * between lines that hold the boundary: what a browser sends for a form with a file
 * (`multipart/form-data`, RFC 7578), and what a server answers to several ranges at once
 * (`multipart/byteranges`, RFC 9110 section 14.6). The reader below takes such a body in pieces of
 * any size - as it arrives - and tells what it found, one event a call, with views into what it was
 * given: it holds no part, only a part's head (in a buffer of yours) and as many bytes as a boundary
 * line has. It allocates nothing; part of the freestanding profile, as the rest of http.h.
 */

/** @brief The longest boundary RFC 2046 allows. */
#define PROVEN_HTTP_BOUNDARY_MAX ((proven_size_t)70)

/**
 * @brief Find the boundary in a `Content-Type` value of any `multipart/` type:
 *        `multipart/form-data; boundary=----x` (or `boundary="..."`).
 * @param boundary Out: a view into `content_type`, without quotes.
 * @return PROVEN_ERR_NOT_FOUND when the type is not `multipart/...` or has no boundary;
 *         PROVEN_ERR_INVALID_FORMAT for an empty boundary, one over 70 characters, one with a
 *         character RFC 2046 does not allow, or a quoted one with a backslash.
 */
[[nodiscard]]
proven_err_t proven_http_multipart_boundary_parse(proven_u8str_view_t content_type, proven_u8str_view_t *boundary);

/** @brief What one call of proven_http_multipart_feed found. */
typedef enum {
    PROVEN_HTTP_MULTIPART_NONE = 0,     /**< nothing yet: everything given was taken; give more */
    PROVEN_HTTP_MULTIPART_PART,         /**< a part begins: `fields` are its head */
    PROVEN_HTTP_MULTIPART_DATA,         /**< some of the part's bytes, in order: `data` */
    PROVEN_HTTP_MULTIPART_PART_END,     /**< the part is over */
    PROVEN_HTTP_MULTIPART_END           /**< the closing boundary: the body is over; what follows is ignored */
} proven_http_multipart_event_kind_t;

/** @brief An event. The views are good until the next call with the same reader. */
typedef struct {
    proven_http_multipart_event_kind_t kind;
    const proven_http_header_t *fields; /**< PART: the part's fields, in the order sent - views into the head buffer */
    proven_size_t field_count;
    proven_mem_view_t data;             /**< DATA: bytes of the part - a view into the input given, or into the reader */
} proven_http_multipart_event_t;

/**
 * @brief A multipart reader. Caller-owned; start it with proven_http_multipart_init. Its fields
 *        are exposed only so it can live on the stack: do not read or write them.
 */
typedef struct {
    proven_byte_t delim[4 + 70];        /* CR LF - - boundary */
    proven_byte_t hold[4 + 70];         /* the start of what may be a boundary line, held back */
    proven_byte_t spill[4 + 70];        /* held bytes that turned out to be data, handed out from here */
    proven_u8 delim_len, held;
    proven_u8 state;
    proven_mem_mut_t head;
    proven_size_t head_len;
    proven_http_header_t *fields;
    proven_size_t field_cap;
} proven_http_multipart_t;

/**
 * @brief Start reading a body whose boundary is `boundary` (from
 *        proven_http_multipart_boundary_parse).
 * @param head Where a part's head is kept while it is read and while its fields are in use: the
 *             largest head taken. 1 KiB is plenty for a browser's.
 * @param fields Room for a part's fields, and how many: the most taken in one part.
 * @return PROVEN_ERR_INVALID_ARG for a bad boundary, no head buffer or no room for a field.
 */
[[nodiscard]]
proven_err_t proven_http_multipart_init(proven_http_multipart_t *r, proven_u8str_view_t boundary, proven_mem_mut_t head,
                                        proven_http_header_t *fields, proven_size_t field_cap);

/**
 * @brief Read some of the body: take some of `in`, report what was found.
 *
 * Call it again with the rest of `in` (`*used` was taken) until it is all taken, then with the
 * next piece. One event a call: a part's head (PART), its bytes (DATA, perhaps in several
 * pieces - bytes held back from an earlier piece among them), its end (PART_END), and once, the end (END). Bytes before
 * the first boundary (the preamble) and after the last (the epilogue) are passed over. A piece that
 * ends in what may be the start of a boundary line keeps those bytes until the next piece decides:
 * they come out as DATA then, if they are data.
 *
 * @return PROVEN_OK; PROVEN_ERR_INVALID_FORMAT for a boundary line followed by something other than
 *         `--`, white space and CRLF, or a head that is not fields; PROVEN_ERR_OUT_OF_BOUNDS for a
 *         head larger than the buffer, or with more fields than there is room for;
 *         PROVEN_ERR_INVALID_ARG. A preamble is passed over however long it is: bound the body.
 *         After an error the reader is stuck: start again.
 */
[[nodiscard]]
proven_err_t proven_http_multipart_feed(proven_http_multipart_t *r, proven_mem_view_t in, proven_size_t *used, proven_http_multipart_event_t *ev);

/** @brief Whether the body was whole: PROVEN_OK after END, PROVEN_ERR_INVALID_FORMAT when it ended
 *         before its closing boundary (call it when the body's bytes are over). */
[[nodiscard]]
proven_err_t proven_http_multipart_end(const proven_http_multipart_t *r);

/** @brief A `Content-Disposition` value (RFC 6266, RFC 7578 section 4.2): the type and the parameters a
 *         form part has. Views into the value; a parameter not there is empty with its flag false. */
typedef struct {
    proven_u8str_view_t type;           /**< `form-data`, `attachment`, `inline` */
    proven_u8str_view_t name;           /**< the form field's name */
    proven_u8str_view_t filename;       /**< the file's name as sent: browsers percent-encode `"`, CR and LF in it (proven_http_multipart_write_part does too) and send the rest as is */
    proven_u8str_view_t filename_ext;   /**< `filename*` (RFC 8187), undecoded: `UTF-8''%e2%82%ac%20rates.txt` */
    bool has_name, has_filename, has_filename_ext;
} proven_http_disposition_t;

/**
 * @brief Read a `Content-Disposition` value. Parameter names compare without case; a parameter
 *        given twice is refused.
 * @return PROVEN_ERR_INVALID_FORMAT for a value that is not a type and parameters;
 *         PROVEN_ERR_UNSUPPORTED for a quoted value with a backslash escape (RFC 7578 says senders
 *         percent-encode instead; this reader returns views and cannot remove them).
 */
[[nodiscard]]
proven_err_t proven_http_content_disposition_parse(proven_u8str_view_t value, proven_http_disposition_t *out);

// -----------------------------------------------------------------------------
// Several ranges (since v0.105.0)
// -----------------------------------------------------------------------------

/** @brief One byte range, `first` to `last` inclusive. */
typedef struct {
    proven_u64 first, last;
} proven_http_range_t;

/**
 * @brief Read a request's `Range` against a resource of `size` bytes, with up to `cap` ranges, as
 *        RFC 9110 section 14.2 suggests a server answer it.
 *
 * Every spelling of proven_http_range_parse, separated by commas. Ranges that start at or past the
 * end are dropped, the rest cut to the end, sorted by where they start, and those that overlap or
 * touch are merged into one. One range left is a `206` with `Content-Range`; several, a `206`
 * whose body is `multipart/byteranges` (proven_http_byteranges_write_part).
 *
 * @return PROVEN_ERR_OUT_OF_BOUNDS when no range is satisfiable (answer 416 with
 *         `Content-Range: bytes STAR/size`); PROVEN_ERR_UNSUPPORTED for a unit other than `bytes`,
 *         more than `cap` ranges as asked, or ranges that together ask for more than the resource
 *         (the pattern of a request meant to make a server work, RFC 9110 section 14.2) - a server
 *         may send the whole resource with `200`; PROVEN_ERR_INVALID_FORMAT for anything else.
 */
[[nodiscard]]
proven_err_t proven_http_range_parse_list(proven_u8str_view_t value, proven_u64 size, proven_http_range_t *ranges,
                                          proven_size_t cap, proven_size_t *count);

/** @brief Append `Content-Type: multipart/byteranges; boundary=...`. PROVEN_ERR_INVALID_ARG as
 *         proven_http_multipart_write_content_type. */
[[nodiscard]]
proven_err_t proven_http_byteranges_write_content_type(proven_mem_mut_t out, proven_size_t *len, proven_u8str_view_t boundary);

/**
 * @brief Append the opening of one part of a `multipart/byteranges` body: the boundary line, the
 *        resource's `Content-Type` (omitted when empty) and `Content-Range: bytes first-last/size`,
 *        and the empty line. The range's bytes follow, then proven_http_multipart_write_part_end;
 *        after the last part, proven_http_multipart_write_end.
 * @return PROVEN_ERR_INVALID_ARG for a bad boundary, a control character in `content_type`, or a
 *         range that is not inside `size`.
 */
[[nodiscard]]
proven_err_t proven_http_byteranges_write_part(proven_mem_mut_t out, proven_size_t *len, proven_u8str_view_t boundary,
                                               proven_u8str_view_t content_type, proven_u64 first, proven_u64 last, proven_u64 size);

// -----------------------------------------------------------------------------
// Dates
// -----------------------------------------------------------------------------

/** @brief Bytes in an HTTP date, without a NUL: `Sun, 06 Nov 1994 08:49:37 GMT`. */
#define PROVEN_HTTP_DATE_SIZE ((proven_size_t)29)

/**
 * @brief Write a wall-clock time as an HTTP date (IMF-fixdate), for `Date`, `Last-Modified`
 *        and `Expires`. Always 29 bytes, always GMT, never localized.
 * @param wall_ns a proven_time_now reading - NOT the monotonic clock, which has no date.
 * @return PROVEN_ERR_INVALID_ARG for a time before 1970.
 */
[[nodiscard]]
proven_err_t proven_http_date_format(proven_time_t wall_ns, proven_byte_t out[PROVEN_HTTP_DATE_SIZE]);

/**
 * @brief Read an HTTP date in any of the three forms a recipient must accept: IMF-fixdate, the
 *        obsolete RFC 850 form (`Sunday, 06-Nov-94 08:49:37 GMT`) and asctime
 *        (`Sun Nov  6 08:49:37 1994`).
 *
 * A two-digit year is read as RFC 9110 says: as the most recent year with those last two
 * digits that is not more than 50 years in the future of `now_ns`.
 *
 * @param now_ns the current wall-clock time, used only for that two-digit rule.
 * @return PROVEN_ERR_INVALID_FORMAT for anything else, including a date that does not exist
 *         (31 Feb), a year before 1970, and a weekday that does not match the date.
 *         PROVEN_ERR_OVERFLOW for a valid date later than proven_time_t can hold - its
 *         nanoseconds end in April 2262. Servers send such dates on purpose (`Expires` in the
 *         year 9999 means "never"): treat this answer as "far in the future", not as an error
 *         in the message.
 */
[[nodiscard]]
proven_err_t proven_http_date_parse(proven_u8str_view_t text, proven_time_t now_ns, proven_time_t *out_wall_ns);

#endif /* PROVEN_HTTP_H */
