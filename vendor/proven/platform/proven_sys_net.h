#ifndef PROVEN_PLATFORM_SYS_NET_H
#define PROVEN_PLATFORM_SYS_NET_H

#include "proven/types.h"

/**
 * @file proven_sys_net.h
 * @brief Platform Abstraction Layer for sockets: POSIX sockets and Winsock.
 *
 * This is the ONLY place a socket header is included. Every call answers with a reason from
 * proven_sys_net_result_t - never errno, never a Winsock code - and the portable layer
 * (src/proven/net.c) turns reasons into proven_err_t in one function.
 *
 * What the unit takes care of, so that nothing above it has to know:
 *
 *   - Every socket is created non-blocking and not inherited by child processes. Blocking
 *     behaviour is built above this unit from readiness and a deadline, which is what makes a
 *     timeout possible at all.
 *   - A write to a closed peer never raises SIGPIPE: MSG_NOSIGNAL where it exists, SO_NOSIGPIPE
 *     where it does not. No signal handler is installed on the caller's behalf.
 *   - EINTR is retried here, except in the wait, which reports "nothing ready" so that the
 *     caller re-reads its deadline rather than waiting the full interval again.
 *   - Winsock is started once per process, lazily, before the first call that needs it -
 *     name resolution included. The handle is a SOCKET and is carried at full width.
 *   - A listening TCP socket sets SO_REUSEADDR on POSIX, so a restarted server can bind while
 *     old connections linger. On Windows that option means something else - it lets a second
 *     process take the port - so SO_EXCLUSIVEADDRUSE is set there instead.
 *   - An IPv6 socket is IPv6-only on every platform, so the same program does not accept IPv4
 *     on one system and refuse it on another.
 *
 * Hosted only. A freestanding build has no sockets and does not compile this unit.
 */

/** A socket, at the platform's full handle width. */
typedef proven_uintptr_t proven_sys_socket_t;
#define PROVEN_SYS_SOCKET_INVALID (~(proven_sys_socket_t)0)

typedef enum {
    PROVEN_SYS_NET_OK = 0,
    PROVEN_SYS_NET_WOULD_BLOCK,   /**< not now; wait for readiness and try again */
    PROVEN_SYS_NET_IN_PROGRESS,   /**< a connect was started and has not finished */
    PROVEN_SYS_NET_EOF,           /**< the peer closed its sending side */
    PROVEN_SYS_NET_TIMEOUT,       /**< the OS itself gave up (a connect that was never answered) */
    PROVEN_SYS_NET_REFUSED,       /**< nothing is listening there */
    PROVEN_SYS_NET_RESET,         /**< the peer reset or aborted the connection, or is gone */
    PROVEN_SYS_NET_UNREACHABLE,   /**< no route to the network or host */
    PROVEN_SYS_NET_ADDR_IN_USE,   /**< the address to bind is taken */
    PROVEN_SYS_NET_ADDR_INVALID,  /**< the address cannot be used here (not local, wrong family) */
    PROVEN_SYS_NET_NOT_FOUND,     /**< the host name has no address; a Unix-domain path is not there */
    PROVEN_SYS_NET_DENIED,        /**< permission refused it (a privileged port, a firewall rule) */
    PROVEN_SYS_NET_UNSUPPORTED,   /**< this address family or socket kind does not exist here */
    PROVEN_SYS_NET_TRUNCATED,     /**< a datagram was larger than the buffer; the rest is gone */
    PROVEN_SYS_NET_TOO_BIG,       /**< a datagram is larger than the network will carry */
    PROVEN_SYS_NET_LIMIT,         /**< out of descriptors, buffers or memory */
    PROVEN_SYS_NET_EXISTS,        /**< a selector already holds this socket */
    PROVEN_SYS_NET_CANCELLED,     /**< an operation of a completion port was cancelled (Windows) */
    PROVEN_SYS_NET_ERROR          /**< anything else */
} proven_sys_net_result_t;

