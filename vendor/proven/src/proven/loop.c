#include "proven/loop.h"
#include "proven_internal_loop.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)

#include "proven/time.h"
#include "proven_internal_net.h"
#include "proven_internal_quic_net.h"
#include <stdatomic.h>

#if defined(_WIN32) || defined(_WIN64)
#define LOOP_IOCP 1
#else
#define LOOP_IOCP 0
#endif
#include <stddef.h>
/* The two ways of doing the operations: over a selector (POSIX), and as operations of a port
 * (Windows, and Linux over io_uring - RFC-0029). On Linux both are built and a loop is made one
 * or the other (proven_loop_create_ex); LOOP_CP(l) says which. */
#define LOOP_RING PROVEN_SYS_RING
#define LOOP_HAS_CP (LOOP_IOCP || LOOP_RING)
#define LOOP_HAS_SEL (!LOOP_IOCP)
#if LOOP_IOCP
#define LOOP_CP(l) ((void)(l), 1)
#elif LOOP_RING
#define LOOP_CP(l) ((l)->cp)
#else
#define LOOP_CP(l) ((void)(l), 0)
#endif
/* With both built, each way's calls are its own (static, named sel_ and cp_) and the public ones
 * choose; with one built, its calls are the public ones. */
#if LOOP_RING
#define LOOP_WAY static
#define SEL_(f) sel_##f
#define CP_(f) cp_##f
#else
#define LOOP_WAY
#define SEL_(f) f
#define CP_(f) f
#endif


/*
 * The event loop: a selector, a hashed timer wheel, a waker, and a queue other threads post to.
 *
 * Two things about its internals are worth knowing before changing it.
 *
 * Removal during dispatch. A callback may close any socket and cancel any timer, including
 * ones whose events were collected in the same round and have not been delivered. So closing
 * a socket blanks its remaining events in the current batch, and due timers are moved to a list
 * of their own before any is fired, from which a cancel can take them.
 *
 * The post queue. Posting is a push onto a lock-free stack from any thread; the loop takes the
 * whole stack at once and reverses it, which gives the order of posting back.
 *
 * The operations (RFC-0025). Here they are done over the selector: a receive waits for
 * readable, a send writes what goes and waits for writable for the rest, an accept waits for
 * readable on the listener. Interest is dropped lazily - a socket stays asked for readable after
 * its receive is answered, because the next receive usually follows at once, and is dropped only
 * when the selector reports it with nothing in flight - so a connection that is read and armed
 * again costs no change of its watch. What must not be answered inside the call that started it
 * (rule 1 of loop.h) goes on the "done" list, a list of sockets with the bits of what is waiting
 * to be delivered; the list is taken whole and delivered after each part of a round, and a round
 * with anything on it does not sleep. A socket's memory is the caller's again only after
 * `closed`, which is always delivered from that list, after the batch of events being delivered
 * - so an event already collected for a closing socket still points at valid memory.
 *
 * Bytes waiting to be sent are in chunks of PROVEN_LOOP_CHUNK_ bytes from a pool of the loop's,
 * a whole TLS record each; a socket holds chunks only while bytes wait, and one gather write
 * sends a socket's chain.
 *
 * On Windows the operations are those of an I/O completion port instead (RFC-0025 step 6): a
 * receive is an overlapped WSARecv - of zero bytes, or into the caller's buffer - a send one
 * WSASend of the socket's chain, an accept AcceptEx, a connect ConnectEx, a UDP receive
 * WSARecvMsg, and the waker a post to the port. What the system holds while an operation is in
 * flight - its OVERLAPPED, its buffer - is in the socket's own struct or in the chunk being sent,
 * and a socket's `closed` comes only once the port has reported every operation of it, so that
 * the system never writes memory its owner has freed. A report finds its node from the OVERLAPPED
 * alone: every struct that holds one has its node first and the OVERLAPPED right after it. The
 * done list, the chunks, the timers and the posts are the same on both.
 */

#define LOOP_WHEEL_SLOTS 1024u
#define LOOP_TICK_NS 16000000
#define LOOP_EVENTS 64u
#define LOOP_SCRATCH ((proven_size_t)64 * 1024)

#define TIMER_IDLE 0
#define TIMER_WHEEL 1
#define TIMER_DUE 2

/* What the selector's tags, and the done list, point at: the first byte tells which. A
 * the loop's own waker has a zero there. */
#define KIND_WAKER 0
#define KIND_SOCK 1
#define KIND_ACCEPT 2
#define KIND_UDP 3
#define KIND_CHUNK 4        /* a chunk's send, on a completion port */

/* A socket's state. */
#define S_NONE 0
#define S_OPEN 1
#define S_CONNECTING 2
#define S_DEAD 3            /* a connect that failed: only proven_loop_close remains */
#define S_CLOSING 4

/* flags */
#define F_QUEUED 0x01       /* on the done list */
#define F_REG 0x02          /* registered with the selector */
#define F_RECV 0x04         /* a receive (an accept) is in flight */
#define F_WAIT 0x08         /* a send wait is asked for */
#define F_OVR 0x10          /* completion port: the node's own OVERLAPPED is the system's (a receive, a connect, an accept) */
#define F_OVS 0x20          /* completion port: a send of the socket's chain is the system's */
#define F_SKIP 0x40         /* completion port: what finishes at once is not reported */
#define F_CANCEL 0x80       /* completion port: what is in flight was asked to stop */
/* On a completion port F_REG says the node is on the loop's list of every node (for destroy),
 * and `want` holds W_CONNECT while the node's OVERLAPPED is a connect's. On io_uring (RFC-0030)
 * it also says whether a socket is read with proven_loop_read, and so served by a multishot
 * receive into the loop's pool; whether its OVERLAPPED holds that receive's buffers and end; and
 * whether the receive is in flight. */
#define W_CONNECT 0x01
#define W_READER 0x02
#define W_MS 0x04
#define W_MSARM 0x08
/* Buffers a socket that nobody reads may hold before its multishot receive is stopped: one slow
 * reader does not take the pool from the others (and the kernel's window then holds the peer). */
#define LOOP_HELD_MAX 4u
#define LOOP_POOL_COUNT 64u
/* On io_uring every send goes through the ring (RFC-0030 D-3): the chain waits for the next entry
 * into the kernel instead of being offered to the socket first. (PROVEN_LOOP_SEND_FIRST_ keeps the
 * socket first: for measurement.) */
#if LOOP_RING && !defined(PROVEN_LOOP_SEND_FIRST_)
#define LOOP_RING_SENDS(l) LOOP_CP(l)
#else
#define LOOP_RING_SENDS(l) ((void)(l), 0)
#endif
#define LOOP_POOL_BYTES 16384u

/* done: what is waiting to be delivered */
#define D_CONNECTED 0x01
#define D_RECEIVED 0x02
#define D_WRITABLE 0x04
#define D_FLUSH 0x08        /* not a callback: committed bytes to be written */
#define D_ACCEPT 0x10
#define D_CLOSED 0x20

#define LOOP_SEND_LIMIT_K 64u
#define LOOP_POOL_KEEP 32u

typedef struct loop_node {
    proven_u8 kind, flags, want, done, state, family;
    proven_u16 err;                       /* the receive's answer, or the accept's */
    struct loop_node *next;               /* on the done list */
#if LOOP_IOCP
    struct loop_node *all_prev, *all_next;    /* every node with an end still to come, for destroy */
#endif
} loop_node_t;

typedef struct loop_sock loop_sock_t;

typedef struct loop_chunk {
#if LOOP_HAS_CP
    loop_node_t node;                     /* kind KIND_CHUNK: what the report of the chunk's send finds */
    proven_sys_net_ov_t ov;               /* the send of the chain that starts at this chunk */
    loop_sock_t *owner;
    proven_sys_net_keep_t keep;           /* what that send keeps while the system holds it */
#endif
    struct loop_chunk *next;              /* in its socket's chain, or in the pool */
    struct loop_chunk *all_prev, *all_next;    /* every chunk out of the pool, for destroy */
    struct loop_chunk *last;              /* in the first chunk of a socket's chain: its last */
    proven_u32 used, sent;
    proven_byte_t data[PROVEN_LOOP_CHUNK_];
} loop_chunk_t;

struct loop_sock {
    loop_node_t node;                     /* node.err: the answer of a receive, or of a connect */
#if LOOP_HAS_CP
    proven_sys_net_ov_t ov;               /* the receive's, or the connect's */
#endif
    proven_u16 send_err;                  /* stays once a send failed */
    proven_u16 limit_k;                   /* what proven_loop_send holds at most, in KiB; LIMIT_NONE: no bound */
    proven_u32 recv_cap;                  /* completion port: what a receive answered at once took */
    proven_u32 queued, wait_at;
    proven_uintptr_t handle;
    const proven_loop_sock_fns_t *fns;
    void *ctx;
    proven_byte_t *recv_buf;
    loop_chunk_t *head;                   /* bytes waiting to be sent; head->last is the chain's end */
};

#define LIMIT_NONE 0xffffu
static proven_u32 sock_limit(const loop_sock_t *s) { return s->limit_k == LIMIT_NONE ? 0xffffffffu : (proven_u32)s->limit_k * 1024u; }
static loop_chunk_t *sock_tail(const loop_sock_t *s) { return s->head ? s->head->last : (void *)0; }

typedef struct loop_acc {
    loop_node_t node;                     /* node.family: the listener's */
#if LOOP_HAS_CP
    proven_sys_net_ov_t ov;
#endif
    proven_uintptr_t handle;
    proven_net_listener_t *listener;
    proven_loop_accept_fn fn;
    void *ctx;
    proven_net_conn_t conn;               /* the accepted connection, until delivered (the socket made for an AcceptEx) */
#if LOOP_IOCP
    proven_byte_t area[PROVEN_SYS_NET_ACCEPT_AREA];
#endif
#if LOOP_RING
    proven_byte_t area[1];                /* io_uring's accept writes no address */
#endif
} loop_acc_t;

typedef struct loop_udp {
    loop_node_t node;                     /* node.err: the receive's answer */
#if LOOP_HAS_CP
    proven_sys_net_ov_t ov;
#endif
    proven_net_udp_t *udp;
    const proven_loop_udp_fns_t *fns;
    void *ctx;
    proven_byte_t *buf;
    proven_net_addr_t *from;
    proven_u32 cap, n, seg;               /* n, seg, ecn: an answer waiting on the done list */
    proven_u8 ecn;
#if LOOP_HAS_CP
    proven_sys_net_msg_t msg;
    proven_loop_timer_t wait;             /* a send wait's answer */
#endif
} loop_udp_t;

_Static_assert(sizeof(loop_udp_t) <= sizeof(proven_loop_udp_t), "proven_loop_udp_t holds the loop's UDP socket");
_Static_assert(_Alignof(loop_udp_t) <= _Alignof(proven_loop_udp_t), "and is aligned for it");
_Static_assert(sizeof(loop_sock_t) <= sizeof(proven_loop_sock_t), "proven_loop_sock_t holds the loop's socket");
_Static_assert(_Alignof(loop_sock_t) <= _Alignof(proven_loop_sock_t), "and is aligned for it");
_Static_assert(sizeof(loop_acc_t) <= sizeof(proven_loop_accept_t), "proven_loop_accept_t holds the loop's acceptor");
_Static_assert(_Alignof(loop_acc_t) <= _Alignof(proven_loop_accept_t), "and is aligned for it");
_Static_assert(PROVEN_LOOP_CHUNK_ >= 16384 + 5 + 16 + 1, "a chunk holds a whole TLS record");
#if LOOP_HAS_CP
#define LOOP_OV_AT offsetof(loop_sock_t, ov)
_Static_assert(offsetof(loop_acc_t, ov) == LOOP_OV_AT && offsetof(loop_udp_t, ov) == LOOP_OV_AT && offsetof(loop_chunk_t, ov) == LOOP_OV_AT,
               "every OVERLAPPED is at the same place after its node");
_Static_assert((int)PROVEN_NET_FAMILY_IPV4 == (int)PROVEN_SYS_NET_FAMILY_IPV4 && (int)PROVEN_NET_FAMILY_IPV6 == (int)PROVEN_SYS_NET_FAMILY_IPV6 &&
               (int)PROVEN_NET_FAMILY_UNIX == (int)PROVEN_SYS_NET_FAMILY_UNIX, "a listener's family is the platform's");
#endif

typedef struct loop_task {
    struct loop_task *next;
    proven_loop_fn fn;
    void *ctx;
} proven_loop_task_t;

struct proven_loop {
    proven_allocator_t alloc;
#if LOOP_RING
    bool cp;                                      /* the operations are the port's: io_uring */
    proven_u8 rbuf_state;                         /* RFC-0030, the pool of receive buffers: 0 not asked for yet, 1 made, 2 none */
    proven_u32 rbuf_count, rbuf_bytes;            /* what it is to be */
#endif
#if LOOP_HAS_CP
    proven_uintptr_t port;
    bool port_open;
    atomic_bool woken;                            /* a post to the port is on its way */
    loop_node_t *all;                             /* every node with an end still to come */
    proven_size_t in_flight;                      /* operations the system holds */
    proven_sys_net_done_t cp_batch[LOOP_EVENTS];  /* the reports of the round being delivered */
#endif
#if LOOP_HAS_SEL
    proven_net_selector_t *selector;
    proven_net_waker_t waker;
    bool waker_open;
    loop_node_t waker_node;                       /* the waker's registration: readable means "look at the posts" */
    proven_net_ready_t batch[LOOP_EVENTS];        /* the events of the round being delivered */
    proven_size_t batch_count, batch_pos;
#endif
    _Atomic(proven_loop_task_t *) tasks;
    atomic_bool stop;
    proven_loop_timer_t *wheel[LOOP_WHEEL_SLOTS];
    proven_loop_timer_t *due;                     /* timers taken off the wheel, not yet fired */
    proven_u64 wheel_tick;
    proven_size_t timed_count;
    proven_byte_t *scratch;
    proven_loop_after_t *after_first, *after_last;    /* "after this round", in the order asked */
    proven_size_t after_count;
    proven_loop_clock_fn clock;                   /* tests only: null is the monotonic clock */
    void *clock_ctx;
    loop_node_t *done_first, *done_last;          /* what waits to be delivered */
    loop_chunk_t *pool;                           /* free chunks, at most LOOP_POOL_KEEP */
    proven_size_t pool_count;
    loop_chunk_t *chunks;                         /* every chunk out of the pool */
    loop_acc_t *delivering;                       /* the acceptor whose function runs now */
    loop_udp_t *delivering_udp;                   /* the UDP socket whose `received` runs now */
};

/* The one place the loop reads the time. */
static proven_time_t loop_now(const proven_loop_t *l) { return l->clock ? l->clock(l->clock_ctx) : proven_time_monotonic_now(); }

void proven_loop_test_clock_(proven_loop_t *l, proven_loop_clock_fn clock, void *ctx) {
    if (!l) return;
    l->clock = clock;
    l->clock_ctx = ctx;
    l->wheel_tick = (proven_u64)loop_now(l) / LOOP_TICK_NS;
}

#if LOOP_HAS_CP
static void loop_drain(proven_loop_t *l);
#endif

