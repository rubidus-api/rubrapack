#ifndef PROVEN_INTERNAL_HTTP_EVENT_H
#define PROVEN_INTERNAL_HTTP_EVENT_H

#include "proven/http_event.h"
#include "proven/deflate.h"
#include "proven_internal_http_coding.h"
#include "proven_internal_loop.h"

/*
 * Internal: a connection of the event-driven server taken over by another protocol.
 *
 * After an upgrade the connection record stays what it was - socket, TLS engine, output
 * buffer, flushing, one timer - and stops being HTTP: bytes that arrive go to the functions
 * below instead of the request parser. ws_event.c is built on this. Not public API.
 *
 * The protocol's own object must begin with a pointer to its operations, and is what every
 * operation receives. All of this runs on the loop's thread.
 */
typedef struct {
    /* Bytes arrived, in memory that may be written (a payload is unmasked where it lies).
     * Returns how many were used; fewer than given means "not now" - paused, or ended. */
    proven_size_t (*on_input)(void *raw, proven_mem_mut_t in);
    /* Output moved towards the client: there may be room for more. */
    void (*on_progress)(void *raw);
    /* The timer set with proven_http_event_raw_timer_ fired. */
    void (*on_timer)(void *raw);
    /* The connection is gone, and was not detached first. Called once. */
    void (*on_closed)(void *raw, proven_err_t why);
    /* Optional. The peer closed its sending side. True: go on writing what is owed, and let
     * go of the connection when that is done. Absent or false: the connection ends now. */
    bool (*on_eof)(void *raw);
    /* Optional. The server stopped listening: finish what is open and take nothing new. */
    void (*on_shutdown)(void *raw);
    /* Optional. The server is ending (proven_http_event_server_shutdown), after on_shutdown:
     * end what is open as soon as the protocol allows - a WebSocket says goodbye. */
    void (*on_drain)(void *raw);
} proven_http_event_raw_ops_t;

/* Inside on_request, with no response begun: send `head` (a complete response head), end the
 * HTTP exchange with on_done(PROVEN_OK), and hand the connection to `raw`.
 * PROVEN_ERR_INVALID_STATE when the stream cannot be taken over now; PROVEN_ERR_RESET when the
 * connection died while the head was being sent - `raw` was then not attached. */
[[nodiscard]]
proven_err_t proven_http_event_raw_begin_(proven_http_stream_t *stream, void *raw, proven_mem_view_t head);

/* Queue `a` then `b` for the client and send what goes at once. False when the connection is
 * gone (on_closed has then been called, unless detached). */
bool proven_http_event_raw_send_(proven_http_stream_t *stream, proven_mem_view_t a, proven_mem_view_t b);

/* Fire on_timer in `ms` milliseconds; 0 cancels. */
void proven_http_event_raw_timer_(proven_http_stream_t *stream, proven_u32 ms);

/* Stop or resume reading the connection. Resuming may deliver input inside the call. */
void proven_http_event_raw_pause_(proven_http_stream_t *stream, bool paused);
/* 0-RTT over TCP (RFC-0027): of the input not yet consumed, how many bytes at its start arrived as
 * early data. */
proven_size_t proven_http_event_early_ahead_(const proven_http_stream_t *stream);

/* Let go of the connection: no operation is called after this. Then either close politely -
 * flush what is queued, say that nothing more will come, wait a moment for the client to
 * close - or at once. After either the stream must not be used. */
void proven_http_event_raw_close_(proven_http_stream_t *stream);
void proven_http_event_raw_kill_(proven_http_stream_t *stream);

proven_allocator_t proven_http_event_raw_allocator_(const proven_http_stream_t *stream);

/* Write `data` straight to the transport - TLS, or the socket - without holding any of it,
 * and say how many bytes went. The owner keeps the rest and offers it again from on_progress,
 * which is called when the transport can take more: so an owner with a queue of its own has
 * one queue, not two. 0 when the connection is gone. */
[[nodiscard]]
proven_size_t proven_http_event_raw_offer_(proven_http_stream_t *stream, proven_mem_view_t data);

/* Ask for on_progress from the loop as soon as the socket is writable - which, with nothing
 * held, is at once. For an owner that has something to do that it may not do from inside the
 * call it is in. The next offer, of anything or of nothing, withdraws the request. */
void proven_http_event_raw_again_(proven_http_stream_t *stream);

/* What the transport itself still holds for the client (TLS records not yet written). */
[[nodiscard]]
proven_size_t proven_http_event_raw_held_(const proven_http_stream_t *stream);

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

