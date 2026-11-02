#ifndef PROVEN_H2_H
#define PROVEN_H2_H

#include "proven/types.h"
#include "proven/memory.h"
#include "proven/http.h"

/**
 * @file h2.h
 * @brief HTTP/2 (RFC 9113): frames, and the rules for a header list.
 *
 * HTTP/2 carries the same requests and responses as HTTP/1.1 in another shape. Everything is
 * a *frame* - nine bytes of header and a payload - on a numbered *stream*; many streams share
 * one connection; header fields travel compressed (hpack.h) and split across frames; and
 * what HTTP/1.1 said in its first line is said by *pseudo-header fields* (`:method`, `:path`,
 * `:status`).
 *
 * This header has two layers. The first has no state: reading and writing frames, the checks
 * each frame type demands, and the checks a decoded header list must pass before it is a
 * request or a response. The second is a connection (proven_h2_conn_t): which streams are
 * open, how much may be sent, what to answer. Both are pure computation and available in a
 * freestanding build; neither touches a socket.
 *
 * **Errors come in two sizes in HTTP/2**, and the functions here say which: a *connection
 * error* ends the connection (after a GOAWAY frame saying why), a *stream error* ends one
 * stream (with RST_STREAM) and leaves the rest alone. Each check reports the code to send.
 */

/** @brief What a client sends first on a connection, before any frame: 24 bytes. */
#define PROVEN_H2_PREFACE "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define PROVEN_H2_PREFACE_SIZE ((proven_size_t)24)

/** @brief Every frame begins with this many bytes of header. */
#define PROVEN_H2_FRAME_HEADER_SIZE ((proven_size_t)9)

/** @brief The largest payload every endpoint must accept, and the largest any may allow. */
#define PROVEN_H2_MIN_MAX_FRAME_SIZE ((proven_u32)16384)
#define PROVEN_H2_MAX_MAX_FRAME_SIZE ((proven_u32)16777215)

/** @brief The largest flow-control window, and the one a stream begins with. */
#define PROVEN_H2_MAX_WINDOW ((proven_u32)0x7fffffff)
#define PROVEN_H2_DEFAULT_WINDOW ((proven_u32)65535)

typedef enum {
    PROVEN_H2_DATA = 0x0,
    PROVEN_H2_HEADERS = 0x1,
    PROVEN_H2_PRIORITY = 0x2,          /**< deprecated by RFC 9113: checked for size and ignored */
    PROVEN_H2_RST_STREAM = 0x3,
    PROVEN_H2_SETTINGS = 0x4,
    PROVEN_H2_PUSH_PROMISE = 0x5,
    PROVEN_H2_PING = 0x6,
    PROVEN_H2_GOAWAY = 0x7,
    PROVEN_H2_WINDOW_UPDATE = 0x8,
    PROVEN_H2_CONTINUATION = 0x9
} proven_h2_frame_type_t;

/** @brief Frame flags. A flag means something only on the frame types named. */
#define PROVEN_H2_FLAG_END_STREAM ((proven_u8)0x01)    /**< DATA, HEADERS: the sender's last frame on this stream */
#define PROVEN_H2_FLAG_ACK ((proven_u8)0x01)           /**< SETTINGS, PING: an acknowledgement */
#define PROVEN_H2_FLAG_END_HEADERS ((proven_u8)0x04)   /**< HEADERS, PUSH_PROMISE, CONTINUATION: the header block ends here */
#define PROVEN_H2_FLAG_PADDED ((proven_u8)0x08)        /**< DATA, HEADERS, PUSH_PROMISE: a length byte and padding surround the content */
#define PROVEN_H2_FLAG_PRIORITY ((proven_u8)0x20)      /**< HEADERS: five bytes of (deprecated) priority precede the block */

