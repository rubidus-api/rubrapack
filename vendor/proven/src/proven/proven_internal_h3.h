#ifndef PROVEN_INTERNAL_H3_H
#define PROVEN_INTERNAL_H3_H

/* HTTP/3 (RFC 9114) and QPACK (RFC 9204) over the QUIC connection of proven_internal_quic.h.
 * Internal: nothing here is public API, and tests reach it by path. Left out of a build
 * compiled with PROVEN_NO_HTTP3. */

#include "proven_internal_quic.h"
#include "proven/h2.h"
#include "proven/hpack.h"

#ifndef PROVEN_NO_HTTP3

// -----------------------------------------------------------------------------
// QPACK with a dynamic table of capacity 0
// -----------------------------------------------------------------------------

#define PROVEN_QPACK_ENTRY_OVERHEAD 32     /* added to each field's name and value when a list is measured */

#define PROVEN_QPACK_DECOMPRESSION_FAILED 0x200
#define PROVEN_QPACK_ENCODER_STREAM_ERROR 0x201
#define PROVEN_QPACK_DECODER_STREAM_ERROR 0x202

/* An encoded field section into `headers` (and `never_indexed`, optional: the N bit of
 * each), as proven_hpack_decode: names and values point into `section` or into `scratch`
 * (Huffman-coded strings). PROVEN_ERR_INVALID_FORMAT: QPACK_DECOMPRESSION_FAILED - a
 * reference to the dynamic table, a Required Insert Count other than 0, a negative Base, a
 * malformed line. PROVEN_ERR_OUT_OF_BOUNDS: the list is larger than `max_list_bytes` (0: no
 * limit), than `header_cap` fields or than the scratch - read to its end otherwise intact.
 * Nothing is kept between calls. */
proven_err_t proven_qpack_decode(proven_mem_view_t section, proven_http_header_t *headers, bool *never_indexed, proven_size_t header_cap,
                                 proven_mem_mut_t scratch, proven_size_t max_list_bytes, proven_size_t *count);
/* The most bytes proven_qpack_encode can write for a list. */
proven_size_t proven_qpack_encode_bound(const proven_http_header_t *headers, proven_size_t count);
/* A field section for `headers`, appended at out[*len]: the static table where it has the
 * field or its name, literals otherwise, Huffman-coded where shorter; `sensitive` (optional,
 * one flag a field) and `authorization` and `proxy-authorization` always with the N bit.
 * PROVEN_ERR_OUT_OF_BOUNDS when fewer than proven_qpack_encode_bound bytes are left. */
proven_err_t proven_qpack_encode(const proven_http_header_t *headers, proven_size_t count, const bool *sensitive,
                                 proven_mem_mut_t out, proven_size_t *len);

/* A decoder with a dynamic table (RFC-0032): the peer's encoder stream builds it, and field
 * sections refer to it. `max_capacity` is what this side said in SETTINGS
 * (QPACK_MAX_TABLE_CAPACITY), 32 to 1 MiB; the peer sets the table's capacity within it. */
typedef struct proven_qpack_dec proven_qpack_dec_t;
proven_err_t proven_qpack_dec_create(proven_allocator_t alloc, proven_u32 max_capacity, proven_qpack_dec_t **out);
void proven_qpack_dec_destroy(proven_qpack_dec_t *d);
/* The peer's encoder stream, fed as it arrives: 0, or PROVEN_QPACK_ENCODER_STREAM_ERROR (an
 * instruction that breaks RFC 9204 section 4.3, or one larger than any the table could take). */
proven_u64 proven_qpack_dec_feed(proven_qpack_dec_t *d, proven_mem_view_t in);
/* Entries inserted so far (the next one's absolute index). */
proven_u64 proven_qpack_dec_inserts(const proven_qpack_dec_t *d);
/* The Required Insert Count of a field section, from its prefix. PROVEN_ERR_INVALID_FORMAT when
 * the prefix is malformed or names a count no encoder could. */
proven_err_t proven_qpack_dec_required(const proven_qpack_dec_t *d, proven_mem_view_t section, proven_u64 *required);
/* proven_qpack_decode with the table (`d` null: capacity 0, as proven_qpack_decode).
 * `*required`: the section's Required Insert Count, also on PROVEN_ERR_OUT_OF_BOUNDS (the section
 * was read whole, so it is acknowledged). PROVEN_ERR_AGAIN: it needs inserts not yet made (a
 * blocked stream). Fields from the table are copied into `scratch`. */
proven_err_t proven_qpack_decode_dyn(proven_qpack_dec_t *d, proven_mem_view_t section, proven_http_header_t *headers, bool *never_indexed, proven_size_t header_cap,
                                     proven_mem_mut_t scratch, proven_size_t max_list_bytes, proven_size_t *count, proven_u64 *required);