typedef enum {
    PROVEN_SYS_NET_FAMILY_NONE = 0,
    PROVEN_SYS_NET_FAMILY_IPV4,
    PROVEN_SYS_NET_FAMILY_IPV6,
    PROVEN_SYS_NET_FAMILY_UNIX
} proven_sys_net_family_t;

typedef enum {
    PROVEN_SYS_NET_STREAM = 0,
    PROVEN_SYS_NET_DATAGRAM
} proven_sys_net_kind_t;

/** Longest Unix-domain path carried, NUL excluded. The smallest sun_path in use is 104 bytes. */
#define PROVEN_SYS_NET_UNIX_PATH_MAX 103

/** An address in a form no OS header is needed to read. Network byte order in `ip`. */
typedef struct {
    proven_sys_net_family_t family;
    proven_u16 port;        /**< host byte order */
    proven_u32 scope_id;    /**< IPv6 zone index; 0 when none */
    proven_u8  ip[16];      /**< IPv4 in the first four bytes */
    proven_u8  path_len;    /**< Unix-domain only */
    char       path[PROVEN_SYS_NET_UNIX_PATH_MAX + 1];
} proven_sys_net_addr_t;

/** Readiness to ask for, and readiness reported. */
enum {
    PROVEN_SYS_NET_READABLE = 1 << 0,
    PROVEN_SYS_NET_WRITABLE = 1 << 1,
    PROVEN_SYS_NET_FAILED   = 1 << 2,  /**< reported only: error or hang-up; the next call says which */
    PROVEN_SYS_NET_IGNORE   = 1 << 7   /**< asked only, of proven_sys_net_wait: leave this item out of the wait - nothing is reported for it */
};

typedef struct {
    proven_sys_socket_t sock;
    proven_u8 want;
    proven_u8 got;
} proven_sys_net_wait_t;

/** Open a listening stream socket bound to `addr` (port 0 lets the OS choose). */
proven_sys_net_result_t proven_sys_net_open_listener(const proven_sys_net_addr_t *addr, int backlog,
                                                     proven_sys_socket_t *out);

/** Open a datagram socket bound to `addr`. IPv4 and IPv6 only. */
proven_sys_net_result_t proven_sys_net_open_datagram(const proven_sys_net_addr_t *addr, proven_sys_socket_t *out);

/** Open a listening stream socket (`stream`) or a datagram socket bound to `addr` with
 *  SO_REUSEPORT, so that others opened this way share the address and the system spreads what
 *  arrives over them (RFC-0037). Linux only: UNSUPPORTED elsewhere. */
proven_sys_net_result_t proven_sys_net_open_shared(const proven_sys_net_addr_t *addr, bool stream, int backlog,
                                                   proven_sys_socket_t *out);

/** Give the group of datagram sockets `sock` belongs to - `n` of them, 1 to 255, opened by
 *  proven_sys_net_open_shared in turn - a program that picks the socket for each datagram from a
 *  QUIC packet's destination connection ID: its first byte modulo `n` is the socket's place in
 *  the order they were opened (SO_ATTACH_REUSEPORT_CBPF). Linux only: UNSUPPORTED elsewhere. */
proven_sys_net_result_t proven_sys_net_udp_steer(proven_sys_socket_t sock, proven_u32 n);

/**
 * Create a stream socket and start connecting it. OK means connected already; IN_PROGRESS means
 * wait for it to become writable and then ask proven_sys_net_connect_result. On any other
 * answer no socket is left open.
 */
proven_sys_net_result_t proven_sys_net_connect_start(const proven_sys_net_addr_t *addr, proven_sys_socket_t *out);

/** The outcome of a connect that was IN_PROGRESS, once the socket reported ready. */
proven_sys_net_result_t proven_sys_net_connect_result(proven_sys_socket_t sock);

/**
 * Make two stream sockets connected to each other, both non-blocking and not inherited: what
 * one writes the other reads. A Unix-domain socket pair on POSIX; on Windows, which has none,
 * a TCP connection through the loopback interface.
 */
