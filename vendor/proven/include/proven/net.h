#ifndef PROVEN_NET_H
#define PROVEN_NET_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/u8str.h"
#include "proven/time.h"
#include "proven/stream.h"

/**
 * @file net.h
 * @brief Sockets: addresses, TCP, UDP, Unix-domain streams, readiness, and deadlines.
 *
 * Three ideas hold the whole header together.
 *
 * **Every call that can wait takes a deadline.** A read with no timeout is a read that a
 * silent peer can hold for ever, and a server built on it is a server one stalled client can
 * stop. A deadline is an absolute reading of the monotonic clock, so one deadline can govern a
 * whole exchange - connect, send, receive - without being recomputed between calls. A deadline
 * that passes is `PROVEN_ERR_TIMEOUT`, and it is not damage: the socket is exactly as usable as
 * before.
 *
 * **A failure says which failure.** Nothing listening is `PROVEN_ERR_REFUSED`; a peer that
 * vanished is `PROVEN_ERR_RESET`; a peer that finished is `PROVEN_ERR_EOF`; no route is
 * `PROVEN_ERR_UNREACHABLE`; a taken port is `PROVEN_ERR_BUSY`. Each asks for a different
 * response, and `PROVEN_ERR_IO` is left for what has no better name.
 *
 * **Nothing is hidden.** No call allocates. No global state is visible. A socket is a small
 * value you own and must close; a reader or writer over one uses state you supply.
 *
 * What it does not do: name resolution has no deadline (it is the system resolver, which
 * offers none), a connection made here is not encrypted (tls.h wraps one), and readiness is `poll`, which is the right tool for
 * hundreds of sockets and not for hundreds of thousands.
 *
 * Hosted only. A freestanding build has no sockets, and `PROVEN_NO_NET` leaves this header and
 * its sources out of a hosted build.
 */

// -----------------------------------------------------------------------------
// Addresses
// -----------------------------------------------------------------------------

typedef enum {
    PROVEN_NET_FAMILY_NONE = 0,
    PROVEN_NET_FAMILY_IPV4,
    PROVEN_NET_FAMILY_IPV6,
    PROVEN_NET_FAMILY_UNIX      /**< a Unix-domain path; stream sockets only */
} proven_net_family_t;

/** @brief Longest Unix-domain path an address carries. The smallest limit among the platforms. */
#define PROVEN_NET_UNIX_PATH_MAX ((proven_size_t)103)

/**
 * @brief Where a socket is, or where it should go. A plain value: copy it, compare it, keep it.
 *
 * `ip` holds the address in network byte order - four bytes for IPv4, sixteen for IPv6. `port`
 * is in host byte order: the number you would write down.
 */
typedef struct {
    proven_net_family_t family;
    proven_u16 port;
    proven_u32 scope_id;        /**< IPv6 zone index for a link-local address; 0 when none */
    proven_byte_t ip[16];
    proven_u8 path_len;         /**< Unix-domain only */
    char path[104];             /**< Unix-domain only; NUL-terminated */
} proven_net_addr_t;

/** @brief An IPv4 address from its four bytes, in the order they are written: 192, 0, 2, 1. */
[[nodiscard]]
proven_net_addr_t proven_net_addr_ipv4(proven_u8 a, proven_u8 b, proven_u8 c, proven_u8 d, proven_u16 port);

/** @brief An IPv6 address from its sixteen bytes, in network order. */
[[nodiscard]]
proven_net_addr_t proven_net_addr_ipv6(const proven_byte_t ip[16], proven_u16 port, proven_u32 scope_id);

/** @brief This machine only: 127.0.0.1 or ::1. Family NONE if `family` is neither. */
[[nodiscard]]
proven_net_addr_t proven_net_addr_loopback(proven_net_family_t family, proven_u16 port);

/** @brief Every interface: 0.0.0.0 or ::. For binding a server that the network may reach. */
[[nodiscard]]
proven_net_addr_t proven_net_addr_any(proven_net_family_t family, proven_u16 port);

/**
 * @brief A Unix-domain address from a filesystem path.
 * @return PROVEN_ERR_INVALID_ARG for an empty path or one with a NUL byte in it;
 *         PROVEN_ERR_OUT_OF_BOUNDS for one longer than PROVEN_NET_UNIX_PATH_MAX.
 */