/* An encoder with a dynamic table (RFC-0032, its encoder half): `capacity` is the table this side
 * uses - at most the peer's QPACK_MAX_TABLE_CAPACITY, `peer_max` - 32 to 1 MiB. Sections refer only
 * to entries the peer has acknowledged, so they never wait at the peer (its blocked streams stay
 * unused). Inserted: fields that are not marked sensitive - nor `authorization`, `cookie` or
 * `proxy-authorization` values under 20 bytes - whose entry is at most a quarter of the table. An
 * entry is evicted only once its insert is acknowledged and no unacknowledged section refers to it. */
typedef struct proven_qpack_enc proven_qpack_enc_t;
proven_err_t proven_qpack_enc_create(proven_allocator_t alloc, proven_u32 capacity, proven_u32 peer_max, proven_qpack_enc_t **out);
void proven_qpack_enc_destroy(proven_qpack_enc_t *e);
/* The most bytes proven_qpack_encode_dyn can append to `ins` for a list. */
proven_size_t proven_qpack_enc_ins_bound(const proven_http_header_t *headers, proven_size_t count);
/* A field section for `headers` appended at out[*len] (room as for proven_qpack_encode), and the
 * encoder-stream instructions it makes appended at ins[*ins_len] (room as
 * proven_qpack_enc_ins_bound). `*required` and `*lowest`: the section's Required Insert Count and
 * the lowest entry it refers to - give both to proven_qpack_enc_sent once the section is on its
 * stream, if `*required` is not 0. `e` null: proven_qpack_encode. */
proven_err_t proven_qpack_encode_dyn(proven_qpack_enc_t *e, const proven_http_header_t *headers, proven_size_t count, const bool *sensitive,
                                     proven_mem_mut_t out, proven_size_t *len, proven_mem_mut_t ins, proven_size_t *ins_len,
                                     proven_u64 *required, proven_u64 *lowest);
/* A section that refers to the table is on stream `stream_id`: kept until it is acknowledged or
 * the stream cancelled. False when memory ran out (the section must then not be sent). */
bool proven_qpack_enc_sent(proven_qpack_enc_t *e, proven_u64 stream_id, proven_u64 required, proven_u64 lowest);
/* The peer's decoder stream, fed as it arrives: 0, or PROVEN_QPACK_DECODER_STREAM_ERROR (an
 * acknowledgement of a section not outstanding, an increment of 0 or past what was inserted). */
proven_u64 proven_qpack_enc_feed(proven_qpack_enc_t *e, proven_mem_view_t in);
/* For tests: entries inserted, and how many the peer has acknowledged. */
proven_u64 proven_qpack_enc_inserts(const proven_qpack_enc_t *e);
proven_u64 proven_qpack_enc_known(const proven_qpack_enc_t *e);

/* This side's decoder-stream instructions (section 4.4), at most 10 bytes each. */
proven_size_t proven_qpack_put_section_ack(proven_byte_t *out, proven_u64 stream_id);
proven_size_t proven_qpack_put_stream_cancel(proven_byte_t *out, proven_u64 stream_id);
proven_size_t proven_qpack_put_insert_count(proven_byte_t *out, proven_u64 increment);

/* The peer's encoder or decoder stream, fed as it arrives: 0, or the code of the connection
 * error (an instruction a table of capacity 0 cannot take). */
typedef struct { proven_byte_t buf[10]; proven_size_t held; } proven_qpack_stream_t;
proven_u64 proven_qpack_encoder_stream_feed(proven_qpack_stream_t *s, proven_mem_view_t in);
proven_u64 proven_qpack_decoder_stream_feed(proven_qpack_stream_t *s, proven_mem_view_t in);

// -----------------------------------------------------------------------------
// HTTP/3
// -----------------------------------------------------------------------------

#define PROVEN_H3_NO_ERROR                0x100
#define PROVEN_H3_GENERAL_PROTOCOL_ERROR  0x101
#define PROVEN_H3_INTERNAL_ERROR          0x102
#define PROVEN_H3_STREAM_CREATION_ERROR   0x103
#define PROVEN_H3_CLOSED_CRITICAL_STREAM  0x104
#define PROVEN_H3_FRAME_UNEXPECTED        0x105
#define PROVEN_H3_FRAME_ERROR             0x106
#define PROVEN_H3_EXCESSIVE_LOAD          0x107
#define PROVEN_H3_ID_ERROR                0x108
#define PROVEN_H3_SETTINGS_ERROR          0x109
#define PROVEN_H3_MISSING_SETTINGS        0x10a
#define PROVEN_H3_REQUEST_REJECTED        0x10b
#define PROVEN_H3_REQUEST_CANCELLED       0x10c
#define PROVEN_H3_REQUEST_INCOMPLETE      0x10d
#define PROVEN_H3_MESSAGE_ERROR           0x10e
#define PROVEN_H3_CONNECT_ERROR           0x10f
#define PROVEN_H3_VERSION_FALLBACK        0x110