/** @brief Error codes, as sent in RST_STREAM and GOAWAY (RFC 9113, section 7). */
typedef enum {
    PROVEN_H2_NO_ERROR = 0x0,
    PROVEN_H2_PROTOCOL_ERROR = 0x1,
    PROVEN_H2_INTERNAL_ERROR = 0x2,
    PROVEN_H2_FLOW_CONTROL_ERROR = 0x3,
    PROVEN_H2_SETTINGS_TIMEOUT = 0x4,
    PROVEN_H2_STREAM_CLOSED = 0x5,
    PROVEN_H2_FRAME_SIZE_ERROR = 0x6,
    PROVEN_H2_REFUSED_STREAM = 0x7,
    PROVEN_H2_CANCEL = 0x8,
    PROVEN_H2_COMPRESSION_ERROR = 0x9,
    PROVEN_H2_CONNECT_ERROR = 0xa,
    PROVEN_H2_ENHANCE_YOUR_CALM = 0xb,
    PROVEN_H2_INADEQUATE_SECURITY = 0xc,
    PROVEN_H2_HTTP_1_1_REQUIRED = 0xd
} proven_h2_error_t;

/** @brief Settings (RFC 9113, section 6.5.2). */
typedef enum {
    PROVEN_H2_SETTINGS_HEADER_TABLE_SIZE = 0x1,
    PROVEN_H2_SETTINGS_ENABLE_PUSH = 0x2,
    PROVEN_H2_SETTINGS_MAX_CONCURRENT_STREAMS = 0x3,
    PROVEN_H2_SETTINGS_INITIAL_WINDOW_SIZE = 0x4,
    PROVEN_H2_SETTINGS_MAX_FRAME_SIZE = 0x5,
    PROVEN_H2_SETTINGS_MAX_HEADER_LIST_SIZE = 0x6,
    PROVEN_H2_SETTINGS_ENABLE_CONNECT_PROTOCOL = 0x8  /**< RFC 8441: 1 from a server that takes extended CONNECT */
} proven_h2_setting_id_t;

/** @brief One setting: an identifier and its value. */
typedef struct {
    proven_u16 id;
    proven_u32 value;
} proven_h2_setting_t;

/** @brief A frame header: everything about a frame but its payload. */
typedef struct {
    proven_u32 length;                 /**< bytes of payload that follow the header: at most 2^24-1 */
    proven_u8 type;                    /**< a proven_h2_frame_type_t, or a type this library does not know */
    proven_u8 flags;
    proven_u32 stream_id;              /**< 0 for the connection itself; the reserved top bit is dropped on reading */
} proven_h2_frame_t;

// -----------------------------------------------------------------------------
// Frames
// -----------------------------------------------------------------------------

/**
 * @brief Read a frame header from the front of `data`.
 * @return PROVEN_ERR_NEED_MORE when `data` is shorter than nine bytes. Nothing else can be
 *         wrong with nine bytes: what they may say is proven_h2_frame_check's business.
 */
[[nodiscard]]
proven_err_t proven_h2_frame_parse(proven_mem_view_t data, proven_h2_frame_t *out);

/**
 * @brief Append a frame header at `out.ptr[*len]`. The payload is yours to write after it.
 * @return PROVEN_ERR_INVALID_ARG for a length above 2^24-1 or a stream identifier above 2^31-1;
 *         PROVEN_ERR_OUT_OF_BOUNDS when `out` is too small, with `*len` unchanged.
 */
[[nodiscard]]
proven_err_t proven_h2_frame_write(proven_mem_mut_t out, proven_size_t *len, const proven_h2_frame_t *frame);

/**
 * @brief Check a frame header against the rules of its type, before its payload is read.
 *
 * Size and stream: no frame is longer than `max_frame_size` (what this side announced);
 * DATA, HEADERS, PRIORITY, RST_STREAM and CONTINUATION need a stream, SETTINGS, PING and
 * GOAWAY must not have one; RST_STREAM and WINDOW_UPDATE are four bytes, PING eight, PRIORITY
 * five, a SETTINGS frame a multiple of six and its acknowledgement empty, GOAWAY at least
 * eight. A type this library does not know passes, as the protocol requires: skip its
 * payload.
 *
 * @param code receives the error code to send when the frame is refused.
 * @param stream_error receives true when the fault ends only the frame's stream (RST_STREAM),
 *        false when it ends the connection (GOAWAY).
 * @return PROVEN_ERR_INVALID_FORMAT when the frame is refused.
 */
[[nodiscard]]
proven_err_t proven_h2_frame_check(const proven_h2_frame_t *frame, proven_u32 max_frame_size, proven_h2_error_t *code, bool *stream_error);