proven_sys_net_result_t proven_sys_net_pair(proven_sys_socket_t *a, proven_sys_socket_t *b);

/** Take one pending connection. WOULD_BLOCK when none is waiting. `peer` may be NULL. */
proven_sys_net_result_t proven_sys_net_accept(proven_sys_socket_t listener, proven_sys_socket_t *out,
                                              proven_sys_net_addr_t *peer);

/** Receive up to `cap` bytes. EOF when the peer has closed; never OK with zero bytes for cap > 0. */
proven_sys_net_result_t proven_sys_net_recv(proven_sys_socket_t sock, void *buf, proven_size_t cap,
                                            proven_size_t *received);

/** Send up to `len` bytes; `sent` says how many went. */
proven_sys_net_result_t proven_sys_net_send(proven_sys_socket_t sock, const void *buf, proven_size_t len,
                                            proven_size_t *sent);

/** One buffer of proven_sys_net_send_vec. */
typedef struct { const void *ptr; proven_size_t len; } proven_sys_net_vec_t;
/** The most buffers one proven_sys_net_send_vec offers; the rest wait for the next call. */
#define PROVEN_SYS_NET_VEC_MAX 16

/** Send from up to PROVEN_SYS_NET_VEC_MAX buffers in order, in one call; `sent` says how many
 *  bytes went, which may end inside any of them. */
proven_sys_net_result_t proven_sys_net_send_vec(proven_sys_socket_t sock, const proven_sys_net_vec_t *vec, proven_size_t count,
                                                proven_size_t *sent);

/** Receive one datagram. TRUNCATED when it did not fit: `received` bytes are in `buf`, the rest is lost. */
proven_sys_net_result_t proven_sys_net_recv_from(proven_sys_socket_t sock, void *buf, proven_size_t cap,
                                                 proven_size_t *received, proven_sys_net_addr_t *from);

/** Send one datagram, whole or not at all. */
proven_sys_net_result_t proven_sys_net_send_to(proven_sys_socket_t sock, const void *buf, proven_size_t len,
                                               const proven_sys_net_addr_t *to);

/** The largest batch proven_sys_net_send_segments takes: the kernel's limits on one UDP send. */
#define PROVEN_SYS_NET_SEGMENTS_MAX_BYTES ((proven_size_t)65507)
#define PROVEN_SYS_NET_SEGMENTS_MAX_COUNT 64

/** Send `total` bytes as datagrams of `seg` bytes each (the last may be shorter) to one address
 *  (RFC-0018 S-3). On Linux one call carries them (UDP_SEGMENT, GSO) when the kernel takes it,
 *  on Windows likewise (UDP_SEND_MSG_SIZE, USO; RFC-0018 section 12); everywhere else, and
 *  after a refusal, one datagram at a time. `sent` says how many bytes
 *  went - all of them on OK; on WOULD_BLOCK a whole number of datagrams, the rest not sent. At
 *  most PROVEN_SYS_NET_SEGMENTS_MAX_BYTES and _MAX_COUNT datagrams. `ecn` (0 to 3, RFC 3168) is
 *  the ECN field of every datagram (RFC-0035): a control message beside the batch's (Linux IP_TOS
 *  / IPV6_TCLASS, Windows IP_ECN / IPV6_ECN); 0 or where the system refuses it, none. */
proven_sys_net_result_t proven_sys_net_send_segments(proven_sys_socket_t sock, const void *buf, proven_size_t total,
                                                     proven_size_t seg, const proven_sys_net_addr_t *to, proven_u8 ecn, proven_size_t *sent);

/** Ask to be told the ECN field of what arrives (RFC-0035): Linux IP_RECVTOS / IPV6_RECVTCLASS,
 *  Windows IP_ECN / IPV6_ECN (with WSARecvMsg at hand). `family` is the socket's. UNSUPPORTED
 *  elsewhere - where nothing is marked on sending either. */
