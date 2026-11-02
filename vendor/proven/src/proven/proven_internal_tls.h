#ifndef PROVEN_INTERNAL_TLS_H
#define PROVEN_INTERNAL_TLS_H

/* The pieces of TLS 1.3 (RFC 8446) below the state machine: cipher suites, the transcript
 * hash, the key schedule and record protection. Internal; tests reach it by path. */

#include "proven/types.h"
#include "proven/memory.h"
#include "proven/hash.h"
#include "proven/hmac.h"
#include "proven/hash_legacy.h"
#include "proven_internal_crypto.h"

#define PROVEN_TLS_AES_128_GCM_SHA256       ((proven_u16)0x1301)
#define PROVEN_TLS_AES_256_GCM_SHA384       ((proven_u16)0x1302)
#define PROVEN_TLS_CHACHA20_POLY1305_SHA256 ((proven_u16)0x1303)

/* Record content types. */
#define PROVEN_TLS_CT_CCS         20
#define PROVEN_TLS_CT_ALERT       21
#define PROVEN_TLS_CT_HANDSHAKE   22
#define PROVEN_TLS_CT_APPLICATION 23

#define PROVEN_TLS_MAX_PLAINTEXT  16384                    /* 2^14, RFC 8446 section 5.1 */
#define PROVEN_TLS_RECORD_HEADER  5
#define PROVEN_TLS_MAX_EXPANSION  (1 + 16)                 /* the inner content type and the tag */
#define PROVEN_TLS_MAX_CIPHERTEXT (PROVEN_TLS_MAX_PLAINTEXT + 256)

typedef struct {
    proven_u16 id;
    proven_hmac_hash_t hash;
    proven_size_t hash_len;
    proven_size_t key_len;
    bool chacha;
} proven_tls_suite_t;

/* NULL for a suite this library does not implement. */
const proven_tls_suite_t *proven_tls_suite_find(proven_u16 id);

/* ---- The transcript: a running hash of the handshake messages ---- */

typedef struct {
    proven_hmac_hash_t hash;
    union { proven_sha256_t s256; proven_sha512_t s512; } ctx;
} proven_tls_transcript_t;

void proven_tls_transcript_init(proven_tls_transcript_t *t, proven_hmac_hash_t hash);
void proven_tls_transcript_update(proven_tls_transcript_t *t, proven_mem_view_t data);
/* The hash of everything so far; the transcript goes on. Returns the hash length. */
proven_size_t proven_tls_transcript_hash(const proven_tls_transcript_t *t, proven_byte_t out[PROVEN_HMAC_MAX_SIZE]);
/* After a HelloRetryRequest the first ClientHello is replaced by a message that carries its
 * hash (RFC 8446 section 4.4.1). */
void proven_tls_transcript_retry(proven_tls_transcript_t *t);

/* ---- The key schedule (RFC 8446 section 7.1). Every secret is hash_len bytes. ---- */

/* HKDF-Expand-Label(secret, label, context, out_len); `label` without the "tls13 " prefix. */
void proven_tls13_expand_label(proven_hmac_hash_t hash, const proven_byte_t *secret, const char *label,
                               proven_mem_view_t context, proven_byte_t *out, proven_size_t out_len);
/* Derive-Secret(secret, label, messages), given the transcript hash of the messages. */
void proven_tls13_derive_secret(proven_hmac_hash_t hash, const proven_byte_t *secret, const char *label,
                                const proven_byte_t *transcript_hash, proven_byte_t *out);
/* HKDF-Extract. A null salt or a null ikm stands for hash_len zero bytes. */
void proven_tls13_extract(proven_hmac_hash_t hash, const proven_byte_t *salt, proven_mem_view_t ikm, proven_byte_t *out);
/* The Finished verify_data: HMAC(finished_key(base_secret), transcript_hash). */
void proven_tls13_finished(proven_hmac_hash_t hash, const proven_byte_t *base_secret, const proven_byte_t *transcript_hash,
                           proven_byte_t *out);

/* ---- Record protection (RFC 8446 section 5.2) ---- */

typedef struct {
    bool active;                                  /* false: records travel in the clear */
    bool chacha;
    proven_crypto_aes_gcm_t aes;                  /* AES: the context. ChaCha20: its key, in the first 32 bytes of aes.rk */
    proven_byte_t iv[12];
    proven_u64 seq;
} proven_tls_keys_t;

/* A connection's two directions, in an allocation of their own; and what remakes one while a
 * quiet connection has given that allocation back (proven_tls_compact_). */
typedef struct { proven_tls_keys_t read, write; } proven_tls_live_keys_t;
typedef struct {
    proven_byte_t key[32];
    proven_byte_t iv[12];
    proven_u64 seq;
    proven_u8 key_len;
    bool active, chacha;
} proven_tls_key_seed_t;

