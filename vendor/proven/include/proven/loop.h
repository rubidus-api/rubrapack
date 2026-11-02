#ifndef PROVEN_LOOP_H
#define PROVEN_LOOP_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/allocator.h"
#include "proven/net.h"

/**
 * @file loop.h
 * @brief An event loop: one thread that waits for many things and is told which happened.
 *
 * The blocking calls of net.h give each connection a thread, or make every connection wait
 * its turn. That is the right tool for a hundred connections and the wrong one for tens of
 * thousands, most of which are doing nothing at any moment. A loop turns it around: one thread
 * starts what each connection needs - a receive, a send, an accept - and is told when each has
 * finished, handling those without ever waiting on any one of them.
 *
 * A `proven_loop_t` is four things in one:
 *
 *   - **operations that complete** on sockets given to it: a receive, a send, an accept, a
 *     connect, each finishing with one call of a function of yours. On Windows they are I/O
 *     completion port operations; elsewhere the loop does the same over the selector of net.h
 *     (epoll, kqueue). The program sees the same calls and the same order either way;
 *   - **timers** that cost nothing while they wait - arming, cancelling and firing are
 *     constant-time, and an idle timer is never looked at;
 *   - a **waker** and a queue of **posted functions**, so that another thread can hand work
 *     to the loop's thread;
 *   - a **scratch buffer** every callback may use and none may keep.
 *
 * The rule that makes it work: **a callback must not wait.** Not on a socket, not on a file,
 * not on a lock that someone may hold for long. Whatever takes time goes to another thread
 * (job.h) and comes back through proven_loop_post.
 *
 * The rules of the operations:
 *
 *   1. **Every operation started is answered exactly once**, by a call of its function on the
 *      loop's thread - never from inside the call that started it, even when it could finish at
 *      once. Success, failure and cancellation all come this way.
 *   2. **One receive at a time per socket**, and one accept at a time per acceptor. Sends are
 *      not operations of yours: proven_loop_send takes the bytes, and the loop holds what the
 *      socket could not take yet.
 *   3. **A socket's end is proven_loop_close**, and its `closed` function after everything
 *      still in flight has been answered. Free the memory of the socket no earlier.
 *
 * Everything but proven_loop_stop and proven_loop_post must be called from the loop's own
 * thread - the one inside proven_loop_run - or before it starts.
 *
 * Hosted only; `PROVEN_NO_NET` leaves it out.
 */

/** @brief A loop. Opaque. */
typedef struct proven_loop proven_loop_t;

/** @brief A function the loop calls on its thread. */
typedef void (*proven_loop_fn)(void *ctx);

/**
 * @brief One timer. Yours to hold; zero-initialise it; do not copy or move it while it is set.
 *        A timer fires once.
 */
typedef struct proven_loop_timer {
    struct proven_loop_timer *prev, *next;
    proven_loop_fn fn;
    void *ctx;
    proven_u64 tick;
    proven_u8 state;
} proven_loop_timer_t;

/**
 * @brief What the loop calls for one socket. Usually one `static const` table per kind of
 *        connection, shared by all of them; a member left null is never called (and an operation
 *        that would need it is refused).
 */
typedef struct proven_loop_sock_fns {
    /**
     * A receive has finished. For a receive of zero bytes, `err` PROVEN_OK and `n` 0 say that
     * data has arrived (or the connection has ended): read it now with proven_net_read and
     * PROVEN_NET_DONT_WAIT. For a receive into a buffer: PROVEN_OK and `n` bytes in it, or
     * PROVEN_ERR_EOF, PROVEN_ERR_RESET, and the other errors of proven_net_read. And
     * PROVEN_ERR_CANCELLED after proven_loop_recv_cancel or proven_loop_close - except that on
     * Windows a receive into a buffer cancelled after data had reached it is answered with that
     * data: the system had taken it from the socket already.
     */
    void (*received)(void *ctx, proven_err_t err, proven_size_t n);
    /**
     * The bytes waiting to be sent have come down to what proven_loop_send_wait asked for
     * (`err` PROVEN_OK), or sending them failed (`err` the failure, also when no wait was asked
     * for; what was waiting is dropped and every later send returns it), or the socket was
     * closed while a wait was asked for (PROVEN_ERR_CANCELLED).
     */
    void (*writable)(void *ctx, proven_err_t err);
    /** A connect begun by proven_loop_connect has ended: PROVEN_OK, or as
     *  proven_net_connect_finish fails; PROVEN_ERR_CANCELLED after proven_loop_close. */
    void (*connected)(void *ctx, proven_err_t err);
    /** Last: proven_loop_close was called and nothing of this socket is in flight any more.
     *  Close the connection itself (proven_net_close) and free the memory now. */
    void (*closed)(void *ctx);
} proven_loop_sock_fns_t;