[[nodiscard]]
proven_err_t proven_net_addr_unix(proven_u8str_view_t path, proven_net_addr_t *out);

/**
 * @brief Parse a literal address: `192.0.2.1`, `2001:db8::1`, `[2001:db8::1]`, `fe80::1%3`.
 *
 * Literals only - a name is proven_net_resolve's job, and this function never touches the
 * network. IPv4 is four decimal numbers 0-255 with no leading zeros (`010` is refused, not read
 * as octal eight). IPv6 takes the forms of RFC 4291, an embedded IPv4 tail, optional brackets,
 * and an optional numeric zone after `%`.
 *
 * @return PROVEN_ERR_INVALID_FORMAT when `text` is not an address; `*out` is then untouched.
 */
[[nodiscard]]
proven_err_t proven_net_addr_parse(proven_u8str_view_t text, proven_u16 port, proven_net_addr_t *out);

/**
 * @brief Write an address as text: `192.0.2.1:80`, `[2001:db8::1]:80`, or a Unix-domain path.
 *
 * IPv6 is written in the canonical form of RFC 5952 (lowercase, the longest run of zero groups
 * compressed), so two equal addresses give equal text. No NUL is written.
 *
 * @return PROVEN_ERR_OUT_OF_BOUNDS when `out` is too small - nothing useful is in it then;
 *         PROVEN_ERR_INVALID_ARG for an address of family NONE.
 */
[[nodiscard]]
proven_err_t proven_net_addr_format(const proven_net_addr_t *addr, proven_mem_mut_t out, proven_size_t *written);

/** @brief Bytes that are always enough for proven_net_addr_format. */
#define PROVEN_NET_ADDR_TEXT_MAX ((proven_size_t)128)

/** @brief Same family, same address, same port, same zone - or the same path. */
[[nodiscard]]
bool proven_net_addr_eq(const proven_net_addr_t *a, const proven_net_addr_t *b);

/**
 * @brief Look a host name up, through the system resolver.
 *
 * Writes up to `cap` addresses, each with `port` filled in, in the order the system prefers.
 * A literal address is answered without a query.
 *
 * @warning This call blocks and has NO deadline: the system resolver offers none, and may take
 *          tens of seconds on a broken network. Call it where that is acceptable - at startup,
 *          or on a job - not in a loop that serves other connections.
 *
 * @return PROVEN_ERR_NOT_FOUND when the name has no address; PROVEN_ERR_TIMEOUT when the name
 *         servers did not answer; PROVEN_ERR_INVALID_ARG for an empty name, one longer than 253
 *         bytes, or one with a NUL in it.
 */
[[nodiscard]]
proven_err_t proven_net_resolve(proven_u8str_view_t host, proven_u16 port,
                                proven_net_addr_t *out, proven_size_t cap, proven_size_t *count);

// -----------------------------------------------------------------------------
// Deadlines
// -----------------------------------------------------------------------------

/**
 * @brief The moment a call must give up: a reading of proven_time_monotonic_now.
 *
 * Absolute, not a duration, so that one deadline bounds a sequence of calls. It is on the
 * monotonic clock, so setting the system time does not move it.
 */
typedef proven_time_t proven_net_deadline_t;

/** @brief Wait as long as it takes. Use it knowingly: a silent peer then waits with you. */
#define PROVEN_NET_NO_DEADLINE ((proven_net_deadline_t)INT64_MAX)

/** @brief Do not wait at all: what can be done right now is done, and anything else is
 *         PROVEN_ERR_TIMEOUT. */
#define PROVEN_NET_DONT_WAIT ((proven_net_deadline_t)INT64_MIN)

/** @brief The deadline `ms` milliseconds from now. */
[[nodiscard]]
proven_net_deadline_t proven_net_deadline_in(proven_u32 ms);

// -----------------------------------------------------------------------------
// Stream sockets: TCP and Unix-domain
// -----------------------------------------------------------------------------

/**
 * @brief A listening socket. Opaque; you own it and must close it.
 *
 * Do not copy one that is open: two copies are two owners of one socket.
 */