/* Install the key and IV derived from a traffic secret, and start the sequence at zero. */
void proven_tls13_set_keys(proven_tls_keys_t *keys, const proven_tls_suite_t *suite, const proven_byte_t *traffic_secret);

/* One protected record: header, the content with its type appended, and the tag, into `out`,
 * which must hold PROVEN_TLS_RECORD_HEADER + content.size + PROVEN_TLS_MAX_EXPANSION bytes.
 * Returns the number of bytes written. `content` and `out` must not overlap. */
proven_size_t proven_tls13_seal(proven_tls_keys_t *keys, proven_byte_t type, proven_mem_view_t content, proven_byte_t *out);

/* Open one protected record in place: `record` is the header and `body_len` bytes after it.
 * On success `*type` is the inner content type and `content` views the plaintext inside
 * `record`, padding removed. False when the tag does not verify or the record is malformed. */
[[nodiscard]] bool proven_tls13_open(proven_tls_keys_t *keys, proven_byte_t *record, proven_size_t body_len,
                                     proven_byte_t *type, proven_mem_mut_t *content);

/* ---- TLS 1.2 (RFC 5246) below its state machine: tls12_keys.c ---- */

typedef struct {
    proven_u16 id;
    proven_hmac_hash_t hash;                      /* of the PRF and the handshake hash */
    proven_size_t key_len;
    bool chacha;
    bool rsa_auth;                                /* ECDHE_RSA: the server's key is RSA; otherwise ECDSA or Ed25519 */
    proven_u8 mac;                                /* 0: an AEAD suite. Else the HMAC of a CBC suite, a proven_tls_lh_t */
    proven_u8 kx;                                 /* PROVEN_TLS_KX_* */
    proven_u8 legacy;                             /* the PROVEN_TLS_LEGACY_* bits a configuration must have set */
} proven_tls12_suite_t;
#define PROVEN_TLS_KX_ECDHE 0
#define PROVEN_TLS_KX_RSA   1
#define PROVEN_TLS_KX_DHE   2

/* NULL for a suite this library does not implement. */
const proven_tls12_suite_t *proven_tls12_suite_find(proven_u16 id);
/* The table in order - the six AEAD suites, then the legacy ones - and NULL past its end. */
const proven_tls12_suite_t *proven_tls12_suite_at(proven_size_t index);

#define PROVEN_TLS12_MASTER_SIZE 48
#define PROVEN_TLS12_VERIFY_SIZE 12

/* PRF(secret, label, seed) = P_hash(secret, label | seed), `out_len` bytes. */
void proven_tls12_prf(proven_hmac_hash_t hash, proven_mem_view_t secret, const char *label, proven_mem_view_t seed,
                      proven_byte_t *out, proven_size_t out_len);
/* The extended master secret (RFC 7627): from the premaster secret and the session hash -
 * the hash of the handshake messages through ClientKeyExchange. The only kind made here. */
void proven_tls12_master_secret(proven_hmac_hash_t hash, proven_mem_view_t premaster, proven_mem_view_t session_hash,
                                proven_byte_t master[PROVEN_TLS12_MASTER_SIZE]);
/* A Finished message's verify_data, from the hash of the handshake messages before it. */
void proven_tls12_finished(proven_hmac_hash_t hash, const proven_byte_t master[PROVEN_TLS12_MASTER_SIZE], bool server,
                           proven_mem_view_t handshake_hash, proven_byte_t out[PROVEN_TLS12_VERIFY_SIZE]);
/* Expand the master secret into the two directions' keys and start both sequences at zero. */
void proven_tls12_set_keys(proven_tls_keys_t *client_write, proven_tls_keys_t *server_write, const proven_tls12_suite_t *suite,
                           const proven_byte_t master[PROVEN_TLS12_MASTER_SIZE], const proven_byte_t client_random[32],
                           const proven_byte_t server_random[32]);
/* One protected record of the given type: header, the explicit nonce (AES-GCM: 8 bytes), the
 * content and the tag, into `out` - PROVEN_TLS_RECORD_HEADER + 8 + content.size + 16 bytes at
 * most. Returns the number written. `content` and `out` must not overlap. */
proven_size_t proven_tls12_seal(proven_tls_keys_t *keys, proven_byte_t type, proven_mem_view_t content, proven_byte_t *out);
/* Open one in place. The type is the header's, in the clear and authenticated. False when the
 * tag does not verify or the record is malformed. */
[[nodiscard]] bool proven_tls12_open(proven_tls_keys_t *keys, proven_byte_t *record, proven_size_t body_len, proven_mem_mut_t *content);