/**
 * @brief The content of a DATA or HEADERS payload: without its padding, and for HEADERS
 *        without the five bytes of priority when those are present.
 *
 * `*content` is a view into `payload`. For flow control the whole payload counts, padding
 * included; this says only where the content is.
 *
 * @return PROVEN_ERR_INVALID_FORMAT (a connection error, PROTOCOL_ERROR) when the padding is
 *         as long as the payload or longer, or the payload too short for what its flags
 *         promise; PROVEN_ERR_INVALID_ARG for another frame type.
 */
[[nodiscard]]
proven_err_t proven_h2_frame_content(const proven_h2_frame_t *frame, proven_mem_view_t payload, proven_mem_view_t *content);

/**
 * @brief Read the next setting of a SETTINGS payload, from `payload.ptr[*pos]`.
 *
 * A setting this library does not know is returned like any other: ignore it, as the protocol
 * requires. A known one outside its range is refused.
 *
 * @return PROVEN_ERR_EOF at the end of the payload. PROVEN_ERR_INVALID_FORMAT with `*code` -
 *         ENABLE_PUSH or ENABLE_CONNECT_PROTOCOL other than 0 or 1 and MAX_FRAME_SIZE outside 2^14..2^24-1
 *         (PROTOCOL_ERROR), INITIAL_WINDOW_SIZE above 2^31-1 (FLOW_CONTROL_ERROR): each a
 *         connection error.
 */
[[nodiscard]]
proven_err_t proven_h2_settings_next(proven_mem_view_t payload, proven_size_t *pos, proven_h2_setting_t *out, proven_h2_error_t *code);

/** @brief Append a whole SETTINGS frame carrying `count` settings (none: an empty frame). */
[[nodiscard]]
proven_err_t proven_h2_write_settings(proven_mem_mut_t out, proven_size_t *len, const proven_h2_setting_t *settings, proven_size_t count);

/** @brief Append the acknowledgement of a SETTINGS frame. */
[[nodiscard]]
proven_err_t proven_h2_write_settings_ack(proven_mem_mut_t out, proven_size_t *len);

/** @brief Append a PING frame with eight bytes of your choosing, or its acknowledgement
 *         carrying the eight that were received. */
[[nodiscard]]
proven_err_t proven_h2_write_ping(proven_mem_mut_t out, proven_size_t *len, const proven_byte_t opaque[8], bool ack);

/** @brief Append a WINDOW_UPDATE frame: `increment` more bytes may be sent on `stream_id`
 *         (0: on the connection). PROVEN_ERR_INVALID_ARG for an increment of 0 or above 2^31-1. */
[[nodiscard]]
proven_err_t proven_h2_write_window_update(proven_mem_mut_t out, proven_size_t *len, proven_u32 stream_id, proven_u32 increment);

/** @brief Append a RST_STREAM frame. PROVEN_ERR_INVALID_ARG for stream 0. */
[[nodiscard]]
proven_err_t proven_h2_write_rst_stream(proven_mem_mut_t out, proven_size_t *len, proven_u32 stream_id, proven_h2_error_t code);

/** @brief Append a GOAWAY frame: no stream above `last_stream_id` was or will be acted on. */
[[nodiscard]]
proven_err_t proven_h2_write_goaway(proven_mem_mut_t out, proven_size_t *len, proven_u32 last_stream_id, proven_h2_error_t code, proven_mem_view_t debug);

/** @brief Read the payload of a WINDOW_UPDATE frame that passed proven_h2_frame_check.
 *  @return PROVEN_ERR_INVALID_FORMAT for an increment of zero: PROTOCOL_ERROR, on the stream
 *          when the frame names one, on the connection otherwise. */
[[nodiscard]]
proven_err_t proven_h2_read_window_update(proven_mem_view_t payload, proven_u32 *increment);

/** @brief Read the payload of a RST_STREAM frame that passed proven_h2_frame_check. */
[[nodiscard]]
proven_err_t proven_h2_read_rst_stream(proven_mem_view_t payload, proven_u32 *code);

/** @brief Read the payload of a GOAWAY frame that passed proven_h2_frame_check. `*debug` is a
 *         view into `payload`. */
[[nodiscard]]
proven_err_t proven_h2_read_goaway(proven_mem_view_t payload, proven_u32 *last_stream_id, proven_u32 *code, proven_mem_view_t *debug);