proven_sys_net_result_t proven_sys_net_udp_ecn(proven_sys_socket_t sock, proven_sys_net_family_t family);

/** Ask for UDP_GRO (Linux) or URO (Windows, UDP_RECV_MAX_COALESCED_SIZE, at most
 *  PROVEN_SYS_NET_SEGMENTS_MAX_BYTES): the system may then hand several datagrams of one sender
 *  in one receive, their size reported by proven_sys_net_recv_segments. UNSUPPORTED elsewhere. */
proven_sys_net_result_t proven_sys_net_udp_gro(proven_sys_socket_t sock);

/** Set the don't-fragment bit on what the socket sends, and fragment nothing here (RFC-0018
 *  11; RFC 9000 section 14): Linux IP_PMTUDISC_PROBE / IPV6_PMTUDISC_PROBE (IP_PMTUDISC_DO where
 *  PROBE is missing), Windows IP_DONTFRAGMENT / IPV6_DONTFRAG, the BSDs IP_DONTFRAG /
 *  IPV6_DONTFRAG. `family` is the socket's. UNSUPPORTED where none of them exists. */
proven_sys_net_result_t proven_sys_net_udp_dontfrag(proven_sys_socket_t sock, proven_sys_net_family_t family);

/** proven_sys_net_recv_from, and in `seg` the size of each datagram when the buffer holds
 *  several (a GRO or URO receive; the last may be shorter), or 0 when it holds one; in `ecn`
 *  (may be NULL) the ECN field they arrived with, 0 when the system did not say. */
proven_sys_net_result_t proven_sys_net_recv_segments(proven_sys_socket_t sock, void *buf, proven_size_t cap,
                                                     proven_size_t *received, proven_size_t *seg, proven_u8 *ecn, proven_sys_net_addr_t *from);

/** For tests: true makes the process send one datagram at a time and ask for no GRO or URO, as
 *  every platform but Linux and Windows does. */
void proven_sys_net_segments_off_for_test_(bool off);

/** Close the sending side; the peer reads end of input, and this side can still receive. */
proven_sys_net_result_t proven_sys_net_shutdown_write(proven_sys_socket_t sock);

proven_sys_net_result_t proven_sys_net_close(proven_sys_socket_t sock);

proven_sys_net_result_t proven_sys_net_local_addr(proven_sys_socket_t sock, proven_sys_net_addr_t *out);
proven_sys_net_result_t proven_sys_net_peer_addr(proven_sys_socket_t sock, proven_sys_net_addr_t *out);

/** Turn Nagle's algorithm off (true) or on (false) for a TCP socket. */
proven_sys_net_result_t proven_sys_net_set_nodelay(proven_sys_socket_t sock, bool on);

/** Bytes of scratch memory proven_sys_net_wait needs for `count` items; SIZE_MAX if not representable. */
proven_size_t proven_sys_net_wait_scratch_size(proven_size_t count);

/**
 * Wait until one of `items` is ready, or `timeout_ms` passes (negative: no limit).
 *
 * `scratch` is caller memory of at least proven_sys_net_wait_scratch_size(count) bytes, aligned
 * for a pointer: the OS wants its own array, and this unit does not allocate.
 *
 * `ready` is the number of items with a non-zero `got`. OK with `ready == 0` means the time
 * passed - or the wait was interrupted, which the caller cannot and need not tell apart: it
 * re-reads its deadline either way.
 *
 * POSIX poll; Windows WSAPoll. WSAPoll did not report a failed connect before Windows 10
 * version 2004; this library is not tested on anything older.
 */
proven_sys_net_result_t proven_sys_net_wait(proven_sys_net_wait_t *items, proven_size_t count,
                                            void *scratch, int timeout_ms, proven_size_t *ready);

/**
 * A kernel-held set of sockets: epoll on Linux, kqueue on the BSDs and macOS. The set persists
 * between waits and a wait returns only the sockets that are ready, so its cost does not grow
 * with the number of idle sockets. Where neither exists (Windows, other systems)
 * proven_sys_net_selector_open answers PROVEN_SYS_NET_UNSUPPORTED and the portable layer
 * builds the same interface on proven_sys_net_wait.
 *
 * Level-triggered: a socket is reported for as long as it is ready.
 */