/* ---- The legacy set (RFC-0011 decision T-8), below the state machine: tls_legacy.c ----
 * Everything here is reached only through a configuration that asks for it. */

typedef enum { PROVEN_TLS_LH_MD5 = 0, PROVEN_TLS_LH_SHA1 = 1, PROVEN_TLS_LH_SHA256 = 2, PROVEN_TLS_LH_SHA384 = 3 } proven_tls_lh_t;
#define PROVEN_TLS_LH_MAX_SIZE 48

/* HMAC over `a` then `b` with one of the four hashes; out takes 16, 20, 32 or 48 bytes. */
void proven_tls_legacy_hmac(proven_tls_lh_t hash, proven_mem_view_t key, proven_mem_view_t a, proven_mem_view_t b, proven_byte_t *out);
/* The PRF of TLS 1.0 and 1.1 (RFC 2246 section 5): P_MD5 over the first half of the secret
 * XOR P_SHA-1 over the second. */
void proven_tls10_prf(proven_mem_view_t secret, const char *label, proven_mem_view_t seed, proven_byte_t *out, proven_size_t out_len);

/* What a CBC suite keeps for one direction beside the cipher and the sequence number, which
 * stay in proven_tls_keys_t. */
typedef struct {
    proven_u8 mac;                                /* PROVEN_TLS_LH_SHA1, _SHA256 or _SHA384 */
    proven_u8 mac_len;                            /* 20, 32 or 48: of the MAC and of its key */
    bool etm;                                     /* encrypt_then_mac (RFC 7366) was agreed */
    bool chained;                                 /* TLS 1.0: a record's IV is the last block of the one before */
    proven_u16 version;                           /* as records and the MAC carry it */
    proven_byte_t mac_key[PROVEN_TLS_LH_MAX_SIZE];
    proven_byte_t iv[16];                         /* the chain, when chained */
} proven_tls_cbc_t;

#define PROVEN_TLS_CBC_MAX_EXPANSION (PROVEN_TLS_RECORD_HEADER + 16 + PROVEN_TLS_LH_MAX_SIZE + 16)      /* 85 */
/* One CBC record: header, the explicit IV unless chained (`iv`: 16 unpredictable bytes from
 * the caller), and the protected content. Returns the number of bytes written to `out`, at
 * most content.size + PROVEN_TLS_CBC_MAX_EXPANSION. `content` and `out` must not overlap. */
proven_size_t proven_tls_cbc_seal(proven_tls_keys_t *keys, proven_tls_cbc_t *cbc, proven_byte_t type, proven_mem_view_t content,
                                  const proven_byte_t iv[16], proven_byte_t *out);
/* Open one in place. Without encrypt_then_mac this does the same work whatever the padding
 * says and wherever the MAC turns out to start (Lucky Thirteen): there is one answer for every
 * kind of failure, and the time to reach it depends only on the record's length. */
/* ---- Finite-field Diffie-Hellman over six known groups: tls_dh.c ---- */
#define PROVEN_TLS_DH_MAX 512                    /* bytes: the largest prime known here, 4096 bits */
#define PROVEN_TLS_DH_EXP_MAX 44                 /* bytes: the longest private exponent */
/* The group a server offers: ffdhe2048 (RFC 7919). Its generator is 2, as every group's here. */
proven_mem_view_t proven_tls_dh_server_group(void);
/* True when `p` is one of the six primes and `g` is 2. Nothing else is ever used. */
[[nodiscard]] bool proven_tls_dh_known(proven_mem_view_t p, proven_mem_view_t g);
/* How many random bytes make a private exponent for a prime of `p_len` bytes: 32, 36 or 44. */
proven_size_t proven_tls_dh_exponent_len(proven_size_t p_len);
/* 2^priv mod p into `out`, p.size bytes. SECRET: priv. */
[[nodiscard]] bool proven_tls_dh_public(proven_mem_view_t p, const proven_byte_t *priv, proven_size_t priv_len, proven_byte_t *out);
/* peer^priv mod p with its leading zero bytes dropped, as TLS wants the premaster: up to
 * p.size bytes into `out`. False when the peer's value is not in 2 .. p-2. SECRET: priv, out. */
[[nodiscard]] bool proven_tls_dh_shared(proven_mem_view_t p, const proven_byte_t *priv, proven_size_t priv_len, proven_mem_view_t peer,
                                        proven_byte_t *out, proven_size_t *out_len);

/* Both directions' keys for a CBC suite from the master secret: the MAC keys, the cipher keys
 * and - in TLS 1.0 only - the first IVs. cbc[0] is the client's writing state, cbc[1] the
 * server's. The PRF is the suite's from TLS 1.2, and the MD5/SHA-1 one before. */