typedef struct {
    struct { proven_uintptr_t handle; bool open; proven_u8 family; } internal;
} proven_net_listener_t;

/** @brief One connection. Opaque; you own it and must close it. Do not copy one that is open. */
typedef struct {
    struct { proven_uintptr_t handle; bool open; proven_u8 family; } internal;
} proven_net_conn_t;

[[nodiscard]]
static inline bool proven_net_listener_is_open(const proven_net_listener_t *l) {
    return l && l->internal.open;
}

[[nodiscard]]
static inline bool proven_net_conn_is_open(const proven_net_conn_t *c) {
    return c && c->internal.open;
}

/**
 * @brief Listen at `at`, and say where that turned out to be.
 *
 * Port 0 asks the OS to choose a free port; `bound` (which may be NULL) receives the address
 * actually bound, so the port is known without a second call. `backlog` is how many finished
 * connections may wait to be accepted; 0 or less takes the system's maximum.
 *
 * An IPv6 listener accepts IPv6 only, on every platform. A server that wants both families
 * opens two listeners. A Unix-domain listener creates the path, and closing the listener does
 * not remove it.
 *
 * @return PROVEN_ERR_BUSY when the address is taken; PROVEN_ERR_PERMISSION when it may not be
 *         bound (a privileged port); PROVEN_ERR_INVALID_ARG when it is not an address of this
 *         machine; PROVEN_ERR_UNSUPPORTED when the family does not exist here.
 */
[[nodiscard]]
proven_err_t proven_net_listen(proven_net_addr_t at, int backlog,
                               proven_net_listener_t *out, proven_net_addr_t *bound);

/**
 * @brief Take the next connection, waiting until `until` for one.
 * @param peer may be NULL. A Unix-domain peer usually has no path.
 * @return PROVEN_ERR_TIMEOUT when none arrived in time - the listener is unaffected.
 */
[[nodiscard]]
proven_err_t proven_net_accept(proven_net_listener_t *listener, proven_net_deadline_t until,
                               proven_net_conn_t *out, proven_net_addr_t *peer);

/** @brief Stop listening. Connections already accepted are not affected. */
proven_err_t proven_net_listener_close(proven_net_listener_t *listener);

/** @brief The address a listener is bound to. */
[[nodiscard]]
proven_err_t proven_net_listener_addr(const proven_net_listener_t *listener, proven_net_addr_t *out);

/**
 * @brief Connect to `to`, waiting until `until`.
 *
 * @return PROVEN_ERR_REFUSED when nothing listens there; PROVEN_ERR_TIMEOUT when no answer came
 *         by the deadline (or the OS gave up first); PROVEN_ERR_UNREACHABLE when there is no
 *         route; PROVEN_ERR_NOT_FOUND when a Unix-domain path does not exist. On any failure
 *         `*out` is not open and there is nothing to close.
 */
[[nodiscard]]
proven_err_t proven_net_connect(proven_net_addr_t to, proven_net_deadline_t until, proven_net_conn_t *out);

/**
 * @brief Begin connecting to `to` and return at once: the first half of proven_net_connect, for
 *        a program that waits for many sockets together (a selector, or the loop of loop.h).
 *
 * @return PROVEN_OK when the connection is already made - a loopback or Unix-domain connect
 *         often is. PROVEN_ERR_AGAIN when it is under way: `*out` is then a socket that can be
 *         watched and closed and nothing else; wait until it is writable
 *         (PROVEN_NET_WRITABLE, or PROVEN_NET_FAILED) and call proven_net_connect_finish.
 *         Otherwise the failure, as proven_net_connect names it, with `*out` not open.
 */
[[nodiscard]]
proven_err_t proven_net_connect_start(proven_net_addr_t to, proven_net_conn_t *out);

/**
 * @brief Ask how a connect begun by proven_net_connect_start ended, once its socket was
 *        reported writable or failed.
 *
 * @return PROVEN_OK: connected, and `conn` is an ordinary connection. PROVEN_ERR_AGAIN: still
 *         under way; wait again. Otherwise the failure - PROVEN_ERR_REFUSED,
 *         PROVEN_ERR_TIMEOUT, PROVEN_ERR_UNREACHABLE - and `conn` has been closed: there is
 *         nothing more to close. PROVEN_ERR_INVALID_STATE when `conn` is not open.
 */