/* The port: Windows' completion port, or Linux's ring. */
#if LOOP_HAS_CP
static proven_err_t loop_open_port(proven_loop_t *l) {
    atomic_init(&l->woken, false);
    proven_err_t e = proven_net_err_from_sys_(proven_sys_net_port_open(&l->port));
    l->port_open = e == PROVEN_OK;
    return e;
}
#endif

/* The selector and its waker. */
#if LOOP_HAS_SEL
static proven_err_t loop_open_selector(proven_loop_t *l, bool poll_kind) {
    proven_err_t e = poll_kind ? proven_net_selector_create_poll(l->alloc, &l->selector) : proven_net_selector_create(l->alloc, &l->selector);
    if (e == PROVEN_OK) {
        e = proven_net_waker_open(&l->waker);
        l->waker_open = e == PROVEN_OK;
    }
    if (e == PROVEN_OK) {
        e = proven_net_selector_add(l->selector, proven_net_waker_handle(&l->waker), PROVEN_NET_READABLE, &l->waker_node);
        if (e == PROVEN_OK) l->waker_node.flags |= F_REG;
    }
    return e;
}
#endif

/* Which backend `asked` makes here: AUTO is the port on Windows; on Linux io_uring where the
 * kernel has all RFC-0030 saves calls with (6.0, and not refused) - the owner's D-4 of RFC-0030,
 * once the ring had run under ASan and TSan - and the selector otherwise and elsewhere. A build
 * made with PROVEN_LOOP_AUTO_URING_ takes io_uring wherever it works at all (5.19): how the suite
 * runs on it in an emulated machine. */
static proven_loop_backend_t loop_pick(proven_loop_backend_t asked) {
    if (asked != PROVEN_LOOP_BACKEND_AUTO) return asked;
#if LOOP_IOCP
    return PROVEN_LOOP_BACKEND_IOCP;
#elif LOOP_RING && defined(PROVEN_LOOP_AUTO_URING_)
    return proven_sys_net_port_available() ? PROVEN_LOOP_BACKEND_URING : PROVEN_LOOP_BACKEND_SELECTOR;
#elif LOOP_RING
    return proven_sys_net_port_preferred() ? PROVEN_LOOP_BACKEND_URING : PROVEN_LOOP_BACKEND_SELECTOR;
#else
    return PROVEN_LOOP_BACKEND_SELECTOR;
#endif
}

static proven_err_t loop_create(proven_allocator_t alloc, proven_loop_t **out, const proven_loop_options_t *o, bool poll_kind) {
    if (out) *out = (void *)0;
    const proven_loop_backend_t asked = o ? o->backend : PROVEN_LOOP_BACKEND_AUTO;
    if (!out || !proven_alloc_is_valid(alloc) || (unsigned)asked > (unsigned)PROVEN_LOOP_BACKEND_IOCP) return PROVEN_ERR_INVALID_ARG;
    proven_size_t pool_count = o && o->ring_buffers ? o->ring_buffers : LOOP_POOL_COUNT;
    const proven_size_t pool_bytes = o && o->ring_buffer_bytes ? o->ring_buffer_bytes : LOOP_POOL_BYTES;
    if (pool_count > 32768u || pool_bytes > 65536u) return PROVEN_ERR_INVALID_ARG;
    proven_size_t round = 1;
    while (round < pool_count) round *= 2;
    pool_count = round;
    const proven_loop_backend_t kind = loop_pick(asked);
#if LOOP_IOCP
    if (kind != PROVEN_LOOP_BACKEND_IOCP) return PROVEN_ERR_UNSUPPORTED;
#elif LOOP_RING
    if (kind == PROVEN_LOOP_BACKEND_IOCP) return PROVEN_ERR_UNSUPPORTED;
    if (kind == PROVEN_LOOP_BACKEND_URING && !proven_sys_net_port_available()) return PROVEN_ERR_UNSUPPORTED;
#else
    if (kind != PROVEN_LOOP_BACKEND_SELECTOR) return PROVEN_ERR_UNSUPPORTED;
#endif
    proven_result_mem_mut_t m = alloc.alloc_fn(alloc.ctx, sizeof(proven_loop_t), 16);
    if (m.err != PROVEN_OK) return m.err;
    proven_loop_t *l = (proven_loop_t *)m.value.ptr;
    for (proven_size_t i = 0; i < sizeof *l; ++i) ((proven_byte_t *)l)[i] = 0;
    l->alloc = alloc;
    atomic_init(&l->tasks, (proven_loop_task_t *)0);
    atomic_init(&l->stop, false);
    l->wheel_tick = (proven_u64)loop_now(l) / LOOP_TICK_NS;
#if LOOP_RING
    l->cp = kind == PROVEN_LOOP_BACKEND_URING;
    l->rbuf_count = (proven_u32)pool_count;
    l->rbuf_bytes = (proven_u32)pool_bytes;
#else
    (void)pool_count; (void)pool_bytes;
#endif
    proven_err_t e = PROVEN_OK;
#if LOOP_HAS_CP
    if (LOOP_CP(l)) e = loop_open_port(l);
#endif
#if LOOP_HAS_SEL
    if (!LOOP_CP(l)) e = loop_open_selector(l, poll_kind);
#else
    (void)poll_kind;
#endif
    if (e == PROVEN_OK) {
        m = alloc.alloc_fn(alloc.ctx, LOOP_SCRATCH, 16);
        if (m.err != PROVEN_OK) e = m.err;
        else l->scratch = m.value.ptr;
    }
    if (e != PROVEN_OK) { proven_loop_destroy(l); return e; }
    *out = l;
    return PROVEN_OK;
}

proven_err_t proven_loop_create(proven_allocator_t alloc, proven_loop_t **out) { return loop_create(alloc, out, (void *)0, false); }
proven_err_t proven_loop_create_poll_(proven_allocator_t alloc, proven_loop_t **out) {
#if LOOP_IOCP
    const proven_loop_options_t o = { .backend = PROVEN_LOOP_BACKEND_IOCP };
#else
    const proven_loop_options_t o = { .backend = PROVEN_LOOP_BACKEND_SELECTOR };
#endif
    return loop_create(alloc, out, &o, true);
}

proven_err_t proven_loop_create_ex(proven_allocator_t alloc, const proven_loop_options_t *options, proven_loop_t **out) {
    return loop_create(alloc, out, options, false);
}

proven_loop_backend_t proven_loop_backend(const proven_loop_t *l) {
#if LOOP_IOCP
    (void)l;
    return PROVEN_LOOP_BACKEND_IOCP;
#else
    return l && LOOP_CP(l) ? PROVEN_LOOP_BACKEND_URING : PROVEN_LOOP_BACKEND_SELECTOR;
#endif
}

#if LOOP_HAS_SEL
/* What has already ended gets its last answer, so that its owner can free it: `closed` of a
 * socket that was closed, and an accept that was cancelled. Nothing else is called. */
static void loop_sel_last(proven_loop_t *l) {
    for (loop_node_t *n = l->done_first, *next; n; n = next) {
        next = n->next;
        if (n->kind == KIND_SOCK && (n->done & D_CLOSED) && n->state == S_CLOSING) {
            loop_sock_t *s = (loop_sock_t *)n;
            s->node.state = S_NONE;
            if (s->fns->closed) s->fns->closed(s->ctx);
        } else if (n->kind == KIND_UDP && (n->done & D_CLOSED) && n->state == S_CLOSING) {
            loop_udp_t *u = (loop_udp_t *)n;
            u->node.state = S_NONE;
            if (u->fns->closed) u->fns->closed(u->ctx);
        } else if (n->kind == KIND_ACCEPT && (n->done & D_ACCEPT) && n->err == (proven_u16)PROVEN_ERR_CANCELLED) {
            loop_acc_t *acc = (loop_acc_t *)n;
            acc->node.flags &= (proven_u8)~F_RECV;
            proven_net_conn_t none = { 0 };
            acc->fn(acc->ctx, PROVEN_ERR_CANCELLED, &none);
        }
    }
}
#endif

void proven_loop_destroy(proven_loop_t *l) {
    if (!l) return;
    proven_allocator_t a = l->alloc;
#if LOOP_HAS_CP
    if (LOOP_CP(l) && l->port_open) loop_drain(l);
#endif
#if LOOP_HAS_SEL
    if (!LOOP_CP(l)) loop_sel_last(l);
#endif
    l->done_first = l->done_last = (void *)0;
    proven_loop_task_t *t = atomic_exchange_explicit(&l->tasks, (proven_loop_task_t *)0, memory_order_acquire);
    while (t) { proven_loop_task_t *next = t->next; a.free_fn(a.ctx, t); t = next; }
#if LOOP_HAS_CP
    if (LOOP_CP(l) && l->port_open) proven_sys_net_port_close(l->port);
#endif
#if LOOP_HAS_SEL
    if (l->waker_open) {
        if (l->waker_node.flags & F_REG) (void)proven_net_selector_remove(l->selector, proven_net_waker_handle(&l->waker));
        proven_net_waker_close(&l->waker);
    }
    if (l->selector) proven_net_selector_destroy(l->selector);
#endif
    if (l->scratch) a.free_fn(a.ctx, l->scratch);
    while (l->pool) { loop_chunk_t *next = l->pool->next; a.free_fn(a.ctx, l->pool); l->pool = next; }
    /* A send the port never reported in the drain's time may still read its chunks: they stay. */
    bool keep_chunks = false;
#if LOOP_HAS_CP
    keep_chunks = LOOP_CP(l) && l->in_flight > 0;
#endif
    if (!keep_chunks)
        while (l->chunks) { loop_chunk_t *next = l->chunks->all_next; a.free_fn(a.ctx, l->chunks); l->chunks = next; }
    a.free_fn(a.ctx, l);
}

proven_mem_mut_t proven_loop_scratch(proven_loop_t *l) {
    if (!l) return (proven_mem_mut_t){ .ptr = (void *)0, .size = 0 };
    return (proven_mem_mut_t){ .ptr = l->scratch, .size = LOOP_SCRATCH };
}

proven_allocator_t proven_loop_allocator(const proven_loop_t *l) { return l->alloc; }

// -----------------------------------------------------------------------------
// Operations (RFC-0025), over the selector
// -----------------------------------------------------------------------------

static loop_sock_t *sock_of(proven_loop_sock_t *s) { return (loop_sock_t *)(void *)s; }
static loop_acc_t *acc_of(proven_loop_accept_t *a) { return (loop_acc_t *)(void *)a; }

static proven_net_conn_t sock_conn(const loop_sock_t *s) {
    proven_net_conn_t c = { 0 };
    c.internal.handle = s->handle;
    c.internal.open = true;
    c.internal.family = s->node.family;
    return c;
}

#if LOOP_HAS_SEL
/* Ask the selector for `want` of a node's socket, registering it on first need. */
static proven_err_t node_watch(proven_loop_t *l, loop_node_t *n, proven_uintptr_t handle, proven_u8 want) {
    proven_net_handle_t h = { .raw = handle, .valid = true };
    proven_err_t e = PROVEN_OK;
    if (n->flags & F_REG) {
        if (n->want == want) return PROVEN_OK;
        e = proven_net_selector_modify(l->selector, h, want, n);
    } else {
        if (want == 0) return PROVEN_OK;
        e = proven_net_selector_add(l->selector, h, want, n);
        if (e == PROVEN_OK) n->flags |= F_REG;
    }
    if (e == PROVEN_OK) n->want = want;
    return e;
}

/* Forget a node's socket: nothing more from the selector, not even an event of this batch. */
static void node_unwatch(proven_loop_t *l, loop_node_t *n, proven_uintptr_t handle) {
    if (n->flags & F_REG) (void)proven_net_selector_remove(l->selector, (proven_net_handle_t){ .raw = handle, .valid = true });
    n->flags &= (proven_u8)~F_REG;
    n->want = 0;
    for (proven_size_t i = l->batch_pos; i < l->batch_count; ++i) {
        if (l->batch[i].tag == n) l->batch[i].tag = (void *)0;
    }
}
#endif


static void node_queue(proven_loop_t *l, loop_node_t *n, proven_u8 bits) {
    n->done |= bits;
    if (n->flags & F_QUEUED) return;
    n->flags |= F_QUEUED;
    n->next = (void *)0;
    if (l->done_last) l->done_last->next = n; else l->done_first = n;
    l->done_last = n;
}

#if LOOP_HAS_CP
/* On Windows the loop keeps every node with an end still to come, to cancel at destroy; io_uring
 * cancels everything of the ring at once, and needs no list. */
static void node_link(proven_loop_t *l, loop_node_t *n) {
#if LOOP_IOCP
    if (n->flags & F_REG) return;
    n->flags |= F_REG;
    n->all_prev = (void *)0;
    n->all_next = l->all;
    if (l->all) l->all->all_prev = n;
    l->all = n;
#else
    (void)l; (void)n;
#endif
}

static void node_unlink(proven_loop_t *l, loop_node_t *n) {
#if LOOP_IOCP
    if (!(n->flags & F_REG)) return;
    n->flags &= (proven_u8)~F_REG;
    if (n->all_prev) n->all_prev->all_next = n->all_next; else l->all = n->all_next;
    if (n->all_next) n->all_next->all_prev = n->all_prev;
    n->all_prev = n->all_next = (void *)0;
#else
    (void)l; (void)n;
#endif
}
#endif

// --- chunks

static loop_chunk_t *chunk_take(proven_loop_t *l) {
    loop_chunk_t *c = l->pool;
    if (c) { l->pool = c->next; l->pool_count--; }
    else {
        proven_result_mem_mut_t m = l->alloc.alloc_fn(l->alloc.ctx, sizeof(loop_chunk_t), 16);
        if (m.err != PROVEN_OK) return (void *)0;
        c = (loop_chunk_t *)m.value.ptr;
    }
    c->next = (void *)0;
    c->used = c->sent = 0;
#if LOOP_HAS_CP
    c->node.kind = KIND_CHUNK;
    c->owner = (void *)0;
#endif
    c->all_prev = (void *)0;
    c->all_next = l->chunks;
    if (l->chunks) l->chunks->all_prev = c;
    l->chunks = c;
    return c;
}

static void chunk_give(proven_loop_t *l, loop_chunk_t *c) {
    if (c->all_prev) c->all_prev->all_next = c->all_next; else l->chunks = c->all_next;
    if (c->all_next) c->all_next->all_prev = c->all_prev;
    if (l->pool_count >= LOOP_POOL_KEEP) { l->alloc.free_fn(l->alloc.ctx, c); return; }
    c->next = l->pool;
    l->pool = c;
    l->pool_count++;
}

static void sock_drop_chunks(proven_loop_t *l, loop_sock_t *s) {
    while (s->head) { loop_chunk_t *next = s->head->next; chunk_give(l, s->head); s->head = next; }
    s->queued = 0;
}

/* Room for `min` bytes at the end of the socket's chain. */
static proven_err_t sock_room(proven_loop_t *l, loop_sock_t *s, proven_size_t min, proven_mem_mut_t *room) {
    if (min == 0) min = 1;
    loop_chunk_t *tail = sock_tail(s);
    if (!tail || PROVEN_LOOP_CHUNK_ - tail->used < min) {
        loop_chunk_t *c = chunk_take(l);
        if (!c) return PROVEN_ERR_NOMEM;
        if (tail) tail->next = c; else s->head = c;
        s->head->last = tail = c;
    }
    *room = (proven_mem_mut_t){ .ptr = tail->data + tail->used, .size = PROVEN_LOOP_CHUNK_ - tail->used };
    return PROVEN_OK;
}