// -----------------------------------------------------------------------------
// A header list as a message (RFC 9113, section 8)
// -----------------------------------------------------------------------------

/** @brief What a request's pseudo-header fields say. Views into the header list. */
typedef struct {
    proven_u8str_view_t method;        /**< `:method` */
    proven_u8str_view_t scheme;        /**< `:scheme`; empty for CONNECT */
    proven_u8str_view_t authority;     /**< `:authority`; empty when the client sent none */
    proven_u8str_view_t path;          /**< `:path`: path and query; empty for CONNECT */
    const proven_http_header_t *fields;    /**< the ordinary fields, which follow the pseudo-header fields */
    proven_size_t field_count;
    proven_u8str_view_t protocol;      /**< `:protocol` of an extended CONNECT (RFC 8441): `websocket`, say; empty otherwise */
} proven_h2_request_head_t;

/**
 * @brief Check a decoded header list as a request, and take it apart.
 *
 * Malformed - PROVEN_ERR_INVALID_FORMAT, a stream error of type PROTOCOL_ERROR, never to be
 * passed on - is: a field name with an uppercase letter, a space, a control or a non-ASCII
 * byte, or a colon anywhere but first; a value with NUL, CR or LF, or beginning or ending
 * with a space or tab; a pseudo-header field after an ordinary one, twice, or unknown;
 * `:status` in a request; `:method`, `:scheme` or `:path` missing, or `:path` empty for
 * `http` and `https` (a CONNECT request has `:method` and `:authority` and neither of the
 * other two - unless it is an extended CONNECT, RFC 8441: `:protocol` with CONNECT only,
 * not empty, and then `:scheme`, `:path` and `:authority` all present); `connection`,
 * `proxy-connection`, `keep-alive`, `transfer-encoding` or `upgrade`; `te` with anything but
 * `trailers`. Whether `:protocol` is allowed at all is the connection's to say (the
 * `enable_connect_protocol` setting): this function only checks its form.
 *
 * These are the rules a message smuggled through a translation to HTTP/1.1 would break.
 */
[[nodiscard]]
proven_err_t proven_h2_request_head(const proven_http_header_t *headers, proven_size_t count, proven_h2_request_head_t *out);

/**
 * @brief Check a decoded header list as a response. As proven_h2_request_head, with `:status`
 *        - three digits - as the one pseudo-header field, required.
 * @param fields receives the ordinary fields; @param field_count how many.
 */
[[nodiscard]]
proven_err_t proven_h2_response_head(const proven_http_header_t *headers, proven_size_t count, proven_u16 *status,
                                     const proven_http_header_t **fields, proven_size_t *field_count);

/** @brief Check a decoded header list as trailers: ordinary fields under the same rules, and
 *         no pseudo-header field at all. */
[[nodiscard]]
proven_err_t proven_h2_trailers_check(const proven_http_header_t *headers, proven_size_t count);

// -----------------------------------------------------------------------------
// A connection (RFC 9113, sections 3, 5 and 6)
// -----------------------------------------------------------------------------

/**
 * @brief One HTTP/2 connection, for either end: its settings, its streams, both directions'
 *        flow control and header compression. Opaque.
 *
 * It does no I/O and reads no clock. You give it the bytes the peer sent
 * (proven_h2_conn_feed) and it tells you what they mean, one event at a time; you ask it to
 * send, and it puts frames where you collect them (proven_h2_conn_pending_output). A socket,
 * TLS and every timeout are yours - or a driver's.
 *
 * **What the peer can make it hold or do is bounded by the configuration**: streams open at
 * once, the size of a header list, the frames it will take, the output it queues for a peer
 * that does not read, and how often streams may be reset without one finishing.
 *
 * A connection is for one thread at a time. Every pointer and view it hands out is good until
 * the next call that takes the connection and is not marked `const`.
 */
typedef struct proven_h2_conn proven_h2_conn_t;

/**
 * @brief How a connection behaves. Zero-initialise it, set `alloc` and `server`, and set what
 *        you need: every zero field has the default written beside it.
 */