[[nodiscard]]
proven_err_t proven_net_connect_finish(proven_net_conn_t *conn);

/**
 * @brief Read up to `dest.size` bytes, waiting until `until` for the first of them.
 *
 * Returns as soon as any bytes are available - a stream has no message boundaries, and a full
 * buffer is not something to wait for.
 *
 * @return `.err` PROVEN_ERR_EOF when the peer closed its sending side: never a zero-byte
 *         success. PROVEN_ERR_TIMEOUT when nothing arrived in time; the connection is still
 *         good. PROVEN_ERR_RESET when the peer is gone.
 */
[[nodiscard]]
proven_result_size_t proven_net_read(proven_net_conn_t *conn, proven_mem_mut_t dest, proven_net_deadline_t until);

/**
 * @brief Write what can be written of `src`, waiting until `until` for room.
 *
 * One call may send only part: `.value` says how much went, also when `.err` is not OK.
 * proven_net_write_all is the call that keeps going.
 */
[[nodiscard]]
proven_result_size_t proven_net_write(proven_net_conn_t *conn, proven_mem_view_t src, proven_net_deadline_t until);

/**
 * @brief Write all of `src`, or fail saying how far it got.
 *
 * `.value` is the number of bytes sent, which is `src.size` exactly when `.err` is PROVEN_OK.
 * On PROVEN_ERR_TIMEOUT the bytes counted in `.value` are with the peer and the rest are not:
 * resume from there, or close.
 */
[[nodiscard]]
proven_result_size_t proven_net_write_all(proven_net_conn_t *conn, proven_mem_view_t src, proven_net_deadline_t until);

/**
 * @brief Say "I have nothing more to send". The peer reads end of input; this side can still
 *        read the peer's answer. The connection must still be closed afterwards.
 */
proven_err_t proven_net_shutdown_write(proven_net_conn_t *conn);

/** @brief Close a connection. Safe on one that is not open; the value is reset either way. */
proven_err_t proven_net_close(proven_net_conn_t *conn);

/** @brief This end's address. */
[[nodiscard]]
proven_err_t proven_net_conn_local_addr(const proven_net_conn_t *conn, proven_net_addr_t *out);

/** @brief The other end's address. */
[[nodiscard]]
proven_err_t proven_net_conn_peer_addr(const proven_net_conn_t *conn, proven_net_addr_t *out);

/**
 * @brief Send small writes at once (true) instead of gathering them (false, the default).
 *
 * For a request-response protocol that writes a message in several small pieces. TCP only;
 * PROVEN_ERR_UNSUPPORTED on a Unix-domain connection.
 */
proven_err_t proven_net_conn_set_nodelay(proven_net_conn_t *conn, bool on);

/**
 * @brief Two connections joined to each other: what is written to one is read from the other.
 *
 * No address, no listener, nothing another process can connect to. It is the local end-to-end
 * pipe - for a test that needs a connection without a network, and for handing data between
 * two threads through the same readiness loop that watches the sockets.
 *
 * Both must be closed. (A Unix-domain pair on POSIX; a loopback TCP connection on Windows,
 * where TCP_NODELAY therefore applies and on POSIX it does not.)
 */
[[nodiscard]]
proven_err_t proven_net_pair(proven_net_conn_t *a, proven_net_conn_t *b);

// -----------------------------------------------------------------------------
// Datagram sockets: UDP
// -----------------------------------------------------------------------------

/** @brief A UDP socket. Opaque; you own it and must close it. Do not copy one that is open. */
typedef struct {
    struct { proven_uintptr_t handle; bool open; proven_u8 family; } internal;
} proven_net_udp_t;

[[nodiscard]]
static inline bool proven_net_udp_is_open(const proven_net_udp_t *u) {
    return u && u->internal.open;
}

/**
 * @brief Open a UDP socket bound to `at` (port 0: the OS chooses; `bound` may be NULL).
 *
 * A socket bound to an IPv4 address sends to IPv4 addresses and one bound to IPv6 to IPv6.
 * A client binds proven_net_addr_any(family, 0).
 */