#define PROVEN_H3_FRAME_DATA         0x00
#define PROVEN_H3_FRAME_HEADERS      0x01
#define PROVEN_H3_FRAME_CANCEL_PUSH  0x03
#define PROVEN_H3_FRAME_SETTINGS     0x04
#define PROVEN_H3_FRAME_PUSH_PROMISE 0x05
#define PROVEN_H3_FRAME_GOAWAY       0x07
#define PROVEN_H3_FRAME_MAX_PUSH_ID  0x0d

#define PROVEN_H3_SETTING_QPACK_MAX_TABLE_CAPACITY 0x01
#define PROVEN_H3_SETTING_MAX_FIELD_SECTION_SIZE   0x06
#define PROVEN_H3_SETTING_QPACK_BLOCKED_STREAMS    0x07

#define PROVEN_H3_STREAM_CONTROL       0x00
#define PROVEN_H3_STREAM_PUSH          0x01
#define PROVEN_H3_STREAM_QPACK_ENCODER 0x02
#define PROVEN_H3_STREAM_QPACK_DECODER 0x03

/* One HTTP/3 connection over a QUIC connection it borrows: the QUIC connection's datagrams,
 * clock and connection IDs stay the caller's (or a driver's), and its events are taken through
 * proven_h3_conn_poll, which passes on those HTTP does not consume. The QUIC connection's TLS
 * configuration must name the protocol `h3`. For one thread at a time; every view an event
 * carries is good until the next call that takes the connection. */
typedef struct proven_h3_conn proven_h3_conn_t;

typedef struct {
    proven_allocator_t alloc;                      /* required */
    bool server;                                   /* as the QUIC connection */
    proven_u32 max_field_section_bytes;            /* the largest field section taken, as RFC 9114 measures it, and said in SETTINGS; at most 1 MiB. 0: 16 KiB */
    proven_u32 max_reset_debt;                     /* streams reset - by the peer, or for its fault - beyond those that finished properly, before H3_EXCESSIVE_LOAD. 0: 100 */
    proven_u32 qpack_capacity;                     /* the QPACK dynamic table this side lets the peer's encoder fill (RFC-0032), 32 to 1 MiB, said in SETTINGS; 0: none, as before */
    proven_u32 qpack_blocked_streams;              /* with a table: the streams whose field sections may wait for inserts at once. 0: 16 */
} proven_h3_config_t;

typedef enum {
    PROVEN_H3_EV_NONE = 0,
    PROVEN_H3_EV_HEADERS,                          /* a server: a request began. A client: a response head (an interim one when `status` is below 200) */
    PROVEN_H3_EV_DATA,                             /* a piece of a body, possibly empty when it is the last */
    PROVEN_H3_EV_TRAILERS,                         /* trailer fields: the peer's side of the stream is over */
    PROVEN_H3_EV_STREAM_RESET,                     /* the stream is over before its end: the peer reset it (`code`), broke a rule on it, or went away without acting on it (PROVEN_H3_REQUEST_REJECTED: safe to send again elsewhere) */
    PROVEN_H3_EV_STOP_SENDING,                     /* the peer wants no more of this stream's body (`code`; PROVEN_H3_NO_ERROR from a server whose response needs no more); what it sends goes on */
    PROVEN_H3_EV_STREAM_WRITABLE,                  /* a send that was cut short can go on */
    PROVEN_H3_EV_GOAWAY,                           /* the peer will take no new request: `id` (from a server, the first request stream not acted on) */
    PROVEN_H3_EV_QUIC,                             /* a QUIC event HTTP does not consume: `quic` (ESTABLISHED, CID_NEW, CID_RETIRED) */
    PROVEN_H3_EV_CLOSED                            /* the connection is over: `code`, `app` (an application's code), `remote` (the peer closed). Every stream with it */
} proven_h3_event_kind_t;

typedef struct {
    proven_h3_event_kind_t kind;
    proven_u64 stream_id;
    bool end_stream;                               /* HEADERS, DATA, TRAILERS: the peer will send nothing more on this stream */
    proven_h2_request_head_t request;              /* HEADERS on a server */
    proven_u16 status;                             /* HEADERS on a client */
    const proven_http_header_t *fields;            /* HEADERS on a client, TRAILERS: the ordinary fields */
    proven_size_t field_count;
    proven_mem_view_t data;                        /* DATA */
    proven_u64 code;                               /* STREAM_RESET, STOP_SENDING, CLOSED */
    bool too_large;                                /* STREAM_RESET: this side reset it for a field section over its limit */
    bool app, remote;                              /* CLOSED */
    proven_u64 id;                                 /* GOAWAY */
    proven_quic_event_t quic;                      /* QUIC */
} proven_h3_event_t;