typedef struct {
    proven_allocator_t alloc;              /**< required */
    bool server;                           /**< true: the end that receives the client preface and answers requests */
    proven_u32 max_concurrent_streams;     /**< streams the peer may have open at once, and the most this side opens. 0: 100 */
    proven_u32 initial_window;             /**< bytes of body the peer may send on a stream before this side says it has consumed some; the connection as a whole has the same allowance, and never less than 65,535. 0: 65,535 */
    proven_u32 max_frame_size;             /**< largest frame payload accepted: 16,384 to 1,048,576. 0: 16,384 */
    proven_u32 max_header_list_bytes;      /**< largest header list accepted, as HPACK counts it; at most 1 MiB. 0: 16 KiB */
    proven_u32 header_table_bytes;         /**< the HPACK table in each direction, when `set_header_table` is true. Otherwise 4,096 */
    bool set_header_table;                 /**< true: use `header_table_bytes` (0 keeps no table: no memory, larger headers) */
    proven_size_t max_buffered_output;     /**< output held for a peer that is not reading before more is refused - and, counted apart, the answers the protocol owes that peer before the connection is ended. 0: 64 KiB */
    proven_u32 max_reset_debt;             /**< streams reset - by the peer, or for its fault - beyond those that finished properly, before the connection is ended. 0: 100 */
    proven_mem_mut_t header_buffer;        /**< optional: memory of yours for reading header lists, of at least proven_h2_header_buffer_size bytes and aligned as a pointer. Connections used by one thread may share one: what is read into it is what an event reports, good until the next call on any of them. Empty: each connection allocates its own while it has streams open */
    bool enable_connect_protocol;          /**< a server: say SETTINGS_ENABLE_CONNECT_PROTOCOL (RFC 8441) and take extended CONNECT - a request with `:protocol`. Without it such a request is refused (PROTOCOL_ERROR for the stream). Ignored on a client */
} proven_h2_config_t;

typedef enum {
    PROVEN_H2_EVENT_NONE = 0,              /**< everything given was used; read more */
    PROVEN_H2_EVENT_HEADERS,               /**< a server: a request began. A client: a response head arrived (an interim one when `status` is below 200) */
    PROVEN_H2_EVENT_DATA,                  /**< a piece of a body, possibly empty when it is the last. Say when you have consumed it: proven_h2_conn_consumed */
    PROVEN_H2_EVENT_TRAILERS,              /**< trailer fields: the peer's side of the stream is over */
    PROVEN_H2_EVENT_STREAM_RESET,          /**< the stream is over before its end: the peer reset it, broke a rule on it, or said goodbye without acting on it (PROVEN_H2_REFUSED_STREAM: safe to send again elsewhere). `code` says which. PROVEN_H2_NO_ERROR from a server: the response is complete and the rest of the request is not wanted */
    PROVEN_H2_EVENT_STREAM_WRITABLE,       /**< a send that was cut short can go on */
    PROVEN_H2_EVENT_GOAWAY,                /**< the peer will take no new stream; those above `last_stream_id` were not acted on. `data` is the text it sent with that, if any */
    PROVEN_H2_EVENT_PONG,                  /**< the answer to proven_h2_conn_ping; `data` is the eight bytes */
    PROVEN_H2_EVENT_CLOSED                 /**< the connection is over, and with it every stream still open - they get no event of their own. `code` says why: PROVEN_H2_NO_ERROR after a goodbye from either side once the last stream has ended. Send what is pending and close */
} proven_h2_event_kind_t;

/** @brief What proven_h2_conn_feed found. The views are good until the next call that takes
 *         the connection. */
typedef struct {
    proven_h2_event_kind_t kind;
    proven_u32 stream_id;
    void *user;                            /**< what proven_h2_conn_stream_set_user attached to the stream, for every event that names one */
    bool end_stream;                       /**< HEADERS, DATA: the peer will send nothing more on this stream. STREAM_RESET on a client: the response is complete - the reset is PROVEN_H2_NO_ERROR and came after the response's END_STREAM, or after every byte its content-length declared (never for a response with no content-length) */
    proven_h2_request_head_t request;      /**< HEADERS on a server: the request's parts; `request.fields` are its ordinary fields */
    proven_u16 status;                     /**< HEADERS on a client */
    const proven_http_header_t *fields;    /**< HEADERS on a client, TRAILERS: the ordinary fields */
    proven_size_t field_count;
    proven_mem_view_t data;                /**< DATA: the piece, inside the buffer you fed. PONG: the eight bytes. GOAWAY: the frame's debug text - its first 256 bytes, empty when there was none or no memory for it; bytes from the peer, for a log and not for a decision */
    proven_u32 code;                       /**< STREAM_RESET, GOAWAY, CLOSED: a proven_h2_error_t, or a code this library does not know */
    bool too_large;                        /**< STREAM_RESET: this side reset the stream because a header list from the peer was larger than `max_header_list_bytes` (`code` is PROVEN_H2_PROTOCOL_ERROR) */
    proven_u32 last_stream_id;             /**< GOAWAY */
} proven_h2_event_t;