// -----------------------------------------------------------------------------
// The server's own types, for the files that are part of it
// -----------------------------------------------------------------------------

#define EV_MAX_LISTENERS 4
#define EV_LINGER_MS 1000u
#define EV_COMPACT_MS 1000u        /* a TLS connection quiet this long gives its record keys' memory back (RFC-0025) */
#define EV_READ_BYTES ((proven_size_t)64 * 1024)
#define EV_PLAIN_BYTES ((proven_size_t)16 * 1024 + 512)

#define EV_HEAD 0          /* waiting for, or in, a request head */
#define EV_BODY 1          /* reading a request body */
#define EV_RESPOND 2       /* the request is complete; its response is not */
#define EV_LINGER 3        /* closing: input is thrown away until the client goes */
#define EV_RAW 4           /* taken over by another protocol (proven_internal_http_event.h) */

typedef struct { proven_byte_t *ptr; proven_size_t len, cap, off; bool borrowed; } proven_ev_buf_t;

/* What every object a handler may hold as a `proven_http_stream_t *` begins with: this
 * connection-and-exchange of HTTP/1.1, and one stream of an HTTP/2 connection, which is another
 * struct altogether. `kind` says which, and every public function reads it before anything
 * else. The fields are ones the HTTP/1.1 struct had already: moved to the front, they cost it
 * nothing. */
typedef struct {
    proven_http_event_server_t *server;
    proven_u32 depth;                  /* entries into this object now on the stack */
    proven_u8 state;
    proven_u8 version_minor;
    proven_u8 kind;                    /* EV_KIND_* */
} proven_ev_head_t;

#define EV_KIND_H1 0       /* a connection of the server's, speaking HTTP/1.1 or taken over */
#define EV_KIND_H2 1       /* one stream of an HTTP/2 connection */
#define EV_KIND_H3 2       /* one stream of an HTTP/3 connection */

struct proven_http_stream {
    proven_ev_head_t h;
    struct proven_http_stream *prev, *next;
    proven_net_conn_t sock;
    proven_net_addr_t peer;
    proven_loop_sock_t lsock;                 /* the connection as the loop knows it (RFC-0025) */
    proven_loop_timer_t timer;
    proven_tls_conn_t *tls;
    proven_ev_buf_t stash;                    /* input not consumed yet */
    proven_ev_buf_t out;                      /* with TLS: plaintext the engine has not taken yet (the loop holds the rest) */
    proven_http_body_t body;
    proven_u64 response_left;          /* body bytes still promised; LENGTH_UNKNOWN when chunked or not counted */
    void *user;
    /* One pointer for two things that never exist together: a connection taken over by
     * another protocol has no HTTP response, and only a response has a compressor. Which it
     * is, `state` says - so an idle connection is no larger for either. */
    union {
        void *raw;                     /* EV_RAW: the protocol's object; it begins with its operations */
        proven_http_compressor_t_ *zip; /* any other state: the response body goes through this, or null */
    };
    proven_http_method_t method;
    proven_u16 local_port;             /* the port the connection came in on: the one `Alt-Svc` names */
    proven_u8 compress_coding;         /* what a response asked to be compressed would go in: identity, gzip or zstd */
    /* One bit each (the loop's socket grew by a word on Linux for io_uring, RFC-0029; these give
     * it back). Only the loop's thread touches them. */
    bool active : 1;                   /* on_request was delivered and on_done has not been */
    bool response_begun : 1, response_ended : 1, chunked : 1, bodiless : 1, keep : 1, expect_continue : 1;
    bool paused : 1, dead : 1, processing : 1, peer_eof : 1, want_writable : 1, close_when_flushed : 1, head_started : 1;
    bool compress_asked : 1;
    bool sniffed : 1;                  /* which protocol this connection speaks has been decided */
    bool raw_more : 1;                 /* EV_RAW: the owner has output the transport would not take yet */
    bool told_stop : 1;                /* EV_RAW: the owner has been told that the server stopped listening */
    bool told_drain : 1;               /* EV_RAW: and that it is shutting down (on_drain) */
    bool attached : 1;                 /* given to the loop: freed only after its `closed` */
    bool recv_armed : 1;               /* a receive of zero bytes is in flight */
    bool sock_gone : 1;                /* `closed` has come */
    bool compact_due : 1;              /* the timer is set for compacting the TLS keys, not for the idle timeout */
    /* 0-RTT over TCP (RFC-0027): the request being served arrived as early data (where its head
     * ended among the plaintext the TLS engine counted says so: proven_tls_read_counts_). */
    bool early_req : 1;
    bool deferred : 1;                 /* input left for after another connection's on_request (proven_http_event_request_end_) */
};