#if defined(_WIN32) || defined(_WIN64)
#define PROVEN_LOOP_SOCK_WORDS_ 15
#define PROVEN_LOOP_ACCEPT_WORDS_ 50
#elif defined(__linux__)
#define PROVEN_LOOP_SOCK_WORDS_ 10
#define PROVEN_LOOP_ACCEPT_WORDS_ 48
#else
#define PROVEN_LOOP_SOCK_WORDS_ 9
#define PROVEN_LOOP_ACCEPT_WORDS_ 48
#endif

/**
 * @brief A connection given to a loop. Yours to hold - inside your connection struct, say -
 *        from proven_loop_attach (or proven_loop_connect) until its `closed`. Do not copy or move
 *        it in between. Opaque: 72 bytes, 80 on Linux and 120 on Windows, which hold everything the loop needs for
 *        the connection, its receive included; the loop allocates nothing per connection except
 *        the buffers of bytes waiting to be sent.
 */
typedef struct proven_loop_sock {
    proven_u64 internal_[PROVEN_LOOP_SOCK_WORDS_];
} proven_loop_sock_t;

/** @brief An accept's answer: `conn` is a new connection, yours from now on - copy it out
 *         before returning - or `err` says why there is none (`conn` then is not open). */
typedef void (*proven_loop_accept_fn)(void *ctx, proven_err_t err, proven_net_conn_t *conn);

/**
 * @brief Accepting on one listener. Yours to hold, beside the listener; zero-initialise it, and
 *        do not copy or move it while an accept is in flight. Opaque. One is enough for a server: it is armed again from
 *        inside its own function, and the loop then takes the next connection waiting without a
 *        new round.
 */
typedef struct proven_loop_accept {
    proven_u64 internal_[PROVEN_LOOP_ACCEPT_WORDS_];
} proven_loop_accept_t;

/** @return PROVEN_ERR_INVALID_ARG, PROVEN_ERR_NOMEM, or what opening the selector returned. */
[[nodiscard]]
proven_err_t proven_loop_create(proven_allocator_t alloc, proven_loop_t **out);

/** @brief How a loop does its work. AUTO: the completion port on Windows; io_uring on Linux 6.0
 *         or later where the kernel lets the process use it (since v0.94.0), else epoll; the
 *         selector (kqueue, poll) elsewhere. URING: io_uring, on Linux 5.19 or later where the
 *         kernel lets the process use it. SELECTOR: epoll, kqueue or poll, everywhere but
 *         Windows - what AUTO was on Linux before v0.94.0. */
typedef enum proven_loop_backend {
    PROVEN_LOOP_BACKEND_AUTO = 0,
    PROVEN_LOOP_BACKEND_SELECTOR = 1,
    PROVEN_LOOP_BACKEND_URING = 2,
    PROVEN_LOOP_BACKEND_IOCP = 3,
} proven_loop_backend_t;

/** @brief Options of proven_loop_create_ex; zero-initialised, they are proven_loop_create's. */
typedef struct proven_loop_options {
    proven_loop_backend_t backend;
    /** io_uring with Linux 6.0 or later: the pool of buffers the kernel fills for the
     *  connections read with proven_loop_read - how many (rounded up to a power of two, at most
     *  32768) and of how many bytes each (at most 65536). 0: 64 and 16384. Made at the first
     *  proven_loop_read; the other backends ignore both. */
    proven_size_t ring_buffers;
    proven_size_t ring_buffer_bytes;
} proven_loop_options_t;

/**
 * @brief proven_loop_create with options; `options` may be null. Everything else about the loop
 *        is the same whichever backend does the work.
 * @return PROVEN_ERR_UNSUPPORTED for a backend this build or this system does not have - io_uring
 *         off Linux, too old a kernel, or refused (a seccomp filter, io_uring_disabled); and
 *         proven_loop_create's errors.
 */
[[nodiscard]]
proven_err_t proven_loop_create_ex(proven_allocator_t alloc, const proven_loop_options_t *options, proven_loop_t **out);

/** @brief The backend a loop was made with: never AUTO. */
proven_loop_backend_t proven_loop_backend(const proven_loop_t *loop);

/**
 * @brief Free the loop. What has already ended gets its last answer now, so that its memory can
 *        go: `closed` of a socket given to proven_loop_close, and the PROVEN_ERR_CANCELLED of an
 *        accept that was cancelled - those functions must only free, not start anything on the
 *        loop. Whatever else is still attached, in flight or set is simply forgotten: nothing is
 *        called, and the bytes waiting to be sent are freed. On Windows and over io_uring what the
 *        system still holds - a receive, a send, an accept - is cancelled first and waited for, up
 *        to two seconds, so that it writes into no memory after the loop is gone. Null is accepted.
 */
void proven_loop_destroy(proven_loop_t *loop);