void proven_tls_cbc_set_keys(proven_tls_keys_t *client_write, proven_tls_keys_t *server_write, proven_tls_cbc_t cbc[2],
                             const proven_tls12_suite_t *suite, proven_u16 version, bool etm,
                             const proven_byte_t master[PROVEN_TLS12_MASTER_SIZE], const proven_byte_t client_random[32],
                             const proven_byte_t server_random[32]);
[[nodiscard]] bool proven_tls_cbc_open(proven_tls_keys_t *keys, proven_tls_cbc_t *cbc, proven_byte_t *record, proven_size_t body_len,
                                       proven_mem_mut_t *content);

/* ---- The configuration and what the state machine needs from it ---- */

#include "proven/tls.h"
#include <stdatomic.h>

#define PROVEN_TLS_MAX_CHAIN 8
#define PROVEN_TLS_TICKET_PSK_MAX 48

typedef enum { PROVEN_TLS_KEY_NONE = 0, PROVEN_TLS_KEY_P256, PROVEN_TLS_KEY_ED25519, PROVEN_TLS_KEY_RSA } proven_tls_key_kind_t;

/* An RSA key and its blinding pair. The pair changes with every signature - from inside
 * handshakes, on whichever thread signs - so a flag serialises that, as for the ticket keys.
 * It is held for one squaring of the pair (and, every 65,536 signatures, for making a new
 * one): longer than the ticket keys' few instructions, and still far shorter than the
 * signature that follows outside it. */
typedef struct {
    proven_crypto_rsa_key_t key;                  /* SECRET */
    atomic_flag lock;
    proven_crypto_rsa_blind_t blind;              /* SECRET */
    proven_u32 uses;
} proven_tls_rsa_t;

/* A server's ticket keys: the current one and the one before. They are replaced on a timer
 * from inside handshakes, on whichever thread gets there first; a flag serialises that. */
typedef struct {
    atomic_flag lock;
    proven_i64 rotated_at;                        /* when `cur` was made */
    proven_byte_t cur[32], prev[32];
    proven_u32 cur_id, prev_id;
    bool have_prev;
} proven_tls_ticket_keys_t;

/* The tickets early data was taken on, so that each is taken once (RFC 8446 section 8.1), for
 * as long as the freshness window could still let it in. Oldest first in a ring; full: refused. */
#define PROVEN_TLS_EARLY_SEEN 4096
#define PROVEN_TLS_EARLY_WINDOW_S 10              /* the freshness window: a ticket's age as the client says it and as the server knows it */
/* Over TCP (RFC-0027): the most early data a ticket allows (its max_early_data_size). */
#define PROVEN_TLS_EARLY_TCP_MAX 16384
/* Where a connection's early data stands, over TCP or in QUIC mode. */
enum { PROVEN_TLS_EARLY_NONE = 0, PROVEN_TLS_EARLY_OFFERED, PROVEN_TLS_EARLY_ACCEPTED, PROVEN_TLS_EARLY_REFUSED };
typedef struct {
    proven_byte_t id[16];                         /* the ticket's key id and nonce: unique to it */
    proven_i64 until;
} proven_tls_early_entry_t;
typedef struct {
    atomic_flag lock;
    proven_size_t head, count, cap;               /* cap: PROVEN_TLS_EARLY_SEEN, smaller in a test */
    proven_tls_early_entry_t e[PROVEN_TLS_EARLY_SEEN];
} proven_tls_early_seen_t;
/* A client's sessions by server name, for the transport wrapper when it is given none
 * (RFC-0027): resumption, and early data where a ticket allows it. The entries are allocated
 * when the first session is kept. */
#define PROVEN_TLS_CLIENT_SESSIONS 8
typedef struct {
    proven_u64 used;                              /* 0: free */
    proven_u8 name_len;
    proven_byte_t name[253];
    proven_tls_session_t s;
} proven_tls_client_entry_t;
typedef struct proven_tls_client_cache {
    atomic_flag lock;
    proven_u64 clock;
    proven_tls_client_entry_t *e;                 /* PROVEN_TLS_CLIENT_SESSIONS of them, or null */
} proven_tls_client_cache_t;
/* The session kept for `name`, copied to `out` (emptied when there is none). */
void proven_tls_client_cache_load_(const proven_tls_config_t *config, proven_u8str_view_t name, proven_tls_session_t *out);
/* Keep `s` as `name`'s newest; an empty one is not kept. */
void proven_tls_client_cache_store_(const proven_tls_config_t *config, proven_u8str_view_t name, const proven_tls_session_t *s);

/* Take a ticket's early data once: false when it was seen already, or the register is full. */
[[nodiscard]] bool proven_tls_early_take_(const proven_tls_config_t *config, const proven_byte_t id[16], proven_i64 now);
/* Tests: shrink a configuration's register (0 restores it). */
void proven_tls_test_early_cap_(const proven_tls_config_t *config, proven_size_t cap);