// --- delivery

static void sock_deliver_writable(loop_sock_t *s, proven_err_t err) {
    if (s->fns->writable) s->fns->writable(s->ctx, err);
}
#if LOOP_HAS_CP
static void CP_(sock_flush)(proven_loop_t *l, loop_sock_t *s, bool answer);
#endif
#if LOOP_HAS_SEL

/* Write what waits; answer a wait that has come true, or the failure - from the loop. With
 * `answer` false (a driver's own call) the answer is queued to come from the loop instead. */
static void SEL_(sock_flush)(proven_loop_t *l, loop_sock_t *s, bool answer) {
    proven_err_t e = PROVEN_OK;
    for (int pass = 0; pass < 4 && s->head; ++pass) {
        proven_mem_view_t vec[PROVEN_NET_VEC_MAX];
        proven_size_t count = 0, offered = 0;
        for (loop_chunk_t *c = s->head; c && count < PROVEN_NET_VEC_MAX; c = c->next) {
            if (c->used == c->sent) continue;
            vec[count++] = (proven_mem_view_t){ .ptr = c->data + c->sent, .size = c->used - c->sent };
            offered += c->used - c->sent;
        }
        proven_size_t sent = 0;
        proven_net_conn_t conn = sock_conn(s);
        if (offered > 0 && count == 1) {                /* one piece: a plain send is cheaper than a gather write */
            proven_result_size_t w = proven_net_write(&conn, vec[0], PROVEN_NET_DONT_WAIT);
            e = w.err;
            sent = w.value;
        } else if (offered > 0) {
            e = proven_net_write_vec_(&conn, vec, count, &sent);
        }
        s->queued -= (proven_u32)sent;
        while (s->head) {
            loop_chunk_t *c = s->head;
            proven_size_t here = c->used - c->sent;
            if (sent < here) { c->sent += (proven_u32)sent; break; }
            sent -= here;
            s->head = c->next;
            if (s->head) s->head->last = c->last;
            chunk_give(l, c);
        }
        if (e != PROVEN_OK || offered == 0 || s->queued > 0) break;
    }
    if (e != PROVEN_OK && e != PROVEN_ERR_TIMEOUT) {
        s->send_err = (proven_u16)e;
        sock_drop_chunks(l, s);
        (void)node_watch(l, &s->node, s->handle, (proven_u8)(s->node.want & ~PROVEN_NET_WRITABLE));
        s->node.flags &= (proven_u8)~F_WAIT;
        if (answer) sock_deliver_writable(s, e); else node_queue(l, &s->node, D_WRITABLE);
        return;
    }
    proven_u8 want = s->queued > 0 ? (proven_u8)(s->node.want | PROVEN_NET_WRITABLE) : (proven_u8)(s->node.want & ~PROVEN_NET_WRITABLE);
    (void)node_watch(l, &s->node, s->handle, want);
    if ((s->node.flags & F_WAIT) && s->queued <= s->wait_at) {
        s->node.flags &= (proven_u8)~F_WAIT;
        if (answer) sock_deliver_writable(s, PROVEN_OK); else node_queue(l, &s->node, D_WRITABLE);
    }
}

#endif

#if LOOP_RING
static void sock_flush(proven_loop_t *l, loop_sock_t *s, bool answer) {
    if (LOOP_CP(l)) cp_sock_flush(l, s, answer); else sel_sock_flush(l, s, answer);
}
#endif

/* Whether a send of the chain is under way: on a port, one is in flight; over a selector, the
 * selector is asked for writable. */
static bool sock_sending(const proven_loop_t *l, const loop_sock_t *s) {
    return LOOP_CP(l) ? (s->node.flags & F_OVS) != 0 : (s->node.want & PROVEN_NET_WRITABLE) != 0;
}

static void sock_connected(loop_sock_t *s, proven_err_t err) {
    if (s->node.state == S_CONNECTING) s->node.state = err == PROVEN_OK ? S_OPEN : S_DEAD;
    s->fns->connected(s->ctx, err);
}

/* What the done list holds for a socket, in this order; bits set meanwhile wait for the next
 * time. `closed` comes alone at the end: after it the memory is the caller's. */
static void sock_deliver(proven_loop_t *l, loop_sock_t *s) {
    proven_u8 bits = s->node.done;
    s->node.done = 0;
    /* Deferred, a receive is answered only when cancelled: nothing taken. */
    if (bits & D_CONNECTED) sock_connected(s, (proven_err_t)s->node.err);
    /* On a port, a receive answered at once (or failed at once), with what it took. */
    if ((bits & D_RECEIVED) && LOOP_CP(l)) {
        /* F_CANCEL stays while a multishot receive is in flight: it is that receive's stop. */
        s->node.flags &= (proven_u8)~(F_RECV | ((s->node.want & W_MSARM) ? 0 : F_CANCEL));
        const proven_err_t e = (proven_err_t)s->node.err;
        s->fns->received(s->ctx, e, e == PROVEN_OK ? s->recv_cap : 0);
    } else if (bits & D_RECEIVED) s->fns->received(s->ctx, (proven_err_t)s->node.err, 0);
    if (bits & D_WRITABLE) {
        proven_err_t e = s->node.state == S_CLOSING ? PROVEN_ERR_CANCELLED : (proven_err_t)s->send_err;
        sock_deliver_writable(s, e);
    }
    if ((bits & D_FLUSH) && s->node.state == S_OPEN && s->head && !sock_sending(l, s)) sock_flush(l, s, true);
    if ((bits & D_CLOSED) && s->node.state == S_CLOSING) {
        s->node.state = S_NONE;
#if LOOP_HAS_CP
        if (LOOP_CP(l)) node_unlink(l, &s->node);
#endif
        if (s->fns->closed) s->fns->closed(s->ctx);
    }
}

static void acc_deliver(proven_loop_t *l, loop_acc_t *a) {
    a->node.done = 0;
    a->node.flags &= (proven_u8)~(F_RECV | F_CANCEL);
#if LOOP_HAS_CP
    if (LOOP_CP(l)) node_unlink(l, &a->node);
#endif
    proven_err_t err = (proven_err_t)a->node.err;
    proven_net_conn_t conn = a->conn;
    a->conn = (proven_net_conn_t){ 0 };
    l->delivering = a;
    a->fn(a->ctx, err, &conn);
    if (l->delivering == a) l->delivering = (void *)0;
}

static void udp_deliver(proven_loop_t *l, loop_udp_t *u);

/* Deliver what is on the done list now, and what that queues in turn - an accept or a receive
 * armed again from inside its own function, which took what was waiting already - up to
 * LOOP_DONE_PASSES passes: so many connections waiting, or datagrams, are taken in one go as
 * a drain of a ready socket would, and the selector is still looked at between goes. */
#define LOOP_DONE_PASSES 64
static void loop_run_done(proven_loop_t *l) {
    for (int pass = 0; pass < LOOP_DONE_PASSES && l->done_first; ++pass) {
        loop_node_t *n = l->done_first;
        l->done_first = l->done_last = (void *)0;
        while (n) {
            loop_node_t *next = n->next;
            n->flags &= (proven_u8)~F_QUEUED;
            if (n->kind == KIND_SOCK) sock_deliver(l, (loop_sock_t *)n);
            else if (n->kind == KIND_ACCEPT && (n->done & D_ACCEPT)) acc_deliver(l, (loop_acc_t *)n);
            else if (n->kind == KIND_UDP) udp_deliver(l, (loop_udp_t *)n);
            n = next;
        }
    }
}


static void udp_received(proven_loop_t *l, loop_udp_t *u, proven_err_t err, proven_size_t n, proven_size_t seg, proven_u8 ecn) {
    u->node.flags &= (proven_u8)~(F_RECV | F_CANCEL);
    l->delivering_udp = u;
    u->fns->received(u->ctx, err, n, seg, ecn);
    if (l->delivering_udp == u) l->delivering_udp = (void *)0;
}

static void udp_deliver(proven_loop_t *l, loop_udp_t *u) {
    proven_u8 bits = u->node.done;
    u->node.done = 0;
    if (bits & D_RECEIVED) udp_received(l, u, (proven_err_t)u->node.err, u->n, u->seg, u->ecn);
    if ((bits & D_WRITABLE) && u->fns->writable) u->fns->writable(u->ctx, u->node.state == S_CLOSING ? PROVEN_ERR_CANCELLED : PROVEN_OK);
    if ((bits & D_CLOSED) && u->node.state == S_CLOSING) {
        u->node.state = S_NONE;
#if LOOP_HAS_CP
        if (LOOP_CP(l)) node_unlink(l, &u->node);
#endif
        if (u->fns->closed) u->fns->closed(u->ctx);
    }
}
#if LOOP_HAS_SEL


/* One receive now. True when there is an answer: datagrams, or nothing worth waiting on. An
 * oversized datagram (none of QUIC's) and a reported error of an earlier send are passed over. */
static bool udp_try(loop_udp_t *u, proven_err_t *err, proven_size_t *n, proven_size_t *seg, proven_u8 *ecn) {
    for (int i = 0; i < 4; ++i) {
        proven_size_t s = 0;
        proven_result_size_t r = proven_net_udp_recv_marked_(u->udp, (proven_mem_mut_t){ .ptr = u->buf, .size = u->cap }, &s, ecn, u->from);
        if (r.err == PROVEN_ERR_TIMEOUT) return false;
        if (r.err != PROVEN_OK) continue;
        *err = PROVEN_OK; *n = r.value; *seg = s;
        return true;
    }
    return false;
}

static void udp_event(proven_loop_t *l, loop_udp_t *u, proven_u8 got) {
    if (u->node.state != S_OPEN) return;
    if (got & PROVEN_NET_WRITABLE) {
        if (u->node.flags & F_WAIT) {
            u->node.flags &= (proven_u8)~F_WAIT;
            (void)node_watch(l, &u->node, (proven_uintptr_t)u->udp->internal.handle, (proven_u8)(u->node.want & ~PROVEN_NET_WRITABLE));
            if (u->fns->writable) u->fns->writable(u->ctx, PROVEN_OK);
        } else if (u->node.want & PROVEN_NET_WRITABLE) {
            (void)node_watch(l, &u->node, (proven_uintptr_t)u->udp->internal.handle, (proven_u8)(u->node.want & ~PROVEN_NET_WRITABLE));
        }
    }
    if (u->node.state != S_OPEN) return;
    if (got & (PROVEN_NET_READABLE | PROVEN_NET_FAILED)) {
        if (!(u->node.flags & F_RECV)) {
            if (u->node.want & PROVEN_NET_READABLE) (void)node_watch(l, &u->node, (proven_uintptr_t)u->udp->internal.handle, (proven_u8)(u->node.want & ~PROVEN_NET_READABLE));
            return;
        }
        if (u->node.done & D_RECEIVED) return;
        proven_err_t err = PROVEN_OK;
        proven_size_t n = 0, seg = 0;
        proven_u8 ecn = 0;
        if (udp_try(u, &err, &n, &seg, &ecn)) udp_received(l, u, err, n, seg, ecn);
    }
}

LOOP_WAY proven_err_t SEL_(proven_loop_udp_attach_)(proven_loop_t *l, proven_loop_udp_t *udp_op, proven_net_udp_t *udp, const proven_loop_udp_fns_t *fns, void *ctx) {
    if (!l || !udp_op || !udp || !fns || !fns->received || !proven_net_udp_is_open(udp)) return PROVEN_ERR_INVALID_ARG;
    loop_udp_t *u = (loop_udp_t *)(void *)udp_op;
    for (proven_size_t i = 0; i < sizeof *u; ++i) ((proven_byte_t *)u)[i] = 0;
    u->node.kind = KIND_UDP;
    u->node.state = S_OPEN;
    u->udp = udp;
    u->fns = fns;
    u->ctx = ctx;
    return PROVEN_OK;
}

LOOP_WAY proven_err_t SEL_(proven_loop_udp_recv_)(proven_loop_t *l, proven_loop_udp_t *udp_op, proven_mem_mut_t buf, proven_net_addr_t *from) {
    if (!l || !udp_op || !buf.ptr || buf.size == 0 || !from) return PROVEN_ERR_INVALID_ARG;
    loop_udp_t *u = (loop_udp_t *)(void *)udp_op;
    if (u->node.kind != KIND_UDP || u->node.state != S_OPEN) return PROVEN_ERR_INVALID_STATE;
    if (u->node.flags & F_RECV) return PROVEN_ERR_BUSY;
    u->buf = buf.ptr;
    u->cap = buf.size > 0x7fffffffu ? 0x7fffffffu : (proven_u32)buf.size;
    u->from = from;
    /* Armed again from inside its own `received`: datagrams are often waiting already, and
     * taking them now saves a round. Still answered from the loop. */
    if (l->delivering_udp == u) {
        proven_err_t err = PROVEN_OK;
        proven_size_t n = 0, seg = 0;
        proven_u8 ecn = 0;
        if (udp_try(u, &err, &n, &seg, &ecn)) {
            u->node.flags |= F_RECV;
            u->node.err = (proven_u16)err;
            u->n = (proven_u32)n;
            u->seg = (proven_u32)seg;
            u->ecn = ecn;
            node_queue(l, &u->node, D_RECEIVED);
            return PROVEN_OK;
        }
    }
    proven_err_t e = node_watch(l, &u->node, (proven_uintptr_t)u->udp->internal.handle, (proven_u8)(u->node.want | PROVEN_NET_READABLE));
    if (e != PROVEN_OK) return e;
    u->node.flags |= F_RECV;
    return PROVEN_OK;
}

LOOP_WAY proven_err_t SEL_(proven_loop_udp_send_wait_)(proven_loop_t *l, proven_loop_udp_t *udp_op) {
    if (!l || !udp_op) return PROVEN_ERR_INVALID_ARG;
    loop_udp_t *u = (loop_udp_t *)(void *)udp_op;
    if (u->node.kind != KIND_UDP || u->node.state != S_OPEN) return PROVEN_ERR_INVALID_STATE;
    if (u->node.flags & F_WAIT) return PROVEN_OK;
    proven_err_t e = node_watch(l, &u->node, (proven_uintptr_t)u->udp->internal.handle, (proven_u8)(u->node.want | PROVEN_NET_WRITABLE));
    if (e == PROVEN_OK) u->node.flags |= F_WAIT;
    return e;
}

LOOP_WAY void SEL_(proven_loop_udp_close_)(proven_loop_t *l, proven_loop_udp_t *udp_op) {
    if (!l || !udp_op) return;
    loop_udp_t *u = (loop_udp_t *)(void *)udp_op;
    if (u->node.kind != KIND_UDP || u->node.state != S_OPEN) return;
    proven_u8 bits = D_CLOSED;
    if (u->node.flags & F_RECV) {
        if (!(u->node.done & D_RECEIVED)) { u->node.err = (proven_u16)PROVEN_ERR_CANCELLED; u->n = 0; u->seg = 0; u->ecn = 0; }
        else u->node.err = (proven_u16)PROVEN_ERR_CANCELLED;      /* datagrams taken and not delivered are dropped */
        bits |= D_RECEIVED;
    }
    if (u->node.flags & F_WAIT) { u->node.flags &= (proven_u8)~F_WAIT; bits |= D_WRITABLE; }
    node_unwatch(l, &u->node, (proven_uintptr_t)u->udp->internal.handle);
    u->node.state = S_CLOSING;
    node_queue(l, &u->node, bits);
}