/**
 * @brief Run until proven_loop_stop: wait, call what is ready, fire what is due, repeat.
 * @return PROVEN_OK after a stop; an error if waiting itself failed.
 */
[[nodiscard]]
proven_err_t proven_loop_run(proven_loop_t *loop);

/**
 * @brief One round: wait until something is ready or `until` passes, and handle it.
 *        PROVEN_NET_DONT_WAIT handles only what is ready now. For a program that owns its
 *        own main loop.
 */
[[nodiscard]]
proven_err_t proven_loop_poll(proven_loop_t *loop, proven_net_deadline_t until);

/** @brief Make proven_loop_run return. Safe from any thread and from a callback. */
void proven_loop_stop(proven_loop_t *loop);

/**
 * @brief Have the loop call `fn(ctx)` on its own thread, soon. **Safe from any thread**: this
 *        is how work done elsewhere comes back. Functions are called in the order posted.
 *
 * The loop's allocator is used for the queue entry, so when other threads post it must be one
 * that is safe to call from several threads (the heap allocator is).
 *
 * @return PROVEN_ERR_INVALID_ARG, PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_err_t proven_loop_post(proven_loop_t *loop, proven_loop_fn fn, void *ctx);

/**
 * @brief Give a connection to the loop. Nothing is started: the connection is the loop's to
 *        watch from now until proven_loop_close and its `closed`. `fns` must stay valid as long.
 *
 * Reading and writing the connection yourself stays allowed - proven_net_read with
 * PROVEN_NET_DONT_WAIT after a receive of zero bytes is the usual way - but a send must go
 * through proven_loop_send once the connection is attached, so that bytes keep their order.
 * A connection is given to one loop, once: on Windows the socket is bound to the loop's
 * completion port for its life.
 *
 * @return PROVEN_ERR_INVALID_ARG for a null pointer or a connection that is not open.
 */
[[nodiscard]]
proven_err_t proven_loop_attach(proven_loop_t *loop, proven_loop_sock_t *sock, const proven_net_conn_t *conn,
                                const proven_loop_sock_fns_t *fns, void *ctx);

/**
 * @brief Begin connecting to `to`, with the new connection in `*out` attached to `sock`; its
 *        `connected` comes when the connect has ended.
 *
 * The socket is attached whatever the outcome: after a failed connect, as after any other end,
 * call proven_loop_close and close `*out` in `closed`. No receive or send may start before
 * `connected` has said PROVEN_OK (PROVEN_ERR_INVALID_STATE).
 *
 * @return as proven_net_connect_start for a connect that failed at once (nothing is attached
 *         and `*out` is not open); PROVEN_ERR_INVALID_ARG for a null pointer or a table without
 *         `connected`.
 */
[[nodiscard]]
proven_err_t proven_loop_connect(proven_loop_t *loop, proven_loop_sock_t *sock, proven_net_addr_t to, proven_net_conn_t *out,
                                 const proven_loop_sock_fns_t *fns, void *ctx);

/**
 * @brief Receive: `received` comes once, when data has arrived.
 *
 * With `buf.size` 0 nothing is taken: the call is "tell me when there is something to read",
 * and the connection holds no buffer while it waits - how ten thousand idle connections cost
 * no memory for their reads. Then read with proven_loop_read into proven_loop_scratch and keep
 * only what cannot be used yet. With a buffer, what arrived is in it; the buffer is the loop's
 * until `received`.
 *
 * @return PROVEN_ERR_BUSY when a receive is already in flight; PROVEN_ERR_INVALID_STATE after
 *         proven_loop_close or before a connect has ended; PROVEN_ERR_INVALID_ARG.
 */
[[nodiscard]]
proven_err_t proven_loop_recv(proven_loop_t *loop, proven_loop_sock_t *sock, proven_mem_mut_t buf);

/** @brief Stop a receive in flight: `received` comes with PROVEN_ERR_CANCELLED (or, on Windows,
 *         with data that reached a buffer first). Harmless when there is none. */
void proven_loop_recv_cancel(proven_loop_t *loop, proven_loop_sock_t *sock);

/**
 * @brief Read what has arrived, after a receive of zero bytes said there is something: the
 *        same as proven_net_read with PROVEN_NET_DONT_WAIT on the connection, and on epoll,
 *        kqueue, poll and Windows it is that call.
 *
 * On io_uring (Linux 6.0 or later) a connection read this way is received for by the kernel
 * into a pool of the loop's as data arrives, and this copies from there - no system call. An
 * idle connection still holds no buffer. Use it, not proven_net_read, for every read of an
 * attached connection: bytes the pool holds come before what the socket holds.
 *
 * @return `.value` bytes read; PROVEN_ERR_TIMEOUT when nothing is there now; PROVEN_ERR_EOF;
 *         the errors of proven_net_read; PROVEN_ERR_INVALID_STATE after proven_loop_close.
 */