struct proven_tls_config {
    proven_allocator_t alloc;
    const proven_cert_store_t *anchors;
    proven_tls_verify_t verify;
    proven_byte_t (*pins)[32];
    proven_size_t pin_count;
    proven_byte_t *chain_mem;                     /* the certificates, DER, back to back */
    proven_mem_view_t chain[PROVEN_TLS_MAX_CHAIN];
    proven_size_t chain_count;
    proven_tls_key_kind_t key_kind;
    proven_byte_t key[32];                        /* the P-256 scalar or the Ed25519 seed. SECRET */
    proven_byte_t key_public[32];                 /* Ed25519 only */
    proven_tls_rsa_t *rsa;                        /* an RSA key lives here instead: it is eight kilobytes, six of them its moduli prepared */
    proven_tls_client_auth_t client_auth;
    proven_byte_t *alpn_wire;                     /* the names, each behind its length byte */
    proven_size_t alpn_wire_len;
    proven_size_t alpn_count;
    proven_tls_random_fn random; void *random_ctx;
    proven_tls_now_fn now; void *now_ctx;
    bool no_resumption;
    bool early_data;
    bool keep_peer_certificate;
    proven_u32 ticket_lifetime_s;
    proven_size_t max_handshake_bytes;
    proven_u16 min_version, max_version;          /* never zero: the defaults are filled in */
    proven_u32 legacy;                            /* PROVEN_TLS_LEGACY_* */
    proven_tls_ticket_keys_t tickets;             /* the one part that changes: see the lock */
    proven_tls_early_seen_t *early_seen;          /* a server with early_data: the strike register (RFC-0021), which also changes under its lock */
    struct proven_tls_client_cache *client_cache; /* a client: the newest session per server name (RFC-0027), under its own lock */
};

/* What a ticket holds, and what a client keeps of a session. */
typedef struct {
    proven_u16 suite;
    proven_u16 version;                           /* 0x0302 or 0x0301 for a session of those versions; 0 otherwise (the suite tells 1.3 from 1.2) */
    proven_size_t psk_len;
    proven_byte_t psk[PROVEN_TLS_TICKET_PSK_MAX];
    proven_i64 issued_at;
    bool has_peer_key;
    proven_byte_t peer_key_hash[32];
    /* Early data (RFC-0021): whether the ticket allows it, the age_add the client was told (for
     * the freshness check), the protocol's index in the configuration (0xff: none), and a digest
     * of the 0-RTT limits the server sent. */
    bool early;
    proven_u32 age_add;
    proven_u8 alpn_index;
    proven_byte_t limits[8];
    bool quic_v2;                                 /* made over QUIC version 2 (RFC-0036): taken only there */
} proven_tls_ticket_state_t;

#define PROVEN_TLS_TICKET_MAX 160
/* Seal a ticket (server). Returns its length, at most PROVEN_TLS_TICKET_MAX. */
proven_size_t proven_tls_ticket_seal(const proven_tls_config_t *config, const proven_tls_ticket_state_t *state,
                                     proven_byte_t out[PROVEN_TLS_TICKET_MAX]);
/* Open one a client offered. False for anything not sealed by a current key, or too old. */
[[nodiscard]] bool proven_tls_ticket_open(const proven_tls_config_t *config, proven_mem_view_t ticket, proven_tls_ticket_state_t *state);

