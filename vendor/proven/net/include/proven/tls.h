#ifndef PROVEN_TLS_H
#define PROVEN_TLS_H

#include "proven/types.h"
#include "proven/error.h"
#include "proven/memory.h"
#include "proven/allocator.h"
#include "proven/u8str.h"
#include "proven/cert.h"
#ifndef PROVEN_FREESTANDING
#ifndef PROVEN_NO_NET
#include "proven/net.h"
#endif
#endif

/**
 * @file tls.h
 * @brief TLS 1.3 (RFC 8446) and a cut-down TLS 1.2 (RFC 5246), client and server.
 *
 * TLS turns a connection anyone on the path can read and change into one that only the two
 * ends can: the peer is who its certificate says, and what is sent arrives unread and
 * unaltered or not at all.
 *
 * There are two layers here, and most programs want the second:
 *
 *   - **The engine** (`proven_tls_conn_t`) is a state machine with no I/O in it. You hand it
 *     the bytes that arrived, it hands you the bytes to send, and between the two it keeps the
 *     keys. It calls no operating system, so it can be tested without a network, driven from
 *     an event loop, and used in a freestanding build that has its own transport.
 *   - **The transport wrapper** (`proven_tls_transport_client`, `proven_tls_transport_server`)
 *     runs the engine over a `proven_transport_t` and is itself a `proven_transport_t`:
 *     anything that reads and writes a transport - the HTTP client and server, a WebSocket
 *     connection - then speaks TLS without knowing it. Hosted only.
 *
 * Both take a `proven_tls_config_t`: whom to believe, who you are, and a few choices. A config
 * is built once, never changes, and is shared by as many connections and threads as you like.
 *
 * **What this is not.** A new implementation with no external audit (the manual says what it
 * was tested against). Of TLS 1.2 only the ECDHE and AEAD suites, with the extended master
 * secret required and renegotiation declined; what is older exists only behind the
 * `legacy` bits of the options, each off by default. 0-RTT only over QUIC (HTTP/3), and only
 * with `early_data`. No revocation checking. A program whose users depend on it against a
 * capable adversary should terminate TLS in something audited.
 */

/** @brief Fill `out` with `len` unpredictable bytes. Must not fail. */
typedef void (*proven_tls_random_fn)(void *ctx, proven_byte_t *out, proven_size_t len);
/** @brief The time, in seconds since 1970-01-01 UTC. */
typedef proven_i64 (*proven_tls_now_fn)(void *ctx);

/** @brief Protocol versions, as `min_version` and `max_version` take them and as
 *         proven_tls_version reports. */
#define PROVEN_TLS_VERSION_1_0 ((proven_u16)0x0301)   /**< legacy: only with PROVEN_TLS_LEGACY_CBC */
#define PROVEN_TLS_VERSION_1_1 ((proven_u16)0x0302)   /**< legacy: only with PROVEN_TLS_LEGACY_CBC */
#define PROVEN_TLS_VERSION_1_2 ((proven_u16)0x0303)
#define PROVEN_TLS_VERSION_1_3 ((proven_u16)0x0304)

/** @brief Bits for `legacy` in the options: parts of TLS that are kept out unless a
 *         configuration names them, because each has a known weakness (the manual says which).
 *         They exist to reach a peer that has nothing better. */
#define PROVEN_TLS_LEGACY_CBC              ((proven_u32)1)  /**< CBC cipher suites with HMAC-SHA1, -SHA256 or -SHA384 */
#define PROVEN_TLS_LEGACY_RSA_KEY_EXCHANGE ((proven_u32)2)  /**< key exchange by RSA encryption: no forward secrecy. A client's only: proven_tls_server_create refuses a config with it */
#define PROVEN_TLS_LEGACY_DHE              ((proven_u32)4)  /**< finite-field Diffie-Hellman key exchange */
#define PROVEN_TLS_LEGACY_NO_EXTENDED_MASTER_SECRET ((proven_u32)8) /**< go on with a peer that lacks RFC 7627 */

