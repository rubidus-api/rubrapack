#ifndef PROVEN_WS_H
#define PROVEN_WS_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/u8str.h"
#include "proven/http.h"

/**
 * @file ws.h
 * @brief The WebSocket protocol (RFC 6455) as a codec: the handshake's values and checks, frames,
 *        and a decoder that turns a byte stream into messages.
 *
 * Like http.h this reads nothing and writes nothing. You hand it bytes you received and it tells
 * you what they mean; you ask it for the bytes of a frame and send them yourself. No socket, no
 * allocation, no clock, no random source - the two places the protocol needs random bytes (the
 * client's key and its masks) take them as arguments. It is part of the freestanding profile.
 * The connection that drives it over a socket is ws_conn.h.
 *
 * The decoder enforces what RFC 6455 tells a receiver to enforce, and refuses rather than
 * repairs: reserved bits and unknown opcodes, a length not written in its shortest form, a
 * control frame that is long or fragmented, a continuation with nothing to continue, a client
 * frame without a mask or a server frame with one, text that is not UTF-8, a close code that
 * may not be sent.
 *
 * One extension is spoken: `permessage-deflate` (RFC 7692), negotiated with the functions of
 * the compression section below and allowed by proven_ws_decoder_allow_compressed. A peer
 * that sets a reserved bit without it is refused.
 */

// -----------------------------------------------------------------------------
// The opening handshake
// -----------------------------------------------------------------------------

/** @brief Characters in a `Sec-WebSocket-Key` value, without a NUL. */
#define PROVEN_WS_KEY_SIZE ((proven_size_t)24)
/** @brief Characters in a `Sec-WebSocket-Accept` value, without a NUL. */
#define PROVEN_WS_ACCEPT_SIZE ((proven_size_t)28)

/**
 * @brief Make a client's `Sec-WebSocket-Key` from 16 random bytes you supply.
 *
 * The key is not a secret and proves nothing about the client; it exists so that a server which
 * does not speak WebSocket cannot produce the right answer by accident. Draw the bytes from
 * proven_random_bytes or a seeded generator; they need only differ from connection to connection.
 */
void proven_ws_make_key(const proven_byte_t random[16], proven_byte_t out[PROVEN_WS_KEY_SIZE]);

/**
 * @brief The `Sec-WebSocket-Accept` value that answers `key`: Base64 of the SHA-1 of the key and
 *        the protocol's fixed GUID.
 * @return PROVEN_ERR_INVALID_FORMAT unless `key` is 24 characters of Base64 that decode to 16
 *         bytes; `out` is then untouched.
 */
[[nodiscard]]
proven_err_t proven_ws_accept_key(proven_u8str_view_t key, proven_byte_t out[PROVEN_WS_ACCEPT_SIZE]);

/**
 * @brief A server's check: is this parsed request a WebSocket upgrade, and a well-formed one?
 *
 * A request that came over HTTP/2 as an extended CONNECT (RFC 8441) is one too: the
 * event-driven server presents it with `method` CONNECT and a `:protocol` field first, and it
 * has no key - the answer there is 200, not 101 (proven_ws_event_accept gives it).
 *
 * @param key receives the client's `Sec-WebSocket-Key`, a view into the request; empty for an
 *        HTTP/2 request, which has none to answer.
 * @return PROVEN_OK: answer 101 with proven_ws_accept_key(key) - or, over HTTP/2, 200.
 *         PROVEN_ERR_NOT_FOUND: not an upgrade to WebSocket at all (no `Upgrade: websocket`) -
 *         an ordinary request; handle it as one.
 *         PROVEN_ERR_UNSUPPORTED: an upgrade for a protocol version other than 13 - answer 426
 *         with `Sec-WebSocket-Version: 13`.
 *         PROVEN_ERR_INVALID_FORMAT: an upgrade, but malformed - not `GET`, not HTTP/1.1, no
 *         `Connection: Upgrade`, or a key that is missing, repeated or not 16 bytes: answer 400.
 */
[[nodiscard]]
proven_err_t proven_ws_check_request(const proven_http_request_t *request, proven_u8str_view_t *key);