/* The client's session record, laid out inside proven_tls_session_t's bytes. */
#define PROVEN_TLS_SESSION_TICKET_MAX 640
#define PROVEN_TLS_SESSION_LIMITS 7                    /* initial_max_data, the three stream data, the two streams, active_connection_id_limit */
#define PROVEN_TLS_SESSION_TOKEN_MAX 255
typedef struct {
    proven_u32 magic;                             /* 0 for an empty session */
    proven_u16 suite;
    proven_u16 psk_len;
    proven_byte_t psk[PROVEN_TLS_TICKET_PSK_MAX];
    proven_i64 received_at;
    proven_u32 lifetime_s;
    proven_u32 age_add;
    proven_u16 name_len;
    proven_u16 ticket_len;
    proven_u16 version;                           /* of a session older than TLS 1.3: the version it was made in and resumes in */
    proven_byte_t name[254];
    proven_byte_t has_peer_key;
    proven_byte_t peer_key_hash[32];              /* the server's key, as verified when the session was made */
    proven_byte_t ticket[PROVEN_TLS_SESSION_TICKET_MAX];
    /* 0-RTT (RFC-0021): what the ticket allows and what early data must assume. */
    proven_byte_t early;                          /* the ticket allows early data (QUIC only) */
    proven_byte_t alpn_len;                       /* the protocol agreed when the ticket came */
    proven_byte_t alpn[255];
    proven_byte_t has_limits;                     /* QUIC: the server's limits below are known */
    proven_u64 limits[PROVEN_TLS_SESSION_LIMITS]; /* QUIC: the server's 0-RTT limits (RFC 9000 section 7.4.1), in proven_quic_tp_t's order */
    /* QUIC: a NEW_TOKEN token for the next connection (RFC 9000 section 8.1.3). Kept apart from
     * the ticket: a new ticket leaves it, and it is used once. */
    proven_u16 token_len;
    proven_byte_t token[PROVEN_TLS_SESSION_TOKEN_MAX];
    /* TCP (RFC-0027): the most early data the ticket allows (its max_early_data_size); 0: none.
     * After every field before it, so that a record written before it reads as 0. */
    proven_u32 early_max;
    /* QUIC version 2 (RFC-0036): the ticket and the token came over it. After every field, so
     * that an older record reads as version 1. */
    proven_byte_t ticket_v2, token_v2;
} proven_tls_session_data_t;
#define PROVEN_TLS_SESSION_MAGIC ((proven_u32)0x70547333)      /* changed with the record's layout: an older record is simply empty */

void proven_tls_config_random(const proven_tls_config_t *config, proven_byte_t *out, proven_size_t len);
proven_i64 proven_tls_config_now(const proven_tls_config_t *config);
/* The index-th ALPN name of the config. */
proven_u8str_view_t proven_tls_config_alpn(const proven_tls_config_t *config, proven_size_t index);

#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)
/* For the HTTP server, whose loop must never wait on one connection. */
/* A server-side TLS transport that has not shaken hands yet: the handshake happens inside its
 * reads, as the bytes arrive. */
proven_err_t proven_tls_transport_server_lazy_(proven_transport_t plain, const proven_tls_config_t *config, proven_transport_t *out);
/* The transport underneath has moved. */
void proven_tls_transport_set_plain_(proven_transport_t tls, proven_transport_t plain);
/* True when input is held inside - bytes not yet fed, or plaintext not yet read - so that a
 * read could return data although the socket has nothing new. */
bool proven_tls_transport_buffered_(proven_transport_t tls);
/* close_notify if it goes out without waiting, then close and free. */
void proven_tls_transport_close_now_(proven_transport_t tls);
/* close_notify if it goes out without waiting; the transport stays open for reading. */
void proven_tls_transport_shutdown_now_(proven_transport_t tls);
#endif

/* ---- Issuing certificates: for tests that need an identity or a chain ---- */

/* A self-signed Ed25519 certificate for `names` (DNS names or IP literals, 1 to 16), valid from
 * `not_before` to `not_after`, for server and client authentication, and its key, as PEM. Public
 * as proven_tls_self_signed until v0.78.0 (owner decision 2026-10-23: an identity comes from
 * files). random NULL: the system's (hosted). PROVEN_ERR_INVALID_ARG, PROVEN_ERR_OUT_OF_BOUNDS. */
proven_err_t proven_tls_self_signed_(const proven_u8str_view_t *names, proven_size_t name_count,
                                     proven_i64 not_before, proven_i64 not_after,
                                     proven_tls_random_fn random, void *random_ctx,
                                     proven_mem_mut_t certificate_pem, proven_size_t *certificate_len,
                                     proven_mem_mut_t private_key_pem, proven_size_t *private_key_len);

typedef struct {
    const char *subject;                          /* the subject's common name */
    const char *issuer;                           /* the issuer's; the same string for a self-signed certificate */
    proven_tls_key_kind_t subject_kind;
    const proven_byte_t *subject_key;             /* 32 bytes: an Ed25519 seed or a P-256 scalar */
    proven_tls_key_kind_t issuer_kind;
    const proven_byte_t *issuer_key;
    const proven_crypto_rsa_key_t *subject_rsa;   /* instead of subject_key, for PROVEN_TLS_KEY_RSA */
    const proven_crypto_rsa_key_t *issuer_rsa;
    bool is_ca;
    proven_u32 eku;                               /* PROVEN_CERT_EKU_SERVER_AUTH | PROVEN_CERT_EKU_CLIENT_AUTH; 0: none */
    const proven_u8str_view_t *names;             /* subjectAltName entries: DNS names and IP literals */
    proven_size_t name_count;
    proven_i64 not_before, not_after;
    proven_byte_t serial[16];
} proven_tls_issue_t;

