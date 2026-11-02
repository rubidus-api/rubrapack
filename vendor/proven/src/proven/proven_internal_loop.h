#ifndef PROVEN_INTERNAL_LOOP_H
#define PROVEN_INTERNAL_LOOP_H

#include "proven/loop.h"
#include "proven/time.h"

/*
 * Internal: the loop's clock, for tests. Not public API.
 *
 * Timers are measured on the monotonic clock. A test of a timer that is a minute away cannot
 * wait a minute, so it gives the loop a clock of its own and moves it. Every reading the loop
 * makes - where the wheel stands, when a timer is due - goes through this one function.
 *
 * Call it on a loop with no timer set: the wheel is put where the new clock says "now" is.
 * With such a clock, proven_loop_poll must be given PROVEN_NET_DONT_WAIT: how long to sleep
 * is still decided against the real clock.
 */
typedef proven_time_t (*proven_loop_clock_fn)(void *ctx);
void proven_loop_test_clock_(proven_loop_t *loop, proven_loop_clock_fn clock, void *ctx);

/*
 * Internal: "after this round". Not public API.
 *
 * Something on the loop's thread that wants to be called back soon, from the loop and not
 * from where it stands - a connection whose caller has just given it something to send - has
 * had two ways: a post, which wakes the loop with a system call, and asking to be told the
 * socket is writable, which is two changes of the socket's watch and a turn of the loop. This
 * is the third, and costs none of that: `fn` runs when the round being delivered has been
 * delivered, or, asked for between rounds, at the start of the next before the loop sleeps.
 *
 * Loop thread only. Asking again while one is waiting changes nothing (it runs once, with
 * the first `fn` and `ctx`). They run in the order asked. One asked for from inside another
 * runs in the same round, twice over at most; after that at the next round, which does not
 * sleep first. Cancel before the memory goes.
 */
typedef struct proven_loop_after {
    struct proven_loop_after *next, *prev;
    proven_loop_fn fn;
    void *ctx;
    bool waiting;
} proven_loop_after_t;
void proven_loop_after_(proven_loop_t *loop, proven_loop_after_t *after, proven_loop_fn fn, void *ctx);
void proven_loop_after_cancel_(proven_loop_t *loop, proven_loop_after_t *after);

/*
 * Internal: UDP on the loop (RFC-0025 step 5), for HTTP/3's sockets. A receive goes into a buffer
 * of the caller's - never a receive of nothing, which on Windows would lose the datagram - and is
 * answered once with what one receive took: `n` bytes, several datagrams of one sender of `seg`
 * bytes each when the system coalesced them (GRO; `seg` 0: one datagram), from `*from`, with the
 * ECN field `ecn` (0 when the system did not say; proven_net_udp_ecn_ asks for it). The
 * buffer and `*from` are the loop's until then. Armed again from inside `received`, the next
 * receive is tried at once and still answered from the loop. Sends are the caller's own
 * (proven_net_udp_send_segments_); when one says PROVEN_ERR_TIMEOUT, proven_loop_udp_send_wait_
 * has `writable` called once when the socket takes datagrams again. The end: close, then
 * `closed` after everything in flight has been answered (PROVEN_ERR_CANCELLED).
 */
typedef struct proven_loop_udp_fns {
    void (*received)(void *ctx, proven_err_t err, proven_size_t n, proven_size_t seg, proven_u8 ecn);
    void (*writable)(void *ctx, proven_err_t err);
    void (*closed)(void *ctx);
} proven_loop_udp_fns_t;
#if defined(_WIN32) || defined(_WIN64) || defined(__linux__)
typedef struct proven_loop_udp { proven_u64 internal_[64]; } proven_loop_udp_t;   /* with what an overlapped WSARecvMsg (or io_uring's recvmsg) keeps */
#else
typedef struct proven_loop_udp { proven_u64 internal_[10]; } proven_loop_udp_t;
#endif
proven_err_t proven_loop_udp_attach_(proven_loop_t *loop, proven_loop_udp_t *u, proven_net_udp_t *udp, const proven_loop_udp_fns_t *fns, void *ctx);
proven_err_t proven_loop_udp_recv_(proven_loop_t *loop, proven_loop_udp_t *u, proven_mem_mut_t buf, proven_net_addr_t *from);
proven_err_t proven_loop_udp_send_wait_(proven_loop_t *loop, proven_loop_udp_t *u);
void proven_loop_udp_close_(proven_loop_t *loop, proven_loop_udp_t *u);

/* Internal, for tests: a loop over the poll kind of selector, so that the emulation is checked
 * over poll as well as over epoll or kqueue. On Windows there is one kind, the completion port,
 * and this makes that. */
proven_err_t proven_loop_create_poll_(proven_allocator_t alloc, proven_loop_t **out);

/*
 * Internal: writing in place into the loop's send buffers (RFC-0025 section 12, D-2). For the
 * drivers, which format a response head or seal a TLS record straight into the buffer that is
 * sent. reserve: room for at least `min` bytes (at most PROVEN_LOOP_CHUNK_) at the end of what is
 * waiting, perhaps more; PROVEN_ERR_AGAIN past the connection's limit, and the errors of
 * proven_loop_send. commit: `n` bytes of that room were written; they go after what was waiting,
 * from the loop, in one gather write with the rest. A reservation is committed before anything
 * else is sent on the socket: proven_loop_send with nothing committed writes straight to the
 * socket and would put its bytes ahead of the room reserved.
 */
#define PROVEN_LOOP_CHUNK_ ((proven_size_t)16416)
/* How much the loop holds for the socket before proven_loop_send takes no more (64 KiB unless
 * set; at most 0xffffffff). For a driver that bounds what it queues by its own rule. */
void proven_loop_send_limit_(proven_loop_sock_t *sock, proven_size_t bytes);
/* proven_loop_send of several pieces in order (at most PROVEN_NET_VEC_MAX): with nothing waiting
 * they go to the socket in one gather write, and only what it does not take is copied - a
 * response head and its body, a frame's header and payload, leave together without a copy. */
proven_result_size_t proven_loop_sendv_(proven_loop_t *loop, proven_loop_sock_t *sock, const proven_mem_view_t *parts, proven_size_t count);
proven_err_t proven_loop_reserve_(proven_loop_t *loop, proven_loop_sock_t *sock, proven_size_t min, proven_mem_mut_t *room);
void proven_loop_commit_(proven_loop_t *loop, proven_loop_sock_t *sock, proven_size_t n);
/* Write what is committed now, not after the current callback: for a driver that has put a
 * response's records in place and wants them on their way before anything else can happen to
 * the connection. Nothing is answered from inside it (a failure is kept for `writable`). */
void proven_loop_flush_(proven_loop_t *loop, proven_loop_sock_t *sock);

/* Internal, for tests: how many buffers of the io_uring pool (RFC-0030) hold data not yet read;
 * 0 on any other backend or with no pool. */
proven_size_t proven_loop_ring_held_(const proven_loop_t *loop);

#endif /* PROVEN_INTERNAL_LOOP_H */