/* One listener, allocated on its own: after stop_listening its accept may still be answered
 * (PROVEN_ERR_CANCELLED), and then it frees itself. */
typedef struct {
    proven_http_event_server_t *server;
    proven_net_listener_t listener;
    proven_loop_accept_t acc;
    bool armed;                        /* an accept is in flight */
    bool cancelling;                   /* it was cancelled; its answer has not come */
    bool orphan;                       /* stop_listening let go of it: it frees itself when answered */
} proven_ev_listener_t;

struct proven_http_event_server {
    proven_loop_t *loop;
    proven_allocator_t alloc;
    proven_http_event_server_config_t cfg;
    proven_ev_listener_t *listeners[EV_MAX_LISTENERS];
    proven_size_t listener_count;
    bool accepting;                    /* the listeners are being watched */
    bool closed_to_new;                /* stop_listening was called: nothing is adopted either */
    bool stopped;
    proven_http_stream_t *conns;
    proven_size_t conn_count;
    proven_size_t ending;              /* connections and listeners ended, waiting for the loop's last answer */
    bool destroyed;                    /* destroy was called: the struct goes when `ending` is 0 */
    proven_http_header_t *headers;
    /* While an on_request runs - HTTP/1.1, 2 or 3 - its head's fields are in `headers` and, for
     * HTTP/2, in `h2buf`; a handler that answers another connection can make that one read its
     * next request head into the same memory. So no connection reads a head meanwhile: it is
     * marked `deferred` and read when the outermost on_request returns. */
    proven_u32 delivering;
    bool deferred_any;
    proven_byte_t *rbuf, *pbuf, *hbuf;
    proven_byte_t *zbuf;               /* what a compressor gives out, on its way to a connection's output; made when first needed */
    proven_byte_t *h2buf;              /* where every HTTP/2 connection reads its header lists: one event at a time, one thread; made when first needed */
    proven_size_t h2buf_size;
    struct ev_h3 *h3;                  /* HTTP/3 (http_event_h3.c): its sockets and connections; null without `http3` */
    proven_u32 streams_now;            /* max_concurrent_streams as it is now (proven_http_event_server_set_max_streams); the configured number is the ceiling */
    proven_u32 window_now;             /* the ceiling on request-body credit (proven_http_event_server_set_window); 0: none was set */
    proven_size_t h3_count;            /* QUIC connections open: they count against `max_connections` with `conn_count` */
    /* proven_http_event_server_shutdown */
    bool draining;                     /* it was called: final */
    bool drain_cut;                    /* the deadline ended something */
    bool drain_told;                   /* on_drained was called */
    proven_loop_timer_t drain_deadline, drain_done;
    proven_http_event_drained_fn on_drained;
    void *drained_ctx;
};

// -----------------------------------------------------------------------------
// HTTP/2 (http_event_h2.c): an owner of a connection, and its streams
// -----------------------------------------------------------------------------

/* Hand a connection that has parsed nothing yet to HTTP/2. Input that is already there is
 * delivered by the loop that called. PROVEN_ERR_NOMEM leaves the connection as it was. */
[[nodiscard]]
proven_err_t proven_http_event_h2_take_(proven_http_stream_t *conn);
/* Whether a taken-over connection's owner is the HTTP/2 driver. */
bool proven_http_event_h2_owns_(const void *raw);
/* Around every on_request of the server's, whatever the protocol: see `delivering`. */
void proven_http_event_request_begin_(proven_http_event_server_t *server);
void proven_http_event_request_end_(proven_http_event_server_t *server);

/* What the public proven_http_stream_* functions do for a stream whose head says EV_KIND_H2.
 * `stream` is such a stream, in the public type's clothes. */
void proven_http_event_h2_compress_(proven_http_stream_t *stream);
[[nodiscard]]
proven_err_t proven_http_event_h2_begin_(proven_http_stream_t *stream, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count, proven_u64 content_length);
[[nodiscard]]
proven_result_size_t proven_http_event_h2_write_(proven_http_stream_t *stream, proven_mem_view_t data);
/* `count` trailers after the body, or none (proven_http_stream_end). */
[[nodiscard]]
proven_err_t proven_http_event_h2_end_(proven_http_stream_t *stream, const proven_http_header_t *trailers, proven_size_t count);
[[nodiscard]]
proven_err_t proven_http_event_h2_trailers_(const proven_http_stream_t *stream, proven_http_header_t *fields, proven_size_t cap, proven_size_t *count);
proven_err_t proven_http_event_h2_flush_(proven_http_stream_t *stream);