/** @brief How large `header_buffer` must be for a header list limit (0: the default, 16 KiB):
 *         five times the limit and a little. */
[[nodiscard]]
proven_size_t proven_h2_header_buffer_size(proven_u32 max_header_list_bytes);

/**
 * @brief Make a connection. A client's preface and both ends' first SETTINGS frame are
 *        pending output at once.
 * @return PROVEN_ERR_INVALID_ARG for an invalid allocator, a setting outside its range, or a
 *         `header_buffer` that is too small or misaligned; PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_err_t proven_h2_conn_create(const proven_h2_config_t *config, proven_h2_conn_t **out);

/** @brief Free a connection and everything it holds. Null is accepted. */
void proven_h2_conn_destroy(proven_h2_conn_t *conn);

/**
 * @brief Give the connection bytes the peer sent, and learn what they mean.
 *
 * Uses bytes of `in` - `*consumed` says how many - until there is one event to report or `in`
 * is used up. Call it again with the rest; when it reports PROVEN_H2_EVENT_NONE, read more.
 * It may be called with nothing, to collect an event that needs no input (a stream that
 * became writable when output was sent).
 *
 * A DATA event's `data` points into `in`: a body of any size passes through without being
 * copied or held. So a frame that arrives over several reads is several events, and
 * `end_stream` is on the last of them. Header fields are held by the connection until the
 * next call.
 *
 * The connection answers what the protocol requires by itself - acknowledgements, window
 * updates, resets and the final GOAWAY go to proven_h2_conn_pending_output - so after every
 * call, send what is pending.
 *
 * @return PROVEN_OK, also with PROVEN_H2_EVENT_CLOSED: the peer's fault or farewell is an
 *         event, not an error of this call. PROVEN_ERR_INVALID_FORMAT on a server whose
 *         client did not begin with the HTTP/2 preface - **this is not HTTP/2**, nothing was
 *         queued for it, and the bytes are yours to treat as something else; it is reported
 *         at the first byte that differs. PROVEN_ERR_INVALID_STATE after the end was
 *         reported; PROVEN_ERR_NOMEM (the connection is then over as well).
 */
[[nodiscard]]
proven_err_t proven_h2_conn_feed(proven_h2_conn_t *conn, proven_mem_view_t in, proven_size_t *consumed, proven_h2_event_t *event);

/** @brief Bytes waiting to be sent to the peer. A view into the connection, good until the
 *         next call that takes it. */
[[nodiscard]]
proven_mem_view_t proven_h2_conn_pending_output(const proven_h2_conn_t *conn);

/** @brief Say that the first `n` pending bytes went out. */
void proven_h2_conn_output_sent(proven_h2_conn_t *conn, proven_size_t n);