// --- what the selector reports

static void sock_event(proven_loop_t *l, loop_sock_t *s, proven_u8 got) {
    if (s->node.state == S_CONNECTING) {
        if (!(got & (PROVEN_NET_WRITABLE | PROVEN_NET_FAILED)) || (s->node.done & D_CONNECTED)) return;
        proven_net_conn_t conn = sock_conn(s);
        proven_err_t e = proven_net_connect_result_(&conn);
        if (e == PROVEN_ERR_AGAIN) return;
        (void)node_watch(l, &s->node, s->handle, 0);
        sock_connected(s, e);
        return;
    }
    if (s->node.state != S_OPEN) return;
    if (got & (PROVEN_NET_READABLE | PROVEN_NET_FAILED)) {
        if (s->node.flags & F_RECV) {
            proven_err_t e = PROVEN_OK;
            proven_size_t n = 0;
            if (s->recv_cap > 0) {
                proven_net_conn_t conn = sock_conn(s);
                proven_result_size_t r = proven_net_read(&conn, (proven_mem_mut_t){ .ptr = s->recv_buf, .size = s->recv_cap }, PROVEN_NET_DONT_WAIT);
                e = r.err;
                n = r.value;
            }
            if (e != PROVEN_ERR_TIMEOUT) {
                s->node.flags &= (proven_u8)~F_RECV;
                s->fns->received(s->ctx, e, n);
            }
        } else if (s->node.want & PROVEN_NET_READABLE) {
            (void)node_watch(l, &s->node, s->handle, (proven_u8)(s->node.want & ~PROVEN_NET_READABLE));
        }
    }
    if (s->node.state != S_OPEN) return;
    if (got & (PROVEN_NET_WRITABLE | PROVEN_NET_FAILED)) {
        if (s->head) sock_flush(l, s, true);
        else if (s->node.want & PROVEN_NET_WRITABLE) (void)node_watch(l, &s->node, s->handle, (proven_u8)(s->node.want & ~PROVEN_NET_WRITABLE));
    }
}

/* Take one connection now. True when there is an answer to deliver: a connection, or a failure
 * worth reporting; a connection the peer aborted before it was taken is not. */
static bool acc_try(loop_acc_t *a) {
    proven_err_t e = proven_net_accept(a->listener, PROVEN_NET_DONT_WAIT, &a->conn, (void *)0);
    if (e == PROVEN_ERR_TIMEOUT || e == PROVEN_ERR_RESET) return false;
    a->node.err = (proven_u16)e;
    return true;
}

static void acc_event(proven_loop_t *l, loop_acc_t *a, proven_u8 got) {
    (void)got;
    if (!(a->node.flags & F_RECV)) {
        (void)node_watch(l, &a->node, a->handle, 0);
        return;
    }
    if ((a->node.done & D_ACCEPT) || !acc_try(a)) return;
    acc_deliver(l, a);
}

// --- the calls

LOOP_WAY proven_err_t SEL_(proven_loop_attach)(proven_loop_t *l, proven_loop_sock_t *sock, const proven_net_conn_t *conn,
                                const proven_loop_sock_fns_t *fns, void *ctx) {
    if (!l || !sock || !conn || !fns || !conn->internal.open) return PROVEN_ERR_INVALID_ARG;
    loop_sock_t *s = sock_of(sock);
    for (proven_size_t i = 0; i < sizeof *s; ++i) ((proven_byte_t *)s)[i] = 0;
    s->node.kind = KIND_SOCK;
    s->node.state = S_OPEN;
    s->node.family = conn->internal.family;
    s->handle = conn->internal.handle;
    s->fns = fns;
    s->ctx = ctx;
    s->limit_k = LOOP_SEND_LIMIT_K;
    return PROVEN_OK;
}

LOOP_WAY proven_err_t SEL_(proven_loop_connect)(proven_loop_t *l, proven_loop_sock_t *sock, proven_net_addr_t to, proven_net_conn_t *out,
                                 const proven_loop_sock_fns_t *fns, void *ctx) {
    if (!l || !sock || !out || !fns || !fns->connected) return PROVEN_ERR_INVALID_ARG;
    proven_err_t e = proven_net_connect_start(to, out);
    if (e != PROVEN_OK && e != PROVEN_ERR_AGAIN) return e;
    (void)SEL_(proven_loop_attach)(l, sock, out, fns, ctx);
    loop_sock_t *s = sock_of(sock);
    s->node.state = S_CONNECTING;
    if (e == PROVEN_OK) {                         /* connected already: answered from the loop */
        s->node.err = PROVEN_OK;
        node_queue(l, &s->node, D_CONNECTED);
        return PROVEN_OK;
    }
    e = node_watch(l, &s->node, s->handle, PROVEN_NET_WRITABLE);
    if (e != PROVEN_OK) {
        s->node.state = S_NONE;
        (void)proven_net_close(out);
        return e;
    }
    return PROVEN_OK;
}

LOOP_WAY proven_err_t SEL_(proven_loop_recv)(proven_loop_t *l, proven_loop_sock_t *sock, proven_mem_mut_t buf) {
    if (!l || !sock || (buf.size > 0 && !buf.ptr)) return PROVEN_ERR_INVALID_ARG;
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK || s->node.state != S_OPEN) return PROVEN_ERR_INVALID_STATE;
    if (!s->fns->received) return PROVEN_ERR_INVALID_ARG;
    if (s->node.flags & F_RECV) return PROVEN_ERR_BUSY;
    proven_err_t e = node_watch(l, &s->node, s->handle, (proven_u8)(s->node.want | PROVEN_NET_READABLE));
    if (e != PROVEN_OK) return e;
    s->recv_buf = buf.ptr;
    s->recv_cap = buf.size > 0x7fffffffu ? 0x7fffffffu : (proven_u32)buf.size;
    s->node.flags |= F_RECV;
    return PROVEN_OK;
}

LOOP_WAY void SEL_(proven_loop_recv_cancel)(proven_loop_t *l, proven_loop_sock_t *sock) {
    if (!l || !sock) return;
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK || !(s->node.flags & F_RECV)) return;
    s->node.flags &= (proven_u8)~F_RECV;
    s->node.err = (proven_u16)PROVEN_ERR_CANCELLED;
    node_queue(l, &s->node, D_RECEIVED);
}

/* What waits is sent when the socket is writable. */
static proven_err_t SEL_(sock_kick)(proven_loop_t *l, loop_sock_t *s) {
    return node_watch(l, &s->node, s->handle, (proven_u8)(s->node.want | PROVEN_NET_WRITABLE));
}

LOOP_WAY void SEL_(proven_loop_close)(proven_loop_t *l, proven_loop_sock_t *sock) {
    if (!l || !sock) return;
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK) return;
    proven_u8 state = s->node.state;
    if (state != S_OPEN && state != S_CONNECTING && state != S_DEAD) return;
    proven_u8 bits = D_CLOSED;
    if (s->node.flags & F_RECV) {
        s->node.flags &= (proven_u8)~F_RECV;
        s->node.err = (proven_u16)PROVEN_ERR_CANCELLED;
        bits |= D_RECEIVED;
    }
    if (state == S_CONNECTING) { s->node.err = (proven_u16)PROVEN_ERR_CANCELLED; bits |= D_CONNECTED; }
    if (s->node.flags & F_WAIT) { s->node.flags &= (proven_u8)~F_WAIT; bits |= D_WRITABLE; }
    sock_drop_chunks(l, s);
    node_unwatch(l, &s->node, s->handle);
    s->node.state = S_CLOSING;
    node_queue(l, &s->node, bits);
}

LOOP_WAY proven_err_t SEL_(proven_loop_accept)(proven_loop_t *l, proven_loop_accept_t *acc, proven_net_listener_t *listener,
                                proven_loop_accept_fn fn, void *ctx) {
    if (!l || !acc || !listener || !fn || !proven_net_listener_is_open(listener)) return PROVEN_ERR_INVALID_ARG;
    loop_acc_t *a = acc_of(acc);
    if (a->node.kind != KIND_ACCEPT || a->listener != listener) {
        if (a->node.kind == KIND_ACCEPT && (a->node.flags & F_RECV)) return PROVEN_ERR_BUSY;
        if (a->node.kind == KIND_ACCEPT) node_unwatch(l, &a->node, a->handle);
        for (proven_size_t i = 0; i < sizeof *a; ++i) ((proven_byte_t *)a)[i] = 0;
        a->node.kind = KIND_ACCEPT;
        a->listener = listener;
        a->handle = listener->internal.handle;
    }
    if (a->node.flags & F_RECV) return PROVEN_ERR_BUSY;
    a->fn = fn;
    a->ctx = ctx;
    /* Armed again from inside its own function: there is often another connection waiting
     * already, and taking it now saves a round. It is still answered from the loop. */
    if (l->delivering == a && acc_try(a)) {
        a->node.flags |= F_RECV;
        node_queue(l, &a->node, D_ACCEPT);
        return PROVEN_OK;
    }
    proven_err_t e = node_watch(l, &a->node, a->handle, PROVEN_NET_READABLE);
    if (e != PROVEN_OK) return e;
    a->node.flags |= F_RECV;
    return PROVEN_OK;
}

LOOP_WAY void SEL_(proven_loop_accept_cancel)(proven_loop_t *l, proven_loop_accept_t *acc) {
    if (!l || !acc) return;
    loop_acc_t *a = acc_of(acc);
    if (a->node.kind != KIND_ACCEPT) return;
    node_unwatch(l, &a->node, a->handle);
    if (!(a->node.flags & F_RECV)) return;
    (void)proven_net_close(&a->conn);             /* one taken and not yet delivered */
    a->node.err = (proven_u16)PROVEN_ERR_CANCELLED;
    node_queue(l, &a->node, D_ACCEPT);
}

#endif  /* LOOP_HAS_SEL */

#if LOOP_HAS_CP

// --- the completion port

static loop_node_t *node_of_ov(proven_sys_net_ov_t *ov) { return (loop_node_t *)(void *)((proven_byte_t *)ov - LOOP_OV_AT); }
static proven_err_t sys_err(proven_sys_net_result_t r) { return proven_net_err_from_sys_(r); }
static bool skip_of(const loop_node_t *n) { return (n->flags & F_SKIP) != 0; }
static proven_sys_socket_t sock_handle(const loop_sock_t *s) { return (proven_sys_socket_t)s->handle; }

/* An operation the system holds from now until the port reports it; and its report. */
static void ov_held(proven_loop_t *l, loop_node_t *n, proven_u8 flag) { n->flags |= flag; l->in_flight++; }
static void ov_back(proven_loop_t *l, loop_node_t *n, proven_u8 flag) { n->flags &= (proven_u8)~flag; l->in_flight--; }

/* A closing socket's end comes once the system holds nothing of it. */
static void sock_maybe_closed(proven_loop_t *l, loop_sock_t *s) {
    if (s->node.state == S_CLOSING && !(s->node.flags & (F_OVR | F_OVS))) node_queue(l, &s->node, D_CLOSED);
}

static void sock_check_wait(proven_loop_t *l, loop_sock_t *s, bool answer) {
    if ((s->node.flags & F_WAIT) && s->queued <= s->wait_at) {
        s->node.flags &= (proven_u8)~F_WAIT;
        if (answer) sock_deliver_writable(s, PROVEN_OK); else node_queue(l, &s->node, D_WRITABLE);
    }
}

static void sock_send_failed(proven_loop_t *l, loop_sock_t *s, proven_err_t e, bool answer) {
    if (e == PROVEN_OK) e = PROVEN_ERR_IO;
    s->send_err = (proven_u16)e;
    sock_drop_chunks(l, s);
    s->node.flags &= (proven_u8)~F_WAIT;
    if (answer) sock_deliver_writable(s, e); else node_queue(l, &s->node, D_WRITABLE);
}

/* `n` bytes from the head of the chain have left. */
static void sock_sent(proven_loop_t *l, loop_sock_t *s, proven_size_t n) {
    s->queued -= (proven_u32)n;
    while (s->head) {
        loop_chunk_t *c = s->head;
        proven_size_t here = c->used - c->sent;
        if (n < here) { c->sent += (proven_u32)n; break; }
        n -= here;
        s->head = c->next;
        if (s->head) s->head->last = c->last;
        chunk_give(l, c);
    }
}

/* Send what waits: one WSASend of the chain, from the head chunk's OVERLAPPED. While it is the
 * system's, what is added waits for the next one; nothing of the chain is given back until the
 * port has reported it. A send that finishes at once is counted at once and the next started,
 * a few times over; then the rest goes after this callback. */
static void CP_(sock_flush)(proven_loop_t *l, loop_sock_t *s, bool answer) {
    for (int pass = 0; pass < 4 && s->head && !(s->node.flags & F_OVS) && s->node.state == S_OPEN; ++pass) {
        proven_sys_net_vec_t vec[PROVEN_SYS_NET_VEC_MAX];
        proven_size_t count = 0;
        for (loop_chunk_t *c = s->head; c && count < PROVEN_SYS_NET_VEC_MAX; c = c->next) {
            if (c->used == c->sent) continue;
            vec[count++] = (proven_sys_net_vec_t){ .ptr = c->data + c->sent, .len = c->used - c->sent };
        }
        if (count == 0) break;
        loop_chunk_t *h = s->head;
        h->owner = s;
        proven_u32 sent = 0;
        const proven_sys_net_result_t r = proven_sys_net_ov_send(l->port, sock_handle(s), &h->ov, &h->keep, vec, count, skip_of(&s->node) && !LOOP_RING_SENDS(l), &sent);
        if (r == PROVEN_SYS_NET_IN_PROGRESS) { ov_held(l, &s->node, F_OVS); break; }
        if (r != PROVEN_SYS_NET_OK) { sock_send_failed(l, s, sys_err(r), answer); return; }
        sock_sent(l, s, sent);
    }
    if (s->queued > 0 && !(s->node.flags & F_OVS) && s->node.state == S_OPEN) node_queue(l, &s->node, D_FLUSH);
    sock_check_wait(l, s, answer);
}

/* What waits is sent from the loop, after the callback that gave it: one send for all of it. */
static proven_err_t CP_(sock_kick)(proven_loop_t *l, loop_sock_t *s) {
    if (!(s->node.flags & F_OVS)) node_queue(l, &s->node, D_FLUSH);
    return PROVEN_OK;
}

#if LOOP_RING
/* Sends through the ring wait for the loop: at a close, what was committed and not yet handed to
 * the ring goes to the socket now, as it would have with the socket first; what it does not take
 * is dropped by the caller. (A send the ring already holds is ahead of the close's cancel in the
 * queue, so it is tried first; what waits behind it is dropped, as behind a full socket - the
 * owner may close the socket as soon as proven_loop_close returns.) */
static void sock_write_now(proven_loop_t *l, loop_sock_t *s) {
    proven_net_conn_t conn = sock_conn(s);
    while (s->head) {
        loop_chunk_t *c = s->head;
        if (c->used == c->sent) { sock_sent(l, s, 0); continue; }     /* an empty chunk: given back */
        proven_result_size_t w = proven_net_write(&conn, (proven_mem_view_t){ .ptr = c->data + c->sent, .size = c->used - c->sent }, PROVEN_NET_DONT_WAIT);
        if (w.err != PROVEN_OK || w.value == 0) break;
        sock_sent(l, s, w.value);
    }
}
#endif