/* A WebSocket on an HTTP/2 stream (RFC 8441), for ws_event.c through the raw_* functions:
 * begin answers 200 - with `headers` (at most six), and no END_STREAM - and hands the stream
 * to `raw`; the rest are the HTTP/1.1 connection's raw operations, for a stream. */
[[nodiscard]]
proven_err_t proven_http_event_h2_raw_begin_(proven_http_stream_t *stream, void *raw, const proven_http_header_t *headers, proven_size_t count);
bool proven_http_event_h2_raw_send_(proven_http_stream_t *stream, proven_mem_view_t a, proven_mem_view_t b);
void proven_http_event_h2_raw_timer_(proven_http_stream_t *stream, proven_u32 ms);
void proven_http_event_h2_raw_pause_(proven_http_stream_t *stream, bool paused);
void proven_http_event_h2_raw_close_(proven_http_stream_t *stream);
void proven_http_event_h2_raw_kill_(proven_http_stream_t *stream);

/* The engine (h2_conn.c), for this server's driver only: a response head is queued whatever
 * output is waiting - see there for what bounds it instead. */
struct proven_h2_conn;
void proven_h2_conn_heads_past_bound_(struct proven_h2_conn *conn);
/* The engine, for both event-driven drivers (RFC-0019 section 10): windows that grow while they
 * are read, up to the caps (KiB); `now` in microseconds. See h2_conn.c. */
void proven_h2_conn_grow_(struct proven_h2_conn *conn, proven_u16 stream_kib, proven_u16 conn_kib, proven_u64 (*now)(void));
/* The engine, for this server's driver (RFC-0024): the first SETTINGS says `n` streams, the
 * configured number staying the ceiling. Right after proven_h2_conn_create. */
void proven_h2_conn_open_with_streams_(struct proven_h2_conn *conn, proven_u32 n);
void proven_h2_conn_cap_window_(struct proven_h2_conn *conn, proven_u32 n);
/* proven_http_event_server_set_max_streams for each protocol's connections. */
void proven_http_event_h2_set_max_streams_(proven_http_event_server_t *server, proven_u32 n);
void proven_http_event_h3_set_max_streams_(proven_http_event_server_t *server, proven_u32 n);
void proven_http_event_h2_set_window_(proven_http_event_server_t *server, proven_u32 n);
void proven_http_event_h3_set_window_(proven_http_event_server_t *server, proven_u32 n);
/* For tests: stream `id`'s receive window now, or (0) the connection's, or (PROVEN_H2_TEST_CAP)
 * what a stream's may grow to. */
#define PROVEN_H2_TEST_CAP 0xffffffffu
proven_u32 proven_h2_conn_test_window_(const struct proven_h2_conn *conn, proven_u32 id);
/* For tests: what an HTTP/2 request stream's receive window may grow to, on the server's side. */
proven_u32 proven_http_event_h2_test_window_(proven_http_stream_t *stream);
[[nodiscard]]
proven_err_t proven_http_event_h2_respond_(proven_http_stream_t *stream, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count, proven_mem_view_t body);
void proven_http_event_h2_abort_(proven_http_stream_t *stream);
void proven_http_event_h2_pause_(proven_http_stream_t *stream);
void proven_http_event_h2_resume_(proven_http_stream_t *stream);
void proven_http_event_h2_set_user_(proven_http_stream_t *stream, void *user);
[[nodiscard]]
void *proven_http_event_h2_user_(const proven_http_stream_t *stream);
[[nodiscard]]
proven_net_addr_t proven_http_event_h2_peer_(const proven_http_stream_t *stream);
[[nodiscard]]
proven_size_t proven_http_event_h2_buffered_(const proven_http_stream_t *stream);

#define PROVEN_HTTP_EVENT_IS_H2_(stream) (((const proven_ev_head_t *)(const void *)(stream))->kind == EV_KIND_H2)
#define PROVEN_HTTP_EVENT_IS_H3_(stream) (((const proven_ev_head_t *)(const void *)(stream))->kind == EV_KIND_H3)

/* The count of connections has changed (one came or went, outside http_event.c): the listeners
 * may take connections again, or must stop at `max_connections`, and a shutdown may be over. */
void proven_http_event_gone_(proven_http_event_server_t *server);
/* Something of the server's (an HTTP/3 socket) that waited for the loop's last answer has had it
 * and given its memory back: the server's struct goes with the last such once destroy returned. */
void proven_http_event_ended_(proven_http_event_server_t *server);

// -----------------------------------------------------------------------------
// HTTP/3 (http_event_h3.c): UDP sockets, QUIC connections, and their streams
// -----------------------------------------------------------------------------