/* Make the HTTP/3 side of `quic`, which it borrows: destroy this first. PROVEN_ERR_INVALID_ARG
 * for a setting out of range; PROVEN_ERR_NOMEM. */
proven_err_t proven_h3_conn_create(const proven_h3_config_t *config, proven_quic_conn_t *quic, proven_h3_conn_t **out);
void proven_h3_conn_destroy(proven_h3_conn_t *h);
/* The next event; false when there is none until the QUIC connection is given a datagram or
 * a timeout. Call it after each of those, until it says false. */
bool proven_h3_conn_poll(proven_h3_conn_t *h, proven_h3_event_t *ev);
/* For tests: this side's QPACK encoder - entries inserted and acknowledged - and decoder -
 * entries inserted by the peer (RFC-0032). */
void proven_h3_conn_test_qpack_(const proven_h3_conn_t *h, proven_u64 *enc_inserts, proven_u64 *enc_known, proven_u64 *dec_inserts);

/* A client: a request (pseudo-header fields first, names in lowercase). PROVEN_ERR_INVALID_ARG
 * for a list proven_h2_request_head refuses or `:protocol`, or on a server;
 * PROVEN_ERR_INVALID_STATE before the handshake, after a GOAWAY or the end; PROVEN_ERR_BUSY at
 * the peer's limit on streams; PROVEN_ERR_OUT_OF_BOUNDS for a list larger than the peer takes;
 * PROVEN_ERR_NOMEM. */
proven_err_t proven_h3_submit_request(proven_h3_conn_t *h, const proven_http_header_t *headers, proven_size_t count, bool end_stream, proven_u64 *stream_id);
/* A server: a response, `:status` first; interim ones (1xx but 101) may come before the final
 * one. A response that ends before its request's body asks the client to stop sending it
 * (STOP_SENDING with H3_NO_ERROR). PROVEN_ERR_NOT_FOUND for a stream that is not open;
 * PROVEN_ERR_INVALID_STATE when the final response was sent; otherwise as above. */
proven_err_t proven_h3_submit_response(proven_h3_conn_t *h, proven_u64 stream_id, const proven_http_header_t *headers, proven_size_t count, bool end_stream);
/* Body bytes: it may take fewer than offered (`*taken`), possibly none - offer the rest after
 * PROVEN_H3_EV_STREAM_WRITABLE. `end_stream` takes effect when all of `data` is taken.
 * PROVEN_ERR_INVALID_STATE before the head, after the end, or after the peer's STOP_SENDING. */
proven_err_t proven_h3_send_data(proven_h3_conn_t *h, proven_u64 stream_id, proven_mem_view_t data, bool end_stream, proven_size_t *taken);
/* End this side of a stream with trailer fields. */
proven_err_t proven_h3_send_trailers(proven_h3_conn_t *h, proven_u64 stream_id, const proven_http_header_t *headers, proven_size_t count);
/* Mark the fields of the next list given to one of the three calls above as never to be
 * indexed (one flag a field, read by that call). */
void proven_h3_next_sensitive(proven_h3_conn_t *h, const bool *sensitive);
/* Give up on a stream both ways with `code` (0: H3_REQUEST_CANCELLED); nothing more is
 * reported for it. PROVEN_ERR_NOT_FOUND for a stream that is not open. */
proven_err_t proven_h3_stream_reset(proven_h3_conn_t *h, proven_u64 stream_id, proven_u64 code);
/* Stop or resume reading a stream: while paused nothing of it is reported, and QUIC's flow
 * control stops the peer when the stream's window is full. */
void proven_h3_stream_pause(proven_h3_conn_t *h, proven_u64 stream_id, bool paused);
/* Say goodbye (GOAWAY): a server takes no request after those it has seen, a client sends
 * none; the connection closes with H3_NO_ERROR when the last stream ends. */
proven_err_t proven_h3_conn_goaway(proven_h3_conn_t *h);
/* The same in two steps, for a server (RFC 9114 section 5.2): `early` says the largest ID a
 * client may use, so that requests already on their way are not refused, and the connection
 * stays open; a round trip later the call without it says the real one. A client's goodbye
 * is always the last. */
proven_err_t proven_h3_conn_goaway_step(proven_h3_conn_t *h, bool early);
/* Bytes the HTTP/3 side holds (streams, buffers), for tests. */
proven_size_t proven_h3_conn_memory(const proven_h3_conn_t *h);

#endif /* PROVEN_NO_HTTP3 */
#endif