[[nodiscard]]
proven_result_size_t proven_loop_read(proven_loop_t *loop, proven_loop_sock_t *sock, proven_mem_mut_t buf);

/**
 * @brief Send `bytes`: the loop takes what it can and the caller's bytes are free again when
 *        the call returns.
 *
 * On the selector and Windows' completion port, with nothing waiting the bytes go straight to the
 * socket, and only what it does not take at once is copied - into buffers of the loop's, given
 * back when sent. On io_uring every send is copied and given to the kernel in the loop's next
 * round, with the receives (so proven_loop_send_queued counts it until then); at
 * proven_loop_close what the loop has not yet given to the kernel is written to the socket first.
 * Bytes keep their order. The loop holds up to 64 KiB waiting per connection: past that, it
 * takes nothing more.
 *
 * @return `.value` the bytes taken; `.err` PROVEN_OK when all were, PROVEN_ERR_AGAIN when the
 *         rest did not fit - ask proven_loop_send_wait to be told when to give it again.
 *         After a send has failed, that failure (and nothing is taken). PROVEN_ERR_INVALID_STATE
 *         after proven_loop_close or before a connect has ended; PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_result_size_t proven_loop_send(proven_loop_t *loop, proven_loop_sock_t *sock, proven_mem_view_t bytes);

/** @brief How many bytes taken by proven_loop_send have not left yet. */
[[nodiscard]]
proven_size_t proven_loop_send_queued(const proven_loop_sock_t *sock);

/**
 * @brief Have `writable` called once when no more than `at_most` bytes are waiting: 0 asks to
 *        be told when everything has left (before a graceful close, say). Called from the loop,
 *        also when it is true already. Asking again replaces the earlier wait.
 * @return PROVEN_ERR_INVALID_STATE after proven_loop_close; PROVEN_ERR_INVALID_ARG for a
 *         table without `writable`.
 */
[[nodiscard]]
proven_err_t proven_loop_send_wait(proven_loop_t *loop, proven_loop_sock_t *sock, proven_size_t at_most);

/**
 * @brief The end of an attached socket. A receive in flight is answered with
 *        PROVEN_ERR_CANCELLED, so are a connect under way and a send wait; bytes still waiting
 *        to be sent are dropped. Then `closed`. The loop stops watching the connection before
 *        this returns, so the connection itself may be closed from now on. Harmless when the
 *        socket is already closing.
 */
void proven_loop_close(proven_loop_t *loop, proven_loop_sock_t *sock);

/**
 * @brief Accept one connection on `listener`: `fn` comes once, with it or with the reason there
 *        is none. Arm it again from inside `fn` to take the next.
 *
 * `acc` must have been zero-initialised before its first use. The listener must stay open while
 * an accept is in flight. Before closing it, call proven_loop_accept_cancel; after that, and
 * once an accept in flight has been answered, the memory of `acc` may go.
 *
 * @return PROVEN_ERR_BUSY when an accept is already in flight on `acc`; PROVEN_ERR_INVALID_ARG.
 */
[[nodiscard]]
proven_err_t proven_loop_accept(proven_loop_t *loop, proven_loop_accept_t *acc, proven_net_listener_t *listener,
                                proven_loop_accept_fn fn, void *ctx);

/** @brief Stop accepting: an accept in flight is answered with PROVEN_ERR_CANCELLED, and the
 *         loop forgets the listener, which may be closed when this returns. Harmless when
 *         nothing is in flight. */
void proven_loop_accept_cancel(proven_loop_t *loop, proven_loop_accept_t *acc);

/**
 * @brief Call `fn(ctx)` once, `ms` milliseconds from now: never earlier, and on POSIX within
 *        about a millisecond of it; on Windows at the system timer's interval (about 16 ms).
 *        Setting a timer that is already set moves it.
 */
void proven_loop_timer_set(proven_loop_t *loop, proven_loop_timer_t *timer, proven_u32 ms, proven_loop_fn fn, void *ctx);

/** @brief Forget a timer. Safe inside any callback, another timer's included; harmless when
 *         it is not set. */
void proven_loop_timer_cancel(proven_loop_t *loop, proven_loop_timer_t *timer);

/** @brief True while the timer is set and has not fired. */
bool proven_loop_timer_is_set(const proven_loop_timer_t *timer);

/**
 * @brief The loop's scratch buffer: 64 KiB that any callback may use while it runs.
 *
 * It is how thousands of connections share one read buffer: read into it, use what arrived,
 * and keep only what cannot be used yet. Its contents mean nothing after the callback returns
 * or calls anything that may itself use it.
 */
proven_mem_mut_t proven_loop_scratch(proven_loop_t *loop);

/** @brief The allocator the loop was made with. */
proven_allocator_t proven_loop_allocator(const proven_loop_t *loop);


#endif /* PROVEN_LOOP_H */