typedef struct {
    void *tag;          /**< what was registered with the socket */
    proven_u8 got;      /**< PROVEN_SYS_NET_READABLE / _WRITABLE / _FAILED */
} proven_sys_net_event_t;

typedef enum {
    PROVEN_SYS_NET_SELECTOR_NONE = 0,
    PROVEN_SYS_NET_SELECTOR_EPOLL,
    PROVEN_SYS_NET_SELECTOR_KQUEUE
} proven_sys_net_selector_kind_t;

/** Which kernel facility this build uses; NONE when it has neither. */
proven_sys_net_selector_kind_t proven_sys_net_selector_kind(void);

proven_sys_net_result_t proven_sys_net_selector_open(proven_uintptr_t *out);
void proven_sys_net_selector_close(proven_uintptr_t selector);

/** Register `sock` (`add` true) or change what is asked of it. EXISTS when added twice,
 *  NOT_FOUND when changed without being there. */
proven_sys_net_result_t proven_sys_net_selector_set(proven_uintptr_t selector, proven_sys_socket_t sock,
                                                    proven_u8 want, void *tag, bool add);

/** Take `sock` out of the set. A socket that is closed leaves the set by itself. */
proven_sys_net_result_t proven_sys_net_selector_remove(proven_uintptr_t selector, proven_sys_socket_t sock);

/**
 * Wait until something in the set is ready or `timeout_ms` passes (negative: no limit), and
 * report at most `cap` of them. OK with `count == 0` means the time passed or the wait was
 * interrupted. A hang-up is reported as readable and writable, since which was asked for is
 * not known here; with kqueue a socket ready both ways appears as two events.
 */
proven_sys_net_result_t proven_sys_net_selector_wait(proven_uintptr_t selector, proven_sys_net_event_t *events,
                                                     proven_size_t cap, int timeout_ms, proven_size_t *count);

/* io_uring on Linux (RFC-0029): the same port, its operations entries of a ring of the kernel's.
 * Built where the kernel's header is there; whether the running kernel has what is needed is
 * asked at run time (proven_sys_net_port_available). PROVEN_SYS_NO_URING leaves it out. */
#if defined(__linux__) && !defined(PROVEN_SYS_NO_URING) && defined(__has_include)
#if __has_include(<linux/io_uring.h>)
#define PROVEN_SYS_RING 1
#endif
#endif
#ifndef PROVEN_SYS_RING
#define PROVEN_SYS_RING 0
#endif

#if defined(_WIN32) || defined(_WIN64) || PROVEN_SYS_RING
/*
 * I/O completion ports, on Windows (RFC-0025 step 6), and the same on Linux over io_uring
 * (RFC-0029): what the event loop's completion operations are built on. Nothing else calls them.
 *
 * On Linux an operation is an entry of the ring, submitted with the next wait, and never
 * finishes at once: every start that does not fail says IN_PROGRESS. What an entry points at -
 * a buffer, the message of a send, the OVERLAPPED - must stay put until the port reports it.
 *
 * An operation is started with an OVERLAPPED of the caller's (proven_sys_net_ov_t), which must
 * stay where it is, untouched, until the port has reported it. `skip` is what
 * proven_sys_net_port_bind said for the socket. A start answers:
 *   OK            finished at once, `*bytes` moved; the port reports nothing for it.
 *   IN_PROGRESS   under way (or finished, on a socket without `skip`): the port reports it.
 *   anything else failed at once; the port reports nothing for it.
 */
#if defined(_WIN32) || defined(_WIN64)
typedef struct { proven_uintptr_t w[sizeof(void *) == 8 ? 4 : 5]; } proven_sys_net_ov_t;
#else
typedef struct { proven_u64 w; } proven_sys_net_ov_t;   /* io_uring: the kind, and the result */
#endif

