#ifndef RUBRAPACK_TLS_H
#define RUBRAPACK_TLS_H

// include/rubrapack/tls.h - a TLS 1.3 client (RFC 8446; RFC-0008 T3) for https timestamp servers,
// over proven_c_lib's TLS engine (src/net/tls.c).
//
// The server's certificate must reach one of the given trust anchors at `now`, name the host and
// be for TLS servers. TLS 1.3 only (a clear error for an older server); no resumption, 0-RTT or
// client certificates, and no option that skips a check.
//
// The client does no I/O ("sans-IO"): the caller moves bytes between it and a socket, which keeps
// proxies and tests outside it.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/crypto.h"
#include "rubrapack/der.h"

// ---- the client ---------------------------------------------------------------------------------

typedef struct {
    const char          *host;              // SNI (a DNS name) and the name the certificate must carry
    const rp_der_span_t *anchors;           // trust anchors (DER certificates)
    size_t               anchor_count;
    int64_t              now;               // seconds since 1970: when the certificates must be valid
} rp_tls_config_t;

typedef struct rp_tls rp_tls_t;

// Starts a handshake: the ClientHello is pending to be sent.
[[nodiscard]] proven_err_t rp_tls_new(proven_allocator_t alloc, const rp_tls_config_t *cfg, rp_tls_t **out, const char **why);
void rp_tls_free(rp_tls_t *t);

// The bytes to send next (a pointer into the client, valid until the next call), and how many of
// them were sent.
size_t rp_tls_pending(rp_tls_t *t, const uint8_t **data);
void rp_tls_sent(rp_tls_t *t, size_t n);

// Gives the client bytes received from the server. An error ends the connection (a fatal alert is
// then pending); *why says what went wrong.
[[nodiscard]] proven_err_t rp_tls_feed(rp_tls_t *t, const uint8_t *in, size_t len, const char **why);

bool rp_tls_connected(const rp_tls_t *t);   // the handshake is complete
bool rp_tls_eof(const rp_tls_t *t);         // the server sent close_notify

// Application data: queue bytes to send (encrypted into pending records), and take received ones.
[[nodiscard]] proven_err_t rp_tls_write(rp_tls_t *t, const uint8_t *data, size_t len);
size_t rp_tls_read(rp_tls_t *t, uint8_t *buf, size_t cap);

// Queues close_notify.
void rp_tls_close(rp_tls_t *t);

#endif // RUBRAPACK_TLS_H