[[nodiscard]]
proven_err_t proven_net_udp_open(proven_net_addr_t at, proven_net_udp_t *out, proven_net_addr_t *bound);

/**
 * @brief Send one datagram to `to`: all of `data`, or nothing.
 *
 * Success means the datagram left this machine, not that it arrived - UDP promises no more.
 *
 * @return PROVEN_ERR_OUT_OF_BOUNDS when `data` is larger than a datagram can be;
 *         PROVEN_ERR_TIMEOUT when the send buffer stayed full until the deadline.
 */
[[nodiscard]]
proven_err_t proven_net_udp_send_to(proven_net_udp_t *udp, proven_net_addr_t to, proven_mem_view_t data,
                                    proven_net_deadline_t until);

/**
 * @brief Receive one datagram, waiting until `until`. `from` may be NULL.
 *
 * A datagram is delivered whole or cut: if it is larger than `dest`, `.err` is
 * PROVEN_ERR_OUT_OF_BOUNDS, `.value` is the `dest.size` bytes that were kept, and the rest of
 * that datagram is gone. A zero-length datagram is a success with `.value` 0 - unlike a stream,
 * where zero bytes would mean the end.
 */
[[nodiscard]]
proven_result_size_t proven_net_udp_recv_from(proven_net_udp_t *udp, proven_mem_mut_t dest,
                                              proven_net_addr_t *from, proven_net_deadline_t until);

/** @brief The address a UDP socket is bound to. */
[[nodiscard]]
proven_err_t proven_net_udp_addr(const proven_net_udp_t *udp, proven_net_addr_t *out);

/** @brief Close a UDP socket. Safe on one that is not open. */
proven_err_t proven_net_udp_close(proven_net_udp_t *udp);

// -----------------------------------------------------------------------------
// Readiness: waiting on many sockets at once
// -----------------------------------------------------------------------------

/** @brief What to wait for on a socket, and what was found. */
typedef enum {
    PROVEN_NET_READABLE = 1 << 0,  /**< a read, an accept or a receive would not wait */
    PROVEN_NET_WRITABLE = 1 << 1,  /**< a write would not wait */
    PROVEN_NET_FAILED   = 1 << 2   /**< reported only: the socket is in error or hung up; the
                                        next call on it says which */
} proven_net_event_t;

/** @brief A socket as proven_net_poll sees it, whatever kind it is. */
typedef struct { proven_uintptr_t raw; bool valid; } proven_net_handle_t;

[[nodiscard]] proven_net_handle_t proven_net_listener_handle(const proven_net_listener_t *listener);
[[nodiscard]] proven_net_handle_t proven_net_conn_handle(const proven_net_conn_t *conn);
[[nodiscard]] proven_net_handle_t proven_net_udp_handle(const proven_net_udp_t *udp);

typedef struct {
    proven_net_handle_t handle;
    proven_u8 want;     /**< PROVEN_NET_READABLE and/or PROVEN_NET_WRITABLE */
    proven_u8 got;      /**< filled in: zero, or the events that are ready */
} proven_net_poll_item_t;

/** @brief The most items proven_net_poll takes; more need proven_net_poll_with. */
#define PROVEN_NET_POLL_INLINE_MAX ((proven_size_t)64)

/**
 * @brief Wait until at least one of `items` is ready, or until `until`.
 *
 * `*ready` is the number of items whose `got` is not zero. Ready means the next call of that
 * kind will not wait - not that it will succeed: a readable connection may read
 * PROVEN_ERR_EOF, and one reported PROVEN_NET_FAILED will say why when it is used.
 *
 * @return PROVEN_ERR_TIMEOUT when nothing became ready in time (`*ready` is 0);
 *         PROVEN_ERR_OUT_OF_BOUNDS for more than PROVEN_NET_POLL_INLINE_MAX items;
 *         PROVEN_ERR_INVALID_ARG if an item's handle is not valid.
 */
[[nodiscard]]
proven_err_t proven_net_poll(proven_net_poll_item_t *items, proven_size_t count,
                             proven_net_deadline_t until, proven_size_t *ready);

/** @brief Bytes of scratch proven_net_poll_with needs for `count` items; SIZE_MAX if `count`
 *         is too large to represent. */
