#ifndef PROVEN_INTERNAL_NET_H
#define PROVEN_INTERNAL_NET_H

/*
 * Internal: net.c calls for the event loop. Not public API.
 */

#include "proven/net.h"

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)
/* A gather write that never waits: up to PROVEN_NET_VEC_MAX of `bufs` in order, in one system
 * call. `*sent` bytes went, which may end inside any buffer; PROVEN_ERR_TIMEOUT when the socket
 * took nothing because it would block; otherwise as proven_net_write. */
#define PROVEN_NET_VEC_MAX 16
proven_err_t proven_net_write_vec_(proven_net_conn_t *conn, const proven_mem_view_t *bufs, proven_size_t count, proven_size_t *sent);
/* How a connect begun by proven_net_connect_start stands, as proven_net_connect_finish says,
 * except that a failed one is not closed: the loop closes nothing of its caller's. */
proven_err_t proven_net_connect_result_(const proven_net_conn_t *conn);
/* Tests only: every send offers at most `cap` bytes (0: no cap), so that a send cut at a chosen
 * byte can be made on demand. Not thread-safe; set it around a test's own sends. */
void proven_net_write_cap_for_test_(proven_size_t cap);

#if defined(_WIN32) || defined(_WIN64) || defined(__linux__)
/* For the loop's completion port (RFC-0025 step 6; io_uring on Linux, RFC-0029), which talks to the platform layer itself:
 * a reason as an error, addresses both ways, and the milliseconds left until a deadline (-1:
 * none; 0: passed; rounded up). */
#include "../../platform/proven_sys_net.h"
proven_err_t proven_net_err_from_sys_(proven_sys_net_result_t r);
void proven_net_addr_to_sys_(const proven_net_addr_t *a, proven_sys_net_addr_t *s);
void proven_net_addr_from_sys_(const proven_sys_net_addr_t *s, proven_net_addr_t *a);
int proven_net_remaining_ms_(proven_net_deadline_t until);
#endif
#endif

#endif /* PROVEN_INTERNAL_NET_H */