typedef enum {
    PROVEN_TLS_CLIENT_AUTH_NONE = 0,     /**< A server does not ask clients for a certificate. */
    PROVEN_TLS_CLIENT_AUTH_REQUEST,      /**< It asks; a client without one is still let in. */
    PROVEN_TLS_CLIENT_AUTH_REQUIRE       /**< It asks; a client without a valid one is refused. */
} proven_tls_client_auth_t;

typedef enum {
    /** A chain from the peer's certificate to `anchors`, valid now, for the expected name. If
     *  pins are given, the peer's key must ALSO be one of them. */
    PROVEN_TLS_VERIFY_CHAIN = 0,
    /** No chain, no dates, no name: the peer's key must be one of the pins, and that is all.
     *  For a peer you operate yourself. */
    PROVEN_TLS_VERIFY_PIN_ONLY
} proven_tls_verify_t;

/**
 * @brief What a config is made from. Zero-initialise it and set what you need; `alloc` is
 *        always required. Everything given by pointer is copied by proven_tls_config_create
 *        except `anchors`, which must outlive the config.
 *
 * `anchors`, `verify` and `pins` describe how THE PEER is verified, whichever role this side
 * plays: a client always verifies the server; a server verifies a client only when
 * `client_auth` is not NONE. There is no setting that verifies nothing.
 */
typedef struct {
    proven_allocator_t alloc;                /**< required */
    const proven_cert_store_t *anchors;      /**< required whenever this side verifies with VERIFY_CHAIN */
    proven_tls_verify_t verify;
    const proven_byte_t (*pins)[32];         /**< SHA-256 of SubjectPublicKeyInfo, as proven_cert_key_sha256 gives */
    proven_size_t pin_count;
    proven_mem_view_t certificate_pem;       /**< this side's certificates, its own first. A server needs one. */
    proven_mem_view_t private_key_pem;       /**< its key, not encrypted: P-256 or Ed25519 or RSA (2048 to 4096 bits), as PKCS #8 ("PRIVATE KEY"), "EC PRIVATE KEY" or "RSA PRIVATE KEY" */
    proven_tls_client_auth_t client_auth;    /**< server only */
    const proven_u8str_view_t *alpn;         /**< application protocols, most preferred first; none: ALPN is not used */
    proven_size_t alpn_count;
    proven_tls_random_fn random;             /**< NULL: the operating system's source. Required when freestanding. */
    void *random_ctx;
    proven_tls_now_fn now;                   /**< NULL: the wall clock. Required when freestanding. */
    void *now_ctx;
    bool no_resumption;                      /**< a server issues no tickets; a client offers none */
    bool early_data;                         /**< 0-RTT (manual chapter 14, section 6): for HTTP/3, and over TCP for the HTTP servers and clients of this library (HTTP/1.1, HTTP/2). A server's tickets allow early data and it takes it - once per ticket, fresh tickets only (a register of 4,096, 96 KiB); a client sends it with a session that allows it, and keeps the newest session of eight server names for the transport wrapper. A program that drives the engine itself neither sends nor takes it over TCP. Early data can be replayed: see the manual. Not with `no_resumption` */
    bool keep_peer_certificate;              /**< keep the peer's certificate for proven_tls_peer_certificate (costs its size per connection) */
    proven_u32 ticket_lifetime_s;            /**< server: how long a ticket is honoured; 0: 7200; at most 604800 */
    proven_size_t max_handshake_bytes;       /**< the largest handshake message accepted; 0: 65536 */
    proven_u16 min_version;                  /**< the oldest protocol version to agree to; 0: PROVEN_TLS_VERSION_1_2. Below 1.2 needs PROVEN_TLS_LEGACY_CBC */
    proven_u16 max_version;                  /**< the newest; 0: PROVEN_TLS_VERSION_1_3 */
    proven_u32 legacy;                       /**< PROVEN_TLS_LEGACY_* bits; 0: none of it */
} proven_tls_options_t;