[[nodiscard]]
proven_size_t proven_net_poll_scratch_size(proven_size_t count);

/**
 * @brief proven_net_poll for any number of items, using memory you supply.
 *
 * The operating system wants an array of its own, and this library does not allocate one
 * behind your back. `scratch` must hold proven_net_poll_scratch_size(count) bytes; it is
 * working memory only and may be reused for the next call.
 *
 * @return as proven_net_poll, and PROVEN_ERR_OUT_OF_BOUNDS when `scratch` is too small.
 */
[[nodiscard]]
proven_err_t proven_net_poll_with(proven_mem_mut_t scratch, proven_net_poll_item_t *items, proven_size_t count,
                                  proven_net_deadline_t until, proven_size_t *ready);

/**
 * @brief A way to interrupt proven_net_poll from another thread.
 *
 * A loop blocked in proven_net_poll sees only sockets. A waker is a socket whose only purpose
 * is to become readable when another thread says so: put its handle in the poll list, and any
 * thread that calls proven_net_waker_wake makes the poll return.
 *
 * Caller-owned; do not copy one that is open.
 */
typedef struct {
    proven_net_conn_t reader;
    proven_net_conn_t writer;
} proven_net_waker_t;

/** @brief Open a waker. PROVEN_ERR_BUSY when the system is out of sockets. */
[[nodiscard]]
proven_err_t proven_net_waker_open(proven_net_waker_t *waker);

/**
 * @brief Make the waker readable. Safe to call from any thread, any number of times, and from
 *        several threads at once; wakes that have not been drained yet merge into one.
 */
void proven_net_waker_wake(proven_net_waker_t *waker);

/** @brief Read away what the wakes wrote, so the waker is quiet until the next wake. Called by
 *         the polling thread once it has woken. */
void proven_net_waker_drain(proven_net_waker_t *waker);

/** @brief The handle to poll for PROVEN_NET_READABLE. */
[[nodiscard]]
proven_net_handle_t proven_net_waker_handle(const proven_net_waker_t *waker);

/** @brief Close a waker. Not safe while another thread may still call proven_net_waker_wake. */
void proven_net_waker_close(proven_net_waker_t *waker);

// -----------------------------------------------------------------------------
// A selector: readiness for many sockets, without handing all of them over each time
// -----------------------------------------------------------------------------

/**
 * @brief A set of sockets that the system watches for you.
 *
 * proven_net_poll is given its whole list on every call, and the system looks at every entry on
 * every call - fine for dozens of sockets, wasteful for thousands of which a handful are busy.
 * A selector is the same question asked the other way round: you register a socket once, and a
 * wait returns only the sockets that are ready. On Linux (epoll) and on the BSDs and macOS
 * (kqueue) the cost of a wait then depends on how many sockets are ready, not on how many are
 * registered.
 *
 * On Windows the selector is built on an I/O completion port: a connection waiting to be read
 * is watched by a receive of zero bytes, and what that cannot tell - room to write, a
 * connection waiting to be accepted, a datagram, a connection lost while nothing is asked - by
 * a notice of the system's thread pool, which costs about 500 bytes and two handles per socket
 * while it is asked. Each wait checks the sockets so reported with the call proven_net_poll uses,
 * so what it reports is what the poll kind would. A connection a selector has watched for
 * reading stays bound to that selector's port for as long as it is open: it cannot later be
 * read through another selector or a loop (another selector falls back to a notice).
 * Where the system has none of these, the selector is built on the call proven_net_poll uses.
 * It behaves identically and scales as proven_net_poll does. proven_net_selector_kind says
 * which you have.
 *
 * Readiness is level-triggered, as with proven_net_poll: a socket is reported for as long as
 * it is ready, so an event you do not act on comes back on the next wait.
 *
 * A socket registered with nothing asked of it (`want` of 0) whose connection is lost is
 * reported once - PROVEN_NET_FAILED, or readable and writable for a hang-up - and then not
 * again until proven_net_selector_modify changes what is asked. (The system would report it
 * on every wait. The kqueue kind reports nothing for such a socket until something is asked.)
 *
 * A selector is for one thread. Opaque; made by proven_net_selector_create.
 */