/* One certificate, DER. PROVEN_ERR_OUT_OF_BOUNDS when `out` is too small (1 KiB is plenty for a few names). */
proven_err_t proven_tls_issue_(const proven_tls_issue_t *what, proven_mem_mut_t out, proven_size_t *len);
/* A private key as PKCS #8 DER; with `sec1`, a P-256 key as an ECPrivateKey instead. At most 80 bytes. */
proven_size_t proven_tls_key_der_(proven_tls_key_kind_t kind, const proven_byte_t key[32], bool sec1, proven_byte_t out[80]);
/* An RSA private key as an RSAPrivateKey (PKCS #1), or with `pkcs8` inside a PrivateKeyInfo.
 * `d` is the private exponent, n_len bytes. Returns the length, or 0 when `out` is too small. */
proven_size_t proven_tls_rsa_key_der_(const proven_crypto_rsa_key_t *key, const proven_byte_t *d, bool pkcs8, proven_mem_mut_t out);
/* A signature of a SHA-256 digest with the config's RSA key, n_len bytes into `sig`:
 * RSASSA-PSS with a salt of 32 bytes, or - `pkcs1`, for TLS 1.2 peers that offer nothing
 * else - PKCS #1 v1.5. False when the config has no RSA key or the signature could not be made. */
[[nodiscard]] bool proven_tls_config_rsa_sign_(const proven_tls_config_t *config, bool pkcs1, proven_mem_view_t digest, proven_byte_t *sig, proven_size_t *len);
/* PEM: the label between the dashes, the bytes in Base64, 64 to a line. */
proven_err_t proven_tls_pem_write_(const char *label, proven_mem_view_t der, proven_mem_mut_t out, proven_size_t *len);
/* 4 or 16 when `text` is an IP literal (cert.c). */
proven_size_t proven_cert_host_as_ip_(proven_u8str_view_t text, proven_byte_t out[16]);

/* Test hooks. Not for any other use. */
void proven_crypto_rsa_test_min_bits(proven_size_t bits);
/* Put one suite first in both roles' order (0: the normal order), and make the server ignore
 * an X25519 key share so that it has to ask for P-256. */
void proven_tls_test_knobs(proven_u16 suite_first, bool server_refuses_x25519);
/* For tests: a server over TCP passes over quic_transport_parameters instead of refusing it, so
 * that a QUIC-mode hello can be answered over TCP and the two key schedules compared. */
void proven_tls_test_quic_tp_tcp_(bool on);
/* 0-RTT over TCP (RFC-0027). Only a connection whose owner asks for it sends or takes early data:
 * a client made by proven_tls_client_create_early_ offers it (with `early_data` in its
 * configuration and a session whose ticket allows it); a server told proven_tls_server_take_early_
 * before its first feed takes it (with `early_data`). Early data is written with proven_tls_write
 * before the handshake, up to the ticket's allowance, and is read with proven_tls_read on the
 * server, where proven_tls_pending_early_ says whether the bytes waiting arrived early. A server
 * that took early data may write from its Finished on (0.5-RTT). */
[[nodiscard]] proven_err_t proven_tls_client_create_early_(const proven_tls_config_t *config, proven_u8str_view_t server_name,
                                                         proven_tls_session_t *session, proven_tls_conn_t **out);
void proven_tls_server_take_early_(proven_tls_conn_t *c);
/* PROVEN_TLS_EARLY_*: none offered; offered and not yet answered; accepted; refused (a client's
 * early bytes did not arrive and are to be sent again). */
int proven_tls_early_(const proven_tls_conn_t *c);
/* Client: the bytes written as early data so far, and how many more the ticket allows. */
proven_size_t proven_tls_early_written_(const proven_tls_conn_t *c);
proven_size_t proven_tls_early_room_(const proven_tls_conn_t *c);
/* Server: the application data waiting to be read arrived as early data. */
bool proven_tls_pending_early_(const proven_tls_conn_t *c);
/* Server: application bytes read so far that arrived early, and in all (held at its largest). */
void proven_tls_read_counts_(const proven_tls_conn_t *c, proven_size_t *early, proven_size_t *total);
/* Application data may be written now (established, or a server's 0.5-RTT). */
bool proven_tls_writable_(const proven_tls_conn_t *c);
bool proven_tls_is_server_(const proven_tls_conn_t *c);
/* Client: this connection has kept a ticket from its peer since its handshake. */
bool proven_tls_ticket_stored_(const proven_tls_conn_t *c);
#if !defined(PROVEN_FREESTANDING) && !defined(PROVEN_NO_NET)
/* What a transport made by the wrapper did with early data (PROVEN_TLS_EARLY_*), and: the next
 * connection under its session is not to send early data (an HTTP 425 came, RFC 8470). */