static void sock_send_done(proven_loop_t *l, loop_chunk_t *c, const proven_sys_net_done_t *d) {
    loop_sock_t *s = c->owner;
    ov_back(l, &s->node, F_OVS);
    if (s->node.state != S_OPEN) {          /* closed meanwhile: what waited goes now that the system has let go of it */
        sock_drop_chunks(l, s);
        sock_maybe_closed(l, s);
        return;
    }
    if (d->failed) { sock_send_failed(l, s, sys_err(proven_sys_net_ov_result(sock_handle(s), &c->ov)), true); return; }
    sock_sent(l, s, d->bytes);
#if LOOP_RING
    /* Through the ring the owner waiting to send is told before the rest of the chain goes, so
     * that what it adds goes in the same send: told after, its bytes waited a round behind a
     * send of the remainder, and a bulk sender's sends came at half their size. */
    if (LOOP_RING_SENDS(l)) sock_check_wait(l, s, true);
#endif
    sock_flush(l, s, true);
}

/* Start the receive asked for; what happens at once is answered from the loop. */
static void sock_post_recv(proven_loop_t *l, loop_sock_t *s) {
    proven_u32 got = 0;
    const proven_sys_net_result_t r = proven_sys_net_ov_recv(l->port, sock_handle(s), &s->ov, s->recv_buf, s->recv_cap, skip_of(&s->node), &got);
    if (r == PROVEN_SYS_NET_IN_PROGRESS) { ov_held(l, &s->node, F_OVR); return; }
    const proven_err_t e = r != PROVEN_SYS_NET_OK ? sys_err(r) : s->recv_cap > 0 && got == 0 ? PROVEN_ERR_EOF : PROVEN_OK;
    s->node.err = (proven_u16)e;
    s->recv_cap = got;                    /* from here on, what it took */
    node_queue(l, &s->node, D_RECEIVED);
}

/* The report of the socket's own OVERLAPPED: a connect's, or a receive's. A socket that is
 * closing has its answer CANCELLED, without asking the system why (its handle may be gone). */
static void sock_ov_done(proven_loop_t *l, loop_sock_t *s, const proven_sys_net_done_t *d) {
    ov_back(l, &s->node, F_OVR);
    const bool cancelled = (s->node.flags & F_CANCEL) != 0;
    s->node.flags &= (proven_u8)~F_CANCEL;
    if (s->node.want & W_CONNECT) {
        s->node.want = 0;
        proven_err_t e = s->node.state != S_CONNECTING ? PROVEN_ERR_CANCELLED
                       : d->failed ? sys_err(proven_sys_net_ov_result(sock_handle(s), &s->ov))
                       : sys_err(proven_sys_net_ov_connect_done(sock_handle(s)));
        if (d->failed && e == PROVEN_OK) e = PROVEN_ERR_IO;
        sock_connected(s, e);
    } else if (s->node.flags & F_RECV) {
        proven_err_t e;
        proven_size_t n = 0;
        if (s->node.state != S_OPEN) e = PROVEN_ERR_CANCELLED;
        else if (d->failed) { e = sys_err(proven_sys_net_ov_result(sock_handle(s), &s->ov)); if (e == PROVEN_OK) e = PROVEN_ERR_IO; }
        else if (s->recv_cap == 0) e = cancelled ? PROVEN_ERR_CANCELLED : PROVEN_OK;   /* took nothing: a late cancel still stands */
        else if (d->bytes == 0) e = PROVEN_ERR_EOF;
        else { e = PROVEN_OK; n = d->bytes; }                                          /* bytes taken are delivered, cancelled or not */
        s->node.flags &= (proven_u8)~F_RECV;
        s->fns->received(s->ctx, e, n);
    }
    sock_maybe_closed(l, s);
}

#if LOOP_RING
// --- io_uring: a multishot receive into the loop's pool (RFC-0030)

/* The pool, made at the first need; false where the kernel has none to offer. */
static bool loop_pool(proven_loop_t *l) {
    if (l->rbuf_state == 0) {
#if defined(PROVEN_LOOP_NO_PBUF_)
        l->rbuf_state = 2;                        /* a build of RFC-0029's ring, for comparison */
#else
        l->rbuf_state = proven_sys_net_port_pool(l->port, l->rbuf_count, l->rbuf_bytes) ? 1 : 2;
#endif
    }
    return l->rbuf_state == 1;
}

/* What a receive of a socket read through the pool finds now: 1 something to answer with (bytes,
 * or how the receive ended), 0 the multishot receive in flight will answer, -1 there is none and
 * a receive is to be started. The OVERLAPPED knows a receive has ended as soon as its last report
 * is taken from the ring, a batch before that report is delivered: until it is (W_MSARM), no other
 * receive is started - its delivery would end the new one instead. */
static int ms_state(proven_loop_t *l, loop_sock_t *s) {
    if (!(s->node.want & W_MS)) return -1;
    if (proven_sys_net_ov_held(l->port, &s->ov) > 0) return 1;
    proven_size_t n = 0;
    const proven_sys_net_result_t r = proven_sys_net_ov_read(l->port, &s->ov, (void *)0, 0, &n);
    if (r == PROVEN_SYS_NET_NOT_FOUND) return (s->node.want & W_MSARM) ? 0 : -1;
    return r == PROVEN_SYS_NET_WOULD_BLOCK ? 0 : 1;
}

/* Take the receive's bytes (or its end) for a receive into a buffer; a receive of nothing takes none. */
static proven_err_t ms_take(proven_loop_t *l, loop_sock_t *s, proven_size_t *n) {
    *n = 0;
    if (s->recv_cap == 0) return PROVEN_OK;
    const proven_sys_net_result_t r = proven_sys_net_ov_read(l->port, &s->ov, s->recv_buf, s->recv_cap, n);
    return r == PROVEN_SYS_NET_OK ? PROVEN_OK : sys_err(r);
}

/* Start the receive asked for: a multishot receive while the pool has a buffer to give, the
 * plain one otherwise. */
static void sock_arm(proven_loop_t *l, loop_sock_t *s) {
    if (loop_pool(l) && proven_sys_net_port_pool_free(l->port)) {
        if (!(s->node.want & W_MS)) s->ov = (proven_sys_net_ov_t){ 0 };
        if (proven_sys_net_ov_recv_multi(l->port, sock_handle(s), &s->ov) == PROVEN_SYS_NET_IN_PROGRESS) {
            s->node.want |= W_MS | W_MSARM;
            ov_held(l, &s->node, F_OVR);
            return;
        }
    }
    s->node.want &= (proven_u8)~W_MS;             /* the OVERLAPPED is the plain receive's now: it held nothing */
    sock_post_recv(l, s);
}

/* A report of the multishot receive: a buffer filled, or its end. It answers a receive asked
 * for; with none asked for, a socket holding LOOP_HELD_MAX buffers has its receive stopped. */
static void sock_ms_done(proven_loop_t *l, loop_sock_t *s, const proven_sys_net_done_t *d) {
    if (!d->more) {
        s->node.want &= (proven_u8)~W_MSARM;
        s->node.flags &= (proven_u8)~F_CANCEL;
        ov_back(l, &s->node, F_OVR);
    }
    if (s->node.state != S_OPEN) {                /* closing: what it held goes back once it has ended */
        if (!d->more) { proven_sys_net_ov_drop(l->port, &s->ov); sock_maybe_closed(l, s); }
        return;
    }
    if ((s->node.flags & F_RECV) && !(s->node.done & D_RECEIVED)) {
        const int st = ms_state(l, s);
        if (st == 0) return;
        if (st < 0) { sock_arm(l, s); return; }   /* stopped with nothing held: started again, the receive still waiting */
        proven_size_t n = 0;
        const proven_err_t e = ms_take(l, s, &n);
        s->node.flags &= (proven_u8)~F_RECV;
        s->fns->received(s->ctx, e, n);
        return;
    }
    if ((s->node.want & W_MSARM) && !(s->node.flags & (F_CANCEL | F_RECV)) && proven_sys_net_ov_held(l->port, &s->ov) >= LOOP_HELD_MAX) {
        s->node.flags |= F_CANCEL;
        proven_sys_net_ov_cancel_multi(l->port, &s->ov);
    }
}
#endif

// --- UDP

static void udp_maybe_closed(proven_loop_t *l, loop_udp_t *u) {
    if (u->node.state == S_CLOSING && !(u->node.flags & F_OVR)) node_queue(l, &u->node, D_CLOSED);
}

static void udp_took(loop_udp_t *u, proven_u32 got) {
    proven_size_t seg = 0;
    proven_sys_net_addr_t from;
    proven_sys_net_ov_msg_done(&u->msg, got, &seg, &u->ecn, &from);
    proven_net_addr_from_sys_(&from, u->from);
    u->node.err = (proven_u16)PROVEN_OK;
    u->n = got;
    u->seg = (proven_u32)seg;
}

/* Start the receive asked for. A datagram cut short (none of QUIC's) and a reported error of an
 * earlier send are passed over, as on the selector. */
static void udp_post(proven_loop_t *l, loop_udp_t *u) {
    proven_sys_net_result_t r = PROVEN_SYS_NET_ERROR;
    for (int i = 0; i < 64; ++i) {
        proven_u32 got = 0;
        r = proven_sys_net_ov_recv_msg(l->port, (proven_sys_socket_t)u->udp->internal.handle, &u->ov, &u->msg, u->buf, u->cap, skip_of(&u->node), &got);
        if (r == PROVEN_SYS_NET_IN_PROGRESS) { ov_held(l, &u->node, F_OVR); return; }
        if (r == PROVEN_SYS_NET_OK) { udp_took(u, got); node_queue(l, &u->node, D_RECEIVED); return; }
        if (r != PROVEN_SYS_NET_TRUNCATED && r != PROVEN_SYS_NET_RESET) break;
    }
    u->node.err = (proven_u16)sys_err(r);
    u->n = u->seg = 0;
    u->ecn = 0;
    node_queue(l, &u->node, D_RECEIVED);
}

static void udp_ov_done(proven_loop_t *l, loop_udp_t *u, const proven_sys_net_done_t *d) {
    ov_back(l, &u->node, F_OVR);
    u->node.flags &= (proven_u8)~F_CANCEL;
    if (u->node.state != S_OPEN) {
        udp_received(l, u, PROVEN_ERR_CANCELLED, 0, 0, 0);
        udp_maybe_closed(l, u);
        return;
    }
    if (d->failed) {
        const proven_sys_net_result_t r = proven_sys_net_ov_result((proven_sys_socket_t)u->udp->internal.handle, &u->ov);
        if (r == PROVEN_SYS_NET_TRUNCATED || r == PROVEN_SYS_NET_RESET) { udp_post(l, u); return; }
        udp_received(l, u, r == PROVEN_SYS_NET_OK ? PROVEN_ERR_IO : sys_err(r), 0, 0, 0);
        return;
    }
    udp_took(u, d->bytes);
    udp_received(l, u, PROVEN_OK, u->n, u->seg, u->ecn);
}

LOOP_WAY proven_err_t CP_(proven_loop_udp_attach_)(proven_loop_t *l, proven_loop_udp_t *udp_op, proven_net_udp_t *udp, const proven_loop_udp_fns_t *fns, void *ctx) {
    if (!l || !udp_op || !udp || !fns || !fns->received || !proven_net_udp_is_open(udp)) return PROVEN_ERR_INVALID_ARG;
    loop_udp_t *u = (loop_udp_t *)(void *)udp_op;
    for (proven_size_t i = 0; i < sizeof *u; ++i) ((proven_byte_t *)u)[i] = 0;
    /* Every receive of a datagram socket is reported by the port (proven_sys_net_port_bind). */
    bool skip = false;
    const proven_sys_net_result_t r = proven_sys_net_port_bind(l->port, (proven_sys_socket_t)udp->internal.handle, false, &skip);
    if (r != PROVEN_SYS_NET_OK) return sys_err(r);
    u->node.kind = KIND_UDP;
    u->node.state = S_OPEN;
    u->udp = udp;
    u->fns = fns;
    u->ctx = ctx;
    node_link(l, &u->node);
    return PROVEN_OK;
}

LOOP_WAY proven_err_t CP_(proven_loop_udp_recv_)(proven_loop_t *l, proven_loop_udp_t *udp_op, proven_mem_mut_t buf, proven_net_addr_t *from) {
    if (!l || !udp_op || !buf.ptr || buf.size == 0 || !from) return PROVEN_ERR_INVALID_ARG;
    loop_udp_t *u = (loop_udp_t *)(void *)udp_op;
    if (u->node.kind != KIND_UDP || u->node.state != S_OPEN) return PROVEN_ERR_INVALID_STATE;
    if (u->node.flags & F_RECV) return PROVEN_ERR_BUSY;
    u->buf = buf.ptr;
    u->cap = buf.size > 0x7fffffffu ? 0x7fffffffu : (proven_u32)buf.size;
    u->from = from;
    u->node.flags |= F_RECV;
    udp_post(l, u);
    return PROVEN_OK;
}

/* Windows does not say when a UDP socket takes datagrams again - its sends are rarely refused -
 * so a wait is answered after a millisecond, on the loop's timer. */
static void udp_wait_fire(void *ctx) {
    loop_udp_t *u = ctx;
    if (u->node.state != S_OPEN || !(u->node.flags & F_WAIT)) return;
    u->node.flags &= (proven_u8)~F_WAIT;
    if (u->fns->writable) u->fns->writable(u->ctx, PROVEN_OK);
}

LOOP_WAY proven_err_t CP_(proven_loop_udp_send_wait_)(proven_loop_t *l, proven_loop_udp_t *udp_op) {
    if (!l || !udp_op) return PROVEN_ERR_INVALID_ARG;
    loop_udp_t *u = (loop_udp_t *)(void *)udp_op;
    if (u->node.kind != KIND_UDP || u->node.state != S_OPEN) return PROVEN_ERR_INVALID_STATE;
    if (u->node.flags & F_WAIT) return PROVEN_OK;
    u->node.flags |= F_WAIT;
    proven_loop_timer_set(l, &u->wait, 1, udp_wait_fire, u);
    return PROVEN_OK;
}

LOOP_WAY void CP_(proven_loop_udp_close_)(proven_loop_t *l, proven_loop_udp_t *udp_op) {
    if (!l || !udp_op) return;
    loop_udp_t *u = (loop_udp_t *)(void *)udp_op;
    if (u->node.kind != KIND_UDP || u->node.state != S_OPEN) return;
    proven_u8 bits = 0;
    if ((u->node.flags & F_RECV) && !(u->node.flags & F_OVR)) {     /* answered already from the list: datagrams taken are dropped */
        u->node.err = (proven_u16)PROVEN_ERR_CANCELLED;
        u->n = u->seg = 0;
        bits |= D_RECEIVED;
    }
    if (u->node.flags & F_OVR) proven_sys_net_ov_cancel(l->port, (proven_sys_socket_t)u->udp->internal.handle, &u->ov);
    if (u->node.flags & F_WAIT) { u->node.flags &= (proven_u8)~F_WAIT; proven_loop_timer_cancel(l, &u->wait); bits |= D_WRITABLE; }
    u->node.state = S_CLOSING;
    if (!(u->node.flags & F_OVR)) bits |= D_CLOSED;
    node_queue(l, &u->node, bits);
}