/**
 * @brief A server's check of where the page that opened the connection came from: is the
 *        request's `Origin` one of `allowed`?
 *
 * A browser sends `Origin` with every WebSocket handshake, and sends the user's cookies with
 * it whatever page asked; the same-origin policy does not cover WebSocket. A server that
 * knows its users by a cookie therefore has to refuse the origins it does not serve, or any
 * page can talk to it as the user. A program that is not a browser sends what it likes, or
 * nothing: this check is about browsers.
 *
 * An origin is written as a browser writes it - `https://app.example`, with the port only
 * when it is not the scheme's own (`http://localhost:8080`), no path and no final slash. It is
 * compared whole, letter case aside; `https://app.example:443` is not `https://app.example`.
 *
 * @param allowed the origins served, `allowed_count` of them.
 * @return PROVEN_OK: the request has one `Origin` and it is in the list.
 *         PROVEN_ERR_NOT_FOUND: it has none - not a browser. Whether that is let in is the
 *         server's decision.
 *         PROVEN_ERR_PERMISSION: an origin that is not in the list (`null` among them), or
 *         more than one `Origin` field - answer 403.
 *         PROVEN_ERR_INVALID_ARG: no request, or a count with no list.
 */
[[nodiscard]]
proven_err_t proven_ws_check_origin(const proven_http_request_t *request, const proven_u8str_view_t *allowed, proven_size_t allowed_count);

/**
 * @brief Whether the request's `Sec-WebSocket-Protocol` offers `name` (compared exactly: the
 *        names are case-sensitive). A server picks at most one of those offered, or none.
 */
[[nodiscard]]
bool proven_ws_request_offers(const proven_http_request_t *request, proven_u8str_view_t name);

/**
 * @brief A client's check: did the server accept the upgrade this client asked for?
 *
 * @param key the `Sec-WebSocket-Key` that was sent.
 * @param offered the `Sec-WebSocket-Protocol` value that was sent (`chat, superchat`), or empty.
 * @param protocol receives the subprotocol the server chose, a view into the response; empty
 *        when it chose none.
 * @return PROVEN_ERR_INVALID_FORMAT unless the status is 101, `Upgrade` and `Connection` say
 *         so, `Sec-WebSocket-Accept` is the answer to `key`, the subprotocol (if any) is one of
 *         those offered, and no extension is named (for a client that offered compression,
 *         see proven_ws_check_response_deflate) - this client offers none, so a server that
 *         names one is not speaking the protocol that was agreed.
 */
[[nodiscard]]
proven_err_t proven_ws_check_response(const proven_http_response_t *response, proven_u8str_view_t key,
                                      proven_u8str_view_t offered, proven_u8str_view_t *protocol);

// -----------------------------------------------------------------------------
// Compression: the permessage-deflate extension (RFC 7692)
// -----------------------------------------------------------------------------

/**
 * @brief What a client sends in `Sec-WebSocket-Extensions` to ask for compressed messages.
 *
 * Both sides are asked to compress every message on its own, keeping nothing from one message
 * to the next ("no context takeover"): a connection then holds no compression state between
 * messages, and the size of one message says nothing about another. That is the only form of
 * the extension this library speaks.
 */
#define PROVEN_WS_DEFLATE_OFFER "permessage-deflate; client_no_context_takeover; server_no_context_takeover"

/** @brief Room that is always enough for the answer proven_ws_deflate_accept writes. */
#define PROVEN_WS_DEFLATE_ANSWER_MAX ((proven_size_t)112)

/**
 * @brief A server's choice: can one of the request's `permessage-deflate` offers be met?
 *
 * The offers are tried in the order the client wrote them and the first that can be met is
 * taken. An offer is passed over when it has a parameter this library does not know, the same
 * parameter twice, a value that is not allowed, or `server_max_window_bits=8` (the smallest
 * window this compressor has is 9 bits). `client_max_window_bits` is accepted and ignored.
 *
 * @param out receives the value to send back in `Sec-WebSocket-Extensions`, appended at
 *        `out.ptr[*len]`: always with `server_no_context_takeover` and
 *        `client_no_context_takeover`, and with `server_max_window_bits` when the offer had it.
 * @param window_bits receives the largest window this side may compress with, 9 to 15.
 * @return PROVEN_ERR_NOT_FOUND when the request offers no `permessage-deflate` that can be
 *         met - go on without compression; PROVEN_ERR_OUT_OF_BOUNDS when `out` is too small
 *         (PROVEN_WS_DEFLATE_ANSWER_MAX is enough), with `*len` unchanged.
 */
[[nodiscard]]
proven_err_t proven_ws_deflate_accept(const proven_http_request_t *request, proven_mem_mut_t out, proven_size_t *len, int *window_bits);