/** One report of a port: the operation's OVERLAPPED (null for a proven_sys_net_port_post),
 *  the bytes it moved, and whether it failed - proven_sys_net_ov_result says why. `more`: the
 *  operation goes on and reports again (a multishot receive's report that is not its last). */
typedef struct {
    proven_sys_net_ov_t *ov;
    proven_u32 bytes;
    bool failed;
    bool more;
} proven_sys_net_done_t;

/** Whether proven_sys_net_port_open can work here: on Windows always; on Linux when io_uring is
 *  there and allowed (a container's seccomp filter may refuse it) and the kernel has every
 *  operation used (5.19 or later: cancelling all of a socket's operations). Asked once. */
bool proven_sys_net_port_available(void);
/** Whether a loop that names no backend should be made on the port: on Windows always; on Linux
 *  when proven_sys_net_port_available and the kernel has multishot receives into a pool of
 *  buffers as well (6.0 or later) - what saves the calls (RFC-0030 D-4). */
bool proven_sys_net_port_preferred(void);
proven_sys_net_result_t proven_sys_net_port_open(proven_uintptr_t *out);
void proven_sys_net_port_close(proven_uintptr_t port);
/** Bind `sock` to the port for the rest of its life. A socket bound already is taken to be bound
 *  to this port. `*skip` (only when `allow_skip`): an operation of this socket that finishes at
 *  once is not reported. Not for datagram sockets: there a receive cut short finishes at once
 *  with an error and is reported all the same. */
proven_sys_net_result_t proven_sys_net_port_bind(proven_uintptr_t port, proven_sys_socket_t sock, bool allow_skip, bool *skip);
/** A report with no operation, from any thread: the loop's waker. */
proven_sys_net_result_t proven_sys_net_port_post(proven_uintptr_t port);
/** Wait up to `timeout_ms` (negative: no limit) and take at most `cap` reports; OK with
 *  `*count` 0 when the time passed. */
proven_sys_net_result_t proven_sys_net_port_wait(proven_uintptr_t port, proven_sys_net_done_t *out, proven_size_t cap,
                                                 int timeout_ms, proven_size_t *count);
/** Why a reported operation failed: CANCELLED, RESET, REFUSED, TRUNCATED (a datagram cut), ... */
proven_sys_net_result_t proven_sys_net_ov_result(proven_sys_socket_t sock, proven_sys_net_ov_t *ov);
/** Cancel `ov`'s operation on `sock`, or every one of them (`ov` null); each is still reported.
 *  The operations take the port they are reported to (Windows: the socket is bound to it). */
void proven_sys_net_ov_cancel(proven_uintptr_t port, proven_sys_socket_t sock, proven_sys_net_ov_t *ov);
#if PROVEN_SYS_RING
/** io_uring: cancel every operation of the port, each still reported (for a loop's end). */
void proven_sys_net_port_cancel_all(proven_uintptr_t port);

/*
 * io_uring, RFC-0030: a pool of `count` buffers of `bytes` each that the kernel fills as data
 * arrives - a multishot receive takes one only when there is something to put in it, so an idle
 * socket holds none. Made once per port; false where the kernel cannot (before 6.0, or refused):
 * receives then stay as they were. `count` a power of two, at most 32768; `bytes` at most 65536.
 */
bool proven_sys_net_port_pool(proven_uintptr_t port, proven_size_t count, proven_size_t bytes);
/** Whether the pool has a buffer the kernel can take now. */
bool proven_sys_net_port_pool_free(proven_uintptr_t port);
/** How many of the pool's buffers hold data not yet taken (for tests). */
proven_size_t proven_sys_net_port_pool_held(proven_uintptr_t port);
/** A multishot receive of `sock` into the pool, reported once per buffer filled (`more`) and
 *  once more at its end. The OVERLAPPED then holds the socket's filled buffers in order - what
 *  proven_sys_net_ov_read takes - and how the receive ended; it is not a result word. */