// --- accepts

/* A socket accepted, made what proven_net_accept gives. False: it failed, and is closed. */
static bool acc_take(loop_acc_t *a, proven_sys_socket_t got) {
    if (proven_sys_net_ov_accept_done((proven_sys_socket_t)a->handle, got) != PROVEN_SYS_NET_OK) { (void)proven_sys_net_close(got); return false; }
    a->conn = (proven_net_conn_t){ 0 };
    a->conn.internal.handle = (proven_uintptr_t)got;
    a->conn.internal.open = true;
    a->conn.internal.family = a->node.family;
    return true;
}

/* Start the accept asked for: AcceptEx, with a socket made for it. A connection aborted before it
 * was taken is passed over. */
static void acc_post(proven_loop_t *l, loop_acc_t *a) {
    proven_sys_net_result_t r = PROVEN_SYS_NET_ERROR;
    for (int i = 0; i < 64; ++i) {
        proven_sys_socket_t got = PROVEN_SYS_SOCKET_INVALID;
        r = proven_sys_net_ov_accept(l->port, (proven_sys_socket_t)a->handle, (proven_sys_net_family_t)a->node.family, &a->ov, a->area, skip_of(&a->node), &got);
        if (r == PROVEN_SYS_NET_IN_PROGRESS) { a->conn.internal.handle = (proven_uintptr_t)got; ov_held(l, &a->node, F_OVR); return; }
        if (r == PROVEN_SYS_NET_OK) {
            if (!acc_take(a, got)) continue;
            a->node.err = (proven_u16)PROVEN_OK;
            node_queue(l, &a->node, D_ACCEPT);
            return;
        }
        if (r != PROVEN_SYS_NET_RESET) break;
    }
    a->conn = (proven_net_conn_t){ 0 };
    a->node.err = (proven_u16)sys_err(r);
    node_queue(l, &a->node, D_ACCEPT);
}

static void acc_ov_done(proven_loop_t *l, loop_acc_t *a, const proven_sys_net_done_t *d) {
    ov_back(l, &a->node, F_OVR);
    proven_sys_socket_t got = (proven_sys_socket_t)a->conn.internal.handle;
    if (got == PROVEN_SYS_SOCKET_INVALID && !d->failed) got = (proven_sys_socket_t)d->bytes;   /* io_uring: the report's bytes */
    a->conn = (proven_net_conn_t){ 0 };
    if (a->node.flags & F_CANCEL) {         /* the listener may be gone: no question asked of it */
        if (got != PROVEN_SYS_SOCKET_INVALID) (void)proven_sys_net_close(got);
        a->node.err = (proven_u16)PROVEN_ERR_CANCELLED;
        acc_deliver(l, a);
        return;
    }
    if (d->failed) {
        const proven_sys_net_result_t r = proven_sys_net_ov_result((proven_sys_socket_t)a->handle, &a->ov);
        if (got != PROVEN_SYS_SOCKET_INVALID) (void)proven_sys_net_close(got);
        if (r == PROVEN_SYS_NET_RESET) { acc_post(l, a); return; }
        a->node.err = (proven_u16)(r == PROVEN_SYS_NET_OK ? PROVEN_ERR_IO : sys_err(r));
        acc_deliver(l, a);
        return;
    }
    if (!acc_take(a, got)) { acc_post(l, a); return; }
    a->node.err = (proven_u16)PROVEN_OK;
    acc_deliver(l, a);
}

/* What the port reports. */
static void loop_report(proven_loop_t *l, const proven_sys_net_done_t *d) {
    if (!d->ov) { atomic_store(&l->woken, false); return; }       /* the waker: the posts are looked at after this batch */
    loop_node_t *n = node_of_ov(d->ov);
    switch (n->kind) {
    case KIND_SOCK:
#if LOOP_RING
        if (n->want & W_MSARM) { sock_ms_done(l, (loop_sock_t *)n, d); break; }
#endif
        sock_ov_done(l, (loop_sock_t *)n, d);
        break;
    case KIND_CHUNK: sock_send_done(l, (loop_chunk_t *)n, d); break;
    case KIND_ACCEPT: acc_ov_done(l, (loop_acc_t *)n, d); break;
    case KIND_UDP: udp_ov_done(l, (loop_udp_t *)n, d); break;
    default: break;
    }
}

/* At destroy: what the system still holds is asked to stop and waited for (for up to two
 * seconds) - until the port has reported it, the system may still write into memory its owner
 * frees once the loop is gone. Then what had ended gets its last answer; a node the system still
 * holds after the wait gets none, so that its memory is not freed under the system. */
/* Whether a node that has ended is owed a last answer at destroy (a closed socket's `closed`, a
 * cancelled accept's PROVEN_ERR_CANCELLED); an accept taken and never delivered is closed. */
static bool node_owed(loop_node_t *n) {
    if (n->kind == KIND_SOCK || n->kind == KIND_UDP) return n->state == S_CLOSING;
    if (n->kind != KIND_ACCEPT) return false;
    loop_acc_t *a = (loop_acc_t *)n;
    const bool owed = (n->flags & F_CANCEL) || ((n->done & D_ACCEPT) && n->err == (proven_u16)PROVEN_ERR_CANCELLED);
    if (!owed && (n->done & D_ACCEPT)) (void)proven_net_close(&a->conn);     /* taken, never delivered */
    return owed;
}

static void node_last_answer(loop_node_t *n) {
    n->flags &= (proven_u8)~(F_RECV | F_CANCEL | F_QUEUED);
    n->done = 0;
    if (n->kind == KIND_SOCK) {
        loop_sock_t *s = (loop_sock_t *)n;
        s->node.state = S_NONE;
        if (s->fns->closed) s->fns->closed(s->ctx);
    } else if (n->kind == KIND_UDP) {
        loop_udp_t *u = (loop_udp_t *)n;
        u->node.state = S_NONE;
        if (u->fns->closed) u->fns->closed(u->ctx);
    } else {
        loop_acc_t *a = (loop_acc_t *)n;
        proven_net_conn_t none = { 0 };
        a->fn(a->ctx, PROVEN_ERR_CANCELLED, &none);
    }
}

/* A report taken at destroy: the operation is no longer the system's; an accepted socket is closed. */
static loop_node_t *drain_report(proven_loop_t *l, const proven_sys_net_done_t *d) {
    loop_node_t *n = node_of_ov(d->ov);
    if (n->kind == KIND_CHUNK) { n = &((loop_chunk_t *)n)->owner->node; ov_back(l, n, F_OVS); return n; }
    if (d->more) return n;                        /* a multishot receive's: its last report is still to come */
    ov_back(l, n, F_OVR);
    if (n->kind == KIND_ACCEPT) {
        loop_acc_t *a = (loop_acc_t *)n;
        proven_sys_socket_t got = (proven_sys_socket_t)a->conn.internal.handle;
        if (got == PROVEN_SYS_SOCKET_INVALID && !d->failed) got = (proven_sys_socket_t)d->bytes;   /* io_uring */
        if (got != PROVEN_SYS_SOCKET_INVALID) (void)proven_sys_net_close(got);
        a->conn = (proven_net_conn_t){ 0 };
    }
    return n;
}

/* Destroy: cancel what the system holds and wait for it, up to two seconds, so that nothing is
 * written into memory after the loop is gone; then the last answers. */
static void loop_drain(proven_loop_t *l) {
#if LOOP_IOCP
    for (loop_node_t *n = l->all; n; n = n->all_next) {
        if (!(n->flags & (F_OVR | F_OVS))) continue;
        if (n->kind == KIND_SOCK && n->state != S_CLOSING) proven_sys_net_ov_cancel(l->port, sock_handle((loop_sock_t *)n), (void *)0);
        else if (n->kind == KIND_ACCEPT && !(n->flags & F_CANCEL)) proven_sys_net_ov_cancel(l->port, (proven_sys_socket_t)((loop_acc_t *)n)->handle, &((loop_acc_t *)n)->ov);
        else if (n->kind == KIND_UDP && n->state != S_CLOSING) proven_sys_net_ov_cancel(l->port, (proven_sys_socket_t)((loop_udp_t *)n)->udp->internal.handle, &((loop_udp_t *)n)->ov);
    }
#else
    if (l->in_flight > 0) proven_sys_net_port_cancel_all(l->port);
#endif
    const proven_net_deadline_t end = proven_net_deadline_in(2000);
    while (l->in_flight > 0 && proven_time_monotonic_now() < end) {
        proven_size_t count = 0;
        if (proven_sys_net_port_wait(l->port, l->cp_batch, LOOP_EVENTS, 100, &count) != PROVEN_SYS_NET_OK) break;
        for (proven_size_t i = 0; i < count; ++i) {
            if (!l->cp_batch[i].ov) continue;
            loop_node_t *n = drain_report(l, &l->cp_batch[i]);
#if !LOOP_IOCP
            /* No list of every node here: one that has just ended and is owed an answer joins
             * the done list, where the others owed one are already. */
            if (!(n->flags & (F_OVR | F_OVS | F_QUEUED)) && (n->state == S_CLOSING || (n->flags & F_CANCEL))) node_queue(l, n, 0);
#else
            (void)n;
#endif
        }
    }
    /* Those owed a last answer are put in order on the done links, and answered after. */
    loop_node_t *ends = (void *)0, **tail = &ends;
#if LOOP_IOCP
    l->done_first = l->done_last = (void *)0;
    while (l->all) {
        loop_node_t *n = l->all;
        node_unlink(l, n);
        if (n->flags & (F_OVR | F_OVS) || !node_owed(n)) continue;
        n->next = (void *)0;
        *tail = n;
        tail = &n->next;
    }
#else
    loop_node_t *n = l->done_first;
    l->done_first = l->done_last = (void *)0;
    for (loop_node_t *next; n; n = next) {
        next = n->next;
        if (n->flags & (F_OVR | F_OVS) || !node_owed(n)) continue;
        n->next = (void *)0;
        *tail = n;
        tail = &n->next;
    }
#endif
    for (loop_node_t *n = ends, *next; n; n = next) {
        next = n->next;
        node_last_answer(n);
    }
}

// --- the calls

LOOP_WAY proven_err_t CP_(proven_loop_attach)(proven_loop_t *l, proven_loop_sock_t *sock, const proven_net_conn_t *conn,
                                const proven_loop_sock_fns_t *fns, void *ctx) {
    if (!l || !sock || !conn || !fns || !conn->internal.open) return PROVEN_ERR_INVALID_ARG;
    loop_sock_t *s = sock_of(sock);
    for (proven_size_t i = 0; i < sizeof *s; ++i) ((proven_byte_t *)s)[i] = 0;
    bool skip = false;
    const proven_sys_net_result_t r = proven_sys_net_port_bind(l->port, (proven_sys_socket_t)conn->internal.handle, true, &skip);
    if (r != PROVEN_SYS_NET_OK) return sys_err(r);
    s->node.kind = KIND_SOCK;
    s->node.state = S_OPEN;
    s->node.family = conn->internal.family;
    if (skip) s->node.flags |= F_SKIP;
    s->handle = conn->internal.handle;
    s->fns = fns;
    s->ctx = ctx;
    s->limit_k = LOOP_SEND_LIMIT_K;
    node_link(l, &s->node);
    return PROVEN_OK;
}

LOOP_WAY proven_err_t CP_(proven_loop_connect)(proven_loop_t *l, proven_loop_sock_t *sock, proven_net_addr_t to, proven_net_conn_t *out,
                                 const proven_loop_sock_fns_t *fns, void *ctx) {
    if (!l || !sock || !out || !fns || !fns->connected) return PROVEN_ERR_INVALID_ARG;
    *out = (proven_net_conn_t){ 0 };
    if (to.family == PROVEN_NET_FAMILY_NONE) return PROVEN_ERR_INVALID_ARG;
    proven_sys_net_addr_t sys;
    proven_net_addr_to_sys_(&to, &sys);
    proven_sys_socket_t h = PROVEN_SYS_SOCKET_INVALID;
    proven_sys_net_result_t r = proven_sys_net_ov_connect_socket(&sys, &h);
    if (r != PROVEN_SYS_NET_OK) return sys_err(r);
    out->internal.handle = (proven_uintptr_t)h;
    out->internal.open = true;
    out->internal.family = (proven_u8)to.family;
    proven_err_t e = CP_(proven_loop_attach)(l, sock, out, fns, ctx);
    if (e != PROVEN_OK) { (void)proven_net_close(out); return e; }
    loop_sock_t *s = sock_of(sock);
    s->node.state = S_CONNECTING;
    r = proven_sys_net_ov_connect(l->port, h, &sys, &s->ov, skip_of(&s->node));
    if (r == PROVEN_SYS_NET_IN_PROGRESS) { s->node.want = W_CONNECT; ov_held(l, &s->node, F_OVR); return PROVEN_OK; }
    if (r == PROVEN_SYS_NET_OK) {                 /* connected already: answered from the loop */
        s->node.err = PROVEN_OK;
        node_queue(l, &s->node, D_CONNECTED);
        return PROVEN_OK;
    }
    node_unlink(l, &s->node);                     /* failed at once: nothing is attached */
    s->node.state = S_NONE;
    (void)proven_net_close(out);
    return sys_err(r);
}

LOOP_WAY proven_err_t CP_(proven_loop_recv)(proven_loop_t *l, proven_loop_sock_t *sock, proven_mem_mut_t buf) {
    if (!l || !sock || (buf.size > 0 && !buf.ptr)) return PROVEN_ERR_INVALID_ARG;
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK || s->node.state != S_OPEN) return PROVEN_ERR_INVALID_STATE;
    if (!s->fns->received) return PROVEN_ERR_INVALID_ARG;
    if (s->node.flags & F_RECV) return PROVEN_ERR_BUSY;
    s->recv_buf = buf.ptr;
    s->recv_cap = buf.size > 0x7fffffffu ? 0x7fffffffu : (proven_u32)buf.size;
    s->node.flags |= F_RECV;
#if LOOP_RING
    /* A socket read with proven_loop_read: from what the pool holds, from the multishot receive
     * in flight, or from one started now. */
    if (s->node.want & W_READER) {
        const int st = ms_state(l, s);
        if (st > 0) {
            proven_size_t n = 0;
            s->node.err = (proven_u16)ms_take(l, s, &n);
            s->recv_cap = (proven_u32)n;          /* from here on, what it took */
            node_queue(l, &s->node, D_RECEIVED);
        } else if (st < 0) {
            sock_arm(l, s);
        }
        return PROVEN_OK;
    }
#endif
    sock_post_recv(l, s);
    return PROVEN_OK;
}

LOOP_WAY void CP_(proven_loop_recv_cancel)(proven_loop_t *l, proven_loop_sock_t *sock) {
    if (!l || !sock) return;
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK || !(s->node.flags & F_RECV)) return;
    if ((s->node.flags & F_OVR) && (s->node.want & W_MSARM)) {
        /* The multishot receive goes on for the next receive: this one is answered from the list. */
        if (!(s->node.done & D_RECEIVED)) { s->node.err = (proven_u16)PROVEN_ERR_CANCELLED; s->recv_cap = 0; node_queue(l, &s->node, D_RECEIVED); }
        return;
    }
    if (s->node.flags & F_OVR) {
        if (!(s->node.flags & F_CANCEL)) { s->node.flags |= F_CANCEL; proven_sys_net_ov_cancel(l->port, sock_handle(s), &s->ov); }
        return;
    }
    /* Answered already, from the list: cancelled, unless bytes were taken. */
    if (s->node.err != (proven_u16)PROVEN_OK || s->recv_cap == 0 || s->recv_buf == (void *)0) s->node.err = (proven_u16)PROVEN_ERR_CANCELLED;
}