typedef struct proven_net_selector proven_net_selector_t;

/** @brief One ready socket: the tag it was registered with, and what is ready. */
typedef struct {
    void *tag;
    proven_u8 got;      /**< PROVEN_NET_READABLE, PROVEN_NET_WRITABLE and/or PROVEN_NET_FAILED */
} proven_net_ready_t;

typedef enum {
    PROVEN_NET_SELECTOR_POLL = 0,   /**< built on poll / WSAPoll: every wait looks at every socket */
    PROVEN_NET_SELECTOR_EPOLL,      /**< Linux */
    PROVEN_NET_SELECTOR_KQUEUE,     /**< the BSDs and macOS */
    PROVEN_NET_SELECTOR_IOCP        /**< Windows: an I/O completion port */
} proven_net_selector_kind_t;

/**
 * @brief Make a selector. `alloc` holds the selector itself and, for the poll kind, its list; for
 *        the IOCP kind, its list and about 170 bytes per socket (64-bit).
 * @return PROVEN_ERR_INVALID_ARG; PROVEN_ERR_NOMEM; PROVEN_ERR_BUSY when the system is out of
 *         descriptors.
 */
[[nodiscard]]
proven_err_t proven_net_selector_create(proven_allocator_t alloc, proven_net_selector_t **out);

/**
 * @brief Make a selector of the poll kind whatever the system offers.
 *
 * The portable construction, on every platform. For measuring one kind against the other, and
 * for testing on one system the code that runs where the system offers nothing better.
 */
[[nodiscard]]
proven_err_t proven_net_selector_create_poll(proven_allocator_t alloc, proven_net_selector_t **out);

/** @brief Free a selector. The sockets registered with it are not closed. NULL is ignored. Of the
 *         IOCP kind, what the system still holds is cancelled and waited for, up to two seconds. */
void proven_net_selector_destroy(proven_net_selector_t *selector);

/** @brief Which facility this selector is built on. */
[[nodiscard]]
proven_net_selector_kind_t proven_net_selector_kind(const proven_net_selector_t *selector);

/** @brief How many sockets are registered. */
[[nodiscard]]
proven_size_t proven_net_selector_count(const proven_net_selector_t *selector);

/**
 * @brief Register a socket.
 * @param want PROVEN_NET_READABLE and/or PROVEN_NET_WRITABLE. May be 0: the socket is then
 *        reported only when it fails or its peer hangs up.
 * @param tag comes back with every event for this socket - a pointer to your own record of it.
 * @return PROVEN_ERR_EXISTS when the socket is already registered; PROVEN_ERR_INVALID_ARG for
 *         a handle that is not valid; PROVEN_ERR_NOMEM; PROVEN_ERR_BUSY at a system limit.
 */
[[nodiscard]]
proven_err_t proven_net_selector_add(proven_net_selector_t *selector, proven_net_handle_t handle, proven_u8 want, void *tag);

/** @brief Change what is asked of a registered socket, and its tag.
 *  @return PROVEN_ERR_NOT_FOUND when it is not registered. */
[[nodiscard]]
proven_err_t proven_net_selector_modify(proven_net_selector_t *selector, proven_net_handle_t handle, proven_u8 want, void *tag);

/**
 * @brief Take a socket out of the selector. **Do this before closing the socket.** A closed
 *        socket's number is reused by the next one opened, and a selector that still holds the
 *        old registration will report events for the wrong socket - or, with the poll kind,
 *        report a failure on every wait.
 * @return PROVEN_ERR_NOT_FOUND when it is not registered.
 */
proven_err_t proven_net_selector_remove(proven_net_selector_t *selector, proven_net_handle_t handle);

/**
 * @brief Wait until a registered socket is ready or until `until`, and report up to `cap`.
 *
 * `*count` events are written. When more than `cap` sockets are ready the rest are reported by
 * the next wait - nothing is lost, and no socket is starved by its position. A socket ready to
 * read and to write may come as one event or as two (kqueue reports them apart).
 *
 * As with proven_net_poll, an event says the next call will not wait, not that it will
 * succeed: follow it with a call that takes PROVEN_NET_DONT_WAIT and act on its result.
 *
 * @return PROVEN_ERR_TIMEOUT when nothing became ready (`*count` is 0); PROVEN_ERR_INVALID_ARG
 *         for `cap` 0 or a null pointer.
 */