/** @brief A configuration. Opaque, immutable, shareable between threads. */
typedef struct proven_tls_config proven_tls_config_t;

/**
 * @brief Build a config. Everything that can be wrong with the options is found here, not at
 *        the first handshake.
 * @return PROVEN_ERR_INVALID_ARG for a missing allocator, anchors missing where this side
 *         verifies a chain, PIN_ONLY without pins, a certificate without a key or a key
 *         without a certificate, more than 8 certificates, an ALPN name that is empty or
 *         longer than 255 bytes, a missing `random` or `now` in a freestanding build;
 *         PROVEN_ERR_INVALID_FORMAT for PEM that does not parse or holds no certificate or
 *         key, and for an RSA key that is not one this library signs with (not 2048 to 4096
 *         bits, not two primes whose product is its modulus, or one that fails to sign);
 *         PROVEN_ERR_UNSUPPORTED for a key of another kind (P-384, say) or an encrypted key
 *         file; PROVEN_ERR_INVALID_STATE when the key is not the certificate's;
 *         PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_err_t proven_tls_config_create(const proven_tls_options_t *options, proven_tls_config_t **out);

/** @brief Free a config and wipe its private key. No connection may still use it. Null is accepted. */
void proven_tls_config_destroy(proven_tls_config_t *config);

/** @brief Bytes in a proven_tls_session_t. */
#define PROVEN_TLS_SESSION_SIZE ((proven_size_t)2048)

/**
 * @brief A client's memory of one server, for resumption. Yours to keep: a plain block with no
 *        pointers in it. Zero-initialise it for "nothing remembered".
 *
 * Give the same session to proven_tls_client_create for each connection to the same server
 * name. The connection writes into it when the server sends a ticket, and the next connection
 * offers that ticket and, if the server accepts, skips the certificates and one signature.
 * It holds a secret: wipe it (proven_mem_wipe) when you are done with it. Not for use by two
 * connections at once. Over QUIC it also keeps what early data needs (the protocol, the
 * server's limits) and the server's NEW_TOKEN token. 2,048 bytes since v0.73.0 (1,024 before):
 * a session written by an older version reads as empty.
 */
typedef struct {
    alignas(8) proven_byte_t opaque[PROVEN_TLS_SESSION_SIZE];   /* aligned for the record the engine keeps in it */
} proven_tls_session_t;

/** @brief One connection's state. Opaque. Used by one thread at a time. */
typedef struct proven_tls_conn proven_tls_conn_t;

/**
 * @brief A connection in the client role. Its first message is in the pending output at once.
 * @param server_name the name you mean to reach: sent to the server and checked against its
 *        certificate. A DNS name or an IP literal; required unless the config is PIN_ONLY.
 * @param session optional; must outlive the connection.
 * @return PROVEN_ERR_INVALID_ARG (also for a server_name longer than 253 bytes),
 *         PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_err_t proven_tls_client_create(const proven_tls_config_t *config, proven_u8str_view_t server_name,
                                      proven_tls_session_t *session, proven_tls_conn_t **out);

/**
 * @brief A connection in the server role. It waits for the client's first message.
 * @return PROVEN_ERR_INVALID_ARG (also for a config with no certificate, and for one with
 *         PROVEN_TLS_LEGACY_RSA_KEY_EXCHANGE: that key exchange is a client's only, from
 *         v0.78.0), PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_err_t proven_tls_server_create(const proven_tls_config_t *config, proven_tls_conn_t **out);

/** @brief Free a connection and wipe its keys. Sends nothing. Null is accepted. */
void proven_tls_conn_destroy(proven_tls_conn_t *conn);