LOOP_WAY void CP_(proven_loop_close)(proven_loop_t *l, proven_loop_sock_t *sock) {
    if (!l || !sock) return;
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK) return;
    const proven_u8 state = s->node.state;
    if (state != S_OPEN && state != S_CONNECTING && state != S_DEAD) return;
    proven_u8 bits = 0;
    /* A receive is answered now unless the system holds it; a multishot receive is not the receive. */
    if ((s->node.flags & F_RECV) && (!(s->node.flags & F_OVR) || (s->node.want & W_MSARM))) { s->node.err = (proven_u16)PROVEN_ERR_CANCELLED; s->recv_cap = 0; bits |= D_RECEIVED; }
#if LOOP_RING
    if ((s->node.want & W_MS) && !(s->node.want & W_MSARM)) proven_sys_net_ov_drop(l->port, &s->ov);
#endif
    if (state == S_CONNECTING && !(s->node.flags & F_OVR)) { s->node.err = (proven_u16)PROVEN_ERR_CANCELLED; bits |= D_CONNECTED; }
    if (s->node.flags & F_WAIT) { s->node.flags &= (proven_u8)~F_WAIT; bits |= D_WRITABLE; }
#if LOOP_RING
    if (LOOP_RING_SENDS(l) && state == S_OPEN && !(s->node.flags & F_OVS)) sock_write_now(l, s);
#endif
    /* What the system holds is cancelled and answered when the port reports it; bytes in a send
     * of the system's go when it has let go of them. */
    if (s->node.flags & (F_OVR | F_OVS)) proven_sys_net_ov_cancel(l->port, sock_handle(s), (void *)0);
    if (!(s->node.flags & F_OVS)) sock_drop_chunks(l, s);
    s->node.state = S_CLOSING;
    if (!(s->node.flags & (F_OVR | F_OVS))) bits |= D_CLOSED;
    node_queue(l, &s->node, bits);
}

LOOP_WAY proven_err_t CP_(proven_loop_accept)(proven_loop_t *l, proven_loop_accept_t *acc, proven_net_listener_t *listener,
                                proven_loop_accept_fn fn, void *ctx) {
    if (!l || !acc || !listener || !fn || !proven_net_listener_is_open(listener)) return PROVEN_ERR_INVALID_ARG;
    loop_acc_t *a = acc_of(acc);
    if (a->node.kind != KIND_ACCEPT || a->listener != listener || a->handle != listener->internal.handle) {
        if (a->node.kind == KIND_ACCEPT && (a->node.flags & F_RECV)) return PROVEN_ERR_BUSY;
        for (proven_size_t i = 0; i < sizeof *a; ++i) ((proven_byte_t *)a)[i] = 0;
        bool skip = false;
        const proven_sys_net_result_t r = proven_sys_net_port_bind(l->port, (proven_sys_socket_t)listener->internal.handle, true, &skip);
        if (r != PROVEN_SYS_NET_OK) return sys_err(r);
        a->node.kind = KIND_ACCEPT;
        a->node.family = listener->internal.family;
        if (skip) a->node.flags |= F_SKIP;
        a->listener = listener;
        a->handle = listener->internal.handle;
    }
    if (a->node.flags & F_RECV) return PROVEN_ERR_BUSY;
    a->fn = fn;
    a->ctx = ctx;
    a->node.flags |= F_RECV;
    node_link(l, &a->node);
    acc_post(l, a);
    return PROVEN_OK;
}

LOOP_WAY void CP_(proven_loop_accept_cancel)(proven_loop_t *l, proven_loop_accept_t *acc) {
    if (!l || !acc) return;
    loop_acc_t *a = acc_of(acc);
    if (a->node.kind != KIND_ACCEPT || !(a->node.flags & F_RECV)) return;
    if (a->node.flags & F_OVR) {
        if (!(a->node.flags & F_CANCEL)) { a->node.flags |= F_CANCEL; proven_sys_net_ov_cancel(l->port, (proven_sys_socket_t)a->handle, &a->ov); }
        return;
    }
    (void)proven_net_close(&a->conn);             /* one taken and not yet delivered */
    a->node.err = (proven_u16)PROVEN_ERR_CANCELLED;
    node_queue(l, &a->node, D_ACCEPT);
}

#endif  /* LOOP_HAS_CP */

#if LOOP_RING
/* Both ways built: the public calls choose by the loop. */
#define LOOP_IS_CP(l) ((l) && LOOP_CP(l))
static proven_err_t sock_kick(proven_loop_t *l, loop_sock_t *s) { return LOOP_CP(l) ? cp_sock_kick(l, s) : sel_sock_kick(l, s); }
proven_err_t proven_loop_udp_attach_(proven_loop_t *l, proven_loop_udp_t *udp_op, proven_net_udp_t *udp, const proven_loop_udp_fns_t *fns, void *ctx) {
    return LOOP_IS_CP(l) ? cp_proven_loop_udp_attach_(l, udp_op, udp, fns, ctx) : sel_proven_loop_udp_attach_(l, udp_op, udp, fns, ctx);
}
proven_err_t proven_loop_udp_recv_(proven_loop_t *l, proven_loop_udp_t *udp_op, proven_mem_mut_t buf, proven_net_addr_t *from) {
    return LOOP_IS_CP(l) ? cp_proven_loop_udp_recv_(l, udp_op, buf, from) : sel_proven_loop_udp_recv_(l, udp_op, buf, from);
}
proven_err_t proven_loop_udp_send_wait_(proven_loop_t *l, proven_loop_udp_t *udp_op) {
    return LOOP_IS_CP(l) ? cp_proven_loop_udp_send_wait_(l, udp_op) : sel_proven_loop_udp_send_wait_(l, udp_op);
}
void proven_loop_udp_close_(proven_loop_t *l, proven_loop_udp_t *udp_op) {
    if (LOOP_IS_CP(l)) cp_proven_loop_udp_close_(l, udp_op); else sel_proven_loop_udp_close_(l, udp_op);
}
proven_err_t proven_loop_attach(proven_loop_t *l, proven_loop_sock_t *sock, const proven_net_conn_t *conn,
                                const proven_loop_sock_fns_t *fns, void *ctx) {
    return LOOP_IS_CP(l) ? cp_proven_loop_attach(l, sock, conn, fns, ctx) : sel_proven_loop_attach(l, sock, conn, fns, ctx);
}
proven_err_t proven_loop_connect(proven_loop_t *l, proven_loop_sock_t *sock, proven_net_addr_t to, proven_net_conn_t *out,
                                 const proven_loop_sock_fns_t *fns, void *ctx) {
    return LOOP_IS_CP(l) ? cp_proven_loop_connect(l, sock, to, out, fns, ctx) : sel_proven_loop_connect(l, sock, to, out, fns, ctx);
}
proven_err_t proven_loop_recv(proven_loop_t *l, proven_loop_sock_t *sock, proven_mem_mut_t buf) {
    return LOOP_IS_CP(l) ? cp_proven_loop_recv(l, sock, buf) : sel_proven_loop_recv(l, sock, buf);
}
void proven_loop_recv_cancel(proven_loop_t *l, proven_loop_sock_t *sock) {
    if (LOOP_IS_CP(l)) cp_proven_loop_recv_cancel(l, sock); else sel_proven_loop_recv_cancel(l, sock);
}
void proven_loop_close(proven_loop_t *l, proven_loop_sock_t *sock) {
    if (LOOP_IS_CP(l)) cp_proven_loop_close(l, sock); else sel_proven_loop_close(l, sock);
}
proven_err_t proven_loop_accept(proven_loop_t *l, proven_loop_accept_t *acc, proven_net_listener_t *listener,
                                proven_loop_accept_fn fn, void *ctx) {
    return LOOP_IS_CP(l) ? cp_proven_loop_accept(l, acc, listener, fn, ctx) : sel_proven_loop_accept(l, acc, listener, fn, ctx);
}
void proven_loop_accept_cancel(proven_loop_t *l, proven_loop_accept_t *acc) {
    if (LOOP_IS_CP(l)) cp_proven_loop_accept_cancel(l, acc); else sel_proven_loop_accept_cancel(l, acc);
}
#endif


/* Copy what the socket did not take into the chain, up to the limit. */
static proven_err_t sock_hold(proven_loop_t *l, loop_sock_t *s, const proven_byte_t *p, proven_size_t n, proven_size_t *taken) {
    *taken = 0;
    while (*taken < n) {
        const proven_u32 limit = sock_limit(s);
        if (s->queued >= limit) return PROVEN_ERR_AGAIN;
        proven_mem_mut_t room;
        proven_err_t e = sock_room(l, s, 1, &room);
        if (e != PROVEN_OK) return e;
        proven_size_t k = n - *taken;
        if (k > room.size) k = room.size;
        if (k > limit - s->queued) k = limit - s->queued;
        for (proven_size_t i = 0; i < k; ++i) room.ptr[i] = p[*taken + i];
        sock_tail(s)->used += (proven_u32)k;
        s->queued += (proven_u32)k;
        *taken += k;
    }
    return PROVEN_OK;
}

proven_result_size_t proven_loop_sendv_(proven_loop_t *l, proven_loop_sock_t *sock, const proven_mem_view_t *parts, proven_size_t count) {
    proven_result_size_t res = { .err = PROVEN_OK, .value = 0 };
    if (!l || !sock || (count > 0 && !parts) || count > PROVEN_NET_VEC_MAX) { res.err = PROVEN_ERR_INVALID_ARG; return res; }
    proven_size_t total = 0;
    for (proven_size_t i = 0; i < count; ++i) {
        if (parts[i].size > 0 && !parts[i].ptr) { res.err = PROVEN_ERR_INVALID_ARG; return res; }
        total += parts[i].size;
    }
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK || s->node.state != S_OPEN) { res.err = PROVEN_ERR_INVALID_STATE; return res; }
    if (s->send_err != PROVEN_OK) { res.err = (proven_err_t)s->send_err; return res; }
    proven_size_t gone = 0;
    if (s->queued == 0 && total > 0 && !LOOP_RING_SENDS(l)) {
        proven_net_conn_t conn = sock_conn(s);
        proven_err_t e;
        /* Small pieces are put together and sent with one plain send: cheaper than a gather
         * write's message header for the usual response of a few hundred bytes. */
        proven_byte_t small[2048];
        if (count > 1 && total <= sizeof small) {
            proven_size_t at = 0;
            for (proven_size_t i = 0; i < count; ++i) { for (proven_size_t k = 0; k < parts[i].size; ++k) small[at + k] = parts[i].ptr[k]; at += parts[i].size; }
            proven_result_size_t w = proven_net_write(&conn, (proven_mem_view_t){ .ptr = small, .size = total }, PROVEN_NET_DONT_WAIT);
            e = w.err;
            gone = w.value;
        } else if (count == 1) {
            proven_result_size_t w = proven_net_write(&conn, parts[0], PROVEN_NET_DONT_WAIT);
            e = w.err;
            gone = w.value;
        } else {
            e = proven_net_write_vec_(&conn, parts, count, &gone);
        }
        if (e != PROVEN_OK && e != PROVEN_ERR_TIMEOUT) {
            s->send_err = (proven_u16)e;
            sock_drop_chunks(l, s);
            res.value = gone;
            res.err = e;
            return res;
        }
    }
    res.value = gone;
    /* The rest, piece by piece from where the socket stopped. */
    proven_size_t at = 0;
    for (proven_size_t i = 0; i < count && res.err == PROVEN_OK; ++i) {
        const proven_size_t size = parts[i].size;
        if (at + size <= gone) { at += size; continue; }
        const proven_size_t skip = gone > at ? gone - at : 0;
        proven_size_t taken = 0;
        proven_err_t e = sock_hold(l, s, parts[i].ptr + skip, size - skip, &taken);
        res.value += taken;
        at += size;
        if (e != PROVEN_OK) res.err = e;
    }
    /* What waits was offered to the socket just now (or waits behind bytes that were): the
     * next try is the loop's - when the socket is writable, or after this callback. */
    if (s->queued > 0) {
        proven_err_t e = sock_kick(l, s);
        if (e != PROVEN_OK && res.err == PROVEN_OK) res.err = e;
    }
    return res;
}

proven_result_size_t proven_loop_send(proven_loop_t *l, proven_loop_sock_t *sock, proven_mem_view_t bytes) {
    if (bytes.size > 0 && !bytes.ptr) return (proven_result_size_t){ .err = PROVEN_ERR_INVALID_ARG, .value = 0 };
    return proven_loop_sendv_(l, sock, &bytes, 1);
}

proven_result_size_t proven_loop_read(proven_loop_t *l, proven_loop_sock_t *sock, proven_mem_mut_t buf) {
    proven_result_size_t res = { .err = PROVEN_OK, .value = 0 };
    if (!l || !sock || (buf.size > 0 && !buf.ptr)) { res.err = PROVEN_ERR_INVALID_ARG; return res; }
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK || s->node.state != S_OPEN) { res.err = PROVEN_ERR_INVALID_STATE; return res; }
#if LOOP_RING
    if (LOOP_CP(l)) {
        /* From now on the socket is received for into the pool (sock_arm). What the pool holds
         * comes first; with nothing there and nothing coming, the socket itself. */
        s->node.want |= W_READER;
        if ((s->node.want & W_MS) && buf.size > 0) {
            proven_size_t n = 0;
            const proven_sys_net_result_t r = proven_sys_net_ov_read(l->port, &s->ov, buf.ptr, buf.size, &n);
            if (r == PROVEN_SYS_NET_OK) { res.value = n; return res; }
            if (r == PROVEN_SYS_NET_WOULD_BLOCK) { res.err = PROVEN_ERR_TIMEOUT; return res; }
            if (r != PROVEN_SYS_NET_NOT_FOUND) { res.err = sys_err(r); return res; }
        }
    }
#endif
    proven_net_conn_t conn = sock_conn(s);
    return proven_net_read(&conn, buf, PROVEN_NET_DONT_WAIT);
}

proven_size_t proven_loop_ring_held_(const proven_loop_t *l) {
#if LOOP_RING
    if (l && LOOP_CP(l) && l->rbuf_state == 1) return proven_sys_net_port_pool_held(l->port);
#endif
    (void)l;
    return 0;
}

proven_size_t proven_loop_send_queued(const proven_loop_sock_t *sock) {
    if (!sock) return 0;
    const loop_sock_t *s = (const loop_sock_t *)(const void *)sock;
    return s->node.kind == KIND_SOCK ? s->queued : 0;
}

proven_err_t proven_loop_send_wait(proven_loop_t *l, proven_loop_sock_t *sock, proven_size_t at_most) {
    if (!l || !sock) return PROVEN_ERR_INVALID_ARG;
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK || s->node.state != S_OPEN) return PROVEN_ERR_INVALID_STATE;
    if (!s->fns->writable) return PROVEN_ERR_INVALID_ARG;
    if (s->queued <= at_most) {
        s->node.flags &= (proven_u8)~F_WAIT;
        node_queue(l, &s->node, D_WRITABLE);
        return PROVEN_OK;
    }
    s->wait_at = at_most > 0xffffffffu ? 0xffffffffu : (proven_u32)at_most;
    s->node.flags |= F_WAIT;
    return PROVEN_OK;
}

