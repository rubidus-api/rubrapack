#ifndef RUBRAPACK_TLS_H
#define RUBRAPACK_TLS_H

// include/rubrapack/tls.h - a TLS 1.3 client (RFC 8446; RFC-0008 T3) for https timestamp servers.
//
// Scope: TLS_AES_128_GCM_SHA256 and TLS_AES_256_GCM_SHA384; key shares x25519 and secp256r1 (one
// HelloRetryRequest); server signatures rsa_pss_rsae_* and ecdsa_secp256r1/384r1 in
// CertificateVerify, PKCS#1 and ECDSA on the certificate path. The server's certificate must reach
// one of the given trust anchors at `now`, name the host (rp_cert_names_host) and be for TLS
// servers. No TLS 1.2 (a clear error), no PSK, 0-RTT, resumption or client certificates, and no
// option that skips a check.
//
// The client does no I/O ("sans-IO"): the caller moves bytes between it and a socket, which keeps
// proxies, tests and fuzzing outside it. The building blocks below it (key schedule, record
// protection, CertificateVerify) are exported so that tests can hold them against RFC 8448's traces.

#include <stddef.h>
#include <stdint.h>

#include "proven/allocator.h"
#include "proven/types.h"
#include "rubrapack/crypto.h"
#include "rubrapack/der.h"

// ---- key schedule (RFC 8446 7.1) --------------------------------------------------------------

// HKDF-Expand-Label(secret, "tls13 " + label, context, len). False when the label or length does
// not fit.
[[nodiscard]] bool rp_tls_expand_label(rp_hash_alg_t alg, const uint8_t *secret, const char *label, const uint8_t *ctx, size_t ctx_len,
                                       uint8_t *out, size_t len);

typedef struct {
    rp_hash_alg_t alg;
    size_t        hl, key_len;              // hash length; AEAD key length (16 or 32)
    uint8_t       hs[RP_HASH_MAX], master[RP_HASH_MAX];
    uint8_t       c_hs[RP_HASH_MAX], s_hs[RP_HASH_MAX], c_ap[RP_HASH_MAX], s_ap[RP_HASH_MAX];
} rp_tls_keys_t;

// From the (EC)DHE shared secret and the transcript hash through ServerHello: the handshake
// secret, both handshake traffic secrets and the master secret (no PSK: the early secret is
// Extract(0, 0)).
void rp_tls_keys_handshake(rp_tls_keys_t *k, rp_hash_alg_t alg, size_t key_len, const uint8_t *shared, size_t shared_len,
                           const uint8_t *th);
// The application traffic secrets, from the transcript hash through the server's Finished.
void rp_tls_keys_app(rp_tls_keys_t *k, const uint8_t *th);
// verify_data of a Finished message: HMAC(finished_key(base_key), transcript hash).
void rp_tls_finished(rp_hash_alg_t alg, const uint8_t *base_key, const uint8_t *th, uint8_t *out);

// ---- record protection (RFC 8446 5.2, 5.3) ------------------------------------------------------

typedef struct {
    rp_gcm_t gcm;
    uint8_t  iv[12];
    uint64_t seq;
} rp_tls_aead_t;

enum { RP_TLS_MAX_PLAIN = 16384, RP_TLS_MAX_CIPHER = 16384 + 256 };

// Keys and IV from a traffic secret; the sequence number starts at 0.
[[nodiscard]] bool rp_tls_aead_init(rp_tls_aead_t *a, rp_hash_alg_t alg, size_t key_len, const uint8_t *secret);
// A whole record (header, ciphertext, tag) for `len` <= RP_TLS_MAX_PLAIN bytes of content `type`;
// `out` has room for len + 22 bytes.
void rp_tls_seal(rp_tls_aead_t *a, uint8_t type, const uint8_t *in, size_t len, uint8_t *out, size_t *out_len);
// Opens a whole record (5-byte header included) into `out` (room for rec_len bytes): the content
// and its real type, padding removed. False for a record that does not authenticate or is malformed.
[[nodiscard]] bool rp_tls_open(rp_tls_aead_t *a, const uint8_t *rec, size_t rec_len, uint8_t *out, size_t *out_len, uint8_t *type);

// ---- CertificateVerify (RFC 8446 4.4.3) --------------------------------------------------------

// Checks the server's CertificateVerify signature `sig` under `scheme` over the transcript hash `th`
// (hl bytes) with the leaf certificate's key: rsa_pss_rsae_sha256/384/512 (salt = hash length) or
// ecdsa_secp256r1_sha256 / ecdsa_secp384r1_sha384 with a key on that curve. RSASSA-PKCS1 and
// rsa_pss_pss_* are refused.
[[nodiscard]] bool rp_tls_cv_check(const uint8_t *leaf, size_t leaf_len, uint16_t scheme, const uint8_t *sig, size_t sig_len,
                                   const uint8_t *th, size_t hl, const char **why);

// ---- the client ---------------------------------------------------------------------------------

typedef struct {
    const char          *host;              // SNI (a DNS name) and the name the certificate must carry
    const rp_der_span_t *anchors;           // trust anchors (DER certificates)
    size_t               anchor_count;
    int64_t              now;               // seconds since 1970: when the certificates must be valid
    const uint8_t       *test_random;       // tests only: 128 bytes (client random, session id,
                                            // x25519 key, P-256 key) instead of the system's
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