/**
 * @brief proven_ws_check_response for a client that sent PROVEN_WS_DEFLATE_OFFER.
 *
 * As that function, except that the answer may name this one extension. `*deflate` is true
 * when the server accepted it: messages may then be compressed in both directions.
 *
 * @return also PROVEN_ERR_INVALID_FORMAT when the server names another extension, names this
 *         one twice, accepts it without `server_no_context_takeover` (it would then compress
 *         in a way this side does not decode), or adds a parameter that was not offered.
 */
[[nodiscard]]
proven_err_t proven_ws_check_response_deflate(const proven_http_response_t *response, proven_u8str_view_t key,
                                              proven_u8str_view_t offered, proven_u8str_view_t *protocol, bool *deflate);

/**
 * @brief What a client sends in `Sec-WebSocket-Extensions` to ask for compressed messages with
 *        **context takeover**: each side may keep its window from one message to the next.
 *
 * A message then refers back to the messages before it, so short, similar messages shrink to
 * a third or a quarter where on their own they would not shrink at all - at the price of a
 * compressor and a decompressor kept for the connection's life. `client_max_window_bits` lets
 * the server bound the window this client compresses with. Check the answer with
 * proven_ws_check_response_takeover.
 */
#define PROVEN_WS_DEFLATE_OFFER_TAKEOVER "permessage-deflate; client_max_window_bits"

/** @brief What the two sides agreed for `permessage-deflate`. */
typedef struct {
    bool server_takeover;     /**< the server may keep its window between messages: the client keeps one decompressor for all of them */
    bool client_takeover;     /**< the client may: the server keeps one decompressor */
    int server_window_bits;   /**< the largest window the server may compress with, 8 to 15 (this compressor's smallest is 9) */
    int client_window_bits;   /**< the largest the client may compress with, 8 to 15. 8: this library's compressor cannot, so such a client sends its messages uncompressed */
} proven_ws_deflate_terms_t;

/**
 * @brief proven_ws_deflate_accept for a server that keeps its window between messages.
 *
 * The offers are tried and passed over as there. The answer forbids takeover only where the
 * offer asked for it: `server_no_context_takeover` and `client_no_context_takeover` are
 * answered when the offer had them, `server_max_window_bits` when the offer had it.
 * `client_max_window_bits` is never answered: a decompressor here holds 32 KiB whatever the
 * window, so there is nothing to save.
 *
 * @param terms receives what was agreed: takeover in each direction not forbidden, the
 *        server's window the offer's or 15, the client's 15.
 * @return as proven_ws_deflate_accept.
 */
[[nodiscard]]
proven_err_t proven_ws_deflate_accept_takeover(const proven_http_request_t *request, proven_mem_mut_t out, proven_size_t *len,
                                               proven_ws_deflate_terms_t *terms);

/**
 * @brief proven_ws_check_response for a client that sent PROVEN_WS_DEFLATE_OFFER_TAKEOVER.
 *
 * The answer may name this one extension, once, with any of its four parameters at most once
 * each; `client_max_window_bits` must then have a value. `*deflate` is true when the server
 * accepted it, and `*terms` says what was agreed (zeroed when it did not).
 *
 * A side whose peer may take over keeps one decompressor and, when it reports `done` (the
 * sender ended a final block), goes on with proven_inflate_restart, not reset. A side that
 * takes over keeps one compressor and ends each message with PROVEN_DEFLATE_FLUSH_SYNC.
 *
 * @return also PROVEN_ERR_INVALID_FORMAT when the server names another extension, names this
 *         one twice, or gives a parameter twice, an unknown one or a value out of range.
 */
[[nodiscard]]
proven_err_t proven_ws_check_response_takeover(const proven_http_response_t *response, proven_u8str_view_t key,
                                               proven_u8str_view_t offered, proven_u8str_view_t *protocol, bool *deflate,
                                               proven_ws_deflate_terms_t *terms);

/**
 * @brief What a client sends to keep its own window between messages and ask the server to
 *        keep none: the server then compresses each message on its own, and this client
 *        needs no decompressor between messages (about 35 KiB less per idle connection), at
 *        the price of the server's short, similar messages shrinking far less. A server that
 *        will not honour it declines the extension (RFC 7692 section 7.1.1.1). Check the answer
 *        with proven_ws_check_response_peer_no_takeover.
 */