/**
 * @brief A client: begin a request. The header list is the pseudo-header fields
 *        (`:method`, `:scheme`, `:authority`, `:path`) and then the ordinary ones, names in
 *        lowercase.
 * @param end_stream true when no body follows.
 * @param stream_id receives the new stream's number.
 * @return PROVEN_ERR_INVALID_ARG for a list proven_h2_request_head refuses, or on a server;
 *         PROVEN_ERR_BUSY when as many streams are open as the peer or this side allows - try
 *         again when one has ended - or when `max_buffered_output` bytes are waiting for a
 *         peer that does not read - try again when some were sent;
 *         PROVEN_ERR_INVALID_STATE after a GOAWAY or the end, or when the stream numbers
 *         are used up; PROVEN_ERR_OUT_OF_BOUNDS for a list larger than the peer said it
 *         accepts; PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_err_t proven_h2_conn_submit_request(proven_h2_conn_t *conn, const proven_http_header_t *headers, proven_size_t count,
                                           bool end_stream, proven_u32 *stream_id);

/**
 * @brief A server: answer a request. The list begins with `:status`. An interim response
 *        (status below 200) may be sent before the final one.
 *
 * A response that ends before the request's body did tells the client to stop sending it
 * (RST_STREAM with NO_ERROR, as RFC 9113 section 8.1 provides): the stream is then over.
 *
 * @return PROVEN_ERR_INVALID_ARG for a list proven_h2_response_head refuses, or on a client;
 *         PROVEN_ERR_NOT_FOUND for a stream that is not open; PROVEN_ERR_INVALID_STATE when
 *         the final response was already sent; PROVEN_ERR_BUSY when `max_buffered_output`
 *         bytes are waiting - PROVEN_H2_EVENT_STREAM_WRITABLE says when to try again;
 *         PROVEN_ERR_OUT_OF_BOUNDS for a list larger than the peer accepts; PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_err_t proven_h2_conn_submit_response(proven_h2_conn_t *conn, proven_u32 stream_id, const proven_http_header_t *headers, proven_size_t count,
                                            bool end_stream);

/**
 * @brief Send body bytes on a stream. **It may take fewer than you offer** - as many as the
 *        stream's window, the connection's window and the output limit allow, possibly none.
 *        Keep the rest and offer it again after PROVEN_H2_EVENT_STREAM_WRITABLE.
 * @param end_stream true when this is the end of the body; it takes effect when all of
 *        `data` has been taken. With no data at all it always takes effect: the result 0
 *        then means "ended", not "nothing fitted".
 * @return the bytes taken. PROVEN_ERR_NOT_FOUND for a stream that is not open;
 *         PROVEN_ERR_INVALID_STATE before the headers, or after this side ended the stream.
 */
[[nodiscard]]
proven_result_size_t proven_h2_conn_send_data(proven_h2_conn_t *conn, proven_u32 stream_id, proven_mem_view_t data, bool end_stream);

/** @brief End this side of a stream with trailer fields (ordinary fields only).
 *  @return as proven_h2_conn_submit_response. */
[[nodiscard]]
proven_err_t proven_h2_conn_send_trailers(proven_h2_conn_t *conn, proven_u32 stream_id, const proven_http_header_t *headers, proven_size_t count);

/**
 * @brief Mark fields of the **next** header list this connection is given - by
 *        proven_h2_conn_submit_request, proven_h2_conn_submit_response or
 *        proven_h2_conn_send_trailers - as ones whose values no one may remember: they are
 *        sent in the form that tells every hop never to index them (see proven_hpack_encode),
 *        as `authorization` and `proxy-authorization` always are.
 * @param sensitive one flag for each field of that list, pseudo-header fields included, in
 *        its order. It is read by that call and must be good until then; the marks are
 *        dropped by that call whether it succeeds or not. Null takes them back.
 */
void proven_h2_conn_next_sensitive(proven_h2_conn_t *conn, const bool *sensitive);

/** @brief Give up on a stream: the peer is told with `code` (PROVEN_H2_CANCEL is the usual
 *         one), nothing more is reported for it, and what the peer had already sent on it
 *         is dropped as it arrives.
 *  @return PROVEN_ERR_NOT_FOUND for a stream that is not open. */
[[nodiscard]]
proven_err_t proven_h2_conn_reset(proven_h2_conn_t *conn, proven_u32 stream_id, proven_h2_error_t code);

/**
 * @brief Say that `n` bytes of a stream's DATA have been dealt with.
 *
 * This is the backpressure of HTTP/2: the peer may send only what this side has room for, and
 * room comes back when you say so here. A handler that falls behind and stops calling this
 * stops its peer, at the cost of `initial_window` bytes a stream.
 *
 * Only what a DATA event carried is yours to account for. Padding, and data for a stream that
 * was reset, the connection gives back by itself; and when a stream ends, whatever it had not
 * consumed is given back to the connection with it - so this call does nothing for a stream
 * that is no longer open, and nothing is lost by not making it.
 */
void proven_h2_conn_consumed(proven_h2_conn_t *conn, proven_u32 stream_id, proven_size_t n);