int proven_tls_transport_early_(proven_transport_t tls);
void proven_tls_transport_no_early_next_(proven_transport_t tls);
/* What is written next is not to go as early data: the handshake is completed first. */
void proven_tls_transport_no_early_now_(proven_transport_t tls);
#endif
/* The protocol a stored session was made for is `name`. */
bool proven_tls_session_alpn_is_(const proven_tls_session_t *session, proven_u8str_view_t name);
/* A stored session whose ticket allows early data over TCP. */
bool proven_tls_session_allows_early_(const proven_tls_session_t *session);
/* A ticket that carried early data once is not offered for early data again (RFC 8446 section 8:
 * the server's register refuses it); resumption with it goes on. */
void proven_tls_session_early_off_(proven_tls_session_t *s);
/* TLS 1.2 test knobs: bit 0, the server answers 1.2 whatever the client offers; bit 1, the
 * client omits the extended master secret; bit 2, the server omits it from its answer; bit 3,
 * the client adds the fallback signal of RFC 7507 to its suites. */
void proven_tls_test_knobs12(unsigned flags);
/* More bits of the same, for the legacy set: 16, the client offers the suite of
 * proven_tls_test_only_suite12 even when its configuration does not allow it; 32, the client
 * does not ask for encrypt_then_mac; 64, the server agrees to the oldest version the client
 * allows instead of the newest; 128, the server's Diffie-Hellman prime is not a known one;
 * 256, its value is 1; 512, the client's value is 1; 4096, the server asks for a client
 * certificate even on a connection without the extended master secret. */
/* The client offers only this TLS 1.2-family suite (0: everything, as usual). */
void proven_tls_test_only_suite12(proven_u16 id);
/* How often an RSA key's blinding pair has been used, put where a test says; returns what it was. */
proven_u32 proven_tls_config_test_rsa_uses_(const proven_tls_config_t *config, proven_u32 uses);
/* The count of records `writer` has written and `reader` has read, put where a test says. */
void proven_tls_test_records_(proven_tls_conn_t *writer, proven_tls_conn_t *reader, proven_u64 count);
/* Send an empty handshake message of `type` under the current keys. */
bool proven_tls_test_send_handshake(proven_tls_conn_t *conn, proven_byte_t type);
/* A copy of a connection's sending keys and application secret, for a test that must forge the
 * next record a peer would send. */
void proven_tls_test_peek_write(const proven_tls_conn_t *conn, proven_tls_keys_t *keys, proven_byte_t secret[48]);

/* RFC-0025 step 3: a quiet connection gives back its record keys' memory - the two expanded
 * contexts, about 850 bytes, wiped - and keeps the keys they were made from; the next call that
 * needs them remakes them (on AES-NI in about a tenth of a microsecond each). False when it
 * cannot: no keys yet, a handshake in progress, QUIC mode, a CBC suite, a dead connection, or
 * compact already. For a driver that knows its connection is quiet (the event server, after a
 * second). SECRET: what is kept is key material, wiped when remade and at destroy. */
bool proven_tls_compact_(proven_tls_conn_t *conn);
/* The two halves of it, for the constant-time check: what remakes one direction's keys, and
 * the keys remade from it. SECRET: both. */
void proven_tls_seed_take_(proven_tls_key_seed_t *seed, const proven_tls_keys_t *keys);
void proven_tls_seed_give_(const proven_tls_key_seed_t *seed, proven_tls_keys_t *keys);
bool proven_tls_is_compact_(const proven_tls_conn_t *conn);

/* RFC-0025 step 3: application data sealed into records straight into `out` - a driver's send
 * buffer - instead of the engine's own output. As many whole records as fit (a record is cut to
 * the room left only when at least 256 bytes of it fit); `.value` is the plaintext taken and
 * `*written` the bytes of records written. Takes nothing while the engine holds records of its own
 * (proven_tls_pending_output: a KeyUpdate, an alert, a handshake message), which must leave first.
 * PROVEN_ERR_UNSUPPORTED on a CBC suite (proven_tls_write instead); otherwise the errors of
 * proven_tls_write. A record needs PROVEN_TLS_AEAD_MAX_RECORD_EXTRA bytes
 * beyond its plaintext. */
#define PROVEN_TLS_AEAD_MAX_RECORD_EXTRA (PROVEN_TLS_RECORD_HEADER + 8 + 16)
proven_result_size_t proven_tls_seal_into_(proven_tls_conn_t *conn, proven_mem_view_t src, proven_mem_mut_t out, proven_size_t *written);
/* A client whose first message and X25519 key are given, to replay a published handshake. */
proven_err_t proven_tls_client_create_replay(const proven_tls_config_t *config, proven_u8str_view_t server_name,
                                             proven_mem_view_t hello, const proven_byte_t x25519_private[32], proven_tls_conn_t **out);

#endif