#define PROVEN_WS_DEFLATE_OFFER_PEER_NO_TAKEOVER "permessage-deflate; client_max_window_bits; server_no_context_takeover"

/**
 * @brief proven_ws_deflate_accept_takeover for a server that keeps its own window and forbids
 *        the client's: the answer always carries `client_no_context_takeover` (RFC 7692
 *        section 7.1.1.2 lets a server add it), so the client compresses each message on its
 *        own and this side needs no decompressor between messages.
 *
 * `server_no_context_takeover` and `server_max_window_bits` are answered as there.
 *
 * @param terms receives what was agreed; `client_takeover` is always false.
 * @return as proven_ws_deflate_accept.
 */
[[nodiscard]]
proven_err_t proven_ws_deflate_accept_peer_no_takeover(const proven_http_request_t *request, proven_mem_mut_t out, proven_size_t *len,
                                                       proven_ws_deflate_terms_t *terms);

/**
 * @brief proven_ws_check_response_takeover for a client that sent
 *        PROVEN_WS_DEFLATE_OFFER_PEER_NO_TAKEOVER.
 *
 * @return also PROVEN_ERR_INVALID_FORMAT when the server accepts the extension without
 *         `server_no_context_takeover`: it would keep a window this side does not keep.
 */
[[nodiscard]]
proven_err_t proven_ws_check_response_peer_no_takeover(const proven_http_response_t *response, proven_u8str_view_t key,
                                                       proven_u8str_view_t offered, proven_u8str_view_t *protocol, bool *deflate,
                                                       proven_ws_deflate_terms_t *terms);

// -----------------------------------------------------------------------------
// Frames
// -----------------------------------------------------------------------------

typedef enum {
    PROVEN_WS_CONTINUATION = 0x0,
    PROVEN_WS_TEXT = 0x1,
    PROVEN_WS_BINARY = 0x2,
    PROVEN_WS_CLOSE = 0x8,
    PROVEN_WS_PING = 0x9,
    PROVEN_WS_PONG = 0xA
} proven_ws_opcode_t;

/** @brief Largest frame header: 2 bytes, 8 of extended length, 4 of mask. */
#define PROVEN_WS_MAX_FRAME_HEADER ((proven_size_t)14)
/** @brief Largest payload of a control frame (close, ping, pong). */
#define PROVEN_WS_MAX_CONTROL ((proven_size_t)125)

/** @brief A frame header: everything about a frame but its payload. */
typedef struct {
    bool fin;                   /**< the last frame of its message */
    proven_u8 opcode;           /**< a proven_ws_opcode_t */
    bool masked;                /**< the payload is XORed with `mask`: always from a client, never from a server */
    proven_byte_t mask[4];
    proven_u64 length;          /**< bytes of payload that follow the header */
    bool rsv1;                  /**< the first reserved bit: with `permessage-deflate`, "this message is compressed". Only on the first frame of a text or binary message */
} proven_ws_frame_t;

/**
 * @brief Parse a frame header from the front of `data`.
 *
 * `*header_size` is how many bytes the header took; `out->length` bytes of payload follow it.
 * This checks the header alone. Whether the frame is allowed here - masked or not, a
 * continuation with something to continue - depends on the connection, and is the decoder's
 * business.
 *
 * @return PROVEN_ERR_NEED_MORE when `data` is the beginning of a header (at most 13 more bytes
 *         are needed); PROVEN_ERR_INVALID_FORMAT for the second or third reserved bit, the
 *         first on a control frame or a continuation, an opcode RFC 6455 does not define, a
 *         length not in its shortest form or with its top bit set, or a control frame that is
 *         fragmented or longer than 125 bytes. The first reserved bit on the first frame of a
 *         message is reported in `rsv1`; whether it is allowed is the decoder's business.
 */
[[nodiscard]]
proven_err_t proven_ws_frame_parse(proven_mem_view_t data, proven_ws_frame_t *out, proven_size_t *header_size);

/**
 * @brief Append a frame header at `out.ptr[*len]`. The payload is yours to write after it -
 *        masked with proven_ws_mask first, if `frame->masked`.
 * @return PROVEN_ERR_INVALID_ARG for a frame the parser would refuse; PROVEN_ERR_OUT_OF_BOUNDS
 *         when `out` is too small, with `*len` unchanged.
 */
[[nodiscard]]
proven_err_t proven_ws_frame_write(proven_mem_mut_t out, proven_size_t *len, const proven_ws_frame_t *frame);