#if !defined(PROVEN_NO_HTTP3)
/* At create, with `http3`: the state every socket shares (connection IDs, the key). */
[[nodiscard]]
proven_err_t proven_http_event_h3_create_(proven_http_event_server_t *server);
/* A UDP socket at `at` (the TCP listener's address, its port chosen). */
[[nodiscard]]
proven_err_t proven_http_event_h3_listen_(proven_http_event_server_t *server, proven_net_addr_t at);
/* listen_port (RFC-0037): the port's UDP socket for loop `index` of `loops`, taken (`*udp` is
 * left closed on success), bound at `at`; the group's key. INVALID_STATE once the server has had a
 * socket. */
[[nodiscard]]
proven_err_t proven_http_event_h3_listen_port_(proven_http_event_server_t *server, proven_net_udp_t *udp, proven_net_addr_t at,
                                               proven_size_t loops, proven_size_t index, const proven_byte_t key[32]);
/* stop_listening: no new connection; each says goodbye, in two steps. */
void proven_http_event_h3_stop_(proven_http_event_server_t *server);
/* Every connection ended now: exchanges get on_done(PROVEN_ERR_RESET). */
void proven_http_event_h3_end_all_(proven_http_event_server_t *server);
/* destroy: everything ended, sockets closed, state freed. */
void proven_http_event_h3_destroy_(proven_http_event_server_t *server);
/* The UDP port `Alt-Svc` names for a TCP connection that came in on `local_port`: 0 for none. */
[[nodiscard]]
proven_u16 proven_http_event_h3_port_(const proven_http_event_server_t *server, proven_u16 local_port);
/* The value of the `Alt-Svc` a TCP response carries, `h3=":PORT"`, into `out`: its length, or 0
 * when there is none to send - no HTTP/3 socket, or the handler's `headers` have one of their own. */
[[nodiscard]]
proven_size_t proven_http_event_alt_svc_(const proven_http_event_server_t *server, proven_u16 local_port, const proven_http_header_t *headers,
                                         proven_size_t header_count, proven_byte_t out[16]);

void proven_http_event_h3_compress_(proven_http_stream_t *stream);
[[nodiscard]]
proven_err_t proven_http_event_h3_begin_(proven_http_stream_t *stream, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count, proven_u64 content_length);
[[nodiscard]]
proven_result_size_t proven_http_event_h3_write_(proven_http_stream_t *stream, proven_mem_view_t data);
/* `count` trailers after the body, or none (proven_http_stream_end). */
[[nodiscard]]
proven_err_t proven_http_event_h3_end_(proven_http_stream_t *stream, const proven_http_header_t *trailers, proven_size_t count);
[[nodiscard]]
proven_err_t proven_http_event_h3_trailers_(const proven_http_stream_t *stream, proven_http_header_t *fields, proven_size_t cap, proven_size_t *count);
proven_err_t proven_http_event_h3_flush_(proven_http_stream_t *stream);
[[nodiscard]]
proven_err_t proven_http_event_h3_respond_(proven_http_stream_t *stream, proven_u16 status, const proven_http_header_t *headers, proven_size_t header_count, proven_mem_view_t body);
void proven_http_event_h3_abort_(proven_http_stream_t *stream);
void proven_http_event_h3_pause_(proven_http_stream_t *stream);
void proven_http_event_h3_resume_(proven_http_stream_t *stream);
void proven_http_event_h3_set_user_(proven_http_stream_t *stream, void *user);
[[nodiscard]]
void *proven_http_event_h3_user_(const proven_http_stream_t *stream);
[[nodiscard]]
proven_net_addr_t proven_http_event_h3_peer_(const proven_http_stream_t *stream);
[[nodiscard]]
proven_size_t proven_http_event_h3_buffered_(const proven_http_stream_t *stream);
[[nodiscard]]
bool proven_http_event_h3_early_(const proven_http_stream_t *stream);
#endif
/* An HTTP/2 stream whose request arrived in early data (RFC-0027). */
bool proven_http_event_h2_early_(const proven_http_stream_t *stream);

/* The server's compression settings, for http_coding.c. */
static inline proven_http_compress_cfg_t_ proven_http_event_compress_cfg_(const proven_http_event_server_config_t *c) {
    return (proven_http_compress_cfg_t_){ c->compress_level, c->compress_window_bits, c->compress_zstd, c->compress_zstd_level, c->compress_zstd_window_log };
}

#endif /* hosted, with a network */

#endif /* PROVEN_INTERNAL_HTTP_EVENT_H */