void proven_loop_send_limit_(proven_loop_sock_t *sock, proven_size_t bytes) {
    if (!sock) return;
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK) return;
    const proven_size_t k = (bytes + 1023u) / 1024u;            /* bytes of (proven_size_t)-1 wrap to 0: no bound either */
    s->limit_k = bytes == (proven_size_t)-1 || k >= LIMIT_NONE ? (proven_u16)LIMIT_NONE : (proven_u16)k;
}

proven_err_t proven_loop_reserve_(proven_loop_t *l, proven_loop_sock_t *sock, proven_size_t min, proven_mem_mut_t *room) {
    if (!l || !sock || !room || min > PROVEN_LOOP_CHUNK_) return PROVEN_ERR_INVALID_ARG;
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK || s->node.state != S_OPEN) return PROVEN_ERR_INVALID_STATE;
    if (s->send_err != PROVEN_OK) return (proven_err_t)s->send_err;
    if (s->queued >= sock_limit(s)) return PROVEN_ERR_AGAIN;
    return sock_room(l, s, min, room);
}

void proven_loop_commit_(proven_loop_t *l, proven_loop_sock_t *sock, proven_size_t n) {
    if (!l || !sock || n == 0) return;
    loop_sock_t *s = sock_of(sock);
    loop_chunk_t *tail = sock_tail(s);
    if (s->node.kind != KIND_SOCK || s->node.state != S_OPEN || !tail || n > PROVEN_LOOP_CHUNK_ - tail->used) return;
    tail->used += (proven_u32)n;
    s->queued += (proven_u32)n;
    node_queue(l, &s->node, D_FLUSH);
}

void proven_loop_flush_(proven_loop_t *l, proven_loop_sock_t *sock) {
    if (!l || !sock) return;
    loop_sock_t *s = sock_of(sock);
    if (s->node.kind != KIND_SOCK || s->node.state != S_OPEN || !s->head || sock_sending(l, s)) return;
    sock_flush(l, s, false);
}

// -----------------------------------------------------------------------------
// Timers
// -----------------------------------------------------------------------------

/* A timer's `tick` holds its due time in nanoseconds on the loop's clock (RFC-0019 T-1); its
 * slot is the tick of the wheel that time falls in. */
static proven_size_t timer_slot(const proven_loop_timer_t *t) { return (proven_size_t)((t->tick / LOOP_TICK_NS) % LOOP_WHEEL_SLOTS); }

static void timer_unlink(proven_loop_t *l, proven_loop_timer_t *t) {
    proven_loop_timer_t **head = t->state == TIMER_DUE ? &l->due : &l->wheel[timer_slot(t)];
    if (t->prev) t->prev->next = t->next;
    else *head = t->next;
    if (t->next) t->next->prev = t->prev;
    t->prev = (void *)0; t->next = (void *)0;
    if (t->state == TIMER_WHEEL) l->timed_count--;
    t->state = TIMER_IDLE;
}

void proven_loop_timer_cancel(proven_loop_t *l, proven_loop_timer_t *t) {
    if (!l || !t || t->state == TIMER_IDLE) return;
    timer_unlink(l, t);
}

bool proven_loop_timer_is_set(const proven_loop_timer_t *t) { return t && t->state != TIMER_IDLE; }

void proven_loop_timer_set(proven_loop_t *l, proven_loop_timer_t *t, proven_u32 ms, proven_loop_fn fn, void *ctx) {
    if (!l || !t || !fn) return;
    proven_loop_timer_cancel(l, t);
    /* Due at the time asked for (RFC-0019 T-1: no longer rounded up to the next tick); never in
     * a tick the wheel has already passed. */
    proven_u64 due = (proven_u64)loop_now(l) + (proven_u64)ms * 1000000u;
    if (due / LOOP_TICK_NS < l->wheel_tick) due = l->wheel_tick * (proven_u64)LOOP_TICK_NS;
    t->fn = fn; t->ctx = ctx; t->tick = due;
    proven_size_t slot = timer_slot(t);
    t->prev = (void *)0;
    t->next = l->wheel[slot];
    if (t->next) t->next->prev = t;
    l->wheel[slot] = t;
    t->state = TIMER_WHEEL;
    l->timed_count++;
}

/* When the next timer is due: from the slot the wheel stands in, the first slot holding a timer
 * of its own turn gives the earliest due time among those. The scan covers one whole turn and
 * the current slot again at its end (its timers of the next turn, exact). When every timer belongs
 * to a later turn - or a crowded slot has shown no timer of its own turn among the first
 * LOOP_LOOK timers looked at - the boundary of a slot after the current one: an early look, never
 * a time already past (which would spin), never late. */
#define LOOP_LOOK 32u
static proven_net_deadline_t loop_next_timer(const proven_loop_t *l) {
    if (l->timed_count == 0) return PROVEN_NET_NO_DEADLINE;
    proven_u64 first_any = 0;
    bool any = false;
    unsigned looked = 0;
    for (proven_u64 s = l->wheel_tick; s <= l->wheel_tick + LOOP_WHEEL_SLOTS; ++s) {
        const proven_loop_timer_t *t = l->wheel[s % LOOP_WHEEL_SLOTS];
        if (!t) continue;
        if (!any && s > l->wheel_tick) { any = true; first_any = s; }
        proven_u64 best = 0;
        bool found = false;
        for (; t; t = t->next) {
            if (t->tick / LOOP_TICK_NS == s && (!found || t->tick < best)) { best = t->tick; found = true; }
            if (!found && ++looked >= LOOP_LOOK) return (proven_net_deadline_t)((s > l->wheel_tick ? s : l->wheel_tick + 1) * LOOP_TICK_NS);
        }
        if (found) return (proven_net_deadline_t)best;
    }
    return any ? (proven_net_deadline_t)(first_any * LOOP_TICK_NS) : PROVEN_NET_NO_DEADLINE;
}

static void loop_timers_advance(proven_loop_t *l) {
    const proven_u64 now = (proven_u64)loop_now(l);
    proven_u64 target = now / LOOP_TICK_NS;
    if (target < l->wheel_tick) target = l->wheel_tick;
    if (l->timed_count > 0) {
        /* From the slot the wheel stands in (its timers may have come due within the tick) to
         * the one `now` falls in; after a long sleep every slot is visited once. */
        proven_u64 from = target - l->wheel_tick >= LOOP_WHEEL_SLOTS ? target - LOOP_WHEEL_SLOTS + 1 : l->wheel_tick;
        /* Slots are visited in the order of their times, and each timer that is due goes into
         * the due list by its due time, so that timers which come due together fire earliest
         * first - within a slot too. Walked back from the tail: they mostly come in order. */
        proven_loop_timer_t *tail = l->due;
        while (tail && tail->next) tail = tail->next;
        for (proven_u64 s = from; s <= target && l->timed_count > 0; ++s) {
            proven_loop_timer_t *t = l->wheel[s % LOOP_WHEEL_SLOTS];
            while (t) {
                proven_loop_timer_t *next = t->next;
                if (t->tick <= now) {
                    timer_unlink(l, t);
                    proven_loop_timer_t *after = tail;
                    while (after && after->tick > t->tick) after = after->prev;
                    t->prev = after;
                    t->next = after ? after->next : l->due;
                    if (t->next) t->next->prev = t;
                    else tail = t;
                    if (after) after->next = t;
                    else l->due = t;
                    t->state = TIMER_DUE;
                }
                t = next;
            }
        }
    }
    l->wheel_tick = target;
    /* Fire them one at a time from the list's head: a callback may cancel any of the others. */
    while (l->due) {
        proven_loop_timer_t *t = l->due;
        proven_loop_fn fn = t->fn;
        void *ctx = t->ctx;
        timer_unlink(l, t);
        fn(ctx);
    }
}

// -----------------------------------------------------------------------------
// Posting, running
// -----------------------------------------------------------------------------

/* One post to the port at a time: the flag is cleared when the post is taken, before the posts
 * are looked at, so that one posted meanwhile posts again. */
static void loop_wake(proven_loop_t *l) {
#if LOOP_HAS_CP
    if (LOOP_CP(l)) {
        if (atomic_exchange(&l->woken, true)) return;
        if (proven_sys_net_port_post(l->port) != PROVEN_SYS_NET_OK) atomic_store(&l->woken, false);
        return;
    }
#endif
#if LOOP_HAS_SEL
    proven_net_waker_wake(&l->waker);
#endif
}

proven_err_t proven_loop_post(proven_loop_t *l, proven_loop_fn fn, void *ctx) {
    if (!l || !fn) return PROVEN_ERR_INVALID_ARG;
    proven_result_mem_mut_t m = l->alloc.alloc_fn(l->alloc.ctx, sizeof(proven_loop_task_t), 16);
    if (m.err != PROVEN_OK) return m.err;
    proven_loop_task_t *t = (proven_loop_task_t *)m.value.ptr;
    t->fn = fn; t->ctx = ctx;
    t->next = atomic_load_explicit(&l->tasks, memory_order_relaxed);
    while (!atomic_compare_exchange_weak_explicit(&l->tasks, &t->next, t, memory_order_release, memory_order_relaxed)) { }
    loop_wake(l);
    return PROVEN_OK;
}

static bool loop_run_tasks(proven_loop_t *l) {
    proven_loop_task_t *t = atomic_exchange_explicit(&l->tasks, (proven_loop_task_t *)0, memory_order_acquire);
    if (!t) return false;
    /* The stack holds the newest first; reverse it to call in the order of posting. */
    proven_loop_task_t *ordered = (void *)0;
    while (t) { proven_loop_task_t *next = t->next; t->next = ordered; ordered = t; t = next; }
    while (ordered) {
        proven_loop_task_t *next = ordered->next;
        proven_loop_fn fn = ordered->fn;
        void *ctx = ordered->ctx;
        l->alloc.free_fn(l->alloc.ctx, ordered);
        fn(ctx);
        ordered = next;
    }
    return true;
}

void proven_loop_stop(proven_loop_t *l) {
    if (!l) return;
    atomic_store_explicit(&l->stop, true, memory_order_release);
    loop_wake(l);
}

void proven_loop_after_(proven_loop_t *l, proven_loop_after_t *a, proven_loop_fn fn, void *ctx) {
    if (!l || !a || !fn || a->waiting) return;
    a->fn = fn; a->ctx = ctx; a->waiting = true;
    a->next = (void *)0; a->prev = l->after_last;
    if (l->after_last) l->after_last->next = a; else l->after_first = a;
    l->after_last = a;
    l->after_count++;
}

void proven_loop_after_cancel_(proven_loop_t *l, proven_loop_after_t *a) {
    if (!l || !a || !a->waiting) return;
    if (a->prev) a->prev->next = a->next; else l->after_first = a->next;
    if (a->next) a->next->prev = a->prev; else l->after_last = a->prev;
    a->waiting = false;
    a->next = a->prev = (void *)0;
    l->after_count--;
}

/* As many as are waiting now, in order; one asked for meanwhile waits for the next call. Each
 * is taken off the list before it runs, so it may ask again or be cancelled by another. */
static void loop_run_after(proven_loop_t *l) {
    for (proven_size_t n = l->after_count; n > 0 && l->after_first; --n) {
        proven_loop_after_t *a = l->after_first;
        proven_loop_after_cancel_(l, a);
        a->fn(a->ctx);
    }
}

proven_err_t proven_loop_poll(proven_loop_t *l, proven_net_deadline_t until) {
    if (!l) return PROVEN_ERR_INVALID_ARG;
    /* What was asked for between rounds runs before the loop sleeps; what that asks for in
     * its turn must not wait behind the sleep either. */
    loop_run_after(l);
    loop_run_done(l);
    if (l->after_first || l->done_first) until = PROVEN_NET_DONT_WAIT;
    /* Work posted before this round must not wait behind a sleep. */
    if (loop_run_tasks(l)) until = PROVEN_NET_DONT_WAIT;
    proven_net_deadline_t next = loop_next_timer(l);
    if (next != PROVEN_NET_NO_DEADLINE && (until == PROVEN_NET_NO_DEADLINE || (until != PROVEN_NET_DONT_WAIT && next < until))) until = next;
    proven_size_t count = 0;
#if LOOP_HAS_CP
    if (LOOP_CP(l)) {
        const proven_sys_net_result_t r = proven_sys_net_port_wait(l->port, l->cp_batch, LOOP_EVENTS, proven_net_remaining_ms_(until), &count);
        if (r != PROVEN_SYS_NET_OK) return proven_net_err_from_sys_(r);
        for (proven_size_t i = 0; i < count; ++i) loop_report(l, &l->cp_batch[i]);
        /* An accept or a receive armed again from its own function is answered by the port, not
         * the done list: look at the port again without waiting, up to LOOP_DONE_PASSES times, so
         * many connections waiting are taken in one round as the selector's drain would. */
        for (int pass = 1; pass < LOOP_DONE_PASSES && count > 0; ++pass) {
            loop_run_done(l);
            if (proven_sys_net_port_wait(l->port, l->cp_batch, LOOP_EVENTS, 0, &count) != PROVEN_SYS_NET_OK) break;
            for (proven_size_t i = 0; i < count; ++i) loop_report(l, &l->cp_batch[i]);
        }
    }
#endif
#if LOOP_HAS_SEL
    if (!LOOP_CP(l)) {
        proven_err_t e = proven_net_selector_wait(l->selector, l->batch, LOOP_EVENTS, until, &count);
        if (e != PROVEN_OK && e != PROVEN_ERR_TIMEOUT) return e;
        l->batch_count = count;
        for (l->batch_pos = 0; l->batch_pos < l->batch_count; ) {
            proven_net_ready_t ev = l->batch[l->batch_pos++];
            if (!ev.tag) continue;
            switch (*(const proven_u8 *)ev.tag) {
            case KIND_WAKER: proven_net_waker_drain(&l->waker); break;
            case KIND_SOCK: sock_event(l, (loop_sock_t *)ev.tag, ev.got); break;
            case KIND_ACCEPT: acc_event(l, (loop_acc_t *)ev.tag, ev.got); break;
            case KIND_UDP: udp_event(l, (loop_udp_t *)ev.tag, ev.got); break;
            default: break;
            }
        }
        l->batch_count = 0; l->batch_pos = 0;
    }
#endif
    loop_run_done(l);
    loop_timers_advance(l);
    loop_run_done(l);
    (void)loop_run_tasks(l);
    loop_run_done(l);
    loop_run_after(l);
    loop_run_after(l);
    loop_run_done(l);
    return PROVEN_OK;
}

proven_err_t proven_loop_run(proven_loop_t *l) {
    if (!l) return PROVEN_ERR_INVALID_ARG;
    while (!atomic_load_explicit(&l->stop, memory_order_acquire)) {
        proven_err_t e = proven_loop_poll(l, PROVEN_NET_NO_DEADLINE);
        if (e != PROVEN_OK) return e;
    }
    atomic_store_explicit(&l->stop, false, memory_order_release);     /* a loop can be run again */
    return PROVEN_OK;
}

#endif