/**
 * @brief Mask or unmask payload bytes in place (the operation is its own inverse).
 * @param offset where `data` begins within the frame's payload, so that a payload handled in
 *        pieces stays in step with the four-byte key.
 *
 * Masking is not encryption and hides nothing from anyone. It exists so that a script in a
 * browser cannot choose the exact bytes that cross the network, which was once enough to
 * poison a caching proxy. A client must mask every frame with a key the application cannot
 * predict; a server must not mask.
 */
void proven_ws_mask(proven_mem_mut_t data, const proven_byte_t mask[4], proven_u64 offset);

// -----------------------------------------------------------------------------
// Closing
// -----------------------------------------------------------------------------

/** @brief Close codes of RFC 6455 section 7.4.1 that a program meets. */
enum {
    PROVEN_WS_CLOSE_NORMAL = 1000,
    PROVEN_WS_CLOSE_GOING_AWAY = 1001,
    PROVEN_WS_CLOSE_PROTOCOL_ERROR = 1002,
    PROVEN_WS_CLOSE_UNSUPPORTED_DATA = 1003,
    PROVEN_WS_CLOSE_NO_STATUS = 1005,       /**< never sent: what a close frame with no code means */
    PROVEN_WS_CLOSE_ABNORMAL = 1006,        /**< never sent: the connection ended without a close frame */
    PROVEN_WS_CLOSE_INVALID_DATA = 1007,
    PROVEN_WS_CLOSE_POLICY = 1008,
    PROVEN_WS_CLOSE_TOO_BIG = 1009,
    PROVEN_WS_CLOSE_INTERNAL_ERROR = 1011
};

/** @brief Whether `code` may appear in a close frame: 1000-1003, 1007-1014, and 3000-4999. */
[[nodiscard]]
bool proven_ws_close_code_is_valid(proven_u16 code);

/**
 * @brief Append the payload of a close frame: the code, then a reason of at most 123 bytes.
 *
 * PROVEN_WS_CLOSE_NO_STATUS with an empty reason appends nothing - a close frame with no
 * payload.
 *
 * @return PROVEN_ERR_INVALID_ARG for a code that may not be sent, a reason longer than 123
 *         bytes, or a reason with no code; PROVEN_ERR_INVALID_ENCODING for a reason that is not
 *         UTF-8; PROVEN_ERR_OUT_OF_BOUNDS.
 */
[[nodiscard]]
proven_err_t proven_ws_close_write(proven_mem_mut_t out, proven_size_t *len, proven_u16 code, proven_u8str_view_t reason);

/**
 * @brief Read the payload of a close frame.
 *
 * An empty payload is PROVEN_OK with `*code` PROVEN_WS_CLOSE_NO_STATUS.
 *
 * @return PROVEN_ERR_INVALID_FORMAT for a one-byte payload or a code that may not be sent;
 *         PROVEN_ERR_INVALID_ENCODING for a reason that is not UTF-8.
 */
[[nodiscard]]
proven_err_t proven_ws_close_parse(proven_mem_view_t payload, proven_u16 *code, proven_u8str_view_t *reason);

/**
 * @brief The close code that tells the peer why a decoder error ended the connection:
 *        1002 for PROVEN_ERR_INVALID_FORMAT, 1007 for PROVEN_ERR_INVALID_ENCODING, 1009 for
 *        PROVEN_ERR_OUT_OF_BOUNDS, 1011 for anything else.
 */
[[nodiscard]]
proven_u16 proven_ws_close_code_for(proven_err_t err);

// -----------------------------------------------------------------------------
// The decoder: bytes in, messages out
// -----------------------------------------------------------------------------

typedef enum {
    PROVEN_WS_EVENT_NONE = 0,   /**< everything given was consumed; read more */
    PROVEN_WS_EVENT_DATA,       /**< a piece of a text or binary message */
    PROVEN_WS_EVENT_PING,       /**< answer with a pong carrying the same payload */
    PROVEN_WS_EVENT_PONG,
    PROVEN_WS_EVENT_CLOSE       /**< the peer is closing; answer with a close and stop */
} proven_ws_event_kind_t;