/**
 * @brief Give the connection bytes that arrived from the peer.
 *
 * It takes whole records as far as it can use them and sets `*consumed`. **It may take fewer
 * than you gave**: when decrypted application data is waiting, it stops until you have read
 * it. So loop - feed, read everything, feed the rest - until `*consumed == in.size`. Bytes of
 * an incomplete record are kept inside; you need not hold them.
 *
 * @return PROVEN_OK; or the reason the connection has failed, once: PROVEN_ERR_UNTRUSTED,
 *         PROVEN_ERR_EXPIRED, PROVEN_ERR_NOT_YET_VALID, PROVEN_ERR_NAME_MISMATCH for the
 *         peer's certificate, PROVEN_ERR_PROTOCOL for everything else the peer did wrong or
 *         for a fatal alert it sent, PROVEN_ERR_NOMEM. The alert that tells the peer is then
 *         in the pending output: send it. After a failure every call but
 *         proven_tls_pending_output, proven_tls_output_sent and the queries returns
 *         PROVEN_ERR_INVALID_STATE.
 */
[[nodiscard]]
proven_err_t proven_tls_feed(proven_tls_conn_t *conn, proven_mem_view_t in, proven_size_t *consumed);

/**
 * @brief The bytes the connection wants sent to the peer: handshake messages, records, alerts.
 *        Empty when there are none. The view is valid until the next call on this connection.
 */
proven_mem_view_t proven_tls_pending_output(proven_tls_conn_t *conn);

/** @brief Say that the first `n` bytes of the pending output were sent. */
void proven_tls_output_sent(proven_tls_conn_t *conn, proven_size_t n);

/** @brief True once the handshake has completed and application data may flow. */
bool proven_tls_is_established(const proven_tls_conn_t *conn);

/**
 * @brief Take decrypted application data.
 * @return the number of bytes copied; PROVEN_ERR_NEED_MORE when there is none yet (feed more);
 *         PROVEN_ERR_EOF when the peer has closed its side properly and everything was read;
 *         PROVEN_ERR_INVALID_STATE after a failure.
 */
[[nodiscard]]
proven_result_size_t proven_tls_read(proven_tls_conn_t *conn, proven_mem_mut_t dest);

/**
 * @brief Encrypt application data into the pending output.
 *
 * Takes all of `src` unless the pending output would grow past 64 KiB of unsent records, in
 * which case it takes what fits - possibly nothing - and you send some output and call again.
 *
 * @return the number of bytes taken; PROVEN_ERR_INVALID_STATE before the handshake is
 *         complete, after proven_tls_close, or after a failure; PROVEN_ERR_NOMEM.
 */
[[nodiscard]]
proven_result_size_t proven_tls_write(proven_tls_conn_t *conn, proven_mem_view_t src);

/**
 * @brief Close this side: a close_notify alert goes into the pending output, and nothing more
 *        can be written. Reading continues until the peer closes too. Calling it twice is
 *        harmless.
 */
proven_err_t proven_tls_close(proven_tls_conn_t *conn);

/** @brief Replace this side's sending key with the next one (RFC 8446 section 4.6.3). */
proven_err_t proven_tls_key_update(proven_tls_conn_t *conn);

/** @brief The cipher suite agreed, or 0 before the ServerHello: 0x1301, 0x1302 or 0x1303 in
 *         TLS 1.3; in TLS 1.2 one of the six ECDHE suites with AES-GCM or ChaCha20-Poly1305,
 *         or a legacy suite where the configuration allowed one (the manual lists them). */
proven_u16 proven_tls_cipher_suite(const proven_tls_conn_t *conn);

/** @brief The protocol version agreed - PROVEN_TLS_VERSION_1_3 or PROVEN_TLS_VERSION_1_2, or
 *         for a configuration that allowed them one of the two legacy versions - or 0 before
 *         it is known. */