[[nodiscard]]
proven_err_t proven_net_selector_wait(proven_net_selector_t *selector, proven_net_ready_t *events, proven_size_t cap,
                                      proven_net_deadline_t until, proven_size_t *count);

// -----------------------------------------------------------------------------
// Transport: a connection, as an interface
// -----------------------------------------------------------------------------

/**
 * @brief A byte stream with deadlines, behind one interface.
 *
 * A TCP connection is one. So is anything layered over a connection - encryption, say - and so
 * is a pair of memory buffers in a test. Code written against a transport does not know which
 * it has, which is the point: a protocol is implemented once and carried by any of them.
 *
 * A small vtable passed by value, like proven_allocator_t and proven_writer_t. The contracts
 * are those of proven_net_read and proven_net_write: read never succeeds with zero bytes, and
 * write reports the bytes that went even when it then fails. `shutdown_fn` and `close_fn` may
 * be NULL when there is nothing to do.
 */
typedef struct {
    void *ctx;
    proven_result_size_t (*read_fn)(void *ctx, proven_mem_mut_t dest, proven_net_deadline_t until);
    proven_result_size_t (*write_fn)(void *ctx, proven_mem_view_t src, proven_net_deadline_t until);
    proven_err_t (*shutdown_fn)(void *ctx);
    proven_err_t (*close_fn)(void *ctx);
} proven_transport_t;

[[nodiscard]]
static inline bool proven_transport_is_valid(proven_transport_t t) {
    return t.read_fn != (void *)0 && t.write_fn != (void *)0;
}

/** @brief A connection as a transport. The connection must outlive it; closing the transport
 *         closes the connection. */
[[nodiscard]]
proven_transport_t proven_net_conn_transport(proven_net_conn_t *conn);

/** @brief Read through a transport. PROVEN_ERR_INVALID_ARG for one that is not valid. */
[[nodiscard]]
proven_result_size_t proven_transport_read(proven_transport_t t, proven_mem_mut_t dest, proven_net_deadline_t until);

/** @brief Write what can be written through a transport. */
[[nodiscard]]
proven_result_size_t proven_transport_write(proven_transport_t t, proven_mem_view_t src, proven_net_deadline_t until);

/** @brief Write all of `src` through a transport, or fail saying how far it got. */
[[nodiscard]]
proven_result_size_t proven_transport_write_all(proven_transport_t t, proven_mem_view_t src, proven_net_deadline_t until);

/** @brief End the sending side. PROVEN_OK when the transport has nothing to do for it. */
proven_err_t proven_transport_shutdown(proven_transport_t t);

/** @brief Close the transport. PROVEN_OK when it has nothing to do for it. */
proven_err_t proven_transport_close(proven_transport_t t);

/**
 * @brief State for a proven_reader_t or proven_writer_t over a transport. Caller-owned; it must
 *        outlive the reader or writer made from it, and must not move while they are in use.
 */
typedef struct {
    proven_transport_t transport;
    proven_u32 timeout_ms;
} proven_transport_stream_t;

/**
 * @brief A proven_reader_t over a transport, for the line reader and everything else that
 *        reads a stream.
 *
 * A reader has no place to pass a deadline, so each read gets its own: `timeout_ms` from the
 * moment it starts (0 means no deadline). A read that times out returns PROVEN_ERR_TIMEOUT and
 * may be tried again.
 */
[[nodiscard]]
proven_reader_t proven_transport_reader(proven_transport_stream_t *state, proven_transport_t t, proven_u32 timeout_ms);

/**
 * @brief A proven_writer_t over a transport, so proven_fprint and a buffered writer can send.
 *
 * Each write gets `timeout_ms` from the moment it starts (0 means no deadline). The writer
 * holds nothing back, so it has no flush; wrap it in proven_writer_buffered to send fewer,
 * larger packets.
 */
[[nodiscard]]
proven_writer_t proven_transport_writer(proven_transport_stream_t *state, proven_transport_t t, proven_u32 timeout_ms);

#endif /* PROVEN_NET_H */