/** @brief What proven_ws_decoder_feed found. */
typedef struct {
    proven_ws_event_kind_t kind;
    bool text;                  /**< DATA: the message is text (UTF-8, checked) rather than binary */
    bool first;                 /**< DATA: this is the first piece of its message */
    bool last;                  /**< DATA: the message is complete with this piece */
    bool compressed;            /**< DATA: the message is compressed (`permessage-deflate`): `data` is compressed bytes, to be inflated - and, for text, checked - by you. Never set unless proven_ws_decoder_allow_compressed was called */
    proven_mem_view_t data;     /**< DATA: the piece, unmasked, inside the buffer you fed. PING, PONG: the payload. CLOSE: the reason. */
    proven_u16 close_code;      /**< CLOSE: the code; PROVEN_WS_CLOSE_NO_STATUS when the frame had none */
} proven_ws_event_t;

/**
 * @brief Decoder state for one direction of one connection. Caller-owned; it allocates nothing
 *        and there is nothing to destroy. Do not copy it mid-stream.
 */
typedef struct {
    proven_u64 max_message;
    proven_u64 message_size;        /* payload bytes of the message so far */
    proven_u64 remaining;           /* payload bytes of the current frame still to come */
    proven_u64 offset;              /* position in the current frame's payload, for the mask */
    proven_ws_frame_t frame;
    proven_byte_t header[PROVEN_WS_MAX_FRAME_HEADER];
    proven_byte_t control[PROVEN_WS_MAX_CONTROL];
    proven_byte_t utf8[4];          /* the start of a character cut by a piece boundary */
    proven_u8 header_len;
    proven_u8 control_len;
    proven_u8 utf8_len;
    proven_err_t failed;
    bool expect_masked;
    bool in_frame;
    bool in_message;                /* a fragmented message is open */
    bool message_text;
    bool message_started;           /* a DATA event of this message was delivered */
    bool closed;
    bool allow_compressed;          /* permessage-deflate was negotiated */
    bool message_compressed;        /* the open message began with RSV1 */
} proven_ws_decoder_t;

/**
 * @brief Begin decoding.
 * @param from_client true for a server decoding what a client sends: every frame must be masked.
 *        false for a client decoding what a server sends: no frame may be.
 * @param max_message_bytes the largest message to accept, fragments together; 0 means no limit.
 */
void proven_ws_decoder_init(proven_ws_decoder_t *decoder, bool from_client, proven_u64 max_message_bytes);

/**
 * @brief Tell the decoder that `permessage-deflate` was negotiated: a message may then begin
 *        with the first reserved bit set. Call it after proven_ws_decoder_init.
 *
 * The decoder does not inflate - it allocates nothing. The pieces of such a message are
 * reported with `compressed` set and are the compressed bytes: text is **not** checked to be
 * UTF-8 (it is not text yet), and `max_message_bytes` counts compressed bytes. Inflating them
 * with a raw-DEFLATE decompressor (deflate.h; append `00 00 ff ff` after the last piece),
 * bounding what comes out and checking text is the caller's part. ws_conn.h and ws_event.h
 * do all of it.
 *
 * Without this call a frame with that bit is PROVEN_ERR_INVALID_FORMAT, as it always was.
 */
void proven_ws_decoder_allow_compressed(proven_ws_decoder_t *decoder);

/**
 * @brief Give the decoder more of what was received.
 *
 * Consumes bytes of `in` - `*consumed` says how many - until there is something to report or
 * `in` is used up. Call it again with the rest; when it reports PROVEN_WS_EVENT_NONE, read more.
 *
 * `in` is writable because a masked payload is unmasked where it lies: a DATA event's `data`
 * points into `in`, and is good until you reuse that memory. A message of any size therefore
 * passes through without being copied or held; you see it in the pieces it arrived in, with
 * `first` and `last` marking its ends. Control frames may arrive between the pieces of a
 * message and are reported where they arrive; their payload is held in the decoder (it is at
 * most 125 bytes) and is good until the next call.
 *
 * @return PROVEN_ERR_INVALID_FORMAT for a violation of the protocol (close with 1002);
 *         PROVEN_ERR_INVALID_ENCODING for text that is not UTF-8 or a close reason that is not
 *         (1007); PROVEN_ERR_OUT_OF_BOUNDS for a message past the limit (1009);
 *         PROVEN_ERR_EOF for bytes after a close frame. After the first three the decoder
 *         repeats the same error: where the next frame starts is no longer known.
 */
[[nodiscard]]
proven_err_t proven_ws_decoder_feed(proven_ws_decoder_t *decoder, proven_mem_mut_t in, proven_size_t *consumed,
                                    proven_ws_event_t *event);

#endif /* PROVEN_WS_H */