/** @brief Ask whether the peer is there: PROVEN_H2_EVENT_PONG follows with the same bytes.
 *  @return PROVEN_ERR_INVALID_STATE after the end. */
[[nodiscard]]
proven_err_t proven_h2_conn_ping(proven_h2_conn_t *conn, const proven_byte_t opaque[8]);

/** @brief Say goodbye: a GOAWAY frame is queued that names the last stream of the peer's
 *         this side acted on, no new stream is taken or begun, and - with
 *         PROVEN_H2_NO_ERROR - the streams that are open may finish:
 *         PROVEN_H2_EVENT_CLOSED follows when the last has ended. Any other code ends the
 *         connection at once. */
void proven_h2_conn_shutdown(proven_h2_conn_t *conn, proven_h2_error_t code);

/**
 * @brief Change how many streams the peer may have open at once (SETTINGS_MAX_CONCURRENT_STREAMS),
 *        after the opening: a SETTINGS frame is queued, and the number holds once the peer
 *        acknowledges it.
 *
 * `n` is 0 to the configuration's `max_concurrent_streams`: what a connection may come to hold
 * is fixed when it is made, so a change lowers the number or raises it back, never above.
 * Streams already open are not touched, however many there are. Until the peer acknowledges,
 * it may still open streams under the number before, and those are taken; after, a stream
 * over the number is refused with PROVEN_H2_REFUSED_STREAM, which the peer may send again.
 *
 * On a client the number is also the most this side opens, and that holds at once.
 *
 * One change is in flight at a time. A call made while one is waiting for its
 * acknowledgement is kept, and the latest values of both calls go out together when it comes.
 *
 * @return PROVEN_ERR_INVALID_ARG for a null connection or `n` above that ceiling;
 *         PROVEN_ERR_INVALID_STATE after a goodbye of this side or the end; PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_err_t proven_h2_conn_set_max_streams(proven_h2_conn_t *conn, proven_u32 n);

/**
 * @brief Change the body bytes the peer may send on each stream before this side gives room
 *        back (SETTINGS_INITIAL_WINDOW_SIZE), after the opening.
 *
 * `n` is 0 to the configuration's `initial_window` (the ceiling, as for streams). Every open
 * stream's window moves by the difference, as RFC 9113 section 6.9.2 says, and a stream begun
 * later starts at `n`. A raise holds at once; a lowering once the peer acknowledges it, so
 * what it sent before it read the change is taken. A lowering can leave a stream that has
 * already been given its room with less than nothing: it then waits until enough is consumed.
 * The connection's own window is not a setting and does not change. On a connection whose
 * windows grow while they are read (the event-driven drivers), a stream that has grown keeps
 * its growth above the new start.
 *
 * Changes are in flight one at a time, with proven_h2_conn_set_max_streams's.
 *
 * @return as proven_h2_conn_set_max_streams.
 */
[[nodiscard]]
proven_err_t proven_h2_conn_set_initial_window(proven_h2_conn_t *conn, proven_u32 n);

/** @brief Whether the connection is over: PROVEN_H2_EVENT_CLOSED was or is about to be
 *         reported. Pending output should still be sent. */
[[nodiscard]]
bool proven_h2_conn_is_closed(const proven_h2_conn_t *conn);

/** @brief Whether the peer's first SETTINGS frame has arrived. Until it has, what the peer
 *         allows is not known: a client that opens streams before then may open more than
 *         the server will take, and have the surplus refused. */
[[nodiscard]]
bool proven_h2_conn_has_peer_settings(const proven_h2_conn_t *conn);

/** @brief How many streams are open. */
[[nodiscard]]
proven_size_t proven_h2_conn_stream_count(const proven_h2_conn_t *conn);

/** @brief How many bytes of body may be sent on a stream now; 0 for one that is not open. */
[[nodiscard]]
proven_size_t proven_h2_conn_send_room(const proven_h2_conn_t *conn, proven_u32 stream_id);

/** @brief Attach a pointer of yours to an open stream, and get it back (null for a stream
 *         that is not open). */
void proven_h2_conn_stream_set_user(proven_h2_conn_t *conn, proven_u32 stream_id, void *user);
[[nodiscard]]
void *proven_h2_conn_stream_user(const proven_h2_conn_t *conn, proven_u32 stream_id);

#endif /* PROVEN_H2_H */