proven_u16 proven_tls_version(const proven_tls_conn_t *conn);
/** @brief The application protocol agreed by ALPN; empty when none was. Points into the config. */
proven_u8str_view_t proven_tls_alpn(const proven_tls_conn_t *conn);
/** @brief True when the handshake resumed an earlier session instead of exchanging certificates. */
bool proven_tls_resumed(const proven_tls_conn_t *conn);
/**
 * @brief SHA-256 of the peer's public key, as proven_cert_key_sha256 gives it. On a resumed
 *        connection it is the key of the original handshake.
 * @return false when the peer presented no certificate.
 */
bool proven_tls_peer_key_sha256(const proven_tls_conn_t *conn, proven_byte_t out[32]);
/** @brief The peer's certificate, DER. Empty unless the config set `keep_peer_certificate`,
 *         and on a resumed connection. Valid until the connection is destroyed. */
proven_mem_view_t proven_tls_peer_certificate(const proven_tls_conn_t *conn);
/** @brief Why the peer's certificate was refused; PROVEN_CERT_FAULT_NONE when it was not. */
proven_cert_fault_t proven_tls_peer_fault(const proven_tls_conn_t *conn);
/** @brief Server role: the name the client asked for (SNI). Empty when it sent none, and after
 *         the handshake. */
proven_u8str_view_t proven_tls_server_name(const proven_tls_conn_t *conn);
/** @brief The alert the peer sent, as its number from RFC 8446 section 6; -1 when none. */
int proven_tls_alert_received(const proven_tls_conn_t *conn);
/** @brief The alert this side sent when it failed; -1 when none. */
int proven_tls_alert_sent(const proven_tls_conn_t *conn);

#ifndef PROVEN_FREESTANDING
#ifndef PROVEN_NO_NET

/**
 * @brief Run the client handshake over `plain` and return an encrypted transport.
 *
 * On success `*out` owns `plain`: closing it sends close_notify and closes `plain`. On failure
 * `plain` is left open and yours.
 *
 * `session` NULL with a configuration that has `early_data`: the configuration's newest session
 * for `server_name` is resumed and the one this connection ends with is kept for the next. When
 * that session allows early data, this returns without sending anything: the ClientHello goes
 * with the first write, which goes with it as early data - unless it may not (the HTTP client
 * says so for what is not safe to repeat); the handshake is completed by the reads and writes
 * that follow, and what the server refused is sent again (manual chapter 14, section 6).
 *
 * @return what proven_tls_feed returns, and the transport's own errors (PROVEN_ERR_TIMEOUT when
 *         `until` passes, PROVEN_ERR_RESET, PROVEN_ERR_EOF for a peer that hung up mid-handshake).
 */
[[nodiscard]]
proven_err_t proven_tls_transport_client(proven_transport_t plain, const proven_tls_config_t *config,
                                         proven_u8str_view_t server_name, proven_tls_session_t *session,
                                         proven_net_deadline_t until, proven_transport_t *out);

/** @brief The same in the server role. */
[[nodiscard]]
proven_err_t proven_tls_transport_server(proven_transport_t plain, const proven_tls_config_t *config,
                                         proven_net_deadline_t until, proven_transport_t *out);

/** @brief The engine inside a transport made by the two calls above, for the queries; NULL for
 *         any other transport. */
proven_tls_conn_t *proven_tls_transport_conn(proven_transport_t tls);

/**
 * @brief A `proven_http_tls_wrap_fn` for the HTTP client: set `tls_wrap` to this and `tls_ctx`
 *        to a `proven_tls_config_t *`, and `https` URLs work. Connections made this way resume
 *        earlier sessions, and send a GET, HEAD or OPTIONS without a body as early data, only
 *        when the configuration has `early_data`.
 */
[[nodiscard]]
proven_err_t proven_tls_http_wrap(void *config, proven_transport_t plain, proven_u8str_view_t host,
                                  proven_net_deadline_t until, proven_transport_t *out);

#endif /* PROVEN_NO_NET */
#endif /* PROVEN_FREESTANDING */

#endif /* PROVEN_TLS_H */