proven_sys_net_result_t proven_sys_net_ov_recv_multi(proven_uintptr_t port, proven_sys_socket_t sock, proven_sys_net_ov_t *ov);
/** Take up to `cap` bytes of what a multishot receive has filled, in order; emptied buffers go
 *  back to the pool. OK with `*n` > 0; WOULD_BLOCK: nothing yet, the receive goes on; EOF or the
 *  reason it failed once everything before it is taken; NOT_FOUND: nothing, and nothing coming
 *  (it was stopped, cancelled or ran out of buffers) - read the socket. */
proven_sys_net_result_t proven_sys_net_ov_read(proven_uintptr_t port, proven_sys_net_ov_t *ov, void *buf, proven_size_t cap, proven_size_t *n);
/** How many buffers the OVERLAPPED holds (at most 15 told). */
unsigned proven_sys_net_ov_held(proven_uintptr_t port, const proven_sys_net_ov_t *ov);
/** Stop a multishot receive; it is still reported, its last report with `more` false. */
void proven_sys_net_ov_cancel_multi(proven_uintptr_t port, proven_sys_net_ov_t *ov);
/** Give every buffer the OVERLAPPED holds back to the pool, and forget how it ended. */
void proven_sys_net_ov_drop(proven_uintptr_t port, proven_sys_net_ov_t *ov);
#endif

/** A receive of at most `cap` bytes (0: done when data has arrived, nothing taken). */
proven_sys_net_result_t proven_sys_net_ov_recv(proven_uintptr_t port, proven_sys_socket_t sock, proven_sys_net_ov_t *ov, void *buf, proven_size_t cap,
                                               bool skip, proven_u32 *bytes);
/** What a send keeps while it is in flight: on Linux its message and the list of its buffers,
 *  which the kernel reads when the entry is submitted - after the call. Kept with the bytes. */
#if PROVEN_SYS_RING
typedef struct { proven_u64 w[8 + 2 * PROVEN_SYS_NET_VEC_MAX]; } proven_sys_net_keep_t;
#else
typedef struct { proven_u8 unused; } proven_sys_net_keep_t;
#endif
/** A send of the buffers in order (at most PROVEN_SYS_NET_VEC_MAX). */
proven_sys_net_result_t proven_sys_net_ov_send(proven_uintptr_t port, proven_sys_socket_t sock, proven_sys_net_ov_t *ov, proven_sys_net_keep_t *keep,
                                               const proven_sys_net_vec_t *vec, proven_size_t count, bool skip, proven_u32 *bytes);

/** The room an accept needs for the two addresses it writes. On Linux `*accepted` is not known
 *  until the report: it says IN_PROGRESS with `*accepted` invalid, and the report's `bytes` is
 *  the new socket. */
#define PROVEN_SYS_NET_ACCEPT_AREA 288
/** An accept: `*accepted` is the socket made for it (open from OK or IN_PROGRESS on, and the
 *  caller's to close if the accept fails or is cancelled); call proven_sys_net_ov_accept_done
 *  on it when the accept has finished well. */
proven_sys_net_result_t proven_sys_net_ov_accept(proven_uintptr_t port, proven_sys_socket_t listener, proven_sys_net_family_t family, proven_sys_net_ov_t *ov,
                                                 void *area, bool skip, proven_sys_socket_t *accepted);
proven_sys_net_result_t proven_sys_net_ov_accept_done(proven_sys_socket_t listener, proven_sys_socket_t accepted);

/** A socket for a connect to `addr` (bound to the wildcard address, as a connect of a port
 *  needs); bind it to the port, then proven_sys_net_ov_connect. A Unix-domain connect is not an
 *  operation of the port: it is made at once (OK) or fails. A connect reported by the port is
 *  finished with proven_sys_net_ov_connect_done; one that finished at once is finished already. */
proven_sys_net_result_t proven_sys_net_ov_connect_socket(const proven_sys_net_addr_t *addr, proven_sys_socket_t *out);
proven_sys_net_result_t proven_sys_net_ov_connect(proven_uintptr_t port, proven_sys_socket_t sock, const proven_sys_net_addr_t *addr, proven_sys_net_ov_t *ov,
                                                  bool skip);
proven_sys_net_result_t proven_sys_net_ov_connect_done(proven_sys_socket_t sock);

/** What a datagram receive keeps while it is in flight: the message, the sender's address and
 *  the control data that carries the size of coalesced datagrams. */
typedef struct { proven_u64 w[40]; } proven_sys_net_msg_t;
/** A datagram receive into `buf`. When it has finished, proven_sys_net_ov_msg_done reads the
 *  size of each datagram (`*seg`, 0 for one), the ECN field (`*ecn`, may be NULL; 0 when not
 *  said) and the sender. */
proven_sys_net_result_t proven_sys_net_ov_recv_msg(proven_uintptr_t port, proven_sys_socket_t sock, proven_sys_net_ov_t *ov, proven_sys_net_msg_t *msg,
                                                   void *buf, proven_size_t cap, bool skip, proven_u32 *bytes);
void proven_sys_net_ov_msg_done(const proven_sys_net_msg_t *msg, proven_u32 bytes, proven_size_t *seg, proven_u8 *ecn, proven_sys_net_addr_t *from);

#endif

#if defined(_WIN32) || defined(_WIN64)
/*
 * What the selector of the IOCP kind needs besides the above (RFC-0025 step 7).
 */

/** Bind `sock` to the port, as proven_sys_net_port_bind without `skip`, but say which it was:
 *  OK bound now, EXISTS bound already (to this port or another - the caller knows which). */
proven_sys_net_result_t proven_sys_net_port_attach(proven_uintptr_t port, proven_sys_socket_t sock);

/** What a socket is: `*stream` a connection-oriented socket, `*listening` one that listens. */
proven_sys_net_result_t proven_sys_net_sock_kind(proven_sys_socket_t sock, bool *stream, bool *listening);

/**
 * A readiness notice through the port, for what a receive of the port cannot tell (writable, a
 * connection waiting, a datagram, a connection lost while nothing is read): WSAEventSelect and a
 * wait of the system's thread pool that, when the event is set, posts `marker` to the port.
 * One notice per arm. The struct must stay where it is from open to close.
 */
typedef struct { proven_uintptr_t w[5]; } proven_sys_net_evt_t;
proven_sys_net_result_t proven_sys_net_evt_open(proven_sys_net_evt_t *evt, proven_uintptr_t port, proven_sys_net_ov_t *marker);
/** Ask for one notice about `sock`: PROVEN_SYS_NET_READABLE (data, or a connection to accept)
 *  and/or _WRITABLE; a connect finished and a connection lost are always asked. What holds
 *  already is noticed at once. */
proven_sys_net_result_t proven_sys_net_evt_arm(proven_sys_net_evt_t *evt, proven_sys_socket_t sock, proven_u8 want);
/** The port has reported the marker: the notice is taken. */
void proven_sys_net_evt_taken(proven_sys_net_evt_t *evt);
/** Stop asking about `sock` (null: the socket is gone) and wait out a notice being posted. True
 *  when a notice has been posted and not yet taken: the port still reports the marker. */
bool proven_sys_net_evt_disarm(proven_sys_net_evt_t *evt, proven_sys_socket_t sock);
/** Free the event and the wait; disarmed first. */
void proven_sys_net_evt_close(proven_sys_net_evt_t *evt);
#endif

/**
 * Resolve a host name to addresses for stream sockets, blocking, through the system resolver.
 * `host` is NUL-terminated. At most `cap` addresses are written; `count` says how many.
 * NOT_FOUND when the name has no address.
 */
proven_sys_net_result_t proven_sys_net_resolve(const char *host, proven_u16 port,
                                               proven_sys_net_addr_t *out, proven_size_t cap,
                                               proven_size_t *count);

#endif /* PROVEN_PLATFORM_SYS_NET_H */
